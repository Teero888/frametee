#include <stdlib.h>

#include "cimgui.h"
#include "plugin_api.h"

typedef struct {
  const tas_api_t *api;
  const tas_context_t *context;
  bool show_example_window;
  int snippet_duration;
} plugin_state_t;

// No plugin_game_id export, so this plugin is global: it stays loaded no
// matter which game the editor is running. Export plugin_game_id returning a
// game id ("ddnet", ...) to bind a plugin to one game instead.

FT_PLUGIN_ABI_EXPORT()

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  plugin_state_t *state = (plugin_state_t *)calloc(1, sizeof(plugin_state_t));
  if (!state)
    return NULL;

  state->api = api;
  state->context = context;
  state->show_example_window = true;
  state->snippet_duration = 100;

  api->log(FT_LOG_INFO, "C API Example", "Plugin initialized successfully!");
  return state;
}

FT_API void plugin_update(void *plugin_data) {
  plugin_state_t *state = (plugin_state_t *)plugin_data;
  igSetCurrentContext(state->context->imgui_context);

  if (igBeginMainMenuBar()) {
    if (igBeginMenu("C Example Plugin", true)) {
      igMenuItem_BoolPtr("Show Window", NULL, &state->show_example_window, true);
      igEndMenu();
    }
    igEndMainMenuBar();
  }

  if (state->show_example_window) {
    if (igBegin("C Plugin Window", &state->show_example_window, ImGuiWindowFlags_None)) {
      igText("This window is rendered from a pure C plugin!");
      igSeparator();

      igText("Host API: %d tracks", state->api->get_track_count());
      igText("Host API: Current tick is %d", state->api->get_current_tick());

      igSeparator();
      igSliderInt("Snippet Duration", &state->snippet_duration, 10, 500, "%d ticks", ImGuiSliderFlags_None);

      int selected_track = state->api->get_selected_track();
      if (selected_track < 0) {
        igTextDisabled("Select a track to create a snippet.");
      } else {
        if (igButton("Create Snippet via API", (ImVec2){0, 0})) {
          int current_tick = state->api->get_current_tick();
          undo_command_t *cmd = state->api->do_create_snippet(selected_track, current_tick,
                                                              state->snippet_duration, NULL);
          state->api->register_undo_command(cmd);
        }
      }
    }
    igEnd();
  }
}

FT_API void plugin_shutdown(void *plugin_data) {
  plugin_state_t *state = (plugin_state_t *)plugin_data;
  state->api->log(FT_LOG_INFO, "C API Example", "Plugin is shutting down.");
  free(state);
}
