#include <stdlib.h>

#include "cimgui.h"
#include "plugin_api.h"

typedef struct {
  const tas_api_t *api;
  const tas_context_t *context;
  bool show_example_window;
  int snippet_duration;
} plugin_state_t;

plugin_info_t get_plugin_info(void) {
  return (plugin_info_t){.name = "C API Example",
                         .author = "Teero",
                         .version = "420.69",
                         .description =
                             "A self-contained plugin written in C that compiles its own ImGui sources."};
}

void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  plugin_state_t *state = (plugin_state_t *)calloc(1, sizeof(plugin_state_t));
  if (!state)
    return NULL;

  state->api = api;
  state->context = context;
  state->show_example_window = true;
  state->snippet_duration = 100;

  api->log_info("C API Example", "Plugin initialized successfully!");
  return state;
}

void plugin_update(void *plugin_data) {
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

      igText("Host Context: %d tracks", state->context->timeline->player_track_count);
      igText("Host API: Current tick is %d", state->api->get_current_tick());

      igSeparator();
      igSliderInt("Snippet Duration", &state->snippet_duration, 10, 500, "%d ticks", ImGuiSliderFlags_None);

      int selected_track = state->context->timeline->selected_player_track_index;
      if (selected_track < 0) {
        igTextDisabled("Select a track to create a snippet.");
      } else {
        if (igButton("Create Snippet via API", (ImVec2){0, 0})) {
          int current_tick = state->api->get_current_tick();
          undo_command_t *cmd =
              state->api->do_create_snippet(selected_track, current_tick, state->snippet_duration);
          state->api->register_undo_command(cmd);
        }
      }
    }
    igEnd();
  }
}

void plugin_shutdown(void *plugin_data) {
  plugin_state_t *state = (plugin_state_t *)plugin_data;
  state->api->log_info("C API Example", "Plugin is shutting down.");
  free(state);
}
