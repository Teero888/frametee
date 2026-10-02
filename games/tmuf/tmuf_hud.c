// What the game shows over the race (UIDriving2SingleUnited.Scene2d's
// FrameRaceCommon), in the race camera:
//
//   countdown  CTrackManiaRace::UpdateCountDownIndex: from the spawn to the
//              race start in three equal parts, "3", "2", "1", then "Go!" for
//              the race's first half second: MenuForever's 321Go.dds cells
//              (Frame321go: centred at (0, 0.0547), 0.109 x 0.117 half size),
//              each fully shown for 0.4 s, fading out by 0.5 s
//   race time  EntryRaceTime: the race's time (negative in the countdown, the
//              final one once finished) as m:ss.cc (h:mm:ss.cc from an hour),
//              hundredths truncated, in the Led font 0.1 high with a black
//              shadow 0.0333 of that to the right and down, centred at
//              (0, -0.71875) over UiBgBottomCenterRace.dds
//   speed      EntrySpeed: the car's forward speed in km/h, truncated, in the
//              same font, its right end at (-0.984375, -0.71875), over
//              UiBgCard.dds; above it EntryDistance, the metres driven (Led
//              0.04, right end at (-0.9453125, -0.65625)), and LabelMeters "m"
//              (United 0.04, 0.8 wide, no shadow, right end at -0.9765625).
//              Both numbers hold for a second after each checkpoint.
//   splits     FrameCheckPointInfo: for 2 s after a checkpoint (the finish
//              too) its time (EntryCheckPointTime, Led 0.06, white, centred
//              at (0.0078, 0.34375)) and under it the difference to the best
//              run's time there (EntryCheckPointDeltaTime at (0.0078,
//              0.28125)): "+" and red ($d00) when slower, blue ($00d, the
//              format's "-") when not; nothing without a best run. The best
//              run here: the fastest finished run of the timeline's other
//              worlds.
//
// The interface's space: x from 1 (the screen's left) to -1, y from -0.75
// (bottom) to 0.75, stretched over the whole view.

#include "tmuf_internal.h"

#include "hud_frag_spv.h"
#include "hud_vert_spv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RACE_START_MS TMUF_RACE_START_MS
#define GO_MS 500u

typedef struct hud_uniforms {
  float rect[4]; // clip space
  float uv[4];
  float tint[4];
} hud_uniforms;

typedef struct hud_glyph {
  uint32_t c;   // code point
  uint8_t page; // the font's picture
  float uv[4];
  float adv, x0, x1, y0, y1;
} hud_glyph;
typedef hud_glyph united_glyph;
#include "tmuf_font_united.h"

// Font_Led.Font.Gbx (Interface\\Media\\Font\\Led_Textures\\Led_00.dds): the chrono's
// characters: uv (u0 v0 u1 v1: v0 the top, the picture bottom row first), advance,
// x0 x1 y0 y1, in the font's units (height64 4325)
#define LED_HEIGHT64 4325.f
static const hud_glyph led_glyphs[] = {
    {'0', 0, {0.869171143f, 0.746154785f, 0.927764893f, 0.503967285f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'1', 0, {0.933624268f, 0.746154785f, 0.992218018f, 0.503967285f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'2', 0, {0.00390625f, 0.500061035f, 0.0625f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'3', 0, {0.0683898926f, 0.500061035f, 0.126983643f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'4', 0, {0.132843018f, 0.500061035f, 0.191436768f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'5', 0, {0.197296143f, 0.500061035f, 0.255889893f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'6', 0, {0.261749268f, 0.500061035f, 0.320343018f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'7', 0, {0.326202393f, 0.500061035f, 0.384796143f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'8', 0, {0.390655518f, 0.500061035f, 0.449249268f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'9', 0, {0.455108643f, 0.500061035f, 0.513702393f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {':', 0, {0.519561768f, 0.500061035f, 0.541046143f, 0.257873535f}, 704.f, 0.f, 704.f, -832.f, 3136.f},
    {'.', 0, {0.546905518f, 0.500061035f, 0.572296143f, 0.257873535f}, 832.f, 0.f, 832.f, -832.f, 3136.f},
    {'-', 0, {0.798858643f, 0.500061035f, 0.839874268f, 0.257873535f}, 1344.f, 0.f, 1344.f, -832.f, 3136.f},
    {'+', 0, {0.845733643f, 0.500061035f, 0.904327393f, 0.257873535f}, 1920.f, 0.f, 1920.f, -832.f, 3136.f},
    {'?', 0, {0.658233643f, 0.500061035f, 0.710968018f, 0.257873535f}, 1664.f, -64.f, 1664.f, -832.f, 3136.f},
};

static tg_texture load_picture(ft_game *game, const char *relative) {
  char path[1024];
  game->engine->resolve_data_path(relative, path, sizeof path);
  void *data = NULL;
  size_t size = 0;
  tg_texture texture = 0;
  if (game->engine->read_file(path, &data, &size)) {
    tm_image image;
    if (tm_image_load(data, size, tg_supports_bc(game->gpu), &image)) {
      texture = tg_texture_create(game->gpu, &image);
      tm_image_free(&image);
    }
    game->engine->free_file_data(data);
  }
  return texture;
}

bool tm_hud_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  const tg_program_desc desc = {.vertex_spirv = k_hud_vert_spv,
                                .vertex_spirv_size = sizeof k_hud_vert_spv,
                                .fragment_spirv = k_hud_frag_spv,
                                .fragment_spirv_size = sizeof k_hud_frag_spv,
                                .uniform_size = sizeof(hud_uniforms),
                                .texture_count = 1};
  game->hud_program = tg_program_create(game->gpu, &desc);
  game->countdown = load_picture(game, "GameData/MenuForever/Media/Texture/Image/321Go.dds");
  game->led_font = load_picture(game, "GameData/Interface/Media/Font/Led_Textures/Led_00.dds");
  game->race_time_back = load_picture(game, "GameData/Interface/Media/Texture/Image/UiBgBottomCenterRace.dds");
  game->speed_back = load_picture(game, "GameData/Interface/Media/Texture/Image/UiBgCard.dds");
  static const char *const pages[4] = {"GameData/Menu/Media/Font/United_Textures/United_00.dds",
                                       "GameData/Menu/Media/Font/United_Textures/United_01.dds", NULL,
                                       "GameData/Menu/Media/Font/United_Textures/United_03.dds"};
  for (int i = 0; i < 4; i++)
    game->united_pages[i] = pages[i] ? load_picture(game, pages[i]) : 0;
  return true;
}

void tm_hud_resources_destroy(ft_game *game) {
  if (game->hud_program) tg_program_destroy(game->gpu, game->hud_program);
  if (game->countdown) tg_texture_destroy(game->gpu, game->countdown);
  if (game->led_font) tg_texture_destroy(game->gpu, game->led_font);
  if (game->race_time_back) tg_texture_destroy(game->gpu, game->race_time_back);
  if (game->speed_back) tg_texture_destroy(game->gpu, game->speed_back);
  for (int i = 0; i < 4; i++)
    if (game->united_pages[i]) tg_texture_destroy(game->gpu, game->united_pages[i]);
  free(game->split_best.times);
  memset(&game->split_best, 0, sizeof game->split_best);
  game->hud_program = NULL;
  game->countdown = game->led_font = game->race_time_back = game->speed_back = 0;
  memset(game->united_pages, 0, sizeof game->united_pages);
}

// Where the interface goes on the view. The game stretches its 2 x 1.5 box
// over the whole view, so its look depends on the screen; here it always
// looks as on a 16:9 screen (the game's own pictures), whatever the view's
// shape: its height follows the view's, its width 16:9's, each element held
// at its own edge of the view (anchor, in ndc: the speed at the bottom
// right, the race time at the bottom centre)
typedef struct hud_layout {
  float kx, ky;         // the box's scale on the view, per axis
  float anchor[2];      // ndc (y up) the element keeps its place from
} hud_layout;

static hud_layout g_layout = {1.f, 1.f, {0.f, 0.f}};

static void layout_set(const ft_game *game, const ft_render_frame *frame, float anchor_x, float anchor_y) {
  const float w = frame->state.camera.viewport.x, h = frame->state.camera.viewport.y;
  const float aspect = w > 0.f && h > 0.f ? w / h : 4.f / 3.f;
  g_layout.kx = (16.f / 9.f) / aspect;
  g_layout.ky = 1.f;
  (void)game;
  g_layout.anchor[0] = anchor_x, g_layout.anchor[1] = anchor_y;
}

// a picture over the interface's rectangle (x0 left .. x1, y0 bottom .. y1,
// the interface's coordinates), uv (u0, v_top, u1, v_bottom)
static void quad(ft_game *game, tg_texture texture, float x0, float y0, float x1, float y1, const float uv[4],
                 const float rgba[4]) {
  // x: the interface's points to the left (ndc -x); y: ndc y / 0.75, then
  // Vulkan's clip space down
  const hud_layout *l = &g_layout;
  const float ax = l->anchor[0], ay = l->anchor[1];
  const float nx0 = ax + (-x0 - ax) * l->kx, nx1 = ax + (-x1 - ax) * l->kx;
  const float ny0 = ay + (y0 / 0.75f - ay) * l->ky, ny1 = ay + (y1 / 0.75f - ay) * l->ky;
  hud_uniforms u = {.rect = {nx0, -ny1, nx1, -ny0}, .uv = {uv[0], uv[1], uv[2], uv[3]}};
  memcpy(u.tint, rgba, sizeof u.tint);
  const tg_state blend = {.blend_src = TG_BLEND_SRCALPHA, .blend_dst = TG_BLEND_INVSRCALPHA, .cull = TG_CULL_NONE};
  const tg_sampler sampler = TG_SAMPLER_CLAMP;
  tg_draw(game->gpu, game->hud_program, &blend, NULL, 0, 6, &texture, &sampler, &u);
}

typedef struct font {
  const hud_glyph *glyphs; // by code point
  uint32_t fallback;       // shown for a code point the font does not have
  size_t count;
  float height64;
  bool has_rect; // V alignment 4: by the font's rect, else the glyphs' box
  float rect_y0, rect_y1;
} font;

static const font k_led = {led_glyphs, '?', sizeof led_glyphs / sizeof led_glyphs[0], LED_HEIGHT64, false, 0.f, 0.f};
// Font_United.Font.Gbx: height64 2232, rect y -0.2007 .. 0.6595
static const font k_united = {k_united_glyphs, 0x25a1, sizeof k_united_glyphs / sizeof k_united_glyphs[0], 2232.f,
                              true, -0.200716853f, 0.659498215f};

static const hud_glyph *glyph(const font *f, uint32_t c) {
  for (size_t i = 0; i < f->count; i++)
    if (f->glyphs[i].c == c) return &f->glyphs[i];
  for (size_t i = 0; i < f->count; i++)
    if (f->glyphs[i].c == f->fallback) return &f->glyphs[i];
  return &f->glyphs[f->count - 1];
}

// the code points of a UTF-8 text (at most cap)
static uint32_t decode_utf8(const char *text, uint32_t *out, uint32_t cap) {
  uint32_t n = 0;
  const unsigned char *c = (const unsigned char *)text;
  while (*c && n < cap) {
    uint32_t cp = *c++, more = 0;
    if (cp >= 0xf0) cp &= 7, more = 3;
    else if (cp >= 0xe0) cp &= 15, more = 2;
    else if (cp >= 0xc0) cp &= 31, more = 1;
    for (; more && (*c & 0xc0) == 0x80; more--)
      cp = cp << 6 | (*c++ & 63u);
    out[n++] = cp;
  }
  return n;
}

// CMwTimer::GetMmSsCcTimeStringFromMwTime
static void time_text(int32_t ms, char *out, size_t size) {
  const bool neg = ms < 0;
  const uint32_t a = neg ? (uint32_t)(-(int64_t)ms) : (uint32_t)ms;
  const uint32_t h = a / 3600000u, m = a % 3600000u / 60000u, s = a % 60000u / 1000u, cc = a % 1000u / 10u;
  if (h) snprintf(out, size, "%s%u:%02u:%02u.%02u", neg ? "-" : "", h, m, s, cc);
  else snprintf(out, size, "%s%u:%02u.%02u", neg ? "-" : "", m, s, cc);
}

// Text at (cx, cy) (CPlugTreeGenText, vertical alignment 4): centred on the
// font's rect or its glyphs' box vertically and, across (align), starting
// there (0), centred (1) or ending there (2); with a shadow ($s: black under
// a light text) first; the glyphs' boxes (and the shadows') laid out,
// squeezed to max_width; wf the width factor. pages: the font's pictures
static void text_draw(ft_game *game, const tg_texture *pages, const font *f, const char *text, float cx, float cy,
                      float height, float wf, float max_width, int align, bool shadow, const float rgba[4]) {
  uint32_t codes[160];
  const uint32_t n = decode_utf8(text, codes, 160);
  if (!n) return;
  const float s = height / f->height64, d = shadow ? -0.0333f * height : 0.f;
  // interface x points left: the pen moves toward -x
  float pen = 0.f, minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (uint32_t i = 0; i < n; i++) {
    const hud_glyph *g = glyph(f, codes[i]);
    const float xa = pen - g->x1 * s * wf, xb = pen - g->x0 * s * wf, ya = g->y0 * s, yb = g->y1 * s;
    minx = fminf(minx, fminf(xa, xa + d)), maxx = fmaxf(maxx, fmaxf(xb, xb + d));
    miny = fminf(miny, fminf(ya, ya + d)), maxy = fmaxf(maxy, fmaxf(yb, yb + d));
    pen -= g->adv * s * wf;
  }
  const float advance = -pen, k = max_width > 0.f && advance > max_width ? max_width / advance : 1.f;
  const float half_w = align == 2 ? -minx : align == 1 ? (maxx - minx) * 0.5f : 0.f;
  const float mid_y = f->has_rect ? 0.5f * height * (f->rect_y0 + f->rect_y1) : (maxy + miny) * 0.5f;
  const float light = fmaxf(rgba[0], fmaxf(rgba[1], rgba[2])), shade_v = light > 0.5f ? 0.f : 1.f;
  const float shade[4] = {shade_v, shade_v, shade_v, rgba[3]};
  for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
    const float off = pass ? 0.f : d;
    pen = 0.f;
    for (uint32_t i = 0; i < n; i++) {
      const hud_glyph *g = glyph(f, codes[i]);
      const float xa = pen - g->x1 * s * wf + off, xb = pen - g->x0 * s * wf + off;
      const float ya = g->y0 * s + off, yb = g->y1 * s + off;
      // on screen xb is the glyph's left
      if (pages[g->page])
        quad(game, pages[g->page], cx + (xb + half_w) * k, cy + ya - mid_y, cx + (xa + half_w) * k, cy + yb - mid_y,
             g->uv, pass ? rgba : shade);
      pen -= g->adv * s * wf;
    }
  }
}

// FrameChallengeInfo: the map's name and its author's login on UiBgCard,
// right-aligned at the top right, United 0.04 x 0.8, white
static void challenge_info(ft_game *game, const ft_level *level) {
  static const float white[4] = {1.f, 1.f, 1.f, 1.f}, full[4] = {0.f, 1.f, 1.f, 0.f};
  if (game->speed_back) quad(game, game->speed_back, -0.6875f, 0.625f, -1.f, 0.734375f, full, white);
  char name[256], author[128];
  tm_strip_formatting(level->info && level->info->name[0] ? level->info->name : level->name, name, sizeof name);
  tm_strip_formatting(level->info ? level->info->author_login : "", author, sizeof author);
  text_draw(game, game->united_pages, &k_united, name, -0.984375f, 0.703125f, 0.04f, 0.8f, 0.234375f, 2, false, white);
  text_draw(game, game->united_pages, &k_united, author, -0.984375f, 0.65625f, 0.04f, 0.8f, 0.234375f, 2, false,
            white);
}

// CTrackManiaRace::UpdateAsync, InputRace: every tick the car's forward
// speed, trunc(|v_local.z| 3.6) km/h, adds speed x 10 ms to the distance
// until the finish; the shown numbers take them unless a checkpoint was
// passed less than a second ago
void tm_race_display_step(ft_world *world) {
  const tmuf_dyna_state *b = &world->w.sim.body.state;
  const float vz = b->rot.m[0][2] * b->lin.x + b->rot.m[1][2] * b->lin.y + b->rot.m[2][2] * b->lin.z;
  const uint32_t speed = (uint32_t)fabsf(vz * 3.6f);
  const tmuf_race *race = &world->w.sim.race;
  tm_race_display *d = &world->display;
  if (!race->completed) d->dist += (float)speed * (float)TMUF_TICK_MS;
  const int32_t now = (int32_t)(world->w.tick * TMUF_TICK_MS) - (int32_t)RACE_START_MS;
  const bool hold = race->checkpoint_time_count &&
                    now - (int32_t)race->checkpoint_times[race->checkpoint_time_count - 1] < 1000;
  if (!hold) {
    d->speed = speed;
    d->distance = (uint32_t)(d->dist / 3600.f);
  }
}

// The best run the splits compare with, looked up again when the shown world
// or its checkpoint count changes: the fastest finished run among the
// timeline's other worlds, at the timeline's end
static void split_best_update(ft_game *game, const ft_render_frame *frame) {
  const tmuf_race *race = &frame->world->w.sim.race;
  if (game->split_best.world == frame->world && game->split_best.checkpoints == race->checkpoint_time_count) return;
  game->split_best.world = frame->world;
  game->split_best.checkpoints = race->checkpoint_time_count;
  game->split_best.count = 0;
  const ft_engine_api *api = game->engine;
  int32_t first = 0, last = 0;
  if (!api->timeline_world_pair || !api->timeline_world_count || !api->timeline_range ||
      !api->timeline_range(&first, &last))
    return;
  const uint32_t worlds = api->timeline_world_count();
  // the shown world's own (its current world at the shown tick)
  uint32_t shown = UINT32_MAX;
  for (uint32_t i = 0; i < worlds && shown == UINT32_MAX; i++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(i, (int32_t)frame->tick, &a, &b) && b == frame->world) shown = i;
  }
  if (shown == UINT32_MAX) return;
  uint32_t best_time = UINT32_MAX;
  for (uint32_t i = 0; i < worlds; i++) {
    const ft_world *a = NULL, *b = NULL;
    if (i == shown || !api->timeline_world_pair(i, last, &a, &b) || !b) continue;
    const tmuf_race *r = &b->w.sim.race;
    if (!r->completed || r->finish_time >= best_time || !r->checkpoint_time_count) continue;
    uint32_t *times = realloc(game->split_best.times, sizeof *times * r->checkpoint_time_count);
    if (!times) continue;
    memcpy(times, r->checkpoint_times, sizeof *times * r->checkpoint_time_count);
    game->split_best.times = times;
    game->split_best.count = r->checkpoint_time_count;
    best_time = r->finish_time;
  }
}

// FrameCheckPointInfo: the last checkpoint's time for 2 s, and the difference
// to the best run's time at the same checkpoint
static void splits(ft_game *game, const ft_render_frame *frame, int32_t now) {
  const tmuf_race *race = &frame->world->w.sim.race;
  const uint32_t n = race->checkpoint_time_count;
  if (!n) return;
  const int32_t at = (int32_t)race->checkpoint_times[n - 1];
  if (now < at || now - at >= 2000) return;
  split_best_update(game, frame);
  layout_set(game, frame, 0.f, 0.f); // the middle
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  char text[40];
  time_text(at, text, sizeof text);
  text_draw(game, &game->led_font, &k_led, text, 0.0078125f, 0.34375f, 0.06f, 1.f, 0.f, 1, true, white);
  if (n > game->split_best.count) return;
  const int32_t delta = at - (int32_t)game->split_best.times[n - 1];
  static const float red[4] = {0xd / 15.f, 0.f, 0.f, 1.f}, blue[4] = {0.f, 0.f, 0xd / 15.f, 1.f};
  if (delta > 0) {
    text[0] = '+';
    time_text(delta, text + 1, sizeof text - 1);
  } else {
    time_text(delta, text, sizeof text);
  }
  text_draw(game, &game->led_font, &k_led, text, 0.0078125f, 0.28125f, 0.06f, 1.f, 0.f, 1, true,
            delta > 0 ? red : blue);
}

void tm_hud_render(ft_game *game, const ft_render_frame *frame) {
  // the game's HUD belongs to its race camera
  if (!game->hud_program || !frame->world || frame->state.camera.mode != 0) return;
  const uint32_t ms = (uint32_t)frame->world->w.tick * TMUF_TICK_MS;

  if (game->settings.race_time && game->led_font) {
    layout_set(game, frame, 0.f, -1.f); // the bottom's middle
    if (game->race_time_back) {
      static const float full[4] = {0.f, 1.f, 1.f, 0.f}, white[4] = {1.f, 1.f, 1.f, 1.f};
      // ndc x +-0.203125, y -1.1667 .. -0.8333: half of it below the screen
      quad(game, game->race_time_back, 0.203125f, -1.1666667f * 0.75f, -0.203125f, -0.8333333f * 0.75f, full, white);
    }
    const tmuf_race *race = &frame->world->w.sim.race;
    const int32_t time = race->completed ? (int32_t)race->finish_time : (int32_t)ms - (int32_t)RACE_START_MS;
    char text[32];
    time_text(time, text, sizeof text);
    static const float white[4] = {1.f, 1.f, 1.f, 1.f};
    text_draw(game, &game->led_font, &k_led, text, 0.f, -0.71875f, 0.1f, 1.f, 0.3125f, 1, true, white);
  }
  if (game->settings.speed && game->led_font) {
    layout_set(game, frame, 1.f, -1.f); // the bottom right corner
    if (game->speed_back) {
      static const float white[4] = {1.f, 1.f, 1.f, 1.f};
      // measured on the game's 16:9 frame: the card's visible edges lie on
      // the box's (x 1811, y 1031 at 2000 x 1125), i.e. the picture's opaque
      // part (its 3 transparent columns at the left and 2 rows at the top
      // fall outside the box); the spec's whole picture would put them 3-5 px
      // inside
      static const float card[4] = {3.f / 256.f, 1.f - 2.f / 64.f, 1.f, 1.f / 64.f};
      quad(game, game->speed_back, -0.8125f, -0.75f, -1.125f, -0.625f, card, white);
    }
    const tm_race_display *d = &frame->world->display;
    char text[16];
    snprintf(text, sizeof text, "%u", d->speed);
    static const float white[4] = {1.f, 1.f, 1.f, 1.f};
    text_draw(game, &game->led_font, &k_led, text, -0.984375f, -0.71875f, 0.1f, 1.f, 0.15625f, 2, true, white);
    snprintf(text, sizeof text, "%u", d->distance);
    text_draw(game, &game->led_font, &k_led, text, -0.9453125f, -0.65625f, 0.04f, 1.f, 0.1171875f, 2, true, white);
    text_draw(game, game->united_pages, &k_united, "m", -0.9765625f, -0.65625f, 0.04f, 0.8f, 0.03125f, 2, false, white);
  }

  if (game->settings.challenge_info && game->level) {
    layout_set(game, frame, 1.f, 1.f); // the top right corner
    challenge_info(game, game->level);
  }

  if (game->settings.splits && game->led_font) splits(game, frame, (int32_t)ms - (int32_t)RACE_START_MS);

  if (!game->settings.countdown || !game->countdown) return;
  layout_set(game, frame, 0.f, 0.f); // the middle
  int index = -1;
  uint32_t shown_ms = 0; // since the picture appeared
  if (ms < RACE_START_MS) {
    index = (int)(ms * 3u / RACE_START_MS);
    shown_ms = ms - (uint32_t)index * RACE_START_MS / 3u;
  } else if (ms < RACE_START_MS + GO_MS) {
    index = 3;
    shown_ms = ms - RACE_START_MS;
  }
  if (index < 0) return;
  // fully shown for 0.4 s, gone by 0.5 s
  const float t = (float)shown_ms / 1000.f;
  const float alpha = t < 0.4f ? 1.f : t < 0.5f ? (0.5f - t) / 0.1f : 0.f;
  if (alpha <= 0.f) return;
  const float uv[4] = {(float)index * 0.25f, 1.f, (float)(index + 1) * 0.25f, 0.f};
  const float tint[4] = {1.f, 1.f, 1.f, alpha};
  quad(game, game->countdown, 0.109375f, 0.0546875f - 0.1171875f, -0.109375f, 0.0546875f + 0.1171875f, uv, tint);
}
