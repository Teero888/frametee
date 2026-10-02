// TrackMania United Forever as a FrameTee game module.
//
// This file is the ABI surface: identity, constraints, the input schema,
// levels, worlds, the properties the editor inspects, timeline events and
// settings. The track on screen is tm_scene.c, the car tm_car.c, the view
// tm_camera.c, the panels tm_ui.c and replay export tm_export.c.

#include "tmuf_internal.h"

#include "present_frag_spv.h"
#include "pack_frag_spv.h"
#include "pack_ms_frag_spv.h"
#include "present_vert_spv.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- TM_PROFILE ---------------------------------------------------------------------

#define PROF_STAGES 64
static struct {
  int on; // -1 unknown
  double last;
  const char *current;
  uint32_t frames;
  uint32_t count;
  const char *names[PROF_STAGES];
  double ms[PROF_STAGES];
  double frame_start, frame_ms;
} g_prof = {.on = -1};

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}

static void prof_close(double t) {
  if (!g_prof.current) return;
  uint32_t k = 0;
  while (k < g_prof.count && strcmp(g_prof.names[k], g_prof.current)) k++;
  if (k == g_prof.count && k < PROF_STAGES) g_prof.names[g_prof.count++] = g_prof.current, g_prof.ms[k] = 0.0;
  if (k < PROF_STAGES) g_prof.ms[k] += t - g_prof.last;
}

void tm_prof(ft_game *game, const char *name) {
  if (g_prof.on < 0) g_prof.on = getenv("TM_PROFILE") != NULL;
  if (!g_prof.on) return;
  const double t = now_ms();
  if (!g_prof.current) g_prof.frame_start = t;
  prof_close(t);
  g_prof.current = name;
  g_prof.last = t;
  if (game->gpu) tg_mark(game->gpu, name);
}

void tm_prof_frame_end(ft_game *game) {
  (void)game;
  if (g_prof.on <= 0 || !g_prof.current) return;
  const double t = now_ms();
  prof_close(t);
  g_prof.frame_ms += t - g_prof.frame_start;
  g_prof.current = NULL;
  if (++g_prof.frames < 120) return;
  fprintf(stderr, "cpu profile (%u frames): %.3f ms per frame (module)\n", g_prof.frames, g_prof.frame_ms / g_prof.frames);
  for (uint32_t k = 0; k < g_prof.count; k++)
    fprintf(stderr, "  cpu %-20s %7.3f ms\n", g_prof.names[k], g_prof.ms[k] / g_prof.frames);
  g_prof.frames = 0;
  g_prof.count = 0;
  g_prof.frame_ms = 0.0;
}

void tm_log(const ft_game *game, ft_log_level level, const char *fmt, ...) {
  if (!game || !game->engine || !game->engine->log) return;
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof buffer, fmt, args);
  va_end(args);
  game->engine->log(level, "TMUF", buffer);
}

// The tick the race starts on: the countdown holds the car before it.
#define RACE_START_TICK ((int32_t)(TMUF_RACE_START_MS / TMUF_TICK_MS))

// --- input schema ----------------------------------------------------------

static const ft_input_field input_fields[TM_FIELD_COUNT] = {
    {"accelerate", "Accelerate", "Throttle", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f, NULL, 0,
     {0.40f, 1.00f, 0.50f, 1.f}},
    {"brake", "Brake", "Brake and reverse", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f, NULL, 0,
     {1.00f, 0.45f, 0.40f, 1.f}},
    {"steer", "Steer", "Full lock is 65536 either way, as the game stores it", FT_INPUT_INT,
     FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X, -65536, 65536, 0, 0.f, 0.f, 0.f, NULL, 0,
     {0.45f, 0.75f, 1.00f, 1.f}},
    // Acts on the tick it is pressed, which the latched and trigger flags tell
    // the timeline to draw and to clear after every tick.
    {"respawn", "Respawn", "Return to the last checkpoint", FT_INPUT_BOOL,
     FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_LATCHED | FT_INPUT_FLAG_TRIGGER, 0, 1, 0, 0.f, 0.f, 0.f, NULL, 0,
     {1.00f, 0.85f, 0.35f, 1.f}},
    {"input_event", "Input event", "A key event that leaves the input as it was (cancels a master jump)", FT_INPUT_BOOL,
     FT_INPUT_FLAG_LATCHED | FT_INPUT_FLAG_EDITOR_HIDDEN, 0, 1, 0, 0.f, 0.f, 0.f, NULL, 0, {0.7f, 0.7f, 0.7f, 1.f}},
};

static const ft_input_control input_controls[] = {
    {"accelerate", "Accelerate", "Throttle", "TMUF", "UpArrow|W", TM_FIELD_ACCELERATE, 1, 0, NULL},
    {"brake", "Brake", "Brake and reverse", "TMUF", "DownArrow|S", TM_FIELD_BRAKE, 1, 0, NULL},
    {"steer_left", "Steer left", "Full lock left", "TMUF", "LeftArrow|A", TM_FIELD_STEER, -65536, FT_CONTROL_ADD, NULL},
    {"steer_right", "Steer right", "Full lock right", "TMUF", "RightArrow|D", TM_FIELD_STEER, 65536, FT_CONTROL_ADD, NULL},
    {"respawn", "Respawn", "Return to the last checkpoint", "TMUF", "Delete|Backspace", TM_FIELD_RESPAWN, 1, 0, NULL},
};

static const ft_input_schema input_schema = {
    sizeof(ft_input_schema),     sizeof(tm_input), _Alignof(tm_input), input_fields, TM_FIELD_COUNT,
    input_controls,              (uint32_t)(sizeof input_controls / sizeof input_controls[0]),
};

static void input_default(ft_game *game, void *record) { memset(record, 0, sizeof(tm_input)); }

static int64_t input_get(ft_game *game, const void *record, uint32_t field) {
  const tm_input *in = record;
  switch (field) {
  case TM_FIELD_ACCELERATE: return in->accelerate;
  case TM_FIELD_BRAKE: return in->brake;
  case TM_FIELD_STEER: return in->steer;
  case TM_FIELD_RESPAWN: return in->respawn;
  case TM_FIELD_INPUT_EVENT: return in->input_event;
  default: return 0;
  }
}

static void input_set(ft_game *game, void *record, uint32_t field, int64_t value) {
  tm_input *in = record;
  switch (field) {
  case TM_FIELD_ACCELERATE: in->accelerate = value != 0; break;
  case TM_FIELD_BRAKE: in->brake = value != 0; break;
  case TM_FIELD_STEER: in->steer = (int32_t)(value < -65536 ? -65536 : value > 65536 ? 65536 : value); break;
  case TM_FIELD_RESPAWN: in->respawn = value != 0; break;
  case TM_FIELD_INPUT_EVENT: in->input_event = value != 0; break;
  default: break;
  }
}

static void input_describe(ft_game *game, const void *record, char *out, size_t out_size) {
  const tm_input *in = record;
  snprintf(out, out_size, "%s%s steer %+d%s", in->accelerate ? "gas " : "", in->brake ? "brake " : "", in->steer,
           in->respawn ? " respawn" : "");
}

// --- properties --------------------------------------------------------------

// Index 0 must be the car's position: the engine reads it to keep a free 3D
// view pointed at the car.
enum {
  PROP_POSITION,
  PROP_VELOCITY,
  PROP_SPEED,
  PROP_GEAR,
  PROP_STEERING,
  PROP_WHEELS_ON_GROUND,
  PROP_CHECKPOINTS,
  PROP_RESPAWNS,
  PROP_TIME,
  PROP_STUNTS,
  PROP_COUNT,
};

static const ft_prop_desc car_props[PROP_COUNT] = {
    {"position", "Position", "Motion", "m", FT_VALUE_VEC3, FT_PROP_SUMMARY | FT_PROP_STARTING, 0.0, 0.0},
    {"velocity", "Velocity", "Motion", "m/s", FT_VALUE_VEC3, FT_PROP_SUMMARY | FT_PROP_STARTING, 0.0, 0.0},
    {"speed", "Speed", "Motion", "km/h", FT_VALUE_FLOAT, FT_PROP_SUMMARY, 0.0, 0.0},
    {"gear", "Gear", "Drivetrain", NULL, FT_VALUE_INT, 0, 0.0, 0.0},
    {"steering", "Steering", "Controls", NULL, FT_VALUE_FLOAT, 0, -1.0, 1.0},
    {"wheels_on_ground", "Wheels on ground", "Contact", NULL, FT_VALUE_INT, FT_PROP_SUMMARY, 0.0, 4.0},
    {"checkpoints", "Checkpoints", "Race", NULL, FT_VALUE_INT, FT_PROP_SUMMARY, 0.0, 0.0},
    {"respawns", "Respawns", "Race", NULL, FT_VALUE_INT, 0, 0.0, 0.0},
    {"time", "Race time", "Race", "s", FT_VALUE_FLOAT, 0, 0.0, 0.0},
    {"stunts", "Stunt score", "Race", NULL, FT_VALUE_INT, 0, 0.0, 0.0},
};

static const ft_entity_class entity_classes[] = {{"car", "Car", car_props, PROP_COUNT}};

static float race_time_s(const tmuf_world *w) {
  const int64_t ms = (int64_t)w->tick * TMUF_TICK_MS - TMUF_RACE_START_MS;
  return (float)ms / 1000.f;
}

static uint32_t wheels_on_ground(const tmuf_world *w) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < w->sim.car.wheel_count; i++)
    n += w->sim.car.wheels[i].contact != 0;
  return n;
}

static bool entity_prop_get(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                            ft_value *out) {
  if (!world || entity_class != FT_ENTITY_CLASS_PLAYER || entity != 0 || !out) return false;
  const tmuf_world *w = &world->w;
  const tmuf_dyna_state *s = &w->sim.body.state;
  switch (prop) {
  case PROP_POSITION: out->kind = FT_VALUE_VEC3, out->as.v3 = (ft_vec3){s->pos.x, s->pos.y, s->pos.z}; return true;
  case PROP_VELOCITY: out->kind = FT_VALUE_VEC3, out->as.v3 = (ft_vec3){s->lin.x, s->lin.y, s->lin.z}; return true;
  case PROP_SPEED:
    out->kind = FT_VALUE_FLOAT;
    out->as.f = sqrt((double)s->lin.x * s->lin.x + (double)s->lin.y * s->lin.y + (double)s->lin.z * s->lin.z) * 3.6;
    return true;
  case PROP_GEAR: out->kind = FT_VALUE_INT, out->as.i = w->sim.car.engine.gear; return true;
  case PROP_STEERING: out->kind = FT_VALUE_FLOAT, out->as.f = w->sim.car.controls.steering; return true;
  case PROP_WHEELS_ON_GROUND: out->kind = FT_VALUE_INT, out->as.i = wheels_on_ground(w); return true;
  case PROP_CHECKPOINTS: out->kind = FT_VALUE_INT, out->as.i = w->sim.race.checkpoints_passed; return true;
  case PROP_RESPAWNS: out->kind = FT_VALUE_INT, out->as.i = w->sim.race.respawns; return true;
  case PROP_TIME: out->kind = FT_VALUE_FLOAT, out->as.f = race_time_s(w); return true;
  case PROP_STUNTS: out->kind = FT_VALUE_INT, out->as.i = w->sim.race.stunts.score; return true;
  default: return false;
  }
}

// The starting state's overrides (the engine's editor in the Player Info
// tab): where the car starts and how fast. A replay always starts on the
// map's start block, so exported replays do not carry them.
static bool entity_prop_set(ft_game *game, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                            const ft_value *value) {
  if (!world || entity_class != FT_ENTITY_CLASS_PLAYER || entity != 0 || !value || value->kind != FT_VALUE_VEC3)
    return false;
  tmuf_dyna_state *s = &world->w.sim.body.state;
  const ft_vec3 v = value->as.v3;
  switch (prop) {
  case PROP_POSITION: s->pos = (tmuf_vec3){v.x, v.y, v.z}; return true;
  case PROP_VELOCITY: s->lin = (tmuf_vec3){v.x, v.y, v.z}; return true;
  default: return false;
  }
}

static int32_t entity_count(ft_game *game, const ft_world *world, uint32_t entity_class) {
  return entity_class == FT_ENTITY_CLASS_PLAYER ? 1 : 0;
}

// --- settings ----------------------------------------------------------------

enum {
  SETTING_TRACK,
  SETTING_CAR,
  SETTING_NEAR_DETAIL,
  SETTING_ANTIALIASING,
  SETTING_ANISOTROPY,
  SETTING_BLOOM,
  SETTING_SHADOWS,
  SETTING_FLARES,
  SETTING_WATER_REFLECTION,
  SETTING_HEADLIGHTS,
  SETTING_MARKS,
  SETTING_PARTICLES,
  SETTING_COUNTDOWN,
  SETTING_RACE_TIME,
  SETTING_SPEED,
  SETTING_CHALLENGE_INFO,
  SETTING_SPLITS,
  SETTING_MUSIC,
  SETTING_MUSIC_VOLUME,
  SETTING_SFX_VOLUME,
  SETTING_COUNT
};

static const ft_setting_desc setting_descs[SETTING_COUNT] = {
    {"draw_track", "Draw the track", NULL, "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"draw_car", "Draw the car", NULL, "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"near_detail", "Close-up detail", "The detail the game only shows near the camera, such as grass tufts",
     "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"antialiasing", "Antialiasing", "Samples per pixel (1: none), up to what the graphics card has", "Rendering",
     FT_VALUE_INT, 1, 16, FT_SETTING_RENDER},
    {"anisotropy", "Texture filtering", "Anisotropic filtering samples (1: trilinear)", "Rendering", FT_VALUE_INT, 1,
     16, FT_SETTING_RENDER},
    {"bloom", "Bloom", "The game's post effects: bright parts glow", "Rendering", FT_VALUE_BOOL, 0, 1,
     FT_SETTING_RENDER},
    {"shadows", "Shadows", "The car's shadow from the sun", "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"flares", "Lens flares", "The sun's and the lights' flares", "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"water_reflection", "Water reflection", "The sea reflects the sky and the scenery", "Rendering", FT_VALUE_BOOL, 0,
     1, FT_SETTING_RENDER},
    {"headlights", "Headlights", "At night the car's headlight lights the road ahead", "Rendering", FT_VALUE_BOOL, 0, 1,
     FT_SETTING_RENDER},
    {"marks", "Skid marks", "The wheels' marks on the ground", "Rendering", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"particles", "Particles", "The car's tyre smoke, dirt and gravel, and water spray and splashes", "Rendering",
     FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"countdown", "Countdown", "The start's 3, 2, 1, Go! (in the race camera)", "HUD", FT_VALUE_BOOL, 0, 1,
     FT_SETTING_RENDER},
    {"race_time", "Race time", "The race's time at the bottom (in the race camera)", "HUD", FT_VALUE_BOOL, 0, 1,
     FT_SETTING_RENDER},
    {"speed", "Speed", "The car's speed at the bottom right (in the race camera)", "HUD", FT_VALUE_BOOL, 0, 1,
     FT_SETTING_RENDER},
    {"challenge_info", "Track name", "The track's name and author, top right (in the race camera)", "HUD",
     FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"splits", "Checkpoint times",
     "At each checkpoint its time and the difference to the fastest finished run of the timeline's other worlds (in "
     "the race camera)",
     "HUD", FT_VALUE_BOOL, 0, 1, FT_SETTING_RENDER},
    {"music", "Music", NULL, "Sound", FT_VALUE_BOOL, 0, 1, 0},
    {"music_volume", "Music volume", NULL, "Sound", FT_VALUE_INT, 0, 100, 0},
    {"sfx_volume", "Effects volume", NULL, "Sound",
     FT_VALUE_INT, 0, 100, 0},
};

static bool *setting_bool(ft_game *game, uint32_t index) {
  switch (index) {
  case SETTING_TRACK: return &game->settings.draw_track;
  case SETTING_CAR: return &game->settings.draw_car;
  case SETTING_NEAR_DETAIL: return &game->settings.near_detail;
  case SETTING_BLOOM: return &game->settings.bloom;
  case SETTING_SHADOWS: return &game->settings.shadows;
  case SETTING_FLARES: return &game->settings.flares;
  case SETTING_WATER_REFLECTION: return &game->settings.water_reflection;
  case SETTING_HEADLIGHTS: return &game->settings.headlights;
  case SETTING_MARKS: return &game->settings.marks;
  case SETTING_PARTICLES: return &game->settings.particles;
  case SETTING_COUNTDOWN: return &game->settings.countdown;
  case SETTING_RACE_TIME: return &game->settings.race_time;
  case SETTING_SPEED: return &game->settings.speed;
  case SETTING_CHALLENGE_INFO: return &game->settings.challenge_info;
  case SETTING_SPLITS: return &game->settings.splits;
  case SETTING_MUSIC: return &game->settings.music;
  default: return NULL;
  }
}
static int *setting_int(ft_game *game, uint32_t index) {
  return index == SETTING_ANTIALIASING ? &game->settings.antialiasing
         : index == SETTING_ANISOTROPY   ? &game->settings.anisotropy
         : index == SETTING_MUSIC_VOLUME ? &game->settings.music_volume
         : index == SETTING_SFX_VOLUME   ? &game->settings.sfx_volume
                                         : NULL;
}

static uint32_t setting_count(ft_game *game) { return SETTING_COUNT; }
static const ft_setting_desc *setting_desc(ft_game *game, uint32_t index) {
  return index < SETTING_COUNT ? &setting_descs[index] : NULL;
}
static bool setting_get(ft_game *game, uint32_t index, ft_value *out) {
  bool *b = setting_bool(game, index);
  int *i = setting_int(game, index);
  if (!out || (!b && !i)) return false;
  if (b) out->kind = FT_VALUE_BOOL, out->as.b = *b;
  else out->kind = FT_VALUE_INT, out->as.i = *i;
  return true;
}
static bool setting_set(ft_game *game, uint32_t index, const ft_value *value) {
  bool *b = setting_bool(game, index);
  int *i = setting_int(game, index);
  if (!value) return false;
  if (b && value->kind == FT_VALUE_BOOL) {
    *b = value->as.b;
    return true;
  }
  if (i && value->kind == FT_VALUE_INT) {
    const ft_setting_desc *d = &setting_descs[index];
    const int64_t v = value->as.i;
    *i = (int)(v < d->min_value ? d->min_value : v > d->max_value ? d->max_value : v);
    return true;
  }
  return false;
}

// --- lifecycle ---------------------------------------------------------------

static ft_game *game_create(const ft_engine_api *engine) {
  ft_game *game = calloc(1, sizeof *game);
  if (!game) return NULL;
  game->engine = engine;
  game->settings = (tm_settings){.draw_track = true,
                                 .draw_car = true,
                                 .near_detail = true,
                                 .antialiasing = 16,
                                 .anisotropy = 16,
                                 .bloom = true,
                                 .shadows = true,
                                 .flares = true,
                                 .water_reflection = true,
                                 .headlights = true,
                                 .marks = true,
                                 .particles = true,
                                 .countdown = true,
                                 .race_time = true,
                                 .speed = true,
                                 .challenge_info = true,
                                 .splits = true,
                                 .music = true,
                                 .music_volume = 50,
                                 .sfx_volume = 100};
  char packs[1024];
  engine->resolve_data_path("Packs", packs, sizeof packs);
  char err[512];
  game->packs = tmuf_packs_open(packs, err, sizeof err);
  if (!game->packs) {
    tm_log(game, FT_LOG_ERROR, "Cannot open the game's packs (%s). Copy Packs and GameData from a TrackMania United "
                                 "Forever installation into data/games/tmuf.",
             err);
    free(game);
    return NULL;
  }
  return game;
}

static void game_destroy(ft_game *game) {
  if (!game) return;
  tm_audio_free(game);
  tmuf_packs_close(game->packs);
  free(game);
}

// The module draws with its own renderer (tmuf_gpu.h) on the engine's
// device, into a texture the engine shows over the viewport.
static bool resources_create(ft_game *game) {
  const ft_engine_api *api = game->engine;
  const ft_gpu_device *gpu = api->gpu_device ? api->gpu_device() : NULL;
  if (!gpu || gpu->api != FT_GPU_API_VULKAN || !api->pipeline_create || !api->texture_gpu_image) return true;
  char error[256];
  game->gpu = tg_create(gpu->physical_device, gpu->device, gpu->queue, gpu->queue_family_index, error, sizeof error);
  if (!game->gpu) {
    tm_log(game, FT_LOG_ERROR, "Cannot draw: %s", error);
    return true;
  }
  if (!tg_supports_bc(game->gpu)) tm_log(game, FT_LOG_WARN, "No BC textures on this device: decoding them");
  const ft_pipeline_desc desc = {.struct_size = sizeof desc,
                                 .vertex_spirv = k_present_vert_spv,
                                 .vertex_spirv_size = sizeof k_present_vert_spv,
                                 .fragment_spirv = k_present_frag_spv,
                                 .fragment_spirv_size = sizeof k_present_frag_spv,
                                 .texture_count = 1,
                                 // the scene's depth for the engine's own 3D drawing (shaders/present.frag)
                                 .depth_test = true};
  game->present = api->pipeline_create(&desc);
  // a quad over the viewport, in clip space (shaders/present.vert)
  const ft_vertex quad[4] = {{{-1, -1}, {1, 1, 1}, {0, 0}},
                             {{1, -1}, {1, 1, 1}, {1, 0}},
                             {{1, 1}, {1, 1, 1}, {1, 1}},
                             {{-1, 1}, {1, 1, 1}, {0, 1}}};
  const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
  game->quad = api->mesh_create(quad, 4, sizeof(ft_vertex), indices, 6);
  if (!game->present || !game->quad) tm_log(game, FT_LOG_ERROR, "Cannot show the frames");
  // the depth handed on beside the colour (shaders/pack.frag)
  if (!tg_enable_depth_export(game->gpu, k_pack_frag_spv, sizeof k_pack_frag_spv, k_pack_ms_frag_spv,
                              sizeof k_pack_ms_frag_spv))
    tm_log(game, FT_LOG_WARN, "No depth for the editor's own 3D drawing: %s", tg_error(game->gpu));
  return tm_track_resources_create(game) && tm_hud_resources_create(game) && tm_post_resources_create(game) &&
         tm_shadow_resources_create(game) && tm_flares_resources_create(game) && tm_pssm_resources_create(game) &&
         tm_ssao_resources_create(game) && tm_bake_resources_create(game);
}

static void release_frame(ft_game *game) {
  if (game->frame) game->engine->texture_destroy(game->frame);
  game->frame = NULL;
  game->frame_width = game->frame_height = 0;
}

static void resources_destroy(ft_game *game) {
  const ft_engine_api *api = game->engine;
  // GPU objects of the level go with the renderer; the next draw rebuilds them.
  if (game->level && game->level->scene) {
    tm_scene_destroy(game, game->level->scene);
    game->level->scene = NULL;
  }
  if (game->level && game->level->car) {
    tm_car_destroy(game, game->level->car);
    game->level->car = NULL;
  }
  tm_track_resources_destroy(game);
  tm_hud_resources_destroy(game);
  tm_post_resources_destroy(game);
  tm_shadow_resources_destroy(game);
  tm_pssm_resources_destroy(game);
  tm_ssao_resources_destroy(game);
  tm_bake_resources_destroy(game);
  tm_flares_resources_destroy(game);
  release_frame(game);
  if (game->present) api->pipeline_destroy(game->present);
  if (game->quad) api->mesh_destroy(game->quad);
  game->present = NULL;
  game->quad = NULL;
  tg_destroy(game->gpu);
  game->gpu = NULL;
}

// --- levels ------------------------------------------------------------------

static bool ends_with(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  if (a < b) return false;
  for (size_t i = 0; i < b; i++) {
    char x = s[a - b + i], y = suffix[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static void level_bounds(ft_level *level) {
  const tmuf_visuals *v = tmuf_track_visuals(level->track);
  for (int k = 0; k < 3; k++) {
    level->bounds_min[k] = FLT_MAX;
    level->bounds_max[k] = -FLT_MAX;
  }
  if (v)
    for (uint32_t i = 0; i < v->instance_count; i++) {
      const float p[3] = {v->instances[i].location.t.x, v->instances[i].location.t.y, v->instances[i].location.t.z};
      for (int k = 0; k < 3; k++) {
        if (p[k] < level->bounds_min[k]) level->bounds_min[k] = p[k];
        if (p[k] > level->bounds_max[k]) level->bounds_max[k] = p[k];
      }
    }
  if (level->bounds_min[0] > level->bounds_max[0]) {
    for (int k = 0; k < 3; k++)
      level->bounds_min[k] = 0.f, level->bounds_max[k] = 1024.f;
  }
}

static ft_level *level_load_memory(ft_game *game, const void *data, size_t size, const char *variant_id) {
  char err[512];
  ft_level *level = calloc(1, sizeof *level);
  if (!level) return NULL;
  // A replay carries its map and the settings it was driven with.
  tmuf_track_options opt = {NULL, 0, 0, 0};
  tmuf_replay *replay = tmuf_replay_load(data, size, err, sizeof err);
  const void *map = data;
  size_t map_size = size;
  if (replay) {
    map = tmuf_replay_map(replay, &map_size);
    opt.vehicle = tmuf_replay_vehicle(replay);
    opt.seed = tmuf_replay_seed(replay);
    opt.laps = tmuf_replay_laps(replay);
  }
  // Rendering data only when there is something to render it with.
  if (game->engine->texture_create) opt.flags |= TMUF_TRACK_VISUALS;
  level->map = malloc(map_size ? map_size : 1);
  if (level->map) {
    memcpy(level->map, map, map_size);
    level->map_size = map_size;
  }
  level->track = level->map ? tmuf_track_load(game->packs, level->map, level->map_size, &opt, err, sizeof err) : NULL;
  if (replay) tmuf_replay_free(replay);
  if (!level->track) {
    tm_log(game, FT_LOG_ERROR, "Cannot load the track: %s", level->map ? err : "out of memory");
    free(level->map);
    free(level);
    return NULL;
  }
  snprintf(level->name, sizeof level->name, "%s", tmuf_track_name(level->track));
  level->info = tmuf_challenge_info_read(level->map, level->map_size, NULL, 0);
  level_bounds(level);
  tm_light_of(game, level->track, &level->light);
  tm_audio_level_load(game, level);
  game->level = level;
  tm_log(game, FT_LOG_INFO, "Loaded %s (%s, %s)", level->name, tmuf_track_environment(level->track),
           tmuf_track_vehicle(level->track));
  return level;
}

static ft_level *level_load_path(ft_game *game, const char *path, const char *variant_id) {
  void *data = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &data, &size)) {
    tm_log(game, FT_LOG_ERROR, "Cannot read %s", path);
    return NULL;
  }
  ft_level *level = ends_with(path, ".gbx") ? level_load_memory(game, data, size, variant_id) : NULL;
  game->engine->free_file_data(data);
  return level;
}

static void level_destroy(ft_game *game, ft_level *level) {
  if (!level) return;
  if (level->scene) tm_scene_destroy(game, level->scene);
  if (level->car) tm_car_destroy(game, level->car);
  if (level->leaves) tm_leaves_destroy(game, level->leaves);
  for (uint32_t g = 0; g < TM_GROUPS_MAX; g++) {
    if (level->marks[g]) tm_marks_destroy(game, level->marks[g]);
    if (level->particles[g]) tm_particles_destroy(game, level->particles[g]);
  }
  if (game->level == level) game->level = NULL;
  tm_audio_level_free(game, level);
  tmuf_track_free(level->track);
  tmuf_challenge_info_free(level->info);
  free(level->map);
  free(level);
}

static bool level_info(ft_game *game, const ft_level *level, ft_level_info *out) {
  if (!level || !out) return false;
  out->name = level->name;
  out->bounds = (ft_rect){level->bounds_min[0], level->bounds_min[2], level->bounds_max[0] - level->bounds_min[0],
                          level->bounds_max[2] - level->bounds_min[2]};
  out->width_tiles = out->height_tiles = 0;
  const tmuf_sim *start = tmuf_track_sim(level->track);
  out->default_spawn = (ft_vec2){start->body.state.pos.x, start->body.state.pos.z};
  return true;
}

static size_t level_serialize(ft_game *game, const ft_level *level, void *out, size_t out_size) {
  if (!level) return 0;
  if (out && out_size >= level->map_size) memcpy(out, level->map, level->map_size);
  return level->map_size;
}

// --- worlds ------------------------------------------------------------------

static ft_world *world_create(ft_game *game, const ft_world_desc *desc) {
  if (!desc || !desc->level) return NULL;
  ft_world *world = calloc(1, sizeof *world);
  if (!world) return NULL;
  world->w = tmuf_world_empty();
  if (!tmuf_world_init(&world->w, desc->level->track)) {
    free(world);
    return NULL;
  }
  world->level = desc->level;
  tm_audio_world_init(world);
  return world;
}

static void world_destroy(ft_game *game, ft_world *world) {
  if (!world) return;
  tm_audio_world_free(world);
  tmuf_world_free(&world->w);
  free(world);
}

static void world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  tmuf_world_copy(&dst->w, &src->w);
  dst->display = src->display;
  tm_audio_copy(dst, src);
}

void tm_world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  const tm_input *in = player_count ? inputs : NULL;
  world->w.input = in ? (tmuf_input){in->accelerate, in->brake, in->respawn, in->input_event, in->steer}
                      : (tmuf_input){0, 0, 0, 0, 0};
  tm_audio_frame before;
  const bool sound = tm_audio_prepare(game, world, &before);
  tmuf_world_tick(&world->w);
  tm_race_display_step(world);
  if (sound) tm_audio_step(game, world, &before);
}

static int32_t world_tick(ft_game *game, const ft_world *world) { return (int32_t)world->w.tick; }
static int32_t world_player_count(ft_game *game, const ft_world *world) { return 1; }

static bool world_player_view(ft_game *game, const ft_world *world, int32_t player, ft_player_view *out) {
  if (!world || player != 0 || !out) return false;
  const tmuf_dyna_state *s = &world->w.sim.body.state;
  out->position = (ft_vec2){s->pos.x, s->pos.z};
  out->velocity = (ft_vec2){s->lin.x, s->lin.z};
  out->aim = (ft_vec2){0.f, 0.f};
  out->flags = FT_PLAYER_ALIVE | (world->w.sim.race.completed ? FT_PLAYER_FINISHED : 0u) |
               ((int32_t)world->w.tick < RACE_START_TICK ? FT_PLAYER_DISABLED : 0u);
  out->run_start_tick = RACE_START_TICK;
  return true;
}

// --- timeline events -------------------------------------------------------

static void collect_events(ft_game *game, const ft_world *previous, const ft_world *world,
                           void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  if (!previous || !world) return;
  const tmuf_race *before = &previous->w.sim.race, *now = &world->w.sim.race;
  char text[96];
  ft_timeline_event event = {.struct_size = sizeof event, .world_index = -1, .tick = (int32_t)world->w.tick, .player = 0,
                             .text = text};
  for (uint32_t i = before->checkpoint_time_count; i < now->checkpoint_time_count; i++) {
    const uint32_t ms = now->checkpoint_times[i];
    const bool finish = now->completed && i + 1 == now->checkpoint_time_count;
    snprintf(text, sizeof text, "%s  %u.%02us", finish ? "Finish" : "Checkpoint", ms / 1000u, ms % 1000u / 10u);
    event.category = finish ? "finish" : "checkpoint";
    event.color = finish ? (ft_color){0.30f, 0.90f, 0.40f, 1.f} : (ft_color){0.95f, 0.80f, 0.30f, 1.f};
    emit(user, &event);
  }
  if (now->respawns > before->respawns) {
    snprintf(text, sizeof text, "Respawn");
    event.category = "respawn";
    event.color = (ft_color){1.f, 0.55f, 0.35f, 1.f};
    emit(user, &event);
  }
}

// --- status ------------------------------------------------------------------

// EPlugSurfaceMaterialId
static const char *const surface_names[TMUF_MAT_COUNT] = {
    "Concrete", "Pavement",     "Grass",       "Ice",       "Metal",     "Sand",        "Dirt",       "Turbo",
    "DirtRoad", "Rubber",       "SlidingRubber", "Test",    "Rock",      "Water",       "Wood",       "Danger",
    "Asphalt",  "WetDirtRoad",  "WetAsphalt",  "WetPavement", "WetGrass", "Snow",       "ResonantMetal",
    "GolfBall", "GolfWall",     "GolfGround",  "Turbo2",    "Bumper",    "NotCollidable", "FreeWheeling",
    "TurboRoulette"};

// The viewport's overlay: the race and the car, as the race panel had them.
static uint32_t status_lines(ft_game *game, const ft_world *world, int32_t player, float alpha, char *out,
                             uint32_t max_lines, uint32_t line_size) {
  if (!world || !max_lines) return 0;
  const tmuf_world *w = &world->w;
  const tmuf_dyna_state *s = &w->sim.body.state;
  const tmuf_car *car = &w->sim.car;
  const tmuf_race *race = &w->sim.race;
  const double speed =
      sqrt((double)s->lin.x * s->lin.x + (double)s->lin.y * s->lin.y + (double)s->lin.z * s->lin.z) * 3.6;
  char lines[16][128];
  uint32_t n = 0;
  snprintf(lines[n++], 128, "%.0f km/h  gear %d  steer %.3f", speed, car->engine.gear, (double)car->controls.steering);
  snprintf(lines[n++], 128, "forward %.1f  side %.1f km/h", (double)(car->frame.forward_speed * 3.6f),
           (double)(car->frame.side_speed * 3.6f));
  const int64_t race_ms = (int64_t)w->tick * TMUF_TICK_MS - TMUF_RACE_START_MS;
  if (race->completed)
    snprintf(lines[n++], 128, "finished %u:%02u.%02u", race->finish_time / 60000u, race->finish_time / 1000u % 60u,
             race->finish_time / 10u % 100u);
  else snprintf(lines[n++], 128, "time %.2f s", (double)race_ms / 1000.0);
  snprintf(lines[n++], 128, "checkpoints %u/%u  respawns %u", race->checkpoints_passed, race->checkpoint_count,
           race->respawns);
  if (race->laps > 1) snprintf(lines[n++], 128, "lap %u/%u", race->completed_laps + (race->completed ? 0 : 1), race->laps);
  if (race->checkpoint_time_count) {
    const uint32_t i = race->checkpoint_time_count - 1, t = race->checkpoint_times[i];
    snprintf(lines[n++], 128, "split %u  %u:%02u.%02u", i + 1, t / 60000u, t / 1000u % 60u, t / 10u % 100u);
  }
  if (race->stunts_enabled) snprintf(lines[n++], 128, "stunts %d", (int)race->stunts.score);
  if (car->turbo.type) snprintf(lines[n++], 128, "turbo %.0f%%", (double)(car->turbo.progress * 100.f));
  static const char *const wheel_names[4] = {"FL", "FR", "RR", "RL"};
  char wheels[128] = "";
  for (uint32_t i = 0; i < car->wheel_count && i < 4; i++) {
    const tmuf_car_wheel *wh = &car->wheels[i];
    const uint8_t mat = wh->contact_material;
    char one[40];
    snprintf(one, sizeof one, "%s%s %s%s", i ? "  " : "", wheel_names[i],
             wh->contact ? (mat < TMUF_MAT_COUNT ? surface_names[mat] : "?") : "air", wh->contact && wh->slipping ? "*" : "");
    strncat(wheels, one, sizeof wheels - strlen(wheels) - 1);
  }
  snprintf(lines[n++], 128, "%s", wheels);
  snprintf(lines[n++], 128, "position %.2f %.2f %.2f", (double)s->pos.x, (double)s->pos.y, (double)s->pos.z);
  uint32_t written = 0;
  for (uint32_t i = 0; i < n && written < max_lines; i++)
    snprintf(out + (size_t)written++ * line_size, line_size, "%s", lines[i]);
  return written;
}

static bool player_label(ft_game *game, const ft_world *world, int32_t player, char *out, size_t out_size) {
  if (!world || !world->w.sim.race.completed) return false;
  const uint32_t ms = world->w.sim.race.finish_time;
  snprintf(out, out_size, "%u:%02u.%02u", ms / 60000u, ms / 1000u % 60u, ms % 1000u / 10u);
  return true;
}

// --- rendering and UI ----------------------------------------------------------

// For comparing pictures with the game's (tools/refshot): TM_TEST_TICK=N
// draws the car as it stands N ticks after the start with no input, the
// game's reference frames being taken well after the countdown
// (TM_TEST_CAR_POS: moved to a point).
static const ft_world *test_world(const ft_render_frame *frame) {
  static ft_world cached;
  static const ft_level *cached_level;
  static bool made;
  const char *spec = getenv("TM_TEST_TICK");
  if (!spec || !frame->world) return NULL;
  if (!made || cached_level != frame->level) {
    if (made) tmuf_world_free(&cached.w);
    cached.w = tmuf_world_empty();
    if (!tmuf_world_copy(&cached.w, &frame->world->w)) return NULL;
    const uint32_t ticks = (uint32_t)strtoul(spec, NULL, 10);
    cached.w.input = (tmuf_input){0, 0, 0, 0, 0};
    while (cached.w.tick < ticks)
      tmuf_world_tick(&cached.w);
    // (TM_TEST_CAR_POS="x y z": the car moved there, as it stands)
    const char *pos = getenv("TM_TEST_CAR_POS");
    float p[3];
    if (pos && sscanf(pos, "%f %f %f", &p[0], &p[1], &p[2]) == 3)
      cached.w.sim.body.state.pos = (tmuf_vec3){p[0], p[1], p[2]};
    made = true;
    cached_level = frame->level;
  }
  return &cached;
}

// The active group's world, track and car, drawn into game->frame and shown.
// the inverse of a column-major 4x4 matrix; false when it has none
static bool mat4_inverse(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
  if (det == 0.0) return false;
  for (int i = 0; i < 16; i++)
    out[i] = (float)(inv[i] / det);
  return true;
}

// the groups the engine shows this frame, read in its per-world entity pass
static void note_shown(ft_game *game, const ft_render_frame *frame) {
  if (frame->first_world) game->shown_next.count = 0;
  if (frame->world && frame->world_index >= 0 && game->shown_next.count < TM_GROUPS_MAX) {
    game->shown_next.group[game->shown_next.count] = frame->world_index;
    game->shown_next.opacity[game->shown_next.count++] = frame->opacity;
  }
  if (frame->last_world) game->shown = game->shown_next;
}

// the timeline group the active world is in (the first when none says so)
static uint32_t active_group(ft_game *game, const ft_render_frame *frame, const ft_world *active) {
  const ft_engine_api *api = game->engine;
  if (!api->timeline_world_pair || !api->timeline_world_count) return 0;
  const uint32_t n = api->timeline_world_count();
  for (uint32_t i = 0; i < n && i < TM_GROUPS_MAX; i++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(i, frame->state.current_tick, &a, &b) && b == active) return i;
  }
  return 0;
}

// another shown group's car as it stands at the timeline's tick: its frame
static bool other_frame(ft_game *game, const ft_render_frame *frame, uint32_t i, const ft_world *active,
                        ft_render_frame *out) {
  const ft_engine_api *api = game->engine;
  const ft_world *a = NULL, *b = NULL;
  if (!api->timeline_world_pair ||
      !api->timeline_world_pair((uint32_t)game->shown.group[i], frame->state.current_tick, &a, &b) || !b ||
      b == active)
    return false;
  *out = *frame;
  out->world = b;
  out->previous_world = a ? a : b;
  out->tick = (int32_t)b->w.tick;
  out->opacity = game->shown.opacity[i];
  out->active = false;
  return true;
}

// (TM_TEST_OTHER_POS="x y z", with TM_TEST_TICK: a second car standing
// there, as another shown group's, for pictures of the module)
static bool test_other_frame(const ft_render_frame *frame, ft_render_frame *out) {
  static ft_world cached;
  static bool made;
  const char *pos = getenv("TM_TEST_OTHER_POS");
  float p[3];
  if (!pos || !frame->world || !getenv("TM_TEST_TICK") || sscanf(pos, "%f %f %f", &p[0], &p[1], &p[2]) != 3)
    return false;
  if (made) tmuf_world_free(&cached.w);
  cached.w = tmuf_world_empty();
  if (!tmuf_world_copy(&cached.w, &frame->world->w)) return made = false;
  cached.w.sim.body.state.pos = (tmuf_vec3){p[0], p[1], p[2]};
  made = true;
  *out = *frame;
  out->world = out->previous_world = &cached;
  out->active = false;
  return true;
}

// the other shown groups' cars' shadows (the sun's, the lamps', the blob),
// each into a slot of its own, before the active car's (which then stays
// posed for the sun's shadow maps)
static void other_cars_shadows(ft_game *game, ft_level *level, const ft_render_frame *frame, const ft_world *active) {
  int slot = 1;
  for (uint32_t i = 0; i < game->shown.count && slot < TM_SHADOW_CARS; i++) {
    ft_render_frame other;
    if (!other_frame(game, frame, i, active, &other) || !tm_shadow_select(game, slot)) continue;
    tm_car_shadow(game, level->car, &other);
    tm_projector blob;
    if (tm_car_blob(game, level->car, &blob)) tm_shadow_blob(game, &blob);
    slot++;
  }
  ft_render_frame test;
  if (slot < TM_SHADOW_CARS && test_other_frame(frame, &test) && tm_shadow_select(game, slot)) {
    tm_car_shadow(game, level->car, &test);
    tm_projector blob;
    if (tm_car_blob(game, level->car, &blob)) tm_shadow_blob(game, &blob);
  }
  tm_shadow_select(game, 0);
}

// the other shown groups' cars; whether there were any
static bool render_other_cars(ft_game *game, ft_level *level, const ft_render_frame *frame, const ft_world *active) {
  bool any = false;
  for (uint32_t i = 0; i < game->shown.count; i++) {
    ft_render_frame other;
    if (!other_frame(game, frame, i, active, &other)) continue;
    tm_car_light_from_map(game, level->car, level->scene, &other);
    tm_car_render(game, level->car, &other);
    any = true;
  }
  ft_render_frame test;
  if (test_other_frame(frame, &test)) {
    tm_car_light_from_map(game, level->car, level->scene, &test);
    tm_car_render(game, level->car, &test);
    any = true;
  }
  return any;
}

static void render(ft_game *game, const ft_render_frame *frame) {
  if (frame && frame->pass == FT_PASS_ENTITIES) note_shown(game, frame);
  if (!frame || !frame->level || frame->pass != FT_PASS_LEVEL_BACKGROUND || !frame->active) return;
  const ft_world *active_world = frame->world;
  const ft_engine_api *api = game->engine;
  if (!game->gpu || !game->present || !game->quad) return;
  ft_level *level = (ft_level *)frame->level;
  tm_prof(game, "setup");
  const ft_world *test = test_world(frame);
  ft_render_frame moved = *frame;
  if (test) {
    moved.world = moved.previous_world = test;
    moved.alpha = 1.f;
  }
  // the engine's own view-projection, for its 3D drawing's depth (present.frag)
  float engine_view_proj[16];
  memcpy(engine_view_proj, moved.state.camera.view_proj, sizeof engine_view_proj);
  tm_audio_listen(game, &moved.state.camera);
  tm_camera_lens(&moved.state.camera);
  frame = &moved;
  const ft_camera *camera = &frame->state.camera;
  const uint32_t width = camera->viewport.x < 1.f ? 1u : camera->viewport.x > 8192.f ? 8192u : (uint32_t)camera->viewport.x;
  const uint32_t height = camera->viewport.y < 1.f ? 1u : camera->viewport.y > 8192.f ? 8192u : (uint32_t)camera->viewport.y;
  if (!game->frame || game->frame_width != width || game->frame_height != height) {
    release_frame(game);
    // the colour and, beside it, the depth (tg_set_depth_export)
    const ft_texture_desc desc = {.struct_size = sizeof desc, .width = tg_depth_exported(game->gpu) ? width * 2u : width,
                                  .height = height, .layers = 1, .format = FT_TEXTURE_RGBA8, .linear_filter = false};
    if (!(game->frame = api->texture_create(&desc))) return;
    game->frame_width = width;
    game->frame_height = height;
  }
  if (game->settings.draw_track && !level->scene) level->scene = tm_scene_create(game, level);
  if (game->settings.draw_car && frame->world && !level->car) level->car = tm_car_create(game, level);
  ft_gpu_image image = {.struct_size = sizeof image};
  if (!api->texture_gpu_image(game->frame, &image)) return;
  tm_prof(game, "frame_begin");
  // alpha 0: where no ground is (tm_scene_render)
  const float clear[4] = {0.f, 0.f, 0.f, 0.f};
  tg_set_samples(game->gpu, (uint32_t)game->settings.antialiasing);
  tg_set_anisotropy(game->gpu, (float)game->settings.anisotropy);
  // (the view's width: the image's is twice it with the depth beside the colour)
  if (!tg_frame_begin(game->gpu, (uint64_t)(uintptr_t)image.image, (uint32_t)image.format, width, image.height,
                      image.layout, clear)) {
    tm_log(game, FT_LOG_ERROR, "Drawing: %s", tg_error(game->gpu));
    return;
  }
  // the car's shadow first: the scenery receives it
  tm_prof(game, "car_shadows");
  tm_shadow_clear(game);
  // (not the followed car while the view is its inside: tm_camera_update)
  const bool show_car = game->settings.draw_car && !game->camera_inside;
  if (game->settings.draw_car && level->car) other_cars_shadows(game, level, frame, active_world);
  if (show_car && frame->world && level->car) {
    tm_car_light_from_map(game, level->car, level->scene, frame);
    tm_car_shadow(game, level->car, frame);
    tm_projector blob;
    if (tm_car_blob(game, level->car, &blob)) tm_shadow_blob(game, &blob);
  }
  // the wheels' marks and the cars' particles, laid and emitted up to this
  // tick: the active group's and the other shown ones'
  ft_render_frame cars[TM_SHADOW_CARS];
  uint32_t groups[TM_SHADOW_CARS], car_count = 0;
  if (frame->world && level->car) {
    cars[0] = *frame;
    groups[0] = active_group(game, frame, active_world);
    car_count = 1;
    for (uint32_t i = 0; i < game->shown.count && car_count < TM_SHADOW_CARS; i++)
      if (other_frame(game, frame, i, active_world, &cars[car_count]) && game->shown.group[i] >= 0 &&
          game->shown.group[i] < TM_GROUPS_MAX)
        groups[car_count++] = (uint32_t)game->shown.group[i];
  }
  tm_prof(game, "marks_particles_upd");
  game->marks_drawn_count = 0;
  tm_particles *particles[TM_SHADOW_CARS];
  uint32_t particle_car[TM_SHADOW_CARS], particle_count = 0;
  for (uint32_t c = 0; c < car_count; c++) {
    const uint32_t g = groups[c];
    // (the active car's LightFromMap: the cell drawn for it, read back after
    // the last frame)
    game->tint_car = c == 0 ? level->car : NULL;
    if (game->settings.marks && !level->marks[g]) level->marks[g] = tm_marks_create(game, level);
    if (game->settings.marks && level->marks[g]) {
      tm_marks_update(game, level->marks[g], &cars[c]);
      game->marks_drawn[game->marks_drawn_count].marks = level->marks[g];
      game->marks_drawn[game->marks_drawn_count++].frame = cars[c];
    }
    if (game->settings.particles && !level->particles[g]) level->particles[g] = tm_particles_create(game, level);
    if (game->settings.particles && level->particles[g]) {
      tm_particles_update(game, level->particles[g], &cars[c]);
      particle_car[particle_count] = c;
      particles[particle_count++] = level->particles[g];
    }
  }
  // the car's headlight (at night), which lights the scenery
  game->has_projector = game->settings.draw_car && frame->world && level->car && game->settings.headlights &&
                        tm_car_projector(game, level->car, &game->projector);
  tm_prof(game, "scene");
  if (game->settings.draw_track && level->scene) tm_scene_render(game, level->scene, frame);
  tm_prof(game, "cars");
  // the other groups' cars first: the active one's pose then stays for its
  // lights' flares (tm_car_lights)
  if (game->settings.draw_car && level->car && render_other_cars(game, level, frame, active_world) && show_car &&
      frame->world)
    tm_car_light_from_map(game, level->car, level->scene, frame); // (theirs drawn over it)
  if (show_car && frame->world && level->car) tm_car_render(game, level->car, frame);
  tm_prof(game, "leaves");
  // Rally's falling leaves: blended, in no order, after the scenery
  if (game->settings.draw_track && level->scene) {
    if (!level->leaves && !level->leaves_tried) {
      level->leaves = tm_leaves_create(game, level);
      level->leaves_tried = true;
    }
    tm_leaves_render(game, level->leaves, frame);
  }
  tm_prof(game, "particles");
  // after the car's glass, before the flares and the post effects (the game's last transparent draws)
  for (uint32_t c = 0; c < particle_count; c++)
    tm_particles_render(game, particles[c], &cars[particle_car[c]]);
  // the flares' tests, against the frame's depth
  tm_prof(game, "flare_tests");
  tm_flares_test(game, level, frame);
  // the samples resolved once: the flares, post effects and HUD drawn over
  // the single-sampled frame
  tm_prof(game, "resolve");
  tg_frame_resolve(game->gpu);
  tm_prof(game, "flares");
  tm_flares_render(game, level, frame, tm_scene_clouds_occlusion(level->scene));
  tm_prof(game, "post");
  tm_post_render(game, image.width, image.height);
  tm_prof(game, "hud");
  tm_hud_render(game, frame);
  // the active car's LightFromMap cell, for the coming frames' marks and particles
  tm_prof(game, "lfm_read");
  if (level->car) tm_car_lfm_read(game, level->car);
  tm_prof(game, "submit");
  if (!tg_frame_end(game->gpu)) {
    tm_log(game, FT_LOG_ERROR, "Drawing: %s", tg_error(game->gpu));
    return;
  }
  tm_prof(game, "after_frame");
  tm_flares_after_frame(game);
  tm_prof(game, "after2");
  { void tm_scene_dump_water(ft_game *, tm_scene *); if (getenv("TM_DUMP_WATER") && level->scene) tm_scene_dump_water(game, level->scene); }
  tm_bake_after_frame(game);
  // the game's lens back to the world, the engine's to its depth
  struct {
    float game_to_world[16], engine_view_proj[16];
  } present;
  if (!mat4_inverse(camera->view_proj, present.game_to_world))
    memset(present.game_to_world, 0, sizeof present.game_to_world);
  memcpy(present.engine_view_proj, engine_view_proj, sizeof present.engine_view_proj);
  api->draw_mesh(game->present, 0.f, game->quad, &game->frame, 1, &present, sizeof present);
  tm_prof_frame_end(game);
}

static bool camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  return tm_camera_update(game, frame, inout);
}

static void ui(ft_game *game, const ft_ui_frame *frame) { tm_ui(game, frame); }

static const ft_panel_desc panels[] = {
    {.window_title = TM_PANEL_PLAYER, .dock = FT_DOCK_LEFT},
};

// --- the vtable --------------------------------------------------------------

static const ft_game_module module = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,

    .info = {.struct_size = sizeof(ft_game_info),
             .id = "tmuf",
             .display_name = "TrackMania United Forever",
             .version = "2.0.0",
             .author = "Teero",
             .thumbnail = "thumbnail.png",
             .world_size = sizeof(ft_world) + 16384},

    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS | FT_CAP_TIMELINE_EVENTS | FT_CAP_EXPORTERS |
                            FT_CAP_LEVEL_FROM_MEMORY | FT_CAP_RECORDINGS | FT_CAP_HOSTS_STARTING_STATE,
                    .dimensions = FT_DIMENSIONS_3D,
                    .min_players = 1,
                    .max_players = 1,
                    .ticks_per_second = (int32_t)(1000u / TMUF_TICK_MS),
                    .units_per_tile = 32.f,
                    // the car a metre from the camera on maps kilometres wide
                    .camera_near_z = 0.1f,
                    .default_camera_height = 6.f,
                    .camera_modes = tm_camera_modes,
                    .camera_mode_count = 2,
                    .level_extension = "Gbx",
                    .level_filter_name = "TrackMania challenge or replay",
                    .recording_extension = "Gbx",
                    .recording_filter_name = "TrackMania replay"},

    .panels = panels,
    .panel_count = 1,
    .input_schema = &input_schema,
    .entity_classes = entity_classes,
    .entity_class_count = 1,

    .create = game_create,
    .destroy = game_destroy,

    .level_load_path = level_load_path,
    .level_load_memory = level_load_memory,
    .level_destroy = level_destroy,
    .level_info = level_info,
    .level_serialize = level_serialize,

    .world_create = world_create,
    .world_destroy = world_destroy,
    .world_copy = world_copy,
    .world_step = tm_world_step,
    .world_tick = world_tick,
    .world_player_count = world_player_count,
    .world_player_view = world_player_view,

    .input_default = input_default,
    .input_get = input_get,
    .input_set = input_set,
    .input_describe = input_describe,

    .entity_prop_get = entity_prop_get,
    .entity_prop_set = entity_prop_set,
    .entity_count = entity_count,

    .render = render,
    .resources_create = resources_create,
    .resources_destroy = resources_destroy,
    .ui = ui,

    .collect_events = collect_events,

    .exporter_count = tm_exporter_count,
    .exporter_desc = tm_exporter_desc,
    .export_run = tm_export_run,

    .status_lines = status_lines,
    .player_label = player_label,
    .camera_update = camera_update,

    .recording_open = tm_recording_open,
    .recording_destroy = tm_recording_destroy,
    .recording_info = tm_recording_info,
    .recording_player = tm_recording_player,
    .recording_level_matches = tm_recording_level_matches,
    .recording_tick_flags = tm_recording_tick_flags,
    .recording_input = tm_recording_input,
    .world_step_playback = tm_world_step_playback,
    .world_audio = tm_world_audio,
    .audio_spatialize = tm_audio_spatialize,
    .event_audio = tm_event_audio,
    .recording_events = tm_recording_events,

    .setting_count = setting_count,
    .setting_desc = setting_desc,
    .setting_get = setting_get,
    .setting_set = setting_set,

    .splash = tm_splash,
    .splash_destroy = tm_splash_destroy,
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  return engine_abi_version == FT_GAME_ABI_VERSION ? &module : NULL;
}
