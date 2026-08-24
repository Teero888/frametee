#include "../games/ddnet/dd_internal.h"

#define DDNET_DEMO_IMPLEMENTATION
#include <ddnet_demo/ddnet_demo.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef const ft_game_module *(*module_entry_fn)(uint32_t engine_abi_version);

static const ft_world *g_previous_world;
static const ft_world *g_current_world;

static void test_log(ft_log_level level, const char *category, const char *message) {
  (void)level;
  (void)category;
  (void)message;
}

static void test_get_state(ft_engine_state *out) {
  if (!out) return;
  *out = (ft_engine_state){.struct_size = sizeof(*out), .headless = true, .selected_player = -1};
}

static bool test_get_player_setup(int32_t player, ft_player_setup *out) {
  if (player != 0 || !out) return false;
  *out = (ft_player_setup){.struct_size = sizeof(*out),
                           .name = "same-tick",
                           .tag = "",
                           .appearance_id = "default",
                           .primary_color = {1.f, 1.f, 1.f, 1.f},
                           .secondary_color = {1.f, 1.f, 1.f, 1.f},
                           .linked_player = -1};
  return true;
}

static uint32_t test_timeline_world_count(void) { return 1; }

static bool test_timeline_world_info(uint32_t world_index, ft_timeline_world_info *out) {
  if (world_index != 0 || !out) return false;
  *out = (ft_timeline_world_info){
      .struct_size = sizeof(*out), .world_index = 0, .start_offset = 0, .player_count = 1, .name = "same-tick"};
  return true;
}

static bool test_timeline_world_pair(uint32_t world_index, int32_t global_tick, const ft_world **out_previous,
                                     const ft_world **out_current) {
  if (world_index != 0 || global_tick != 1 || !g_previous_world || !g_current_world) return false;
  if (out_previous) *out_previous = g_previous_world;
  if (out_current) *out_current = g_current_world;
  return true;
}

static int32_t test_timeline_player_track(uint32_t world_index, uint32_t local_player) {
  return world_index == 0 && local_player == 0 ? 0 : -1;
}

static bool has_particle_event(const ft_world *world, int type) {
  for (int i = 0; i < world->physics_particle_event_count; ++i)
    if (world->physics_particle_events[i].type == type) return true;
  return false;
}

static bool has_sound_event(const ft_world *world, int sound_id) {
  for (int i = 0; i < world->physics_sound_event_count; ++i)
    if (world->physics_sound_events[i].sound_id == sound_id) return true;
  return false;
}

static bool demo_has_explosion_and_sound(const char *path) {
  FILE *file = fopen(path, "rb");
  dd_demo_reader *reader = demo_r_create();
  bool found_explosion = false;
  bool found_fire_sound = false;
  bool found_explosion_sound = false;
  if (!file || !reader || !demo_r_open(reader, file)) goto done;

  dd_demo_chunk chunk;
  uint8_t unpacked[DD_SNAPSHOT_MAX_SIZE];
  while (demo_r_next_chunk(reader, &chunk)) {
    const dd_snapshot *snapshot = NULL;
    if (chunk.type == DD_CHUNK_SNAP) snapshot = (const dd_snapshot *)chunk.data;
    else if (chunk.type == DD_CHUNK_SNAP_DELTA && demo_r_unpack_delta(reader, chunk.data, unpacked) > 0)
      snapshot = (const dd_snapshot *)unpacked;
    if (!snapshot) continue;
    for (int i = 0; i < snapshot->num_items; ++i) {
      const dd_snap_item *item = dd_snap_get_item(snapshot, i);
      if (!item) continue;
      if (dd_snap_item_type(item) == DD_NETEVENTTYPE_EXPLOSION) found_explosion = true;
      if (dd_snap_item_type(item) == DD_NETEVENTTYPE_SOUNDWORLD) {
        const dd_netevent_sound_world *sound = (const dd_netevent_sound_world *)dd_snap_item_data(item);
        if (sound->m_SoundId == SOUND_TYPE_GRENADE_FIRE) found_fire_sound = true;
        if (sound->m_SoundId == SOUND_TYPE_GRENADE_EXPLODE) found_explosion_sound = true;
      }
    }
  }

done:
  if (reader) demo_r_destroy(&reader);
  if (file) fclose(file);
  return found_explosion && found_fire_sound && found_explosion_sound;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s DDNET_MODULE MAP OUTPUT_DEMO\n", argv[0]);
    return 2;
  }

  void *library = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "could not load DDNet module: %s\n", dlerror());
    return 1;
  }
  module_entry_fn entry = (module_entry_fn)dlsym(library, FT_GAME_MODULE_ENTRY_NAME);
  const ft_game_module *module = entry ? entry(FT_GAME_ABI_VERSION) : NULL;
  if (!module) {
    fprintf(stderr, "could not resolve the DDNet module entry point\n");
    dlclose(library);
    return 1;
  }

  const ft_engine_api engine = {.struct_size = sizeof(engine),
                                .log = test_log,
                                .get_state = test_get_state,
                                .get_player_setup = test_get_player_setup,
                                .timeline_world_count = test_timeline_world_count,
                                .timeline_world_info = test_timeline_world_info,
                                .timeline_world_pair = test_timeline_world_pair,
                                .timeline_player_track = test_timeline_player_track};
  ft_game *game = module->create(&engine);
  ft_level *level = game ? module->level_load_path(game, argv[2], NULL) : NULL;
  const ft_world_desc desc = {
      .struct_size = sizeof(desc), .level = level, .variant_id = NULL, .player_count = 1, .world_index = 0};
  ft_world *world = level ? module->world_create(game, &desc) : NULL;
  if (!world) {
    fprintf(stderr, "could not create the DDNet test world\n");
    if (level) module->level_destroy(game, level);
    if (game) module->destroy(game);
    dlclose(library);
    return 1;
  }
  ft_world *previous = module->world_create(game, &desc);
  if (!previous) {
    fprintf(stderr, "could not create the previous DDNet test world\n");
    module->world_destroy(game, world);
    module->level_destroy(game, level);
    module->destroy(game);
    dlclose(library);
    return 1;
  }
  // Put the muzzle inside a solid tile. The projectile is created by input,
  // reports its collision and is removed during this same world_step.
  ft_value inside_block = {.kind = FT_VALUE_VEC2};
  bool found_block = false;
  for (int y = 1; y + 1 < level->collision.m_MapData.height && !found_block; ++y) {
    for (int x = 1; x + 1 < level->collision.m_MapData.width; ++x) {
      if (!check_point(&level->collision, vec2_init((float)x * 32.f + 16.f, (float)y * 32.f + 16.f))) continue;
      inside_block.as.v = (ft_vec2){(float)x + 0.5f, (float)y + 0.5f};
      found_block = true;
      break;
    }
  }
  const ft_value grenade = {.kind = FT_VALUE_INT, .as.i = WEAPON_GRENADE};
  if (!found_block || !module->entity_prop_set(game, world, FT_ENTITY_CLASS_PLAYER, 0, 0, &inside_block) ||
      !module->entity_prop_set(game, world, FT_ENTITY_CLASS_PLAYER, 0, 2, &grenade)) {
    fprintf(stderr, "could not prepare the same-tick grenade\n");
    module->world_destroy(game, previous);
    module->world_destroy(game, world);
    module->level_destroy(game, level);
    module->destroy(game);
    dlclose(library);
    return 1;
  }
  module->world_copy(game, previous, world);

  SPlayerInput input;
  module->input_default(game, &input);
  input.m_TargetX = 1;
  input.m_TargetY = 0;
  input.m_WantedWeapon = WEAPON_GRENADE;
  input.m_Fire = 1;
  module->world_step(game, world, &input, 1);

  const bool exploded = has_particle_event(world, PARTICLE_TYPE_EXPLOSION);
  const bool fire_sound = has_sound_event(world, SOUND_TYPE_GRENADE_FIRE);
  const bool explosion_sound = has_sound_event(world, SOUND_TYPE_GRENADE_EXPLODE);
  const bool projectile_survived = module->entity_count(game, world, 1) != 0;

  ft_world *copy = module->world_create(game, &desc);
  if (copy) module->world_copy(game, copy, world);
  const bool copy_retained_event = copy && has_particle_event(copy, PARTICLE_TYPE_EXPLOSION);

  g_previous_world = previous;
  g_current_world = copy;
  const ft_export_request request = {.struct_size = sizeof(request), .path = argv[3], .start_tick = 1, .end_tick = 1};
  const bool exported = copy && module->export_run(game, 0, &request);
  const bool exported_explosion = exported && demo_has_explosion_and_sound(argv[3]);
  remove(argv[3]);
  g_previous_world = NULL;
  g_current_world = NULL;

  if (copy) module->world_destroy(game, copy);
  module->world_destroy(game, previous);
  module->world_destroy(game, world);
  module->level_destroy(game, level);
  module->destroy(game);
  dlclose(library);

  if (!exploded || !fire_sound || !explosion_sound || projectile_survived || !copy_retained_event || !exported_explosion) {
    fprintf(stderr,
            "same-tick grenade regression: exploded=%d fire_sound=%d explosion_sound=%d projectile_survived=%d copied=%d exported=%d\n",
            exploded, fire_sound, explosion_sound, projectile_survived, copy_retained_event, exported_explosion);
    return 1;
  }
  return 0;
}
