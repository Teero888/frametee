#ifndef SM64_GAME_PUBLIC_H
#define SM64_GAME_PUBLIC_H

// What the Super Mario 64 game module promises its own plugins.
//
// The engine treats ft_world and ft_level as opaque, and rightly so. A plugin
// that declares itself SM64-specific with
//
//   FT_API const char *plugin_game_id(void) { return "sm64"; }
//
// is however built for this game and may look inside, which is what this header
// is for. Nothing here is part of the engine ABI: it is a contract between one
// game and the plugins written for it, and it changes when this game changes.

#include <frametee/game_abi.h>
#include <sm64/sm64_physics.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SM64 simulation runs at 30 ticks per second.
enum {
  SM64_TICKS_PER_SECOND = 30,
  SM64_MARIO_MAX_HEALTH = 0x880,
};

typedef struct sm64_camera {
  float eye[3];
  float target[3];
  float up[3];
  float view_proj[16];
  float fov_y;
  float near_z;
  float far_z;
} sm64_camera;

typedef struct sm64_level sm64_level;
typedef struct sm64_world sm64_world;

struct ft_level {
  sm64_level *handle;
};

// The layout the engine hands around as an opaque pointer.
// Both the game module and plugins include this definition, so plugins can
// read Mario's simulation state directly from the world pointer without
// going through graphics or C bridge overhead.
struct ft_world {
  sm64_world *handle;
  bool failed;
  int32_t index; // Editor world index, or -1 for scratch/prediction worlds
  sm64_view view;
  // Cold operation: creates an independent native image and replays this state.
  sm64_physics *(*physics_create)(const sm64_world *, char *, size_t);
};

// Opens an isolated native image at this world's state. The returned object
// owns its runtime and can be stepped concurrently with the editor world.
static inline sm64_physics *sm64_world_physics(const struct ft_world *world,
                                               char *error, size_t error_size) {
  return world && world->physics_create ? world->physics_create(world->handle, error, error_size) : NULL;
}

// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
static inline const sm64_view *sm64_world_view(const ft_world *world) {
  return world ? &world->view : NULL;
}

static inline sm64_world *sm64_world_handle(const ft_world *world) {
  return world ? world->handle : NULL;
}

// Input records the engine stores are the module's own sm64_input. The engine
// pads them into a fixed-size slot, so read and write through these rather than
// casting an array.
static inline const sm64_input *sm64_input_record(const void *record) {
  return (const sm64_input *)record;
}

static inline sm64_input *sm64_input_record_mut(void *record) {
  return (sm64_input *)record;
}

#ifdef __cplusplus
}
#endif

#endif // SM64_GAME_PUBLIC_H
