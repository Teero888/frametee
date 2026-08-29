#include "api_impl.h"
#include "../logger/logger.h"
#include "../renderer/graphics_backend.h"
#include "../user_interface/timeline/timeline_commands.h"
#include "../user_interface/timeline/timeline_model.h"
#include "../scripting/script_engine.h"
#include "renderer/renderer.h"
#include <engine/input_record.h>
#include <limits.h>
#include <nfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=================================================================================================
// API IMPLEMENTATION
//=================================================================================================

// this global pointer allows the static API functions to access the application's state.
// it is set once by api_init() and is internal to this file.
static ui_handler_t *g_ui_handler_for_api = NULL;

static void api_log(ft_log_level level, const char *category, const char *message) {
  const char *source = category && *category ? category : "Plugin";
  const char *text = message ? message : "";
  if (level >= FT_LOG_ERROR)
    log_error(source, "%s", text);
  else if (level == FT_LOG_WARN)
    log_warn(source, "%s", text);
  else
    log_info(source, "%s", text);
}

static void api_register_script_command(const char *name, void (*callback)(int argc, const char **argv)) {
  script_engine_register_command(name, callback);
}

static int api_get_current_tick(void) { return g_ui_handler_for_api->timeline.current_tick; }

static int api_get_track_count(void) { return g_ui_handler_for_api->timeline.player_track_count; }

static int api_get_selected_track(void) { return g_ui_handler_for_api->timeline.selected_player_track_index; }

static int track_last_tick(const player_track_t *track) {
  int last = -1;
  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active && snippet->input_count > 0 && snippet->end_tick > 0 && snippet->end_tick - 1 > last)
      last = snippet->end_tick - 1;
  }
  for (int i = 0; i < track->recording_snippet_count; ++i) {
    const input_snippet_t *snippet = &track->recording_snippets[i];
    if (snippet->is_active && snippet->input_count > 0 && snippet->end_tick > 0 && snippet->end_tick - 1 > last)
      last = snippet->end_tick - 1;
  }
  return last;
}

static int api_get_track_last_tick(int track_index) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;
  if (track_index < 0 || track_index >= ts->player_track_count) return -1;
  return track_last_tick(&ts->player_tracks[track_index]);
}

static bool api_get_track_position_at(int track_index, int tick, ft_vec2 *out_position) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;
  if (!out_position || tick < 0 || track_index < 0 || track_index >= ts->player_track_count) return false;

  const int group_index = model_track_group_index(ts, track_index);
  const int local_player = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_player < 0) return false;

  // model_group_world_at_tick takes shared/global time; plugin tracks and their
  // snippets expose local time, so put this query on the owning group's clock.
  const int64_t global_tick = (int64_t)tick + ts->groups[group_index]->start_offset;
  if (global_tick < INT_MIN || global_tick > INT_MAX) return false;
  const ft_world *world = model_group_world_at_tick(ts, group_index, (int)global_tick);
  if (!world) return false;

  ft_player_view view = {.struct_size = sizeof(view)};
  if (!gh_world_player_view(&g_ui_handler_for_api->gfx_handler->game_host, world, local_player, &view)) return false;
  *out_position = view.position;
  return true;
}

// READ ONLY PLEASE. The handle belongs to the active game; a plugin that looks
// inside it has to be built for that game and declare so via plugin_game_id().
static const ft_world *api_get_initial_world(void) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;
  if (!g_ui_handler_for_api->gfx_handler->level) return NULL;
  if (ts->active_group_index < 0 || ts->active_group_index >= ts->group_count) return NULL;
  return ts->groups[ts->active_group_index]->initial_world;
}

// Borrowed from the timeline's own cache, so a plugin must not hold it across
// another timeline query.
static ft_world *api_get_world_state_at(int tick) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;
  if (ts->active_group_index < 0 || ts->active_group_index >= ts->group_count) return NULL;
  const ft_world *world = model_group_world_at_tick(ts, ts->active_group_index, tick);
  if (!world) return NULL;
  game_host_t *host = &g_ui_handler_for_api->gfx_handler->game_host;
  ft_world *copy = gh_world_create(host, g_ui_handler_for_api->gfx_handler->level, gh_world_player_count(host, world),
                                   ts->active_group_index);
  if (copy) gh_world_copy(host, copy, world);
  return copy;
}

static void api_destroy_world(ft_world *world) {
  gh_world_destroy(&g_ui_handler_for_api->gfx_handler->game_host, world);
}

// Schema reflection, so a plugin can edit inputs without knowing the layout.
static uint32_t api_input_record_size(void) {
  return game_input_size(&g_ui_handler_for_api->gfx_handler->game_host);
}

static void api_input_default(void *record) {
  game_host_t *host = &g_ui_handler_for_api->gfx_handler->game_host;
  if (!record) return;
  memset(record, 0, game_input_size(host));
  gh_input_default(host, record);
}

static uint32_t api_input_field_count(void) {
  const ft_input_schema *schema = game_input_schema(&g_ui_handler_for_api->gfx_handler->game_host);
  return schema ? schema->field_count : 0;
}

static const ft_input_field *api_input_field(uint32_t index) {
  const ft_input_schema *schema = game_input_schema(&g_ui_handler_for_api->gfx_handler->game_host);
  return schema && index < schema->field_count ? &schema->fields[index] : NULL;
}

static int api_input_field_index(const char *field_id) {
  return game_input_field_index(&g_ui_handler_for_api->gfx_handler->game_host, field_id);
}

static long long api_input_get(const void *record, int field) {
  return field >= 0 ? gh_input_get(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field) : 0;
}

static void api_input_set(void *record, int field, long long value) {
  if (field >= 0) gh_input_set(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field, value);
}

static float api_input_get_float(const void *record, int field) {
  return field >= 0 ? gh_input_get_float(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field) : 0.f;
}

static void api_input_set_float(void *record, int field, float value) {
  if (field >= 0) gh_input_set_float(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field, value);
}

static ft_vec2 api_input_get_vec2(const void *record, int field) {
  return field >= 0 ? gh_input_get_vec2(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field)
                    : (ft_vec2){0.f, 0.f};
}

static void api_input_set_vec2(void *record, int field, ft_vec2 value) {
  if (field >= 0) gh_input_set_vec2(&g_ui_handler_for_api->gfx_handler->game_host, record, (unsigned)field, value);
}

// A plugin may hand over a profile for the new track, in the same opaque form
// the active game writes: it either knows that game's format or passes nothing.
static struct undo_command_t *api_do_create_track(const ft_player_setup *setup, int *out_track_index) {
  player_profile_t profile = {0};
  const player_profile_t *profile_ptr = NULL;
  if (setup && setup->data && setup->data_size > 0 && setup->data_size <= FT_PLAYER_PROFILE_MAX) {
    memcpy(profile.data, setup->data, setup->data_size);
    profile.size = setup->data_size;
    profile_ptr = &profile;
  }
  return timeline_api_create_track(g_ui_handler_for_api, profile_ptr, out_track_index);
}

static struct undo_command_t *api_do_create_snippet(int track_index, int start_tick, int duration, int *out_snippet_id) {
  return timeline_api_create_snippet(g_ui_handler_for_api, track_index, start_tick, duration, out_snippet_id);
}

static bool api_find_snippet_at(int track_index, int tick, int *out_snippet_id, int *out_tick_offset, int *out_available) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;
  if (track_index < 0 || track_index >= ts->player_track_count) return false;
  player_track_t *track = &ts->player_tracks[track_index];
  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet_t *snippet = &track->snippets[i];
    if (!snippet->is_active) continue;
    if (tick < snippet->start_tick || tick >= snippet->end_tick) continue;
    const int offset = tick - snippet->start_tick;
    if (out_snippet_id) *out_snippet_id = snippet->id;
    if (out_tick_offset) *out_tick_offset = offset;
    if (out_available) *out_available = snippet->input_count - offset;
    return true;
  }
  return false;
}

static struct undo_command_t *api_do_set_inputs(int snippet_id, int tick_offset, int count, const void *new_inputs,
                                                 size_t record_stride) {
  game_host_t *host = &g_ui_handler_for_api->gfx_handler->game_host;
  const size_t record_size = game_input_size(host);
  if (!new_inputs || count <= 0 || record_stride < record_size) return NULL;
  input_record_t *records = calloc((size_t)count, sizeof(*records));
  if (!records) return NULL;
  for (int i = 0; i < count; ++i)
    memcpy(records[i].bytes, (const uint8_t *)new_inputs + (size_t)i * record_stride, record_size);
  struct undo_command_t *command = timeline_api_set_snippet_inputs(g_ui_handler_for_api, snippet_id, tick_offset, count, records);
  free(records);
  return command;
}

// A command whose change has already been applied but whose history entry is not
// wanted: the bulk edit below records itself as one snapshot instead.
static void discard_undo_command(undo_command_t *command) {
  if (!command) return;
  if (command->cleanup) command->cleanup(command);
  free(command);
}

static int api_do_export_run_to_group(const char *name, int start_tick, int count, const void *records,
                                      size_t record_stride) {
  ui_handler_t *ui = g_ui_handler_for_api;
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const size_t record_size = game_input_size(host);
  if (!records || count <= 0 || record_stride < record_size || start_tick < 0) return -1;

  timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
  if (!before) return -1;

  char group_name[MAX_TIMELINE_GROUP_NAME];
  snprintf(group_name, sizeof(group_name), "%s", name && name[0] ? name : "Run");
  if (!model_add_group(ts, group_name)) {
    commands_free_timeline_data_snapshot(before);
    return -1;
  }

  const int group_index = ts->group_count - 1;
  model_set_active_group(ts, group_index);
  model_sync_tracks_to_world(ts, group_index);

  // The track lands in the active group, which is the one just made.
  int track_index = -1;
  discard_undo_command(timeline_api_create_track(ui, NULL, &track_index));
  if (track_index < 0) {
    commands_free_timeline_data_snapshot(before);
    return -1;
  }

  int snippet_id = -1;
  discard_undo_command(timeline_api_create_snippet(ui, track_index, start_tick, count, &snippet_id));
  input_snippet_t *snippet = snippet_id >= 0 ? model_find_snippet_by_id(ts, snippet_id, NULL) : NULL;
  if (!snippet) {
    commands_free_timeline_data_snapshot(before);
    return -1;
  }

  input_record_t *window = snippet_window(snippet);
  const int writable = count < snippet->input_count ? count : snippet->input_count;
  for (int i = 0; i < writable; ++i)
    memcpy(window[i].bytes, (const uint8_t *)records + (size_t)i * record_stride, record_size);

  model_recalc_physics(ts, start_tick);

  undo_command_t *change = commands_create_timeline_data_change(ui, before, "Export Run to Group");
  if (change) undo_manager_register_command(&ui->undo_manager, change);
  return group_index;
}

static const char *api_get_level_name(void) { return g_ui_handler_for_api->loaded_level_name; }
static const char *api_get_level_path(void) { return g_ui_handler_for_api->loaded_level_path; }
static bool api_viewport_accepts_input(bool continuing_drag) {
  return g_ui_handler_for_api->viewport_focused && (g_ui_handler_for_api->viewport_hovered || continuing_drag);
}

static void api_register_undo_command(struct undo_command_t *command) {
  if (command) {
    // Tagged with whichever plugin is running, so unloading it takes the
    // command with it: the command may be the plugin's own, and its undo, redo
    // and cleanup would point into the unmapped library. Host-built commands
    // registered by a plugin are tagged too, which costs nothing -- the host
    // can rebuild history it owns, and dropping a step is better than keeping
    // one whose partner is gone.
    undo_manager_register_command_owned(&g_ui_handler_for_api->undo_manager, command, plugin_manager_running_plugin());
  }
}

static void api_draw_line_world(vec2 start, vec2 end, float z, vec4 color, float thickness) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_line(g_ui_handler_for_api->gfx_handler, z + 10.0f, start, end, color, thickness);
}

static void api_draw_circle_world(vec2 center, float radius, vec4 color) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_circle_filled(g_ui_handler_for_api->gfx_handler, 10.0f, center, radius, color, 16);
}

static void api_draw_rect_filled_world(vec2 pos, vec2 size, float z, vec4 color) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_rect_filled(g_ui_handler_for_api->gfx_handler, z + 10.0f, pos, size, color);
}

static void api_draw_text_world(vec2 pos, const char *text, vec4 color) {
  // Not implemented yet in renderer, but we should have a stub to avoid NULL call
  (void)pos; (void)text; (void)color;
}

static void api_draw_line_world3(vec3 start, vec3 end, vec4 color, float thickness) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_line3(g_ui_handler_for_api->gfx_handler, start, end, color, thickness);
}

static void api_draw_box_world3(vec3 center, vec3 size, vec4 color, bool wire) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_box3(g_ui_handler_for_api->gfx_handler, center, size, color, wire);
}

static void api_screen_to_world(float screen_x, float screen_y, float *world_x, float *world_y) {
  float lx = screen_x - g_ui_handler_for_api->viewport_window_pos.x;
  float ly = screen_y - g_ui_handler_for_api->viewport_window_pos.y;
  
  static ImGuiWindow *s_viewport_window = NULL;
  if (!s_viewport_window) {
    s_viewport_window = igFindWindowByName("Viewport");
  }
  if (s_viewport_window) {
    lx -= s_viewport_window->DecoOuterSizeX1;
    ly -= s_viewport_window->DecoOuterSizeY1;
  }
  
  screen_to_world(g_ui_handler_for_api->gfx_handler, lx, ly, world_x, world_y);
}

static bool api_screen_ray_world3(float screen_x, float screen_y, vec3 out_origin, vec3 out_dir) {
  // Same viewport-local correction as api_screen_to_world: the coordinates come
  // from ImGui and are relative to the whole window.
  float lx = screen_x - g_ui_handler_for_api->viewport_window_pos.x;
  float ly = screen_y - g_ui_handler_for_api->viewport_window_pos.y;

  static ImGuiWindow *s_viewport_window = NULL;
  if (!s_viewport_window) {
    s_viewport_window = igFindWindowByName("Viewport");
  }
  if (s_viewport_window) {
    lx -= s_viewport_window->DecoOuterSizeX1;
    ly -= s_viewport_window->DecoOuterSizeY1;
  }

  return screen_ray3(g_ui_handler_for_api->gfx_handler, lx, ly, out_origin, out_dir);
}

static void api_world_to_screen(float world_x, float world_y, float *screen_x, float *screen_y) {
  world_to_screen(g_ui_handler_for_api->gfx_handler, world_x, world_y, screen_x, screen_y);
  *screen_x += g_ui_handler_for_api->viewport_window_pos.x;
  *screen_y += g_ui_handler_for_api->viewport_window_pos.y;
  
  static ImGuiWindow *s_viewport_window = NULL;
  if (!s_viewport_window) {
    s_viewport_window = igFindWindowByName("Viewport");
  }
  if (s_viewport_window) {
    *screen_x += s_viewport_window->DecoOuterSizeX1;
    *screen_y += s_viewport_window->DecoOuterSizeY1;
  }
}

static double api_get_time(void) {
  return glfwGetTime();
}

static bool api_save_file_dialog(const char *filter_name, const char *filter_ext, const char *default_name, char *out_path, int out_path_size) {
  if (!out_path || out_path_size <= 0) return false;
  out_path[0] = '\0';

  extern bool g_is_headless;
  if (g_is_headless) {
    log_warn("PluginAPI", "save_file_dialog is unavailable in headless mode");
    return false;
  }

  nfdu8filteritem_t filter = {filter_name, filter_ext};
  nfdu8char_t *save_path = NULL;
  nfdresult_t result = NFD_SaveDialogU8(&save_path, (filter_name && filter_ext) ? &filter : NULL,
                                        (filter_name && filter_ext) ? 1 : 0, NULL, default_name);
  if (result != NFD_OKAY || !save_path) {
    if (result == NFD_ERROR) log_error("PluginAPI", "Save dialog failed: %s", NFD_GetError());
    return false;
  }

  snprintf(out_path, (size_t)out_path_size, "%s", save_path);
  NFD_FreePathU8(save_path);
  return true;
}

static void api_get_camera_info(vec2 pos, float *zoom) {
  glm_vec2_copy(g_ui_handler_for_api->gfx_handler->renderer.camera.pos, pos);
  *zoom = g_ui_handler_for_api->gfx_handler->renderer.camera.zoom;
}

tas_api_t api_init(ui_handler_t *ui_handler) {
  g_ui_handler_for_api = ui_handler;

  return (tas_api_t){
      .log = api_log,
      .get_current_tick = api_get_current_tick,
      .get_track_count = api_get_track_count,
      .get_selected_track = api_get_selected_track,
      .get_track_last_tick = api_get_track_last_tick,
      .get_track_position_at = api_get_track_position_at,
      .get_initial_world = api_get_initial_world,
      .get_world_state_at = api_get_world_state_at,
      .destroy_world = api_destroy_world,
      .input_record_size = api_input_record_size,
      .input_default = api_input_default,
      .input_field_count = api_input_field_count,
      .input_field = api_input_field,
      .input_field_index = api_input_field_index,
      .input_get = api_input_get,
      .input_set = api_input_set,
      .input_get_float = api_input_get_float,
      .input_set_float = api_input_set_float,
      .input_get_vec2 = api_input_get_vec2,
      .input_set_vec2 = api_input_set_vec2,
      .do_create_track = api_do_create_track,
      .register_undo_command = api_register_undo_command,
      .do_export_run_to_group = api_do_export_run_to_group,
      .do_create_snippet = api_do_create_snippet,
      .find_snippet_at = api_find_snippet_at,
      .do_set_inputs = api_do_set_inputs,
      .get_level_name = api_get_level_name,
      .get_level_path = api_get_level_path,
      .viewport_accepts_input = api_viewport_accepts_input,
      .draw_line_world = api_draw_line_world,
      .draw_circle_world = api_draw_circle_world,
      .draw_rect_filled_world = api_draw_rect_filled_world,
      .draw_text_world = api_draw_text_world,
      .draw_line_world3 = api_draw_line_world3,
      .draw_box_world3 = api_draw_box_world3,
      .screen_to_world = api_screen_to_world,
      .screen_ray_world3 = api_screen_ray_world3,
      .world_to_screen = api_world_to_screen,
      .get_time = api_get_time,
      .get_camera_info = api_get_camera_info,
      .register_script_command = api_register_script_command,
      .save_file_dialog = api_save_file_dialog,
  };
}
