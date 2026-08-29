#ifndef ENGINE_API_H
#define ENGINE_API_H

// Construction of the service table handed to game modules. The table itself is
// declared in <frametee/game_abi.h>; this header only exposes the few engine
// entry points needed to build and feed it.

#include "game_host.h"
#include <types.h>

// Builds the singleton service table around `handler`. The returned pointer
// stays valid for the process lifetime.
const ft_engine_api *engine_api_init(struct gfx_handler_t *handler);

// Fills in the read-only engine snapshot passed to render and UI callbacks.
void engine_api_fill_state(ft_engine_state *out);

// The camera as the ABI describes it, including the visible world rectangle.
void engine_api_camera_get(ft_camera *out);

// Publishes the playfield extent the renderer normalizes world coordinates by.
// Called when a level finishes loading, from the level's own reported bounds.
void engine_api_set_world_extent(struct gfx_handler_t *handler, float width, float height);

// Suppresses game-owned presentation effects around read-only timeline scans.
// Returns the previous state so callers can restore nested scopes safely.
bool engine_api_set_presentation_effects(bool enabled);

// Reports the current scope without changing it. Timeline cache selection uses
// this to distinguish a visible replay from a read-only simulation.
bool engine_api_presentation_effects_enabled(void);

#endif // ENGINE_API_H
