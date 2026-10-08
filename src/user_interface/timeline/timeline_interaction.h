#ifndef UI_TIMELINE_INTERACTION_H
#define UI_TIMELINE_INTERACTION_H

#include "timeline_types.h"

// Main Interaction Handlers
void interaction_handle_playback_and_shortcuts(timeline_state_t *ts);
void interaction_handle_header(timeline_state_t *ts, ImRect header_bb);
void interaction_set_scrubbing(timeline_state_t *ts, bool scrubbing);
void interaction_handle_timeline_area(timeline_state_t *ts, ImRect timeline_bb);
void interaction_handle_context_menu(timeline_state_t *ts);

// Selection Helpers
void interaction_clear_selection(timeline_state_t *ts);
void interaction_add_snippet_to_selection(timeline_state_t *ts, int snippet_id);
void interaction_remove_snippet_from_selection(timeline_state_t *ts, int snippet_id);
bool interaction_is_snippet_selected(const timeline_state_t *ts, int snippet_id);
void interaction_select_track(timeline_state_t *ts, int track_index);

// Recording Helpers
void interaction_toggle_recording(timeline_state_t *ts);
void interaction_cancel_recording(timeline_state_t *ts);
void interaction_trim_recording_snippet(timeline_state_t *ts, bool only_controlled);
void interaction_switch_recording_target(timeline_state_t *ts, int new_track_index);
void interaction_apply_linked_inputs(ui_handler_t *ui);
// While recording, every linked tee but the controlled one lets go of the keys it holds and holds
// them released. The aim and choices such as the weapon stay.
void interaction_release_linked_keys(timeline_state_t *ts);
// A track driven by its link right now: linked, and not the one being controlled.
bool interaction_track_is_linked(const timeline_state_t *ts, int track_index);
void interaction_calculate_drag_destination(timeline_state_t *ts, ImRect timeline_bb, int *out_snapped_tick, int *out_base_track);
void interaction_update_recording_input(ui_handler_t *ui);
void interaction_update_mouse(timeline_state_t *ts);

input_record_t interaction_predict_input(ui_handler_t *ui, const ft_world *world, int track_idx);

#endif // UI_TIMELINE_INTERACTION_H
