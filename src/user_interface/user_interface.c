#include "user_interface.h"
#include "../renderer/graphics_backend.h"
#include "../renderer/renderer.h"
#include "cimgui.h"
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
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.25f, NULL, &dock_id_right);
    ImGuiID dock_id_left = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.25f, NULL, &dock_id_left);

    // assign windows to docks
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    igDockBuilderDockWindow("Player Info", dock_id_left);
    igDockBuilderDockWindow("Players", dock_id_left);
    igDockBuilderDockWindow("Snippet Editor", dock_id_right);

    igDockBuilderFinish(main_dockspace_id);
  }
}

// ---------------- Snippet Editor Panel ----------------
static const char *dir_options[] = {"Left", "Neutral", "Right"};
static const char *weapon_options[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};

void render_snippet_editor_panel(timeline_state_t *ts) {
  if (igBegin("Snippet Editor", NULL, 0)) {
    if (ts->selected_snippet_id != -1) {
      // Find snippet globally, not tied to current selected player
      input_snippet_t *snippet = NULL;
      for (int i = 0; i < ts->player_track_count; i++) {
        snippet = find_snippet_by_id(&ts->player_tracks[i], ts->selected_snippet_id);
        if (snippet)
          break;
      }
      if (snippet && snippet->input_count > 0) {
        igText("Editing Snippet %d (%d inputs)", snippet->id, snippet->input_count);
        igSeparator();

        igBeginChild_Str("InputsScroll", (ImVec2){0, 0}, true, 0);
        for (int i = 0; i < snippet->input_count; i++) {
          igPushID_Int(i);
          SPlayerInput *inp = &snippet->inputs[i];
          if (igTreeNode_Ptr(inp, "Tick %d", snippet->start_tick + i)) {
            int dir_idx = inp->m_Direction + 1;
            if (igCombo_Str_arr("Direction", &dir_idx, dir_options, 3, -1))
              inp->m_Direction = dir_idx - 1;
            igInputInt("Target X", &inp->m_TargetX, 1, 10, 0);
            igInputInt("Target Y", &inp->m_TargetY, 1, 10, 0);
            igInputInt("TeleOut", &inp->m_TeleOut, 1, 10, 0);
            bool flag;
            flag = inp->m_Jump;
            if (igCheckbox("Jump", &flag))
              inp->m_Jump = flag;
            flag = inp->m_Fire;
            if (igCheckbox("Fire", &flag))
              inp->m_Fire = flag;
            flag = inp->m_Hook;
            if (igCheckbox("Hook", &flag))
              inp->m_Hook = flag;
            flag = inp->m_TeleOut;
            int weapon_idx = inp->m_WantedWeapon;
            if (igCombo_Str_arr("Weapon", &weapon_idx, weapon_options, 5, -1))
              inp->m_WantedWeapon = (uint8_t)weapon_idx;
            igTreePop();
          }
          igPopID();
        }
        igEndChild();
      } else {
        igText("Snippet has no inputs");
      }
    } else {
      igText("No snippet selected");
    }
  }
  igEnd();
}
// ---------------- Player Manager Panel ----------------
static bool g_remove_confirm_needed = true;
static int g_pending_remove_index = -1;

static void remove_player(timeline_state_t *ts, ph_t *ph, int index) {
  if (index < 0 || index >= ts->player_track_count)
    return;
  player_track_t *track = &ts->player_tracks[index];
  if (track->snippets) {
    for (int j = 0; j < track->snippet_count; j++) {
      free_snippet_inputs(&track->snippets[j]);
    }
    free(track->snippets);
  }
  // shift array
  for (int j = index; j < ts->player_track_count - 1; j++) {
    ts->player_tracks[j] = ts->player_tracks[j + 1];
  }
  ts->player_track_count--;
  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * ts->player_track_count);

  if (ts->selected_player_track_index == index)
    ts->selected_player_track_index = -1;
  else if (ts->selected_player_track_index > index)
    ts->selected_player_track_index--;
  wc_remove_character(&ph->world, index);
}

void render_player_manager(timeline_state_t *ts, ph_t *ph) {
  if (igBegin("Players", NULL, 0)) {
    for (int i = 0; i < ts->player_track_count; i++) {
      igPushID_Int(i);
      bool sel = (i == ts->selected_player_track_index);
      const char *label =
          ts->player_tracks[i].player_info.name[0] ? ts->player_tracks[i].player_info.name : "(Unnamed)";

      // Selectable only
      igSetNextItemAllowOverlap();
      if (igSelectable_Bool(label, sel, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){0, 0})) {
        ts->selected_player_track_index = i;
      }

      ImVec2 vMin;
      igGetContentRegionAvail(&vMin);
      // Place "X" button at row end
      igSameLine(vMin.x - 20.f, -1.0f); // shift right
      if (igSmallButton("X")) {
        if (g_remove_confirm_needed && ts->player_tracks[i].snippet_count > 0) {
          g_pending_remove_index = i;
          igPopID();
          igOpenPopup_Str("ConfirmRemovePlayer", ImGuiPopupFlags_AnyPopupLevel);
          igPushID_Int(i);
        } else {
          remove_player(ts, ph, i);
        }
      }
      igPopID();
    }
    igSeparator();
    if (ph->world.m_pCollision && igButton("Add Player", (ImVec2){0, 0})) {
      add_new_track(ts, ph);
    }

    if (igBeginPopupModal("ConfirmRemovePlayer", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
      igText("This player has inputs. Remove anyway?");
      static bool dont_ask_again = false;
      igCheckbox("Do not ask again", &dont_ask_again);
      if (igButton("Yes", (ImVec2){0, 0})) {
        remove_player(ts, ph, g_pending_remove_index);
        if (dont_ask_again)
          g_remove_confirm_needed = false;
        g_pending_remove_index = -1;
        igCloseCurrentPopup();
      }
      igSameLine(0, 10);
      if (igButton("Cancel", (ImVec2){0, 0})) {
        g_pending_remove_index = -1;
        igCloseCurrentPopup();
      }
      igEndPopup();
    }
  }
  igEnd();
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
    camera->zoom_wanted = glm_clamp(camera->zoom_wanted, 0.005f, 1000.0f);
  }
  float smoothing_factor = 1.0f - expf(-10.0f * io->DeltaTime); // Adjust 10.0f for speed
  camera->zoom = camera->zoom + (camera->zoom_wanted - camera->zoom) * smoothing_factor;

  float window_ratio = (float)width / (float)height;
  float map_ratio = (float)handler->map_data->width / (float)handler->map_data->height;
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
    float max_map_size = fmax(handler->map_data->width, handler->map_data->height) * 0.001;
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
  timeline_init(&ui->timeline);
  camera_init(&gfx_handler->renderer.camera);
  NFD_Init();
}

void render_players(ui_handler_t *ui) {
  gfx_handler_t *gfx = ui->gfx_handler;
  physics_handler_t *ph = &gfx->physics_handler;

  SWorldCore world = wc_empty();
  wc_copy_world(&world, &ph->world);
  for (int i = 0; i < ui->timeline.current_tick; ++i) {
    for (int p = 0; p < world.m_NumCharacters; ++p) {
      SPlayerInput input = get_input(&ui->timeline, p, i);
      cc_on_input(&world.m_pCharacters[p], &input);
    }
    wc_tick(&world);
  }
  for (int i = 0; i < ph->world.m_NumCharacters; ++i) {
    SCharacterCore *core = &world.m_pCharacters[i];
    vec2 p = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec4 color = {[3] = 1.f};
    memcpy(color, ui->timeline.player_tracks[i].player_info.color, 3 * sizeof(float));
    renderer_draw_circle_filled(gfx, p, 0.4375f, color, 32);
    if (core->m_HookState != 0)
      renderer_draw_line(gfx, p, (vec2){vgetx(core->m_HookPos) / 32.f, vgety(core->m_HookPos) / 32.f},
                         (vec4){.5f, .5f, .5f, 1.f}, 0.05);
  }
}

void ui_render(ui_handler_t *ui) {
  on_camera_update(ui->gfx_handler);
  render_menu_bar(ui);
  setup_docking(ui);
  if (ui->show_timeline) {
    render_timeline(&ui->timeline);
    render_player_manager(&ui->timeline, &ui->gfx_handler->physics_handler);
    render_snippet_editor_panel(&ui->timeline);
    if (ui->timeline.selected_player_track_index != -1)
      render_player_info(&ui->timeline);
  }
  render_players(ui);
}

void ui_cleanup(ui_handler_t *ui) {
  timeline_cleanup(&ui->timeline);
  NFD_Quit();
}
