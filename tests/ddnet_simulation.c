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
  int switches_checked = 0;
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

  // What a player does besides moving its tee, before the tick its record is for: its team and the
  // lock of the team, /spec, leaving and coming back, and the player flags.
  {
    const int team = api->input_field_index("team"), lock = api->input_field_index("lock");
    const int spec = api->input_field_index("spec"), connection = api->input_field_index("connection");
    const int chatting = api->input_field_index("flag_chatting"), tele_out = api->input_field_index("tele_out");
    const int direction = api->input_field_index("direction"), kill = api->input_field_index("kill");
    CHECK(team >= 0 && lock >= 0 && spec >= 0 && connection >= 0 && chatting >= 0 && tele_out >= 0);
    CHECK(!(api->input_field((uint32_t)tele_out)->flags & FT_INPUT_FLAG_INTERNAL));
    for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);
    void *record = inputs;
    // Nothing set does nothing: the team, its lock and the connection are left as they are.
    CHECK(api->input_get(record, team) == -1 && api->input_get(record, lock) == 0 && api->input_get(record, connection) == 0 &&
          api->input_get(record, spec) == 0 && api->input_get(record, chatting) == 0);

    ft_world *w = api->clone_world(initial);
    CHECK(w);
    const ddnet_world_t *core = ddnet_world(w);
    const int client = ddnet_player_client(w, 0);
#define STEP(n)                                                                    \
  do {                                                                             \
    for (int step = 0; step < (n); ++step) CHECK(api->step_world(w, inputs, (uint32_t)players)); \
  } while (0)
    STEP(1);
    CHECK(core->players[client].team == 0);

    // Into team 3, locked; then unlocked.
    api->input_set(record, team, 3);
    api->input_set(record, lock, 1);
    STEP(1);
    CHECK(core->players[client].team == 3 && ddnet_world_team(core, 3).locked);
    api->input_set(record, lock, 2);
    STEP(1);
    CHECK(core->players[client].team == 3 && !ddnet_world_team(core, 3).locked);

    // Left to the game, a tee that dies in a team that is not locked comes back in team 0; held, the
    // team puts it back.
    api->input_set(record, team, -1);
    api->input_set(record, lock, 0);
    api->input_set(record, kill, 1);
    STEP(1);
    api->input_set(record, kill, 0);
    STEP(3);
    CHECK(ddnet_player_character(w, 0) && core->players[client].team == 0);
    api->input_set(record, team, 3);
    api->input_set(record, kill, 1);
    STEP(1);
    api->input_set(record, kill, 0);
    STEP(3);
    CHECK(ddnet_player_character(w, 0) && core->players[client].team == 3);
    api->input_set(record, team, 0);
    STEP(1);
    CHECK(core->players[client].team == 0);

    // /spec without sv_pauseable pauses the tee where it is, which takes no input until it is back.
    api->input_set(record, spec, 1);
    STEP(1);
    CHECK(core->players[client].paused == DDNET_PAUSE_PAUSED && ddnet_player_character(w, 0));
    api->input_set(record, direction, 1);
    STEP(5);
    CHECK(ddnet_player_character(w, 0)->core.input.direction == 0);
    api->input_set(record, spec, 0);
    STEP(1);
    CHECK(core->players[client].paused == DDNET_PAUSE_NONE && ddnet_player_character(w, 0)->core.input.direction == 1);

    // Chatting keeps the input from the tee as well.
    api->input_set(record, direction, 0);
    STEP(1);
    api->input_set(record, chatting, 1);
    api->input_set(record, direction, -1);
    STEP(5);
    CHECK(ddnet_player_character(w, 0)->core.input.direction == 0);
    api->input_set(record, chatting, 0);
    STEP(1);
    CHECK(ddnet_player_character(w, 0)->core.input.direction == -1);
    api->input_set(record, direction, 0);

    // Leaving takes the tee out of the game, and it stays out once that is no longer said; coming
    // back, it spawns again.
    api->input_set(record, connection, 2);
    STEP(1);
    CHECK(!core->players[client].active && !ddnet_player_character(w, 0));
    ft_player_view gone;
    CHECK(!api->world_player_view(w, 0, &gone));
    api->input_set(record, connection, 0);
    STEP(10);
    CHECK(!core->players[client].active);
    api->input_set(record, connection, 1);
    STEP(2);
    CHECK(core->players[client].active && ddnet_player_character(w, 0) && ddnet_player_client(w, 0) == client);
#undef STEP
    api->destroy_world(w);
    for (int p = 0; p < players; ++p) api->input_default((char *)inputs + (size_t)p * record_size);
  }

  // A starting state can have the tee in a team, locked, with its race started some time ago, and
  // a stored world keeps all of it.
  {
    const ft_entity_class *player_class = api->entity_class(FT_ENTITY_CLASS_PLAYER);
    CHECK(player_class);
    int connected = -1, team = -1, locked = -1, started = -1, elapsed = -1, switches = -1;
    for (uint32_t i = 0; i < player_class->prop_count; ++i) {
      const char *id = player_class->props[i].id;
      if (strcmp(id, "connected") == 0) connected = (int)i;
      if (strcmp(id, "team") == 0) team = (int)i;
      if (strcmp(id, "team_locked") == 0) locked = (int)i;
      if (strcmp(id, "race_started") == 0) started = (int)i;
      if (strcmp(id, "race_elapsed") == 0) elapsed = (int)i;
      if (strcmp(id, "switches_off") == 0) switches = (int)i;
    }
    CHECK(connected >= 0 && team >= 0 && locked >= 0 && started >= 0 && elapsed >= 0 && switches >= 0);
    // (being in the game before the team, and the team before what is about the team the tee is in)
    CHECK(connected < team && team < locked && team < started && team < switches && started < elapsed);

    ft_world *w = api->clone_world(initial);
    ft_world *stored = api->clone_world(initial);
    CHECK(w && stored);
    const ft_value team_5 = {.kind = FT_VALUE_INT, .as.i = 5}, yes = {.kind = FT_VALUE_BOOL, .as.b = true};
    const ft_value two_and_a_half = {.kind = FT_VALUE_FLOAT, .as.f = 2.5}, none = {.kind = FT_VALUE_STRING, .as.s = ""};
    const ft_value bad = {.kind = FT_VALUE_STRING, .as.s = "3-1"};
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)team, &team_5));
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)locked, &yes));
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)started, &yes));
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)elapsed, &two_and_a_half));
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)switches, &none));
    CHECK(!api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)switches, &bad));
    const ddnet_world_t *core = ddnet_world(w);
    const int client = ddnet_player_client(w, 0);
    CHECK(core->players[client].team == 5 && ddnet_world_team(core, 5).locked &&
          ddnet_world_team(core, 5).state == DDNET_TEAMSTATE_STARTED);
    const ddnet_character_t *chr = ddnet_player_character(w, 0);
    CHECK(chr && chr->race_state == DDNET_RACE_STARTED && core->tick - chr->start_time == 125);

    size_t size = api->serialize_world(w, NULL, 0);
    void *data = size ? malloc(size) : NULL;
    CHECK(data && api->serialize_world(w, data, size) == size);
    const bool restored = api->deserialize_world(stored, data, size);
    free(data);
    CHECK(restored);
    const ddnet_world_t *again = ddnet_world(stored);
    const ddnet_character_t *chr_again = ddnet_player_character(stored, 0);
    CHECK(again->players[ddnet_player_client(stored, 0)].team == 5 && ddnet_world_team(again, 5).locked && chr_again &&
          chr_again->race_state == DDNET_RACE_STARTED && again->tick - chr_again->start_time == 125);
    ft_value value;
    CHECK(api->entity_prop_get(stored, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)elapsed, &value) && value.as.f == 2.5);

    // Deactivated switches, on a map that has any.
    if (core->num_switchers > 2) {
      const ft_value first = {.kind = FT_VALUE_STRING, .as.s = "1"};
      CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)switches, &first));
      CHECK(!ddnet_world_switch(core, 1, 5).status && ddnet_world_switch(core, 2, 5).status);
      CHECK(api->entity_prop_get(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)switches, &value) && strcmp(value.as.s, "1") == 0);
      switches_checked = core->num_switchers - 1;
    }

    // A player can start out of the game, and a stored world keeps it out.
    const ft_value no = {.kind = FT_VALUE_BOOL, .as.b = false};
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)connected, &no));
    CHECK(!core->players[client].active && !ddnet_player_character(w, 0));
    CHECK(api->entity_prop_get(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)connected, &value) && !value.as.b);
    size = api->serialize_world(w, NULL, 0);
    data = size ? malloc(size) : NULL;
    CHECK(data && api->serialize_world(w, data, size) == size);
    const bool restored_out = api->deserialize_world(stored, data, size);
    free(data);
    CHECK(restored_out && !again->players[ddnet_player_client(stored, 0)].active && !ddnet_player_character(stored, 0));
    CHECK(api->entity_prop_set(w, FT_ENTITY_CLASS_PLAYER, 0, (uint32_t)connected, &yes));
    CHECK(core->players[client].active && ddnet_player_character(w, 0));
    api->destroy_world(w);
    api->destroy_world(stored);
  }

  printf("DDNet plugin simulation: %d players, clone, step, copy and direct library stepping agree; teams, /spec, "
         "connecting and player flags work (%d switches)\n",
         players, switches_checked);
  result = 0;
cleanup:
  if (a) api->destroy_world(a);
  if (b) api->destroy_world(b);
  ddnet_world_free(&own);
  free(inputs);
  return result;
#undef CHECK
}
