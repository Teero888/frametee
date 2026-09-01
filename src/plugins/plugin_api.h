#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cglm/cglm.h>
#include <frametee/game_abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// forward declare ImGuiContext to avoid plugins needing to include cimgui.h if they don't have a UI.
struct ImGuiContext;

#ifndef FRAMETEE_UNDO_COMMAND_T_DEFINED
#define FRAMETEE_UNDO_COMMAND_T_DEFINED
typedef struct undo_command_t undo_command_t;
#endif
typedef struct tas_context_t tas_context_t;
typedef struct tas_api_t tas_api_t;

typedef void *(*plugin_init_func)(tas_context_t *context, const tas_api_t *api);
typedef void (*plugin_shutdown_func)(void *plugin_data);
typedef void (*plugin_update_func)(void *plugin_data);
typedef void (*plugin_show_ui_func)(void *plugin_data);

// passed to plugins to provide read-only access to high-level application state.
struct tas_context_t {
  struct ImGuiContext *imgui_context;
  bool is_headless;
  // Id of the game module currently driving the editor, or "" when none is
  // active. A global plugin can branch on this; a game-specific one is only
  // ever initialized while its own game is active, so it can assume its match.
  const char *active_game_id;
  // The directory the plugin was loaded from, which is the plugin's own: it may
  // read its resources from there. Set for the duration of plugin_init and null
  // outside it, so copy it if it is needed later.
  const char *plugin_directory;
  // False while the editor's interface is down (Tab), which is how a user asks
  // for the level with nothing over it. A plugin is still updated every frame
  // while it is false -- searches, recordings and world overlays are the work
  // the editor is being cleared to show -- so what belongs behind this test is
  // the plugin's panels, exactly what the editor and the games drop from the
  // same gesture. Menu bar entries stay: the menu bar itself does.
  //
  // Live, and read through the context pointer the plugin was handed: check it
  // in plugin_update rather than copying it in plugin_init.
  bool ui_visible;
};

// api functions provided to plugins for interacting with the host application.
struct tas_api_t {
  // General host services. The message is consumed during the call.
  void (*log)(ft_log_level level, const char *category, const char *message);

  // Timeline & Input API.
  //
  // Worlds and input records belong to the active game. A plugin that reads
  // them has to be built for that game and say so via plugin_game_id(); the
  // game's own headers then give the handles meaning.
  int (*get_current_tick)(void);
  int (*get_track_count)(void);
  int (*get_selected_track)(void);
  // Last tick carrying active input on this track, in the track's own local
  // time, or -1 when it has no input. Recording snippets are included.
  int (*get_track_last_tick)(int track_index);
  // Position reported by the active game's ft_player_view for this track at a
  // track-local tick. The host resolves both the track's simulation group and
  // its player index within that group, so callers must not confuse a global
  // timeline track index with a world's local player index.
  bool (*get_track_position_at)(int track_index, int tick, ft_vec2 *out_position);
  const ft_world *(*get_initial_world)(void);
  // Owned copy. Release it with destroy_world.
  ft_world *(*get_world_state_at)(int tick);
  void (*destroy_world)(ft_world *world);
  // Reflection over the active game's input schema, for plugins that stay
  // game-agnostic. Records passed here are tightly packed game-owned bytes.
  uint32_t (*input_record_size)(void);
  void (*input_default)(void *record);
  uint32_t (*input_field_count)(void);
  // Borrowed for as long as the current game remains active.
  const ft_input_field *(*input_field)(uint32_t index);
  int (*input_field_index)(const char *field_id);
  long long (*input_get)(const void *record, int field);
  void (*input_set)(void *record, int field, long long value);
  float (*input_get_float)(const void *record, int field);
  void (*input_set_float)(void *record, int field, float value);
  ft_vec2 (*input_get_vec2)(const void *record, int field);
  void (*input_set_vec2)(void *record, int field, ft_vec2 value);

  // Undo-able Write Operations
  struct undo_command_t *(*do_create_track)(const ft_player_setup *setup, int *out_track_index);
  struct undo_command_t *(*do_create_snippet)(int track_index, int start_tick, int duration, int *out_snippet_id);
  struct undo_command_t *(*do_delete_snippet)(int snippet_id);
  bool (*find_snippet_at)(int track_index, int tick, int *out_snippet_id, int *out_tick_offset, int *out_available);
  struct undo_command_t *(*do_set_inputs)(int snippet_id, int tick_offset, int count, const void *new_inputs,
                                          size_t record_stride);
  void (*register_undo_command)(struct undo_command_t *command);

  // Lays one run out in a group of its own: a new group holding a single track
  // that carries `count` input records starting at `start_tick`. Returns the
  // group's index, or -1.
  //
  // A group is the engine's unit of parallel simulation, so for a game that
  // seats one player per world it is the only way to show two runs at once.
  // This is a structural edit, undone by restoring the timeline rather than by
  // inverting each step, and the whole call lands as a single entry in the
  // history -- which is what makes it usable in a loop.
  int (*do_export_run_to_group)(const char *name, int start_tick, int count, const void *records,
                                size_t record_stride);

  const char *(*get_level_name)(void);
  const char *(*get_level_path)(void);
  bool (*viewport_accepts_input)(bool continuing_drag);

  // Debug Drawing API
  void (*draw_line_world)(vec2 start, vec2 end, float z, vec4 color, float thickness);
  void (*draw_circle_world)(vec2 center, float radius, vec4 color);
  void (*draw_rect_filled_world)(vec2 pos, vec2 size, float z, vec4 color);
  void (*draw_text_world)(vec2 pos, const char *text, vec4 color);

  // The same, for a game that declared FT_DIMENSIONS_3D. These are world-space
  // and depth-tested, so submit order does not matter and there is no z to
  // order them by. A plugin built for a 3D game has nowhere to put its third
  // axis in the plane API above, which is the only reason these exist.
  void (*draw_line_world3)(vec3 start, vec3 end, vec4 color, float thickness);
  void (*draw_box_world3)(vec3 center, vec3 size, vec4 color, bool wire);

  void (*screen_to_world)(float screen_x, float screen_y, float *world_x, float *world_y);
  // The world-space ray under a screen position, which is what picking in a 3D
  // level needs: there is no single world point under a cursor when the level
  // has depth. False when there is no usable camera.
  bool (*screen_ray_world3)(float screen_x, float screen_y, vec3 out_origin, vec3 out_dir);
  // Projects a 3D world point into ImGui screen coordinates. False when the
  // point is behind the camera or outside the viewport.
  bool (*world_to_screen3)(vec3 world, float *screen_x, float *screen_y);
  void (*world_to_screen)(float world_x, float world_y, float *screen_x, float *screen_y);

  double (*get_time)(void);
  void (*get_camera_info)(vec2 pos, float *zoom);

  void (*register_script_command)(const char *name, void (*callback)(int argc, const char **argv));

  // opens the native "save file" dialog and writes the chosen path into out_path.
  // filter_name/filter_ext describe a single file type (e.g. "FrameTee Script", "ftee"), both may be NULL.
  // returns false when the user cancelled, the dialog failed, or the app runs headless.
  bool (*save_file_dialog)(const char *filter_name, const char *filter_ext, const char *default_name, char *out_path, int out_path_size);
};

// A plugin's name, author, version and description come from the manifest
// beside its library, not from an export: the editor has to be able to show
// them for a plugin it has not loaded, and it never loads one the user has not
// enabled. See docs/plugins.md.

#define GET_PLUGIN_INIT_FUNC_NAME "plugin_init"
#define GET_PLUGIN_UPDATE_FUNC_NAME "plugin_update"
#define GET_PLUGIN_SHUTDOWN_FUNC_NAME "plugin_shutdown"

// Everything in this header is a shape two separately compiled programs agree
// on, and a plugin built against a different version of it calls through
// structs that have since moved. There is no way to notice that from the
// symbols alone, so every plugin says which version it was built for and the
// host refuses anything else, exactly as it does for game modules.
//
// Bump this whenever the structs, the exports, or the meaning of either change.
#define FRAMETEE_PLUGIN_ABI_VERSION 3u
#define GET_PLUGIN_ABI_VERSION_FUNC_NAME "plugin_abi_version"
typedef uint32_t (*plugin_abi_version_func)(void);

// Writes the required export. Put it with a plugin's other entry points:
//
//   FT_PLUGIN_ABI_EXPORT()
//
#define FT_PLUGIN_ABI_EXPORT() \
  FT_API uint32_t plugin_abi_version(void) { return FRAMETEE_PLUGIN_ABI_VERSION; }

// Optional. Export it to tie a plugin to one game, returning that game's module
// id (e.g. "ddnet"); the host then only loads the plugin while that game is
// active, and unloads it when the user switches away.
//
//   FT_API const char *plugin_game_id(void) { return "ddnet"; }
//
// A plugin that does not export this symbol is global: it works with every
// game and stays loaded across game switches. Anything that reaches into a
// game's world or input records belongs to that game and should say so, since
// the bytes mean nothing under a different one.
//
// The manifest names the game too. Before opening a library, the host may use
// that claim only to withhold a load under another game; after opening it, this
// export is authoritative. It cannot drift from the code the way a text file
// beside it can, and loading a plugin under a game it was not written for is a
// question of whether its reads mean anything, not of what it calls itself.
#define GET_PLUGIN_GAME_ID_FUNC_NAME "plugin_game_id"

#undef FT_API
#ifdef _WIN32
#define FT_API __declspec(dllexport)
#else
#define FT_API extern
#endif

#endif // PLUGIN_API_H
