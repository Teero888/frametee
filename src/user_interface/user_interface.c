#include "user_interface.h"
#include "cimgui.h"
#include "ddnet_map_loader.h"
#include "timeline.h"
#include <limits.h>
#include <nfd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Docking Setup ---
void setup_docking(ui_handler_t *ui) {
  // main menu bar
  if (igBeginMainMenuBar()) {
    if (igBeginMenu("File", true)) {
      if (igMenuItem_Bool("Open", NULL, false, true)) {
        nfdchar_t *out_path = NULL;
        nfdresult_t result = NFD_OpenDialog("map", NULL, &out_path);
        if (result == NFD_OKAY) {
          printf("Selected file: %s\n", out_path);
          free_map_data(&ui->map_data);
          ui->map_data = load_map(out_path);
          if (!ui->map_data.game_layer.data)
            free_map_data(&ui->map_data);
          free(out_path);
        } else if (result == NFD_CANCEL) {
          printf("File dialog canceled.\n");
        } else {
          printf("File dialog error: %s\n", NFD_GetError());
        }
      }
      if (igMenuItem_Bool("Save", NULL, false, true)) {
        printf("Save selected (not implemented).\n");
      }
      igEndMenu();
    }

    // View menu
    if (igBeginMenu("View", true)) {
      igMenuItem_BoolPtr("Timeline", NULL, &ui->show_timeline, true);
      igEndMenu();
    }

    igEndMainMenuBar();
  }

  ImGuiID main_dockspace_id = igGetID_Str("MainDockSpace");

  // Ensure the dockspace covers the entire viewport initially
  ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos(viewport->WorkPos, ImGuiCond_Always, (ImVec2){0.0f, 0.0f});
  igSetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  igSetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove;
  host_window_flags |=
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0.0f, 0.0f});
  igBegin("DockSpace Host Window", NULL,
          host_window_flags); // pass null for p_open to prevent closing the host window
  igPopStyleVar(3);

  // create the main dockspace
  igDockSpace(main_dockspace_id, (ImVec2){0.0f, 0.0f}, ImGuiDockNodeFlags_PassthruCentralNode,
              NULL); // Passthru allows seeing background
  igEnd();

  // -- Build the initial layout programmatically (optional, but good for setup) --
  // This needs to be done *after* the DockSpace call, often on the first frame or after a reset.
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // Split the main dockspace: Timeline at bottom, rest on top
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom = igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.30f, NULL,
                                                    &dock_id_top); // Timeline takes 30%

    // Split the top area: Player list on left, properties on right
    // ImGuiID dock_id_left;
    // ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.80f, NULL,
    //                                                &dock_id_left); // Player list takes 20% (1.0 - 0.8)
    // The remaining central node of the top split (where dock_id_right was created) will be left empty by
    // default with PassthruCentralNode

    // Assign windows to docks
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    // igDockBuilderDockWindow("Player List", dock_id_left);
    // igDockBuilderDockWindow("Properties", dock_id_right);

    igDockBuilderFinish(main_dockspace_id);
  }
}

void ui_init(ui_handler_t *ui) {
  ui->show_timeline = true;
  memset(&ui->map_data, 0, sizeof(map_data_t));
  timeline_init(&ui->timeline);
}

void ui_render(ui_handler_t *ui) {
  setup_docking(ui);
  if (ui->show_timeline) {
    render_timeline(&ui->timeline);
  }
}

void ui_cleanup(ui_handler_t *ui) { free(ui->timeline.player_tracks); }
