#include "user_interface.h"
#include "cglm/vec2.h"
#include "cimgui.h"
#include "ddnet_map_loader.h"
#include "ddnet_physics/collision.h"
#include "demo.h"
#include "net_events.h"
#include "player_info.h"
#include "skin_browser.h"
#include "snippet_editor.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "undo_redo.h"
#include "widgets/hsl_colorpicker.h"
#include "widgets/imcol.h"
#include <animation/anim_data.h>
#include <ddnet_physics/gamecore.h>
#include <limits.h>
#include <logger/logger.h>
#include <math.h>
#include <nfd.h>
#include <plugins/api_impl.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <symbols.h>
#include <system/config.h>
#include <system/include_cimgui.h>
#include <system/save.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *LOG_SOURCE = "UI";

void render_menu_bar(ui_handler_t *ui) {
  bool open_export_popup = false;
  if (igBeginMainMenuBar()) {
    if (igBeginMenu("File", true)) {
      if (igMenuItem_Bool("Open Map", NULL, false, true)) {
        nfdu8char_t *out_path;
        nfdu8filteritem_t filters[] = {{"map files", "map"}};
        nfdopendialogu8args_t args = {0};
        args.filterList = filters;
        args.filterCount = 1;
        nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
        if (result == NFD_OKAY) {
          on_map_load_path(ui->gfx_handler, out_path);
          NFD_FreePathU8(out_path);
        } else if (result == NFD_CANCEL) log_warn(LOG_SOURCE, "Canceled map load.");
        else log_error(LOG_SOURCE, "Error: %s", NFD_GetError());
      }
      igSeparator();
      if (igMenuItem_Bool("Open Project", "Ctrl+O", false, true)) {
        nfdu8char_t *out_path;
        nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
        nfdopendialogu8args_t args = {0};
        args.filterList = filters;
        args.filterCount = 1;
        nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
        if (result == NFD_OKAY) {
          load_project(ui, out_path);
          NFD_FreePathU8(out_path);
        }
      }
      if (igMenuItem_Bool("Save Project As...", "Ctrl+S", false, true)) {
        nfdu8char_t *save_path;
        nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
        nfdresult_t result = NFD_SaveDialogU8(&save_path, filters, 1, NULL, "unnamed.tasp");
        if (result == NFD_OKAY) {
          save_project(ui, save_path);
          NFD_FreePathU8(save_path);
        }
      }
      igSeparator();
      if (igMenuItem_Bool("Export Demo...", NULL, false, ui->gfx_handler->physics_handler.loaded)) {
        demo_exporter_t *dx = &ui->demo_exporter;
        // Set default values when opening the popup
        dx->num_ticks = model_get_max_timeline_tick(&ui->timeline);
        if (strlen(dx->map_name) == 0) {
          strncpy(dx->map_name, "unnamed_map", sizeof(dx->map_name) - 1);
        }
        open_export_popup = true;
      }
      igEndMenu();
    }

    // Edit menu
    if (igBeginMenu("Edit", true)) {
      bool can_undo = undo_manager_can_undo(&ui->undo_manager);
      if (igMenuItem_Bool("Undo", "Ctrl+Z", false, can_undo)) {
        undo_manager_undo(&ui->undo_manager, &ui->timeline);
      }
      bool can_redo = undo_manager_can_redo(&ui->undo_manager);
      if (igMenuItem_Bool("Redo", "Ctrl+Y", false, can_redo)) {
        undo_manager_redo(&ui->undo_manager, &ui->timeline);
      }
      igEndMenu();
    }

    // view menu
    if (igBeginMenu("View", true)) {
      igMenuItem_BoolPtr("Timeline", NULL, &ui->show_timeline, true);
      igMenuItem_BoolPtr("Controls", NULL, &ui->keybinds.show_settings_window, true);
      igMenuItem_BoolPtr("Undo History", NULL, &ui->undo_manager.show_history_window, true);
      igMenuItem_BoolPtr("Show prediction", NULL, &ui->show_prediction, true);
      igMenuItem_BoolPtr("Show skin manager", NULL, &ui->show_skin_browser, true);
      igMenuItem_BoolPtr("Show net events", NULL, &ui->show_net_events_window, true);
      igEndMenu();
    }

    if (igBeginMenu("Settings", true)) {
      if (igBeginMenu("Graphics", true)) {
        if (igCheckbox("VSync", &ui->vsync)) {
          ui->gfx_handler->g_swap_chain_rebuild = true;
        }

        igCheckbox("Show FPS", &ui->show_fps);

        igSliderInt("FPS Limit", &ui->fps_limit, 0, 1000, "%d", 0);
        if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("0 = Unlimited");

        if (igDragFloat("LOD Bias", &ui->lod_bias, 0.1f, -5.0f, 5.0f, "%.1f", 0)) {
          ui->gfx_handler->renderer.lod_bias = ui->lod_bias;
        }

        igColorEdit3("Background Color", ui->bg_color, ImGuiColorEditFlags_NoInputs);
        igSeparator();
        igDragFloat("Prediction alpha own", &ui->prediction_alpha[0], 0.1f, 0.0f, 1.0f, "%.3f", 0);
        igDragFloat("Prediction alpha others", &ui->prediction_alpha[1], 0.1f, 0.0f, 1.0f, "%.3f", 0);
        igCheckbox("Show center dot", &ui->center_dot);

        igEndMenu();
      }
      igEndMenu();
    }

    const char *button_text = "Reload Plugins";
    ImVec2 button_size;
    igCalcTextSize(&button_size, button_text, NULL, false, 0.0f);
    button_size.x += igGetStyle()->FramePadding.x * 2.0f;
    ImVec2 region_avail;
    igGetContentRegionAvail(&region_avail);

    float fps_width = 0.0f;
    char fps_text[64];
    if (ui->show_fps) {
      ImGuiIO *io = igGetIO_Nil();
      snprintf(fps_text, sizeof(fps_text), "FPS: %.1f (%.2f ms) | ", io->Framerate, 1000.0f / io->Framerate);
      ImVec2 fps_size;
      igCalcTextSize(&fps_size, fps_text, NULL, false, 0.0f);
      fps_width = fps_size.x;
    }

    igSetCursorPosX(igGetCursorPosX() + region_avail.x - button_size.x - fps_width);

    if (ui->show_fps) {
      igText("%s", fps_text);
      igSameLine(0, 0);
    }

    if (igButton(button_text, (ImVec2){0, 0})) plugin_manager_reload_all(&ui->plugin_manager, "plugins");

    igEndMainMenuBar();
  }
  if (open_export_popup) {
    igOpenPopup_Str("Demo Export", ImGuiPopupFlags_AnyPopupLevel);
  }
}

// docking setup
void setup_docking(void) {
  ImGuiID main_dockspace_id = igGetID_Str("MainDockSpace");

  // Ensure the dockspace covers the entire viewport initially
  ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos(viewport->WorkPos, ImGuiCond_Always, (ImVec2){0.0f, 0.0f});
  igSetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  igSetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
  host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

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

  // build the initial layout programmatically --
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // split root into bottom + top remainder
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom = igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.20f, NULL, &dock_id_top);

    // split top remainder into left + remainder
    ImGuiID dock_id_left;
    ImGuiID dock_id_center;
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.25f, NULL, &dock_id_center);
    dock_id_left = igDockBuilderSplitNode(dock_id_center, ImGuiDir_Left, 0.40f, NULL, &dock_id_center);

    igDockBuilderDockWindow("viewport", dock_id_center);
    igDockBuilderDockWindow("Controls", dock_id_center);
    igDockBuilderDockWindow("Skin Browser", dock_id_center);

    igDockBuilderDockWindow("Timeline", dock_id_bottom);

    igDockBuilderDockWindow("Player Info", dock_id_left);
    igDockBuilderDockWindow("Players", dock_id_left);
    igDockBuilderDockWindow("Skin manager", dock_id_left);

    igDockBuilderDockWindow("Snippet Editor", dock_id_right);
    igDockBuilderFinish(main_dockspace_id);
  }
}

// player manager panel
static bool g_remove_confirm_needed = true;
static int g_pending_remove_index = -1;

void render_player_manager(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  physics_handler_t *ph = &ui->gfx_handler->physics_handler;
  float dpi_scale = gfx_get_ui_scale();
  if (igBegin("Players", NULL, 0)) {
    static int num_to_add = 1;
    igPushItemWidth(50 * dpi_scale);
    igDragInt("##NumToAdd", &num_to_add, 1, 1, 1000, "%d", ImGuiSliderFlags_None);
    // igInputInt("##NumToAdd", &num_to_add, 0, 0, 0);
    igPopItemWidth();
    if (num_to_add < 1) num_to_add = 1;

    igSameLine(0, 5.0f * dpi_scale);

    char aLabel[16];
    snprintf(aLabel, 16, "Add Player%s", num_to_add > 1 ? "s" : "");
    if (ph->world.m_pCollision && igButton(aLabel, (ImVec2){0, 0})) {
      for (int i = 0; i < num_to_add; ++i) {
        undo_command_t *cmd = timeline_api_create_track(ui, NULL, NULL);
        if (cmd) undo_manager_register_command(&ui->undo_manager, cmd);
      }
    }
    // igSameLine(0, 10.f);
    // if (ph->world.m_pCollision && igButton("Add 1000 Players", (ImVec2){0, 0})) {
    //   add_new_track(ts, ph, 1000);
    // }
    igSameLine(0, 10.f * dpi_scale);
    igText("Players: %d", ts->player_track_count);

    igSeparator();
    for (int i = 0; i < ts->player_track_count; i++) {
      igPushID_Int(i);
      bool sel = (i == ts->selected_player_track_index);
      const char *label = ts->player_tracks[i].player_info.name[0] ? ts->player_tracks[i].player_info.name : "nameless tee";

      // Selectable only
      igSetNextItemAllowOverlap();
      if (igSelectable_Bool(label, sel, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){0, 0})) {
        ts->selected_player_track_index = i;
      }

      ImVec2 vMin;
      igGetContentRegionAvail(&vMin);
      igSameLine(vMin.x - 20.f * gfx_get_ui_scale(), -1.0f); // shift right
      if (igSmallButton(ICON_KI_TRASH)) {
        if (g_remove_confirm_needed && ts->player_tracks[i].snippet_count > 0) {
          g_pending_remove_index = i;
          igPopID();
          igOpenPopup_Str("Confirm remove player", ImGuiPopupFlags_AnyPopupLevel);
          igPushID_Int(i);
        } else {
          undo_command_t *cmd = commands_create_remove_track(ui, i);
          undo_manager_register_command(&ui->undo_manager, cmd);
        }
      }
      igPopID();
    }
    if (ts->player_track_count > 0) igSeparator();
  }
  if (igBeginPopupModal("Confirm remove player", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
    igText("This player has inputs. Remove anyway?");
    static bool dont_ask_again = false;
    igCheckbox("Do not ask again", &dont_ask_again);
    if (igButton("Yes", (ImVec2){0, 0})) {
      undo_command_t *cmd = commands_create_remove_track(ui, g_pending_remove_index);
      undo_manager_register_command(&ui->undo_manager, cmd);
      if (dont_ask_again) g_remove_confirm_needed = false;
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
  igEnd();
}

void on_camera_update(gfx_handler_t *handler, bool hovered) {
  if (!handler->map_data || !handler->map_data->game_layer.data) return;
  camera_t *camera = &handler->renderer.camera;
  ImGuiIO *io = igGetIO_Nil();

  float scroll_y = !hovered ? 0.0f : io->MouseWheel;
  if (!igIsAnyItemActive()) { // Prevent shortcuts while typing in a text field
    if (keybinds_is_action_pressed(&handler->user_interface.keybinds, ACTION_ZOOM_IN, true)) scroll_y = 1.0f;
    if (keybinds_is_action_pressed(&handler->user_interface.keybinds, ACTION_ZOOM_OUT, true)) scroll_y = -1.0f;
  }
  if (scroll_y != 0.0f) {
    float zoom_factor = 1.0f + scroll_y * 0.1f;
    camera->zoom_wanted *= zoom_factor;
    camera->zoom_wanted = glm_clamp(camera->zoom_wanted, 0.005f, 1000.0f);
  }
  float smoothing_factor = 1.0f - expf(-10.0f * io->DeltaTime); // Adjust 10.0f for speed
  camera->zoom = camera->zoom + (camera->zoom_wanted - camera->zoom) * smoothing_factor;

  float viewport_ratio = (float)handler->viewport[0] / (float)handler->viewport[1];
  float map_ratio = (float)handler->map_data->width / (float)handler->map_data->height;
  float aspect = (float)viewport_ratio / (float)map_ratio;
  if (handler->user_interface.timeline.recording) {
  } else if (hovered && igIsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
    if (!camera->is_dragging) {
      camera->is_dragging = true;
      ImVec2 mouse_pos;
      igGetMousePos(&mouse_pos);
      camera->drag_start_pos[0] = mouse_pos.x;
      camera->drag_start_pos[1] = mouse_pos.y;
    }

    ImVec2 drag_delta;
    igGetMouseDragDelta(&drag_delta, ImGuiMouseButton_Right, 0.0f);
    float dx = drag_delta.x / (handler->viewport[0] * camera->zoom);
    float dy = drag_delta.y / (handler->viewport[1] * camera->zoom * aspect);
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

void ui_init_config(ui_handler_t *ui) {
  ui->mouse_sens = 80.f;
  ui->mouse_max_distance = 400.f;
  ui->vsync = true;
  ui->fps_limit = 0;
  ui->lod_bias = -0.5f;
  ui->bg_color[0] = 30.f / 255.f;
  ui->bg_color[1] = 35.f / 255.f;
  ui->bg_color[2] = 40.f / 255.f;
  ui->prediction_alpha[0] = 1.0f;
  ui->prediction_alpha[1] = 1.0f;
  ui->center_dot = 1;

  keybinds_init(&ui->keybinds);
  config_load(ui);
}

void ui_init(ui_handler_t *ui, gfx_handler_t *gfx_handler) {
  ImGuiIO *io = igGetIO_Nil();
  ImFontAtlas *atlas = io->Fonts;

  float scale = gfx_get_ui_scale();

  ui->font = ImFontAtlas_AddFontFromFileTTF(io->Fonts, "data/fonts/Roboto-SemiBold.ttf", 19.f * scale, NULL, NULL);

  ImFontConfig *config = ImFontConfig_ImFontConfig();
  config->MergeMode = true;
  config->GlyphMinAdvanceX = 13.0f;
  config->GlyphOffset = (ImVec2){0.0f, 1.0f};

  ImFontAtlas_AddFontFromFileTTF(atlas, "data/fonts/kenney-icon-font.ttf", 14.0f * scale, config, NULL);

  ImFontConfig_destroy(config);

  ui->gfx_handler = gfx_handler;
  ui->show_timeline = true;
  ui->show_prediction = true;
  ui->prediction_length = 100;
  ui->show_skin_browser = false;
  ui->show_net_events_window = false;
  particle_system_init(&ui->particle_system);
  timeline_init(ui);
  camera_init(&gfx_handler->renderer.camera);
  undo_manager_init(&ui->undo_manager);
  skin_manager_init(&ui->skin_manager);
  NFD_Init();

  ui->plugin_api = api_init(ui);
  ui->plugin_context.ui_handler = ui;
  ui->plugin_context.timeline = &ui->timeline;
  ui->plugin_context.gfx_handler = gfx_handler;
  ui->plugin_context.imgui_context = igGetCurrentContext();
  plugin_manager_init(&ui->plugin_manager, &ui->plugin_context, &ui->plugin_api);
  plugin_manager_load_all(&ui->plugin_manager, "plugins");

  particle_system_init(&ui->particle_system);

  ui->num_pickups = 0;
  ui->pickups = NULL;
  ui->pickup_positions = NULL;
}

static float lint2(float a, float b, float f) { return a + f * (b - a); }
static void lerp(vec2 a, vec2 b, float f, vec2 out) {
  out[0] = lint2(a[0], b[0], f);
  out[1] = lint2(a[1], b[1], f);
}

static void process_net_events(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;

  if (ts->current_tick < ts->last_event_scan_tick)
    ts->last_event_scan_tick = ts->current_tick;

  // Only process if playing and not skipping too much
  if (!ts->is_playing || abs(ts->current_tick - ts->last_event_scan_tick) > 100) {
    ts->last_event_scan_tick = ts->current_tick;
    return;
  }

  // Iterate events in range [last_scan + 1, current_tick]
  /*   for (int i = 0; i < ts->net_event_count; ++i) {
      net_event_t *ev = &ts->net_events[i];
      if (ev->tick > ts->last_event_scan_tick && ev->tick <= ts->current_tick) {
        if (ev->type == NET_EVENT_SOUND_GLOBAL) {
        }
      }
    } */
  ts->last_event_scan_tick = ts->current_tick;
}

void render_players(ui_handler_t *ui) {
  gfx_handler_t *gfx = ui->gfx_handler;
  physics_handler_t *ph = &gfx->physics_handler;
  if (!ph->loaded) return;

  SWorldCore prev_world = wc_empty();
  SWorldCore world = wc_empty();

  // Get the world state at the current tick. The model handles caching internally.
  model_get_world_state_at_tick(&ui->timeline, ui->timeline.current_tick - 1, &prev_world, true);
  model_get_world_state_at_tick(&ui->timeline, ui->timeline.current_tick, &world, true);

  if (ui->timeline.player_track_count != world.m_NumCharacters) {
    wc_free(&prev_world);
    wc_free(&world);
    return;
  }

  float speed_scale = ui->timeline.is_reversing ? 2.0f : 1.0f;
  float intra = fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / (ui->timeline.playback_speed * speed_scale)), 1.f);
  if (ui->timeline.is_reversing) intra = 1.f - intra;

  if (ui->timeline.recording) {
    SCharacterCore *core = &world.m_pCharacters[gfx->user_interface.timeline.selected_player_track_index];
    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    glm_vec2_copy(p, ui->last_render_pos);
    ui->gfx_handler->renderer.camera.pos[0] = (p[0]) / ui->gfx_handler->map_data->width;
    ui->gfx_handler->renderer.camera.pos[1] = (p[1]) / ui->gfx_handler->map_data->height;
  }

  for (int i = 0; i < world.m_NumCharacters; ++i) {
    SCharacterCore *core = &world.m_pCharacters[i];

    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    anim_state_t anim_state;
    anim_state_set(&anim_state, &anim_base, 0.0f);

    bool stationary = fabsf(vgetx(core->m_Vel) * 256.f) <= 1;
    bool running = fabsf(vgetx(core->m_Vel) * 256.f) >= 5000;
    bool want_other_dir = (core->m_Input.m_Direction == -1 && vgetx(core->m_Vel) > 0) || (core->m_Input.m_Direction == 1 && vgetx(core->m_Vel) < 0);
    bool inactive = get_flag_sit(&core->m_Input);
    bool in_air = !(core->m_pCollision->m_pTileInfos[core->m_BlockIdx] & INFO_CANGROUND) ||
                  !(check_point(core->m_pCollision, vec2_init(vgetx(core->m_Pos), vgety(core->m_Pos) + 16)));
    float attack_ticks_passed = (world.m_GameTick - core->m_AttackTick) + intra;
    float last_attack_time = attack_ticks_passed / (float)GAME_TICK_SPEED;

    float walk_time = fmod(p[0] * 32.f, 100.0f) / 100.0f;
    float run_time = fmod(p[0] * 32.f, 200.0f) / 200.0f;
    if (walk_time < 0.0f) walk_time += 1.0f;
    if (run_time < 0.0f) run_time += 1.0f;

    if (in_air) anim_state_add(&anim_state, &anim_inair, 0.0f, 1.0f);
    else if (stationary) {
      if (inactive) anim_state_add(&anim_state, core->m_Input.m_Direction < 0 ? &anim_sit_left : &anim_sit_right, 0.0f, 1.0f);
      else anim_state_add(&anim_state, &anim_idle, 0.0f, 1.0f);
    } else if (!want_other_dir) {
      if (running) anim_state_add(&anim_state, vgetx(core->m_Vel) < 0.0f ? &anim_run_left : &anim_run_right, run_time, 1.0f);
      else anim_state_add(&anim_state, &anim_walk, walk_time, 1.0f);
    }
    if (core->m_ActiveWeapon == WEAPON_HAMMER)
      anim_state_add(&anim_state, &anim_hammer_swing, last_attack_time * 5.f, 1.0f);
    if (core->m_ActiveWeapon == WEAPON_NINJA)
      anim_state_add(&anim_state, &anim_ninja_swing, last_attack_time * 2.f, 1.0f);

    vec2 dir;
    if (ui->timeline.recording && i == ui->timeline.selected_player_track_index) {
      dir[0] = ui->recording_mouse_pos[0];
      dir[1] = ui->recording_mouse_pos[1];
    } else {
      dir[0] = core->m_Input.m_TargetX;
      dir[1] = core->m_Input.m_TargetY;
    }

    glm_vec2_normalize(dir);
    player_info_t *info = &ui->timeline.player_tracks[i].player_info;
    int skin = info->skin;
    int eye = get_flag_eye_state(&core->m_Input);
    vec3 feet_col = {1.f, 1.f, 1.f};
    vec3 body_col = {0.0f, 0.0f, 0.0f};
    bool custom_col = info->use_custom_color;

    if (core->m_FreezeTime > 0 || core->m_ActiveWeapon == WEAPON_NINJA) {
      skin = gfx->x_ninja_skin;
      if (core->m_FreezeTime > 0 && eye == 0) eye = EYE_BLINK;
      custom_col = false;
    }

    if (custom_col) {
      packed_hsl_to_rgb(info->color_body, body_col);
      packed_hsl_to_rgb(info->color_feet, feet_col);
    }
    if (core->m_JumpedTotal >= core->m_Jumps - 1) {
      if (custom_col) {
        feet_col[0] *= 0.5f;
        feet_col[1] *= 0.5f;
        feet_col[2] *= 0.5f;
      } else {
        feet_col[0] = 0.5f;
      }
    }

    renderer_submit_skin(gfx, Z_LAYER_SKINS, p, 1.0f, skin, eye, dir, &anim_state, body_col, feet_col, custom_col);

    if (!ui->timeline.recording && i == ui->timeline.selected_player_track_index) {
      // vec2 box_size = {2.0f, 2.0f};
      vec2 min_pos = {p[0] - 1.0f, p[1] - 1.0f};
      vec4 red_col = {1.0f, 0.0f, 0.0f, 1.0f};
      vec2 p1 = {min_pos[0], min_pos[1]};
      vec2 p2 = {min_pos[0] + 2.0f, min_pos[1]};
      vec2 p3 = {min_pos[0] + 2.0f, min_pos[1] + 2.0f};
      vec2 p4 = {min_pos[0], min_pos[1] + 2.0f};

      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p1, p2, red_col, 0.05f);
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p2, p3, red_col, 0.05f);
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p3, p4, red_col, 0.05f);
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p4, p1, red_col, 0.05f);
    }
    if (ui->center_dot) {
      int idx = (int)p[1] * world.m_pCollision->m_MapData.width + (int)p[0];
      bool freeze = world.m_pCollision->m_MapData.game_layer.data[idx] == TILE_FREEZE;
      if (!freeze && world.m_pCollision->m_MapData.front_layer.data && world.m_pCollision->m_MapData.front_layer.data[idx] == TILE_FREEZE)
        freeze = true;
      renderer_submit_circle_filled(gfx, Z_LAYER_PREDICTION_LINES + 1.0f, p, 2.f / 32.f, freeze ? (vec4){0, 0, 1, 1} : (vec4){0, 1, 0, 1}, 4);
    }

    SCharacterCore *prev_core = &prev_world.m_pCharacters[i];
    // render hook
    if (core->m_HookState >= 1) {

      // do interpolation
      vec2 hook_pos;
      {
        vec2 __ = {vgetx(prev_core->m_HookPos) / 32.f, vgety(prev_core->m_HookPos) / 32.f};
        vec2 _ = {vgetx(core->m_HookPos) / 32.f, vgety(core->m_HookPos) / 32.f};
        if (core->m_HookedPlayer != -1) {
          SCharacterCore *hooked = &world.m_pCharacters[core->m_HookedPlayer];
          __[0] = vgetx(hooked->m_PrevPos) / 32.f;
          __[1] = vgety(hooked->m_PrevPos) / 32.f;
          _[0] = vgetx(hooked->m_Pos) / 32.f;
          _[1] = vgety(hooked->m_Pos) / 32.f;
        }
        lerp(__, _, intra, hook_pos);
      }

      vec2 direction;
      glm_vec2_sub(hook_pos, p, direction);
      float length = glm_vec2_norm(direction);
      glm_vec2_normalize(direction);
      float angle = atan2f(-direction[1], direction[0]);

      if (length > 0) {
        vec2 center_pos;
        center_pos[0] = p[0] + direction[0] * (length - 0.5f) * 0.5f;
        center_pos[1] = p[1] + direction[1] * (length - 0.5f) * 0.5f;
        vec2 chain_size = {-length + 0.5f, 0.5};
        renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_HOOK, center_pos, chain_size, angle, GAMESKIN_HOOK_CHAIN, true, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);
      }
      sprite_definition_t *head_sprite_def = &gfx->renderer.gameskin_renderer.sprite_definitions[GAMESKIN_HOOK_HEAD];
      vec2 head_size = {(float)head_sprite_def->w / 64.0f, (float)head_sprite_def->h / 64.0f};
      renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_HOOK, hook_pos, head_size, angle, GAMESKIN_HOOK_HEAD, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);
    }
    if (!core->m_FreezeTime && core->m_ActiveWeapon < NUM_WEAPONS) {
      const weapon_spec_t *spec = &game_data.weapons.id[core->m_ActiveWeapon];
      float aim_angle = atan2f(-dir[1], dir[0]);

      bool is_sit = inactive && !in_air && stationary;
      float flip_factor = (dir[0] < 0.0f) ? -1.0f : 1.0f;

      // Start with interpolated physics position
      vec2 phys_pos_prev = {vgetx(core->m_PrevPos), vgety(core->m_PrevPos)};
      vec2 phys_pos_curr = {vgetx(core->m_Pos), vgety(core->m_Pos)};
      vec2 phys_pos;
      lerp(phys_pos_prev, phys_pos_curr, intra, phys_pos);

      vec2 weapon_pos; // This will be in physics units until the end
      glm_vec2_copy(phys_pos, weapon_pos);

      float anim_attach_angle_rad = anim_state.attach.angle * (2.0f * M_PI);
      float weapon_angle = anim_attach_angle_rad + aim_angle;

      int weapon_sprite_id = -1;

      if (core->m_ActiveWeapon == WEAPON_HAMMER) {
        weapon_sprite_id = GAMESKIN_HAMMER_BODY;
        weapon_pos[0] += anim_state.attach.x;
        weapon_pos[1] += anim_state.attach.y;
        weapon_pos[1] += spec->offsety;
        if (dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;
        if (is_sit) weapon_pos[1] += 3.0f;

        if (!inactive) {
          anim_attach_angle_rad = anim_state.attach.angle * (2.0f * M_PI);
          weapon_angle = M_PI / 2.0f - flip_factor * anim_attach_angle_rad;
        } else {
          weapon_angle = dir[0] < 0.0 ? 100.f : 500.f;
        }
      } else if (core->m_ActiveWeapon == WEAPON_NINJA) {
        weapon_sprite_id = GAMESKIN_NINJA_BODY;
        weapon_pos[1] += spec->offsety;
        if (is_sit) weapon_pos[1] += 3.0f;
        if (dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;

        anim_attach_angle_rad = anim_state.attach.angle * (2.0f * M_PI);
        weapon_angle = -M_PI / 2.0f + flip_factor * anim_attach_angle_rad;

        float attack_time_sec = attack_ticks_passed / (float)GAME_TICK_SPEED;
        if (attack_time_sec <= 1.0f / 6.0f && spec->num_muzzles > 0) {

          int muzzle_idx = world.m_GameTick % spec->num_muzzles;
          vec2 hadoken_dir = {vgetx(core->m_Pos) - vgetx(prev_core->m_Pos), vgety(core->m_Pos) - vgety(prev_core->m_Pos)};
          if (glm_vec2_norm2(hadoken_dir) < 0.0001f) {
            hadoken_dir[0] = 1.0f;
            hadoken_dir[1] = 0.0f;
          }
          glm_vec2_normalize(hadoken_dir);

          float hadoken_angle = atan2f(-hadoken_dir[1], hadoken_dir[0]);
          vec2 muzzle_phys_pos;
          glm_vec2_copy(phys_pos, muzzle_phys_pos);
          muzzle_phys_pos[0] -= hadoken_dir[0] * spec->muzzleoffsetx;
          muzzle_phys_pos[1] -= hadoken_dir[1] * spec->muzzleoffsetx;

          int muzzle_sprite_id = GAMESKIN_NINJA_MUZZLE1 + muzzle_idx;
          sprite_definition_t *muzzle_sprite_def = &gfx->renderer.gameskin_renderer.sprite_definitions[muzzle_sprite_id];
          float f = sqrtf(powf(muzzle_sprite_def->w, 2) + powf(muzzle_sprite_def->h, 2));
          float scaleX = muzzle_sprite_def->w / f;
          float scaleY = muzzle_sprite_def->h / f;
          vec2 muzzle_size = {160.0f * scaleX / 32.0f, 160.0f * scaleY / 32.0f};

          vec2 render_pos = {muzzle_phys_pos[0] / 32.0f, muzzle_phys_pos[1] / 32.0f};
          renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_WEAPONS, render_pos, muzzle_size, hadoken_angle, muzzle_sprite_id, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);
        }
      } else {
        switch (core->m_ActiveWeapon) {
        case WEAPON_GUN:
          weapon_sprite_id = GAMESKIN_GUN_BODY;
          break;
        case WEAPON_SHOTGUN:
          weapon_sprite_id = GAMESKIN_SHOTGUN_BODY;
          break;
        case WEAPON_GRENADE:
          weapon_sprite_id = GAMESKIN_GRENADE_BODY;
          break;
        case WEAPON_LASER:
          weapon_sprite_id = GAMESKIN_LASER_BODY;
          break;
        }

        float recoil = 0.0f;
        float a = attack_ticks_passed / 5.0f;
        if (a < 1.0f) recoil = sinf(a * M_PI);

        weapon_pos[0] += dir[0] * (spec->offsetx - recoil * 10.0f);
        weapon_pos[1] += dir[1] * (spec->offsetx - recoil * 10.0f);
        weapon_pos[1] += spec->offsety;

        if (is_sit) weapon_pos[1] += 3.0f;

        if ((core->m_ActiveWeapon == WEAPON_GUN || core->m_ActiveWeapon == WEAPON_SHOTGUN) && spec->num_muzzles > 0) {
          if (attack_ticks_passed > 0 && attack_ticks_passed < spec->muzzleduration + 3.0f) {
            int muzzle_idx = world.m_GameTick % spec->num_muzzles;
            vec2 muzzle_dir_y = {-dir[1], dir[0]};
            float offset_y = -spec->muzzleoffsety * flip_factor;

            vec2 muzzle_phys_pos;
            glm_vec2_copy(weapon_pos, muzzle_phys_pos);
            muzzle_phys_pos[0] += dir[0] * spec->muzzleoffsetx + muzzle_dir_y[0] * offset_y;
            muzzle_phys_pos[1] += dir[1] * spec->muzzleoffsetx + muzzle_dir_y[1] * offset_y;

            int muzzle_sprite_id = (core->m_ActiveWeapon == WEAPON_GUN ? GAMESKIN_GUN_MUZZLE1 : GAMESKIN_SHOTGUN_MUZZLE1) + muzzle_idx;

            float w = 96.0f, h = 64.0f;
            float f = sqrtf(w * w + h * h);
            float scale_x = w / f;
            float scale_y = h / f;

            vec2 muzzle_size;
            muzzle_size[0] = spec->visual_size * scale_x * (4.0f / 3.0f) / 32.0f;
            muzzle_size[1] = spec->visual_size * scale_y / 32.0f;
            muzzle_size[1] *= flip_factor;

            vec2 render_pos = {muzzle_phys_pos[0] / 32.0f, muzzle_phys_pos[1] / 32.0f};
            renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_WEAPONS, render_pos, muzzle_size, weapon_angle, muzzle_sprite_id, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);
          }
        }
      }

      if (weapon_sprite_id != -1) {
        sprite_definition_t *sprite_def = &gfx->renderer.gameskin_renderer.sprite_definitions[weapon_sprite_id];
        float w = sprite_def->w;
        float h = sprite_def->h;
        float f = sqrtf(w * w + h * h);
        float scaleX = w / f;
        float scaleY = h / f;

        vec2 weapon_size = {spec->visual_size * scaleX / 32.0f, spec->visual_size * scaleY / 32.0f};
        weapon_size[1] *= flip_factor;

        vec2 render_pos = {weapon_pos[0] / 32.0f, weapon_pos[1] / 32.0f};

        renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_WEAPONS, render_pos, weapon_size, weapon_angle, weapon_sprite_id, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);
      }
    }
  }
  int id = 0;
  for (SProjectile *ent = (SProjectile *)world.m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; ent;
       ent = (SProjectile *)ent->m_Base.m_pNextTypeEntity) {
    float pt = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick - 1) / (float)GAME_TICK_SPEED;
    float ct = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick) / (float)GAME_TICK_SPEED;
    mvec2 prev_pos = prj_get_pos(ent, pt);
    mvec2 cur_pos = prj_get_pos(ent, ct);

    vec2 ppp = {vgetx(prev_pos) / 32.f, vgety(prev_pos) / 32.f};
    vec2 pp = {vgetx(cur_pos) / 32.f, vgety(cur_pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    renderer_submit_atlas(gfx, &gfx->renderer.gameskin_renderer, Z_LAYER_PROJECTILES, p, (vec2){1, 1}, -((world.m_GameTick + intra) / 50.f) * 4 * M_PI + id, GAMESKIN_GRENADE_PROJ, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, false);

    ++id;
  }
  (void)id;
  for (SLaser *ent = (SLaser *)world.m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; ent; ent = (SLaser *)ent->m_Base.m_pNextTypeEntity) {
    vec2 p1 = {vgetx(ent->m_Base.m_Pos) / 32.f, vgety(ent->m_Base.m_Pos) / 32.f};
    vec2 p0 = {vgetx(ent->m_From) / 32.f, vgety(ent->m_From) / 32.f};

    vec4 lsr_col = {0.f, 0.f, 1.f, 0.9f};
    vec4 sg_col = {0.570315f, 0.4140625f, 025.f, 0.9f};

    renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p0, p1, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 0.25f);
    renderer_submit_circle_filled(gfx, Z_LAYER_PREDICTION_LINES, p0, 0.2, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 8);
  }

  if (ui->timeline.selected_player_track_index >= 0) {
    SCharacterCore *p = &world.m_pCharacters[ui->timeline.selected_player_track_index];
    ui->pos_x = vgetx(p->m_Pos) - 200 * 32;
    ui->pos_y = vgety(p->m_Pos) - 200 * 32;
    ui->vel_x = vgetx(p->m_Vel);
    ui->vel_y = vgety(p->m_Vel);
    ui->vel_m = p->m_VelMag;
    ui->vel_r = p->m_VelRamp;
    ui->freezetime = p->m_FreezeTime;
    ui->reloadtime = p->m_ReloadTimer;
    ui->weapon = p->m_ActiveWeapon;
    for (int i = 0; i < NUM_WEAPONS; ++i)
      ui->weapons[i] = p->m_aWeaponGot[i];
  }

  if (ui->timeline.selected_player_track_index < 0 || !ui->show_prediction) {
    wc_free(&prev_world);
    wc_free(&world);
    return;
  }

  for (int i = 0; i < world.m_NumCharacters; ++i) {
    SCharacterCore *core = &world.m_pCharacters[i];
    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);
    vec4 color = {[3] = ui->prediction_alpha[i != ui->timeline.selected_player_track_index]};
    if (core->m_FreezeTime > 0) color[0] = 1.f;
    else color[1] = 1.f;
    renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, pp, p, color, 0.05);
  }

  for (SProjectile *ent = (SProjectile *)world.m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; ent;
       ent = (SProjectile *)ent->m_Base.m_pNextTypeEntity) {
    float pt = (world.m_GameTick - ent->m_StartTick - 1) / (float)GAME_TICK_SPEED;
    float ct = (world.m_GameTick - ent->m_StartTick) / (float)GAME_TICK_SPEED;
    mvec2 prev_pos = prj_get_pos(ent, pt);
    mvec2 cur_pos = prj_get_pos(ent, ct);
    vec2 ppp = {vgetx(prev_pos) / 32.f, vgety(prev_pos) / 32.f};
    vec2 pp = {vgetx(cur_pos) / 32.f, vgety(cur_pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    vec4 color = {1.0f, 0.5f, 0.5f, 0.8f};
    renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, pp, p, color, 0.05f);
  }

  // draw the rest of the lines
  for (int t = 0; t < ui->prediction_length; ++t) {
    for (int i = 0; i < world.m_NumCharacters; ++i) {
      SPlayerInput input = interaction_predict_input(ui, &world, i);
      cc_on_input(&world.m_pCharacters[i], &input);
    }

    for (SProjectile *ent = (SProjectile *)world.m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; ent;
         ent = (SProjectile *)ent->m_Base.m_pNextTypeEntity) {
      float pt = (world.m_GameTick - ent->m_StartTick) / (float)GAME_TICK_SPEED;
      float ct = (world.m_GameTick - ent->m_StartTick + 1) / (float)GAME_TICK_SPEED;
      mvec2 prev_pos = prj_get_pos(ent, pt);
      mvec2 cur_pos = prj_get_pos(ent, ct);

      mvec2 col;
      mvec2 new;
      bool collide = intersect_line(ent->m_Base.m_pCollision, prev_pos, cur_pos, &col, &new);

      vec2 pp = {vgetx(prev_pos) / 32.f, vgety(prev_pos) / 32.f};
      vec2 p;
      if (collide) {
        p[0] = vgetx(col) / 32.f;
        p[1] = vgety(col) / 32.f;
      } else {
        p[0] = vgetx(cur_pos) / 32.f;
        p[1] = vgety(cur_pos) / 32.f;
      }

      vec4 color = {1.0f, 0.5f, 0.5f, 0.8f};
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, pp, p, color, 0.05f);
    }

    for (SLaser *ent = (SLaser *)world.m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; ent; ent = (SLaser *)ent->m_Base.m_pNextTypeEntity) {
      vec2 p1 = {vgetx(ent->m_Base.m_Pos) / 32.f, vgety(ent->m_Base.m_Pos) / 32.f};
      vec2 p0 = {vgetx(ent->m_From) / 32.f, vgety(ent->m_From) / 32.f};

      vec4 color = {0.5f, 0.5f, 1.0f, 0.8f};
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, p0, p1, color, 0.05f);
    }

    wc_tick(&world);

    for (int i = 0; i < world.m_NumCharacters; ++i) {
      SCharacterCore *core = &world.m_pCharacters[i];
      vec2 pp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
      vec2 p = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
      vec4 color = {[3] = ui->prediction_alpha[i != ui->timeline.selected_player_track_index]};
      if (core->m_FreezeTime > 0) color[0] = 1.f;
      else color[1] = 1.f;
      renderer_submit_line(gfx, Z_LAYER_PREDICTION_LINES, pp, p, color, 0.05);
    }
  }
  wc_free(&prev_world);
  wc_free(&world);
}

void render_pickups(ui_handler_t *ui) {
  if (ui->num_pickups <= 0) return;
  gfx_handler_t *h = ui->gfx_handler;
  atlas_renderer_t *ar = &h->renderer.gameskin_renderer;

  atlas_instance_t *instances = malloc(sizeof(atlas_instance_t) * ui->num_pickups);
  if (!instances) return;

  uint32_t count = 0;

  float speed_scale = ui->timeline.is_reversing ? 2.0f : 1.0f;
  float intra = fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / (ui->timeline.playback_speed * speed_scale)), 1.f);
  if (ui->timeline.is_reversing) intra = 1.f - intra;
  intra += h->user_interface.timeline.current_tick;

  for (int i = 0; i < ui->num_pickups; ++i) {
    vec2 pos = {vgetx(ui->pickup_positions[i]) / 32.f, vgety(ui->pickup_positions[i]) / 32.f};
    vec2 size = {1.0f, 1.0f};
    SPickup pickup = ui->pickups[i];
    int idx = -1;

    // render health/armor
    if (pickup.m_Type == POWERUP_HEALTH || pickup.m_Type == POWERUP_ARMOR) {
      idx = GAMESKIN_PICKUP_HEALTH + pickup.m_Type;
      sprite_definition_t *sprite_def = &ar->sprite_definitions[idx];
      float w = sprite_def->w;
      float h = sprite_def->h;
      float f = sqrtf(w * w + h * h);
      float scaleX = w / f;
      float scaleY = h / f;
      size[0] = 1.f / scaleX;
      size[1] = 1.f / scaleY;
    } else if (pickup.m_Type >= POWERUP_ARMOR_SHOTGUN) { // render weapon armor
      idx = GAMESKIN_PICKUP_ARMOR_SHOTGUN + pickup.m_Type - POWERUP_ARMOR_SHOTGUN;
      sprite_definition_t *sprite_def = &ar->sprite_definitions[idx];
      float w = sprite_def->w;
      float h = sprite_def->h;
      float f = sqrtf(w * w + h * h);
      float scaleX = w / f;
      float scaleY = h / f;
      size[0] = 1.f / scaleX;
      size[1] = 1.f / scaleY;
    } else if (pickup.m_Type == POWERUP_WEAPON) { // render weapon pickup
      idx = GAMESKIN_PICKUP_HAMMER + pickup.m_Subtype;
      const weapon_spec_t *spec = &game_data.weapons.id[pickup.m_Subtype];
      sprite_definition_t *sprite_def = &ar->sprite_definitions[idx];
      float w = sprite_def->w;
      float h = sprite_def->h;
      float f = sqrtf(w * w + h * h);
      float scaleX = w / f;
      float scaleY = h / f;

      size[0] = spec->visual_size * scaleX / 32.0f;
      size[1] = spec->visual_size * scaleY / 32.0f;
    } else if (pickup.m_Type == POWERUP_NINJA) { // render ninja pickup
      idx = GAMESKIN_PICKUP_NINJA;
      sprite_definition_t *sprite_def = &ar->sprite_definitions[idx];
      float w = sprite_def->w;
      float h = sprite_def->h;
      float f = sqrtf(w * w + h * h);
      float scaleX = w / f;
      float scaleY = h / f;

      size[0] = 4.f * scaleX;
      size[1] = 4.f * scaleY;
      pos[0] -= 10.f / 32.f;
    }

    if (idx != -1) {
      float Offset = pos[1] + pos[0];
      pos[0] += (cos((intra / GAME_TICK_SPEED) * 2.0f + Offset) * 2.5f) / 32.f;
      pos[1] += (sin((intra / GAME_TICK_SPEED) * 2.0f + Offset) * 2.5f) / 32.f;

      glm_vec2_copy(pos, instances[count].pos);
      glm_vec2_copy(size, instances[count].size);
      instances[count].rotation = 0.0f;
      instances[count].sprite_index = idx;
      glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, instances[count].color);
      instances[count].tiling[0] = 1.0f;
      instances[count].tiling[1] = 1.0f;

      renderer_calculate_atlas_uvs(ar, idx, &instances[count]);
      count++;
    } else {
      log_warn("pickups", "Unknown pickup type %d encountered in render_pickups\n", pickup.m_Type);
    }
  }

  if (count > 0) {
    renderer_submit_atlas_batch(h, ar, Z_LAYER_PICKUPS, instances, count, false);
  }
  free(instances);
}

void render_cursor(ui_handler_t *ui) {
  if (!ui->timeline.recording) return;

  gfx_handler_t *handler = ui->gfx_handler;

  if (handler->user_interface.timeline.recording) {
    renderer_submit_atlas(handler, &handler->renderer.cursor_renderer, Z_LAYER_CURSOR, (vec2){ui->gfx_handler->viewport[0] * 0.5f + ui->recording_mouse_pos[0], ui->gfx_handler->viewport[1] * 0.5f + ui->recording_mouse_pos[1]}, (vec2){64.f, 64.f}, 0.0f, handler->user_interface.weapon, false, (vec4){1.0f, 1.0f, 1.0f, 1.0f}, true);
  }
}

void ui_render(ui_handler_t *ui) {
  process_net_events(ui);
  interaction_update_recording_input(ui);
  render_menu_bar(ui);

  // render menu bar first so the plugin can add menu items
  plugin_manager_update_all(&ui->plugin_manager);

  keybinds_process_inputs(ui);
  interaction_handle_playback_and_shortcuts(&ui->timeline);
  setup_docking();
  if (ui->show_timeline) {
    if (!ui->timeline.ui) ui->timeline.ui = ui;
    render_timeline(ui);
    render_player_manager(ui);
    render_snippet_editor_panel(ui);
    if (ui->timeline.selected_player_track_index != -1) render_player_info(ui->gfx_handler);
  }

  // Render the demo window/popup logic
  render_demo_window(ui);

  keybinds_render_settings_window(ui);
  undo_manager_render_history_window(&ui->undo_manager);
  if (ui->show_skin_browser) render_skin_browser(ui->gfx_handler);
  render_net_events_window(ui);
}

// render viewport and related things
bool ui_render_late(ui_handler_t *ui) {
  bool hovered = false;
  // igShowDemoWindow(NULL);
  if (ui->gfx_handler->offscreen_initialized && ui->gfx_handler->offscreen_texture != NULL) {
    igBegin("viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 start;
    igGetCursorScreenPos(&start);

    igGetWindowPos(&ui->viewport_window_pos);
    igSetCursorScreenPos(ui->viewport_window_pos);
    ImVec2 img_size = {(float)ui->gfx_handler->offscreen_width, (float)ui->gfx_handler->offscreen_height};
    igImage(*ui->gfx_handler->offscreen_texture, img_size, (ImVec2){0, 0}, (ImVec2){1, 1});

    igGetWindowSize((ImVec2 *)&ui->gfx_handler->viewport[0]);
    hovered = igIsWindowHovered(0);

    if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
      ImGuiIO *io = igGetIO_Nil();
      float mx = io->MousePos.x - ui->viewport_window_pos.x;
      float my = io->MousePos.y - ui->viewport_window_pos.y;
      float wx, wy;
      screen_to_world(ui->gfx_handler, mx, my, &wx, &wy);

      SWorldCore world = wc_empty();
      model_get_world_state_at_tick(&ui->timeline, ui->timeline.current_tick, &world, false);

      float speed_scale = ui->timeline.is_reversing ? 2.0f : 1.0f;
      float intra = fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / (ui->timeline.playback_speed * speed_scale)), 1.f);
      if (ui->timeline.is_reversing) intra = 1.f - intra;

      int best_match = -1;
      float best_dist = 1.5f;

      for (int i = 0; i < world.m_NumCharacters; ++i) {
        SCharacterCore *core = &world.m_pCharacters[i];
        vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
        vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
        vec2 p;
        lerp(ppp, pp, intra, p);

        float dx = p[0] - wx;
        float dy = p[1] - wy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < best_dist) {
          best_dist = dist;
          best_match = i;
        }
      }

      if (best_match != -1) {
        interaction_select_track(&ui->timeline, best_match);
      } else {
        if (!ui->selecting_override_pos)
          interaction_select_track(&ui->timeline, -1);
      }
      wc_free(&world);
    }

    if (ui->timeline.recording) {
      const char *text = "Recording... (ESC to Stop, F4 to Discard)";
      ImVec2 text_size;
      igCalcTextSize(&text_size, text, NULL, false, 0.0f);
      ImVec2 avail;
      igGetContentRegionAvail(&avail);
      ImVec2 text_pos = {start.x + avail.x - text_size.x - 10.0f, start.y};
      igGetWindowDrawList(); // Ensure draw list is active
      ImDrawList_AddText_Vec2(igGetWindowDrawList(), text_pos, IM_COL32(255, 50, 50, 255), text, NULL);
    }

    if ((hovered || ui->timeline.recording) && igIsKeyPressed_Bool(ImGuiKey_Tab, false)) {
      ui->show_timeline = !ui->show_timeline;
    }

    if (ui->timeline.selected_player_track_index >= 0) {
      igPushFont(ui->font, 25.f * gfx_get_ui_scale());
      igSetCursorScreenPos(start);
      igText("Character:");
      igText("Pos: %d, %d; (%.4f, %.4f)", ui->pos_x, ui->pos_y, ui->pos_x / 32.f, ui->pos_y / 32.f);
      igText("Vel: %.2f, %.2f; (%.2f, %.2f BPS)", ui->vel_x * ui->vel_r, ui->vel_y, ui->vel_x * ui->vel_r * (50.f / 32.f), ui->vel_y * (50.f / 32.f));
      igText("Freeze: %d", ui->freezetime);
      igText("Reload: %d", ui->reloadtime);
      igText("Weapon: %d", ui->weapon);
      igText("Weapons: [ %d, %d, %d, %d, %d, %d ]", ui->weapons[0], ui->weapons[1], ui->weapons[2], ui->weapons[3], ui->weapons[4], ui->weapons[5]);
      SPlayerInput Input = ui->timeline.player_tracks[ui->timeline.selected_player_track_index].current_input;
      if (!ui->timeline.recording)
        Input = model_get_input_at_tick(&ui->timeline, ui->timeline.selected_player_track_index, ui->timeline.current_tick);
      igText("");
      igText("Input:");
      igText("Direction: %d", Input.m_Direction);
      igText("TargetX: %d", Input.m_TargetX);
      igText("TargetY: %d", Input.m_TargetY);
      igText("Jump: %d", Input.m_Jump);
      igText("Fire: %d", Input.m_Fire);
      igText("Hook: %d", Input.m_Hook);
      igText("WantedWeapon: %d", Input.m_WantedWeapon);
      igText("TeleOut: %d", Input.m_TeleOut);
#define WORD_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c"
#define WORD_TO_BINARY(word)                                                                                                      \
  ((word) & 0x8000 ? '1' : '0'), ((word) & 0x4000 ? '1' : '0'), ((word) & 0x2000 ? '1' : '0'), ((word) & 0x1000 ? '1' : '0'),     \
      ((word) & 0x0800 ? '1' : '0'), ((word) & 0x0400 ? '1' : '0'), ((word) & 0x0200 ? '1' : '0'), ((word) & 0x0100 ? '1' : '0'), \
      ((word) & 0x0080 ? '1' : '0'), ((word) & 0x0040 ? '1' : '0'), ((word) & 0x0020 ? '1' : '0'), ((word) & 0x0010 ? '1' : '0'), \
      ((word) & 0x0008 ? '1' : '0'), ((word) & 0x0004 ? '1' : '0'), ((word) & 0x0002 ? '1' : '0'), ((word) & 0x0001 ? '1' : '0')
      igText("Flags: " WORD_TO_BINARY_PATTERN, WORD_TO_BINARY(Input.m_Flags));
#undef WORD_TO_BINARY
#undef WORD_TO_BINARY_PATTERN
      igPopFont();
    }
    igEnd();
  }
  return hovered;
}

void ui_post_map_load(ui_handler_t *ui) {
  // by default they are NULL so this should be fine
  free(ui->pickups);
  free(ui->pickup_positions);
  free(ui->ninja_pickup_indices);
  // function might return early leaving them dangling so reset them
  ui->num_pickups = 0;
  ui->pickups = NULL;
  ui->pickup_positions = NULL;
  ui->ninja_pickup_indices = NULL;
  ui->num_ninja_pickups = 0;

  gfx_handler_t *h = ui->gfx_handler;
  int width = h->physics_handler.collision.m_MapData.width;
  int height = h->physics_handler.collision.m_MapData.height;
  int num = 0;
  for (int i = 0; i < width * height; ++i) {
    const SPickup pickup = h->physics_handler.collision.m_pPickups[i];
    if (pickup.m_Type >= 0) ++num;
    const SPickup fpickup = h->physics_handler.collision.m_pFrontPickups[i];
    if (fpickup.m_Type >= 0) ++num;
  }
  if (!num) return;
  ui->num_pickups = num;
  ui->pickups = malloc(sizeof(SPickup) * num);
  ui->pickup_positions = malloc(sizeof(mvec2) * num);
  ui->ninja_pickup_indices = malloc(sizeof(int) * num);
  ui->num_ninja_pickups = 0;

  num = 0;
  for (int i = 0; i < width * height; ++i) {
    const SPickup pickup = h->physics_handler.collision.m_pPickups[i];
    if (pickup.m_Type >= 0) {
      ui->pickup_positions[num] = vec2_init((i % width) * 32.f + 16.f, (int)(i / width) * 32.f + 16.f);
      ui->pickups[num] = pickup;
      if (pickup.m_Type == POWERUP_NINJA) {
        ui->ninja_pickup_indices[ui->num_ninja_pickups++] = num;
      }
      num++;
    }
    const SPickup fpickup = h->physics_handler.collision.m_pFrontPickups[i];
    if (fpickup.m_Type >= 0) {
      ui->pickup_positions[num] = vec2_init((i % width) * 32.f + 16.f, (int)(i / width) * 32.f + 16.f);
      ui->pickups[num] = fpickup;
      if (fpickup.m_Type == POWERUP_NINJA) {
        ui->ninja_pickup_indices[ui->num_ninja_pickups++] = num;
      }
      num++;
    }
  }
}

void ui_cleanup(ui_handler_t *ui) {
  free(ui->pickups);
  free(ui->pickup_positions);
  free(ui->ninja_pickup_indices);
  config_save(ui);
  plugin_manager_shutdown(&ui->plugin_manager);
  particle_system_cleanup(&ui->particle_system);
  timeline_cleanup(&ui->timeline);
  undo_manager_cleanup(&ui->undo_manager);
  skin_manager_free(&ui->skin_manager);
  NFD_Quit();
}
