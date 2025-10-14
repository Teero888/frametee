#ifndef UI_TIMELINE_COMMANDS_H
#define UI_TIMELINE_COMMANDS_H

#include "timeline_types.h"

typedef struct {
  int snippet_id;
  int old_track_index;
  int new_track_index;
  int old_start_tick;
  int new_start_tick;
  int old_layer;
  int new_layer;
} MoveSnippetInfo;

undo_command_t *commands_create_add_snippet(ui_handler_t *ui, int track_idx, int start_tick, int duration);
undo_command_t *commands_create_delete_selected(ui_handler_t *ui);
undo_command_t *commands_create_split_selected(ui_handler_t *ui);
undo_command_t *commands_create_merge_selected(ui_handler_t *ui);
undo_command_t *commands_create_move_snippets(ui_handler_t *ui, const MoveSnippetInfo *infos, int count);
undo_command_t *commands_create_remove_track(ui_handler_t *ui, int track_index);

// Special command for the snippet editor
undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, int *indices, int count, SPlayerInput *before_states,
                                           SPlayerInput *after_states);

// API-level commands
undo_command_t *timeline_api_create_track(ui_handler_t *ui, const player_info_t *info, int *out_track_index);
undo_command_t *timeline_api_create_snippet(ui_handler_t *ui, int track_index, int start_tick, int duration, int *out_snippet_id);
undo_command_t *timeline_api_set_snippet_inputs(ui_handler_t *ui, int snippet_id, int tick_offset, int count, const SPlayerInput *new_inputs);

#endif // UI_TIMELINE_COMMANDS_H
