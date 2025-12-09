#ifndef UI_TIMELINE_INTERACTION_H
#define UI_TIMELINE_INTERACTION_H

#include "timeline_types.h"

// Main Interaction Handlers
void interaction_handle_playback_and_shortcuts(timeline_state_t *ts);
void interaction_handle_header(timeline_state_t *ts, ImRect header_bb);
void interaction_handle_timeline_area(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y);
void interaction_handle_context_menu(timeline_state_t *ts);

// Selection Helpers
void interaction_clear_selection(timeline_state_t *ts);
void interaction_add_snippet_to_selection(timeline_state_t *ts, int snippet_id, int track_index);
void interaction_remove_snippet_from_selection(timeline_state_t *ts, int snippet_id);
bool interaction_is_snippet_selected(const timeline_state_t *ts, int snippet_id);
void interaction_select_track(timeline_state_t *ts, int track_index);

// Recording Helpers
void interaction_toggle_recording(timeline_state_t *ts);
void interaction_cancel_recording(timeline_state_t *ts);
void interaction_trim_recording_snippet(timeline_state_t *ts);
void interaction_switch_recording_target(timeline_state_t *ts, int new_track_index);
void interaction_apply_dummy_inputs(ui_handler_t *ui);
void interaction_calculate_drag_destination(timeline_state_t *ts, ImRect timeline_bb, float scroll_y, int *out_snapped_tick, int *out_base_track);
void interaction_update_recording_input(ui_handler_t *ui);

#endif // UI_TIMELINE_INTERACTION_H