#ifndef STARTING_STATE_H
#define STARTING_STATE_H

// The starting-state editor: what a player is before its first tick.
//
// Which values those are is entirely the game's to say. It publishes them as
// properties flagged FT_PROP_STARTING, and everything here walks that list: a
// checkbox for a bool, a drag for a number, one per axis for a vector. The
// editor stores each as an override on the track and writes them into the
// group's starting world through the game's own property table, so it can offer
// this without knowing what a jetpack or a livery is.

#include <stdbool.h>
#include <types.h>

// Draws the editor inline, at the current ImGui cursor, for one track. Games
// call this through the ABI to place it inside a panel of their own. False when
// there was no editor to draw: no track selected, or a game with nothing to
// override at the start.
bool starting_state_draw(ui_handler_t *ui, int track_index);

// The editor in a window of its own, for games that do not declare
// FT_CAP_HOSTS_STARTING_STATE. One that does has already placed it itself.
void starting_state_render_window(ui_handler_t *ui);

// True while "Pick position" is waiting for a click in the viewport, so the
// viewport can say what it is waiting for.
bool starting_state_is_picking(void);

// Offers a viewport click to that wait. True when it was taken, which is when
// the caller must not also read the click as a selection. Coordinates are world
// units, the space games report positions in. The position is staged like every
// other edit here: it lands on the run when Apply does.
bool starting_state_take_world_click(ui_handler_t *ui, float world_x, float world_y);

// Drops the wait, for Escape or for anything that makes it meaningless.
void starting_state_cancel_pick(void);

// Marks every overridden start in the level itself, in its group's colour, so a
// start that was taken over is visible where it will put the player. Call it
// from the render path, after the game has drawn.
void starting_state_render_markers(ui_handler_t *ui, struct gfx_handler_t *gfx);

#endif // STARTING_STATE_H
