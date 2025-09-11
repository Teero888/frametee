#include "user_interface.h"
#include "../graphics_backend.h"
#include "../renderer.h"
#include "cimgui.h"
#include "ddnet_map_loader.h"
#include "player_info.h"
#include "timeline.h"
#include <limits.h>
#include <nfd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void on_map_load(gfx_handler_t *handler, const char *map_path);
void render_menu_bar(ui_handler_t *ui) {
  if (igBeginMainMenuBar()) {
    if (igBeginMenu("File", true)) {
      if (igMenuItem_Bool("Open", NULL, false, true)) {
        nfdu8char_t *out_path;
        nfdu8filteritem_t filters[] = {{"map files", "map"}};
        nfdopendialogu8args_t args = {0};
        args.filterList = filters;
        args.filterCount = 1;
        nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
        if (result == NFD_OKAY) {
          on_map_load(ui->gfx_handler, out_path);
          NFD_FreePathU8(out_path);
        } else if (result == NFD_CANCEL)
          puts("Canceled map load.");
        else
          printf("Error: %s\n", NFD_GetError());
      }
      if (igMenuItem_Bool("Save", NULL, false, true)) {
        printf("Save selected (not implemented).\n");
      }
      igEndMenu();
    }

    // view menu
    if (igBeginMenu("View", true)) {
      igMenuItem_BoolPtr("Timeline", NULL, &ui->show_timeline, true);
      igEndMenu();
    }

    igEndMainMenuBar();
  }
}

// --- Docking Setup ---
void setup_docking(ui_handler_t *ui) {

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

  // -- build the initial layout programmatically --
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom =
        igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.30f, NULL, &dock_id_top);
    ImGuiID dock_id_right;
    ImGuiID dock_id_left = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.15f, NULL, &dock_id_left);

    // assign windows to docks
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    igDockBuilderDockWindow("Player Info", dock_id_left);
    // igDockBuilderDockWindow("Properties", dock_id_right);

    igDockBuilderFinish(main_dockspace_id);
  }
}

void on_camera_update(gfx_handler_t *handler) {
  camera_t *camera = &handler->renderer.camera;
  ImGuiIO *io = igGetIO_Nil();
  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;

  float scroll_y = io->MouseWheel;
  if (igIsKeyPressed_Bool(ImGuiKey_W, true))
    scroll_y = 1.0f;
  if (igIsKeyPressed_Bool(ImGuiKey_S, true))
    scroll_y = -1.0f;
  if (scroll_y != 0.0f) {
    float zoom_factor = 1.0f + scroll_y * 0.1f;
    camera->zoom_wanted *= zoom_factor;
    camera->zoom_wanted = glm_clamp(camera->zoom_wanted, 0.005f, 100.0f);
  }
  float smoothing_factor = 1.0f - expf(-10.0f * io->DeltaTime); // Adjust 10.0f for speed
  camera->zoom = camera->zoom + (camera->zoom_wanted - camera->zoom) * smoothing_factor;

  float window_ratio = (float)width / (float)height;
  float map_ratio = (float)handler->map_data.width / (float)handler->map_data.height;
  float aspect = (float)window_ratio / (float)map_ratio;
  if (!io->WantCaptureMouse && igIsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
    if (!camera->is_dragging) {
      camera->is_dragging = true;
      ImVec2 mouse_pos;
      igGetMousePos(&mouse_pos);
      camera->drag_start_pos[0] = mouse_pos.x;
      camera->drag_start_pos[1] = mouse_pos.y;
    }

    ImVec2 drag_delta;
    igGetMouseDragDelta(&drag_delta, ImGuiMouseButton_Right, 0.0f);
    float dx = drag_delta.x / (width * camera->zoom);
    float dy = drag_delta.y / (height * camera->zoom * aspect);
    float max_map_size = fmax(handler->map_data.width, handler->map_data.height) * 0.001;
    camera->pos[0] -= (dx * 2) / max_map_size;
    camera->pos[1] -= (dy * 2) / max_map_size;
    igResetMouseDragDelta(ImGuiMouseButton_Right);
  } else {
    camera->is_dragging = false;
  }
}

void camera_init(camera_t *camera) {
  memset(camera, 0, sizeof(camera_t));
  camera->zoom = 5.0f;
  camera->zoom_wanted = 5.0f;
}

void ui_init(ui_handler_t *ui, gfx_handler_t *gfx_handler) {
  ui->gfx_handler = gfx_handler;
  ui->show_timeline = false;
  memset(&ui->map_data, 0, sizeof(map_data_t));
  timeline_init(&ui->timeline);
  camera_init(&gfx_handler->renderer.camera);
  NFD_Init();
}

void ui_render(ui_handler_t *ui) {
  on_camera_update(ui->gfx_handler);
  render_menu_bar(ui);
  setup_docking(ui);
  if (ui->show_timeline)
    render_timeline(&ui->timeline);
  if (ui->timeline.selected_player_track_index != -1)
    render_player_info(&ui->timeline);
}

void ui_cleanup(ui_handler_t *ui) {
  timeline_cleanup(&ui->timeline);
  free_map_data(&ui->map_data);
  NFD_Quit();
}
