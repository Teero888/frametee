#include "user_interface.h"
#include "../../symbols.h"
#include "../animation/anim_data.h"
#include "../renderer/graphics_backend.h"
#include "../renderer/renderer.h"
#include "cglm/vec2.h"
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
      igMenuItem_BoolPtr("Show prediction", NULL, &ui->show_prediction, true);
      igMenuItem_BoolPtr("Show skin manager", NULL, &ui->show_skin_manager, true);
      igEndMenu();
    }

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

    // Split root into bottom + top remainder
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom =
        igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.30f, NULL, &dock_id_top);

    // Split top remainder into left + remainder
    ImGuiID dock_id_left;
    ImGuiID dock_id_center; // this will be "viewport"
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.25f, NULL, &dock_id_center);
    dock_id_left = igDockBuilderSplitNode(dock_id_center, ImGuiDir_Left, 0.25f, NULL, &dock_id_center);

    // now dock_id_center is the leftover = central piece
    igDockBuilderDockWindow("viewport", dock_id_center);
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    igDockBuilderDockWindow("Player Info", dock_id_left);
    igDockBuilderDockWindow("Players", dock_id_left);
    igDockBuilderDockWindow("Snippet Editor", dock_id_right);

    igDockBuilderFinish(main_dockspace_id);
  }
}

// snippet editor panel
static const char *dir_options[] = {"Left", "Neutral", "Right"};
static const char *weapon_options[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};

void render_snippet_editor_panel(timeline_state_t *ts) {
  if (igBegin("Snippet Editor", NULL, 0)) {
    if (ts->selected_snippet_id != -1) {
      // Find snippet globally
      input_snippet_t *snippet = NULL;
      for (int i = 0; i < ts->player_track_count; i++) {
        snippet = find_snippet_by_id(&ts->player_tracks[i], ts->selected_snippet_id);
        if (snippet)
          break;
      }

      if (snippet && snippet->input_count > 0) {
        igText("Editing Snippet %d (%d inputs)", snippet->id, snippet->input_count);
        igSeparator();

        // Begin scrollable child region
        igBeginChild_Str("InputsScroll", (ImVec2){0, 0}, true, 0);

        // Define table flags
        ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInner;

        // Begin table with columns
        if (igBeginTable("SnippetInputsTable", 9, flags, (ImVec2){0, 0}, 0)) {

          // Setup column headers
          igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
          igTableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
          igTableSetupColumn("TargetX", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
          igTableSetupColumn("TargetY", ImGuiTableColumnFlags_WidthFixed, 0.0f, 3);
          igTableSetupColumn("Tele", ImGuiTableColumnFlags_WidthFixed, 0.0f, 4);
          igTableSetupColumn("Jump", ImGuiTableColumnFlags_WidthFixed, 0.0f, 5);
          igTableSetupColumn("Fire", ImGuiTableColumnFlags_WidthFixed, 0.0f, 6);
          igTableSetupColumn("Hook", ImGuiTableColumnFlags_WidthFixed, 0.0f, 7);
          igTableSetupColumn("Wpn", ImGuiTableColumnFlags_WidthFixed, 0.0f, 8);

          igTableHeadersRow();

          // Iterate over inputs
          for (int i = 0; i < snippet->input_count; i++) {
            SPlayerInput *inp = &snippet->inputs[i];
            igTableNextRow(ImGuiTableRowFlags_None, 0.0f);

            // Tick (read-only display)
            igTableSetColumnIndex(0);
            igText("%d", snippet->start_tick + i);

            // Direction: plain int input (-1, 0, 1)
            igTableSetColumnIndex(1);
            igPushID_Int(i * 10 + 1);
            int dir_temp = (int)inp->m_Direction;    // promote to int
            igInputInt("##Dir", &dir_temp, 0, 0, 0); // edit as int
            dir_temp = iclamp(dir_temp, -1, 1);
            if (dir_temp != (int)inp->m_Direction)
              inp->m_Direction = (int8_t)dir_temp;
            igPopID();

            // Target X: full int range, no buttons
            igTableSetColumnIndex(2);
            igPushID_Int(i * 10 + 2);
            igInputInt("##TX", &inp->m_TargetX, 0, 0, 0);
            igPopID();

            // Target Y: full int range, no buttons
            igTableSetColumnIndex(3);
            igPushID_Int(i * 10 + 3);
            igInputInt("##TY", &inp->m_TargetY, 0, 0, 0);
            igPopID();

            // TeleOut: keep as is, but remove buttons
            igTableSetColumnIndex(4);
            igPushID_Int(i * 10 + 4);
            int tele_temp = (int)inp->m_TeleOut;
            igInputInt("##TO", &tele_temp, 0, 0, 0);
            tele_temp = iclamp(tele_temp, 0, 4);
            if (tele_temp != (int)inp->m_TeleOut)
              inp->m_TeleOut = (uint8_t)tele_temp;
            igPopID();

            // Jump (Checkbox)
            igTableSetColumnIndex(5);
            igPushID_Int(i * 10 + 5);
            bool jump = inp->m_Jump;
            if (igCheckbox("##J", &jump))
              inp->m_Jump = jump;
            igPopID();

            // Fire (Checkbox)
            igTableSetColumnIndex(6);
            igPushID_Int(i * 10 + 6);
            bool fire = inp->m_Fire;
            if (igCheckbox("##F", &fire))
              inp->m_Fire = fire;
            igPopID();

            // Hook (Checkbox)
            igTableSetColumnIndex(7);
            igPushID_Int(i * 10 + 7);
            bool hook = inp->m_Hook;
            if (igCheckbox("##H", &hook))
              inp->m_Hook = hook;
            igPopID();

            // weapon
            igTableSetColumnIndex(8);
            igPushID_Int(i * 10 + 8);
            int wpn_temp = (int)inp->m_WantedWeapon;
            igInputInt("##Wpn", &wpn_temp, 0, 0, 0);
            wpn_temp = iclamp(wpn_temp, 0, 4);
            if (wpn_temp != (int)inp->m_WantedWeapon)
              inp->m_WantedWeapon = (int8_t)wpn_temp;
            igPopID();
          }

          igEndTable();
        }

        igEndChild(); // End InputsScroll
      } else {
        igText("Snippet has no inputs");
      }
    } else {
      igText("No snippet selected");
    }
  }
  igEnd();
}
// player manager panel
static bool g_remove_confirm_needed = true;
static int g_pending_remove_index = -1;

// todo: move this to timeline.c?
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

  ts->vec.current_size = 1;

  // Update timeline copies with the new world
  wc_copy_world(&ts->vec.data[0], &ph->world);
  wc_copy_world(&ts->previous_world, &ph->world);

  wc_remove_character(&ph->world, index);
}

void render_player_manager(timeline_state_t *ts, ph_t *ph) {
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
          ts->player_tracks[i].player_info.name[0] ? ts->player_tracks[i].player_info.name : "(Unnamed)";

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
          remove_player(ts, ph, i);
        }
      }
      igPopID();
    }
    if (ts->player_track_count > 0)
      igSeparator();

    if (igBeginPopupModal("Confirm remove player", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
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

void on_camera_update(gfx_handler_t *handler, bool hovered) {
  camera_t *camera = &handler->renderer.camera;
  ImGuiIO *io = igGetIO_Nil();

  float scroll_y = !hovered ? 0.0f : io->MouseWheel;
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
  timeline_init(&ui->timeline);
  camera_init(&gfx_handler->renderer.camera);
  skin_manager_init(&ui->skin_manager);
  NFD_Init();
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

  // DEBUG:
  // wc_copy_world(&world, &ui->timeline.vec.data[0]);
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

  int i = 0;
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
    ++i;
  }
  wc_copy_world(&ui->timeline.previous_world, &world);

  float intra =
      fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / ui->timeline.playback_speed), 1.f);
  if (igIsKeyDown_Nil(ImGuiKey_C))
    intra = 1.f - intra;

  // quick fix the camera so it is updated in time for all the transformations
  if (ui->timeline.recording) {
    SCharacterCore *core = &world.m_pCharacters[gfx->user_interface.timeline.selected_player_track_index];
    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);
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
    bool running =
        fabsf(vgetx(core->m_Vel) * 256.f) >= 5000; // 19.53125 is an ugly number so im keeping this here
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

    vec2 dir = (vec2){core->m_Input.m_TargetX, core->m_Input.m_TargetY};
    glm_vec2_normalize(dir);
    int skin = gfx->user_interface.timeline.player_tracks[i].player_info.skin;
    int eye = get_flag_eye_state(&core->m_Input);
    // TODO: implement spec in the physics properly
    if (core->m_FreezeTime > 0) {
      skin = gfx->x_ninja_skin;
      if (eye == 0)
        eye = EYE_BLINK;
    }
    renderer_push_skin_instance(gfx, p, 1.0f, skin, eye, dir,
                                &anim_state); // normal eyes

    // if (i == gfx->user_interface.timeline.selected_player_track_index)
    //   renderer_draw_line(gfx, p, (vec2){p[0] + vgetx(core->m_Vel) / 32.f, p[1] + vgety(core->m_Vel)
    //   / 32.f},
    //                      (vec4){1.f, 0.f, 0.f, 1.f}, 0.05);

    if (core->m_HookState >= 1)
      renderer_draw_line(gfx, p, (vec2){vgetx(core->m_HookPos) / 32.f, vgety(core->m_HookPos) / 32.f},
                         (vec4){1.f, 1.f, 1.f, 1.f}, 0.05);
  }
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

    renderer_draw_circle_filled(gfx, p, 0.2, (vec4){1.f, 0.f, 0.f, 0.9f}, 8);
  }
  for (SLaser *ent = world.m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; ent;
       ent = ent->m_Base.m_pNextTypeEntity) {
    vec2 p1 = {vgetx(ent->m_Base.m_Pos) / 32.f, vgety(ent->m_Base.m_Pos) / 32.f};
    vec2 p0 = {vgetx(ent->m_From) / 32.f, vgety(ent->m_From) / 32.f};

    vec4 lsr_col = (vec4){0.f, 0.f, 1.f, 0.9f};
    vec4 sg_col = (vec4){0.570315f, 0.4140625f, 025.f, 0.9f};

    renderer_draw_line(gfx, p0, p1, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 0.25f);
    renderer_draw_circle_filled(gfx, p0, 0.2, ent->m_Type == WEAPON_LASER ? lsr_col : sg_col, 8);
  }

  // render own cursor
  if (ui->timeline.recording) {
    SCharacterCore *core = &world.m_pCharacters[gfx->user_interface.timeline.selected_player_track_index];
    vec2 ppp = {vgetx(core->m_PrevPos) / 32.f, vgety(core->m_PrevPos) / 32.f};
    vec2 pp = {vgetx(core->m_Pos) / 32.f, vgety(core->m_Pos) / 32.f};
    vec2 p;
    lerp(ppp, pp, intra, p);
    renderer_draw_circle_filled(gfx,
                                (vec2){p[0] + gfx->user_interface.timeline.recording_input.m_TargetX / 64.f,
                                       p[1] + gfx->user_interface.timeline.recording_input.m_TargetY / 64.f},
                                0.25, (vec4){1.f, 0.f, 0.f, 0.4f}, 16);
  }

  // int pos_x, pos_y;
  // float vel_x, vel_y;
  // int freezetime;
  // int reloadtime;
  // int weapon;
  // bool weapons[NUM_WEAPONS];
  // fill data to be displayed
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

  if (ui->timeline.selected_player_track_index >= 0 && ui->show_prediction) {
    // draw the first line from the currently interpolated position to the actual position
    for (int p = 0; p < world.m_NumCharacters; ++p) {
      SCharacterCore *core = &world.m_pCharacters[ui->timeline.selected_player_track_index];
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

    for (int i = 0; i < 100; ++i) {
      for (int p = 0; p < world.m_NumCharacters; ++p) {
        SPlayerInput input = ui->timeline.recording && p == ui->timeline.selected_player_track_index
                                 ? ui->timeline.recording_input
                                 : get_input(&ui->timeline, p, world.m_GameTick);
        cc_on_input(&world.m_pCharacters[p], &input);
      }
      wc_tick(&world);

      for (int p = 0; p < world.m_NumCharacters; ++p) {
        SCharacterCore *core = &world.m_pCharacters[ui->timeline.selected_player_track_index];
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
  }
  wc_free(&world);
}

void ui_render(ui_handler_t *ui) {
  timeline_update_inputs(&ui->timeline, ui->gfx_handler);

  render_menu_bar(ui);
  setup_docking(ui);
  if (ui->show_timeline) {
    render_timeline(&ui->timeline);
    render_player_manager(&ui->timeline, &ui->gfx_handler->physics_handler);
    render_snippet_editor_panel(&ui->timeline);
    if (ui->timeline.selected_player_track_index != -1)
      render_player_info(ui->gfx_handler);
  }
  if (ui->show_skin_manager)
    render_skin_manager(ui->gfx_handler);
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
  timeline_cleanup(&ui->timeline);
  skin_manager_free(&ui->skin_manager);
  NFD_Quit();
}
