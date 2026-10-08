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

#include <ddnet_physics/ddnet_physics.h>
#include <frametee/game_abi.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- input -------------------------------------------------------------------

// DDNet's input as the engine stores it for every tick of a track: the record
// of this game's input schema. Not the library's ddnet_input_t, which a step
// is made from (ddnet_input_from_record): fire and the weapon are what the
// editor edits, and it carries what only shows (eyes, emote, sitting) and the
// teleporter exit. Padded to 16 bytes and 8-byte aligned so that copying one is
// two moves.
typedef struct __attribute__((aligned(8))) dd_input {
  int8_t m_Direction; // -1 left, 0, 1 right
  int16_t m_TargetX;  // aim, relative to the tee
  int16_t m_TargetY;
  uint8_t m_Jump;
  // A counter like DDNet's: the low bit is "held", and every press and release
  // counts it up (the editor toggles the low bit).
  uint8_t m_Fire;
  uint8_t m_Hook;
  uint8_t m_WantedWeapon; // DDNET_WEAPON_*, switched to as soon as the tee has it
  uint8_t m_TeleOut;      // picks the exit of a teleporter with several, see ddnet_character_t::tele_out
  uint16_t m_Flags;       // FLAG_*
  uint8_t m_aPadding[2];
} dd_input_t;

enum {
  FLAG_KILL = 1 << 0, // kills the tee before the tick (DDNet's kill command)
  FLAG_SPEC = 1 << 1,
  FLAG_HOOKLINE = 1 << 2,
  FLAG_CHATBUBBLE = 1 << 3,
  FLAG_SIT = 1 << 4,
  FLAG_CONNECT = 1 << 5,
  FLAG_DISCONNECT = 1 << 6,
  FLAG_EYESTATE = 7 << 7, // bits 7..9
  FLAG_EMOTE_TRIGGER = 1 << 10,
  FLAG_EMOTE_INDEX = 15 << 11, // bits 11..14
};

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
DD_INPUT_FLAG_ACCESSORS(hookline, FLAG_HOOKLINE)
DD_INPUT_FLAG_ACCESSORS(chatbubble, FLAG_CHATBUBBLE)
DD_INPUT_FLAG_ACCESSORS(sit, FLAG_SIT)
DD_INPUT_FLAG_ACCESSORS(connect, FLAG_CONNECT)
DD_INPUT_FLAG_ACCESSORS(disconnect, FLAG_DISCONNECT)
DD_INPUT_FLAG_ACCESSORS(emote_trigger, FLAG_EMOTE_TRIGGER)
#undef DD_INPUT_FLAG_ACCESSORS

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

// What the physics gets from a record on the tick it is for (the kill flag is
// done apart, before the tick, and the teleporter exit goes to the tee). `out`
// is the tee's input of the tick before, which it replaces.
static inline void ddnet_input_from_record(const dd_input_t *record, ddnet_input_t *out) {
  out->direction = record->m_Direction;
  out->target_x = record->m_TargetX;
  out->target_y = record->m_TargetY;
  out->jump = record->m_Jump != 0;
  // Fire is a counter whose low bit is the button, and the physics counts
  // every value between two ticks' counters as presses and releases. A record
  // only says whether the button is down, as its lane shows: where inputs made
  // apart meet (a take recorded on after rewinding, a snippet boundary, an
  // edited tick) its counter can jump or drop, which would count as presses.
  // So the counter goes on from the tick before's, one step when the button
  // changes, and the tee shoots when it goes down and only then.
  if ((out->fire & 1) != (record->m_Fire & 1)) out->fire = (out->fire + 1) & 0x3f;
  out->hook = record->m_Hook != 0;
  out->player_flags = 0;
  // DDNet's client sends the weapon it wants on every tick, as a number one
  // higher (0 is none).
  out->wanted_weapon = record->m_WantedWeapon < DDNET_NUM_WEAPONS ? record->m_WantedWeapon + 1 : 0;
  out->next_weapon = 0;
  out->prev_weapon = 0;
}

// --- worlds ------------------------------------------------------------------

// The layout the engine hands around as an opaque pointer. The game itself
// includes this definition too, so plugins and the module cannot drift apart.
struct dd_physics_particle_event;
struct dd_physics_damage_event;
struct dd_physics_sound_event;
struct ft_world {
  ddnet_world_t core;
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
  // core.num_clients; room for client_room):
  // the record each client held on its last tick: what its tee shows (eyes,
  // emote, sitting), and what a player the engine has no input for goes on
  // holding;
  dd_input_t *inputs;
  // the game tick a client's tee last showed pain (a replayed tee, from its
  // recording): its eyes show it for half a second.
  int *pain_ticks;
  int player_room, client_room;
  bool render_physics_effects;
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
  // The recording's clients this world replays, one bit each: what belongs to
  // anyone else (their shots, their events) is left out. All bits when every
  // player of the recording is imported.
  uint64_t replay_clients;
  // Per player, how replaying a recording left it (see dd_recording.c); NULL
  // until a player of this world first replays one.
  struct dd_replay_slot *replay_slots;
  int replay_slot_count;
  // While the physics steps: effects raised by replayed players are dropped,
  // the recording brings its own.
  bool replay_muted;
};

// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
static inline const ddnet_world_t *ddnet_world(const ft_world *world) { return world ? &world->core : 0; }
static inline ddnet_world_t *ddnet_world_mut(ft_world *world) { return world ? &world->core : 0; }

// The engine's tick of a world, which is its game tick: a world is made at
// tick 0 with its tees at the spawn (ddnet_player_spawn).
static inline int ddnet_engine_tick(const ft_world *world) { return world ? world->core.tick : 0; }

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

// The tee of the world's player `player`, or NULL while it has none: before it
// spawns, between dying and respawning, and while it is in /spec.
static inline ddnet_character_t *ddnet_player_character_mut(ft_world *world, int player) {
  const int client_id = ddnet_player_client(world, player);
  return client_id >= 0 ? ddnet_world_character(&world->core, client_id) : 0;
}
static inline const ddnet_character_t *ddnet_player_character(const ft_world *world, int player) {
  return ddnet_player_character_mut((ft_world *)world, player);
}

// Whether a step of the world calls its effect callbacks (core.sound and so
// on), which takes the event build of the physics (ddnet_ev_*).
static inline bool ddnet_step_events(const ft_world *world) {
  return world->core.sound || world->core.particle || world->core.damage_indicator;
}

// Input records the engine stores are dd_input_t. The engine pads them into a
// fixed-size slot, so read and write through these rather than casting an array.
static inline const dd_input_t *ddnet_input(const void *record) { return (const dd_input_t *)record; }
static inline dd_input_t *ddnet_input_mut(void *record) { return (dd_input_t *)record; }

#ifdef __cplusplus
}
#endif

#endif // DDNET_GAME_PUBLIC_H
