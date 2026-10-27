#ifndef DDNET_GAME_PUBLIC_H
#define DDNET_GAME_PUBLIC_H

// What the DDNet game module promises its own plugins.
//
// The engine treats ft_world and ft_level as opaque, and rightly so. A plugin
// that declares itself DDNet-specific with
//
//   FT_API const char *plugin_game_id(void) { return "ddnet"; }
//
// is however built for this game and may look inside, which is what this header
// is for. Nothing here is part of the engine ABI: it is a contract between one
// game and the plugins written for it, and it changes when this game changes.
//
// The physics is ddnet_physics (<ddnet_physics/ddnet_physics.h>), its optimized
// backend. A world steps with ddnet_world_tick, or with ddnet_ev_world_tick when
// its effects are wanted (see ddnet_step_events).
//
// The module is built twice from the same sources: as DDNet (game "ddnet", the
// physics' DDNet backend) and as the Unique Race mod of it (game "unique", its
// Unique backend, which defines DDNET_PHYSICS_GAME_UNIQUE and DD_UNIQUE here).
// Unique Race is played on solo servers: a world of the module has a world of
// the physics for each of its players, which has that one player, client 0,
// and no teams, switches or other tees (see dd_world_space). The dd_client_*
// functions below read a player of a world of the physics either way.

#include <ddnet_physics/ddnet_physics.h>
#include <frametee/game_abi.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DDNET_PHYSICS_GAME_UNIQUE)
#define DD_UNIQUE 1
#define DD_GAME_NAME "Unique Race"
#else
#define DD_UNIQUE 0
#define DD_GAME_NAME "DDNet"
#endif

// --- players of the physics ---------------------------------------------------

// The player of a client id (Unique Race: the one player).
static inline ddnet_player_t *dd_client(const ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  (void)client_id;
  return (ddnet_player_t *)&world->player;
#else
  return (ddnet_player_t *)&world->players[client_id];
#endif
}

// The living tee of a client id, or NULL.
static inline ddnet_character_t *dd_client_character(const ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  return client_id == 0 ? ddnet_world_character((ddnet_world_t *)world) : 0;
#else
  return ddnet_world_character((ddnet_world_t *)world, client_id);
#endif
}

// To be called after writing to the tee of a client id (ddnet_character_changed).
static inline void dd_client_character_changed(ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  (void)client_id;
  ddnet_character_changed(world);
#else
  ddnet_character_changed(world, client_id);
#endif
}

// How many client ids the world has had (Unique Race: its one player).
static inline int dd_client_count(const ddnet_world_t *world) {
#if DD_UNIQUE
  (void)world;
  return 1;
#else
  return world->num_clients;
#endif
}

// Whether a client is in the game (Unique Race: always).
static inline bool dd_client_active(const ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  (void)world;
  return client_id == 0;
#else
  return world->players[client_id].active;
#endif
}

// The ddrace team of a client (Unique Race: 0, which is no team).
static inline int dd_client_team(const ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  (void)world;
  (void)client_id;
  return 0;
#else
  return world->players[client_id].team;
#endif
}

// The race time of a client that finished, in seconds; -1 until then.
static inline float dd_client_finish_time(const ddnet_world_t *world, int client_id) {
  const ddnet_player_t *player = dd_client(world, client_id);
  if (player->finish_tick < 0) return -1.f;
#if DD_UNIQUE
  return player->finish_time;
#else
  return (float)player->finish_time_ticks / (float)DDNET_TICK_SPEED;
#endif
}

// The race time of a client that finished, in whole ticks (Unique Race times
// its races in fractions of a tick: rounded).
static inline int dd_client_finish_ticks(const ddnet_world_t *world, int client_id) {
#if DD_UNIQUE
  return (int)(dd_client_finish_time(world, client_id) * (float)DDNET_TICK_SPEED + 0.5f);
#else
  return dd_client(world, client_id)->finish_time_ticks;
#endif
}

// What only a tee of DDNet can be (never in Unique Race): deep frozen, in a
// freeze tile, with a jetpack.
static inline bool dd_tee_deep_frozen(const ddnet_character_t *chr) {
#if DD_UNIQUE
  (void)chr;
  return false;
#else
  return chr->core.deep_frozen;
#endif
}
static inline bool dd_tee_in_freeze(const ddnet_character_t *chr) {
#if DD_UNIQUE
  (void)chr;
  return false;
#else
  return chr->core.is_in_freeze;
#endif
}
static inline bool dd_tee_jetpack(const ddnet_character_t *chr) {
#if DD_UNIQUE
  (void)chr;
  return false;
#else
  return chr->core.jetpack;
#endif
}

// --- input -------------------------------------------------------------------

// DDNet's input as the engine stores it for every tick of a track: the record
// of this game's input schema. Not the library's ddnet_input_t, which a step
// is made from (ddnet_input_from_record): fire and the weapon are what the
// editor edits, and it carries what only shows (eyes, emote, sitting) and what
// a player does to the world outside of its tee (team, lock, /spec, being
// connected, the teleporter exit). Padded to 16 bytes and 8-byte aligned so
// that copying one is two moves.
//
// Records of older projects have zeros in m_Team, m_PlayerFlags, FLAG_SPEC,
// FLAG_LOCK and FLAG_CONNECTION, which all mean "nothing" at zero.
typedef struct __attribute__((aligned(8))) dd_input {
  int8_t m_Direction; // -1 left, 0, 1 right
  int16_t m_TargetX;  // aim, relative to the tee
  int16_t m_TargetY;
  uint8_t m_Jump;
  // 1 while the fire button is held. A press that holding does not show (a tap
  // between two ticks, pressing again while held) is FLAG_FIRE_PRESS. DDNet's
  // counter of presses and releases is made from the two (ddnet_input_from_record).
  uint8_t m_Fire;
  uint8_t m_Hook;
  uint8_t m_WantedWeapon; // DDNET_WEAPON_*, switched to as soon as the tee has it
  uint8_t m_TeleOut;      // picks the exit of a teleporter with several, see ddnet_character_t::tele_out
  // Unique Race: the side of fastcap the player chooses while it is set (dd_input_fastcap_side). In a
  // byte of its own, so that a project keeps its teams and its sides when it moves between DDNet and
  // Unique Race (each ignores the other's). Zero in older records, which is no side.
  uint8_t m_Side;
  uint16_t m_Flags;       // FLAG_*
  // The ddrace team the player is put into while this is set, as team + 1; 0
  // leaves the team to the physics (spawning, finishing and so on move tees).
  uint8_t m_Team;
  uint8_t m_PlayerFlags; // DDNET_PLAYERFLAG_*, as a DDNet client sends them
} dd_input_t;

enum {
  FLAG_KILL = 1 << 0, // kills the tee before the tick (DDNet's kill command)
  FLAG_SPEC = 1 << 1, // the player wants to be in /spec (ddnet_character_t::spec)
  FLAG_LOCK = 3 << 2, // bits 2..3: DD_LOCK_*, what is done to the lock of the player's team
  FLAG_SIT = 1 << 4,
  FLAG_CONNECTION = 3 << 5, // bits 5..6: DD_CONNECTION_*, whether the player is in the game
  FLAG_EYESTATE = 7 << 7,   // bits 7..9
  FLAG_EMOTE_TRIGGER = 1 << 10,
  FLAG_EMOTE_INDEX = 15 << 11, // bits 11..14
  FLAG_FIRE_PRESS = 1 << 15,   // the fire button went down during the tick (see m_Fire)
};

// What a record does to the lock of the player's team while it says so.
enum { DD_LOCK_KEEP = 0, DD_LOCK_LOCKED, DD_LOCK_UNLOCKED, DD_LOCK_COUNT };
// What a record does to whether the player is in the game while it says so.
enum { DD_CONNECTION_KEEP = 0, DD_CONNECTION_CONNECTED, DD_CONNECTION_DISCONNECTED, DD_CONNECTION_COUNT };

// Not the same ordering as in DDNet, but it matches the texture offsets.
enum { EYE_NORMAL, EYE_ANGRY, EYE_PAIN, EYE_HAPPY, EYE_BLINK, EYE_SURPRISE, NUM_EYES };

#define DD_INPUT_FLAG_ACCESSORS(name, flag)                                                                      \
  static inline int get_flag_##name(const dd_input_t *p) { return p->m_Flags & (flag); }                       \
  static inline void set_flag_##name(dd_input_t *p, int value) {                                               \
    if (value)                                                                                                 \
      p->m_Flags |= (flag);                                                                                    \
    else                                                                                                       \
      p->m_Flags &= (uint16_t)~(flag);                                                                         \
  }
DD_INPUT_FLAG_ACCESSORS(kill, FLAG_KILL)
DD_INPUT_FLAG_ACCESSORS(spec, FLAG_SPEC)
DD_INPUT_FLAG_ACCESSORS(sit, FLAG_SIT)
DD_INPUT_FLAG_ACCESSORS(emote_trigger, FLAG_EMOTE_TRIGGER)
DD_INPUT_FLAG_ACCESSORS(fire_press, FLAG_FIRE_PRESS)
#undef DD_INPUT_FLAG_ACCESSORS

// Whether fire does anything on the tick of a record: held, or pressed during the tick.
static inline bool dd_input_fire_active(const dd_input_t *p) { return (p->m_Fire & 1) || get_flag_fire_press(p); }

static inline uint8_t get_flag_eye_state(const dd_input_t *p) { return (uint8_t)((p->m_Flags & FLAG_EYESTATE) >> 7); }
static inline void set_flag_eye_state(dd_input_t *p, uint8_t state) {
  state &= 0x7;
  p->m_Flags = (uint16_t)((p->m_Flags & ~FLAG_EYESTATE) | (state << 7));
}
static inline uint8_t get_flag_emote_index(const dd_input_t *p) { return (uint8_t)((p->m_Flags & FLAG_EMOTE_INDEX) >> 11); }
static inline void set_flag_emote_index(dd_input_t *p, uint8_t index) {
  index &= 0xF;
  p->m_Flags = (uint16_t)((p->m_Flags & ~FLAG_EMOTE_INDEX) | (index << 11));
}
// DD_LOCK_*: what the record does to the lock of the player's team (an unknown value does nothing).
static inline uint8_t get_flag_lock(const dd_input_t *p) {
  const uint8_t lock = (uint8_t)((p->m_Flags & FLAG_LOCK) >> 2);
  return lock < DD_LOCK_COUNT ? lock : DD_LOCK_KEEP;
}
static inline void set_flag_lock(dd_input_t *p, uint8_t lock) {
  if (lock >= DD_LOCK_COUNT) lock = DD_LOCK_KEEP;
  p->m_Flags = (uint16_t)((p->m_Flags & ~FLAG_LOCK) | (lock << 2));
}
// DD_CONNECTION_*: what the record does to whether the player is in the game.
static inline uint8_t get_flag_connection(const dd_input_t *p) {
  const uint8_t connection = (uint8_t)((p->m_Flags & FLAG_CONNECTION) >> 5);
  return connection < DD_CONNECTION_COUNT ? connection : DD_CONNECTION_KEEP;
}
static inline void set_flag_connection(dd_input_t *p, uint8_t connection) {
  if (connection >= DD_CONNECTION_COUNT) connection = DD_CONNECTION_KEEP;
  p->m_Flags = (uint16_t)((p->m_Flags & ~FLAG_CONNECTION) | (connection << 5));
}
// The team the record puts the player into, or -1 for none.
static inline int dd_input_team(const dd_input_t *p) { return p->m_Team > 0 && p->m_Team <= DDNET_NUM_TEAMS ? p->m_Team - 1 : -1; }
static inline void dd_input_set_team(dd_input_t *p, int team) {
  p->m_Team = (uint8_t)(team >= 0 && team < DDNET_NUM_TEAMS ? team + 1 : 0);
}
// Unique Race: the side of fastcap the record chooses while it is set (the /red
// and /blue commands): 1 red, 2 blue, 0 none.
enum { DD_SIDE_KEEP = 0, DD_SIDE_RED, DD_SIDE_BLUE, DD_SIDE_COUNT };
static inline int dd_input_fastcap_side(const dd_input_t *p) { return p->m_Side < DD_SIDE_COUNT ? p->m_Side : DD_SIDE_KEEP; }

// The player flags the physics acts on: the input of a tee whose player is
// chatting or in the spectator camera does not reach it. The others only show,
// and leaving them out keeps the physics on its fast way for a world of one tee
// that does nothing special.
#define DD_PHYSICS_PLAYER_FLAGS (DDNET_PLAYERFLAG_CHATTING | DDNET_PLAYERFLAG_SPEC_CAM)

// What the physics gets from a record on the tick it is for (the kill, the
// team, the lock, /spec and the connection are done apart, before the tick,
// and the teleporter exit goes to the tee). `out` is the tee's input of the
// tick before, which it replaces.
static inline void ddnet_input_from_record(const dd_input_t *record, ddnet_input_t *out) {
  out->direction = record->m_Direction;
  out->target_x = record->m_TargetX;
  out->target_y = record->m_TargetY;
  out->jump = record->m_Jump != 0;
  // DDNet's fire is a counter of presses and releases, and the physics only
  // asks two things of it: whether the button is down (the low bit) and whether
  // it went down since the tick before (any press between the two counters). A
  // record says just that, held and pressed, and the counter goes on from the
  // tick before's by the steps that give it: one for the button changing, two
  // more for a press the change does not make (down and up again, or up and
  // down again). Going on from the tick before, it cannot jump where inputs made
  // apart meet (a snippet boundary, an edited tick, a take recorded after
  // rewinding), so the tee shoots exactly where the records say.
  const bool was_held = out->fire & 1, held = record->m_Fire & 1;
  int fire_steps = was_held != held;
  if (get_flag_fire_press(record) && !(held && !was_held)) fire_steps += 2;
  out->fire = (out->fire + fire_steps) & 0x3f;
  out->hook = record->m_Hook != 0;
  out->player_flags = record->m_PlayerFlags & DD_PHYSICS_PLAYER_FLAGS;
  // DDNet's client sends the weapon it wants on every tick, as a number one
  // higher (0 is none).
  out->wanted_weapon = record->m_WantedWeapon < DDNET_NUM_WEAPONS ? record->m_WantedWeapon + 1 : 0;
  out->next_weapon = 0;
  out->prev_weapon = 0;
}

// Hands a record to the physics for the tick about to be stepped, for client
// `client_id`: what the player does to the world first (being connected, its
// team and the lock of the team, a kill), then what goes to the physics with
// the tick (/spec, the teleporter exit and the input). `events` picks the event
// build of the physics (see ddnet_step_events). A player that connects spawns
// during the tick after, as on a server.
//
// Unique Race: the side of fastcap (which kills the tee to respawn there), the
// kill, the teleporter exit (one for the world) and the input.
static inline void ddnet_record_apply(ddnet_world_t *world, int client_id, const dd_input_t *record, bool events) {
  if (client_id < 0 || client_id >= dd_client_count(world)) return;
#if defined(DDNET_PHYSICS_HAS_EVENTS)
#define DD_PHYSICS_CALL(name, ...) (events ? ddnet_ev_##name(__VA_ARGS__) : ddnet_##name(__VA_ARGS__))
#else
#define DD_PHYSICS_CALL(name, ...) ((void)events, ddnet_##name(__VA_ARGS__))
#endif
#if DD_UNIQUE
  const int side = dd_input_fastcap_side(record);
  if (side != DD_SIDE_KEEP && side != world->player.fastcap_side) DD_PHYSICS_CALL(player_set_fastcap_side, world, side);
  if (get_flag_kill(record)) DD_PHYSICS_CALL(player_kill, world);
  world->tele_out = record->m_TeleOut;
  ddnet_input_from_record(record, &world->player.input);
#else
  ddnet_player_t *player = &world->players[client_id];
  const uint8_t connection = get_flag_connection(record);
  if (connection == DD_CONNECTION_CONNECTED && !player->active)
    DD_PHYSICS_CALL(player_join, world, client_id);
  else if (connection == DD_CONNECTION_DISCONNECTED && player->active)
    DD_PHYSICS_CALL(player_leave, world, client_id);
  if (player->active) {
    const int team = dd_input_team(record);
    if (team >= 0 && team != player->team) DD_PHYSICS_CALL(player_set_team, world, client_id, team);
    const uint8_t lock = get_flag_lock(record);
    if (lock != DD_LOCK_KEEP && player->team != DDNET_TEAM_FLOCK &&
        ddnet_world_team(world, player->team).locked != (lock == DD_LOCK_LOCKED))
      ddnet_world_lock_team(world, player->team, lock == DD_LOCK_LOCKED);
    if (get_flag_kill(record) && ddnet_world_character(world, client_id)) DD_PHYSICS_CALL(player_kill, world, client_id);
  }
  world->characters[client_id].spec = get_flag_spec(record) != 0;
  world->characters[client_id].tele_out = record->m_TeleOut;
  ddnet_input_from_record(record, &player->input);
#endif
#undef DD_PHYSICS_CALL
}

// --- worlds ------------------------------------------------------------------

// The layout the engine hands around as an opaque pointer. The game itself
// includes this definition too, so plugins and the module cannot drift apart.
struct dd_physics_particle_event;
struct dd_physics_damage_event;
struct dd_physics_sound_event;
struct dd_space_effects;
struct ft_world {
#if DD_UNIQUE
  // Unique Race is played on solo servers: every player races alone, in a world
  // of the physics of its own, and sees the others go. These are those worlds,
  // by client id (NULL for an id no player has now), the tick they are all at
  // (which a world without players keeps too), and whom the effects each raises
  // are of while they are shown (see dd_particles_bind). Read through
  // dd_world_space and the other dd_world_* functions below.
  ddnet_world_t **spaces;              // [client_count] (room for client_room)
  struct dd_space_effects *space_effects; // [client_room]
  int client_count;
  int tick;
  // The map they all play on, which a world without players has too.
  const ddnet_collision_t *collision;
  bool space_events; // while the spaces' effect callbacks are set
#else
  ddnet_world_t core;
#endif
  ft_level *level;
  ft_game *game;
  // Which editor world this belongs to. Every cached copy of one shares it, so
  // effects raised while stepping land in the right particle system.
  int index;
  // The engine's players (its tracks) are clients of the world, in this order:
  // player i is client client_ids[i]. A client keeps its id for as long as it
  // is in the world, whatever players are added or removed before it.
  int player_count;
  uint8_t *client_ids; // [player_count] (room for player_room)
  // By client id, for the client ids the world has had (below
  // dd_world_clients; room for client_room):
  // the record each client held on its last tick: what its tee shows (eyes,
  // emote, sitting), and what a player the engine has no input for goes on
  // holding;
  dd_input_t *inputs;
  // the game tick a client's tee last showed pain (a replayed tee, from its
  // recording): its eyes show it for half a second.
  int *pain_ticks;
  int player_room, client_room;
  bool render_physics_effects;
  // While it steps with its effects shown: the player they are seen as (see
  // dd_team_alpha in the game), -1 for none.
  int effects_viewer;
  struct dd_physics_particle_event *physics_particle_events;
  int physics_particle_event_count;
  int physics_particle_event_capacity;
  struct dd_physics_damage_event *physics_damage_events;
  int physics_damage_event_count;
  int physics_damage_event_capacity;
  struct dd_physics_sound_event *physics_sound_events;
  int physics_sound_event_count;
  int physics_sound_event_capacity;
  // The sounds world_audio last reported from this world; never copied.
  struct ft_audio_sound *audio_sounds;
  int audio_sound_capacity;
  // The recording whose world (projectiles, lasers, chat) this world shows, and
  // the recording tick it shows; NULL when no player replays one.
  const struct ft_recording *replay_recording;
  int replay_tick;
  // The recording's clients this world replays, one bit each (DDNet has up to
  // 128): what belongs to anyone else (their shots, their events) is left out.
  // All bits when every player of the recording is imported.
  uint64_t replay_clients[2];
  // Per player, how replaying a recording left it (see dd_recording.c); NULL
  // until a player of this world first replays one.
  struct dd_replay_slot *replay_slots;
  int replay_slot_count;
  // While the physics steps: effects raised by replayed players are dropped,
  // the recording brings its own.
  bool replay_muted;
  // A step without effects leaves some bullets and laser walls behind until
  // they matter (see ddnet_world_sync); ddnet_world_entities_current brings
  // them up to date before anything reads them.
  bool entities_stale;
};

// --- the worlds of the physics in a world -------------------------------------
//
// A world of DDNet is one world of the physics with all its players in it. One
// of Unique Race has a world of the physics for each player (a solo server), in
// which that player is client 0. These read a world either way, by the
// module's client ids.

// The world of the physics client `client_id` plays in: DDNet's one world, or
// the client's own in Unique Race (NULL for an id no player has).
static inline ddnet_world_t *dd_world_space(const ft_world *world, int client_id) {
#if DD_UNIQUE
  return world && client_id >= 0 && client_id < world->client_count ? world->spaces[client_id] : 0;
#else
  (void)client_id;
  return world ? (ddnet_world_t *)&world->core : 0;
#endif
}

// The map a world is played on.
static inline const ddnet_collision_t *dd_world_collision(const ft_world *world) {
#if DD_UNIQUE
  return world ? world->collision : 0;
#else
  return world ? world->core.collision : 0;
#endif
}

// The id a client has in the world of the physics it plays in (Unique Race: 0).
static inline int dd_space_client(int client_id) {
#if DD_UNIQUE
  (void)client_id;
  return 0;
#else
  return client_id;
#endif
}

// How many client ids the world has had.
static inline int dd_world_clients(const ft_world *world) {
#if DD_UNIQUE
  return world ? world->client_count : 0;
#else
  return world ? world->core.num_clients : 0;
#endif
}

// Whether a client is in the game.
static inline bool dd_world_client_active(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space && client_id >= 0 && dd_client_active(space, dd_space_client(client_id));
}

// The player of a client id (NULL for none).
static inline ddnet_player_t *dd_world_client(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space && client_id >= 0 ? dd_client(space, dd_space_client(client_id)) : 0;
}

// The living tee of a client id, or NULL.
static inline ddnet_character_t *dd_world_character(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space && client_id >= 0 ? dd_client_character(space, dd_space_client(client_id)) : 0;
}

// To be called after writing to the tee of a client id.
static inline void dd_world_character_changed(ft_world *world, int client_id) {
  ddnet_world_t *space = dd_world_space(world, client_id);
  if (space) dd_client_character_changed(space, dd_space_client(client_id));
}

// The ddrace team of a client (Unique Race: 0, which is no team).
static inline int dd_world_team(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space ? dd_client_team(space, dd_space_client(client_id)) : 0;
}

// The race time of a client that finished, in seconds (-1 until then) and in
// whole ticks.
static inline float dd_world_finish_time(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space ? dd_client_finish_time(space, dd_space_client(client_id)) : -1.f;
}
static inline int dd_world_finish_ticks(const ft_world *world, int client_id) {
  const ddnet_world_t *space = dd_world_space(world, client_id);
  return space ? dd_client_finish_ticks(space, dd_space_client(client_id)) : -1;
}

// For going through what is in the worlds of the physics (shots, pickups): how
// many there are, and each with the client it is of (-1 for DDNet's one, which
// is everyone's). dd_world_space_at can be NULL in Unique Race, for a client id
// no player has.
static inline int dd_world_space_count(const ft_world *world) {
#if DD_UNIQUE
  return world ? world->client_count : 0;
#else
  return world ? 1 : 0;
#endif
}
static inline ddnet_world_t *dd_world_space_at(const ft_world *world, int index, int *out_client) {
#if DD_UNIQUE
  if (out_client) *out_client = index;
  return dd_world_space(world, index);
#else
  (void)index;
  if (out_client) *out_client = -1;
  return world ? (ddnet_world_t *)&world->core : 0;
#endif
}

// The client a thing of the world of the physics `space_client` is of, which
// that world calls `owner` (-1 for the world's own): DDNet's client ids are the
// module's, a world of Unique Race has its one client.
static inline int dd_space_owner(int space_client, int owner) {
#if DD_UNIQUE
  return owner >= 0 ? space_client : -1;
#else
  (void)space_client;
  return owner;
#endif
}

// Brings a world's entities up to date for reading them (its projectiles and
// lasers, the entities of each dd_world_space): the optimized physics moves
// bullets and laser walls that no tee is near only when they matter, and a
// world stepped without effects has them where they were then. Changes nothing
// the world will do.
static inline void ddnet_world_entities_current(const ft_world *world) {
  if (!world || !world->entities_stale) return;
  ft_world *current = (ft_world *)world;
  for (int i = 0; i < dd_world_space_count(world); ++i) {
    ddnet_world_t *space = dd_world_space_at(world, i, 0);
    if (space) ddnet_world_sync(space);
  }
  current->entities_stale = false;
}

#if !DD_UNIQUE
// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
// Its entities as they are: see ddnet_world_entities_current. (Unique Race has
// a world of the physics for each player: ddnet_player_world.)
static inline const ddnet_world_t *ddnet_world(const ft_world *world) { return world ? &world->core : 0; }
static inline ddnet_world_t *ddnet_world_mut(ft_world *world) { return world ? &world->core : 0; }
#endif

// The engine's tick of a world, which is its game tick: a world is made at
// tick 0 with its tees at the spawn (ddnet_player_spawn).
static inline int ddnet_engine_tick(const ft_world *world) {
#if DD_UNIQUE
  return world ? world->tick : 0;
#else
  return world ? world->core.tick : 0;
#endif
}

// The client id of the world's player `player`, or -1.
static inline int ddnet_player_client(const ft_world *world, int player) {
  return world && player >= 0 && player < world->player_count ? world->client_ids[player] : -1;
}

// The player of a client id, or -1.
static inline int ddnet_client_player(const ft_world *world, int client_id) {
  if (!world) return -1;
  for (int i = 0; i < world->player_count; ++i)
    if (world->client_ids[i] == client_id) return i;
  return -1;
}

// The world of the physics the world's player `player` plays in (DDNet: the
// world's one), and its client id there (dd_space_client); NULL for none.
static inline ddnet_world_t *ddnet_player_world_mut(ft_world *world, int player) {
  return dd_world_space(world, ddnet_player_client(world, player));
}
static inline const ddnet_world_t *ddnet_player_world(const ft_world *world, int player) {
  return ddnet_player_world_mut((ft_world *)world, player);
}

// The tee of the world's player `player`, or NULL while it has none: before it
// spawns, between dying and respawning, and while it is in /spec.
static inline ddnet_character_t *ddnet_player_character_mut(ft_world *world, int player) {
  const int client_id = ddnet_player_client(world, player);
  return client_id >= 0 ? dd_world_character(world, client_id) : 0;
}
static inline const ddnet_character_t *ddnet_player_character(const ft_world *world, int player) {
  return ddnet_player_character_mut((ft_world *)world, player);
}

// Gives a tee a weapon or takes it away. A weapon only fires with ammo, and a
// DDNet tee has endless ammo (-1) in every weapon it has, as a pickup gives it.
// (Unique Race: but the grenade of sv_health_and_ammo, see dd_set_weapon_got.)
static inline void ddnet_set_weapon_got(ddnet_character_core_t *core, int weapon, bool got) {
  core->weapons[weapon].got = got;
  core->weapons[weapon].ammo = got ? -1 : 0;
}

// The same for a tee of a world, with the ammo a pickup gives in it.
static inline void dd_set_weapon_got(const ddnet_world_t *world, ddnet_character_core_t *core, int weapon, bool got) {
  ddnet_set_weapon_got(core, weapon, got);
#if DD_UNIQUE
  if (got && weapon == DDNET_WEAPON_GRENADE && world->config.sv_health_and_ammo) core->weapons[weapon].ammo = DDNET_GRENADE_MAX_AMMO;
#else
  (void)world;
#endif
}

// Whether a step of the world calls its effect callbacks (core.sound and so
// on), which takes the event build of the physics (ddnet_ev_*).
static inline bool ddnet_step_events(const ft_world *world) {
#if DD_UNIQUE
  return world->space_events;
#else
  return world->core.sound || world->core.particle || world->core.damage_indicator;
#endif
}

// Input records the engine stores are dd_input_t. The engine pads them into a
// fixed-size slot, so read and write through these rather than casting an array.
static inline const dd_input_t *ddnet_input(const void *record) { return (const dd_input_t *)record; }
static inline dd_input_t *ddnet_input_mut(void *record) { return (dd_input_t *)record; }

#ifdef __cplusplus
}
#endif

#endif // DDNET_GAME_PUBLIC_H
