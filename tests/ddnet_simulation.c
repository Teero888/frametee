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

// How often the first tee shoots its gun while its record's fire counter goes through `runs` (pairs
// of value and ticks, ended by a zero length): a shot shows as the reload timer going up.
static int gun_shots(const tas_api_t *api, const ft_world *initial, void *inputs, uint32_t record_size, int players,
                     const int (*runs)[2]) {
  ft_world *world = api->clone_world(initial);
  if (!world) return -1;
  for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);
  dd_input_t *record = inputs;
  record->m_TargetX = 100;
  record->m_TargetY = 0;
  int shots = 0, reload = 0;
  for (int run = 0; runs[run][1] > 0; ++run) {
    for (int tick = 0; tick < runs[run][1]; ++tick) {
      record->m_Fire = (uint8_t)runs[run][0];
      if (!api->step_world(world, inputs, (uint32_t)players)) {
        api->destroy_world(world);
        return -1;
      }
      const ddnet_character_t *chr = ddnet_player_character(world, 0);
      if (chr && chr->reload_timer > reload) ++shots;
      reload = chr ? chr->reload_timer : 0;
    }
  }
  api->destroy_world(world);
  for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);
  return shots;
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

  // Fire is a counter whose low bit is the button, and the physics counts every value between two
  // ticks' counters as presses and releases: pressing or letting go only ever steps it up, so a
  // release is +1 from odd and nothing from even. Lowering it would fire.
  {
    dd_input_t *record = inputs;
    const int fire = api->input_field_index("fire");
    const uint8_t start = record->m_Fire;
    api->input_set(record, fire, 0);
    CHECK(record->m_Fire == start);
    api->input_set(record, fire, 1);
    CHECK(record->m_Fire == start + 1 && api->input_get(record, fire) == 1);
    api->input_set(record, fire, 1);
    CHECK(record->m_Fire == start + 1);
    api->input_set(record, fire, 0);
    CHECK(record->m_Fire == start + 2 && api->input_get(record, fire) == 0);
    api->input_set(record, fire, 0);
    CHECK(record->m_Fire == start + 2);
    record->m_Fire = 63;
    api->input_set(record, fire, 0);
    CHECK(record->m_Fire == 0);
    api->input_default(record);
  }

  // The record only says whether fire is held, as its lane shows; the counter itself may jump or
  // drop where inputs made apart meet (a take recorded on after rewinding, a snippet boundary, an
  // edited tick). The tee shoots when the button goes down, and only then.
  {
    static const int held_then_let_go[][2] = {{0, 5}, {1, 30}, {0, 30}, {0, 0}};
    static const int jump_while_held[][2] = {{0, 5}, {1, 30}, {5, 30}, {0, 0}};
    static const int drop_while_released[][2] = {{0, 5}, {1, 10}, {2, 20}, {0, 20}, {0, 0}};
    static const int pressed_twice[][2] = {{0, 5}, {1, 30}, {2, 30}, {3, 30}, {0, 0}};
    CHECK(gun_shots(api, initial, inputs, record_size, players, held_then_let_go) == 1);
    CHECK(gun_shots(api, initial, inputs, record_size, players, jump_while_held) == 1);
    CHECK(gun_shots(api, initial, inputs, record_size, players, drop_while_released) == 1);
    CHECK(gun_shots(api, initial, inputs, record_size, players, pressed_twice) == 2);
  }

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
