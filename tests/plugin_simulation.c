#include "plugin_api.h"
#include <sm64/sm64_game.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A headless SM64 plugin, the way a bruteforcer is written: the generic
// simulation API, and libsm64_physics called directly on the worlds the engine
// hands over (sm64/sm64_game.h).

// One of the game's own variables, as its headers declare it (game_init.h).
extern uint32_t gGlobalTimer;

FT_PLUGIN_ABI_EXPORT()
FT_API const char *plugin_game_id(void) { return SM64_GAME_ID; }
FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  return context->is_headless ? (void *)api : NULL;
}
FT_API void plugin_shutdown(void *data) { (void)data; }

FT_API int plugin_cli(void *data, int argc, const char **argv) {
  (void)argc; (void)argv;
  const tas_api_t *api = data;
  if (!api) return 1;
  ft_world *a = NULL, *b = NULL;
  sm64_world *own = NULL;
  void *inputs = NULL;
  int result = 1;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "simulation plugin: %s failed\n", #condition); goto cleanup; } } while (0)
  const ft_world *initial = api->get_initial_world();
  CHECK(initial);
  CHECK(api->entity_class_count() > 0);
  a = api->clone_world(initial);
  b = api->clone_world(initial);
  CHECK(a && b && api->world_player_count(a) == 1);
  inputs = calloc(1, api->input_record_size());
  CHECK(inputs);
  api->input_default(inputs);
  const int start = api->input_field_index("start");
  CHECK(start >= 0);
  // Power-on to the title screen, pressing Start now and then.
  for (int tick = 0; tick < 400; ++tick) {
    api->input_set(inputs, start, tick % 60 == 59);
    CHECK(api->step_world(a, inputs, 1));
    CHECK(api->step_world(b, inputs, 1));
  }
  CHECK(api->world_tick(a) == 400 && api->world_tick(initial) == 0);

  // The library, directly: the same console in memory of the plugin's own.
  own = sm64_world_clone(sm64_game_world(a));
  CHECK(own);
  api->copy_world(b, a);
  for (int tick = 0; tick < 200; ++tick) {
    api->input_set(inputs, start, tick % 30 == 29);
    CHECK(api->step_world(b, inputs, 1));
    sm64_step(own, sm64_game_input(inputs));
  }
  struct sm64_mario_info m1, m2;
  sm64_mario(sm64_game_world(b), &m1);
  sm64_mario(own, &m2);
  CHECK(memcmp(&m1, &m2, sizeof(m1)) == 0);
  CHECK(m1.global_timer == m2.global_timer && m1.global_timer > 0);
  // The game read directly: the world's copy of a variable.
  CHECK(*(const uint32_t *)sm64_world_variable(own, &gGlobalTimer) == m2.global_timer);
  puts("SM64 plugin simulation: clone, step, copy and direct library stepping agree");
  result = 0;
cleanup:
  if (a) api->destroy_world(a);
  if (b) api->destroy_world(b);
  sm64_world_destroy(own);
  free(inputs);
  return result;
#undef CHECK
}
