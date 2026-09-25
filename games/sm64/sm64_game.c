// Super Mario 64 for FrameTee: the game module around libsm64_physics.
//
// The library is the whole game, bit-identical to the console: a world here is
// a console from power-on (sm64_world), stepped one frame per tick with one
// controller read. The user's ROM is the level; .m64 movies open as recordings
// and export as movies.
#include <frametee/game_abi.h>
#include <sm64/sm64_game.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cimgui.h>

#include "sm64_vulkan.h"

// --- The game ------------------------------------------------------------------

struct ft_game {
  const ft_engine_api *engine;
  // Drawing: the frame is drawn into `frame` on the engine's device and shown
  // with a quad over the viewport. Absent headless or without Vulkan.
  sm64_vulkan *vk;
  sm64_world *draw_world;
  ft_texture *frame;
  uint32_t frame_width, frame_height;
  ft_pipeline *present;
  ft_mesh *quad;
  // What the frame shows, to draw it again only when that changes.
  const ft_world *drawn_world;
  uint64_t drawn_revision, drawn_previous_revision;
  bool drawn_from_camera;
  float drawn_camera[4][4];
  bool drawn;
  bool draw_failed;
};

struct ft_level {
  char name[64];
};

static void log_error(const ft_engine_api *engine, const char *message) {
  if (engine && engine->log) engine->log(FT_LOG_ERROR, SM64_GAME_ID, message);
}

static ft_game *game_create(const ft_engine_api *engine) {
  ft_game *game = calloc(1, sizeof(*game));
  if (game) game->engine = engine;
  return game;
}

static void game_destroy(ft_game *game) {
  if (!game) return;
  sm64_world_destroy(game->draw_world);
  free(game);
}

// --- The version ------------------------------------------------------------------
// One module per game version, with the library of that version.

struct version {
  const char *id;      // sm64_version()
  const char *code;    // the ROM's game code
  uint8_t revision;    // and its revision
  char country;        // its country code, also in .m64 headers
  const char *region;  // as ROM files are named
  const char *adjective;
  // TASVideos movies (Mupen64-rr) against the console the library follows
  // (mupen64plus); see movie_offset. EU has no movie that fixed it yet.
  int32_t movie_offset;
  uint8_t vi_rate;     // vertical interrupts per second: NTSC or PAL
};

static const struct version kVersions[] = {
    {"jp", "NSMJ", 0, 'J', "Japan", "Japanese", -16, 60},
    {"us", "NSME", 0, 'E', "USA", "American", -55, 60},
    {"eu", "NSMP", 0, 'P', "Europe", "European", 0, 50},
    {"sh", "NSMJ", 3, 'J', "Japan (Rev A) (Shindou Edition)", "Shindou Edition", 0, 60},
};

static const struct version *version(void) {
  for (size_t i = 0; i < sizeof(kVersions) / sizeof(kVersions[0]); ++i)
    if (strcmp(kVersions[i].id, sm64_version()) == 0) return &kVersions[i];
  return &kVersions[1];
}

// --- The ROM: the level ----------------------------------------------------------

// The ROM's header, big-endian: its internal name, its game code (NSMJ, NSME
// or NSMP) and its revision (the Shindou Edition is JP's 3).
static bool rom_header(const uint8_t *rom, size_t size, char name[21], char code[5], uint8_t *revision) {
  if (size < 0x40) return false;
  uint8_t header[0x40];
  // .z64 is big-endian, .v64 swaps each 16-bit word, .n64 each 32-bit one.
  int swap = rom[0] == 0x80 && rom[1] == 0x37 ? 0 : rom[0] == 0x37 && rom[1] == 0x80 ? 1 : rom[3] == 0x80 ? 3 : -1;
  if (swap < 0) return false;
  for (size_t i = 0; i < sizeof(header); ++i) header[i] = rom[swap == 1 ? i ^ 1 : swap == 3 ? i ^ 3 : i];
  memcpy(name, header + 0x20, 20);
  name[20] = '\0';
  for (int i = 19; i >= 0 && name[i] == ' '; --i) name[i] = '\0';
  memcpy(code, header + 0x3B, 4);
  code[4] = '\0';
  *revision = header[0x3F];
  return true;
}

// The ROM this library is built for.
static bool wanted_rom(const char code[5], uint8_t revision) {
  return strcmp(code, version()->code) == 0 && revision == version()->revision;
}

static ft_level *level_load_path(ft_game *game, const char *path, const char *variant_id) {
  (void)variant_id;
  void *data = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &data, &size)) {
    log_error(game->engine, "cannot read the ROM");
    return NULL;
  }
  ft_level *level = NULL;
  char name[21], code[5];
  uint8_t revision = 0;
  if (!rom_header(data, size, name, code, &revision) || !wanted_rom(code, revision)) {
    char message[256];
    snprintf(message, sizeof(message), "%s is not a Super Mario 64 (%s) ROM", path, version()->region);
    log_error(game->engine, message);
  } else if (!sm64_load_rom(data, size)) {
    log_error(game->engine, "the ROM is not the version this build of the game is for");
  } else if ((level = calloc(1, sizeof(*level)))) {
    snprintf(level->name, sizeof(level->name), "%s", SM64_GAME_NAME);
  }
  game->engine->free_file_data(data);
  return level;
}

static void level_destroy(ft_game *game, ft_level *level) {
  (void)game;
  free(level);
}

static bool level_info(ft_game *game, const ft_level *level, ft_level_info *out) {
  (void)game;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  out->name = level->name;
  // Courses span about 16384 units on each side.
  out->bounds = (ft_rect){-8192.f, -8192.f, 16384.f, 16384.f};
  return true;
}

// --- Worlds ------------------------------------------------------------------------

static uint64_t next_revision(void) {
  static uint64_t revision;
  return __atomic_add_fetch(&revision, 1, __ATOMIC_RELAXED);
}

static ft_world *world_create(ft_game *game, const ft_world_desc *desc) {
  (void)desc;
  ft_world *world = calloc(1, sizeof(*world));
  if (!world) return NULL;
  if (!(world->sim = sm64_world_create())) {
    log_error(game->engine, "out of memory for a world");
    free(world);
    return NULL;
  }
  world->revision = next_revision();
  return world;
}

static void world_destroy(ft_game *game, ft_world *world) {
  (void)game;
  if (!world) return;
  sm64_world_destroy(world->sim);
  free(world);
}

static void world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  (void)game;
  sm64_world_copy(dst->sim, src->sim);
  dst->tick = src->tick;
  dst->input = src->input;
  dst->revision = src->revision;
}

static void world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  (void)game;
  world->input = player_count > 0 && inputs ? sm64_game_input(inputs) : 0;
  sm64_step(world->sim, world->input);
  ++world->tick;
  world->revision = next_revision();
}

static int32_t world_tick(ft_game *game, const ft_world *world) {
  (void)game;
  return world->tick;
}

static int32_t world_player_count(ft_game *game, const ft_world *world) {
  (void)game;
  (void)world;
  return 1;
}

static bool world_player_view(ft_game *game, const ft_world *world, int32_t player, ft_player_view *out) {
  (void)game;
  if (player != 0) return false;
  struct sm64_mario_info mario;
  const bool spawned = sm64_mario(world->sim, &mario);
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  // The ground plane: x and z.
  out->position = (ft_vec2){mario.pos[0], mario.pos[2]};
  out->velocity = (ft_vec2){mario.vel[0], mario.vel[2]};
  out->flags = spawned ? FT_PLAYER_ALIVE : FT_PLAYER_DISABLED;
  out->run_start_tick = 0;
  return true;
}

// --- Inputs --------------------------------------------------------------------------
// A record is one controller read in the .m64 sample layout: buttons in bits
// 0-15, stick X in bits 16-23, stick Y in bits 24-31 (both signed).

enum {
  FIELD_STICK_X,
  FIELD_STICK_Y,
  FIELD_A,
  FIELD_B,
  FIELD_Z,
  FIELD_START,
  FIELD_L,
  FIELD_R,
  FIELD_C_UP,
  FIELD_C_DOWN,
  FIELD_C_LEFT,
  FIELD_C_RIGHT,
  FIELD_D_UP,
  FIELD_D_DOWN,
  FIELD_D_LEFT,
  FIELD_D_RIGHT,
  FIELD_COUNT
};

// The .m64 bit of each button field.
static const uint32_t kButtonBits[FIELD_COUNT] = {
    [FIELD_A] = 0x0080,      [FIELD_B] = 0x0040,      [FIELD_Z] = 0x0020,      [FIELD_START] = 0x0010,
    [FIELD_L] = 0x2000,      [FIELD_R] = 0x1000,      [FIELD_C_UP] = 0x0800,   [FIELD_C_DOWN] = 0x0400,
    [FIELD_C_LEFT] = 0x0200, [FIELD_C_RIGHT] = 0x0100, [FIELD_D_UP] = 0x0008,   [FIELD_D_DOWN] = 0x0004,
    [FIELD_D_LEFT] = 0x0002, [FIELD_D_RIGHT] = 0x0001,
};

#define STICK(id, name, flags)                                                                                         \
  {id, name, "Raw stick value", FT_INPUT_INT, FT_INPUT_FLAG_TIMELINE_LANE | (flags), -128, 127, 0, 0, 0, 0,             \
   NULL, 0, {0.30f, 0.70f, 1.f, 1.f}}
#define BUTTON(id, name)                                                                                               \
  {id, name, NULL, FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0, 0, 0, NULL, 0, {1.f, 0.70f, 0.30f, 1.f}}
static const ft_input_field kFields[FIELD_COUNT] = {
    STICK("stick_x", "Stick X", FT_INPUT_FLAG_MIRROR_X), STICK("stick_y", "Stick Y", 0),
    BUTTON("a", "A"),         BUTTON("b", "B"),           BUTTON("z", "Z"),
    BUTTON("start", "Start"), BUTTON("l", "L"),           BUTTON("r", "R"),
    BUTTON("c_up", "C Up"),   BUTTON("c_down", "C Down"), BUTTON("c_left", "C Left"),
    BUTTON("c_right", "C Right"), BUTTON("d_up", "D-Pad Up"), BUTTON("d_down", "D-Pad Down"),
    BUTTON("d_left", "D-Pad Left"), BUTTON("d_right", "D-Pad Right"),
};
#undef STICK
#undef BUTTON

static const ft_input_control kControls[] = {
    {"left", "Stick left", NULL, "Stick", "A", FIELD_STICK_X, -80, FT_CONTROL_ADD, NULL},
    {"right", "Stick right", NULL, "Stick", "D", FIELD_STICK_X, 80, FT_CONTROL_ADD, NULL},
    {"up", "Stick up", NULL, "Stick", "W", FIELD_STICK_Y, 80, FT_CONTROL_ADD, NULL},
    {"down", "Stick down", NULL, "Stick", "S", FIELD_STICK_Y, -80, FT_CONTROL_ADD, NULL},
    {"a", "A", "Jump", "Buttons", "Space", FIELD_A, 1, 0, NULL},
    {"b", "B", "Punch, dive", "Buttons", "F", FIELD_B, 1, 0, NULL},
    {"z", "Z", "Crouch, ground pound", "Buttons", "LeftShift", FIELD_Z, 1, 0, NULL},
    {"start", "Start", NULL, "Buttons", "Enter", FIELD_START, 1, 0, NULL},
    {"l", "L", NULL, "Buttons", "Q", FIELD_L, 1, 0, NULL},
    {"r", "R", NULL, "Camera", "E", FIELD_R, 1, 0, NULL},
    {"c_up", "C Up", NULL, "Camera", "I", FIELD_C_UP, 1, 0, NULL},
    {"c_down", "C Down", NULL, "Camera", "K", FIELD_C_DOWN, 1, 0, NULL},
    {"c_left", "C Left", NULL, "Camera", "J", FIELD_C_LEFT, 1, 0, NULL},
    {"c_right", "C Right", NULL, "Camera", "L", FIELD_C_RIGHT, 1, 0, NULL},
};

static const ft_input_schema kInputSchema = {
    sizeof(ft_input_schema), sizeof(uint32_t), sizeof(uint32_t), kFields, FIELD_COUNT, kControls,
    sizeof(kControls) / sizeof(kControls[0]),
};

static void input_default(ft_game *game, void *record) {
  (void)game;
  memset(record, 0, sizeof(uint32_t));
}

static int64_t input_get(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const uint32_t value = sm64_game_input(record);
  if (field == FIELD_STICK_X) return (int8_t)(value >> 16);
  if (field == FIELD_STICK_Y) return (int8_t)(value >> 24);
  return field < FIELD_COUNT && (value & kButtonBits[field]) != 0;
}

static void input_set(ft_game *game, void *record, uint32_t field, int64_t value) {
  (void)game;
  uint32_t input = sm64_game_input(record);
  if (field == FIELD_STICK_X || field == FIELD_STICK_Y) {
    const int shift = field == FIELD_STICK_X ? 16 : 24;
    const int64_t clamped = value < -128 ? -128 : value > 127 ? 127 : value;
    input = (input & ~(0xFFu << shift)) | ((uint32_t)(uint8_t)(int8_t)clamped << shift);
  } else if (field < FIELD_COUNT) {
    input = value ? input | kButtonBits[field] : input & ~kButtonBits[field];
  }
  memcpy(record, &input, sizeof(input));
}

static void input_describe(ft_game *game, const void *record, char *out, size_t out_size) {
  (void)game;
  const uint32_t value = sm64_game_input(record);
  int n = snprintf(out, out_size, "stick %d %d", (int8_t)(value >> 16), (int8_t)(value >> 24));
  for (uint32_t f = FIELD_A; f < FIELD_COUNT && n > 0 && (size_t)n < out_size; ++f)
    if (value & kButtonBits[f]) n += snprintf(out + n, out_size - n, " %s", kFields[f].display_name);
}

// --- Mario: properties and status --------------------------------------------------

enum { PROP_POSITION, PROP_VELOCITY, PROP_SPEED, PROP_ACTION, PROP_HEALTH, PROP_STARS, PROP_COINS, PROP_COUNT };

static const ft_prop_desc kProps[PROP_COUNT] = {
    {"position", "Position", "Mario", "units", FT_VALUE_VEC3, FT_PROP_SUMMARY, 0, 0},
    {"velocity", "Velocity", "Mario", "units/frame", FT_VALUE_VEC3, 0, 0, 0},
    {"forward_speed", "Forward speed", "Mario", "units/frame", FT_VALUE_FLOAT, FT_PROP_SUMMARY, 0, 0},
    {"action", "Action", "Mario", NULL, FT_VALUE_INT, FT_PROP_SUMMARY, 0, 0},
    {"health", "Health", "Mario", NULL, FT_VALUE_INT, 0, 0, 0},
    {"stars", "Stars", "Mario", NULL, FT_VALUE_INT, 0, 0, 0},
    {"coins", "Coins", "Mario", NULL, FT_VALUE_INT, 0, 0, 0},
};

static const ft_entity_class kEntityClasses[] = {{"mario", "Mario", kProps, PROP_COUNT}};

static int32_t entity_count(ft_game *game, const ft_world *world, uint32_t entity_class) {
  (void)game;
  (void)world;
  return entity_class == FT_ENTITY_CLASS_PLAYER ? 1 : 0;
}

static bool entity_prop_get(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                            ft_value *out) {
  (void)game;
  if (entity_class != FT_ENTITY_CLASS_PLAYER || entity != 0 || prop >= PROP_COUNT) return false;
  struct sm64_mario_info m;
  if (!sm64_mario(world->sim, &m)) return false;
  switch (prop) {
  case PROP_POSITION: *out = (ft_value){.kind = FT_VALUE_VEC3, .as.v3 = {m.pos[0], m.pos[1], m.pos[2]}}; break;
  case PROP_VELOCITY: *out = (ft_value){.kind = FT_VALUE_VEC3, .as.v3 = {m.vel[0], m.vel[1], m.vel[2]}}; break;
  case PROP_SPEED: *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = m.forward_vel}; break;
  case PROP_ACTION: *out = (ft_value){.kind = FT_VALUE_INT, .as.i = m.action}; break;
  case PROP_HEALTH: *out = (ft_value){.kind = FT_VALUE_INT, .as.i = m.health}; break;
  case PROP_STARS: *out = (ft_value){.kind = FT_VALUE_INT, .as.i = m.num_stars}; break;
  case PROP_COINS: *out = (ft_value){.kind = FT_VALUE_INT, .as.i = m.num_coins}; break;
  }
  return true;
}

static bool entity_prop_set(ft_game *game, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                            const ft_value *value) {
  // The game's state only changes through inputs: a run stays one the console
  // can play.
  (void)game, (void)world, (void)entity_class, (void)entity, (void)prop, (void)value;
  return false;
}

static uint32_t status_lines(ft_game *game, const ft_world *world, int32_t player, float alpha, char *out,
                             uint32_t max_lines, uint32_t line_size) {
  (void)game, (void)player, (void)alpha;
  struct sm64_mario_info m;
  const bool spawned = sm64_mario(world->sim, &m);
  uint32_t n = 0;
#define LINE(...)                                                                                                      \
  do {                                                                                                                 \
    if (n < max_lines) snprintf(out + (size_t)n++ * line_size, line_size, __VA_ARGS__);                                \
  } while (0)
  LINE("frame %d   level %d area %d", world->tick, m.level, m.area);
  if (spawned) {
    LINE("x %.3f  y %.3f  z %.3f", m.pos[0], m.pos[1], m.pos[2]);
    LINE("speed %.3f  vy %.3f  yaw %d", m.forward_vel, m.vel[1], (uint16_t)m.face_angle[1]);
    LINE("action %08X  state %u  timer %u", m.action, m.action_state, m.action_timer);
    LINE("health %d  stars %d  coins %d", m.health >> 8, m.num_stars, m.num_coins);
  }
#undef LINE
  return n;
}

// --- The camera ----------------------------------------------------------------------

static const ft_camera_mode kCameraModes[] = {
    {"game", "Game camera", "Lakitu, as the game places him", FT_CAMERA_MODE_DIRECTED},
};

static bool camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  (void)game;
  if (frame->mode != 0 || !frame->world) return false;
  struct sm64_camera_info c;
  sm64_camera(frame->world->sim, &c);
  inout->eye = (ft_vec3){c.pos[0], c.pos[1], c.pos[2]};
  inout->target = (ft_vec3){c.focus[0], c.focus[1], c.focus[2]};
  // Roll turns the up vector about the view direction.
  const float roll = c.roll * (float)(M_PI / 32768.0);
  float fx = c.focus[0] - c.pos[0], fy = c.focus[1] - c.pos[1], fz = c.focus[2] - c.pos[2];
  const float length = sqrtf(fx * fx + fy * fy + fz * fz);
  if (length > 0.f) fx /= length, fy /= length, fz /= length;
  // Up without roll: world up made perpendicular to the view direction.
  float ux = -fx * fy, uy = 1.f - fy * fy, uz = -fz * fy;
  const float ulength = sqrtf(ux * ux + uy * uy + uz * uz);
  if (ulength > 0.f) ux /= ulength, uy /= ulength, uz /= ulength;
  // Rodrigues about the view direction.
  const float rx = fy * uz - fz * uy, ry = fz * ux - fx * uz, rz = fx * uy - fy * ux;
  inout->up = (ft_vec3){ux * cosf(roll) + rx * sinf(roll), uy * cosf(roll) + ry * sinf(roll),
                        uz * cosf(roll) + rz * sinf(roll)};
  inout->fov_y = c.fov * (float)(M_PI / 180.0);
  inout->near_z = 100.f;
  inout->far_z = 20000.f;
  inout->orthographic = false;
  return true;
}

// --- Drawing -------------------------------------------------------------------------
// The game draws its own frames, as on the console: to show tick N, a copy of
// the world at N - 1 steps again with drawing (sm64_step_draw), and the display
// list it hands over is drawn by f3d/ (our Fast3D interpreter) with
// Vulkan on the engine's device, into a texture shown over the viewport. The
// 3D takes the viewport's aspect ratio; the HUD stays 4:3 in the middle.

static void release_frame(ft_game *game) {
  if (game->frame) game->engine->texture_destroy(game->frame);
  game->frame = NULL;
  game->frame_width = game->frame_height = 0;
  game->drawn = false;
}

static bool resources_create(ft_game *game) {
  const ft_engine_api *api = game->engine;
  const ft_gpu_device *gpu = api->gpu_device ? api->gpu_device() : NULL;
  if (!gpu || gpu->api != FT_GPU_API_VULKAN) return true;
  char path[4096], error[256];
  void *vert = NULL, *frag = NULL;
  size_t vert_size = 0, frag_size = 0;
  api->resolve_data_path("shaders/present.vert.spv", path, sizeof(path));
  const bool have_vert = api->read_file(path, &vert, &vert_size);
  api->resolve_data_path("shaders/present.frag.spv", path, sizeof(path));
  const bool have_frag = api->read_file(path, &frag, &frag_size);
  if (have_vert && have_frag) {
    ft_pipeline_desc desc = {.struct_size = sizeof(desc),
                             .vertex_spirv = vert,
                             .vertex_spirv_size = vert_size,
                             .fragment_spirv = frag,
                             .fragment_spirv_size = frag_size,
                             .texture_count = 1};
    game->present = api->pipeline_create(&desc);
  }
  if (have_vert) api->free_file_data(vert);
  if (have_frag) api->free_file_data(frag);
  // A quad over the whole viewport, in clip space (shaders/present.vert).
  const ft_vertex quad[4] = {{{-1, -1}, {1, 1, 1}, {0, 0}},
                             {{1, -1}, {1, 1, 1}, {1, 0}},
                             {{1, 1}, {1, 1, 1}, {1, 1}},
                             {{-1, 1}, {1, 1, 1}, {0, 1}}};
  const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
  game->quad = api->mesh_create(quad, 4, sizeof(ft_vertex), indices, 6);
  game->vk = sm64_vulkan_create((VkPhysicalDevice)gpu->physical_device, (VkDevice)gpu->device, (VkQueue)gpu->queue,
                                gpu->queue_family_index, error, sizeof(error));
  if (!game->vk) {
    char message[320];
    snprintf(message, sizeof(message), "cannot draw the game: %s", error);
    log_error(api, message);
  }
  if (!game->present || !game->quad) log_error(api, "cannot show the game's frames");
  return true;
}

static void resources_destroy(ft_game *game) {
  const ft_engine_api *api = game->engine;
  release_frame(game);
  sm64_vulkan_destroy(game->vk);
  game->vk = NULL;
  if (game->present) api->pipeline_destroy(game->present);
  if (game->quad) api->mesh_destroy(game->quad);
  game->present = NULL;
  game->quad = NULL;
}

// The engine's view-projection (column vectors, Vulkan's clip space with Y
// down, reversed depth from 1 at the near plane to 0 at the far one) as the
// game's projections are: row vectors, OpenGL's clip space. X is narrowed
// back to 4:3, which f3d.c widens to the viewport again.
static void game_camera_from_engine(const ft_camera *camera, float out[4][4]) {
  const float aspect = camera->viewport.y > 0.f ? camera->viewport.x / camera->viewport.y : 4.f / 3.f;
  const float *m = camera->view_proj; // row r, column c at m[c * 4 + r]
  for (int c = 0; c < 4; ++c) {
    out[c][0] = m[c * 4 + 0] * aspect * 0.75f;
    out[c][1] = -m[c * 4 + 1];
    out[c][2] = m[c * 4 + 3] - 2.f * m[c * 4 + 2];
    out[c][3] = m[c * 4 + 3];
  }
}

// Draws the frame of `world` into game->frame, if it changed. With a camera,
// through it rather than the game's.
static bool draw_frame(ft_game *game, const ft_world *world, const ft_world *previous, const float (*camera)[4],
                       uint32_t width, uint32_t height) {
  const ft_engine_api *api = game->engine;
  if (!game->frame || game->frame_width != width || game->frame_height != height) {
    release_frame(game);
    ft_texture_desc desc = {.struct_size = sizeof(desc), .width = width, .height = height, .layers = 1,
                            .format = FT_TEXTURE_RGBA8, .linear_filter = true};
    if (!(game->frame = api->texture_create(&desc))) return false;
    game->frame_width = width;
    game->frame_height = height;
  }
  if (game->drawn && game->drawn_world == world && game->drawn_revision == world->revision &&
      game->drawn_previous_revision == previous->revision && game->drawn_from_camera == (camera != NULL) &&
      (!camera || memcmp(game->drawn_camera, camera, sizeof(game->drawn_camera)) == 0))
    return true;
  if (!game->draw_world && !(game->draw_world = sm64_world_create())) return false;
  sm64_world_copy(game->draw_world, previous->sim);
  const void *list = sm64_step_draw(game->draw_world, world->input);
  if (!list) return game->drawn;
  ft_gpu_image image = {.struct_size = sizeof(image)};
  char error[256];
  if (!api->texture_gpu_image || !api->texture_gpu_image(game->frame, &image) ||
      !sm64_vulkan_draw(game->vk, list, camera, (VkImage)image.image, (VkFormat)image.format, image.width, image.height,
                        (VkImageLayout)image.layout, error, sizeof(error))) {
    if (!game->draw_failed) {
      char message[320];
      snprintf(message, sizeof(message), "drawing the game failed: %s", error);
      log_error(api, message);
    }
    game->draw_failed = true;
    return false;
  }
  game->draw_failed = false;
  game->drawn = true;
  game->drawn_world = world;
  game->drawn_revision = world->revision;
  game->drawn_previous_revision = previous->revision;
  game->drawn_from_camera = camera != NULL;
  if (camera) memcpy(game->drawn_camera, camera, sizeof(game->drawn_camera));
  return true;
}

static void render(ft_game *game, const ft_render_frame *frame) {
  const ft_engine_api *api = game->engine;
  if (frame->pass != FT_PASS_LEVEL_BACKGROUND || !frame->active || !frame->world || !game->vk || !game->present ||
      !game->quad)
    return;
  // Frame N is the step from N - 1 drawn again; power-on has drawn nothing.
  const ft_world *previous = frame->previous_world;
  if (!previous || previous->tick != frame->world->tick - 1) return;
  ft_camera camera = {0};
  api->camera_get(&camera);
  const uint32_t width = camera.viewport.x < 1.f ? 1u : camera.viewport.x > 4096.f ? 4096u : (uint32_t)camera.viewport.x;
  const uint32_t height = camera.viewport.y < 1.f ? 1u : camera.viewport.y > 4096.f ? 4096u : (uint32_t)camera.viewport.y;
  // The game's own camera shows the frame as the console draws it; the
  // engine's (freecam, top-down) show its 3D scene from where they are.
  float view[4][4];
  const bool own_camera = camera.mode >= sizeof(kCameraModes) / sizeof(kCameraModes[0]);
  if (own_camera) game_camera_from_engine(&camera, view);
  if (draw_frame(game, frame->world, previous, own_camera ? (const float (*)[4])view : NULL, width, height))
    api->draw_mesh(game->present, 0.f, game->quad, &game->frame, 1, NULL, 0);
}

// --- .m64 movies ---------------------------------------------------------------------
// An .m64 holds one controller read per poll, from power-on: the console's
// reads while it boots (sm64_boot_polls), then one per game frame. The movies
// on TASVideos were made on Mupen64-rr, whose boot polls the controller a
// different number of times than the console this library follows
// (mupen64plus): tick t reads sample t + sm64_boot_polls() + movie_offset.
// (The Shindou Edition also reads the controller to look for a Rumble Pak
// every 60 vertical interrupts without one; those samples are not skipped.)

static int32_t movie_offset(void) { return version()->movie_offset; }

static int32_t movie_tick(int32_t sample) { return sample - sm64_boot_polls() - movie_offset(); }

struct ft_recording {
  uint32_t *samples;
  uint32_t count;
  char name[256];
  char code; // the ROM's country code: J, E or P
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static ft_recording *recording_open(ft_game *game, const void *data, size_t size, const char *name,
                                    bool (*progress)(void *user, float fraction), void *progress_user, char *error,
                                    size_t error_size) {
  (void)game, (void)progress, (void)progress_user;
  const uint8_t *bytes = data;
  if (size < 0x400 || memcmp(bytes, "M64\x1a", 4) != 0) {
    snprintf(error, error_size, "not an .m64 movie");
    return NULL;
  }
  if (le32(bytes + 4) != 3) {
    snprintf(error, error_size, ".m64 version %u is not supported", le32(bytes + 4));
    return NULL;
  }
  const uint32_t samples = le32(bytes + 0x18);
  if (samples > (size - 0x400) / 4) {
    snprintf(error, error_size, "the movie lists %u inputs but holds %zu", samples, (size - 0x400) / 4);
    return NULL;
  }
  ft_recording *recording = calloc(1, sizeof(*recording));
  if (!recording || !(recording->samples = malloc((samples ? samples : 1) * sizeof(uint32_t)))) {
    free(recording);
    snprintf(error, error_size, "out of memory");
    return NULL;
  }
  for (uint32_t i = 0; i < samples; ++i) recording->samples[i] = le32(bytes + 0x400 + 4 * i);
  recording->count = samples;
  recording->code = (char)bytes[0xE8];
  snprintf(recording->name, sizeof(recording->name), "%s", name ? name : "movie");
  return recording;
}

static void recording_destroy(ft_game *game, ft_recording *recording) {
  (void)game;
  if (recording) free(recording->samples);
  free(recording);
}

static bool recording_info(ft_game *game, const ft_recording *recording, ft_recording_info *out) {
  (void)game;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  out->name = recording->name;
  out->first_tick = movie_tick(0) < 0 ? 0 : movie_tick(0);
  out->last_tick = movie_tick((int32_t)recording->count - 1);
  out->player_count = 1;
  return true;
}

static bool recording_player(ft_game *game, const ft_recording *recording, uint32_t index, ft_recording_player *out) {
  (void)game;
  if (index != 0) return false;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  out->name = "Controller 1";
  out->first_tick = movie_tick(0) < 0 ? 0 : movie_tick(0);
  out->last_tick = movie_tick((int32_t)recording->count - 1);
  out->suggested = true;
  return true;
}

static bool recording_level_matches(ft_game *game, const ft_recording *recording, const ft_level *level) {
  (void)game, (void)level;
  return recording->code == version()->country;
}

// The input the movie holds for a tick: nothing before its first sample.
static uint32_t movie_input(const ft_recording *recording, int32_t tick) {
  const int64_t sample = (int64_t)tick + sm64_boot_polls() + movie_offset();
  return sample >= 0 && sample < recording->count ? recording->samples[sample] : 0;
}

static void recording_tick_flags(ft_game *game, const ft_recording *recording, int32_t player, int32_t first_tick,
                                 uint32_t count, uint8_t *out) {
  (void)game, (void)player;
  for (uint32_t i = 0; i < count; ++i) {
    const int64_t sample = (int64_t)first_tick + i + 1 + movie_offset();
    out[i] = sample < (int64_t)recording->count ? FT_RECORDING_TICK_PRESENT : 0;
  }
}

static bool recording_input(ft_game *game, const ft_recording *recording, int32_t player, int32_t tick,
                            void *out_record) {
  (void)game;
  if (player != 0) return false;
  const uint32_t input = movie_input(recording, tick);
  memcpy(out_record, &input, sizeof(input));
  return true;
}

static void world_step_playback(ft_game *game, ft_world *world, const void *inputs, const ft_player_playback *playback,
                                uint32_t player_count) {
  // Playing a movie back is stepping the game with its inputs: the step that
  // reaches `tick` reads the tick before.
  if (player_count > 0 && playback[0].recording) {
    const uint32_t input = movie_input(playback[0].recording, playback[0].tick - 1);
    world_step(game, world, &input, 1);
  } else {
    world_step(game, world, inputs, player_count);
  }
}

// Export: an .m64 from power-on, for Mupen64-rr.
static const ft_exporter_desc kExporter = {"m64", "M64 movie", "m64", "Mupen64 movie"};

static uint32_t exporter_count(ft_game *game) {
  (void)game;
  return 1;
}

static const ft_exporter_desc *exporter_desc(ft_game *game, uint32_t index) {
  (void)game;
  return index == 0 ? &kExporter : NULL;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static bool export_run(ft_game *game, uint32_t index, const ft_export_request *request) {
  const ft_engine_api *api = game->engine;
  if (index != 0 || request->end_tick < 0) return false;
  // Sample 0 is read at movie_tick(0): the ticks before cannot be in a movie.
  const int32_t first = movie_tick(0);
  for (int32_t tick = 0; tick < first && tick <= request->end_tick; ++tick) {
    uint32_t input = 0;
    if (api->get_player_input(0, tick, &input) && input != 0) {
      log_error(api, "inputs before the movie's first sample are left out of the .m64");
      break;
    }
  }
  const uint32_t samples = request->end_tick >= first ? (uint32_t)(request->end_tick - first + 1) : 0;
  uint8_t *file = calloc(1, 0x400 + (size_t)samples * 4);
  if (!file) return false;
  memcpy(file, "M64\x1a", 4);
  put32(file + 0x04, 3);
  put32(file + 0x0C, samples * 2); // vertical interrupts: two per game frame
  file[0x14] = version()->vi_rate;
  file[0x15] = 1;
  put32(file + 0x18, samples);
  file[0x1C] = 2; // from power-on
  put32(file + 0x20, 1); // controller 1 present
  memcpy(file + 0xC4, "SUPER MARIO 64", 14);
  file[0xE8] = version()->country;
  memcpy(file + 0x222, "FrameTee", 8);
  for (uint32_t i = 0; i < samples; ++i) {
    uint32_t input = 0; // the boot's reads, before tick 0, see nothing pressed
    if (first + (int32_t)i >= 0) api->get_player_input(0, first + (int32_t)i, &input);
    put32(file + 0x400 + 4 * i, input);
    if (request->progress && i % 4096 == 0) request->progress(request->progress_user, (float)i / samples, NULL);
  }
  FILE *out = fopen(request->path, "wb");
  const bool ok = out && fwrite(file, 1, 0x400 + (size_t)samples * 4, out) == 0x400 + (size_t)samples * 4;
  if (out) fclose(out);
  free(file);
  if (!ok) log_error(api, "cannot write the .m64");
  return ok;
}

// --- The start screen: pick a ROM -----------------------------------------------------

struct rom_list {
  const ft_engine_api *engine;
  char directory[1024];
  char paths[16][1024];
  char names[16][64];
  int count;
};

static bool visit_rom(void *user, const ft_directory_entry *entry) {
  struct rom_list *list = user;
  const char *dot = strrchr(entry->name, '.');
  if (entry->is_directory || !dot || (strcmp(dot, ".z64") && strcmp(dot, ".n64") && strcmp(dot, ".v64"))) return true;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", list->directory, entry->name);
  void *data = NULL;
  size_t size = 0;
  if (!list->engine->read_file(path, &data, &size)) return true;
  char name[21], code[5];
  uint8_t revision = 0;
  if (rom_header(data, size, name, code, &revision) && wanted_rom(code, revision) && list->count < 16) {
    snprintf(list->paths[list->count], sizeof(list->paths[0]), "%s", path);
    snprintf(list->names[list->count], sizeof(list->names[0]), "%s", entry->name);
    ++list->count;
  }
  list->engine->free_file_data(data);
  return list->count < 16;
}

static void splash(const ft_engine_api *engine, void **context, const ft_ui_frame *frame) {
  if (!engine || !frame || frame->slot != FT_UI_SPLASH) return;
  struct rom_list *list = *context;
  if (!list && (list = calloc(1, sizeof(*list)))) {
    *context = list;
    list->engine = engine;
    // Every version's ROMs are in the US module's directory, data/games/sm64.
    engine->resolve_data_path("", list->directory, sizeof(list->directory));
    size_t length = strlen(list->directory);
    while (length > 0 && (list->directory[length - 1] == '/' || list->directory[length - 1] == '\\'))
      list->directory[--length] = '\0';
    char *own = strrchr(list->directory, '/');
    if (!own) own = strrchr(list->directory, '\\');
    if (own && (size_t)(own - list->directory) + 6 < sizeof(list->directory)) strcpy(own + 1, "sm64");
    engine->visit_directory(list->directory, visit_rom, list);
  }
  if (!list) return;
  igTextUnformatted("Super Mario 64", NULL);
  igSpacing();
  igText("The whole game from power-on, frame for frame as on the console (%s version).", version()->adjective);
  igSpacing();
  igSeparator();
  igSpacing();
  if (list->count == 0) {
    igTextWrapped("Put your Super Mario 64 ROM (%s) into %s.", version()->region, list->directory);
    return;
  }
  for (int i = 0; i < list->count; ++i)
    if (igButton(list->names[i], (ImVec2){0.f, 40.f}) && engine->request_level) engine->request_level(list->paths[i]);
}

static void splash_destroy(void *context) { free(context); }

// --- The module ----------------------------------------------------------------------

static const ft_game_module kModule = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,
    .info = {.struct_size = sizeof(ft_game_info),
             .id = SM64_GAME_ID,
             .display_name = SM64_GAME_NAME,
             .version = "0.2.0",
             .author = "Teero",
             .thumbnail = "thumbnail.png"},
    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_HEADLESS | FT_CAP_EXPORTERS | FT_CAP_RECORDINGS | FT_CAP_RENDERS_LEVEL,
                    .dimensions = FT_DIMENSIONS_3D,
                    .min_players = 1,
                    .max_players = 1,
                    .ticks_per_second = SM64_TICKS_PER_SECOND,
                    .units_per_tile = 100.f,
                    .default_camera_height = 20.f,
                    .camera_modes = kCameraModes,
                    .camera_mode_count = sizeof(kCameraModes) / sizeof(kCameraModes[0]),
                    .level_extension = "z64",
                    .level_filter_name = "Super Mario 64 ROM",
                    .recording_extension = "m64",
                    .recording_filter_name = "Mupen64 movie"},
    .input_schema = &kInputSchema,
    .entity_classes = kEntityClasses,
    .entity_class_count = 1,
    .create = game_create,
    .destroy = game_destroy,
    .level_load_path = level_load_path,
    .level_destroy = level_destroy,
    .level_info = level_info,
    .world_create = world_create,
    .world_destroy = world_destroy,
    .world_copy = world_copy,
    .world_step = world_step,
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
    .exporter_count = exporter_count,
    .exporter_desc = exporter_desc,
    .export_run = export_run,
    .status_lines = status_lines,
    .camera_update = camera_update,
    .splash = splash,
    .splash_destroy = splash_destroy,
    .recording_open = recording_open,
    .recording_destroy = recording_destroy,
    .recording_info = recording_info,
    .recording_player = recording_player,
    .recording_level_matches = recording_level_matches,
    .recording_tick_flags = recording_tick_flags,
    .recording_input = recording_input,
    .world_step_playback = world_step_playback,
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  return engine_abi_version == FT_GAME_ABI_VERSION ? &kModule : NULL;
}
