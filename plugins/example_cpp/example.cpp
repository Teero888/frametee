// define CIMGUI_INCLUDED to prevent cimgui.h from including the C++ ImGui headers.
#define CIMGUI_INCLUDED

#include "imgui.h"
#include "plugin_api.h"

class CppPlugin {
private:
  const tas_api_t *m_api;
  const tas_context_t *m_context;
  bool m_show_window;
  int m_snippet_duration;

public:
  CppPlugin(tas_context_t *context, const tas_api_t *api)
      : m_api(api), m_context(context), m_show_window(true), m_snippet_duration(100) {
    m_api->log_info("Native C++ ImGui Plugin", "Plugin instance created!");
  }

  ~CppPlugin() { m_api->log_info("Native C++ ImGui Plugin", "Plugin instance destroyed."); }

  void update() {
    ImGui::SetCurrentContext(m_context->imgui_context);

    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("C++ Native Plugin")) {
        ImGui::MenuItem("Show Window", nullptr, &m_show_window);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    if (m_show_window) {
      if (ImGui::Begin("C++ Native Plugin Window", &m_show_window)) {
        ImGui::Text("This window is rendered from a C++ plugin using the native ImGui API!");
        ImGui::Separator();

        ImGui::Text("Host Context: %d tracks", m_context->timeline->player_track_count);
        ImGui::Text("Host API: Current tick is %d", m_api->get_current_tick());
        ImGui::Separator();

        ImGui::SliderInt("Snippet Duration", &m_snippet_duration, 10, 500, "%d ticks");

        int selected_track = m_context->timeline->selected_player_track_index;
        if (selected_track < 0) {
          ImGui::TextDisabled("Select a track to create a snippet.");
        } else {
          if (ImGui::Button("Create Snippet via API", ImVec2(0, 0))) {
            int current_tick = m_api->get_current_tick();
            undo_command_t *cmd = m_api->do_create_snippet(selected_track, current_tick, m_snippet_duration);
            m_api->register_undo_command(cmd);
          }
        }
      }
      ImGui::End();
    }
  }
};

extern "C" {

plugin_info_t get_plugin_info() {
  return {"C++ Native ImGui Example", "Teero", "69.420",
          "A self-contained plugin written in C++ using the native ImGui API."};
}

void *plugin_init(tas_context_t *context, const tas_api_t *api) { return new CppPlugin(context, api); }

void plugin_update(void *plugin_data) { static_cast<CppPlugin *>(plugin_data)->update(); }

void plugin_shutdown(void *plugin_data) { delete static_cast<CppPlugin *>(plugin_data); }
}
