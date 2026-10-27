#include "plugin_api.h"
#include <stddef.h>
#include <stdio.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <stdlib.h>
#include <string.h>
#include <system/save.h>
#include <user_interface/timeline/timeline_commands.h>
#include <user_interface/timeline/timeline_interaction.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline/timeline_recordings.h>
#include <user_interface/timeline_events.h>
#include <user_interface/undo_redo.h>
#include <user_interface/user_interface.h>

// A project moved from DDNet to Unique Race and back (switch_project_backend) is the project it was:
// it saves to the same bytes, and its undo history holds the same steps, which undo and redo it to
// the same projects as they did before. A global plugin, so that it stays loaded while the games
// change, run headless on a DDNet level; it reaches into the editor itself, whose symbols the
// executable exports (-rdynamic), to build the project and to move it. With --recording <demo>, the
// recording that goes along is that demo. With --round-trip, it only moves the project it is given
// (--project) to the other backend and back, and compares what that saves to.

FT_PLUGIN_ABI_EXPORT()

static tas_context_t *g_context;
static const tas_api_t *g_api;

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  if (!context->is_headless) return NULL;
  g_context = context;
  g_api = api;
  return context;
}

FT_API void plugin_shutdown(void *data) { (void)data; }

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "backend switch test: %s failed (line %d)\n", #condition, __LINE__); \
      goto done;                                                             \
    }                                                                        \
  } while (0)

typedef struct file_t {
  unsigned char *data;
  size_t size;
} file_t;

// The project as it saves now.
static bool snapshot(ui_handler_t *ui, file_t *out) {
  static const char *path = "backend_switch_test.tasp";
  out->data = NULL;
  out->size = 0;
  if (!save_project_snapshot(ui, path)) return false;
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  bool ok = fseek(file, 0, SEEK_END) == 0;
  const long length = ok ? ftell(file) : -1;
  ok = ok && length > 0 && fseek(file, 0, SEEK_SET) == 0 && (out->data = malloc((size_t)length)) != NULL &&
       fread(out->data, (size_t)length, 1, file) == 1;
  fclose(file);
  remove(path);
  out->size = ok ? (size_t)length : 0;
  return ok;
}

static bool same_file(const file_t *a, const file_t *b, const char *what) {
  if (a->size == b->size && memcmp(a->data, b->data, a->size) == 0) return true;
  size_t at = 0;
  while (at < a->size && at < b->size && a->data[at] == b->data[at]) ++at;
  fprintf(stderr, "backend switch test: %s differs: %zu and %zu bytes, from byte %zu on (kept as backend_switch_a.tasp and "
          "backend_switch_b.tasp)\n", what, a->size, b->size, at);
  const file_t *files[2] = {a, b};
  const char *paths[2] = {"backend_switch_a.tasp", "backend_switch_b.tasp"};
  for (int i = 0; i < 2; ++i) {
    FILE *file = fopen(paths[i], "wb");
    if (!file) continue;
    fwrite(files[i]->data, 1, files[i]->size, file);
    fclose(file);
  }
  return false;
}

// The project at each step of a walk through its history: every step undone, every one there was
// redone, and undone again to where it was.
typedef struct walk_t {
  file_t *files;
  int count;
} walk_t;

static bool walk_history(ui_handler_t *ui, walk_t *out) {
  const int undo = ui->undo_manager.undo_count, redo = ui->undo_manager.redo_count;
  out->count = 0;
  out->files = calloc((size_t)(2 * (undo + redo)), sizeof(*out->files));
  if (!out->files) return false;
  for (int i = 0; i < undo; ++i) {
    undo_manager_undo(&ui->undo_manager, &ui->timeline);
    if (!snapshot(ui, &out->files[out->count++])) return false;
  }
  for (int i = 0; i < undo + redo; ++i) {
    undo_manager_redo(&ui->undo_manager, &ui->timeline);
    if (!snapshot(ui, &out->files[out->count++])) return false;
  }
  for (int i = 0; i < redo; ++i) {
    undo_manager_undo(&ui->undo_manager, &ui->timeline);
    if (!snapshot(ui, &out->files[out->count++])) return false;
  }
  return ui->undo_manager.undo_count == undo && ui->undo_manager.redo_count == redo;
}

static void walk_free(walk_t *walk) {
  for (int i = 0; walk->files && i < walk->count; ++i) free(walk->files[i].data);
  free(walk->files);
  memset(walk, 0, sizeof(*walk));
}

static void add_override(starting_config_t *config, const char *id, ft_value value) {
  starting_override_t *override = &config->overrides[config->override_count++];
  memset(override, 0, sizeof(*override));
  snprintf(override->prop_id, sizeof(override->prop_id), "%s", id);
  override->value = value;
  if (value.kind == FT_VALUE_STRING) {
    snprintf(override->string_value, sizeof(override->string_value), "%s", value.as.s);
    override->value.as.s = override->string_value;
  }
}

// A snippet of a run on `track`, typed in: running, jumping and hooking now and then.
static int add_run(ui_handler_t *ui, int track, int start, int count, int seed) {
  int id = -1;
  undo_command_t *command = timeline_api_create_snippet(ui, track, start, count, &id);
  if (!command) return -1;
  undo_manager_register_command(&ui->undo_manager, command);
  input_record_t *inputs = calloc((size_t)count, sizeof(*inputs));
  if (!inputs) return -1;
  for (int tick = 0; tick < count; ++tick) {
    void *record = inputs[tick].bytes;
    g_api->input_default(record);
    g_api->input_set(record, g_api->input_field_index("direction"), (tick + seed) % 50 < 30 ? 1 : -1);
    g_api->input_set(record, g_api->input_field_index("jump"), (tick + seed) % 23 < 3);
    g_api->input_set(record, g_api->input_field_index("hook"), (tick + seed) % 57 < 20);
    g_api->input_set_vec2(record, g_api->input_field_index("target"), (ft_vec2){(float)((tick * 7 + seed) % 200 - 100), -60.f});
  }
  command = timeline_api_set_snippet_inputs(ui, id, 0, count, inputs);
  free(inputs);
  if (!command) return -1;
  undo_manager_register_command(&ui->undo_manager, command);
  return id;
}

static bool switch_to(ui_handler_t *ui, const char *game_id) {
  game_host_t *host = &ui->gfx_handler->game_host;
  char error[2048];
  const int index = game_host_find_id(host, game_id);
  if (index < 0) {
    fprintf(stderr, "backend switch test: no game '%s'\n", game_id);
    return false;
  }
  if (!switch_project_backend(ui, index, error, sizeof(error))) {
    fprintf(stderr, "backend switch test: the switch to '%s' failed: %s\n", game_id, error);
    return false;
  }
  if (logger_warnings()[0]) printf("backend switch test: warnings switching to '%s':\n%s\n", game_id, logger_warnings());
  return strcmp(game_host_active_id(host), game_id) == 0;
}

// The file at `path`, or NULL.
static unsigned char *read_file(const char *path, size_t *size) {
  FILE *file = path ? fopen(path, "rb") : NULL;
  if (!file) return NULL;
  unsigned char *data = NULL;
  long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
  if (length > 0 && fseek(file, 0, SEEK_SET) == 0 && (data = malloc((size_t)length)) != NULL &&
      fread(data, (size_t)length, 1, file) != 1) {
    free(data);
    data = NULL;
  }
  fclose(file);
  *size = data ? (size_t)length : 0;
  return data;
}

// Whether the timeline holds the recording `id` with these bytes.
static bool holds_recording(const timeline_state_t *ts, int id, const unsigned char *data, size_t size) {
  const timeline_recording_t *recording = recordings_find(ts, id);
  return recording && recording->size == size && memcmp(recording->data, data, size) == 0 &&
         strcmp(recording->name, "kept demo") == 0;
}

// The playhead, the selection and the starting worlds as the test leaves them: the history's steps
// move the first two, and its starting worlds are made from the overrides here, as every switch makes
// them (the editor builds them up step by step otherwise, to the same start but not the same bytes).
static void settle(ui_handler_t *ui, int snippet_id) {
  timeline_state_t *ts = &ui->timeline;
  interaction_clear_selection(ts);
  snippet_id_vector_add(&ts->selected_snippets, snippet_id);
  ts->active_snippet_id = snippet_id;
  ts->current_tick = 77;
  for (int g = 0; g < ts->group_count; ++g) model_rebuild_group_start(ts, g);
}

// --round-trip: only moves the open project (one given with --project, say) to the other backend
// and back, and compares what it saves to before and after.
static int round_trip(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  char home[FT_ID_MAX];
  snprintf(home, sizeof(home), "%s", game_host_active_id(host));
  const char *other = strcmp(home, "ddnet") == 0 ? "unique" : "ddnet";
  file_t before = {0}, after = {0};
  int result = 1;
  for (int g = 0; g < ts->group_count; ++g) model_rebuild_group_start(ts, g);
  CHECK(snapshot(ui, &before));
  CHECK(switch_to(ui, other) && switch_to(ui, home));
  CHECK(snapshot(ui, &after) && same_file(&before, &after, "the project after the round trip"));
  printf("backend switch test: %d groups and %d tracks went from %s to %s and back unchanged\n", ts->group_count,
         ts->player_track_count, home, other);
  result = 0;
done:
  free(before.data);
  free(after.data);
  return result;
}

FT_API int plugin_cli(void *data, int argc, const char **argv) {
  (void)data;
  ui_handler_t *ui = (ui_handler_t *)((char *)g_context - offsetof(ui_handler_t, plugin_context));
  for (int i = 0; i < argc; ++i)
    if (strcmp(argv[i], "--round-trip") == 0) return round_trip(ui);
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  file_t before = {0}, refused = {0}, settled = {0}, after = {0};
  walk_t history = {0}, history_after = {0};
  uint32_t uids[8] = {0};
  unsigned char *demo = NULL;
  int result = 1;

  CHECK(strcmp(game_host_active_id(host), "ddnet") == 0 && ts->group_count == 1 && ts->player_track_count == 1);
  CHECK(game_host_find_id(host, "unique") >= 0);

  // An event a game wrote (first: the history's snapshots of the timeline hold it).
  timeline_event_t event = {.tick = 42, .group_index = 0, .player = 0, .color = {1.f, 0.5f, 0.f, 1.f}, .data_size = 3};
  snprintf(event.category, sizeof(event.category), "test");
  snprintf(event.message, sizeof(event.message), "written by the test");
  memcpy(event.data, "abc", 3);
  timeline_events_add(ts, event);

  // Two groups of two and three tees, each with a run, the second group's made in one step.
  undo_command_t *command = timeline_api_create_track(ui, NULL, NULL);
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);
  CHECK(add_run(ui, 0, 0, 120, 0) > 0 && add_run(ui, 1, 10, 90, 17) > 0);
  timeline_data_snapshot_t *group_before = commands_capture_timeline_data(ts);
  CHECK(group_before && model_add_group(ts, "Second"));
  model_set_active_group(ts, 1);
  for (int i = 0; i < 3; ++i) CHECK(model_add_new_track(ts, 1));
  ts->groups[1]->start_offset = 25;
  command = commands_create_timeline_data_change(ui, group_before, "Add Group");
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);
  CHECK(ts->player_track_count == 5);
  const int split = add_run(ui, 3, 5, 150, 31);
  CHECK(split > 0 && add_run(ui, 4, 0, 60, 5) > 0);

  // Starting overrides, of properties only DDNet has and of one only Unique Race has too.
  starting_config_t start_before = ts->player_tracks[0].starting_config;
  starting_config_t *start = &ts->player_tracks[0].starting_config;
  start->enabled = true;
  add_override(start, "position", (ft_value){.kind = FT_VALUE_VEC2, .as.v = {12.5f, 7.f}});
  add_override(start, "team", (ft_value){.kind = FT_VALUE_INT, .as.i = 3});
  add_override(start, "jetpack", (ft_value){.kind = FT_VALUE_BOOL, .as.b = true});
  add_override(start, "switches_off", (ft_value){.kind = FT_VALUE_STRING, .as.s = "2 5-7"});
  add_override(start, "grenades", (ft_value){.kind = FT_VALUE_INT, .as.i = 4});
  add_override(start, "jumps", (ft_value){.kind = FT_VALUE_INT, .as.i = 3});
  model_rebuild_group_start(ts, 0);
  command = commands_create_starting_config_change(ui, 0, &start_before, "Set Start");
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);

  // A split, a rename and a deleted snippet, the last two undone: some steps to redo.
  interaction_clear_selection(ts);
  snippet_id_vector_add(&ts->selected_snippets, split);
  ts->current_tick = 60;
  command = commands_create_split_selected(ui);
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);
  snprintf(ts->player_tracks[2].name, sizeof(ts->player_tracks[2].name), "Renamed");
  command = commands_create_track_name_change(ui, 2, "Track 1");
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);
  interaction_clear_selection(ts);
  snippet_id_vector_add(&ts->selected_snippets, split);
  command = commands_create_delete_selected(ui);
  CHECK(command);
  undo_manager_register_command(&ui->undo_manager, command);
  undo_manager_undo(&ui->undo_manager, ts);
  undo_manager_undo(&ui->undo_manager, ts);

  // The camera on the first track, which no step of the history removes (a camera character whose
  // track an undo removes is not brought back with it), and which the snapshot steps find again by uid.
  ui->camera_timeline.subject_tracks[0] = 0;
  ui->camera_timeline.subject_count = 1;
  CHECK(camera_insert_focus_key(&ui->camera_timeline, 1.5, 0) >= 0);

  // DDNet's project data, which Unique Race reads too: a demo export selection that is not the
  // default, on what no step of the history removes (the first group is not exported, and its first
  // tee not either, with a ping).
  static const unsigned char selection[] = {'D', 'D', 'E', 'X', 1, 0, 0, 0, 2, 0, 0, 0,
                                            0, 2, 0, 0, 0, 0, 37, 0, 0, 0, 1, 0, 0, 0, 0,
                                            1, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
  unsigned char kept[sizeof(selection)];
  CHECK(gh_project_load(host, selection, sizeof(selection)));

  settle(ui, split);
  CHECK(snapshot(ui, &before));
  const int undo_count = ui->undo_manager.undo_count, redo_count = ui->undo_manager.redo_count;
  const int next_snippet_id = ts->next_snippet_id;
  CHECK(undo_count >= 8 && redo_count == 2);
  for (int i = 0; i < ts->player_track_count; ++i) uids[i] = ts->player_tracks[i].uid;
  printf("backend switch test: %d tracks in %d groups, %d undo and %d redo steps\n", ts->player_track_count,
         ts->group_count, undo_count, redo_count);

  // A switch the other game refuses changes nothing.
  ts->recording = true;
  char error[512];
  CHECK(!switch_project_backend(ui, game_host_find_id(host, "unique"), error, sizeof(error)) && error[0]);
  ts->recording = false;
  CHECK(strcmp(game_host_active_id(host), "ddnet") == 0 && ui->undo_manager.undo_count == undo_count);
  CHECK(snapshot(ui, &refused) && same_file(&before, &refused, "the project after a refused switch"));

  // What the history makes of the project on DDNet, and the project back where it was.
  CHECK(walk_history(ui, &history));
  settle(ui, split);
  CHECK(snapshot(ui, &settled) && same_file(&before, &settled, "the project after a walk through its history"));

  // On Unique Race: the same tracks and history, its own starting worlds made from the overrides
  // it knows, the others kept.
  CHECK(switch_to(ui, "unique"));
  CHECK(game_host_variant(host) == NULL);
  CHECK(ts->player_track_count == 5 && ts->group_count == 2 && ts->current_tick == 77 && ts->active_snippet_id == split);
  CHECK(ts->selected_snippets.count == 1 && ts->selected_snippets.ids[0] == split && ts->next_snippet_id == next_snippet_id);
  for (int i = 0; i < ts->player_track_count; ++i) CHECK(ts->player_tracks[i].uid == uids[i]);
  CHECK(ui->undo_manager.undo_count == undo_count && ui->undo_manager.redo_count == redo_count);
  CHECK(ui->camera_timeline.subject_count == 1 && ui->camera_timeline.subject_tracks[0] == 0);
  CHECK(ts->player_tracks[0].starting_config.override_count == 6);
  CHECK(gh_project_save(host, NULL, 0) == sizeof(selection) && gh_project_save(host, kept, sizeof(kept)) == sizeof(kept) &&
        memcmp(kept, selection, sizeof(kept)) == 0);
  // (Unique Race's own override is one it knows, DDNet's are not; the jumps both have are set)
  const int jumps = model_find_player_prop(host, "jumps");
  ft_value value;
  CHECK(model_find_player_prop(host, "grenades") >= 0 && model_find_player_prop(host, "team") < 0);
  CHECK(jumps >= 0 &&
        gh_entity_prop_get(host, ts->groups[0]->initial_world, FT_ENTITY_CLASS_PLAYER, 0, (unsigned)jumps, &value) &&
        value.as.i == 3);
  // (its history works there too, camera characters found again by uid included)
  walk_t history_unique = {0};
  const bool walked = walk_history(ui, &history_unique);
  walk_free(&history_unique);
  CHECK(walked && ui->camera_timeline.subject_count == 1 && ui->camera_timeline.subject_tracks[0] == 0 &&
        ui->camera_timeline.focus_keys[0].track == 0);
  settle(ui, split);

  // Back on DDNet: the project it was, with the history it had.
  CHECK(switch_to(ui, "ddnet"));
  CHECK(game_host_variant(host) && strcmp(game_host_variant(host), "ddrace") == 0);
  for (int i = 0; i < ts->player_track_count; ++i) CHECK(ts->player_tracks[i].uid == uids[i]);
  CHECK(ui->undo_manager.undo_count == undo_count && ui->undo_manager.redo_count == redo_count);
  CHECK(snapshot(ui, &after) && same_file(&before, &after, "the project after DDNet, Unique Race and DDNet"));
  CHECK(walk_history(ui, &history_after) && history_after.count == history.count);
  for (int i = 0; i < history.count; ++i) {
    char what[64];
    snprintf(what, sizeof(what), "step %d of the history walk", i + 1);
    CHECK(same_file(&history.files[i], &history_after.files[i], what));
  }
  printf("backend switch test: the project and its %d history steps are the same after the round trip\n", history.count);

  // A recording no snippet replays (a demo whose import was undone, say) stays with the project
  // while the history can bring its snippets back, so it goes along with every switch, under its
  // id. A game that plays no demos refuses the project instead. (The demo given with --recording,
  // else bytes that are none.)
  const char *demo_path = NULL;
  for (int i = 0; i + 1 < argc; ++i)
    if (strcmp(argv[i], "--recording") == 0) demo_path = argv[i + 1];
  static const unsigned char not_a_demo[] = "not a demo";
  size_t demo_size = sizeof(not_a_demo);
  demo = read_file(demo_path, &demo_size);
  const unsigned char *demo_data = demo ? demo : not_a_demo;
  if (!demo) demo_size = sizeof(not_a_demo);
  const timeline_recording_t *added = recordings_add(ts, -1, "kept demo", demo_data, demo_size);
  CHECK(added);
  const int recording_id = added->id, next_recording_id = ts->next_recording_id;
  const int unique = game_host_find_id(host, "unique");
  if (host->slots[unique].module->constraints.caps & FT_CAP_RECORDINGS) {
    CHECK(switch_to(ui, "unique") && holds_recording(ts, recording_id, demo_data, demo_size));
    CHECK(switch_to(ui, "ddnet") && holds_recording(ts, recording_id, demo_data, demo_size));
    CHECK(ts->next_recording_id == next_recording_id);
    // (and opens again: DDNet plays the demo)
    if (demo) CHECK(recordings_wait(ts, recordings_find(ts, recording_id)));
    printf("backend switch test: a recording no snippet replays went along, %s\n", demo ? "a demo" : "not one");
  } else {
    CHECK(!switch_project_backend(ui, unique, error, sizeof(error)) && strstr(error, "demo"));
    CHECK(strcmp(game_host_active_id(host), "ddnet") == 0 && holds_recording(ts, recording_id, demo_data, demo_size));
    printf("backend switch test: Unique Race plays no demos and refused a project with one\n");
  }
  result = 0;

done:
  free(before.data);
  free(refused.data);
  free(settled.data);
  free(demo);
  free(after.data);
  walk_free(&history);
  walk_free(&history_after);
  return result;
}
