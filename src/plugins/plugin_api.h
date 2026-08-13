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
typedef struct plugin_info_t plugin_info_t;

typedef void *(*plugin_init_func)(tas_context_t *context, const tas_api_t *api);
typedef void (*plugin_shutdown_func)(void *plugin_data);
typedef void (*plugin_update_func)(void *plugin_data);
typedef void (*plugin_show_ui_func)(void *plugin_data);
typedef plugin_info_t (*get_plugin_info_func)(void);

// passed to plugins to provide read-only access to high-level application state.
struct tas_context_t {
  struct ImGuiContext *imgui_context;
  bool is_headless;
  // Id of the game module currently driving the editor, or "" when none is
  // active. A global plugin can branch on this; a game-specific one is only
  // ever initialized while its own game is active, so it can assume its match.
  const char *active_game_id;
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

  const char *(*get_level_name)(void);
  const char *(*get_level_path)(void);
  bool (*viewport_accepts_input)(bool continuing_drag);

  // Debug Drawing API
  void (*draw_line_world)(vec2 start, vec2 end, float z, vec4 color, float thickness);
  void (*draw_circle_world)(vec2 center, float radius, vec4 color);
  void (*draw_rect_filled_world)(vec2 pos, vec2 size, float z, vec4 color);
  void (*draw_text_world)(vec2 pos, const char *text, vec4 color);

  void (*screen_to_world)(float screen_x, float screen_y, float *world_x, float *world_y);
  void (*world_to_screen)(float world_x, float world_y, float *screen_x, float *screen_y);

  double (*get_time)(void);
  void (*get_camera_info)(vec2 pos, float *zoom);

  void (*register_script_command)(const char *name, void (*callback)(int argc, const char **argv));

  // opens the native "save file" dialog and writes the chosen path into out_path.
  // filter_name/filter_ext describe a single file type (e.g. "FrameTee Script", "ftee"), both may be NULL.
  // returns false when the user cancelled, the dialog failed, or the app runs headless.
  bool (*save_file_dialog)(const char *filter_name, const char *filter_ext, const char *default_name, char *out_path, int out_path_size);
};

struct plugin_info_t {
  const char *name;
  const char *author;
  const char *version;
  const char *description;
};

#define GET_PLUGIN_INFO_FUNC_NAME "get_plugin_info"
#define GET_PLUGIN_INIT_FUNC_NAME "plugin_init"
#define GET_PLUGIN_UPDATE_FUNC_NAME "plugin_update"
#define GET_PLUGIN_SHUTDOWN_FUNC_NAME "plugin_shutdown"

// Optional. Export it to tie a plugin to one game, returning that game's module
// id (e.g. "ddnet"); the host then only loads the plugin while that game is
// active, and unloads it when the user switches away.
//
//   FT_API const char *plugin_game_id(void) { return "ddnet"; }
//
// A plugin that does not export this symbol is global: it works with every
// game and stays loaded across game switches. Anything that reaches into a
// game's world or input records belongs to that game and should say so, since
// the bytes mean nothing under a different one. This is a separate symbol
// rather than a plugin_info_t field because that struct is returned by value,
// so growing it would make already-built plugins read past their own stack.
#define GET_PLUGIN_GAME_ID_FUNC_NAME "plugin_game_id"

#undef FT_API
#ifdef _WIN32
#define FT_API __declspec(dllexport)
#else
#define FT_API extern
#endif

#endif // PLUGIN_API_H
