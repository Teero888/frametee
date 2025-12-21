#ifndef UI_TIMELINE_MODEL_H
#define UI_TIMELINE_MODEL_H

#include "timeline_types.h"

// Initialization and Cleanup
void model_init(timeline_state_t *ts, ui_handler_t *ui);
void model_cleanup(timeline_state_t *ts);

// Snippet ID Vector Helpers
void snippet_id_vector_init(snippet_id_vector_t *vec);
void snippet_id_vector_free(snippet_id_vector_t *vec);
void snippet_id_vector_add(snippet_id_vector_t *vec, int snippet_id);
bool snippet_id_vector_remove(snippet_id_vector_t *vec, int snippet_id);
bool snippet_id_vector_contains(const snippet_id_vector_t *vec, int snippet_id);

// Finders
input_snippet_t *model_find_snippet_by_id(timeline_state_t *ts, int snippet_id, int *out_track_index);
input_snippet_t *model_find_snippet_in_track(player_track_t *track, int snippet_id);
int model_find_available_layer(const player_track_t *track, int start_tick, int end_tick, int exclude_snippet_id);
int model_get_stack_size_at_tick_range(const player_track_t *track, int start_tick, int end_tick);
int model_get_max_timeline_tick(timeline_state_t *ts);

// Data Modification
void timeline_solve_snippet_layers(input_snippet_t **snippets, int count);
void model_insert_snippet_into_track(player_track_t *track, const input_snippet_t *snippet);
bool model_remove_snippet_from_track(timeline_state_t *ts, player_track_t *track, int snippet_id);
void model_resize_snippet_inputs(timeline_state_t *ts, input_snippet_t *snippet, int new_duration);
void model_snippet_clone(input_snippet_t *dest, const input_snippet_t *src);
void model_free_snippet_inputs(input_snippet_t *snippet);
player_track_t *model_add_new_track(timeline_state_t *ts, physics_handler_t *ph, int num);
void model_remove_track_logic(timeline_state_t *ts, int track_index);
void model_insert_track_physics(timeline_state_t *ts, int track_index);
void model_compact_layers_for_track(player_track_t *track);

// Recording & Merging
void model_apply_input_to_main_buffer(timeline_state_t *ts, player_track_t *track, int tick, const SPlayerInput *input);
void model_clear_all_recording_buffers(timeline_state_t *ts);
void model_insert_snippet_into_recording_track(player_track_t *track, const input_snippet_t *snippet);

// Physics & Playback
void model_recalc_physics(timeline_state_t *ts, int tick);
SPlayerInput model_get_input_at_tick(const timeline_state_t *ts, int track_index, int tick);
void model_advance_tick(timeline_state_t *ts, int steps);
void model_activate_snippet(timeline_state_t *ts, int track_index, int snippet_id_to_activate);
void model_get_world_state_at_tick(timeline_state_t *ts, int tick, SWorldCore *out_world, bool effects);
void model_apply_starting_config(timeline_state_t *ts, int track_index);

#endif // UI_TIMELINE_MODEL_H
