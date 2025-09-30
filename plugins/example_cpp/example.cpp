// define CIMGUI_INCLUDED to prevent cimgui.h from including the C++ ImGui headers.
#define CIMGUI_INCLUDED

#include "imgui.h"
#include "plugin_api.h"

class CppPlugin {
private:
  const tas_api_t *m_pAPI;
  const tas_context_t *m_pContext;
  bool m_ShowWindow;
  int m_SnippetDuration;

public:
  CppPlugin(tas_context_t *pContext, const tas_api_t *pAPI)
      : m_pAPI(pAPI), m_pContext(pContext), m_ShowWindow(true), m_SnippetDuration(100) {
    m_pAPI->log_info("Native C++ ImGui Plugin", "Plugin instance created!");
  }

  ~CppPlugin() { m_pAPI->log_info("Native C++ ImGui Plugin", "Plugin instance destroyed."); }

  void update() {
    ImGui::SetCurrentContext(m_pContext->imgui_context);

    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("C++ Native Plugin")) {
        ImGui::MenuItem("Show Window", nullptr, &m_ShowWindow);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    if (m_ShowWindow) {
      if (ImGui::Begin("C++ Native Plugin Window", &m_ShowWindow)) {
        ImGui::Text("This window is rendered from a C++ plugin using the native ImGui API!");
        ImGui::Separator();

        ImGui::Text("Host Context: %d tracks", m_pContext->timeline->player_track_count);
        ImGui::Text("Host API: Current tick is %d", m_pAPI->get_current_tick());
        ImGui::Separator();

        ImGui::SliderInt("Snippet Duration", &m_SnippetDuration, 10, 500, "%d ticks");

        int SelectedTrack = m_pContext->timeline->selected_player_track_index;
        if (SelectedTrack < 0) {
          ImGui::TextDisabled("Select a track to create a snippet.");
        } else {
          if (ImGui::Button("Create Snippet via API", ImVec2(0, 0))) {
            int CurrentTick = m_pAPI->get_current_tick();
            undo_command_t *pCmd = m_pAPI->do_create_snippet(SelectedTrack, CurrentTick, m_SnippetDuration);
            m_pAPI->register_undo_command(pCmd);
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
