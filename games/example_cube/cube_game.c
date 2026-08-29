#include <frametee/game_abi.h>

#include <cimgui.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The arena, in world units. Y is up, matching the engine's 3D convention.
#define ARENA_X 40.f
#define ARENA_Y 24.f
#define ARENA_Z 40.f
#define CUBE_HALF 1.5f

enum { FIELD_PUSH_X = 0, FIELD_PUSH_Z, FIELD_JUMP, FIELD_COUNT };

typedef struct cube_input {
  int8_t push_x;
  int8_t push_z;
  int8_t jump;
  int8_t padding;
} cube_input;

struct ft_game {
  const ft_engine_api *engine;
};

struct ft_level {
  int unused;
};

struct ft_world {
  int32_t tick;
  float pos[3];
  float vel[3];
};

// --- input -------------------------------------------------------------------

static const ft_input_field input_fields[FIELD_COUNT] = {
    {"push_x", "Push X", "Nudge along X", FT_INPUT_INT, FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X, -1, 1, 0, 0.f, 0.f, 0.f, NULL,
     0, {0.4f, 0.8f, 1.f, 1.f}},
    {"push_z", "Push Z", "Nudge along Z", FT_INPUT_INT, FT_INPUT_FLAG_TIMELINE_LANE, -1, 1, 0, 0.f, 0.f, 0.f, NULL, 0,
     {0.5f, 1.f, 0.6f, 1.f}},
    {"jump", "Jump", "Only fires from the floor", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f, NULL, 0,
     {1.f, 0.85f, 0.35f, 1.f}},
};

static const ft_input_control input_controls[] = {
    {"push_x_neg", "Push -X", NULL, "Cube", "A", FIELD_PUSH_X, -1, 0, NULL},
    {"push_x_pos", "Push +X", NULL, "Cube", "D", FIELD_PUSH_X, 1, 0, NULL},
    {"push_z_neg", "Push -Z", NULL, "Cube", "W", FIELD_PUSH_Z, -1, 0, NULL},
    {"push_z_pos", "Push +Z", NULL, "Cube", "S", FIELD_PUSH_Z, 1, 0, NULL},
    {"jump", "Jump", NULL, "Cube", "Space", FIELD_JUMP, 1, 0, NULL},
};

static const ft_input_schema input_schema = {
    .struct_size = sizeof(ft_input_schema),
    .record_size = sizeof(cube_input),
    .record_align = _Alignof(cube_input),
    .fields = input_fields,
    .field_count = FIELD_COUNT,
    .controls = input_controls,
    .control_count = (uint32_t)(sizeof(input_controls) / sizeof(input_controls[0])),
};

// --- properties --------------------------------------------------------------

enum { PROP_POSITION = 0, PROP_VELOCITY, PROP_COUNT };

// A 3D game describes its positions as vectors of three, which is what
// FT_VALUE_VEC3 exists for: the inspector shows one row rather than three.
static const ft_prop_desc cube_props[PROP_COUNT] = {
    {"position", "Position", "Motion", "units", FT_VALUE_VEC3, FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY, 0.0, 0.0},
    {"velocity", "Velocity", "Motion", "units/tick", FT_VALUE_VEC3, FT_PROP_WRITABLE | FT_PROP_SUMMARY, 0.0, 0.0},
};

static const ft_entity_class entity_classes[] = {
    {"cube", "Cube", cube_props, PROP_COUNT},
};

// --- lifecycle ---------------------------------------------------------------

static ft_game *game_create(const ft_engine_api *engine) {
  ft_game *game = calloc(1, sizeof(ft_game));
  if (game) game->engine = engine;
  return game;
}

static void game_destroy(ft_game *game) { free(game); }

static ft_level *level_load_path(ft_game *game, const char *path, const char *variant) {
  (void)game;
  (void)path;
  (void)variant;
  return calloc(1, sizeof(ft_level));
}

static void level_destroy(ft_game *game, ft_level *level) {
  (void)game;
  free(level);
}

static bool level_info(ft_game *game, const ft_level *level, ft_level_info *out) {
  (void)game;
  if (!level || !out) return false;
  out->struct_size = sizeof(*out);
  out->name = "Cube arena";
  // The bounds stay a rectangle: it is the ground plane the editor frames the
  // camera against, and the height is the game's business.
  out->bounds = (ft_rect){0.f, 0.f, ARENA_X, ARENA_Z};
  out->width_tiles = (int)ARENA_X;
  out->height_tiles = (int)ARENA_Z;
  out->default_spawn = (ft_vec2){ARENA_X * 0.5f, ARENA_Z * 0.5f};
  return true;
}

static ft_world *world_create(ft_game *game, const ft_world_desc *desc) {
  (void)game;
  (void)desc;
  ft_world *world = calloc(1, sizeof(ft_world));
  if (!world) return NULL;
  world->pos[0] = ARENA_X * 0.5f;
  world->pos[1] = ARENA_Y * 0.6f;
  world->pos[2] = ARENA_Z * 0.5f;
  world->vel[0] = 0.18f;
  world->vel[2] = 0.11f;
  return world;
}

static void world_destroy(ft_game *game, ft_world *world) {
  (void)game;
  free(world);
}

static void world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  (void)game;
  if (dst && src) *dst = *src;
}

static void bounce_axis(float *pos, float *vel, float low, float high) {
  if (*pos < low) {
    *pos = low;
    *vel = -*vel * 0.86f;
  } else if (*pos > high) {
    *pos = high;
    *vel = -*vel * 0.86f;
  }
}

static void world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  (void)game;
  if (!world) return;
  const cube_input *in = inputs;

  if (in && player_count > 0) {
    world->vel[0] += (float)in->push_x * 0.02f;
    world->vel[2] += (float)in->push_z * 0.02f;
    if (in->jump && world->pos[1] <= CUBE_HALF + 0.001f) world->vel[1] = 0.62f;
  }

  world->vel[1] -= 0.021f; // gravity
  for (int i = 0; i < 3; ++i) world->pos[i] += world->vel[i];

  bounce_axis(&world->pos[0], &world->vel[0], CUBE_HALF, ARENA_X - CUBE_HALF);
  bounce_axis(&world->pos[1], &world->vel[1], CUBE_HALF, ARENA_Y - CUBE_HALF);
  bounce_axis(&world->pos[2], &world->vel[2], CUBE_HALF, ARENA_Z - CUBE_HALF);

  // Ground friction, so a run settles instead of sliding forever.
  if (world->pos[1] <= CUBE_HALF + 0.001f) {
    world->vel[0] *= 0.98f;
    world->vel[2] *= 0.98f;
  }
  ++world->tick;
}

static int32_t world_tick(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? world->tick : 0;
}

static int32_t world_player_count(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? 1 : 0;
}

static void input_default(ft_game *game, void *record) {
  (void)game;
  cube_input value = {0};
  memcpy(record, &value, sizeof(value));
}

static void input_set(ft_game *game, void *record, uint32_t field, int64_t value) {
  (void)game;
  cube_input *in = record;
  if (!in) return;
  switch (field) {
  case FIELD_PUSH_X: in->push_x = (int8_t)value; break;
  case FIELD_PUSH_Z: in->push_z = (int8_t)value; break;
  case FIELD_JUMP: in->jump = value ? 1 : 0; break;
  default: break;
  }
}

static int64_t input_get(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const cube_input *in = record;
  if (!in) return 0;
  switch (field) {
  case FIELD_PUSH_X: return in->push_x;
  case FIELD_PUSH_Z: return in->push_z;
  case FIELD_JUMP: return in->jump;
  default: return 0;
  }
}

// --- entities ----------------------------------------------------------------

static int32_t entity_count(ft_game *game, const ft_world *world, uint32_t class_index) {
  (void)game;
  return (world && class_index == 0) ? 1 : 0;
}

static bool entity_prop_get(ft_game *game, const ft_world *world, uint32_t class_index, int32_t entity, uint32_t prop, ft_value *out) {
  (void)game;
  if (!world || !out || class_index != 0 || entity != 0) return false;
  switch (prop) {
  case PROP_POSITION:
    out->kind = FT_VALUE_VEC3;
    out->as.v3 = (ft_vec3){world->pos[0], world->pos[1], world->pos[2]};
    return true;
  case PROP_VELOCITY:
    out->kind = FT_VALUE_VEC3;
    out->as.v3 = (ft_vec3){world->vel[0], world->vel[1], world->vel[2]};
    return true;
  default: return false;
  }
}

// Writable so the editor can author a starting state: a 3D game's position is
// one vec3 rather than a pair of floats, which is the whole point of the kind.
static bool entity_prop_set(ft_game *game, ft_world *world, uint32_t class_index, int32_t entity, uint32_t prop, const ft_value *value) {
  (void)game;
  if (!world || !value || class_index != 0 || entity != 0 || value->kind != FT_VALUE_VEC3) return false;
  float *target = prop == PROP_POSITION ? world->pos : prop == PROP_VELOCITY ? world->vel : NULL;
  if (!target) return false;
  target[0] = value->as.v3.x;
  target[1] = value->as.v3.y;
  target[2] = value->as.v3.z;
  return true;
}

// --- rendering ---------------------------------------------------------------

static void render(ft_game *game, const ft_render_frame *frame) {
  if (!game || !frame || !game->engine) return;
  const ft_engine_api *api = game->engine;

  if (frame->pass == FT_PASS_LEVEL_BACKGROUND) {
    // The arena as a wireframe box, and a grid on its floor. Both are ordinary
    // engine primitives: no shader, no pipeline, no texture.
    if (api->draw_box3) {
      api->draw_box3((ft_vec3){ARENA_X * 0.5f, ARENA_Y * 0.5f, ARENA_Z * 0.5f}, (ft_vec3){ARENA_X, ARENA_Y, ARENA_Z},
                     (ft_color){0.35f, 0.42f, 0.58f, 1.f}, true);
    }
    if (api->draw_line3) {
      const ft_color grid = {0.22f, 0.26f, 0.36f, 1.f};
      for (int i = 0; i <= 8; ++i) {
        const float t = (float)i / 8.f;
        api->draw_line3((ft_vec3){t * ARENA_X, 0.f, 0.f}, (ft_vec3){t * ARENA_X, 0.f, ARENA_Z}, grid, 0.06f);
        api->draw_line3((ft_vec3){0.f, 0.f, t * ARENA_Z}, (ft_vec3){ARENA_X, 0.f, t * ARENA_Z}, grid, 0.06f);
      }
    }
    return;
  }

  if (frame->pass != FT_PASS_ENTITIES || !frame->world) return;

  const ft_world *now = frame->world;
  const ft_world *before = frame->previous_world ? frame->previous_world : now;
  const float alpha = (frame->alpha >= 0.f && frame->alpha <= 1.f) ? frame->alpha : 1.f;

  ft_vec3 pos;
  pos.x = before->pos[0] + (now->pos[0] - before->pos[0]) * alpha;
  pos.y = before->pos[1] + (now->pos[1] - before->pos[1]) * alpha;
  pos.z = before->pos[2] + (now->pos[2] - before->pos[2]) * alpha;

  if (api->draw_box3) {
    api->draw_box3(pos, (ft_vec3){CUBE_HALF * 2.f, CUBE_HALF * 2.f, CUBE_HALF * 2.f}, frame->accent, false);
    // A flat shadow marker on the floor, which is what makes height readable in
    // a still frame.
    api->draw_box3((ft_vec3){pos.x, 0.02f, pos.z}, (ft_vec3){CUBE_HALF * 2.f, 0.02f, CUBE_HALF * 2.f},
                   (ft_color){0.f, 0.f, 0.f, 0.35f}, false);
  }
  if (api->draw_line3) {
    // Velocity, the same affordance the 2D examples draw with draw_line.
    api->draw_line3(pos, (ft_vec3){pos.x + now->vel[0] * 8.f, pos.y + now->vel[1] * 8.f, pos.z + now->vel[2] * 8.f},
                    (ft_color){0.45f, 0.82f, 1.f, 0.9f}, 0.1f);
  }
}

// The start screen. A game owns its own, which is how the engine avoids
// knowing anything about what a level is for a particular game.
static void ui(ft_game *game, const ft_ui_frame *frame) {
  if (!game || !frame || frame->slot != FT_UI_SPLASH) return;

  igTextUnformatted("Cube Arena", NULL);
  igSpacing();
  igTextUnformatted("A cube bouncing in a volume, drawn entirely with the engine's 3D primitives.", NULL);
  igSpacing();
  igSeparator();
  igSpacing();

  if (igButton("Start cube arena", (ImVec2){220.f, 48.f}) && game->engine && game->engine->request_level)
    game->engine->request_level("builtin:cube-arena");
}

// --- module ------------------------------------------------------------------

static const ft_game_module module = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,
    .info = {.struct_size = sizeof(ft_game_info),
             .id = "example-cube",
             .display_name = "Cube Arena (3D)",
             .version = "1.0.0",
             .author = "FrameTee"},
    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_HEADLESS | FT_CAP_RENDERS_LEVEL,
                    .dimensions = FT_DIMENSIONS_3D,
                    .min_players = 1,
                    .max_players = 1,
                    .ticks_per_second = 60,
                    .units_per_tile = 1.f,
                    .default_camera_height = ARENA_Z},
    .input_schema = &input_schema,
    .entity_classes = entity_classes,
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
    .input_default = input_default,
    .input_set = input_set,
    .input_get = input_get,
    .entity_count = entity_count,
    .entity_prop_get = entity_prop_get,
    .entity_prop_set = entity_prop_set,
    .render = render,
    .ui = ui,
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  return engine_abi_version == FT_GAME_ABI_VERSION ? &module : NULL;
}
