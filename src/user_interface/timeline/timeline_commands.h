#ifndef UI_TIMELINE_COMMANDS_H
#define UI_TIMELINE_COMMANDS_H

#include "timeline_types.h"

struct undo_command_t;
typedef struct timeline_data_snapshot_t timeline_data_snapshot_t;

typedef struct {
  int snippet_id;
  int old_track_index;
  int new_track_index;
  int old_start_tick;
  int new_start_tick;
  int old_layer;
  int new_layer;
} MoveSnippetInfo;

struct undo_command_t *commands_create_add_snippet(ui_handler_t *ui, int track_idx, int start_tick, int duration);
struct undo_command_t *commands_create_delete_selected(ui_handler_t *ui);
struct undo_command_t *commands_create_split_selected(ui_handler_t *ui);
struct undo_command_t *commands_create_merge_selected(ui_handler_t *ui);
struct undo_command_t *commands_create_move_snippets(ui_handler_t *ui, const MoveSnippetInfo *infos, int count);
struct undo_command_t *commands_create_duplicate_snippets(ui_handler_t *ui, const MoveSnippetInfo *infos, int count);
struct undo_command_t *commands_create_toggle_selected_snippets_active(ui_handler_t *ui);
// Moves one edge of a snippet. Retained source outside the window comes back into view where it
// exists; past that the snippet grows with blank ticks.
struct undo_command_t *commands_create_trim_snippet(ui_handler_t *ui, int snippet_id, int new_start_tick, int new_end_tick);
struct undo_command_t *commands_create_remove_track(ui_handler_t *ui, int track_index);

// Structural group operations can change group/track indices and their owned snippets/events in
// one step. Capture immediately before the operation, then create the command after it succeeds.
// The resulting command owns `before` and restores the complete timeline data on undo/redo.
timeline_data_snapshot_t *commands_capture_timeline_data(const timeline_state_t *ts);
void commands_free_timeline_data_snapshot(timeline_data_snapshot_t *snapshot);
struct undo_command_t *commands_create_timeline_data_change(ui_handler_t *ui, timeline_data_snapshot_t *before,
                                                            const char *description);

// Property commands are created after the widget has applied the new value. `before` is the value
// from the start of the edit (not merely the previous drag frame), so one drag/text edit is one
// undo step.
struct undo_command_t *commands_create_group_name_change(ui_handler_t *ui, int group_index, const char *before);
struct undo_command_t *commands_create_group_color_change(ui_handler_t *ui, int group_index, const float before[4]);
struct undo_command_t *commands_create_group_visibility_change(ui_handler_t *ui, int group_index, bool before);
struct undo_command_t *commands_create_group_start_offset_change(ui_handler_t *ui, int group_index, int before);
// One edit to what a track starts as. Returns NULL when nothing actually
// changed, which is what a drag that ended where it began amounts to.
struct undo_command_t *commands_create_starting_config_change(ui_handler_t *ui, int track_index, const starting_config_t *before,
                                                              const char *description);
struct undo_command_t *commands_create_group_export_change(ui_handler_t *ui, int group_index, bool before);
struct undo_command_t *commands_create_track_name_change(ui_handler_t *ui, int track_index, const char *before);
struct undo_command_t *commands_create_track_export_change(ui_handler_t *ui, int track_index, bool before);
struct undo_command_t *commands_create_timeline_event_group_change(ui_handler_t *ui, int event_index, int before);

// Special command for the snippet editor
struct undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, const int *indices, int count,
                                                  const input_record_t *before_states, const input_record_t *after_states);
// Captures an already-applied change to one snippet's ordered effect stack.
struct undo_command_t *commands_create_input_effects_change(ui_handler_t *ui, int snippet_id,
                                                            const input_effect_t *before, int before_count,
                                                            const char *description);

// API-level commands
struct undo_command_t *timeline_api_create_track(ui_handler_t *ui, const player_profile_t *profile, int *out_track_index);
struct undo_command_t *timeline_api_create_snippet(ui_handler_t *ui, int track_index, int start_tick, int duration, int *out_snippet_id);
struct undo_command_t *timeline_api_set_snippet_inputs(ui_handler_t *ui, int snippet_id, int tick_offset, int count,
                                                       const input_record_t *new_inputs);

struct undo_command_t *commands_create_commit_recording(ui_handler_t *ui);

#endif // UI_TIMELINE_COMMANDS_H
