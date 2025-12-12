#ifndef UI_TIMELINE_H
#define UI_TIMELINE_H

#include "timeline_types.h" // Include the shared types

// The main public API for the timeline component
void timeline_init(struct ui_handler *ui);
void timeline_cleanup(timeline_state_t *ts);
void render_timeline(struct ui_handler *ui);

// Other public functions that might be called from outside
void timeline_switch_recording_target(timeline_state_t *ts, int new_track_index);

#endif // UI_TIMELINE_H
