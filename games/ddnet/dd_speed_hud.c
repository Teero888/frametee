// A speed HUD for videos: how fast the followed tee moves along the map, against the most DDNet's
// velocity ramp lets it reach, a small plot of that ramp with the tee riding along it, and how far
// along the map the tee is.
//
// CCharacterCore::Move() moves a tee by vel.x times VelocityRamp() and leaves vel.y alone, so the
// ramp only limits the horizontal speed: that is the one shown. Its limit is a summit, not a
// wall. Below a raw speed of velramp_range / ln(velramp_curvature) more raw speed still moves the
// tee faster; above it the ramp takes away more than it adds.
//
// The ramp also makes the speed wobble with every bounce of the tee, since the vertical speed
// counts towards it. So by default the HUD holds the fastest the tee has been so far, read from a
// history of the whole run that is taken again whenever its simulation changes.
#include "dd_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The HUD is laid out on a screen 300 units high, like DDNet's.
#define HUD_HEIGHT 300.f
#define Z_PANEL (DD_Z_OVERLAYS - 0.02f)
#define Z_SHAPES (DD_Z_OVERLAYS - 0.01f)
#define CURVE_SEGMENTS 96

// The fastest each tee of one world had been by every tick of its run.
struct dd_speed_history_t {
  bool valid;
  uint64_t revision;
  uint64_t seen_revision; // as the last update saw it, to wait for an edit to settle
  int first_tick;         // the engine tick of entry 0
  int ticks, players;
  float *top; // [tick * players + player]: the fastest after the ramp so far, in blocks per second
  float *raw; // the same without the ramp
};

static const ft_color ACCENT = {1.f, .72f, .25f, 1.f}, DIM = {1.f, 1.f, 1.f, .85f}, FAINT = {1.f, 1.f, 1.f, .45f};
static const ft_color PANEL = {0.f, 0.f, 0.f, .5f};

float dd_velocity_ramp(const ddnet_tuning_values_t *tuning, float speed) {
  if (speed < tuning->velramp_start) return 1.0f;
  return 1.0f / powf(tuning->velramp_curvature, (speed - tuning->velramp_start) / tuning->velramp_range);
}

static const ddnet_tuning_values_t *tee_tuning(const ft_world *world, const ddnet_character_t *chr) {
  const int zone = chr->tune_zone >= 0 && chr->tune_zone < world->core.num_tune_zones ? chr->tune_zone : 0;
  return &world->core.tuning_values[zone];
}

// A tee's horizontal speed in blocks per second, after the ramp and without it.
static void tee_speeds(const ft_world *world, const ddnet_character_t *chr, float *after, float *raw) {
  const ddnet_vec2_t v = chr->core.vel;
  const float ramp = dd_velocity_ramp(tee_tuning(world, chr), sqrtf(v.x * v.x + v.y * v.y) * GAME_TICK_SPEED);
  *raw = fabsf(v.x) * GAME_TICK_SPEED / PX_PER_TILE;
  *after = *raw * ramp;
}

// The speed in blocks per second the ramp lets through of `raw`, for a tee that does not move vertically.
static float ramped(const ddnet_tuning_values_t *tuning, float raw) {
  return raw * dd_velocity_ramp(tuning, raw * PX_PER_TILE);
}

// The raw speed at the ramp's summit, in blocks per second, or a negative number when the ramp
// never turns down (a curvature of at most 1).
static float summit(const ddnet_tuning_values_t *tuning) {
  if (tuning->velramp_curvature <= 1.f || tuning->velramp_range <= 0.f) return -1.f;
  const float top = tuning->velramp_range / logf(tuning->velramp_curvature);
  return (top > tuning->velramp_start ? top : tuning->velramp_start) / PX_PER_TILE;
}

// --- history -----------------------------------------------------------------

static bool history_reserve(struct dd_speed_history_t *history, int ticks, int players) {
  const size_t count = (size_t)ticks * (size_t)players;
  float *top = realloc(history->top, count * sizeof(*top));
  if (top) history->top = top;
  float *raw = top ? realloc(history->raw, count * sizeof(*raw)) : NULL;
  if (raw) history->raw = raw;
  return top && raw;
}

static void collect_history(ft_game *game, uint32_t world_index, const ft_timeline_world_info *info, int32_t end_tick,
                            struct dd_speed_history_t *history) {
  history->ticks = 0;
  const int ticks = end_tick - info->start_offset + 1;
  const int players = (int)info->player_count;
  if (ticks <= 0 || players <= 0 || !history_reserve(history, ticks, players)) return;
  float *top = calloc((size_t)players * 2, sizeof(*top));
  if (!top) return;
  float *raw = top + players;

  for (int t = 0; t < ticks; ++t) {
    const ft_world *world = NULL;
    if (game->engine->timeline_world_pair(world_index, info->start_offset + t, NULL, &world) && world) {
      if (t == 0) history->first_tick = ddnet_engine_tick(world);
      for (int player = 0; player < players && player < world->player_count; ++player) {
        const ddnet_character_t *chr = ddnet_player_character(world, player);
        if (!chr || dd_replay_absent(world, player)) continue;
        float after, without;
        tee_speeds(world, chr, &after, &without);
        if (after > top[player]) top[player] = after;
        if (without > raw[player]) raw[player] = without;
      }
    }
    memcpy(history->top + (size_t)t * players, top, sizeof(*top) * (size_t)players);
    memcpy(history->raw + (size_t)t * players, raw, sizeof(*raw) * (size_t)players);
  }
  free(top);
  history->ticks = ticks;
  history->players = players;
}

static bool holds_top(const dd_settings_t *settings) {
  return (settings->render_speed || settings->render_speed_curve) && settings->speed_hold_top;
}

// Before the frame is drawn: the history of every world whose simulation changed, once it has
// stopped changing. An edit changes it every frame while the mouse drags, and taking the whole run
// again each time would stall the editor; until then the HUD shows the speed of the moment.
void dd_speed_hud_update(ft_game *game, const ft_engine_state *state) {
  const ft_engine_api *engine = game->engine;
  if (!holds_top(&game->settings) || !engine->timeline_world_count || !engine->timeline_world_info ||
      !engine->timeline_world_pair || !engine->timeline_range)
    return;
  const int count = (int)engine->timeline_world_count();
  if (count > game->speed_history_worlds) {
    struct dd_speed_history_t *grown = realloc(game->speed_history, sizeof(*grown) * (size_t)count);
    if (!grown) return;
    memset(grown + game->speed_history_worlds, 0, sizeof(*grown) * (size_t)(count - game->speed_history_worlds));
    game->speed_history = grown;
    game->speed_history_worlds = count;
  }
  int32_t start_tick = 0, end_tick = 0;
  if (!engine->timeline_range(&start_tick, &end_tick)) return;
  const int recorded = state->recording && engine->timeline_active_world ? engine->timeline_active_world() : -1;
  for (int world = 0; world < count; ++world) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (!engine->timeline_world_info((uint32_t)world, &info)) continue;
    struct dd_speed_history_t *history = &game->speed_history[world];
    const bool settled = history->seen_revision == info.physics_revision;
    history->seen_revision = info.physics_revision;
    // (the world being recorded changes with every tick and is left until the take is done)
    if (world == recorded || (history->valid && (history->revision == info.physics_revision || !settled))) continue;
    collect_history(game, (uint32_t)world, &info, end_tick, history);
    history->revision = info.physics_revision;
    history->valid = true;
  }
}

// The fastest player `player` had been by the tick `world` is at, from the history when it is
// current. False when it is not, or does not reach that tick.
static bool history_at(const ft_game *game, const ft_render_frame *frame, const ft_world *world, int player, float *top,
                       float *raw) {
  if (frame->world_index < 0 || frame->world_index >= game->speed_history_worlds) return false;
  const struct dd_speed_history_t *history = &game->speed_history[frame->world_index];
  if (!history->valid || history->revision != history->seen_revision || player >= history->players) return false;
  const int tick = ddnet_engine_tick(world) - history->first_tick;
  if (tick < 0 || tick >= history->ticks) return false;
  *top = history->top[(size_t)tick * history->players + player];
  *raw = history->raw[(size_t)tick * history->players + player];
  return true;
}

// The speeds the HUD shows of `player`, between the previous tick and this one.
static bool shown_speeds(const ft_game *game, const ft_render_frame *frame, int player, float *after, float *raw) {
  const ft_world *world = frame->world;
  const ft_world *previous = frame->previous_world ? frame->previous_world : world;
  float a0, r0, a1, r1;
  const bool found = game->settings.speed_hold_top && history_at(game, frame, world, player, &a1, &r1);
  if (found) {
    if (!history_at(game, frame, previous, player, &a0, &r0)) {
      a0 = a1;
      r0 = r1;
    }
  } else {
    const ddnet_character_t *chr = ddnet_player_character(world, player);
    if (!chr) return false;
    tee_speeds(world, chr, &a1, &r1);
    const ddnet_character_t *before = previous->player_count == world->player_count ? ddnet_player_character(previous, player) : NULL;
    if (before) tee_speeds(previous, before, &a0, &r0);
    else {
      a0 = a1;
      r0 = r1;
    }
  }
  *after = a0 + (a1 - a0) * frame->alpha;
  *raw = r0 + (r1 - r0) * frame->alpha;
  return true;
}

// --- drawing -----------------------------------------------------------------
//
// Everything is drawn with the engine's smooth shapes: the hard-edged ones are for DDNet's own
// art, which has them in DDNet too.

typedef struct {
  ft_game *game;
  float scale; // screen pixels per HUD unit
} hud_t;

static ft_vec2 hud_point(const hud_t *hud, float x, float y) {
  return hud->game->engine->screen_to_world((ft_vec2){x * hud->scale, y * hud->scale});
}

// The length of `units` HUD units in world units.
static float hud_length(const hud_t *hud, float units) {
  const ft_vec2 a = hud_point(hud, 0.f, 0.f), b = hud_point(hud, 0.f, units);
  return b.y - a.y;
}

// A rectangle with rounded corners; a radius of half its height makes a pill.
static void hud_rect(const hud_t *hud, float z, float x, float y, float w, float h, float radius, ft_color color) {
  enum { STEPS = 6 };
  radius = fminf(radius, fminf(w, h) * .5f);
  // Clockwise from the top right corner, each turning a quarter.
  const float corner_x[4] = {x + w - radius, x + w - radius, x + radius, x + radius};
  const float corner_y[4] = {y + radius, y + h - radius, y + h - radius, y + radius};
  ft_vec2 points[4 * (STEPS + 1)];
  int count = 0;
  for (int corner = 0; corner < 4; ++corner)
    for (int step = 0; step <= STEPS; ++step) {
      const float angle = (float)M_PI * .5f * ((float)corner - 1.f + (float)step / STEPS);
      points[count++] = hud_point(hud, corner_x[corner] + cosf(angle) * radius, corner_y[corner] + sinf(angle) * radius);
    }
  hud->game->engine->draw_polygon_smooth(z, points, (uint32_t)count, color);
}

static void hud_circle(const hud_t *hud, float z, float x, float y, float radius, ft_color color) {
  enum { STEPS = 32 };
  ft_vec2 points[STEPS];
  for (int step = 0; step < STEPS; ++step) {
    const float angle = 2.f * (float)M_PI * (float)step / STEPS;
    points[step] = hud_point(hud, x + cosf(angle) * radius, y + sinf(angle) * radius);
  }
  hud->game->engine->draw_polygon_smooth(z, points, STEPS, color);
}

static void hud_line(const hud_t *hud, float x0, float y0, float x1, float y1, float thickness, ft_color color) {
  const ft_vec2 points[2] = {hud_point(hud, x0, y0), hud_point(hud, x1, y1)};
  hud->game->engine->draw_polyline_smooth(Z_SHAPES, points, 2, color, hud_length(hud, thickness), false);
}

static void hud_dashes(const hud_t *hud, float x0, float x1, float y, float thickness, ft_color color) {
  for (float x = x0; x < x1; x += 4.f) hud_line(hud, x, y, fminf(x + 2.f, x1), y, thickness, color);
}

// Text `size` units high with its top left at (x, y).
static void hud_text(const hud_t *hud, float x, float y, float size, ft_color color, const char *text) {
  dd_draw_text_screen(hud->game, x * hud->scale, y * hud->scale, size * hud->scale, color, text);
}

static float text_width(const hud_t *hud, float size, const char *text) { return dd_text_width(hud->game, size, text); }

// A bar `fraction` full, over its own faint track.
static void hud_bar(const hud_t *hud, float x, float y, float w, float h, float fraction) {
  hud_rect(hud, Z_SHAPES - .001f, x, y, w, h, h / 2.f, (ft_color){1.f, 1.f, 1.f, .15f});
  if (fraction > 0.f) hud_rect(hud, Z_SHAPES, x, y, w * fminf(fraction, 1.f), h, h / 2.f, ACCENT);
}

// 20000 as "20,000".
static void format_count(int value, char *out, size_t size) {
  char digits[16];
  snprintf(digits, sizeof(digits), "%d", value > 0 ? value : 0);
  const size_t length = strlen(digits);
  size_t at = 0;
  for (size_t i = 0; i < length && at + 2 < size; ++i) {
    if (i > 0 && (length - i) % 3 == 0) out[at++] = ',';
    out[at++] = digits[i];
  }
  out[at] = '\0';
}

// The three panels share a row along the bottom of the screen: position at the left, speed in the
// middle, the ramp at the right.
#define PANEL_MARGIN 8.f
#define PANEL_WIDTH 112.f
#define PANEL_HEIGHT 46.f

// How far along the map the tee is, in blocks from its left edge, out of the map's width.
static void draw_position(const hud_t *hud, float blocks, int map_blocks) {
  const float panel_x = PANEL_MARGIN, panel_y = HUD_HEIGHT - PANEL_MARGIN - PANEL_HEIGHT, padding = 6.f;
  hud_rect(hud, Z_PANEL, panel_x, panel_y, PANEL_WIDTH, PANEL_HEIGHT, 3.f, PANEL);
  hud_text(hud, panel_x + 4.f, panel_y + 3.f, 6.f, DIM, "Position");

  // The number right-aligned in the width the map's own takes, so the total beside it stays put.
  const float number_size = 14.f, total_size = 8.f, small = 6.f, top = panel_y + 10.f;
  char number[32], total[32], text[48];
  format_count((int)blocks, number, sizeof(number));
  format_count(map_blocks, total, sizeof(total));
  const float box = text_width(hud, number_size, total);
  const float left = panel_x + padding;
  hud_text(hud, left + box - text_width(hud, number_size, number), top, number_size, (ft_color){1.f, 1.f, 1.f, 1.f}, number);
  snprintf(text, sizeof(text), "/ %s", total);
  hud_text(hud, left + box + 3.f, top + .8f * (number_size - total_size), total_size, DIM, text);

  const float bar_w = PANEL_WIDTH - 2.f * padding, bar_y = top + number_size + 4.f, label_y = bar_y + 6.f;
  const float fraction = map_blocks > 0 ? blocks / (float)map_blocks : 0.f;
  hud_bar(hud, left, bar_y, bar_w, 3.f, fraction);
  snprintf(text, sizeof(text), "%.2f%%", 100.f * fraction);
  hud_text(hud, left, label_y, small, DIM, text);
  format_count(map_blocks - (int)blocks, number, sizeof(number));
  snprintf(text, sizeof(text), "%s to go", number);
  hud_text(hud, left + bar_w - text_width(hud, small, text), label_y, small, DIM, text);
}

// The number, the bar from nothing to the ramp's limit, and the raw speed under them.
static void draw_readout(const hud_t *hud, float width, const ddnet_tuning_values_t *tuning, float after, float raw) {
  const float center_x = width / 2.f, panel_y = HUD_HEIGHT - PANEL_MARGIN - PANEL_HEIGHT, padding = 6.f;
  hud_rect(hud, Z_PANEL, center_x - PANEL_WIDTH / 2.f, panel_y, PANEL_WIDTH, PANEL_HEIGHT, 3.f, PANEL);

  // Right-aligned in the width its digits can take, so it does not shake as they change.
  const float number_size = 22.f, unit_size = 8.f, gap = 3.f, small = 6.f, top = panel_y + 3.f;
  char text[48];
  snprintf(text, sizeof(text), "%.2f", after);
  const float box = text_width(hud, number_size, after >= 100.f ? "000.00" : "00.00");
  const float left = center_x - (box + gap + text_width(hud, unit_size, "BPS")) / 2.f;
  hud_text(hud, left + box - text_width(hud, number_size, text), top, number_size, (ft_color){1.f, 1.f, 1.f, 1.f}, text);
  // (on the number's baseline, roughly four fifths down a line)
  hud_text(hud, left + box + gap, top + .8f * (number_size - unit_size), unit_size, DIM, "BPS");

  // Under the bar, the share of the limit at its left and the raw speed at its right.
  const float bar_w = PANEL_WIDTH - 2.f * padding, bar_x = center_x - bar_w / 2.f, bar_y = top + number_size + 4.f;
  const float label_y = bar_y + 6.f;
  const float limit = summit(tuning) > 0.f ? ramped(tuning, summit(tuning)) : -1.f;
  if (limit > 0.f) {
    hud_bar(hud, bar_x, bar_y, bar_w, 3.f, after / limit);
    snprintf(text, sizeof(text), "%.2f%% of %.2f", 100.f * after / limit, limit);
    hud_text(hud, bar_x, label_y, small, DIM, text);
  }
  snprintf(text, sizeof(text), "raw %.2f", raw);
  hud_text(hud, bar_x + bar_w - text_width(hud, small, text), label_y, small, DIM, text);
}

// What the ramp makes of every raw speed, the part the tee has been through drawn bright, and the
// tee as a dot on it.
static void draw_curve(const hud_t *hud, float width, const ddnet_tuning_values_t *tuning, float raw) {
  const float panel_h = 66.f;
  const float panel_x = width - PANEL_MARGIN - PANEL_WIDTH, panel_y = HUD_HEIGHT - PANEL_MARGIN - panel_h;
  hud_rect(hud, Z_PANEL, panel_x, panel_y, PANEL_WIDTH, panel_h, 3.f, PANEL);
  hud_text(hud, panel_x + 4.f, panel_y + 3.f, 6.f, DIM, "Velocity ramp");

  // The plot: raw speed across, the speed it moves the tee at up. A ramp without a summit is
  // shown as far as the tee has gone and then half again.
  const float top_raw = summit(tuning);
  const float limit = top_raw > 0.f ? ramped(tuning, top_raw) : -1.f;
  float max_x = top_raw > 0.f ? top_raw * 1.6f : fmaxf(raw * 1.5f, 50.f);
  max_x = ceilf(max_x / 50.f) * 50.f;
  const float max_y = (limit > 0.f ? limit : ramped(tuning, max_x)) * 1.2f;
  const float plot_x = panel_x + 6.f, plot_w = PANEL_WIDTH - 12.f, plot_y = panel_y + 13.f, plot_h = panel_h - 23.f;
#define PX(v) (plot_x + plot_w * (v) / max_x)
#define PY(v) (plot_y + plot_h * (1.f - (v) / max_y))

  hud_line(hud, plot_x, plot_y + plot_h, plot_x + plot_w, plot_y + plot_h, .5f, FAINT);
  char text[32];
  if (limit > 0.f) {
    hud_dashes(hud, plot_x, plot_x + plot_w, PY(limit), .5f, FAINT);
    snprintf(text, sizeof(text), "%.2f", limit);
    hud_text(hud, plot_x + plot_w - text_width(hud, 5.f, text), PY(limit) - 6.f, 5.f, DIM, text);
    snprintf(text, sizeof(text), "%.0f", top_raw);
    hud_text(hud, PX(top_raw) - text_width(hud, 5.f, text) / 2.f, plot_y + plot_h + 1.5f, 5.f, DIM, text);
    hud_line(hud, PX(top_raw), plot_y + plot_h, PX(top_raw), plot_y + plot_h + 1.5f, .5f, FAINT);
  }
  hud_text(hud, plot_x, plot_y + plot_h + 1.5f, 5.f, DIM, "0");
  hud_text(hud, plot_x + plot_w - text_width(hud, 5.f, "raw BPS"), plot_y + plot_h + 1.5f, 5.f, DIM, "raw BPS");

  // Two lines that meet at the dot: the way the tee came, and the rest.
  const float shown = fminf(raw, max_x);
  const ft_vec2 dot = hud_point(hud, PX(shown), PY(ramped(tuning, shown)));
  ft_vec2 points[CURVE_SEGMENTS + 2];
  uint32_t count = 0;
  points[count++] = hud_point(hud, PX(0.f), PY(0.f));
  int step = 1;
  for (; step <= CURVE_SEGMENTS && max_x * (float)step / CURVE_SEGMENTS < shown; ++step) {
    const float x = max_x * (float)step / CURVE_SEGMENTS;
    points[count++] = hud_point(hud, PX(x), PY(ramped(tuning, x)));
  }
  points[count++] = dot;
  hud->game->engine->draw_polyline_smooth(Z_SHAPES + .001f, points, count, ACCENT, hud_length(hud, 1.2f), false);
  count = 0;
  points[count++] = dot;
  for (; step <= CURVE_SEGMENTS; ++step) {
    const float x = max_x * (float)step / CURVE_SEGMENTS;
    points[count++] = hud_point(hud, PX(x), PY(ramped(tuning, x)));
  }
  if (count > 1) hud->game->engine->draw_polyline_smooth(Z_SHAPES, points, count, FAINT, hud_length(hud, 1.f), false);

  hud_circle(hud, Z_SHAPES + .002f, PX(shown), PY(ramped(tuning, shown)), 2.4f, ACCENT);
  hud_circle(hud, Z_SHAPES + .003f, PX(shown), PY(ramped(tuning, shown)), 1.4f, (ft_color){1.f, 1.f, 1.f, 1.f});
#undef PX
#undef PY
}

void dd_speed_hud_render(ft_game *game, const ft_render_frame *frame) {
  const dd_settings_t *settings = &game->settings;
  const bool speed = settings->render_speed || settings->render_speed_curve;
  if ((!speed && !settings->render_position) || !frame->active || !frame->world || frame->world_index < 0) return;
  if (!game->engine->draw_polygon_smooth || !game->engine->draw_polyline_smooth) return;
  ft_camera camera;
  game->engine->camera_get(&camera);
  if (camera.viewport.y <= 0.f) return;
  const hud_t hud = {.game = game, .scale = camera.viewport.y / HUD_HEIGHT};
  const float width = camera.viewport.x / hud.scale;

  const ft_world *world = frame->world;
  const int player = dd_hud_player(frame);
  const ddnet_character_t *chr = player >= 0 ? ddnet_player_character(world, player) : NULL;
  if (!chr) return;

  if (settings->render_position && frame->level) {
    const float x = chr->prev_pos.x + (chr->pos.x - chr->prev_pos.x) * frame->alpha;
    draw_position(&hud, fmaxf(x / PX_PER_TILE, 0.f), frame->level->collision.width);
  }
  float after, raw;
  if (!speed || !shown_speeds(game, frame, player, &after, &raw)) return;
  const ddnet_tuning_values_t *tuning = tee_tuning(world, chr);
  if (settings->render_speed) draw_readout(&hud, width, tuning, after, raw);
  if (settings->render_speed_curve) draw_curve(&hud, width, tuning, raw);
}

void dd_speed_hud_cleanup(ft_game *game) {
  for (int world = 0; world < game->speed_history_worlds; ++world) {
    free(game->speed_history[world].top);
    free(game->speed_history[world].raw);
  }
  free(game->speed_history);
  game->speed_history = NULL;
  game->speed_history_worlds = 0;
}
