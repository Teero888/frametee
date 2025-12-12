#include "timeline_commands.h"
#include <renderer/graphics_backend.h>
#include <user_interface/user_interface.h>
#include "timeline_interaction.h"
#include "timeline_model.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Define a reasonable max number of tracks to handle for batch operations
#define MAX_MODIFIED_TRACKS_PER_COMMAND 256

// Command Struct Definitions

typedef struct {
  undo_command_t base;
  int track_index;
  input_snippet_t snippet_copy;
} AddSnippetCommand;

typedef struct {
  input_snippet_t snippet_copy;
  int track_index;
} DeletedSnippetInfo;

typedef struct {
  undo_command_t base;
  DeletedSnippetInfo *deleted_info;
  int count;
} DeleteSnippetsCommand;

typedef struct {
  undo_command_t base;
  MoveSnippetInfo *move_info;
  int count;
} MoveSnippetsCommand;

typedef struct {
  int track_index;
  int original_snippet_id;
  int new_snippet_id;
  SPlayerInput *moved_inputs;
  int moved_inputs_count;
} SplitInfo;

typedef struct {
  undo_command_t base;
  SplitInfo *infos;
  int count;
  int split_tick;
} MultiSplitCommand;

typedef struct {
  undo_command_t base;
  int track_index;
  int target_snippet_id;
  int original_target_end_tick;
  DeletedSnippetInfo *merged_snippets;
  int merged_snippets_count;
} MergeSnippetsCommand;

typedef struct {
  undo_command_t base;
  int track_index;
  player_track_t track_copy;
} RemoveTrackCommand;

typedef struct {
  undo_command_t base;
  int track_index;
  player_info_t player_info;
} AddTrackCommand;

typedef struct {
  undo_command_t base;
  int snippet_id;
  int count;
  int *indices;
  SPlayerInput *before;
  SPlayerInput *after;
} EditInputsCommand;

// Forward Declarations for Command Logic

static void undo_add_snippet(void *cmd, void *ts);
static void redo_add_snippet(void *cmd, void *ts);
static void cleanup_add_snippet_cmd(void *cmd);

static void undo_delete_snippets(void *cmd, void *ts);
static void redo_delete_snippets(void *cmd, void *ts);
static void cleanup_delete_snippets_cmd(void *cmd);

static void undo_move_snippets(void *cmd, void *ts);
static void redo_move_snippets(void *cmd, void *ts);
static void cleanup_move_snippets_cmd(void *cmd);

static void undo_multi_split(void *cmd, void *ts);
static void redo_multi_split(void *cmd, void *ts);
static void cleanup_multi_split_cmd(void *cmd);

static void undo_merge_snippets(void *cmd, void *ts);
static void redo_merge_snippets(void *cmd, void *ts);
static void cleanup_merge_snippets_cmd(void *cmd);

static void undo_remove_track(void *cmd, void *ts);
static void redo_remove_track(void *cmd, void *ts);
static void cleanup_remove_track_cmd(void *cmd);

static void undo_add_track(void *cmd, void *ts);
static void redo_add_track(void *cmd, void *ts);
static void cleanup_add_track_cmd(void *cmd);

static void undo_edit_inputs(void *cmd, void *ts);
static void redo_edit_inputs(void *cmd, void *ts);
static void cleanup_edit_inputs_cmd(void *cmd);

// Command Creation Functions

undo_command_t *commands_create_add_snippet(struct ui_handler *ui, int track_idx, int start_tick, int duration) {
  timeline_state_t *ts = &ui->timeline;
  if (track_idx < 0 || track_idx >= ts->player_track_count) return NULL;

  player_track_t *track = &ts->player_tracks[track_idx];
  int new_layer = model_find_available_layer(track, start_tick, start_tick + duration, -1);
  if (new_layer == -1) return NULL;

  input_snippet_t snip;
  snip.id = ts->next_snippet_id++;
  snip.start_tick = start_tick;
  snip.end_tick = start_tick + duration;
  snip.is_active = true;
  AddSnippetCommand *cmd = calloc(1, sizeof(AddSnippetCommand));
  cmd->base.undo = undo_add_snippet;
  cmd->base.redo = redo_add_snippet;
  cmd->base.cleanup = cleanup_add_snippet_cmd;
  model_snippet_clone(&cmd->snippet_copy, &snip);

  // Perform the action
  model_insert_snippet_into_track(track, &snip);
  model_compact_layers_for_track(track);

  return &cmd->base;
}

undo_command_t *commands_create_delete_selected(struct ui_handler *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count <= 0) return NULL;

  DeleteSnippetsCommand *cmd = calloc(1, sizeof(DeleteSnippetsCommand));
  cmd->base.undo = undo_delete_snippets;
  cmd->base.redo = redo_delete_snippets;
  cmd->base.cleanup = cleanup_delete_snippets_cmd;
  cmd->count = ts->selected_snippets.count;
  cmd->deleted_info = calloc(cmd->count, sizeof(DeletedSnippetInfo));

  // Gather info BEFORE changing state
  int info_idx = 0;
  for (int i = 0; i < ts->selected_snippets.count; ++i) {
    int sid = ts->selected_snippets.ids[i];
    int track_idx;
    input_snippet_t *snip = model_find_snippet_by_id(ts, sid, &track_idx);
    if (snip) {
      cmd->deleted_info[info_idx].track_index = track_idx;
      model_snippet_clone(&cmd->deleted_info[info_idx].snippet_copy, snip);
      info_idx++;
    }
  }
  cmd->count = info_idx; // Update count in case some snippets weren't found

  // Perform the action (by calling redo)
  redo_delete_snippets(&cmd->base, ts);

  // Clear selection after deletion
  ts->selected_snippets.count = 0;

  return &cmd->base;
}

undo_command_t *commands_create_move_snippets(struct ui_handler *ui, const MoveSnippetInfo *infos, int count) {
  if (count <= 0) return NULL;
  timeline_state_t *ts = &ui->timeline;

  MoveSnippetsCommand *cmd = calloc(1, sizeof(MoveSnippetsCommand));
  cmd->base.undo = undo_move_snippets;
  cmd->base.redo = redo_move_snippets;
  cmd->base.cleanup = cleanup_move_snippets_cmd;
  cmd->count = count;
  cmd->move_info = malloc(sizeof(MoveSnippetInfo) * count);
  memcpy(cmd->move_info, infos, sizeof(MoveSnippetInfo) * count);

  // Deactivate conflicting snippets before moving
  for (int i = 0; i < count; ++i) {
    input_snippet_t *moving_snippet = model_find_snippet_by_id(ts, infos[i].snippet_id, NULL);
    if (moving_snippet && moving_snippet->is_active) {
      player_track_t *target_track = &ts->player_tracks[infos[i].new_track_index];
      for (int j = 0; j < target_track->snippet_count; j++) {
        input_snippet_t *other = &target_track->snippets[j];
        if (snippet_id_vector_contains(&ts->selected_snippets, other->id)) continue;
        if (other->is_active && infos[i].new_start_tick < other->end_tick &&
            (infos[i].new_start_tick + moving_snippet->input_count) > other->start_tick) {
          other->is_active = false;
        }
      }
    }
  }

  // Perform the action
  redo_move_snippets(&cmd->base, ts);

  return &cmd->base;
}

undo_command_t *commands_create_split_selected(struct ui_handler *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count == 0 || ts->current_tick <= 0) return NULL;

  MultiSplitCommand *cmd = calloc(1, sizeof(MultiSplitCommand));
  cmd->base.undo = undo_multi_split;
  cmd->base.redo = redo_multi_split;
  cmd->base.cleanup = cleanup_multi_split_cmd;
  cmd->split_tick = ts->current_tick;

  SplitInfo *valid_splits = NULL;
  int split_count = 0;

  // Perform the split and gather data for the command
  for (int i = 0; i < ts->selected_snippets.count; ++i) {
    int sid = ts->selected_snippets.ids[i];
    int track_idx;
    input_snippet_t *snippet = model_find_snippet_by_id(ts, sid, &track_idx);

    if (snippet && ts->current_tick > snippet->start_tick && ts->current_tick < snippet->end_tick) {
      valid_splits = realloc(valid_splits, sizeof(SplitInfo) * (split_count + 1));
      SplitInfo *info = &valid_splits[split_count];

      int offset = ts->current_tick - snippet->start_tick;
      int right_count = snippet->end_tick - ts->current_tick;

      info->track_index = track_idx;
      info->original_snippet_id = snippet->id;
      info->new_snippet_id = ts->next_snippet_id++;
      info->moved_inputs_count = right_count;
      info->moved_inputs = malloc(sizeof(SPlayerInput) * right_count);
      memcpy(info->moved_inputs, snippet->inputs + offset, sizeof(SPlayerInput) * right_count);
      split_count++;
    }
  }

  if (split_count == 0) {
    free(cmd);
    return NULL;
  }

  cmd->infos = valid_splits;
  cmd->count = split_count;

  // Perform the action
  redo_multi_split(&cmd->base, ts);
  return &cmd->base;
}

static int compare_snippets_by_start_tick(const void *p1, const void *p2) {
  const input_snippet_t *a = *(const input_snippet_t **)p1;
  const input_snippet_t *b = *(const input_snippet_t **)p2;
  return a->start_tick - b->start_tick;
}

undo_command_t *commands_create_merge_selected(struct ui_handler *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count < 2) return NULL;

  MergeSnippetsCommand *cmd = calloc(1, sizeof(MergeSnippetsCommand));
  cmd->base.undo = undo_merge_snippets;
  cmd->base.redo = redo_merge_snippets;
  cmd->base.cleanup = cleanup_merge_snippets_cmd;

  bool merged_something = false;

  // This implementation merges snippets on a per-track basis.
  // For simplicity, this example only creates one command for the first track that has mergeable snippets.
  // A more complex implementation could create a single command that handles merges across multiple tracks.
  for (int ti = 0; ti < ts->player_track_count; ++ti) {
    player_track_t *track = &ts->player_tracks[ti];

    input_snippet_t **candidates = malloc(sizeof(input_snippet_t *) * track->snippet_count);
    int candidate_count = 0;
    for (int i = 0; i < track->snippet_count; ++i) {
      if (snippet_id_vector_contains(&ts->selected_snippets, track->snippets[i].id)) {
        candidates[candidate_count++] = &track->snippets[i];
      }
    }

    if (candidate_count < 2) {
      free(candidates);
      continue;
    }

    qsort(candidates, candidate_count, sizeof(input_snippet_t *), compare_snippets_by_start_tick);

    // We need a list to store the IDs of snippets that get merged away.
    int *ids_to_remove = malloc(sizeof(int) * candidate_count);
    int remove_count = 0;

    for (int i = 0; i < candidate_count - 1; ++i) {
      input_snippet_t *a = candidates[i];
      input_snippet_t *b = candidates[i + 1];

      if (a->end_tick == b->start_tick) { // Adjacent
        if (!merged_something) {          // First merge operation
          cmd->track_index = ti;
          cmd->target_snippet_id = a->id;
          cmd->original_target_end_tick = a->end_tick;
        }

        // Store B for undo
        cmd->merged_snippets_count++;
        cmd->merged_snippets = realloc(cmd->merged_snippets, sizeof(DeletedSnippetInfo) * cmd->merged_snippets_count);
        cmd->merged_snippets[cmd->merged_snippets_count - 1].track_index = ti;
        model_snippet_clone(&cmd->merged_snippets[cmd->merged_snippets_count - 1].snippet_copy, b);

        // Perform the merge on snippet 'a's data. This does not reallocate the track's snippets array.
        int old_a_duration = a->input_count;
        int b_duration = b->input_count;
        model_resize_snippet_inputs(ts, a, old_a_duration + b_duration);
        memcpy(&a->inputs[old_a_duration], b->inputs, sizeof(SPlayerInput) * b_duration);

        // Defer the removal of snippet 'b' by adding its ID to a list.
        ids_to_remove[remove_count++] = b->id;

        // Chain the merge so the next iteration can merge with the now-expanded 'a'
        candidates[i + 1] = a;
        merged_something = true;
      }
    }

    // Now that the merge loop is finished, perform all removals at once.
    // This is safe because we are no longer iterating with the 'candidates' pointers.
    for (int i = 0; i < remove_count; ++i) {
      model_remove_snippet_from_track(ts, track, ids_to_remove[i]);
    }
    free(ids_to_remove);

    free(candidates);
    if (merged_something) {
      model_compact_layers_for_track(track);
      break; // Only handle one track per command
    }
  }

  if (!merged_something) {
    cleanup_merge_snippets_cmd(&cmd->base);
    return NULL;
  }

  return &cmd->base;
}

undo_command_t *commands_create_remove_track(struct ui_handler *ui, int track_index) {
  timeline_state_t *ts = &ui->timeline;
  ph_t *ph = &ui->gfx_handler->physics_handler;
  if (track_index < 0 || track_index >= ts->player_track_count) return NULL;

  RemoveTrackCommand *cmd = calloc(1, sizeof(RemoveTrackCommand));
  cmd->base.undo = undo_remove_track;
  cmd->base.redo = redo_remove_track;
  cmd->base.cleanup = cleanup_remove_track_cmd;
  cmd->track_index = track_index;

  player_track_t *original_track = &ts->player_tracks[track_index];
  cmd->track_copy = *original_track;
  // Then deep-copy the snippets array
  cmd->track_copy.snippets = malloc(sizeof(input_snippet_t) * original_track->snippet_count);
  for (int i = 0; i < original_track->snippet_count; i++) {
    model_snippet_clone(&cmd->track_copy.snippets[i], &original_track->snippets[i]);
  }

  // Perform the action
  redo_remove_track(&cmd->base, ts);
  wc_remove_character(&ph->world, track_index);

  return &cmd->base;
}

// Command Implementations (Undo/Redo/Cleanup)

// Add Snippet
static void undo_add_snippet(void *cmd, void *ts_void) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];
  model_remove_snippet_from_track(ts, track, c->snippet_copy.id);
  model_compact_layers_for_track(track);
}
static void redo_add_snippet(void *cmd, void *ts_void) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];
  input_snippet_t new_snip;
  model_snippet_clone(&new_snip, &c->snippet_copy);
  model_insert_snippet_into_track(track, &new_snip);
  model_compact_layers_for_track(track);
}
static void cleanup_add_snippet_cmd(void *cmd) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  model_free_snippet_inputs(&c->snippet_copy);
  free(c);
}

// Snippet Editor Command
undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, int *indices, int count, SPlayerInput *before_states,
                                           SPlayerInput *after_states) {
  EditInputsCommand *cmd = calloc(1, sizeof(EditInputsCommand));
  cmd->base.undo = undo_edit_inputs;
  cmd->base.redo = redo_edit_inputs;
  cmd->base.cleanup = cleanup_edit_inputs_cmd;

  // We must take ownership of the provided pointers
  cmd->indices = indices;
  cmd->before = before_states;
  cmd->after = after_states;
  cmd->snippet_id = snippet->id;
  cmd->count = count;

  return &cmd->base;
}

// Delete Snippets
static void undo_delete_snippets(void *cmd, void *ts_void) {
  DeleteSnippetsCommand *c = (DeleteSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};

  for (int i = 0; i < c->count; ++i) {
    int track_idx = c->deleted_info[i].track_index;
    if (track_idx < 0 || track_idx >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[track_idx];
    input_snippet_t new_snip;
    model_snippet_clone(&new_snip, &c->deleted_info[i].snippet_copy);
    model_insert_snippet_into_track(track, &new_snip);
    if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
  }
  // Compact all affected tracks once at the end
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void redo_delete_snippets(void *cmd, void *ts_void) {
  DeleteSnippetsCommand *c = (DeleteSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};

  for (int i = 0; i < c->count; ++i) {
    int track_idx = c->deleted_info[i].track_index;
    if (track_idx < 0 || track_idx >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[track_idx];
    model_remove_snippet_from_track(ts, track, c->deleted_info[i].snippet_copy.id);
    if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
  }
  // Compact all affected tracks once at the end
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void cleanup_delete_snippets_cmd(void *cmd) {
  DeleteSnippetsCommand *c = (DeleteSnippetsCommand *)cmd;
  for (int i = 0; i < c->count; ++i) {
    model_free_snippet_inputs(&c->deleted_info[i].snippet_copy);
  }
  free(c->deleted_info);
  free(c);
}

// Move Snippets
static void move_snippet_logic(timeline_state_t *ts, int snippet_id, int from_track_idx, int to_track_idx, int to_start_tick, int to_layer) {
  player_track_t *source_track = &ts->player_tracks[from_track_idx];
  input_snippet_t *snippet_to_move = model_find_snippet_in_track(source_track, snippet_id);
  if (!snippet_to_move) return;

  input_snippet_t snip_copy;
  model_snippet_clone(&snip_copy, snippet_to_move);
  snip_copy.start_tick = to_start_tick;
  snip_copy.end_tick = to_start_tick + snip_copy.input_count;
  snip_copy.layer = to_layer;

  model_remove_snippet_from_track(ts, source_track, snippet_id);
  model_insert_snippet_into_track(&ts->player_tracks[to_track_idx], &snip_copy);
}

static void undo_move_snippets(void *cmd, void *ts_void) {
  MoveSnippetsCommand *c = (MoveSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};
  for (int i = 0; i < c->count; i++) {
    MoveSnippetInfo *info = &c->move_info[i];
    move_snippet_logic(ts, info->snippet_id, info->new_track_index, info->old_track_index, info->old_start_tick, info->old_layer);
    if (info->new_track_index < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[info->new_track_index] = true;
    if (info->old_track_index < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[info->old_track_index] = true;
  }
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void redo_move_snippets(void *cmd, void *ts_void) {
  MoveSnippetsCommand *c = (MoveSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};
  for (int i = 0; i < c->count; i++) {
    MoveSnippetInfo *info = &c->move_info[i];
    move_snippet_logic(ts, info->snippet_id, info->old_track_index, info->new_track_index, info->new_start_tick, info->new_layer);
    if (info->old_track_index < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[info->old_track_index] = true;
    if (info->new_track_index < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[info->new_track_index] = true;
  }
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void cleanup_move_snippets_cmd(void *cmd) {
  MoveSnippetsCommand *c = (MoveSnippetsCommand *)cmd;
  free(c->move_info);
  free(c);
}

// Multi-Split Snippets
static void undo_multi_split(void *cmd, void *ts_void) {
  MultiSplitCommand *c = (MultiSplitCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};
  for (int i = 0; i < c->count; i++) {
    SplitInfo *info = &c->infos[i];
    int track_idx = info->track_index;
    if (track_idx < 0 || track_idx >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[track_idx];
    input_snippet_t *original = model_find_snippet_in_track(track, info->original_snippet_id);
    if (!original) continue;

    int old_duration = original->input_count;
    int new_duration = old_duration + info->moved_inputs_count;
    original->inputs = realloc(original->inputs, sizeof(SPlayerInput) * new_duration);
    memcpy(&original->inputs[old_duration], info->moved_inputs, sizeof(SPlayerInput) * info->moved_inputs_count);
    original->input_count = new_duration;
    original->end_tick = original->start_tick + new_duration;

    model_remove_snippet_from_track(ts, track, info->new_snippet_id);
    if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
  }
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void redo_multi_split(void *cmd, void *ts_void) {
  MultiSplitCommand *c = (MultiSplitCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};
  for (int i = 0; i < c->count; i++) {
    SplitInfo *info = &c->infos[i];
    int track_idx = info->track_index;
    if (track_idx < 0 || track_idx >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[track_idx];
    input_snippet_t *original = model_find_snippet_in_track(track, info->original_snippet_id);
    if (!original) continue;

    input_snippet_t right;
    right.id = info->new_snippet_id;
    right.start_tick = c->split_tick;
    right.input_count = info->moved_inputs_count;
    right.end_tick = right.start_tick + right.input_count;
    right.is_active = original->is_active;
    right.layer = original->layer;
    right.inputs = malloc(sizeof(SPlayerInput) * right.input_count);
    memcpy(right.inputs, info->moved_inputs, sizeof(SPlayerInput) * right.input_count);

    model_resize_snippet_inputs(ts, original, c->split_tick - original->start_tick);
    model_insert_snippet_into_track(track, &right);
    interaction_add_snippet_to_selection(ts, right.id);
    if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
  }
  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void cleanup_multi_split_cmd(void *cmd) {
  MultiSplitCommand *c = (MultiSplitCommand *)cmd;
  for (int i = 0; i < c->count; i++) {
    free(c->infos[i].moved_inputs);
  }
  free(c->infos);
  free(c);
}

// Merge Snippets
static void undo_merge_snippets(void *cmd, void *ts_void) {
  MergeSnippetsCommand *c = (MergeSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];
  input_snippet_t *target = model_find_snippet_in_track(track, c->target_snippet_id);
  if (!target) return;

  model_resize_snippet_inputs(ts, target, c->original_target_end_tick - target->start_tick);

  for (int i = 0; i < c->merged_snippets_count; i++) {
    input_snippet_t new_snip;
    model_snippet_clone(&new_snip, &c->merged_snippets[i].snippet_copy);
    model_insert_snippet_into_track(track, &new_snip);
  }
  model_compact_layers_for_track(track);
}

static void redo_merge_snippets(void *cmd, void *ts_void) {
  MergeSnippetsCommand *c = (MergeSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];

  for (int i = 0; i < c->merged_snippets_count; i++) {
    input_snippet_t *target = model_find_snippet_in_track(track, c->target_snippet_id);
    if (!target) return;

    DeletedSnippetInfo *info = &c->merged_snippets[i];
    int old_duration = target->input_count;
    int new_duration = old_duration + info->snippet_copy.input_count;

    target->inputs = realloc(target->inputs, sizeof(SPlayerInput) * new_duration);
    memcpy(&target->inputs[old_duration], info->snippet_copy.inputs, sizeof(SPlayerInput) * info->snippet_copy.input_count);
    target->input_count = new_duration;
    target->end_tick = target->start_tick + new_duration;

    model_remove_snippet_from_track(ts, track, info->snippet_copy.id);
  }
  model_compact_layers_for_track(track);
}
static void cleanup_merge_snippets_cmd(void *cmd) {
  MergeSnippetsCommand *c = (MergeSnippetsCommand *)cmd;
  if (c->merged_snippets) {
    for (int i = 0; i < c->merged_snippets_count; i++) {
      model_free_snippet_inputs(&c->merged_snippets[i].snippet_copy);
    }
    free(c->merged_snippets);
  }
  free(c);
}

// Remove Track
static void undo_remove_track(void *cmd, void *ts_void) {
  RemoveTrackCommand *c = (RemoveTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  int new_count = ts->player_track_count + 1;
  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * new_count);
  memmove(&ts->player_tracks[c->track_index + 1], &ts->player_tracks[c->track_index],
          (ts->player_track_count - c->track_index) * sizeof(player_track_t));
  player_track_t *new_track = &ts->player_tracks[c->track_index];
  *new_track = c->track_copy;
  new_track->snippets = malloc(sizeof(input_snippet_t) * new_track->snippet_count);
  for (int i = 0; i < new_track->snippet_count; i++) {
    model_snippet_clone(&new_track->snippets[i], &c->track_copy.snippets[i]);
  }
  ts->player_track_count = new_count;
  model_recalc_physics(ts, 0);
}

static void redo_remove_track(void *cmd, void *ts_void) {
  RemoveTrackCommand *c = (RemoveTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  model_remove_track_logic(ts, c->track_index);
}

static void cleanup_remove_track_cmd(void *cmd) {
  RemoveTrackCommand *c = (RemoveTrackCommand *)cmd;
  for (int i = 0; i < c->track_copy.snippet_count; i++) {
    model_free_snippet_inputs(&c->track_copy.snippets[i]);
  }
  free(c->track_copy.snippets);
  free(c);
}

// API Command Implementations

undo_command_t *timeline_api_create_track(struct ui_handler *ui, const player_info_t *info, int *out_track_index) {
  timeline_state_t *ts = &ui->timeline;
  ph_t *ph = &ui->gfx_handler->physics_handler;

  int new_index = ts->player_track_count;
  player_track_t *new_track = model_add_new_track(ts, ph, 1);
  if (!new_track) return NULL;

  if (info) new_track->player_info = *info;
  if (out_track_index) *out_track_index = new_index;

  AddTrackCommand *cmd = calloc(1, sizeof(AddTrackCommand));
  cmd->base.undo = undo_add_track;
  cmd->base.redo = redo_add_track;
  cmd->base.cleanup = cleanup_add_track_cmd;
  cmd->track_index = new_index;
  cmd->player_info = new_track->player_info;

  return &cmd->base;
}

static void undo_add_track(void *cmd, void *ts_void) {
  AddTrackCommand *c = (AddTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  ph_t *ph = &ts->ui->gfx_handler->physics_handler;
  model_remove_track_logic(ts, c->track_index);
  wc_remove_character(&ph->world, c->track_index);
}

static void redo_add_track(void *cmd, void *ts_void) {
  AddTrackCommand *c = (AddTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  ph_t *ph = &ts->ui->gfx_handler->physics_handler;

  player_track_t *new_track = model_add_new_track(ts, ph, 1);
  if (!new_track) return;

  // This logic assumes adding to the end. A more complex undo might need to handle insertion.
  new_track->player_info = c->player_info;
}

static void cleanup_add_track_cmd(void *cmd) { free(cmd); }

undo_command_t *timeline_api_create_snippet(struct ui_handler *ui, int track_index, int start_tick, int duration, int *out_snippet_id) {
  timeline_state_t *ts = &ui->timeline;
  if (track_index < 0 || track_index >= ts->player_track_count || duration <= 0) return NULL;

  player_track_t *track = &ts->player_tracks[track_index];
  int new_layer = model_find_available_layer(track, start_tick, start_tick + duration, -1);
  if (new_layer == -1) return NULL;

  input_snippet_t snippet;
  snippet.id = ts->next_snippet_id++;
  snippet.start_tick = start_tick;
  snippet.end_tick = start_tick + duration;
  snippet.is_active = true;
  snippet.layer = new_layer;
  snippet.input_count = duration;
  snippet.inputs = calloc(duration, sizeof(SPlayerInput));

  if (out_snippet_id) *out_snippet_id = snippet.id;

  AddSnippetCommand *cmd = calloc(1, sizeof(AddSnippetCommand));
  cmd->base.undo = undo_add_snippet;
  cmd->base.redo = redo_add_snippet;
  cmd->base.cleanup = cleanup_add_snippet_cmd;
  model_snippet_clone(&cmd->snippet_copy, &snippet);

  model_insert_snippet_into_track(track, &snippet);
  model_compact_layers_for_track(track);
  return &cmd->base;
}

static void apply_input_states(timeline_state_t *ts, int snippet_id, int count, const int *indices, const SPlayerInput *states) {
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, NULL);
  if (!snippet) return;
  for (int i = 0; i < count; i++) {
    int idx = indices[i];
    if (idx >= 0 && idx < snippet->input_count) {
      snippet->inputs[idx] = states[i];
    }
  }
}

static void undo_edit_inputs(void *cmd, void *ts_void) {
  EditInputsCommand *c = (EditInputsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  apply_input_states(ts, c->snippet_id, c->count, c->indices, c->before);
}
static void redo_edit_inputs(void *cmd, void *ts_void) {
  EditInputsCommand *c = (EditInputsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  apply_input_states(ts, c->snippet_id, c->count, c->indices, c->after);
}
static void cleanup_edit_inputs_cmd(void *cmd) {
  EditInputsCommand *c = (EditInputsCommand *)cmd;
  free(c->indices);
  free(c->before);
  free(c->after);
  free(c);
}

undo_command_t *timeline_api_set_snippet_inputs(struct ui_handler *ui, int snippet_id, int tick_offset, int count, const SPlayerInput *new_inputs) {
  timeline_state_t *ts = &ui->timeline;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, NULL);
  if (!snippet || !new_inputs || count <= 0 || tick_offset < 0 || tick_offset >= snippet->input_count) return NULL;

  int max_write = imin(count, snippet->input_count - tick_offset);
  if (max_write <= 0) return NULL;

  EditInputsCommand *cmd = calloc(1, sizeof(EditInputsCommand));
  cmd->base.undo = undo_edit_inputs;
  cmd->base.redo = redo_edit_inputs;
  cmd->base.cleanup = cleanup_edit_inputs_cmd;
  cmd->snippet_id = snippet_id;
  cmd->count = max_write;
  cmd->indices = malloc(sizeof(int) * max_write);
  cmd->before = malloc(sizeof(SPlayerInput) * max_write);
  cmd->after = malloc(sizeof(SPlayerInput) * max_write);

  for (int i = 0; i < max_write; ++i) {
    int idx = tick_offset + i;
    cmd->indices[i] = idx;
    cmd->before[i] = snippet->inputs[idx];
    cmd->after[i] = new_inputs[i];
    snippet->inputs[idx] = new_inputs[i]; // Apply change immediately
  }

  if (snippet->start_tick + tick_offset <= ts->current_tick) model_recalc_physics(ts, snippet->start_tick + tick_offset);

  return &cmd->base;
}
