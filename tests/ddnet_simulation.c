#include "plugin_api.h"
#include <ddnet/ddnet_game.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A headless DDNet plugin, the way a bruteforcer is written: the generic
// simulation API, and ddnet_physics called directly on the worlds the engine
// hands over (ddnet/ddnet_game.h). Both have to step a world the same way.

FT_PLUGIN_ABI_EXPORT()
FT_API const char *plugin_game_id(void) { return "ddnet"; }
FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  return context->is_headless ? (void *)api : NULL;
}
FT_API void plugin_shutdown(void *data) { (void)data; }

// The same tee, bit for bit.
static bool same_tee(const ddnet_character_t *a, const ddnet_character_t *b) {
  if (!a || !b) return a == b;
  return memcmp(&a->pos, &b->pos, sizeof(a->pos)) == 0 && memcmp(&a->core.vel, &b->core.vel, sizeof(a->core.vel)) == 0 &&
         a->core.hook_state == b->core.hook_state && a->freeze_time == b->freeze_time && a->core.jumped == b->core.jumped;
}

// What a player does on a tick of the test: running right, jumping, hooking and shooting now and then.
static void scripted_input(const tas_api_t *api, void *record, int tick) {
  api->input_set(record, api->input_field_index("direction"), (tick / 40) % 3 == 2 ? -1 : 1);
  api->input_set(record, api->input_field_index("jump"), tick % 23 < 3);
  api->input_set(record, api->input_field_index("hook"), tick % 57 < 20);
  api->input_set(record, api->input_field_index("fire"), tick % 31 == 0);
  api->input_set_vec2(record, api->input_field_index("target"), (ft_vec2){(float)(tick % 200 - 100), -60.f});
}

FT_API int plugin_cli(void *data, int argc, const char **argv) {
  (void)argc;
  (void)argv;
  const tas_api_t *api = data;
  if (!api) return 1;
  ft_world *a = NULL, *b = NULL;
  ddnet_world_t own;
  memset(&own, 0, sizeof(own));
  void *inputs = NULL;
  int result = 1;
#define CHECK(condition)                                                \
  do {                                                                  \
    if (!(condition)) {                                                 \
      fprintf(stderr, "ddnet simulation plugin: %s failed\n", #condition); \
      goto cleanup;                                                     \
    }                                                                   \
  } while (0)
  const ft_world *initial = api->get_initial_world();
  CHECK(initial);
  CHECK(api->world_player_count(initial) >= 1 && api->world_tick(initial) == 0);
  // The engine's first tick has the tee standing at the spawn.
  const ddnet_character_t *spawned = ddnet_player_character(initial, 0);
  CHECK(spawned && ddnet_world(initial)->tick == 0);
  const int players = api->world_player_count(initial);

  a = api->clone_world(initial);
  b = api->clone_world(initial);
  CHECK(a && b);
  const uint32_t record_size = api->input_record_size();
  CHECK(record_size >= sizeof(dd_input_t));
  inputs = calloc((size_t)players, record_size);
  CHECK(inputs);
  for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);

  // Two clones, stepped alike, stay alike.
  for (int tick = 0; tick < 300; ++tick) {
    for (int p = 0; p < players; ++p) scripted_input(api, (char *)inputs + (size_t)p * record_size, tick + p * 7);
    CHECK(api->step_world(a, inputs, (uint32_t)players));
    CHECK(api->step_world(b, inputs, (uint32_t)players));
    for (int p = 0; p < players; ++p) CHECK(same_tee(ddnet_player_character(a, p), ddnet_player_character(b, p)));
  }
  CHECK(api->world_tick(a) == 300 && api->world_tick(initial) == 0);
  ft_player_view view;
  CHECK(api->world_player_view(a, 0, &view));
  CHECK(!same_tee(ddnet_player_character(a, 0), spawned));

  // The library, directly: the world of the clone, stepped by ddnet_physics with the records turned into its input.
  CHECK(ddnet_world_copy(&own, ddnet_world(b)));
  for (int tick = 300; tick < 800; ++tick) {
    for (int p = 0; p < players; ++p) {
      dd_input_t *record = (dd_input_t *)((char *)inputs + (size_t)p * record_size);
      scripted_input(api, record, tick + p * 7);
      const int client_id = ddnet_player_client(b, p);
      ddnet_input_from_record(record, &own.players[client_id].input);
      own.characters[client_id].tele_out = record->m_TeleOut;
    }
    CHECK(api->step_world(b, inputs, (uint32_t)players));
    ddnet_world_tick(&own);
    for (int p = 0; p < players; ++p) {
      const int client_id = ddnet_player_client(b, p);
      CHECK(same_tee(ddnet_player_character(b, p), ddnet_world_character(&own, client_id)));
    }
  }
  CHECK(own.tick == ddnet_world(b)->tick);

  // A kill is done before the tick: the tee dies and spawns again.
  memset(inputs, 0, (size_t)players * record_size);
  for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);
  api->input_set(inputs, api->input_field_index("kill"), 1);
  CHECK(api->step_world(a, inputs, (uint32_t)players));
  api->input_set(inputs, api->input_field_index("kill"), 0);
  for (int tick = 0; tick < 3; ++tick) CHECK(api->step_world(a, inputs, (uint32_t)players));
  const ddnet_character_t *respawned = ddnet_player_character(a, 0);
  CHECK(respawned && respawned->spawn_tick > 300);

  printf("DDNet plugin simulation: %d players, clone, step, copy and direct library stepping agree\n", players);
  result = 0;
cleanup:
  if (a) api->destroy_world(a);
  if (b) api->destroy_world(b);
  ddnet_world_free(&own);
  free(inputs);
  return result;
#undef CHECK
}
