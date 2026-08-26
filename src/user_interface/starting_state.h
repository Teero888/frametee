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

#endif // STARTING_STATE_H
