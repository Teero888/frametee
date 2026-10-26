// DDNet's race HUD: the timer at the top of the screen and, under it, the checkpoint a tee just
// reached or its finish (CHud::RenderGameTimer and CHud::RenderDDRaceEffects).
//
// DDNet compares a checkpoint with the player's own best. Here a run is compared with the other
// groups of the project: under the timer goes how far the tee is behind the fastest of them at
// the checkpoint it just reached, or its own time there when none of them is faster.
#include "dd_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// DDNet lays its HUD out on a screen 300 units high.
#define HUD_HEIGHT 300.f
// A split shows for six seconds, the last two of them fading (CHud::RenderDDRaceEffects).
#define SPLIT_TICKS (6 * GAME_TICK_SPEED)
#define SPLIT_FADE_TICKS (2 * GAME_TICK_SPEED)

// The fastest the tees of one world were at each checkpoint and at the finish, over its whole run.
struct dd_race_splits_t {
  bool valid;
  uint64_t revision;
  float checkpoint[DDNET_MAX_CHECKPOINTS]; // race seconds; 0 where none of them got there
  int finish_ticks;                        // -1 where none of them finished
};

static void take_checkpoints(struct dd_race_splits_t *splits, const ddnet_character_t *chr) {
  for (int cp = 0; cp < DDNET_MAX_CHECKPOINTS; ++cp) {
    const float time = chr->current_time_cp[cp];
    if (time > 0.f && (splits->checkpoint[cp] <= 0.f || time < splits->checkpoint[cp])) splits->checkpoint[cp] = time;
  }
}

// One world's splits from where its tees are at the end of the timeline and, for those that
// finished, at the tick they did: the times of a finished run stay even if the tee dies after it.
static void collect_splits(ft_game *game, uint32_t world_index, const ft_timeline_world_info *info, int32_t end_tick,
                           struct dd_race_splits_t *splits) {
  memset(splits->checkpoint, 0, sizeof(splits->checkpoint));
  splits->finish_ticks = -1;
  const ft_engine_api *engine = game->engine;
  const ft_world *world = NULL;
  if (end_tick < info->start_offset || !engine->timeline_world_pair(world_index, end_tick, NULL, &world) || !world) return;

  int finish_ticks[DDNET_MAX_CLIENTS];
  int finished = 0;
  for (int player = 0; player < world->player_count; ++player) {
    const int client = ddnet_player_client(world, player);
    if (client < 0) continue;
    const ddnet_character_t *chr = ddnet_player_character(world, player);
    if (chr) take_checkpoints(splits, chr);
    const ddnet_player_t *state = &world->core.players[client];
    if (state->finish_tick < 0) continue;
    if (splits->finish_ticks < 0 || state->finish_time_ticks < splits->finish_ticks) splits->finish_ticks = state->finish_time_ticks;
    finish_ticks[finished++] = state->finish_tick;
  }
  // (each query hands back a world that is only good until the next one)
  for (int i = 0; i < finished; ++i) {
    const ft_world *at_finish = NULL;
    if (!engine->timeline_world_pair(world_index, finish_ticks[i] + info->start_offset, NULL, &at_finish) || !at_finish) continue;
    for (int player = 0; player < at_finish->player_count; ++player) {
      const ddnet_character_t *chr = ddnet_player_character(at_finish, player);
      if (chr) take_checkpoints(splits, chr);
    }
  }
}

// Before the frame is drawn: the splits of every world whose simulation changed since they were
// taken. The world being recorded changes with every tick and is left until the take is done.
void dd_race_hud_update(ft_game *game, const ft_engine_state *state) {
  const ft_engine_api *engine = game->engine;
  if (!game->settings.render_splits || !engine->timeline_world_count || !engine->timeline_world_info ||
      !engine->timeline_world_pair || !engine->timeline_range)
    return;
  const int count = (int)engine->timeline_world_count();
  if (count > game->race_split_worlds) {
    struct dd_race_splits_t *grown = realloc(game->race_splits, sizeof(*grown) * (size_t)count);
    if (!grown) return;
    memset(grown + game->race_split_worlds, 0, sizeof(*grown) * (size_t)(count - game->race_split_worlds));
    game->race_splits = grown;
    game->race_split_worlds = count;
  }
  int32_t start_tick = 0, end_tick = 0;
  if (!engine->timeline_range(&start_tick, &end_tick)) return;
  const int recorded = state->recording && engine->timeline_active_world ? engine->timeline_active_world() : -1;
  for (int world = 0; world < count; ++world) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (world == recorded || !engine->timeline_world_info((uint32_t)world, &info)) continue;
    struct dd_race_splits_t *splits = &game->race_splits[world];
    if (splits->valid && splits->revision == info.physics_revision) continue;
    collect_splits(game, (uint32_t)world, &info, end_tick, splits);
    splits->revision = info.physics_revision;
    splits->valid = true;
  }
}

// The fastest any other world was at checkpoint `cp` (cp < 0: the finish), in race seconds, or
// a negative number where none of them got there.
static float fastest_other(const ft_game *game, int world_index, int cp) {
  float best = -1.f;
  for (int world = 0; world < game->race_split_worlds; ++world) {
    const struct dd_race_splits_t *splits = &game->race_splits[world];
    if (world == world_index || !splits->valid) continue;
    float time = -1.f;
    if (cp >= 0) time = splits->checkpoint[cp];
    else if (splits->finish_ticks >= 0) time = (float)splits->finish_ticks / GAME_TICK_SPEED;
    if (time > 0.f && (best < 0.f || time < best)) best = time;
  }
  return best;
}

// DDNet's timer (str_time with ETimeFormat::DAYS), with centiseconds: 00:00.00, 00:00:00.00 or
// 0d 00:00:00.00.
static void format_clock(int centisecs, char *out, size_t size) {
  if (centisecs < 0) centisecs = 0;
  const int seconds = centisecs / 100, hundredths = centisecs % 100;
  if (seconds >= 24 * 3600)
    snprintf(out, size, "%dd %02d:%02d:%02d.%02d", seconds / (24 * 3600), seconds % (24 * 3600) / 3600, seconds % 3600 / 60,
             seconds % 60, hundredths);
  else if (seconds >= 3600)
    snprintf(out, size, "%02d:%02d:%02d.%02d", seconds / 3600, seconds % 3600 / 60, seconds % 60, hundredths);
  else
    snprintf(out, size, "%02d:%02d.%02d", seconds / 60, seconds % 60, hundredths);
}

// str_time_float with ETimeFormat::HOURS_CENTISECS: 00.00, 00:00.00 or 00:00:00.00.
static void format_race_time(float seconds, char *out, size_t size) {
  long long centisecs = llround((double)seconds * 1000.0) / 10;
  if (centisecs < 0) centisecs = 0;
  if (centisecs >= 360000)
    snprintf(out, size, "%02lld:%02lld:%02lld.%02lld", centisecs / 360000, centisecs % 360000 / 6000, centisecs % 6000 / 100, centisecs % 100);
  else if (centisecs >= 6000)
    snprintf(out, size, "%02lld:%02lld.%02lld", centisecs / 6000, centisecs % 6000 / 100, centisecs % 100);
  else
    snprintf(out, size, "%02lld.%02lld", centisecs % 6000 / 100, centisecs % 100);
}

// The tee the HUD is about: the frame's focus, else the one the camera's animation follows, else
// the selected one, else the first that is in the game.
int dd_hud_player(const ft_render_frame *frame) {
  const ft_world *world = frame->world;
  const int focus = dd_focus_player(frame);
  if (focus >= 0) return focus;
  const bool covers = frame->struct_size >= offsetof(ft_render_frame, camera_animated) + sizeof(frame->camera_animated);
  if (covers && frame->camera_animated)
    for (int player = 0; player < world->player_count && player < 64; ++player)
      if ((frame->followed_players >> player) & 1u) return player;
  if (frame->selected_player >= 0 && frame->selected_player < world->player_count) return frame->selected_player;
  for (int player = 0; player < world->player_count; ++player)
    if (ddnet_player_character(world, player)) return player;
  return world->player_count > 0 ? 0 : -1;
}

// Text `size` units high, its middle at `center_x`, its top at `y` (HUD units).
static void hud_text(ft_game *game, float scale, float center_x, float y, float size, float width, ft_color color,
                     const char *text) {
  dd_draw_text_screen(game, (center_x - width / 2.f) * scale, y * scale, size * scale, color, text);
}

void dd_race_hud_render(ft_game *game, const ft_render_frame *frame) {
  const bool timer = game->settings.render_timer, splits = game->settings.render_splits;
  if ((!timer && !splits) || !frame->active || !frame->world || frame->world_index < 0) return;
  ft_camera camera;
  game->engine->camera_get(&camera);
  if (camera.viewport.y <= 0.f) return;
  const float scale = camera.viewport.y / HUD_HEIGHT;
  const float center_x = camera.viewport.x / scale / 2.f;

  const ft_world *world = frame->world;
  const int player = dd_hud_player(frame);
  const int client = ddnet_player_client(world, player);
  if (client < 0) return;
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  const ddnet_player_t *state = &world->core.players[client];
  const int tick = ddnet_engine_tick(world);
  const bool racing = chr && chr->race_state == DDNET_RACE_STARTED;
  char text[64];

  if (timer) {
    // The race time while the tee races, else the round's, as long as the world has been going.
    const int ticks = racing ? tick - chr->start_time : tick;
    const int seconds = ticks / GAME_TICK_SPEED;
    format_clock(ticks * 100 / GAME_TICK_SPEED, text, sizeof(text));
    // centred on the width of the format, so it does not shake with the digits
    const char *shape = seconds >= 24 * 3600 * 100  ? "000d 00:00:00.00"
                        : seconds >= 24 * 3600 * 10 ? "00d 00:00:00.00"
                        : seconds >= 24 * 3600      ? "0d 00:00:00.00"
                        : seconds >= 3600           ? "00:00:00.00"
                                                    : "00:00.00";
    hud_text(game, scale, center_x, 2.f, 10.f, dd_text_width(game, 10.f, shape), (ft_color){1.f, 1.f, 1.f, 1.f}, text);
  }

  if (!splits) return;
  const ft_color red = {1.f, .5f, .5f, 1.f}, white = {1.f, 1.f, 1.f, 1.f};
  if (state->finish_tick >= 0 && tick >= state->finish_tick && tick - state->finish_tick < SPLIT_TICKS) {
    const int age = tick - state->finish_tick;
    const float alpha = age < SPLIT_TICKS - SPLIT_FADE_TICKS ? 1.f : (float)(SPLIT_TICKS - age) / SPLIT_FADE_TICKS;
    const float time = (float)state->finish_time_ticks / GAME_TICK_SPEED;
    char clock[32];
    format_race_time(time, clock, sizeof(clock));
    snprintf(text, sizeof(text), "Finish time: %s", clock);
    hud_text(game, scale, center_x, 20.f, 12.f, dd_text_width(game, 12.f, text), (ft_color){white.r, white.g, white.b, alpha}, text);
    const float best = fastest_other(game, frame->world_index, -1);
    if (best > 0.f && best < time) {
      format_race_time(time - best, clock, sizeof(clock));
      snprintf(text, sizeof(text), "+%s", clock);
      hud_text(game, scale, center_x, 34.f, 10.f, dd_text_width(game, 10.f, text), (ft_color){red.r, red.g, red.b, alpha}, text);
    }
  } else if (racing && chr->last_time_cp >= 0 && chr->last_time_cp < DDNET_MAX_CHECKPOINTS) {
    const int cp = chr->last_time_cp;
    const float time = chr->current_time_cp[cp];
    const int age = tick - (chr->start_time + (int)lroundf(time * GAME_TICK_SPEED));
    if (time <= 0.f || age < 0 || age >= SPLIT_TICKS) return;
    const float alpha = age < SPLIT_TICKS - SPLIT_FADE_TICKS ? 1.f : (float)(SPLIT_TICKS - age) / SPLIT_FADE_TICKS;
    const float best = fastest_other(game, frame->world_index, cp);
    char clock[32];
    ft_color color = white;
    if (best > 0.f && best < time) {
      format_race_time(time - best, clock, sizeof(clock));
      snprintf(text, sizeof(text), "+%s", clock);
      color = red;
    } else {
      format_race_time(time, text, sizeof(text));
    }
    color.a = alpha;
    hud_text(game, scale, center_x, 20.f, 10.f, dd_text_width(game, 10.f, text), color, text);
  }
}

void dd_race_hud_cleanup(ft_game *game) {
  free(game->race_splits);
  game->race_splits = NULL;
  game->race_split_worlds = 0;
}
