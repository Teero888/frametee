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

// The layouts the engine hands around as opaque pointers. Keep in step with
// games/ddnet/dd_internal.h.
struct ft_world {
  SWorldCore core;
  ft_level *level;
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
