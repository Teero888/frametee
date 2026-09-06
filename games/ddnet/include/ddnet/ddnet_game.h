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

#include <ddnet_physics/gamecore.h>
#include <frametee/game_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

// The layout the engine hands around as an opaque pointer. The game itself
// includes this definition too, so plugins and the module cannot drift apart.
struct dd_physics_particle_event;
struct dd_physics_damage_event;
struct dd_physics_sound_event;
struct ft_world {
  SWorldCore core;
  ft_level *level;
  ft_game *game;
  // Which editor world this belongs to. Every cached copy of one shares it, so
  // effects raised while stepping land in the right particle system.
  int index;
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
};

// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
static inline const SWorldCore *ddnet_world(const ft_world *world) { return world ? &world->core : 0; }
static inline SWorldCore *ddnet_world_mut(ft_world *world) { return world ? &world->core : 0; }

// Input records the engine stores are DDNet's own SPlayerInput. The engine
// pads them into a fixed-size slot, so read and write through these rather than
// casting an array.
static inline const SPlayerInput *ddnet_input(const void *record) { return (const SPlayerInput *)record; }
static inline SPlayerInput *ddnet_input_mut(void *record) { return (SPlayerInput *)record; }

#ifdef __cplusplus
}
#endif

#endif // DDNET_GAME_PUBLIC_H
