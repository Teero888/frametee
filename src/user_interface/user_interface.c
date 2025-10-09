#include "user_interface.h"
#include "../../symbols.h"
#include "../animation/anim_data.h"
#include "../logger/logger.h"
#include "../plugins/api_impl.h"
#include "../renderer/graphics_backend.h"
#include "../renderer/renderer.h"
#include "../system/save.h"
#include "cglm/vec2.h"
#include "cimgui.h"
#include "gamecore.h"
#include "player_info.h"
#include "skin_browser.h"
#include "snippet_editor.h"
#include "timeline.h"
#include "undo_redo.h"
#include "widgets/hsl_colorpicker.h"
#include <limits.h>
#include <math.h>
#include <nfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *LOG_SOURCE = "UI";

typedef struct ui_handler ui_handler_t;

void render_menu_bar(ui_handler_t *ui) {
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
        } else if (result == NFD_CANCEL)
          log_warn(LOG_SOURCE, "Canceled map load.");
        else
          log_error(LOG_SOURCE, "Error: %s\n", NFD_GetError());
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
      igMenuItem_BoolPtr("Keybind Settings", NULL, &ui->keybinds.show_settings_window, true);
      igMenuItem_BoolPtr("Show prediction", NULL, &ui->show_prediction, true);
      igMenuItem_BoolPtr("Show skin manager", NULL, &ui->show_skin_browser, true);
      igEndMenu();
    }

    const char *button_text = "Reload Plugins";
    ImVec2 button_size;
    igCalcTextSize(&button_size, button_text, NULL, false, 0.0f);
    button_size.x += igGetStyle()->FramePadding.x * 2.0f;
    ImVec2 region_avail;
    igGetContentRegionAvail(&region_avail);
    igSetCursorPosX(igGetCursorPosX() + region_avail.x - button_size.x);
    if (igButton(button_text, (ImVec2){0, 0}))
      plugin_manager_reload_all(&ui->plugin_manager, "plugins");

    igEndMainMenuBar();
  }
}

// docking setup
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

  // build the initial layout programmatically --
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // split root into bottom + top remainder
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom =
        igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.20f, NULL, &dock_id_top);

    // split top remainder into left + remainder
    ImGuiID dock_id_left;
    ImGuiID dock_id_center;
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.25f, NULL, &dock_id_center);
    dock_id_left = igDockBuilderSplitNode(dock_id_center, ImGuiDir_Left, 0.40f, NULL, &dock_id_center);

    igDockBuilderDockWindow("viewport", dock_id_center);
    igDockBuilderDockWindow("Keybind Settings", dock_id_center);
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
  ph_t *ph = &ui->gfx_handler->physics_handler;
  if (igBegin("Players", NULL, 0)) {
    if (ph->world.m_pCollision && igButton("Add Player", (ImVec2){0, 0})) {
      add_new_track(ts, ph, 1);
    }
    igSameLine(0, 10.f);
    if (ph->world.m_pCollision && igButton("Add 1000 Players", (ImVec2){0, 0})) {
      add_new_track(ts, ph, 1000);
    }
    igSameLine(0, 10.f);
    igText("Players: %d", ts->player_track_count);

    igSeparator();
    for (int i = 0; i < ts->player_track_count; i++) {
      igPushID_Int(i);
      bool sel = (i == ts->selected_player_track_index);
      const char *label =
          ts->player_tracks[i].player_info.name[0] ? ts->player_tracks[i].player_info.name : "nameless tee";

      // Selectable only
      igSetNextItemAllowOverlap();
      if (igSelectable_Bool(label, sel, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){0, 0})) {
        ts->selected_player_track_index = i;
      }

      ImVec2 vMin;
      igGetContentRegionAvail(&vMin);
      igSameLine(vMin.x - 20.f, -1.0f); // shift right
      if (igSmallButton(ICON_KI_TRASH)) {
        if (g_remove_confirm_needed && ts->player_tracks[i].snippet_count > 0) {
          g_pending_remove_index = i;
          igPopID();
          igOpenPopup_Str("Confirm remove player", ImGuiPopupFlags_AnyPopupLevel);
          igPushID_Int(i);
        } else {
          undo_command_t *cmd = do_remove_player_track(ui, i);
          undo_manager_register_command(&ui->undo_manager, cmd);
        }
      }
      igPopID();
    }
    if (ts->player_track_count > 0)
      igSeparator();
  }
  if (igBeginPopupModal("Confirm remove player", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
    igText("This player has inputs. Remove anyway?");
    static bool dont_ask_again = false;
    igCheckbox("Do not ask again", &dont_ask_again);
    if (igButton("Yes", (ImVec2){0, 0})) {
      undo_command_t *cmd = do_remove_player_track(ui, g_pending_remove_index);
      undo_manager_register_command(&ui->undo_manager, cmd);
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
  igEnd();
}

void on_camera_update(gfx_handler_t *handler, bool hovered) {
  if (!handler->map_data || !handler->map_data->game_layer.data)
    return;
  camera_t *camera = &handler->renderer.camera;
  ImGuiIO *io = igGetIO_Nil();

  float scroll_y = !hovered ? 0.0f : io->MouseWheel;
  if (!igIsAnyItemActive()) { // Prevent shortcuts while typing in a text field
    if (is_key_combo_pressed(&handler->user_interface.keybinds.bindings[ACTION_ZOOM_IN].combo, true))
      scroll_y = 1.0f;
    if (is_key_combo_pressed(&handler->user_interface.keybinds.bindings[ACTION_ZOOM_OUT].combo, true))
      scroll_y = -1.0f;
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

void ui_init(ui_handler_t *ui, gfx_handler_t *gfx_handler) {
  ImGuiIO *io = igGetIO_Nil();
  ImFontAtlas *atlas = io->Fonts;

  ui->font =
      ImFontAtlas_AddFontFromFileTTF(io->Fonts, "data/fonts/JetBrainsMono-Medium.ttf", 19.f, NULL, NULL);

  ImFontConfig *config = ImFontConfig_ImFontConfig();
  config->MergeMode = true;
  config->GlyphMinAdvanceX = 13.0f;
  config->GlyphOffset = (ImVec2){0.0f, 1.0f};

  ImFontAtlas_AddFontFromFileTTF(atlas, "data/fonts/kenney-icon-font.ttf", 14.0f, config, NULL);

  ImFontConfig_destroy(config);

  ui->gfx_handler = gfx_handler;
  ui->show_timeline = true;
  ui->show_prediction = true;
  ui->prediction_length = 100;
  ui->show_skin_browser = false;
  timeline_init(&ui->timeline);
  camera_init(&gfx_handler->renderer.camera);
  keybinds_init(&ui->keybinds);
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
}

static float lint2(float a, float b, float f) { return a + f * (b - a); }
static void lerp(vec2 a, vec2 b, float f, vec2 out) {
  out[0] = lint2(a[0], b[0], f);
  out[1] = lint2(a[1], b[1], f);
}

static int find_snapshot_index_le(const physics_v_t *vec, int target_tick) {
  // returns index of the last snapshot with m_GameTick <= target_tick
  // returns -1 if none found
  if (vec->current_size == 0)
    return -1;
  for (int i = vec->current_size - 1; i >= 0; --i) {
    if (vec->data[i].m_GameTick <= target_tick)
      return i;
  }
  return -1;
}

void render_players(ui_handler_t *ui) {
  gfx_handler_t *gfx = ui->gfx_handler;
  physics_handler_t *ph = &gfx->physics_handler;
  if (!ph->loaded)
    return;

  SWorldCore world = wc_empty();

  const int step = 50;
  int target_tick = ui->timeline.current_tick;

  if (target_tick < ui->timeline.previous_world.m_GameTick)
    wc_copy_world(
        &world,
        &ui->timeline.vec.data[iclamp((target_tick - 1) / step, 0, ui->timeline.vec.current_size - 1)]);
  else
    wc_copy_world(&world, &ui->timeline.previous_world);

  if (ui->timeline.player_track_count != world.m_NumCharacters) {
    wc_free(&world);
    return;
  }

  while (world.m_GameTick < target_tick) {
    for (int p = 0; p < world.m_NumCharacters; ++p) {
      SPlayerInput input = get_input(&ui->timeline, p, world.m_GameTick);
      cc_on_input(&world.m_pCharacters[p], &input);
    }
    wc_tick(&world);
    if (world.m_GameTick % step == 0) {
      if (world.m_GameTick / step >= ui->timeline.vec.current_size)
        v_push(&ui->timeline.vec, &world);
      else {
        wc_copy_world(&ui->timeline.vec.data[world.m_GameTick / step], &world);
      }
    }
  }
  wc_copy_world(&ui->timeline.previous_world, &world);

  float intra =
      fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / ui->timeline.playback_speed), 1.f);
  if (igIsKeyDown_Nil(ImGuiKey_C))
    intra = 1.f - intra;

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
    bool want_other_dir = (core->m_Input.m_Direction == -1 && vgetx(core->m_Vel) > 0) ||
                          (core->m_Input.m_Direction == 1 && vgetx(core->m_Vel) < 0);
    bool inactive = get_flag_sit(&core->m_Input);
    bool in_air = !(core->m_pCollision->m_pTileInfos[core->m_BlockIdx] & INFO_CANGROUND) ||
                  !(check_point(core->m_pCollision, vec2_init(vgetx(core->m_Pos), vgety(core->m_Pos) + 16)));

    float walk_time = fmod(p[0] * 32.f, 100.0f) / 100.0f;
    float run_time = fmod(p[0] * 32.f, 200.0f) / 200.0f;
    if (walk_time < 0.0f)
      walk_time += 1.0f;
    if (run_time < 0.0f)
      run_time += 1.0f;

    if (in_air)
      anim_state_add(&anim_state, &anim_inair, 0.0f, 1.0f);
    else if (stationary) {
      if (inactive)
        anim_state_add(&anim_state, core->m_Input.m_Direction < 0 ? &anim_sit_left : &anim_sit_right, 0.0f,
                       1.0f);
      else
        anim_state_add(&anim_state, &anim_idle, 0.0f, 1.0f);
    } else if (!want_other_dir) {
      if (running)
        anim_state_add(&anim_state, vgetx(core->m_Vel) < 0.0f ? &anim_run_left : &anim_run_right, run_time,
                       1.0f);
      else
        anim_state_add(&anim_state, &anim_walk, walk_time, 1.0f);
    }

    vec2 dir = {core->m_Input.m_TargetX, core->m_Input.m_TargetY};
    glm_vec2_normalize(dir);
    player_info_t *info = &gfx->user_interface.timeline.player_tracks[i].player_info;
    int skin = info->skin;
    int eye = get_flag_eye_state(&core->m_Input);
    vec3 feet_col = {1.f, 1.f, 1.f};
    vec3 body_col = {0.0f, 0.0f, 0.0f};
    bool custom_col = info->use_custom_color;

    if (core->m_FreezeTime > 0) {
      skin = gfx->x_ninja_skin;
      if (eye == 0)
        eye = EYE_BLINK;
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

    renderer_push_skin_instance(gfx, p, 1.0f, skin, eye, dir, &anim_state, body_col, feet_col, custom_col);

    // render hook
    if (core->m_HookState >= 1) {
      vec2 hook_pos = {vgetx(core->m_HookPos) / 32.f, vgety(core->m_HookPos) / 32.f};

      vec2 direction;
      glm_vec2_sub(hook_pos, p, direction);
      float length = glm_vec2_norm(direction);
      glm_vec2_normalize(direction);
      float angle = atan2f(-direction[1], direction[0]);

      if (length > 0) {
        vec2 center_pos;
        center_pos[0] = p[0] + direction[0] * (length - 1.0) * 0.5f;
        center_pos[1] = p[1] + direction[1] * (length - 1.0) * 0.5f;
        vec2 chain_size = {-length, 0.5};
        renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, center_pos, chain_size, angle,
                                     GAMESKIN_HOOK_CHAIN, true);
      }
      sprite_definition_t *head_sprite_def =
          &gfx->renderer.gameskin_renderer.sprite_definitions[GAMESKIN_HOOK_HEAD];
      vec2 head_size = {(float)head_sprite_def->w / 64.0f, (float)head_sprite_def->h / 64.0f};
      renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, hook_pos, head_size, angle,
                                   GAMESKIN_HOOK_HEAD, false);
    }
    if (!core->m_FreezeTime && core->m_ActiveWeapon >= WEAPON_HAMMER && core->m_ActiveWeapon < NUM_WEAPONS) {
      const weapon_spec_t *spec = &game_data.weapons.id[core->m_ActiveWeapon];
      float fire_delay_ticks = spec->firedelay * (float)GAME_TICK_SPEED / 1000.0f;
      float attack_ticks_passed = (world.m_GameTick - core->m_AttackTick) + intra;
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
        if (dir[0] < 0.0f)
          weapon_pos[0] -= spec->offsetx;
        if (is_sit)
          weapon_pos[1] += 3.0f;

        if (!inactive) {
          anim_state_add(&anim_state, &anim_hammer_swing, (float)attack_ticks_passed / fire_delay_ticks,
                         1.0f);
          anim_attach_angle_rad = anim_state.attach.angle * (2.0f * M_PI);
          weapon_angle = M_PI / 2.0f - flip_factor * anim_attach_angle_rad;
        } else {
          weapon_angle = dir[0] < 0.0 ? 100.f : 500.f;
        }
      } else if (core->m_ActiveWeapon == WEAPON_NINJA) {
        weapon_sprite_id = GAMESKIN_NINJA_BODY;
        weapon_pos[1] += spec->offsety;
        if (is_sit)
          weapon_pos[1] += 3.0f;
        if (dir[0] < 0.0f)
          weapon_pos[0] -= spec->offsetx;

        if (attack_ticks_passed <= fire_delay_ticks) {
          anim_state_add(&anim_state, &anim_ninja_swing, (float)attack_ticks_passed / fire_delay_ticks, 1.0f);
        }

        anim_attach_angle_rad = anim_state.attach.angle * (2.0f * M_PI);
        weapon_angle = -M_PI / 2.0f + flip_factor * anim_attach_angle_rad;

        float attack_time_sec = attack_ticks_passed / (float)GAME_TICK_SPEED;
        if (attack_time_sec <= 1.0f / 6.0f && spec->num_muzzles > 0) {
          int muzzle_idx = world.m_GameTick % spec->num_muzzles;
          vec2 hadoken_dir = {vgetx(core->m_Vel), vgety(core->m_Vel)};
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
          sprite_definition_t *muzzle_sprite_def =
              &gfx->renderer.gameskin_renderer.sprite_definitions[muzzle_sprite_id];
          float f = sqrtf(powf(muzzle_sprite_def->w, 2) + powf(muzzle_sprite_def->h, 2));
          float scaleX = muzzle_sprite_def->w / f;
          float scaleY = muzzle_sprite_def->h / f;
          vec2 muzzle_size = {160.0f * scaleX / 32.0f, 160.0f * scaleY / 32.0f};

          vec2 render_pos = {muzzle_phys_pos[0] / 32.0f, muzzle_phys_pos[1] / 32.0f};
          renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, render_pos, muzzle_size,
                                       hadoken_angle, muzzle_sprite_id, false);
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
        if (attack_ticks_passed > 0 && a < 1.0f)
          recoil = sinf(a * M_PI);

        weapon_pos[0] += dir[0] * (spec->offsetx - recoil * 10.0f);
        weapon_pos[1] += dir[1] * (spec->offsetx - recoil * 10.0f);
        weapon_pos[1] += spec->offsety;

        if (is_sit)
          weapon_pos[1] += 3.0f;

        if ((core->m_ActiveWeapon == WEAPON_GUN || core->m_ActiveWeapon == WEAPON_SHOTGUN) &&
            spec->num_muzzles > 0) {
          if (attack_ticks_passed > 0 && attack_ticks_passed < spec->muzzleduration + 3.0f) {
            int muzzle_idx = world.m_GameTick % spec->num_muzzles;
            vec2 muzzle_dir_y = {-dir[1], dir[0]};
            float offset_y = -spec->muzzleoffsety * flip_factor;

            vec2 muzzle_phys_pos;
            glm_vec2_copy(weapon_pos, muzzle_phys_pos);
            muzzle_phys_pos[0] += dir[0] * spec->muzzleoffsetx + muzzle_dir_y[0] * offset_y;
            muzzle_phys_pos[1] += dir[1] * spec->muzzleoffsetx + muzzle_dir_y[1] * offset_y;

            int muzzle_sprite_id =
                (core->m_ActiveWeapon == WEAPON_GUN ? GAMESKIN_GUN_MUZZLE1 : GAMESKIN_SHOTGUN_MUZZLE1) +
                muzzle_idx;

            float w = 96.0f, h = 64.0f;
            float f = sqrtf(w * w + h * h);
            float scale_x = w / f;
            float scale_y = h / f;

            vec2 muzzle_size;
            muzzle_size[0] = spec->visual_size * scale_x * (4.0f / 3.0f) / 32.0f;
            muzzle_size[1] = spec->visual_size * scale_y / 32.0f;
            muzzle_size[1] *= flip_factor;

            vec2 render_pos = {muzzle_phys_pos[0] / 32.0f, muzzle_phys_pos[1] / 32.0f};
            renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, render_pos, muzzle_size,
                                         weapon_angle, muzzle_sprite_id, false);
          }
        }
      }

      if (weapon_sprite_id != -1) {
        sprite_definition_t *sprite_def =
            &gfx->renderer.gameskin_renderer.sprite_definitions[weapon_sprite_id];
        float w = sprite_def->w;
        float h = sprite_def->h;
        float f = sqrtf(w * w + h * h);
        float scaleX = w / f;
        float scaleY = h / f;

        vec2 weapon_size = {spec->visual_size * scaleX / 32.0f, spec->visual_size * scaleY / 32.0f};
        weapon_size[1] *= flip_factor;

        vec2 render_pos = {weapon_pos[0] / 32.0f, weapon_pos[1] / 32.0f};

        renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, render_pos, weapon_size, weapon_angle,
                                     weapon_sprite_id, false);
      }
    }
  }
  int id = 0;
  for (SProjectile *ent = world.m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; ent;
       ent = ent->m_Base.m_pNextTypeEntity) {
    float pt = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick - 1) / (float)GAME_TICK_SPEED;
    float ct = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick) / (float)GAME_TICK_SPEED;
    mvec2 prev_pos = prj_get_pos(ent, pt);
    mvec2 cur_pos = prj_get_pos(ent, ct);

    vec2 ppp = {vgetx(prev_pos) / 32.f, vgety(prev_pos) / 32.f};
    vec2 pp = {vgetx(cur_pos) / 32.f, vgety(cur_pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    renderer_push_atlas_instance(&gfx->renderer.gameskin_renderer, p, (vec2){1, 1},
                                 -((world.m_GameTick + intra) / 50.f) * 4 * M_PI + id, GAMESKIN_GRENADE_PROJ,
                                 false);
    ++id;
  }
  (void)id;
  for (SLaser *ent = world.m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; ent;
       ent = ent->m_Base.m_pNextTypeEntity) {
    vec2 p1 = {vgetx(ent->m_Base.m_Pos) / 32.f, vgety(ent->m_Base.m_Pos) / 32.f};
    vec2 p0 = {vgetx(ent->m_From) / 32.f, vgety(ent->m_From) / 32.f};

    vec4 lsr_col = {0.f, 0.f, 1.f, 0.9f};
    vec4 sg_col = {0.570315f, 0.4140625f, 025.f, 0.9f};

    renderer_draw_line(gfx, p0, p1, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 0.25f);
    renderer_draw_circle_filled(gfx, p0, 0.2, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 8);
  }

  // if (ui->timeline.recording) {
  //   SCharacterCore *core = &world.m_pCharacters[gfx->user_interface.timeline.selected_player_track_index];
  //   vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
  //   vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
  //   vec2 p;
  //   lerp(ppp, pp, intra, p);
  //   renderer_draw_circle_filled(gfx,
  //                               (vec2){p[0] + gfx->user_interface.timeline.recording_input.m_TargetX
  //                               / 64.f,
  //                                      p[1] + gfx->user_interface.timeline.recording_input.m_TargetY
  //                                      / 64.f},
  //                               0.25, (vec4){1.f, 0.f, 0.f, 0.4f}, 16);
  // }

  if (ui->timeline.selected_player_track_index >= 0) {
    SCharacterCore *p = &world.m_pCharacters[ui->timeline.selected_player_track_index];
    ui->pos_x = vgetx(p->m_Pos);
    ui->pos_y = vgety(p->m_Pos);
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
    wc_free(&world);
    return;
  }

  // draw the first line
  for (int i = 0; i < world.m_NumCharacters; ++i) {
    SCharacterCore *core = &world.m_pCharacters[i];
    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);

    vec4 color = {[3] = 0.8f};
    if (core->m_FreezeTime > 0)
      color[0] = 1.f;
    else
      color[1] = 1.f;
    renderer_draw_line(gfx, pp, p, color, 0.05);
  }

  // draw the rest of the lines
  for (int t = 0; t < ui->prediction_length; ++t) {
    for (int i = 0; i < world.m_NumCharacters; ++i) {
      SPlayerInput input = ui->timeline.recording && i == ui->timeline.selected_player_track_index &&
                                   world.m_GameTick >= ui->timeline.recording_snippets.snippets[0]->end_tick
                               ? ui->timeline.recording_input
                               : get_input(&ui->timeline, i, world.m_GameTick);
      cc_on_input(&world.m_pCharacters[i], &input);
    }
    wc_tick(&world);

    for (int i = 0; i < world.m_NumCharacters; ++i) {
      SCharacterCore *core = &world.m_pCharacters[i];
      vec2 pp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
      vec2 p = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
      vec4 color = {[3] = 0.8f};
      if (core->m_FreezeTime > 0)
        color[0] = 1.f;
      else
        color[1] = 1.f;
      renderer_draw_line(gfx, pp, p, color, 0.05);
    }
  }
  wc_free(&world);
}

void render_cursor(ui_handler_t *ui) {
  if (!ui->timeline.recording)
    return;

  gfx_handler_t *handler = ui->gfx_handler;

  // TODO: for now draw this in world space. fix later when pipeline is better.
  if (handler->user_interface.timeline.recording) {
    float norm_x = ui->last_render_pos[0] + handler->user_interface.timeline.recording_input.m_TargetX / 64.f;
    float norm_y = ui->last_render_pos[1] + handler->user_interface.timeline.recording_input.m_TargetY / 64.f;
    renderer_push_atlas_instance(&handler->renderer.cursor_renderer, (vec2){norm_x, norm_y}, (vec2){1.f, 1.f},
                                 0.0f, handler->user_interface.weapon, false);
  }
}

void ui_render(ui_handler_t *ui) {
  timeline_update_inputs(&ui->timeline, ui->gfx_handler);
  render_menu_bar(ui);

  // render menu bar first so the plugin can add menu items
  plugin_manager_update_all(&ui->plugin_manager);

  ImGuiIO *io = igGetIO_Nil();
  keybinds_process_inputs(ui);
  setup_docking(ui);
  if (ui->show_timeline) {
    if (!ui->timeline.ui)
      ui->timeline.ui = ui;
    render_timeline(ui);
    render_player_manager(ui);
    render_snippet_editor_panel(ui);
    if (ui->timeline.selected_player_track_index != -1)
      render_player_info(ui->gfx_handler);
  }
  keybinds_render_settings_window(&ui->keybinds);
  if (ui->show_skin_browser)
    render_skin_browser(ui->gfx_handler);
}

// render viewport and related things
bool ui_render_late(ui_handler_t *ui) {
  bool hovered = false;
  // igShowDemoWindow(NULL);
  if (ui->gfx_handler->offscreen_initialized && ui->gfx_handler->offscreen_texture != NULL) {
    igBegin("viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 start;
    igGetCursorScreenPos(&start);

    ImVec2 wpos;
    igGetWindowPos(&wpos);
    igSetCursorScreenPos(wpos);
    ImVec2 img_size = {(float)ui->gfx_handler->offscreen_width, (float)ui->gfx_handler->offscreen_height};
    igImage(*ui->gfx_handler->offscreen_texture, img_size, (ImVec2){0, 0}, (ImVec2){1, 1});

    igGetWindowSize(&ui->gfx_handler->viewport[0]);
    hovered = igIsWindowHovered(0);

    if (ui->timeline.selected_player_track_index >= 0) {
      igPushFont(ui->font, 25.f);
      igSetCursorScreenPos(start);
      igText("Character:");
      igText("Pos: %d, %d; (%.4f, %.4f)", ui->pos_x, ui->pos_y, ui->pos_x / 32.f, ui->pos_y / 32.f);
      igText("Vel: %.2f, %.2f; (%.2f, %.2f BPS)", ui->vel_x * ui->vel_r, ui->vel_y,
             ui->vel_x * ui->vel_r * (50.f / 32.f), ui->vel_y * (50.f / 32.f));
      igText("Freeze: %d", ui->freezetime);
      igText("Reload: %d", ui->reloadtime);
      igText("Weapon: %d", ui->weapon);
      igText("Weapons: [ %d, %d, %d, %d, %d, %d ]", ui->weapons[0], ui->weapons[1], ui->weapons[2],
             ui->weapons[3], ui->weapons[4], ui->weapons[5]);
      SPlayerInput Input = ui->timeline.recording_input;
      if (!ui->timeline.recording)
        Input = get_input(&ui->timeline, ui->timeline.selected_player_track_index, ui->timeline.current_tick);
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
#define WORD_TO_BINARY(word)                                                                                 \
  ((word) & 0x8000 ? '1' : '0'), ((word) & 0x4000 ? '1' : '0'), ((word) & 0x2000 ? '1' : '0'),               \
      ((word) & 0x1000 ? '1' : '0'), ((word) & 0x0800 ? '1' : '0'), ((word) & 0x0400 ? '1' : '0'),           \
      ((word) & 0x0200 ? '1' : '0'), ((word) & 0x0100 ? '1' : '0'), ((word) & 0x0080 ? '1' : '0'),           \
      ((word) & 0x0040 ? '1' : '0'), ((word) & 0x0020 ? '1' : '0'), ((word) & 0x0010 ? '1' : '0'),           \
      ((word) & 0x0008 ? '1' : '0'), ((word) & 0x0004 ? '1' : '0'), ((word) & 0x0002 ? '1' : '0'),           \
      ((word) & 0x0001 ? '1' : '0')
      igText("Flags: " WORD_TO_BINARY_PATTERN, WORD_TO_BINARY(Input.m_Flags));
#undef WORD_TO_BINARY
#undef WORD_TO_BINARY_PATTERN
      igPopFont();
    }
    igEnd();
  }
  return hovered;
}

void ui_cleanup(ui_handler_t *ui) {
  plugin_manager_shutdown(&ui->plugin_manager);
  timeline_cleanup(&ui->timeline);
  undo_manager_cleanup(&ui->undo_manager);
  skin_manager_free(&ui->skin_manager);
  NFD_Quit();
}
