#ifndef UI_TIMELINE_MODEL_H
#define UI_TIMELINE_MODEL_H

#include "timeline_types.h"

// Initialization and Cleanup
void model_init(timeline_state_t *ts, ui_handler_t *ui);
void model_cleanup(timeline_state_t *ts);

// Groups
timeline_group_t *model_add_group(timeline_state_t *ts, const char *name);
bool model_remove_group(timeline_state_t *ts, int group_index);
void model_reset_groups_for_level(timeline_state_t *ts);
int model_track_group_index(const timeline_state_t *ts, int track_index);
int model_group_track_count(const timeline_state_t *ts, int group_index);
int model_group_track_index(const timeline_state_t *ts, int group_index, int local_index);
int model_group_local_track_index(const timeline_state_t *ts, int track_index);
int model_group_playhead_tick(const timeline_state_t *ts, int group_index);
// Earliest meaningful shared/global tick. Group-local playheads remain clamped to zero.
int model_get_min_global_tick(const timeline_state_t *ts);
int model_clamp_global_tick_for_group(const timeline_state_t *ts, int group_index, int tick);
void model_set_active_group(timeline_state_t *ts, int group_index);
player_track_t *model_clone_track_to_group(timeline_state_t *ts, int track_index, int group_index, int *out_track_index);
void model_align_group_starts(timeline_state_t *ts);

// Snippet ID Vector Helpers
void snippet_id_vector_init(snippet_id_vector_t *vec);
void snippet_id_vector_free(snippet_id_vector_t *vec);
void snippet_id_vector_add(snippet_id_vector_t *vec, int snippet_id);
bool snippet_id_vector_remove(snippet_id_vector_t *vec, int snippet_id);
bool snippet_id_vector_contains(const snippet_id_vector_t *vec, int snippet_id);

// Source Window Access
// Snippet inputs are addressed through the visible window, never through `inputs` directly, because
// the source buffer can hold trimmed-away ticks on either side. Index 0 is the tick at start_tick.
static inline input_record_t *snippet_window(const input_snippet_t *snippet) { return snippet->inputs + snippet->source_offset; }
// Ticks of retained source sitting outside the window, i.e. how far each edge can still be pulled
// before blank ticks have to be invented.
static inline int snippet_source_before(const input_snippet_t *snippet) { return snippet->source_offset; }
static inline int snippet_source_after(const input_snippet_t *snippet) {
  return snippet->source_count - snippet->source_offset - snippet->input_count;
}
// Repairs source bookkeeping for snippets built by older code paths or loaded from older files.
void model_snippet_normalize(input_snippet_t *snippet);
// Drops the retained ticks outside the window so `inputs` holds exactly what plays. Used by the
// operations that rewrite a snippet's buffer wholesale.
void model_snippet_flatten(input_snippet_t *snippet);
// Moves the window to [new_start_tick, new_end_tick), revealing retained source where it exists and
// padding with blank ticks where it does not.
bool model_trim_snippet(timeline_state_t *ts, input_snippet_t *snippet, int new_start_tick, int new_end_tick);

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
player_track_t *model_add_new_track(timeline_state_t *ts, int num);
// Creates timeline rows for players already present in a group's starting
// world. Fixed-cast games use this because their players exist at world
// creation time and cannot be added through world_add_player.
void model_sync_tracks_to_world(timeline_state_t *ts, int group_index);
void model_remove_track_logic(timeline_state_t *ts, int track_index);
void model_insert_track_physics(timeline_state_t *ts, int track_index);
void model_compact_layers_for_track(player_track_t *track);

// Recording & Merging
void model_apply_input_to_main_buffer(timeline_state_t *ts, player_track_t *track, int tick, const input_record_t *input);
void model_clear_all_recording_buffers(timeline_state_t *ts);
void model_insert_snippet_into_recording_track(player_track_t *track, const input_snippet_t *snippet);

// Physics & Playback
void model_recalc_physics(timeline_state_t *ts, int tick);
input_record_t model_get_input_at_tick(const timeline_state_t *ts, int track_index, int tick);
void model_advance_tick(timeline_state_t *ts, int steps);
void model_activate_snippet(timeline_state_t *ts, int track_index, int snippet_id_to_activate);
// World access. The returned world is borrowed from the group's cache and stays
// valid until the next call for that group, which is enough for a frame's worth
// of rendering and inspection without copying a world per query.
const ft_world *model_world_at_tick(timeline_state_t *ts, int tick);
const ft_world *model_group_world_at_tick(timeline_state_t *ts, int group_index, int tick);
// Both the tick and the one before it, for interpolated rendering.
void model_group_world_pair(timeline_state_t *ts, int group_index, int tick, const ft_world **out_prev, const ft_world **out_cur);

// Index of a player property by id in the active game's table, or -1.
int model_find_player_prop(game_host_t *host, const char *prop_id);
void model_apply_starting_config(timeline_state_t *ts, int track_index);
void model_rebind_starting_strings(starting_config_t *config);

#endif // UI_TIMELINE_MODEL_H
