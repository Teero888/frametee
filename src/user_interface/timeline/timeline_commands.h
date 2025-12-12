#ifndef UI_TIMELINE_COMMANDS_H
#define UI_TIMELINE_COMMANDS_H

#include "timeline_types.h"

struct undo_command_t;

typedef struct {
  int snippet_id;
  int old_track_index;
  int new_track_index;
  int old_start_tick;
  int new_start_tick;
  int old_layer;
  int new_layer;
} MoveSnippetInfo;

struct undo_command_t *commands_create_add_snippet(struct ui_handler *ui, int track_idx, int start_tick, int duration);
struct undo_command_t *commands_create_delete_selected(struct ui_handler *ui);
struct undo_command_t *commands_create_split_selected(struct ui_handler *ui);
struct undo_command_t *commands_create_merge_selected(struct ui_handler *ui);
struct undo_command_t *commands_create_move_snippets(struct ui_handler *ui, const MoveSnippetInfo *infos, int count);
struct undo_command_t *commands_create_remove_track(struct ui_handler *ui, int track_index);

// Special command for the snippet editor
struct undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, int *indices, int count, SPlayerInput *before_states,
                                                  SPlayerInput *after_states);

// API-level commands
struct undo_command_t *timeline_api_create_track(struct ui_handler *ui, const player_info_t *info, int *out_track_index);
struct undo_command_t *timeline_api_create_snippet(struct ui_handler *ui, int track_index, int start_tick, int duration, int *out_snippet_id);
struct undo_command_t *timeline_api_set_snippet_inputs(struct ui_handler *ui, int snippet_id, int tick_offset, int count,
                                                       const SPlayerInput *new_inputs);

#endif // UI_TIMELINE_COMMANDS_H
