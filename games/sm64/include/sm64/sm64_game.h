#ifndef SM64_GAME_PUBLIC_H
#define SM64_GAME_PUBLIC_H

// What the SM64 game module promises the plugins written for it.
//
// The engine treats ft_world as opaque. A plugin that declares itself
// SM64-specific (plugin_game_id() returning "sm64") may look inside: an
// ft_world is a console of libsm64_physics, which the plugin links and calls
// directly (sm64_physics.h), the way DDNet plugins call ddnet_physics. The
// module and every plugin share one copy of the library, so a world handed
// over by the engine can be stepped, copied (sm64_world_copy into a world of
// the plugin's own) and read by the plugin, on any thread, one world per
// thread at a time.
//
// Nothing here is part of the engine ABI: it is a contract between one game
// and its plugins, and it changes when this game changes.

#include <frametee/game_abi.h>
#include <sm64_physics.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ft_world {
  sm64_world *sim;
  // Frames stepped since power-on.
  int32_t tick;
  // The controller read the last frame stepped with: drawing a world steps a
  // copy of the one before it again (sm64_step_draw).
  uint32_t input;
  // Changes whenever the world does; copies keep it.
  uint64_t revision;
};

// A world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
static inline const sm64_world *sm64_game_world(const ft_world *world) { return world ? world->sim : 0; }
static inline sm64_world *sm64_game_world_mut(ft_world *world) { return world ? world->sim : 0; }

// Input records the engine stores are one controller read, in the layout of
// an .m64 movie sample (mupen64plus BUTTONS): sm64_step's argument.
static inline uint32_t sm64_game_input(const void *record) { return *(const uint32_t *)record; }

#ifdef __cplusplus
}
#endif

#endif
