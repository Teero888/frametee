#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "../user_interface/timeline.h"

// forward declare ImGuiContext to avoid plugins needing to include cimgui.h if they don't have a UI.
struct ImGuiContext;

// passed to plugins to provide read-only access to high-level application state.
typedef struct {
  ui_handler_t *ui_handler;
  timeline_state_t *timeline;
  gfx_handler_t *gfx_handler;
  struct ImGuiContext *imgui_context;
} tas_context_t;

// api functions provided to plugins for interacting with the host application.
typedef struct {
  // Timeline & Input API
  int (*get_current_tick)(void);
  int (*get_track_count)(void);
  SWorldCore *(*get_initial_world)(void);

  // Undo-able Write Operations
  undo_command_t *(*do_create_track)(const player_info_t *info, int *out_track_index);
  undo_command_t *(*do_create_snippet)(int track_index, int start_tick, int duration, int *out_snippet_id);
  undo_command_t *(*do_delete_snippet)(int snippet_id);
  undo_command_t *(*do_set_inputs)(int snippet_id, int tick_offset, int count,
                                   const SPlayerInput *new_inputs);
  void (*register_undo_command)(undo_command_t *command);

  // Debug Drawing API
  void (*draw_line_world)(vec2 start, vec2 end, vec4 color, float thickness);
  void (*draw_circle_world)(vec2 center, float radius, vec4 color);
  void (*draw_text_world)(vec2 pos, const char *text, vec4 color);

  // Utility API
  void (*log_info)(const char *plugin_name, const char *message);
  void (*log_warning)(const char *plugin_name, const char *message);
  void (*log_error)(const char *plugin_name, const char *message);
} tas_api_t;

typedef struct {
  const char *name;
  const char *author;
  const char *version;
  const char *description;
} plugin_info_t;

typedef void *(*plugin_init_func)(tas_context_t *context, const tas_api_t *api);
typedef void (*plugin_update_func)(void *plugin_data);
typedef void (*plugin_shutdown_func)(void *plugin_data);
typedef plugin_info_t (*get_plugin_info_func)(void);

#define GET_PLUGIN_INFO_FUNC_NAME "get_plugin_info"
#define GET_PLUGIN_INIT_FUNC_NAME "plugin_init"
#define GET_PLUGIN_UPDATE_FUNC_NAME "plugin_update"
#define GET_PLUGIN_SHUTDOWN_FUNC_NAME "plugin_shutdown"

#undef FT_API
#ifdef _WIN32
#define FT_API __declspec(dllexport)
#else
#define FT_API extern
#endif

#endif // PLUGIN_API_H
