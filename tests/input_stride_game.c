#include <frametee/game_abi.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_MAGIC = 0x4654494e50555431ULL };

typedef struct __attribute__((aligned(8))) stride_input_t {
  uint64_t magic;
  uint32_t value;
  uint32_t padding;
} stride_input_t;

struct ft_game {
  int unused;
};

struct ft_level {
  int unused;
};

struct ft_world {
  int tick;
};

static const ft_input_schema input_schema = {
    .struct_size = sizeof(ft_input_schema),
    .record_size = sizeof(stride_input_t),
    .record_align = _Alignof(stride_input_t),
};

static ft_game *game_create(const ft_engine_api *engine) {
  (void)engine;
  return calloc(1, sizeof(ft_game));
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
  out->name = "Input stride test";
  out->bounds = (ft_rect){0.f, 0.f, 1.f, 1.f};
  return true;
}

static ft_world *world_create(ft_game *game, const ft_world_desc *desc) {
  (void)game;
  (void)desc;
  return calloc(1, sizeof(ft_world));
}

static void world_destroy(ft_game *game, ft_world *world) {
  (void)game;
  free(world);
}

static void world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  (void)game;
  if (dst && src) *dst = *src;
}

static void world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  (void)game;
  const stride_input_t *records = inputs;
  if (!world || !records || player_count != 2 || records[0].magic != INPUT_MAGIC || records[1].magic != INPUT_MAGIC) abort();
  ++world->tick;
}

static int32_t world_tick(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? world->tick : 0;
}

static int32_t world_player_count(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? 2 : 0;
}

static void input_default(ft_game *game, void *record) {
  (void)game;
  stride_input_t value = {.magic = INPUT_MAGIC};
  memcpy(record, &value, sizeof(value));
}

static const ft_game_module module = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,
    .info = {.struct_size = sizeof(ft_game_info),
             .id = "input-stride-test",
             .display_name = "Input stride test",
             .version = "1.0.0",
             .author = "FrameTee tests"},
    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_HEADLESS,
                    .min_players = 2,
                    .max_players = 2,
                    .ticks_per_second = 50,
                    .units_per_tile = 1.f,
                    .default_camera_height = 1.f},
    .input_schema = &input_schema,
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
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  return engine_abi_version == FT_GAME_ABI_VERSION ? &module : NULL;
}
