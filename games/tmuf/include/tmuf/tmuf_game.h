#ifndef TMUF_GAME_PUBLIC_H
#define TMUF_GAME_PUBLIC_H

// What the TrackMania United Forever module promises its own plugins.
//
// The engine treats ft_world and ft_level as opaque. A plugin that declares
// itself TrackMania-specific with
//
//   FT_API const char *plugin_game_id(void) { return "tmuf"; }
//
// is built for this game and may look inside, which is what this header is
// for. Nothing here is part of the engine ABI: it is a contract between one
// game and the plugins written for it, and it changes when this game changes.
//
// The physics is tmuf_physics (<tmuf_physics/tmuf_physics.h>). A plugin that
// links the library itself can copy and step the worlds it gets from the
// editor on its own: they hold no state outside the library's structs.

#include <tmuf_physics/tmuf_physics.h>
#include <frametee/game_abi.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- input -------------------------------------------------------------------

// The record of this game's input schema for every tick of a track: the game's
// own inputs, as tmuf_input takes them.
typedef struct tm_input {
  uint8_t accelerate;
  uint8_t brake;
  uint8_t respawn;
  // A key event that leaves the input as it was (a second steering key):
  // imported from replays, where it cancels a master jump.
  uint8_t input_event;
  int32_t steer; // -65536 (full left) .. 65536 (full right)
} tm_input;

// --- worlds ------------------------------------------------------------------

// A world of this module starts with its tmuf_world: the car on the track of the
// level, at world->tick ticks from the start. What follows it is the module's
// own (what the race interface and the sound keep).
static inline const tmuf_world *tmuf_game_world(const ft_world *world) { return (const tmuf_world *)world; }

#ifdef __cplusplus
}
#endif

#endif // TMUF_GAME_PUBLIC_H
