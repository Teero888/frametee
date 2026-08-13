#include <engine/int_math.h>
#include "timeline_commands.h"
#include "timeline_interaction.h"
#include "timeline_model.h"
#include <limits.h>
#include <renderer/graphics_backend.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>

// Define a reasonable max number of tracks to handle for batch operations
#define MAX_MODIFIED_TRACKS_PER_COMMAND 256

// Command Struct Definitions

typedef struct {
  undo_command_t base;
  int track_index;
  input_snippet_t snippet_copy;
  int *deactivated_ids;
  int deactivated_count;
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
  int *deactivated_ids;
  int deactivated_count;
} MoveSnippetsCommand;

typedef struct {
  undo_command_t base;
  MoveSnippetInfo *dup_info;
  int count;
  int *created_ids;
  int *deactivated_ids;
  int deactivated_count;
} DuplicateSnippetsCommand;

// Splitting hands both halves the full source buffer and gives them different windows onto it, so
// no inputs are moved anywhere and widening either half brings the other half's ticks back.
typedef struct {
  int track_index;
  int original_snippet_id;
  int new_snippet_id;
  int original_input_count; // window length before the split, restored on undo
  int split_tick;
} SplitInfo;

typedef struct {
  undo_command_t base;
  SplitInfo *infos;
  int count;
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
  int group_index;
  player_info_t player_info;
} AddTrackCommand;

typedef struct {
  undo_command_t base;
  int snippet_id;
  int count;
  int *indices;
  input_record_t *before;
  input_record_t *after;
} EditInputsCommand;

typedef struct {
  undo_command_t base;
  int snippet_id;
  int old_start_tick, old_end_tick;
  int new_start_tick, new_end_tick;
  int *deactivated_ids; // snippets the widened snippet now covers
  int deactivated_count;
} TrimSnippetCommand;

typedef struct {
  char name[MAX_TIMELINE_GROUP_NAME];
  float color[4];
  bool visible;
  bool export_enabled;
  bool prediction_enabled;
  int start_offset;
} TimelineGroupData;

struct timeline_data_snapshot_t {
  TimelineGroupData *groups;
  int group_count;
  player_track_t *tracks;
  int track_count;
  timeline_event_t *events;
  int event_count;
  int current_tick;
  int next_snippet_id;
  int active_group_index;
  int selected_player_track_index;
  int active_snippet_id;
  int context_menu_snippet_id;
  int *selected_snippet_ids;
  int selected_snippet_count;
};

typedef struct {
  undo_command_t base;
  timeline_data_snapshot_t *before;
  timeline_data_snapshot_t *after;
} TimelineDataCommand;

typedef enum {
  TIMELINE_PROPERTY_GROUP_NAME,
  TIMELINE_PROPERTY_GROUP_COLOR,
  TIMELINE_PROPERTY_GROUP_VISIBILITY,
  TIMELINE_PROPERTY_GROUP_START_OFFSET,
  TIMELINE_PROPERTY_GROUP_EXPORT,
  TIMELINE_PROPERTY_TRACK_NAME,
  TIMELINE_PROPERTY_TRACK_EXPORT,
  TIMELINE_PROPERTY_EVENT_GROUP,
} TimelinePropertyKind;

typedef union {
  char text[MAX_TIMELINE_GROUP_NAME];
  float color[4];
  bool boolean;
  int integer;
} TimelinePropertyValue;

typedef struct {
  undo_command_t base;
  TimelinePropertyKind kind;
  int index;
  TimelinePropertyValue before;
  TimelinePropertyValue after;
} TimelinePropertyCommand;

static void clone_track_data(player_track_t *destination, const player_track_t *source) {
  *destination = *source;
  model_rebind_starting_strings(&destination->starting_config);
  destination->snippets = NULL;
  destination->snippet_capacity = source->snippet_count;
  if (source->snippet_count > 0) {
    destination->snippets = calloc((size_t)source->snippet_count, sizeof(*destination->snippets));
    for (int i = 0; i < source->snippet_count; ++i)
      model_snippet_clone(&destination->snippets[i], &source->snippets[i]);
  }

  destination->recording_snippets = NULL;
  destination->recording_snippet_capacity = source->recording_snippet_count;
  if (source->recording_snippet_count > 0) {
    destination->recording_snippets = calloc((size_t)source->recording_snippet_count, sizeof(*destination->recording_snippets));
    for (int i = 0; i < source->recording_snippet_count; ++i)
      model_snippet_clone(&destination->recording_snippets[i], &source->recording_snippets[i]);
  }
}

static void free_track_data(player_track_t *track) {
  for (int i = 0; i < track->snippet_count; ++i) model_free_snippet_inputs(&track->snippets[i]);
  for (int i = 0; i < track->recording_snippet_count; ++i) model_free_snippet_inputs(&track->recording_snippets[i]);
  free(track->snippets);
  free(track->recording_snippets);
  track->snippets = NULL;
  track->recording_snippets = NULL;
}

timeline_data_snapshot_t *commands_capture_timeline_data(const timeline_state_t *ts) {
  if (!ts || ts->recording || ts->group_count <= 0) return NULL;
  timeline_data_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
  if (!snapshot) return NULL;

  snapshot->group_count = ts->group_count;
  snapshot->groups = calloc((size_t)snapshot->group_count, sizeof(*snapshot->groups));
  if (!snapshot->groups) {
    commands_free_timeline_data_snapshot(snapshot);
    return NULL;
  }
  for (int i = 0; i < snapshot->group_count; ++i) {
    timeline_group_t *source = ts->groups[i];
    TimelineGroupData *destination = &snapshot->groups[i];
    memcpy(destination->name, source->name, sizeof(destination->name));
    memcpy(destination->color, source->color, sizeof(destination->color));
    destination->visible = source->visible;
    destination->export_enabled = source->export_enabled;
    destination->prediction_enabled = source->prediction_enabled;
    destination->start_offset = source->start_offset;
  }

  snapshot->track_count = ts->player_track_count;
  if (snapshot->track_count > 0) {
    snapshot->tracks = calloc((size_t)snapshot->track_count, sizeof(*snapshot->tracks));
    if (!snapshot->tracks) {
      commands_free_timeline_data_snapshot(snapshot);
      return NULL;
    }
    for (int i = 0; i < snapshot->track_count; ++i) clone_track_data(&snapshot->tracks[i], &ts->player_tracks[i]);
  }

  snapshot->event_count = ts->event_count;
  if (snapshot->event_count > 0) {
    snapshot->events = malloc(sizeof(*snapshot->events) * (size_t)snapshot->event_count);
    if (!snapshot->events) {
      commands_free_timeline_data_snapshot(snapshot);
      return NULL;
    }
    memcpy(snapshot->events, ts->events, sizeof(*snapshot->events) * (size_t)snapshot->event_count);
  }

  snapshot->selected_snippet_count = ts->selected_snippets.count;
  if (snapshot->selected_snippet_count > 0) {
    snapshot->selected_snippet_ids = malloc(sizeof(*snapshot->selected_snippet_ids) * (size_t)snapshot->selected_snippet_count);
    if (!snapshot->selected_snippet_ids) {
      commands_free_timeline_data_snapshot(snapshot);
      return NULL;
    }
    memcpy(snapshot->selected_snippet_ids, ts->selected_snippets.ids,
           sizeof(*snapshot->selected_snippet_ids) * (size_t)snapshot->selected_snippet_count);
  }

  snapshot->current_tick = ts->current_tick;
  snapshot->next_snippet_id = ts->next_snippet_id;
  snapshot->active_group_index = ts->active_group_index;
  snapshot->selected_player_track_index = ts->selected_player_track_index;
  snapshot->active_snippet_id = ts->active_snippet_id;
  snapshot->context_menu_snippet_id = ts->context_menu_snippet_id;
  return snapshot;
}

void commands_free_timeline_data_snapshot(timeline_data_snapshot_t *snapshot) {
  if (!snapshot) return;
  for (int i = 0; i < snapshot->track_count; ++i) free_track_data(&snapshot->tracks[i]);
  free(snapshot->tracks);
  free(snapshot->groups);
  free(snapshot->events);
  free(snapshot->selected_snippet_ids);
  free(snapshot);
}

static void apply_timeline_data_snapshot(timeline_state_t *ts, const timeline_data_snapshot_t *snapshot) {
  if (!ts || !snapshot || ts->recording || snapshot->group_count <= 0) return;

  interaction_clear_selection(ts);
  ts->drag_state.active = false;
  ts->trim_state.active = false;
  ts->selection_box_active = false;
  ts->pending_single_select_id = -1;
  model_clear_all_recording_buffers(ts);

  for (int i = ts->player_track_count - 1; i >= 0; --i) model_remove_track_logic(ts, i);
  while (ts->group_count > snapshot->group_count) model_remove_group(ts, ts->group_count - 1);
  while (ts->group_count < snapshot->group_count) {
    if (!model_add_group(ts, snapshot->groups[ts->group_count].name)) return;
  }

  for (int i = 0; i < snapshot->group_count; ++i) {
    timeline_group_t *destination = ts->groups[i];
    const TimelineGroupData *source = &snapshot->groups[i];
    memcpy(destination->name, source->name, sizeof(destination->name));
    memcpy(destination->color, source->color, sizeof(destination->color));
    destination->visible = source->visible;
    destination->export_enabled = source->export_enabled;
    destination->prediction_enabled = source->prediction_enabled;
    destination->start_offset = i == 0 ? 0 : source->start_offset;
  }

  for (int group_index = 0; group_index < snapshot->group_count; ++group_index) {
    ts->active_group_index = group_index;
    for (int track_index = 0; track_index < snapshot->track_count; ++track_index) {
      const player_track_t *source = &snapshot->tracks[track_index];
      if (source->group_index != group_index) continue;
      player_track_t *destination = model_add_new_track(ts, 1);
      if (!destination) return;
      player_track_t copy;
      clone_track_data(&copy, source);
      copy.group_index = group_index;
      *destination = copy;
    }
  }

  for (int i = 0; i < ts->player_track_count; ++i)
    if (ts->player_tracks[i].starting_config.enabled) model_apply_starting_config(ts, i);

  free(ts->events);
  ts->events = NULL;
  ts->event_count = snapshot->event_count;
  ts->event_capacity = snapshot->event_count;
  if (snapshot->event_count > 0) {
    ts->events = malloc(sizeof(*ts->events) * (size_t)snapshot->event_count);
    memcpy(ts->events, snapshot->events, sizeof(*ts->events) * (size_t)snapshot->event_count);
  }

  if (snapshot->selected_snippet_count > ts->selected_snippets.capacity) {
    ts->selected_snippets.ids = realloc(ts->selected_snippets.ids,
                                        sizeof(*ts->selected_snippets.ids) * (size_t)snapshot->selected_snippet_count);
    ts->selected_snippets.capacity = snapshot->selected_snippet_count;
  }
  ts->selected_snippets.count = snapshot->selected_snippet_count;
  if (snapshot->selected_snippet_count > 0)
    memcpy(ts->selected_snippets.ids, snapshot->selected_snippet_ids,
           sizeof(*ts->selected_snippets.ids) * (size_t)snapshot->selected_snippet_count);

  ts->current_tick = snapshot->current_tick;
  ts->next_snippet_id = snapshot->next_snippet_id;
  ts->active_group_index = snapshot->active_group_index;
  ts->selected_player_track_index = snapshot->selected_player_track_index;
  ts->active_snippet_id = snapshot->active_snippet_id;
  ts->context_menu_snippet_id = snapshot->context_menu_snippet_id;
  model_recalc_physics(ts, 0);
}

static void undo_timeline_data_change(void *command, void *ts) {
  TimelineDataCommand *c = command;
  apply_timeline_data_snapshot(ts, c->before);
}

static void redo_timeline_data_change(void *command, void *ts) {
  TimelineDataCommand *c = command;
  apply_timeline_data_snapshot(ts, c->after);
}

static void cleanup_timeline_data_change(void *command) {
  TimelineDataCommand *c = command;
  commands_free_timeline_data_snapshot(c->before);
  commands_free_timeline_data_snapshot(c->after);
  free(c);
}

static bool timeline_data_summary_equal(const timeline_data_snapshot_t *a, const timeline_data_snapshot_t *b) {
  if (a->group_count != b->group_count || a->track_count != b->track_count || a->event_count != b->event_count ||
      a->current_tick != b->current_tick || a->next_snippet_id != b->next_snippet_id ||
      a->active_group_index != b->active_group_index ||
      a->selected_player_track_index != b->selected_player_track_index || a->active_snippet_id != b->active_snippet_id ||
      a->context_menu_snippet_id != b->context_menu_snippet_id || a->selected_snippet_count != b->selected_snippet_count)
    return false;
  if (memcmp(a->groups, b->groups, sizeof(*a->groups) * (size_t)a->group_count) != 0) return false;
  if (a->event_count > 0 && memcmp(a->events, b->events, sizeof(*a->events) * (size_t)a->event_count) != 0) return false;
  if (a->selected_snippet_count > 0 &&
      memcmp(a->selected_snippet_ids, b->selected_snippet_ids,
             sizeof(*a->selected_snippet_ids) * (size_t)a->selected_snippet_count) != 0)
    return false;
  return true;
}

undo_command_t *commands_create_timeline_data_change(ui_handler_t *ui, timeline_data_snapshot_t *before, const char *description) {
  if (!ui || !before) return NULL;
  timeline_data_snapshot_t *after = commands_capture_timeline_data(&ui->timeline);
  if (!after) {
    commands_free_timeline_data_snapshot(before);
    return NULL;
  }
  if (timeline_data_summary_equal(before, after)) {
    commands_free_timeline_data_snapshot(before);
    commands_free_timeline_data_snapshot(after);
    return NULL;
  }
  TimelineDataCommand *command = calloc(1, sizeof(*command));
  if (!command) {
    commands_free_timeline_data_snapshot(before);
    commands_free_timeline_data_snapshot(after);
    return NULL;
  }
  snprintf(command->base.description, sizeof(command->base.description), "%s", description);
  command->base.undo = undo_timeline_data_change;
  command->base.redo = redo_timeline_data_change;
  command->base.cleanup = cleanup_timeline_data_change;
  command->before = before;
  command->after = after;
  return &command->base;
}

static void apply_timeline_property(TimelinePropertyCommand *command, timeline_state_t *ts, const TimelinePropertyValue *value) {
  if (!command || !ts || !value) return;
  switch (command->kind) {
  case TIMELINE_PROPERTY_GROUP_NAME:
    if (command->index >= 0 && command->index < ts->group_count)
      memcpy(ts->groups[command->index]->name, value->text, sizeof(ts->groups[command->index]->name));
    break;
  case TIMELINE_PROPERTY_GROUP_COLOR:
    if (command->index >= 0 && command->index < ts->group_count)
      memcpy(ts->groups[command->index]->color, value->color, sizeof(ts->groups[command->index]->color));
    break;
  case TIMELINE_PROPERTY_GROUP_VISIBILITY:
    if (command->index >= 0 && command->index < ts->group_count) ts->groups[command->index]->visible = value->boolean;
    break;
  case TIMELINE_PROPERTY_GROUP_START_OFFSET:
    if (command->index > 0 && command->index < ts->group_count) ts->groups[command->index]->start_offset = value->integer;
    break;
  case TIMELINE_PROPERTY_GROUP_EXPORT:
    if (command->index >= 0 && command->index < ts->group_count) ts->groups[command->index]->export_enabled = value->boolean;
    break;
  case TIMELINE_PROPERTY_TRACK_NAME:
    if (command->index >= 0 && command->index < ts->player_track_count)
      memcpy(ts->player_tracks[command->index].name, value->text, sizeof(ts->player_tracks[command->index].name));
    break;
  case TIMELINE_PROPERTY_TRACK_EXPORT:
    if (command->index >= 0 && command->index < ts->player_track_count)
      ts->player_tracks[command->index].export_enabled = value->boolean;
    break;
  case TIMELINE_PROPERTY_EVENT_GROUP:
    if (command->index >= 0 && command->index < ts->event_count && value->integer >= 0 && value->integer < ts->group_count)
      ts->events[command->index].group_index = value->integer;
    break;
  }
}

static void undo_timeline_property(void *command, void *ts) {
  TimelinePropertyCommand *c = command;
  apply_timeline_property(c, ts, &c->before);
}

static void redo_timeline_property(void *command, void *ts) {
  TimelinePropertyCommand *c = command;
  apply_timeline_property(c, ts, &c->after);
}

static void cleanup_timeline_property(void *command) { free(command); }

static TimelinePropertyCommand *create_timeline_property(TimelinePropertyKind kind, int index, const char *description) {
  TimelinePropertyCommand *command = calloc(1, sizeof(*command));
  if (!command) return NULL;
  snprintf(command->base.description, sizeof(command->base.description), "%s", description);
  command->base.undo = undo_timeline_property;
  command->base.redo = redo_timeline_property;
  command->base.cleanup = cleanup_timeline_property;
  command->kind = kind;
  command->index = index;
  return command;
}

undo_command_t *commands_create_group_name_change(ui_handler_t *ui, int group_index, const char *before) {
  if (!ui || !before || group_index < 0 || group_index >= ui->timeline.group_count ||
      strcmp(before, ui->timeline.groups[group_index]->name) == 0) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_GROUP_NAME, group_index, "Rename Group");
  if (!command) return NULL;
  snprintf(command->before.text, sizeof(command->before.text), "%s", before);
  memcpy(command->after.text, ui->timeline.groups[group_index]->name, sizeof(command->after.text));
  return &command->base;
}

undo_command_t *commands_create_group_color_change(ui_handler_t *ui, int group_index, const float before[4]) {
  if (!ui || !before || group_index < 0 || group_index >= ui->timeline.group_count ||
      memcmp(before, ui->timeline.groups[group_index]->color, sizeof(float) * 4) == 0) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_GROUP_COLOR, group_index, "Change Group Color");
  if (!command) return NULL;
  memcpy(command->before.color, before, sizeof(command->before.color));
  memcpy(command->after.color, ui->timeline.groups[group_index]->color, sizeof(command->after.color));
  return &command->base;
}

undo_command_t *commands_create_group_visibility_change(ui_handler_t *ui, int group_index, bool before) {
  if (!ui || group_index < 0 || group_index >= ui->timeline.group_count || before == ui->timeline.groups[group_index]->visible) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_GROUP_VISIBILITY, group_index, "Toggle Group Visibility");
  if (!command) return NULL;
  command->before.boolean = before;
  command->after.boolean = ui->timeline.groups[group_index]->visible;
  return &command->base;
}

undo_command_t *commands_create_group_start_offset_change(ui_handler_t *ui, int group_index, int before) {
  if (!ui || group_index <= 0 || group_index >= ui->timeline.group_count || before == ui->timeline.groups[group_index]->start_offset) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_GROUP_START_OFFSET, group_index, "Change Group Start Offset");
  if (!command) return NULL;
  command->before.integer = before;
  command->after.integer = ui->timeline.groups[group_index]->start_offset;
  return &command->base;
}

undo_command_t *commands_create_group_export_change(ui_handler_t *ui, int group_index, bool before) {
  if (!ui || group_index < 0 || group_index >= ui->timeline.group_count ||
      before == ui->timeline.groups[group_index]->export_enabled) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_GROUP_EXPORT, group_index, "Toggle Group Export");
  if (!command) return NULL;
  command->before.boolean = before;
  command->after.boolean = ui->timeline.groups[group_index]->export_enabled;
  return &command->base;
}

undo_command_t *commands_create_track_name_change(ui_handler_t *ui, int track_index, const char *before) {
  if (!ui || !before || track_index < 0 || track_index >= ui->timeline.player_track_count ||
      strcmp(before, ui->timeline.player_tracks[track_index].name) == 0) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_TRACK_NAME, track_index, "Rename Track");
  if (!command) return NULL;
  snprintf(command->before.text, sizeof(command->before.text), "%s", before);
  memcpy(command->after.text, ui->timeline.player_tracks[track_index].name, sizeof(command->after.text));
  return &command->base;
}

undo_command_t *commands_create_track_export_change(ui_handler_t *ui, int track_index, bool before) {
  if (!ui || track_index < 0 || track_index >= ui->timeline.player_track_count ||
      before == ui->timeline.player_tracks[track_index].export_enabled) return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_TRACK_EXPORT, track_index, "Toggle Track Export");
  if (!command) return NULL;
  command->before.boolean = before;
  command->after.boolean = ui->timeline.player_tracks[track_index].export_enabled;
  return &command->base;
}

undo_command_t *commands_create_timeline_event_group_change(ui_handler_t *ui, int event_index, int before) {
  if (!ui || event_index < 0 || event_index >= ui->timeline.event_count || before < 0 || before >= ui->timeline.group_count ||
      before == ui->timeline.events[event_index].group_index)
    return NULL;
  TimelinePropertyCommand *command = create_timeline_property(TIMELINE_PROPERTY_EVENT_GROUP, event_index, "Move Timeline Event to Group");
  if (!command) return NULL;
  command->before.integer = before;
  command->after.integer = ui->timeline.events[event_index].group_index;
  return &command->base;
}

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

static void undo_duplicate_snippets(void *cmd, void *ts);
static void redo_duplicate_snippets(void *cmd, void *ts);
static void cleanup_duplicate_snippets_cmd(void *cmd);

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

// shared by every "add a snippet" entry point
static undo_command_t *create_add_snippet_command(timeline_state_t *ts, int track_idx, int start_tick, int duration, const char *description,
                                                  int *out_snippet_id) {
  if (track_idx < 0 || track_idx >= ts->player_track_count || duration <= 0) return NULL;

  player_track_t *track = &ts->player_tracks[track_idx];
  int new_layer = model_find_available_layer(track, start_tick, start_tick + duration, -1);
  if (new_layer == -1) return NULL;

  input_snippet_t snip = {0};
  snip.id = ts->next_snippet_id++;
  snip.start_tick = start_tick;
  snip.end_tick = start_tick + duration;
  snip.is_active = true;
  snip.layer = new_layer;
  snip.input_count = duration;
  snip.inputs = calloc(duration, sizeof(input_record_t));

  if (out_snippet_id) *out_snippet_id = snip.id;

  AddSnippetCommand *cmd = calloc(1, sizeof(AddSnippetCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "%s", description);
  cmd->base.undo = undo_add_snippet;
  cmd->base.redo = redo_add_snippet;
  cmd->base.cleanup = cleanup_add_snippet_cmd;
  cmd->track_index = track_idx;
  model_snippet_clone(&cmd->snippet_copy, &snip);

  // deactivate overlapping snippets
  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet_t *other = &track->snippets[i];
    if (other->is_active && snip.start_tick < other->end_tick && snip.end_tick > other->start_tick) {
      other->is_active = false;
      cmd->deactivated_ids = realloc(cmd->deactivated_ids, sizeof(int) * (cmd->deactivated_count + 1));
      cmd->deactivated_ids[cmd->deactivated_count++] = other->id;
    }
  }

  // Perform the action
  model_insert_snippet_into_track(track, &snip);
  model_compact_layers_for_track(track);
  model_recalc_physics(ts, snip.start_tick);

  return &cmd->base;
}

// Trim Snippet
static void apply_trim(TrimSnippetCommand *c, timeline_state_t *ts, bool redo) {
  int track_idx = -1;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, c->snippet_id, &track_idx);
  if (!snippet || track_idx < 0) return;

  if (redo) model_trim_snippet(ts, snippet, c->new_start_tick, c->new_end_tick);
  else model_trim_snippet(ts, snippet, c->old_start_tick, c->old_end_tick);

  // The covered snippets are only displaced while the trimmed snippet is at its new extent.
  player_track_t *track = &ts->player_tracks[track_idx];
  for (int i = 0; i < c->deactivated_count; ++i) {
    input_snippet_t *other = model_find_snippet_in_track(track, c->deactivated_ids[i]);
    if (other) other->is_active = !redo;
  }

  model_compact_layers_for_track(track);
}

static void undo_trim_snippet(void *cmd, void *ts_void) { apply_trim((TrimSnippetCommand *)cmd, (timeline_state_t *)ts_void, false); }
static void redo_trim_snippet(void *cmd, void *ts_void) { apply_trim((TrimSnippetCommand *)cmd, (timeline_state_t *)ts_void, true); }

static void cleanup_trim_snippet_cmd(void *cmd) {
  TrimSnippetCommand *c = (TrimSnippetCommand *)cmd;
  free(c->deactivated_ids);
  free(c);
}

undo_command_t *commands_create_trim_snippet(ui_handler_t *ui, int snippet_id, int new_start_tick, int new_end_tick) {
  timeline_state_t *ts = &ui->timeline;
  int track_idx = -1;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, &track_idx);
  if (!snippet || track_idx < 0) return NULL;

  int old_start_tick = snippet->start_tick;
  int old_end_tick = snippet->end_tick;
  if (!model_trim_snippet(ts, snippet, new_start_tick, new_end_tick)) return NULL;

  TrimSnippetCommand *cmd = calloc(1, sizeof(TrimSnippetCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Resize Snippet %d", snippet_id);
  cmd->base.undo = undo_trim_snippet;
  cmd->base.redo = redo_trim_snippet;
  cmd->base.cleanup = cleanup_trim_snippet_cmd;
  cmd->snippet_id = snippet_id;
  cmd->old_start_tick = old_start_tick;
  cmd->old_end_tick = old_end_tick;
  cmd->new_start_tick = snippet->start_tick;
  cmd->new_end_tick = snippet->end_tick;

  // Growing over a neighbour displaces it, same rule as adding a snippet on top of one.
  player_track_t *track = &ts->player_tracks[track_idx];
  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet_t *other = &track->snippets[i];
    if (other->id == snippet_id) continue;
    if (other->is_active && snippet->start_tick < other->end_tick && snippet->end_tick > other->start_tick) {
      other->is_active = false;
      cmd->deactivated_ids = realloc(cmd->deactivated_ids, sizeof(int) * (cmd->deactivated_count + 1));
      cmd->deactivated_ids[cmd->deactivated_count++] = other->id;
    }
  }

  model_compact_layers_for_track(track);
  return &cmd->base;
}

undo_command_t *commands_create_add_snippet(ui_handler_t *ui, int track_idx, int start_tick, int duration) {
  return create_add_snippet_command(&ui->timeline, track_idx, start_tick, duration, "Add Snippet", NULL);
}

undo_command_t *commands_create_delete_selected(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count <= 0) return NULL;

  DeleteSnippetsCommand *cmd = calloc(1, sizeof(DeleteSnippetsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Delete %d Snippets", ts->selected_snippets.count);
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

undo_command_t *commands_create_move_snippets(ui_handler_t *ui, const MoveSnippetInfo *infos, int count) {
  if (count <= 0) return NULL;
  timeline_state_t *ts = &ui->timeline;

  MoveSnippetsCommand *cmd = calloc(1, sizeof(MoveSnippetsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Move %d Snippets", count);
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
          cmd->deactivated_ids = realloc(cmd->deactivated_ids, sizeof(int) * (cmd->deactivated_count + 1));
          cmd->deactivated_ids[cmd->deactivated_count++] = other->id;
        }
      }
    }
  }

  // Perform the action
  redo_move_snippets(&cmd->base, ts);

  return &cmd->base;
}

undo_command_t *commands_create_duplicate_snippets(ui_handler_t *ui, const MoveSnippetInfo *infos, int count) {
  if (count <= 0) return NULL;
  timeline_state_t *ts = &ui->timeline;

  DuplicateSnippetsCommand *cmd = calloc(1, sizeof(DuplicateSnippetsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Duplicate %d Snippets", count);
  cmd->base.undo = undo_duplicate_snippets;
  cmd->base.redo = redo_duplicate_snippets;
  cmd->base.cleanup = cleanup_duplicate_snippets_cmd;
  cmd->count = count;
  cmd->dup_info = malloc(sizeof(MoveSnippetInfo) * count);
  memcpy(cmd->dup_info, infos, sizeof(MoveSnippetInfo) * count);
  cmd->created_ids = malloc(sizeof(int) * count);

  // Assign IDs immediately and deactivate overlapping snippets
  for (int i = 0; i < count; ++i) {
    cmd->created_ids[i] = ts->next_snippet_id++;

    // Check overlaps
    const MoveSnippetInfo *info = &infos[i];
    player_track_t *target_track = &ts->player_tracks[info->new_track_index];
    int start = info->new_start_tick;
    // We need to find the duration. The 'infos' struct doesn't have duration directly,
    // but we can find the source snippet to get it.
    input_snippet_t *src_snippet = model_find_snippet_by_id(ts, info->snippet_id, NULL);
    if (src_snippet) {
      int end = start + src_snippet->input_count;
      for (int j = 0; j < target_track->snippet_count; j++) {
        input_snippet_t *other = &target_track->snippets[j];
        if (snippet_id_vector_contains(&ts->selected_snippets, other->id)) continue;
        if (other->is_active && start < other->end_tick && end > other->start_tick) {
          other->is_active = false;
          cmd->deactivated_ids = realloc(cmd->deactivated_ids, sizeof(int) * (cmd->deactivated_count + 1));
          cmd->deactivated_ids[cmd->deactivated_count++] = other->id;
        }
      }
    }
  }

  // Perform the action
  redo_duplicate_snippets(&cmd->base, ts);

  return &cmd->base;
}

undo_command_t *commands_create_split_selected(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count == 0) return NULL;

  MultiSplitCommand *cmd = calloc(1, sizeof(MultiSplitCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Split %d Snippets", ts->selected_snippets.count);
  cmd->base.undo = undo_multi_split;
  cmd->base.redo = redo_multi_split;
  cmd->base.cleanup = cleanup_multi_split_cmd;

  SplitInfo *valid_splits = NULL;
  int split_count = 0;

  // Perform the split and gather data for the command
  for (int i = 0; i < ts->selected_snippets.count; ++i) {
    int sid = ts->selected_snippets.ids[i];
    int track_idx;
    input_snippet_t *snippet = model_find_snippet_by_id(ts, sid, &track_idx);
    if (!snippet) continue;
    int group_index = model_track_group_index(ts, track_idx);
    int split_tick = model_group_playhead_tick(ts, group_index);

    if (split_tick > snippet->start_tick && split_tick < snippet->end_tick) {
      valid_splits = realloc(valid_splits, sizeof(SplitInfo) * (split_count + 1));
      SplitInfo *info = &valid_splits[split_count];

      info->track_index = track_idx;
      info->original_snippet_id = snippet->id;
      info->new_snippet_id = ts->next_snippet_id++;
      info->original_input_count = snippet->input_count;
      info->split_tick = split_tick;
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

undo_command_t *commands_create_merge_selected(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count < 2) return NULL;

  MergeSnippetsCommand *cmd = calloc(1, sizeof(MergeSnippetsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Merge %d Snippets", ts->selected_snippets.count);
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
        // Merging fuses the two windows into one buffer; whatever either side had trimmed away is
        // dropped here, the same way consolidating clips in an editor discards their handles.
        input_record_t *b_window = snippet_window(b);
        model_resize_snippet_inputs(ts, a, old_a_duration + b_duration);
        memcpy(&a->inputs[old_a_duration], b_window, sizeof(input_record_t) * b_duration);

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

undo_command_t *commands_create_remove_track(ui_handler_t *ui, int track_index) {
  timeline_state_t *ts = &ui->timeline;
  if (track_index < 0 || track_index >= ts->player_track_count) return NULL;

  RemoveTrackCommand *cmd = calloc(1, sizeof(RemoveTrackCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Remove Track %d", track_index);
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

  return &cmd->base;
}

// Command Implementations (Undo/Redo/Cleanup)

// Add Snippet
static void undo_add_snippet(void *cmd, void *ts_void) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];

  model_remove_snippet_from_track(ts, track, c->snippet_copy.id);

  for (int i = 0; i < c->deactivated_count; ++i) {
    input_snippet_t *s = model_find_snippet_in_track(track, c->deactivated_ids[i]);
    if (s) s->is_active = true;
  }

  model_compact_layers_for_track(track);
}
static void redo_add_snippet(void *cmd, void *ts_void) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  player_track_t *track = &ts->player_tracks[c->track_index];

  for (int i = 0; i < c->deactivated_count; ++i) {
    input_snippet_t *s = model_find_snippet_in_track(track, c->deactivated_ids[i]);
    if (s) s->is_active = false;
  }

  input_snippet_t new_snip;
  model_snippet_clone(&new_snip, &c->snippet_copy);
  model_insert_snippet_into_track(track, &new_snip);
  model_compact_layers_for_track(track);
}
static void cleanup_add_snippet_cmd(void *cmd) {
  AddSnippetCommand *c = (AddSnippetCommand *)cmd;
  model_free_snippet_inputs(&c->snippet_copy);
  free(c->deactivated_ids);
  free(c);
}

// Snippet Editor Command
undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, const int *indices, int count, const input_record_t *before_states,
                                           const input_record_t *after_states) {
  if (!snippet || !indices || !before_states || !after_states || count <= 0) return NULL;

  EditInputsCommand *cmd = calloc(1, sizeof(EditInputsCommand));
  if (!cmd) return NULL;
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Edit %d Inputs in Snippet %d", count, snippet->id);
  cmd->base.undo = undo_edit_inputs;
  cmd->base.redo = redo_edit_inputs;
  cmd->base.cleanup = cleanup_edit_inputs_cmd;

  // The editor owns its before-state buffer and the after states usually point
  // directly into the snippet.  An undo command can outlive both, so it must
  // retain independent copies of everything it needs.
  cmd->indices = malloc(sizeof(*cmd->indices) * (size_t)count);
  cmd->before = malloc(sizeof(*cmd->before) * (size_t)count);
  cmd->after = malloc(sizeof(*cmd->after) * (size_t)count);
  if (!cmd->indices || !cmd->before || !cmd->after) {
    cleanup_edit_inputs_cmd(cmd);
    return NULL;
  }
  memcpy(cmd->indices, indices, sizeof(*cmd->indices) * (size_t)count);
  memcpy(cmd->before, before_states, sizeof(*cmd->before) * (size_t)count);
  memcpy(cmd->after, after_states, sizeof(*cmd->after) * (size_t)count);
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

  // Reactivate snippets
  for (int i = 0; i < c->deactivated_count; ++i) {
    int track_idx;
    input_snippet_t *s = model_find_snippet_by_id(ts, c->deactivated_ids[i], &track_idx);
    if (s) {
      s->is_active = true;
      if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
    }
  }

  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}
static void redo_move_snippets(void *cmd, void *ts_void) {
  MoveSnippetsCommand *c = (MoveSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};

  // Deactivate snippets
  for (int i = 0; i < c->deactivated_count; ++i) {
    int track_idx;
    input_snippet_t *s = model_find_snippet_by_id(ts, c->deactivated_ids[i], &track_idx);
    if (s) {
      s->is_active = false;
      if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
    }
  }

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
  free(c->deactivated_ids);
  free(c);
}

// Duplicate Snippets
static void undo_duplicate_snippets(void *cmd, void *ts_void) {
  DuplicateSnippetsCommand *c = (DuplicateSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};

  for (int i = 0; i < c->count; ++i) {
    MoveSnippetInfo *info = &c->dup_info[i];
    int track_idx = info->new_track_index;
    if (track_idx < 0 || track_idx >= ts->player_track_count) continue;

    player_track_t *track = &ts->player_tracks[track_idx];
    model_remove_snippet_from_track(ts, track, c->created_ids[i]);
    if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
  }

  // Reactivate snippets
  for (int i = 0; i < c->deactivated_count; ++i) {
    int track_idx;
    input_snippet_t *s = model_find_snippet_by_id(ts, c->deactivated_ids[i], &track_idx);
    if (s) {
      s->is_active = true;
      if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
    }
  }

  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}

static void redo_duplicate_snippets(void *cmd, void *ts_void) {
  DuplicateSnippetsCommand *c = (DuplicateSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  bool modified_tracks[MAX_MODIFIED_TRACKS_PER_COMMAND] = {false};

  // Clear current selection so we can select the newly created duplicates
  interaction_clear_selection(ts);

  // Deactivate snippets
  for (int i = 0; i < c->deactivated_count; ++i) {
    int track_idx;
    input_snippet_t *s = model_find_snippet_by_id(ts, c->deactivated_ids[i], &track_idx);
    if (s) {
      s->is_active = false;
      if (track_idx < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[track_idx] = true;
    }
  }

  for (int i = 0; i < c->count; ++i) {
    MoveSnippetInfo *info = &c->dup_info[i];
    if (info->old_track_index < 0 || info->old_track_index >= ts->player_track_count) continue;
    if (info->new_track_index < 0 || info->new_track_index >= ts->player_track_count) continue;

    player_track_t *src_track = &ts->player_tracks[info->old_track_index];
    player_track_t *dst_track = &ts->player_tracks[info->new_track_index];

    input_snippet_t *src_snippet = model_find_snippet_in_track(src_track, info->snippet_id);
    if (!src_snippet) continue;

    input_snippet_t new_snippet;
    model_snippet_clone(&new_snippet, src_snippet);
    new_snippet.id = c->created_ids[i];
    new_snippet.start_tick = info->new_start_tick;
    new_snippet.end_tick = new_snippet.start_tick + new_snippet.input_count;
    new_snippet.layer = info->new_layer;

    model_insert_snippet_into_track(dst_track, &new_snippet);
    interaction_add_snippet_to_selection(ts, new_snippet.id);

    if (info->new_track_index < MAX_MODIFIED_TRACKS_PER_COMMAND) modified_tracks[info->new_track_index] = true;
  }

  for (int i = 0; i < ts->player_track_count; i++) {
    if (i < MAX_MODIFIED_TRACKS_PER_COMMAND && modified_tracks[i]) model_compact_layers_for_track(&ts->player_tracks[i]);
  }
}

static void cleanup_duplicate_snippets_cmd(void *cmd) {
  DuplicateSnippetsCommand *c = (DuplicateSnippetsCommand *)cmd;
  free(c->dup_info);
  free(c->created_ids);
  free(c->deactivated_ids);
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

    // The left half never lost the right half's ticks, so undoing is just widening it again.
    model_trim_snippet(ts, original, original->start_tick, original->start_tick + info->original_input_count);

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

    // The right half is a copy of the whole source with its window moved past the split point; the
    // left half keeps the same source and only narrows its window.
    int split_index = info->split_tick - original->start_tick;
    input_snippet_t right = {0};
    model_snippet_clone(&right, original);
    right.id = info->new_snippet_id;
    right.start_tick = info->split_tick;
    right.source_offset = original->source_offset + split_index;
    right.input_count = original->input_count - split_index;
    right.end_tick = right.start_tick + right.input_count;

    model_trim_snippet(ts, original, original->start_tick, info->split_tick);
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

    model_snippet_flatten(target);
    target->inputs = realloc(target->inputs, sizeof(input_record_t) * new_duration);
    memcpy(&target->inputs[old_duration], snippet_window(&info->snippet_copy), sizeof(input_record_t) * info->snippet_copy.input_count);
    target->input_count = new_duration;
    target->end_tick = target->start_tick + new_duration;
    target->source_offset = 0;
    target->source_count = new_duration;

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

// Toggle Snippets Active
typedef struct {
  int snippet_id;
  int track_index;
  bool new_state;
  int *overlapping_ids;
  int overlapping_count;
} ToggleSnippetInfo;

typedef struct {
  undo_command_t base;
  ToggleSnippetInfo *infos;
  int count;
} ToggleSnippetsCommand;

static void undo_toggle_snippets(void *cmd, void *ts_void) {
  ToggleSnippetsCommand *c = (ToggleSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;

  for (int i = 0; i < c->count; ++i) {
    ToggleSnippetInfo *info = &c->infos[i];
    if (info->track_index < 0 || info->track_index >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[info->track_index];

    input_snippet_t *target = model_find_snippet_in_track(track, info->snippet_id);
    if (!target) continue;

    if (info->new_state) {
      // It WAS activated, so deactivate it
      target->is_active = false;
      // And reactivate the ones that were overlapped
      for (int j = 0; j < info->overlapping_count; ++j) {
        input_snippet_t *overlap = model_find_snippet_in_track(track, info->overlapping_ids[j]);
        if (overlap) overlap->is_active = true;
      }
    } else {
      // It WAS deactivated, so activate it
      target->is_active = true;
    }
  }
}

static void redo_toggle_snippets(void *cmd, void *ts_void) {
  ToggleSnippetsCommand *c = (ToggleSnippetsCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;

  for (int i = 0; i < c->count; ++i) {
    ToggleSnippetInfo *info = &c->infos[i];
    if (info->track_index < 0 || info->track_index >= ts->player_track_count) continue;
    player_track_t *track = &ts->player_tracks[info->track_index];

    input_snippet_t *target = model_find_snippet_in_track(track, info->snippet_id);
    if (!target) continue;

    if (info->new_state) {
      // Activate target
      target->is_active = true;
      // Deactivate overlaps
      for (int j = 0; j < info->overlapping_count; ++j) {
        input_snippet_t *overlap = model_find_snippet_in_track(track, info->overlapping_ids[j]);
        if (overlap) overlap->is_active = false;
      }
    } else {
      // Deactivate target
      target->is_active = false;
    }
  }
}

static void cleanup_toggle_snippets_cmd(void *cmd) {
  ToggleSnippetsCommand *c = (ToggleSnippetsCommand *)cmd;
  for (int i = 0; i < c->count; ++i) {
    free(c->infos[i].overlapping_ids);
  }
  free(c->infos);
  free(c);
}

undo_command_t *commands_create_toggle_selected_snippets_active(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (ts->selected_snippets.count == 0) return NULL;

  ToggleSnippetsCommand *cmd = calloc(1, sizeof(ToggleSnippetsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Toggle %d Snippets", ts->selected_snippets.count);
  cmd->base.undo = undo_toggle_snippets;
  cmd->base.redo = redo_toggle_snippets;
  cmd->base.cleanup = cleanup_toggle_snippets_cmd;
  cmd->count = ts->selected_snippets.count;
  cmd->infos = calloc(cmd->count, sizeof(ToggleSnippetInfo));

  int earliest_tick = INT_MAX;

  for (int i = 0; i < ts->selected_snippets.count; ++i) {
    int sid = ts->selected_snippets.ids[i];
    int track_idx;
    input_snippet_t *snippet = model_find_snippet_by_id(ts, sid, &track_idx);

    if (!snippet) continue;

    if (snippet->start_tick < earliest_tick) earliest_tick = snippet->start_tick;

    ToggleSnippetInfo *info = &cmd->infos[i];
    info->snippet_id = sid;
    info->track_index = track_idx;
    info->new_state = !snippet->is_active;

    if (info->new_state) {
      // We are activating this snippet. Find overlapping active snippets on the same track.
      player_track_t *track = &ts->player_tracks[track_idx];
      for (int j = 0; j < track->snippet_count; ++j) {
        input_snippet_t *other = &track->snippets[j];
        if (other->id != sid && other->is_active &&
            snippet->start_tick < other->end_tick && snippet->end_tick > other->start_tick) {

          info->overlapping_ids = realloc(info->overlapping_ids, sizeof(int) * (info->overlapping_count + 1));
          info->overlapping_ids[info->overlapping_count++] = other->id;
        }
      }
    }
  }

  // Apply changes immediately (Redo logic)
  redo_toggle_snippets(&cmd->base, ts);

  if (earliest_tick != INT_MAX) {
    model_recalc_physics(ts, earliest_tick);
  }

  return &cmd->base;
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
  model_insert_track_physics(ts, c->track_index);
  if (new_track->starting_config.enabled) model_apply_starting_config(ts, c->track_index);
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

undo_command_t *timeline_api_create_track(ui_handler_t *ui, const player_info_t *info, int *out_track_index) {
  timeline_state_t *ts = &ui->timeline;

  player_track_t *new_track = model_add_new_track(ts, 1);
  if (!new_track) return NULL;
  int new_index = (int)(new_track - ts->player_tracks);

  if (info) new_track->player_info = *info;
  if (out_track_index) *out_track_index = new_index;

  AddTrackCommand *cmd = calloc(1, sizeof(AddTrackCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Add Track");
  cmd->base.undo = undo_add_track;
  cmd->base.redo = redo_add_track;
  cmd->base.cleanup = cleanup_add_track_cmd;
  cmd->track_index = new_index;
  cmd->group_index = new_track->group_index;
  cmd->player_info = new_track->player_info;

  return &cmd->base;
}

static void undo_add_track(void *cmd, void *ts_void) {
  AddTrackCommand *c = (AddTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;
  model_remove_track_logic(ts, c->track_index);
}

static void redo_add_track(void *cmd, void *ts_void) {
  AddTrackCommand *c = (AddTrackCommand *)cmd;
  struct timeline_state *ts = (struct timeline_state *)ts_void;

  int old_active = ts->active_group_index;
  ts->active_group_index = c->group_index;
  player_track_t *new_track = model_add_new_track(ts, 1);
  ts->active_group_index = old_active;
  if (!new_track) return;

  c->track_index = (int)(new_track - ts->player_tracks);
  new_track->player_info = c->player_info;
}

static void cleanup_add_track_cmd(void *cmd) { free(cmd); }

undo_command_t *timeline_api_create_snippet(ui_handler_t *ui, int track_index, int start_tick, int duration, int *out_snippet_id) {
  return create_add_snippet_command(&ui->timeline, track_index, start_tick, duration, "Create Snippet (API)", out_snippet_id);
}

static void apply_input_states(timeline_state_t *ts, int snippet_id, int count, const int *indices, const input_record_t *states) {
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, NULL);
  if (!snippet) return;
  for (int i = 0; i < count; i++) {
    int idx = indices[i];
    if (idx >= 0 && idx < snippet->input_count) {
      snippet_window(snippet)[idx] = states[i];
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

undo_command_t *timeline_api_set_snippet_inputs(ui_handler_t *ui, int snippet_id, int tick_offset, int count, const input_record_t *new_inputs) {
  timeline_state_t *ts = &ui->timeline;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, NULL);
  if (!snippet || !new_inputs || count <= 0 || tick_offset < 0 || tick_offset >= snippet->input_count) return NULL;

  int max_write = imin(count, snippet->input_count - tick_offset);
  if (max_write <= 0) return NULL;

  EditInputsCommand *cmd = calloc(1, sizeof(EditInputsCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Set Snippet Inputs (API)");
  cmd->base.undo = undo_edit_inputs;
  cmd->base.redo = redo_edit_inputs;
  cmd->base.cleanup = cleanup_edit_inputs_cmd;
  cmd->snippet_id = snippet_id;
  cmd->count = max_write;
  cmd->indices = malloc(sizeof(int) * max_write);
  cmd->before = malloc(sizeof(input_record_t) * max_write);
  cmd->after = malloc(sizeof(input_record_t) * max_write);

  input_record_t *window = snippet_window(snippet);
  for (int i = 0; i < max_write; ++i) {
    int idx = tick_offset + i;
    cmd->indices[i] = idx;
    cmd->before[i] = window[idx];
    cmd->after[i] = new_inputs[i];
    window[idx] = new_inputs[i]; // Apply change immediately
  }

  // The world simulated from these inputs is now stale.
  model_recalc_physics(ts, snippet->start_tick + tick_offset);

  return &cmd->base;
}

// Commit Recording
typedef struct {
  int track_index;
  input_snippet_t *snippets;
  int snippet_count;
} TrackState;

typedef struct {
  undo_command_t base;
  TrackState *tracks_before;
  TrackState *tracks_after;
  int count;
} CommitRecordingCommand;

static void free_track_state(TrackState *state) {
  if (state->snippets) {
    for (int i = 0; i < state->snippet_count; i++) {
      model_free_snippet_inputs(&state->snippets[i]);
    }
    free(state->snippets);
  }
}

static void capture_track_state(timeline_state_t *ts, int track_idx, TrackState *out_state) {
  player_track_t *track = &ts->player_tracks[track_idx];
  out_state->track_index = track_idx;
  out_state->snippet_count = track->snippet_count;
  if (track->snippet_count > 0) {
    out_state->snippets = malloc(sizeof(input_snippet_t) * track->snippet_count);
    for (int i = 0; i < track->snippet_count; i++) {
      model_snippet_clone(&out_state->snippets[i], &track->snippets[i]);
    }
  } else {
    out_state->snippets = NULL;
  }
}

static void restore_track_state(timeline_state_t *ts, const TrackState *state) {
  if (state->track_index < 0 || state->track_index >= ts->player_track_count) return;
  player_track_t *track = &ts->player_tracks[state->track_index];

  // Free existing
  for (int i = 0; i < track->snippet_count; i++) {
    model_free_snippet_inputs(&track->snippets[i]);
  }
  free(track->snippets);

  // Restore
  track->snippet_count = state->snippet_count;
  track->snippet_capacity = state->snippet_count;
  if (track->snippet_count > 0) {
    track->snippets = malloc(sizeof(input_snippet_t) * track->snippet_count);
    for (int i = 0; i < track->snippet_count; i++) {
      model_snippet_clone(&track->snippets[i], &state->snippets[i]);
    }
  } else {
    track->snippets = NULL;
  }
  model_compact_layers_for_track(track);
}

static void undo_commit_recording(void *cmd, void *ts_void) {
  CommitRecordingCommand *c = (CommitRecordingCommand *)cmd;
  timeline_state_t *ts = (timeline_state_t *)ts_void;
  for (int i = 0; i < c->count; i++) {
    restore_track_state(ts, &c->tracks_before[i]);
  }
}

static void redo_commit_recording(void *cmd, void *ts_void) {
  CommitRecordingCommand *c = (CommitRecordingCommand *)cmd;
  timeline_state_t *ts = (timeline_state_t *)ts_void;
  for (int i = 0; i < c->count; i++) {
    restore_track_state(ts, &c->tracks_after[i]);
  }
}

static void cleanup_commit_recording_cmd(void *cmd) {
  CommitRecordingCommand *c = (CommitRecordingCommand *)cmd;
  for (int i = 0; i < c->count; i++) {
    free_track_state(&c->tracks_before[i]);
    free_track_state(&c->tracks_after[i]);
  }
  free(c->tracks_before);
  free(c->tracks_after);
  free(c);
}

undo_command_t *commands_create_commit_recording(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;

  // Identify affected tracks
  int affected_count = 0;
  int *affected_indices = NULL;

  for (int i = 0; i < ts->player_track_count; ++i) {
    if (ts->player_tracks[i].recording_snippet_count > 0) {
      affected_indices = realloc(affected_indices, sizeof(int) * (affected_count + 1));
      affected_indices[affected_count++] = i;
    }
  }

  if (affected_count == 0) {
    free(affected_indices);
    return NULL;
  }

  CommitRecordingCommand *cmd = calloc(1, sizeof(CommitRecordingCommand));
  snprintf(cmd->base.description, sizeof(cmd->base.description), "Record Inputs");
  cmd->base.undo = undo_commit_recording;
  cmd->base.redo = redo_commit_recording;
  cmd->base.cleanup = cleanup_commit_recording_cmd;
  cmd->count = affected_count;
  cmd->tracks_before = calloc(affected_count, sizeof(TrackState));
  cmd->tracks_after = calloc(affected_count, sizeof(TrackState));

  // Capture Before State
  for (int i = 0; i < affected_count; i++) {
    capture_track_state(ts, affected_indices[i], &cmd->tracks_before[i]);
  }

  // Perform Merge (Logic moved from interaction)
  for (int i = 0; i < affected_count; ++i) {
    int track_idx = affected_indices[i];
    player_track_t *track = &ts->player_tracks[track_idx];
    for (int j = 0; j < track->recording_snippet_count; ++j) {
      input_snippet_t *rec_snip = &track->recording_snippets[j];
      for (int k = 0; k < rec_snip->input_count; ++k) {
        int tick = rec_snip->start_tick + k;
        model_apply_input_to_main_buffer(ts, track, tick, &snippet_window(rec_snip)[k]);
      }
    }
  }

  // Capture After State
  for (int i = 0; i < affected_count; i++) {
    capture_track_state(ts, affected_indices[i], &cmd->tracks_after[i]);
  }

  free(affected_indices);
  return &cmd->base;
}
