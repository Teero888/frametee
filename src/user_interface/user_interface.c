#include "user_interface.h"
#include "cglm/vec2.h"
#include "cimgui.h"
#include "player_profile.h"
#include "starting_state.h"
#include "snippet_editor.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "timeline_events.h"
#include "undo_redo.h"
#include "widgets/imcol.h"
#include <GLFW/glfw3.h>
#include <engine/engine_api.h>
#include <engine/int_math.h>
#include <frametee/icons.h>
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
#include <system/config.h>
#include <system/fs.h>
#include <system/include_cimgui.h>
#include <system/input.h>
#include <system/save.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *LOG_SOURCE = "UI";

// The name a Save As dialog starts with, derived from the active level.
static void project_default_file_name(ui_handler_t *ui, char *out, size_t out_size) {
  const char *level_name = (ui->loaded_level_name[0] != '\0') ? ui->loaded_level_name : "unnamed_level";
  snprintf(out, out_size, "%s.tasp", level_name);
}

static void render_game_ui_slot(ui_handler_t *ui, ft_ui_slot slot, int track_index) {
  if (!ui || !game_host_ready(&ui->gfx_handler->game_host)) return;
  timeline_state_t *timeline = &ui->timeline;
  int group_index = model_track_group_index(timeline, track_index);
  if (group_index < 0) group_index = timeline->active_group_index;

  ft_ui_frame frame = {0};
  frame.struct_size = sizeof(frame);
  frame.slot = slot;
  frame.tick = timeline->current_tick;
  frame.player = model_group_local_track_index(timeline, track_index);
  if (ui->gfx_handler->level && group_index >= 0 && group_index < timeline->group_count)
    frame.world = model_group_world_at_tick(timeline, group_index, timeline->current_tick);
  engine_api_fill_state(&frame.state);
  gh_ui(&ui->gfx_handler->game_host, &frame);
}

void render_menu_bar(ui_handler_t *ui) {
  if (igBeginMainMenuBar()) {
    if (igBeginMenu("File", true)) {
      if (igMenuItem_Bool("New Project", NULL, false, true)) {
        ui_request_new_project(ui);
      }
      igSeparator();
      if (igMenuItem_Bool("Save Project", "Ctrl+S", false, true)) {
        ui_quick_save(ui);
      }
      if (igMenuItem_Bool("Save Project As...", "Ctrl+Shift+S", false, true)) {
        nfdu8char_t *save_path;
        nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
        char default_file_name[256];
        project_default_file_name(ui, default_file_name, sizeof(default_file_name));
        nfdresult_t result = NFD_SaveDialogU8(&save_path, filters, 1, NULL, default_file_name);
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
      igMenuItem_BoolPtr("Controls", NULL, &ui->keybinds.show_settings_window, true);
      igMenuItem_BoolPtr("Undo History", NULL, &ui->undo_manager.show_history_window, true);
      igMenuItem_BoolPtr("Timeline Events", NULL, &ui->show_timeline_events_window, true);
      igMenuItem_BoolPtr("Plugin Manager", NULL, &ui->show_plugin_manager, true);
      igEndMenu();
    }

    // plugins menu
    if (igBeginMenu("Plugins", true)) {
      igMenuItem_BoolPtr("Plugin Manager", NULL, &ui->show_plugin_manager, true);
      igSeparator();
      if (ui->plugin_manager.count == 0) {
        igTextDisabled("No plugins found");
      } else {
        for (int i = 0; i < ui->plugin_manager.count; ++i) {
          loaded_plugin_t *p = &ui->plugin_manager.plugins[i];
          bool is_loaded = (p->status == PLUGIN_STATUS_LOADED);
          char label[256];
          snprintf(label, sizeof(label), "%s##menu_%d", (p->info.name && p->info.name[0]) ? p->info.name : p->key, i);
          if (igMenuItem_Bool(label, NULL, is_loaded, true)) {
            plugin_manager_toggle_plugin(&ui->plugin_manager, i);
          }
        }
      }
      igSeparator();
      if (igMenuItem_Bool("Reload All", NULL, false, true)) {
        plugin_manager_reload_all(&ui->plugin_manager, ui->plugin_manager.directory);
      }
      igEndMenu();
    }

    if (igBeginMenu("Settings", true)) {
      if (igBeginMenu("Graphics", true)) {
        if (igCheckbox("VSync", &ui->vsync)) {
          ui->gfx_handler->g_swap_chain_rebuild = true;
          config_save(ui);
        }

        if (igCheckbox("Show FPS", &ui->show_fps)) config_save(ui);

        if (igSliderInt("FPS Limit", &ui->fps_limit, 0, 1000, "%d", 0)) config_save(ui);
        if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("0 = Unlimited");

        if (igDragFloat("LOD Bias", &ui->lod_bias, 0.1f, -5.0f, 5.0f, "%.1f", 0)) {
          ui->gfx_handler->renderer.lod_bias = ui->lod_bias;
          config_save(ui);
        }

        if (igColorEdit3("Background Color", ui->bg_color, ImGuiColorEditFlags_NoInputs)) config_save(ui);
        igSeparator();
        igText("Render Elements");
        if (igCheckbox("Level", &ui->render_level)) config_save(ui);

        // Everything else about how the world looks belongs to the game. It
        // describes its options and the editor draws them, so a module needs no
        // UI toolkit to be configurable.
        ui_render_game_settings(ui);
        igEndMenu();
      }
      if (igBeginMenu("Auto-Save", true)) {
        if (igCheckbox("Enable Auto-Save", &ui->auto_save_enabled)) config_save(ui);
        if (igSliderInt("Interval", &ui->auto_save_interval_sec, 15, 600, "%d s", 0)) config_save(ui);
        igEndMenu();
      }
      render_game_ui_slot(ui, FT_UI_SETTINGS, ui->timeline.selected_player_track_index);
      igEndMenu();
    }

    render_game_ui_slot(ui, FT_UI_MAIN_MENU, ui->timeline.selected_player_track_index);

    if (ui->has_unsaved_changes) {
      igPushStyleColor_Vec4(ImGuiCol_Text, (ImVec4){1.0f, 0.70f, 0.20f, 1.0f});
      igText("*");
      igPopStyleColor(1);
      if (igIsItemHovered(0)) {
        igSetTooltip("Unsaved Changes (Ctrl+S to save)");
      }
    }

    igSeparator();
    render_game_ui_slot(ui, FT_UI_STATUS_BAR, ui->timeline.selected_player_track_index);
    igEndMainMenuBar();
  }
}

// The editor's own nodes, kept so a game's windows can be given a home. Indexed
// by ft_dock_slot; FT_DOCK_FLOATING stays zero and means "leave it alone".
static ImGuiID g_dock_nodes[FT_DOCK_CENTER + 1];

// A game names its own windows, so the editor cannot dock them when it lays
// itself out: the game is not loaded yet. Each one is placed the first time it
// is seen instead, and is the user's to move from then on.
static void place_game_panels(ui_handler_t *ui) {
  enum { MAX_PLACED = 16 };
  static char placed[MAX_PLACED][64];
  static int placed_count = 0;
  static char placed_game[FT_NAME_MAX] = {0};

  game_host_t *host = &ui->gfx_handler->game_host;
  if (!game_host_ready(host)) return;
  const char *game_id = game_host_active_id(host);
  if (!game_id) return;
  if (strcmp(placed_game, game_id) != 0) {
    snprintf(placed_game, sizeof(placed_game), "%s", game_id);
    placed_count = 0;
  }

  uint32_t count = 0;
  const ft_panel_desc *panels = gh_panels(host, &count);
  for (uint32_t i = 0; i < count && placed_count < MAX_PLACED; ++i) {
    const ft_panel_desc *panel = &panels[i];
    if (!panel->window_title || panel->dock <= FT_DOCK_FLOATING || panel->dock > FT_DOCK_CENTER) continue;
    const ImGuiID node = g_dock_nodes[panel->dock];
    if (!node) continue;

    bool already_placed = false;
    for (int j = 0; j < placed_count; ++j)
      if (strcmp(placed[j], panel->window_title) == 0) already_placed = true;
    if (already_placed) continue;

    snprintf(placed[placed_count++], sizeof(placed[0]), "%s", panel->window_title);
    igDockBuilderDockWindow(panel->window_title, node);
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
  host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
  host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0.0f, 0.0f});
  igBegin("DockSpace Host Window", NULL,
          host_window_flags); // pass null for p_open to prevent closing the host window
  igPopStyleVar(3);

  // create the main dockspace
  igDockSpace(main_dockspace_id, (ImVec2){0.0f, 0.0f}, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoWindowMenuButton, NULL);
  igEnd();

  // build the initial layout programmatically
  static bool first_time = true;
  static ImGuiID dock_id_left, dock_id_right, dock_id_center, dock_id_bottom;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // split root into bottom + top remainder
    ImGuiID dock_id_top;
    dock_id_bottom = igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.20f, NULL, &dock_id_top);

    // split top remainder into left + remainder
    dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.25f, NULL, &dock_id_center);
    dock_id_left = igDockBuilderSplitNode(dock_id_center, ImGuiDir_Left, 0.40f, NULL, &dock_id_center);

    igDockBuilderDockWindow("Viewport", dock_id_center);
    igDockBuilderDockWindow("Controls", dock_id_center);
    igDockBuilderDockWindow("Undo History", dock_id_center);

    igDockBuilderDockWindow("Timeline", dock_id_bottom);

    igDockBuilderDockWindow("Players", dock_id_left);
    igDockBuilderDockWindow("Snippet Editor", dock_id_right);

    for (int i = 0; i < ui->plugin_manager.count; ++i) {
      loaded_plugin_t *p = &ui->plugin_manager.plugins[i];
      if (p->info.name) {
        igDockBuilderDockWindow(p->info.name, dock_id_right);
      }
    }

    igDockBuilderFinish(main_dockspace_id);
  }

  g_dock_nodes[FT_DOCK_LEFT] = dock_id_left;
  g_dock_nodes[FT_DOCK_RIGHT] = dock_id_right;
  g_dock_nodes[FT_DOCK_BOTTOM] = dock_id_bottom;
  g_dock_nodes[FT_DOCK_CENTER] = dock_id_center;
  place_game_panels(ui);
}

// player manager panel
static bool g_remove_confirm_needed = true;
static int g_pending_remove_index = -1;

typedef struct {
  bool active;
  int index;
  char before[MAX_TIMELINE_GROUP_NAME];
} TimelineTextEditUndo;

typedef struct {
  bool active;
  int index;
  float before[4];
} TimelineColorEditUndo;

typedef struct {
  bool active;
  int index;
  int before;
} TimelineIntEditUndo;

static TimelineTextEditUndo g_group_name_edit_undo;
static TimelineTextEditUndo g_track_name_edit_undo;
static TimelineColorEditUndo g_group_color_edit_undo;
static TimelineIntEditUndo g_group_offset_edit_undo;

static void register_timeline_data_change(ui_handler_t *ui, timeline_data_snapshot_t *before, const char *description) {
  undo_command_t *command = commands_create_timeline_data_change(ui, before, description);
  if (command) undo_manager_register_command(&ui->undo_manager, command);
}

void render_player_manager(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  float dpi_scale = gfx_get_ui_scale();
  if (igBegin("Players", NULL, 0)) {
    if (ts->recording) igBeginDisabled(true);

    // The active game decides what configurations are even possible. A game
    // that cannot fork its state keeps one group; one with a fixed cast (a
    // single-player run, say) hides adding and removing players entirely,
    // rather than offering an action that would then be refused.
    game_host_t *host = &ui->gfx_handler->game_host;
    const bool allow_add_player = game_can_add_player(host, ts->player_track_count);
    const bool fixed_cast = game_has_fixed_players(host);

    if (igButton(ICON_FA_PLUS " Group", (ImVec2){0, 0})) {
      char name[MAX_TIMELINE_GROUP_NAME];
      snprintf(name, sizeof(name), "Group %d", ts->group_count + 1);
      timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
      if (before && model_add_group(ts, name)) {
        model_set_active_group(ts, ts->group_count - 1);
        model_sync_tracks_to_world(ts, ts->active_group_index);
        register_timeline_data_change(ui, before, "Add Group");
      } else commands_free_timeline_data_snapshot(before);
    }
    igSameLine(0, 5.0f * dpi_scale);
    if (igButton(ICON_FA_FILE_IMPORT " Import", (ImVec2){0, 0})) {
      nfdu8char_t *path = NULL;
      nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
      if (NFD_OpenDialogU8(&path, filters, 1, NULL) == NFD_OKAY && path) {
        timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
        if (before && import_project_as_group(ui, path)) register_timeline_data_change(ui, before, "Import Project as Group");
        else commands_free_timeline_data_snapshot(before);
        NFD_FreePathU8(path);
      }
    }
    if (ts->group_count > 1) {
      igSameLine(0, 5.0f * dpi_scale);
      if (igButton(ICON_FA_ARROWS_LEFT_RIGHT_TO_LINE " Align starts", (ImVec2){0, 0})) {
        timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
        if (before) {
          model_align_group_starts(ts);
          register_timeline_data_change(ui, before, "Align Group Starts");
        }
      }
      if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("Align every group race start to Group 1");
    }
    if (ts->recording) igEndDisabled();

    igSeparator();
    if (!fixed_cast) {
      static int num_to_add = 1;
      igPushItemWidth(50 * dpi_scale);
      const int headroom = game_max_players(host) > 0 ? game_max_players(host) - ts->player_track_count : 1000;
      igDragInt("##NumToAdd", &num_to_add, 1, 1, headroom > 1 ? headroom : 1, "%d", ImGuiSliderFlags_None);
      igPopItemWidth();
      if (num_to_add < 1) num_to_add = 1;
      if (headroom > 0 && num_to_add > headroom) num_to_add = headroom;

      igSameLine(0, 5.0f * dpi_scale);

      char aLabel[16];
      snprintf(aLabel, 16, "Add Player%s", num_to_add > 1 ? "s" : "");
      if (!allow_add_player) igBeginDisabled(true);
      if (ui->gfx_handler->level && igButton(aLabel, (ImVec2){0, 0})) {
        for (int i = 0; i < num_to_add && game_can_add_player(host, ts->player_track_count); ++i) {
          undo_command_t *cmd = timeline_api_create_track(ui, NULL, NULL);
          if (cmd) undo_manager_register_command(&ui->undo_manager, cmd);
        }
      }
      if (!allow_add_player) {
        igEndDisabled();
        if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          igSetTooltip("%s allows at most %d players.", ui->plugin_context.active_game_id, game_max_players(host));
      }
      igSameLine(0, 10.f * dpi_scale);
    }
    igText("Players: %d%s  |  Active: %s", ts->player_track_count, fixed_cast ? " (fixed by the game)" : "",
           ts->groups[ts->active_group_index]->name);

    igSeparator();
    int pending_group_remove = -1;
    int pending_track_remove = -1;
    int pending_clone_track = -1;
    int pending_clone_group = -1;
    for (int group_index = 0; group_index < ts->group_count; ++group_index) {
      timeline_group_t *group = ts->groups[group_index];
      igPushID_Int(10000 + group_index);

      ImVec4 header_color = {group->color[0], group->color[1], group->color[2], group_index == ts->active_group_index ? 0.55f : 0.28f};
      igPushStyleColor_Vec4(ImGuiCol_Header, header_color);
      igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, (ImVec4){group->color[0], group->color[1], group->color[2], 0.65f});
      bool open = igCollapsingHeader_TreeNodeFlags(group->name, ImGuiTreeNodeFlags_DefaultOpen);
      igPopStyleColor(2);
      if (igIsItemClicked(ImGuiMouseButton_Left)) model_set_active_group(ts, group_index);
      if (igBeginPopupContextItem("##group_context", ImGuiPopupFlags_MouseButtonRight)) {
        bool can_delete = ts->group_count > 1 && !ts->recording;
        if (!can_delete) igBeginDisabled(true);
        if (igMenuItem_Bool(ICON_FA_TRASH " Delete group", NULL, false, can_delete)) pending_group_remove = group_index;
        if (!can_delete) igEndDisabled();
        igEndPopup();
      }

      if (open) {
        char name_before_frame[MAX_TIMELINE_GROUP_NAME];
        memcpy(name_before_frame, group->name, sizeof(name_before_frame));
        igSetNextItemWidth(155.0f * dpi_scale);
        bool name_changed = igInputText("Name", group->name, sizeof(group->name), ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        if (igIsItemActivated()) {
          g_group_name_edit_undo.active = true;
          g_group_name_edit_undo.index = group_index;
          memcpy(g_group_name_edit_undo.before, name_before_frame, sizeof(g_group_name_edit_undo.before));
        }
        if (name_changed) ui_mark_unsaved(ui);
        if (igIsItemDeactivatedAfterEdit() && g_group_name_edit_undo.active && g_group_name_edit_undo.index == group_index) {
          undo_command_t *command = commands_create_group_name_change(ui, group_index, g_group_name_edit_undo.before);
          if (command) undo_manager_register_command(&ui->undo_manager, command);
          g_group_name_edit_undo.active = false;
        }

        float color_before_frame[4];
        memcpy(color_before_frame, group->color, sizeof(color_before_frame));
        igSameLine(0, 5.0f * dpi_scale);
        igSetNextItemWidth(45.0f * dpi_scale);
        bool color_changed = igColorEdit3("Color", group->color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        if (igIsItemActivated()) {
          g_group_color_edit_undo.active = true;
          g_group_color_edit_undo.index = group_index;
          memcpy(g_group_color_edit_undo.before, color_before_frame, sizeof(g_group_color_edit_undo.before));
        }
        if (color_changed) ui_mark_unsaved(ui);
        if (igIsItemDeactivatedAfterEdit() && g_group_color_edit_undo.active && g_group_color_edit_undo.index == group_index) {
          undo_command_t *command = commands_create_group_color_change(ui, group_index, g_group_color_edit_undo.before);
          if (command) undo_manager_register_command(&ui->undo_manager, command);
          g_group_color_edit_undo.active = false;
        }

        igSameLine(0, 8.0f * dpi_scale);
        bool visible_before = group->visible;
        if (igCheckbox("Show in viewport", &group->visible)) {
          undo_command_t *command = commands_create_group_visibility_change(ui, group_index, visible_before);
          if (command) undo_manager_register_command(&ui->undo_manager, command);
        }

        if (group_index == 0) {
          group->start_offset = 0;
          igBeginDisabled(true);
        }
        int offset_before_frame = group->start_offset;
        igSetNextItemWidth(120.0f * dpi_scale);
        bool offset_changed = igDragInt("Start offset", &group->start_offset, 1.0f, -1000000, 1000000, "%d ticks", ImGuiSliderFlags_AlwaysClamp);
        if (igIsItemActivated()) {
          g_group_offset_edit_undo.active = true;
          g_group_offset_edit_undo.index = group_index;
          g_group_offset_edit_undo.before = offset_before_frame;
        }
        if (offset_changed) {
          model_recalc_physics(ts, 0);
          ui_mark_unsaved(ui);
        }
        if (igIsItemDeactivatedAfterEdit() && g_group_offset_edit_undo.active && g_group_offset_edit_undo.index == group_index) {
          undo_command_t *command = commands_create_group_start_offset_change(ui, group_index, g_group_offset_edit_undo.before);
          if (command) undo_manager_register_command(&ui->undo_manager, command);
          g_group_offset_edit_undo.active = false;
        }
        if (group_index == 0) igEndDisabled();

        const ft_world *world = model_group_world_at_tick(ts, group_index, ts->current_tick);
        const int player_count = gh_world_player_count(host, world);
        for (int local_index = 0; local_index < player_count; ++local_index) {
          int i = model_group_track_index(ts, group_index, local_index);
          if (i < 0) continue;
          player_track_t *track = &ts->player_tracks[i];
          igPushID_Int(i);
          bool selected = i == ts->selected_player_track_index;
          // The editor's own label for the track. Who the player is (a
          // nickname, a number, a car) is the game's to say, and it says it
          // through player_label further down this row.
          char row_label[160];
          snprintf(row_label, sizeof(row_label), "%s", track->name[0] ? track->name : "Track");

          ImDrawList *row_draw_list = igGetWindowDrawList();
          ImVec4 row_color = {0.18f, 0.18f, 0.18f, 0.55f};
          ImVec4 row_selected_color = {0.30f, 0.30f, 0.30f, 0.70f};
          ImVec4 row_hovered_color = {0.25f, 0.25f, 0.25f, 0.75f};
          ImVec4 row_active_color = {0.34f, 0.34f, 0.34f, 0.80f};

          // Keep Selectable's native text-height layout so the label and the timing text placed on
          // the same line share a baseline. Draw the persistent tint in a lower channel after the
          // exact item rectangle is known.
          ImDrawList_ChannelsSplit(row_draw_list, 2);
          ImDrawList_ChannelsSetCurrent(row_draw_list, 1);
          float row_left_padding = 12.0f * dpi_scale;
          igIndent(row_left_padding);
          igPushStyleColor_Vec4(ImGuiCol_Header, row_selected_color);
          igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, row_hovered_color);
          igPushStyleColor_Vec4(ImGuiCol_HeaderActive, row_active_color);
          bool track_clicked = igSelectable_Bool(row_label, selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns,
                                                 (ImVec2){0, 0});
          igPopStyleColor(3);
          igUnindent(row_left_padding);

          ImVec2 row_min = igGetItemRectMin();
          ImVec2 row_max = igGetItemRectMax();
          ImDrawList_ChannelsSetCurrent(row_draw_list, 0);
          ImDrawList_AddRectFilled(row_draw_list, row_min, row_max, igGetColorU32_Vec4(row_color), 3.0f * dpi_scale, ImDrawFlags_RoundCornersAll);

          ImDrawList_ChannelsSetCurrent(row_draw_list, 1);
          ImU32 rail_color = igGetColorU32_Vec4((ImVec4){0.65f, 0.65f, 0.65f, 0.85f});
          ImVec2 rail_min = {row_min.x + 1.0f * dpi_scale, row_min.y + 2.0f * dpi_scale};
          ImVec2 rail_max = {row_min.x + 4.0f * dpi_scale, row_max.y - 2.0f * dpi_scale};
          ImDrawList_AddRectFilled(row_draw_list, rail_min, rail_max, rail_color, 1.5f * dpi_scale, ImDrawFlags_RoundCornersAll);
          ImDrawList_ChannelsMerge(row_draw_list);
          if (track_clicked) interaction_select_track(ts, i);

          if (igBeginPopupContextItem("##track_context", ImGuiPopupFlags_MouseButtonRight)) {
            interaction_select_track(ts, i);
            char track_name_before_frame[MAX_TRACK_NAME];
            memcpy(track_name_before_frame, track->name, sizeof(track_name_before_frame));
            igSetNextItemWidth(180.0f * dpi_scale);
            bool track_name_changed = igInputText("Track name", track->name, sizeof(track->name), ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
            if (igIsItemActivated()) {
              g_track_name_edit_undo.active = true;
              g_track_name_edit_undo.index = i;
              memcpy(g_track_name_edit_undo.before, track_name_before_frame, sizeof(g_track_name_edit_undo.before));
            }
            if (track_name_changed) ui_mark_unsaved(ui);
            if (igIsItemDeactivatedAfterEdit() && g_track_name_edit_undo.active && g_track_name_edit_undo.index == i) {
              undo_command_t *command = commands_create_track_name_change(ui, i, g_track_name_edit_undo.before);
              if (command) undo_manager_register_command(&ui->undo_manager, command);
              g_track_name_edit_undo.active = false;
            }
            if (ts->group_count > 1 && igBeginMenu("Clone to group", !ts->recording)) {
              for (int target_group = 0; target_group < ts->group_count; ++target_group) {
                if (target_group == group_index) continue;
                if (igMenuItem_Bool(ts->groups[target_group]->name, NULL, false, true)) {
                  pending_clone_track = i;
                  pending_clone_group = target_group;
                }
              }
              igEndMenu();
            }
            igSeparator();
            if (igMenuItem_Bool(ICON_FA_TRASH " Delete Player Track", NULL, false, true)) {
              if (g_remove_confirm_needed && track->snippet_count > 0) {
                g_pending_remove_index = i;
                igOpenPopup_Str("Confirm remove player", ImGuiPopupFlags_AnyPopupLevel);
              } else {
                pending_track_remove = i;
              }
            }
            igEndPopup();
          }

          // Whatever the game wants shown next to a player in a list: DDNet puts
          // the finish time or the last checkpoint there.
          char annotation[64];
          if (gh_player_label(host, world, local_index, annotation, sizeof(annotation)) && annotation[0]) {
            igSameLine(0, 10.f * dpi_scale);
            igTextDisabled("%s", annotation);
          }
          render_game_ui_slot(ui, FT_UI_PLAYER_ROW, i);
          igPopID();
        }

        igSeparator();
      }
      igPopID();
    }
    if (pending_clone_track >= 0) {
      timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
      if (before && model_clone_track_to_group(ts, pending_clone_track, pending_clone_group, NULL))
        register_timeline_data_change(ui, before, "Clone Track to Group");
      else commands_free_timeline_data_snapshot(before);
    }
    if (pending_track_remove >= 0) {
      undo_command_t *cmd = commands_create_remove_track(ui, pending_track_remove);
      if (cmd) undo_manager_register_command(&ui->undo_manager, cmd);
    }
    if (pending_group_remove >= 0) {
      timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
      if (before && model_remove_group(ts, pending_group_remove)) register_timeline_data_change(ui, before, "Delete Group");
      else commands_free_timeline_data_snapshot(before);
    }
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

// The viewport camera for a game whose world is a volume: drag to orbit, scroll
// to pull in and out. The 2D path below is left exactly as it was, because
// panning a plane and orbiting a volume have nothing useful in common.
static void on_camera3_update(gfx_handler_t *handler, bool hovered, float intra) {
  camera3_t *c = &handler->renderer.camera3;
  keybind_manager_t *keys = &handler->user_interface.keybinds;
  const bool typing = igIsAnyItemActive();

  // Flying is a camera mode, so selecting it is what turns it on. The renderer
  // carries the view across the swap; noticing that the mode changed is all
  // there is to do here.
  const bool want_free = game_camera_mode_is_freecam(&handler->game_host, handler->renderer.camera.mode);
  if (want_free != c->free_mode) renderer_camera3_toggle_free(handler);

  float scroll_y = !hovered ? 0.f : (float)input_scroll_y();
  if (!typing) {
    if (keybinds_is_action_pressed(keys, ACTION_ZOOM_IN, true)) scroll_y = 1.f;
    if (keybinds_is_action_pressed(keys, ACTION_ZOOM_OUT, true)) scroll_y = -1.f;
  }

  double raw_dx = 0.0, raw_dy = 0.0;
  input_mouse_delta(&raw_dx, &raw_dy);
  // The same button that pans a 2D view turns a 3D one, so the gesture a user
  // already knows carries over into both modes.
  const bool drag_down = input_mouse_down(GLFW_MOUSE_BUTTON_RIGHT);
  if (drag_down && (hovered || c->orbiting)) {
    c->orbiting = true;
    c->yaw += (float)raw_dx * 0.005f;
    c->pitch += (float)raw_dy * 0.005f;
    // Stop just short of the poles: looking straight down makes the up vector
    // ambiguous and the view flips.
    c->pitch = glm_clamp(c->pitch, -1.5f, 1.5f);
  } else {
    c->orbiting = false;
  }

  if (!c->free_mode) {
    // Orbiting: the wheel changes how far out the camera sits.
    if (scroll_y != 0.f) c->distance = glm_clamp(c->distance * (1.f - scroll_y * 0.1f), 1.f, 100000.f);

    timeline_state_t *ts = &handler->user_interface.timeline;
    const int group_index = model_track_group_index(ts, ts->selected_player_track_index);
    const ft_world *previous = NULL;
    const ft_world *current = NULL;
    if (group_index >= 0) model_group_world_pair(ts, group_index, ts->current_tick, &previous, &current);

    ft_camera_frame frame = {0};
    frame.struct_size = sizeof(frame);
    frame.mode = handler->renderer.camera.mode;
    frame.world = current;
    frame.previous_world = previous;
    frame.alpha = intra;
    frame.player = model_group_local_track_index(ts, ts->selected_player_track_index);
    frame.recording = ts->recording;

    ft_camera view;
    engine_api_camera_get(&view);
    if (gh_camera_update(&handler->game_host, &frame, &view)) {
      c->target[0] = view.target.x;
      c->target[1] = view.target.y;
      c->target[2] = view.target.z;

      vec3 offset;
      offset[0] = view.eye.x - view.target.x;
      offset[1] = view.eye.y - view.target.y;
      offset[2] = view.eye.z - view.target.z;
      float dist = glm_vec3_norm(offset);
      if (dist > 0.01f && !c->orbiting) {
        c->distance = dist;
        c->pitch = asinf(glm_clamp(offset[1] / dist, -1.5f, 1.5f));
        c->yaw = atan2f(offset[2], offset[0]);
      }
    } else if (current) {
      ft_value pos_val;
      int player_idx = frame.player >= 0 ? frame.player : 0;
      if (gh_entity_prop_get(&handler->game_host, current, FT_ENTITY_CLASS_PLAYER, player_idx, 0, &pos_val) &&
          pos_val.kind == FT_VALUE_VEC3) {
        if (previous) {
          ft_value prev_val;
          if (gh_entity_prop_get(&handler->game_host, previous, FT_ENTITY_CLASS_PLAYER, player_idx, 0, &prev_val) &&
              prev_val.kind == FT_VALUE_VEC3) {
            c->target[0] = glm_lerp(prev_val.as.v3.x, pos_val.as.v3.x, intra);
            c->target[1] = glm_lerp(prev_val.as.v3.y, pos_val.as.v3.y, intra);
            c->target[2] = glm_lerp(prev_val.as.v3.z, pos_val.as.v3.z, intra);
          } else {
            c->target[0] = pos_val.as.v3.x;
            c->target[1] = pos_val.as.v3.y;
            c->target[2] = pos_val.as.v3.z;
          }
        } else {
          c->target[0] = pos_val.as.v3.x;
          c->target[1] = pos_val.as.v3.y;
          c->target[2] = pos_val.as.v3.z;
        }
      }
    }
    return;
  }

  // Flying: the wheel trims how fast, because a speed that suits crossing a
  // level is unusable for lining up a shot next to a body.
  if (scroll_y != 0.f) c->move_speed = glm_clamp(c->move_speed * (1.f + scroll_y * 0.15f), 0.5f, 10000.f);
  if (typing) return;
  // Flying is driven entirely from the keyboard, and letters are what other
  // panels are being used with: a shortcut that flies the camera while someone
  // is working in the timeline or a plugin window is a bug, not a shortcut.
  // Orbiting needs no such gate, because it is driven by the mouse and already
  // asks whether the cursor is over the viewport.
  if (!handler->user_interface.viewport_focused) return;

  vec3 forward, right, up;
  renderer_camera3_forward(handler, forward);
  glm_vec3_cross(forward, (vec3){0.f, 1.f, 0.f}, right);
  glm_vec3_normalize(right);
  // The camera's own up, so rising while pitched moves along the view rather
  // than along the world axis.
  glm_vec3_cross(right, forward, up);
  glm_vec3_normalize(up);

  vec3 move = {0.f, 0.f, 0.f};
  if (keybinds_is_action_held(keys, ACTION_FREECAM_FORWARD)) glm_vec3_add(move, forward, move);
  if (keybinds_is_action_held(keys, ACTION_FREECAM_BACK)) glm_vec3_sub(move, forward, move);
  if (keybinds_is_action_held(keys, ACTION_FREECAM_RIGHT)) glm_vec3_add(move, right, move);
  if (keybinds_is_action_held(keys, ACTION_FREECAM_LEFT)) glm_vec3_sub(move, right, move);
  if (keybinds_is_action_held(keys, ACTION_FREECAM_UP)) glm_vec3_add(move, up, move);
  if (keybinds_is_action_held(keys, ACTION_FREECAM_DOWN)) glm_vec3_sub(move, up, move);
  if (glm_vec3_norm(move) <= 0.f) return;

  glm_vec3_normalize(move);
  float speed = c->move_speed * igGetIO_Nil()->DeltaTime;
  if (keybinds_is_action_held(keys, ACTION_FREECAM_FAST)) speed *= 4.f;
  glm_vec3_scale(move, speed, move);
  glm_vec3_add(c->eye, move, c->eye);
}

void on_camera_update(gfx_handler_t *handler, bool hovered, float intra) {
  if (!handler->level) return;
  if (game_is_3d(&handler->game_host)) {
    on_camera3_update(handler, hovered, intra);
    return;
  }
  camera_t *camera = &handler->renderer.camera;
  ImGuiIO *io = igGetIO_Nil();

  // Scroll comes from the GLFW callback: imgui holds back the mouse position events queued behind a
  // wheel event, so reading both from imgui made panning stutter exactly while zooming.
  float scroll_y = !hovered ? 0.0f : (float)input_scroll_y();
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
  float map_ratio = handler->world_width / handler->world_height;
  float aspect = (float)viewport_ratio / (float)map_ratio;

  // Panning uses the summed GLFW motion for this frame so it stays in step with the mouse no matter
  // how many events arrived between frames. Not gated on io->WantCaptureMouse: the viewport is an
  // imgui window, so that flag is set the whole time the cursor is over it; `hovered` decides where
  // a pan may start.
  double raw_dx = 0.0, raw_dy = 0.0;
  input_mouse_delta(&raw_dx, &raw_dy);
  bool pan_button_down = input_mouse_down(GLFW_MOUSE_BUTTON_RIGHT);

  game_host_t *host = &handler->game_host;
  const ft_camera_mode *mode = game_camera_mode(host, camera->mode);
  const bool directed = mode && (mode->flags & FT_CAMERA_MODE_DIRECTED) != 0;

  // Panning out of a directed mode drops back to a free one, so a drag always
  // does something.
  if (directed && hovered && pan_button_down) {
    for (unsigned i = 0; i < game_camera_mode_count(host); ++i) {
      const ft_camera_mode *candidate = game_camera_mode(host, i);
      if (candidate && (candidate->flags & FT_CAMERA_MODE_FREE)) {
        camera->mode = i;
        break;
      }
    }
  }

  // Let the game place the camera. It is given the same pair of worlds and the
  // same interpolation the renderer uses, so a locked camera tracks a smoothly
  // drawn player instead of stepping once per tick.
  {
    timeline_state_t *ts = &handler->user_interface.timeline;
    const int group_index = model_track_group_index(ts, ts->selected_player_track_index);
    const ft_world *previous = NULL;
    const ft_world *current = NULL;
    if (group_index >= 0) model_group_world_pair(ts, group_index, ts->current_tick, &previous, &current);

    ft_camera_frame frame = {0};
    frame.struct_size = sizeof(frame);
    frame.mode = camera->mode;
    frame.world = current;
    frame.previous_world = previous;
    frame.alpha = intra;
    frame.player = model_group_local_track_index(ts, ts->selected_player_track_index);
    frame.recording = ts->recording;

    ft_camera view;
    engine_api_camera_get(&view);
    if (gh_camera_update(host, &frame, &view)) {
      if (handler->world_width > 0.f) camera->pos[0] = view.position.x / handler->world_width;
      if (handler->world_height > 0.f) camera->pos[1] = view.position.y / handler->world_height;
      camera->is_dragging = false;
      return;
    }
  }

  // Nobody is directing the camera, so the user drives it. Only start a pan
  // inside the viewport, but keep it running once started even if the cursor
  // wanders over a panel.
  if (pan_button_down && (hovered || camera->is_dragging)) {
    if (!camera->is_dragging) {
      camera->is_dragging = true;
      double mouse_x = 0.0, mouse_y = 0.0;
      input_cursor_pos(&mouse_x, &mouse_y);
      camera->drag_start_pos[0] = (float)mouse_x;
      camera->drag_start_pos[1] = (float)mouse_y;
    }

    const float dx = (float)raw_dx / (handler->viewport[0] * camera->zoom);
    const float dy = (float)raw_dy / (handler->viewport[1] * camera->zoom * aspect);
    const float max_map_size = fmax(handler->world_width, handler->world_height) * 0.001;
    camera->pos[0] -= (dx * 2) / max_map_size;
    camera->pos[1] -= (dy * 2) / max_map_size;
  } else {
    camera->is_dragging = false;
  }
}

void camera_init(camera_t *camera) {
  memset(camera, 0, sizeof(camera_t));
  camera->zoom = 5.0f;
  camera->zoom_wanted = 5.0f;
  camera->mode = 0;
}

void ui_init_config(ui_handler_t *ui) {
  ui->mouse_sens = 200.f;
  ui->mouse_max_distance = 400.f;
  ui->vsync = true;
  ui->fps_limit = 0;
  ui->lod_bias = -0.5f;
  ui->bg_color[0] = 0.253f;
  ui->bg_color[1] = 0.253f;
  ui->bg_color[2] = 0.253f;
  ui->render_level = true;

  ui->auto_save_enabled = true;
  ui->auto_save_interval_sec = 60;
  ui->last_auto_save_time = 0.0;
  keybinds_init(&ui->keybinds);
  config_load(ui);
}

static void ui_apply_theme(void) {
  ImGuiStyle *style = igGetStyle();

  style->WindowPadding = (ImVec2){12.0f, 12.0f};
  style->FramePadding = (ImVec2){8.0f, 4.0f};
  style->ItemSpacing = (ImVec2){8.0f, 8.0f};
  style->ItemInnerSpacing = (ImVec2){6.0f, 6.0f};

  style->WindowRounding = 8.0f;
  style->ChildRounding = 4.0f;
  style->FrameRounding = 4.0f;
  style->ScrollbarRounding = 4.0f;
  style->FrameBorderSize = 1.0f;
  style->WindowBorderSize = 1.0f;

  ImVec4 *colors = style->Colors;

  colors[ImGuiCol_WindowBg] = (ImVec4){0.10f, 0.11f, 0.12f, 1.00f};
  colors[ImGuiCol_PopupBg] = (ImVec4){0.10f, 0.11f, 0.12f, 1.00f};
  colors[ImGuiCol_ChildBg] = (ImVec4){0.10f, 0.11f, 0.12f, 1.00f};

  colors[ImGuiCol_FrameBg] = (ImVec4){0.15f, 0.16f, 0.17f, 1.00f};
  colors[ImGuiCol_FrameBgHovered] = (ImVec4){0.20f, 0.22f, 0.23f, 1.00f};
  colors[ImGuiCol_FrameBgActive] = (ImVec4){0.12f, 0.13f, 0.14f, 1.00f};

  colors[ImGuiCol_Border] = (ImVec4){0.23f, 0.25f, 0.27f, 1.00f};
  colors[ImGuiCol_BorderShadow] = (ImVec4){0.00f, 0.00f, 0.00f, 0.00f};

  colors[ImGuiCol_Button] = (ImVec4){0.16f, 0.45f, 0.85f, 1.00f};
  colors[ImGuiCol_ButtonHovered] = (ImVec4){0.26f, 0.55f, 0.95f, 1.00f};
  colors[ImGuiCol_ButtonActive] = (ImVec4){0.11f, 0.35f, 0.75f, 1.00f};

  colors[ImGuiCol_Text] = (ImVec4){0.88f, 0.89f, 0.91f, 1.00f};
  colors[ImGuiCol_TextDisabled] = (ImVec4){0.54f, 0.57f, 0.60f, 1.00f};

  colors[ImGuiCol_TitleBg] = (ImVec4){0.10f, 0.11f, 0.12f, 1.00f};
  colors[ImGuiCol_TitleBgActive] = (ImVec4){0.15f, 0.16f, 0.17f, 1.00f};
  colors[ImGuiCol_TitleBgCollapsed] = (ImVec4){0.05f, 0.06f, 0.07f, 1.00f};

  colors[ImGuiCol_Header] = (ImVec4){0.16f, 0.45f, 0.85f, 0.50f};
  colors[ImGuiCol_HeaderHovered] = (ImVec4){0.16f, 0.45f, 0.85f, 0.80f};
  colors[ImGuiCol_HeaderActive] = (ImVec4){0.16f, 0.45f, 0.85f, 1.00f};

  colors[ImGuiCol_SliderGrab] = (ImVec4){0.16f, 0.45f, 0.85f, 1.00f};
  colors[ImGuiCol_SliderGrabActive] = (ImVec4){0.26f, 0.55f, 0.95f, 1.00f};

  colors[ImGuiCol_CheckMark] = (ImVec4){0.88f, 0.89f, 0.91f, 1.00f};

  colors[ImGuiCol_Tab] = (ImVec4){0.15f, 0.16f, 0.17f, 1.00f};
  colors[ImGuiCol_TabHovered] = (ImVec4){0.26f, 0.55f, 0.95f, 0.80f};
  colors[ImGuiCol_TabSelected] = (ImVec4){0.16f, 0.45f, 0.85f, 1.00f};
  colors[ImGuiCol_TabDimmed] = (ImVec4){0.10f, 0.11f, 0.12f, 1.00f};
  colors[ImGuiCol_TabDimmedSelected] = (ImVec4){0.15f, 0.16f, 0.17f, 1.00f};

  colors[ImGuiCol_DockingPreview] = (ImVec4){0.16f, 0.45f, 0.85f, 0.70f};
}

void ui_init(ui_handler_t *ui, gfx_handler_t *gfx_handler) {
  extern bool g_is_headless;
  if (!g_is_headless) {
    ImGuiIO *io = igGetIO_Nil();
    ImFontAtlas *atlas = io->Fonts;
    const float scale = gfx_get_ui_scale();
    ui->font = ImFontAtlas_AddFontFromFileTTF(io->Fonts, "data/fonts/Roboto-SemiBold.ttf", 19.f * scale, NULL, NULL);

    static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    ImFontConfig *icons_config = ImFontConfig_ImFontConfig();
    icons_config->MergeMode = true;
    icons_config->PixelSnapH = true;
    ImFontAtlas_AddFontFromFileTTF(atlas, "data/fonts/fa-solid-900.ttf", 15.0f * scale, icons_config, icons_ranges);
    ImFontConfig_destroy(icons_config);

    ImFontConfig *icon_cfg = ImFontConfig_ImFontConfig();
    icon_cfg->PixelSnapH = true;
    ui->icon_font = ImFontAtlas_AddFontFromFileTTF(atlas, "data/fonts/fa-solid-900.ttf", 15.0f * scale, icon_cfg, icons_ranges);
    ImFontConfig_destroy(icon_cfg);
  }

  ui_apply_theme();

  ui->gfx_handler = gfx_handler;
  strncpy(ui->loaded_level_name, "unnamed_level", sizeof(ui->loaded_level_name) - 1);
  ui->loaded_level_path[0] = '\0';
  ui->current_project_path[0] = '\0';
  ui->has_unsaved_changes = false;
  ui->show_timeline = true;
  ui->show_timeline_events_window = false;
  ui->show_plugin_manager = false;
  entity_inspector_clear(&ui->entity_inspector);
  timeline_init(ui);
  camera_init(&gfx_handler->renderer.camera);
  config_apply_game_editor_state(ui);
  undo_manager_init(&ui->undo_manager);
  if (!g_is_headless) {
    NFD_Init();
  }

  ui->plugin_api = api_init(ui);
  ui->plugin_context.imgui_context = igGetCurrentContext();
  ui->plugin_context.is_headless = g_is_headless;
  // Points into the host's slot table, which outlives every plugin.
  ui->plugin_context.active_game_id = game_host_active_id(&gfx_handler->game_host);
  plugin_manager_init(&ui->plugin_manager, &ui->plugin_context, &ui->plugin_api, ui);
  plugin_manager_load_all(&ui->plugin_manager, "plugins");
}

void ui_add_recent_project(ui_handler_t *ui, const char *path) {
  char path_copy[1024];
  strncpy(path_copy, path, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  int found_idx = -1;
  for (int i = 0; i < ui->num_recent_projects; i++) {
    if (strcmp(ui->recent_projects[i], path_copy) == 0) {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1) {
    for (int i = found_idx; i > 0; i--) {
      strcpy(ui->recent_projects[i], ui->recent_projects[i - 1]);
    }
  } else {
    if (ui->num_recent_projects < 10) {
      ui->num_recent_projects++;
    }
    for (int i = ui->num_recent_projects - 1; i > 0; i--) {
      strcpy(ui->recent_projects[i], ui->recent_projects[i - 1]);
    }
  }
  strncpy(ui->recent_projects[0], path_copy, 1023);
  ui->recent_projects[0][1023] = '\0';
  config_save(ui);
}

static void render_new_project_prompt(ui_handler_t *ui) {
  const char *popup_id = "Unsaved Changes##NewProject";

  if (ui->show_new_project_prompt) {
    ui->show_new_project_prompt = false;
    igOpenPopup_Str(popup_id, ImGuiPopupFlags_None);
  }

  ImVec2 center;
  ImGuiViewport *viewport = igGetMainViewport();
  center.x = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
  center.y = viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
  igSetNextWindowPos(center, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f});

  if (igBeginPopupModal(popup_id, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (ui->current_project_path[0] != '\0') {
      igText("'%s' has unsaved changes.", ui->current_project_path);
    } else {
      igText("The current project has unsaved changes.");
    }
    igText("Save them before starting a new project?");

    igSpacing();
    igSeparator();
    igSpacing();

    float dpi = gfx_get_ui_scale();
    ImVec2 button_size = {110.0f * dpi, 0.0f};

    if (igButton("Save", button_size)) {
      // a cancelled or failed save leaves the dialog up instead of moving on
      if (ui_quick_save(ui)) {
        ui->show_splash = true;
        igCloseCurrentPopup();
      }
    }
    igSameLine(0.0f, 8.0f);
    if (igButton("Don't Save", button_size)) {
      ui->show_splash = true;
      igCloseCurrentPopup();
    }
    igSameLine(0.0f, 8.0f);
    if (igButton("Cancel", button_size)) {
      igCloseCurrentPopup();
    }
    igEndPopup();
  }
}

// Picking a bare level from the splash starts a fresh project, so the previously open project's path
// must not stay behind or the next Ctrl+S would silently overwrite it.
static void splash_on_level_picked(ui_handler_t *ui) {
  ui->current_project_path[0] = '\0';
  ui->has_unsaved_changes = false;
  ui->last_auto_save_time = 0.0;
  ui->show_splash = false;
}

// --- splash: choosing a game -------------------------------------------------

// Thumbnails are loaded once per game and kept for the session. Cached here
// rather than in the host because they are purely a UI concern.
typedef struct {
  texture_t *texture;
  struct ImTextureRef_c *ref;
  bool attempted;
} game_thumbnail_t;

static game_thumbnail_t g_game_thumbnails[32];

static struct ImTextureRef_c *splash_game_thumbnail(gfx_handler_t *gfx, const game_module_slot_t *slot, int index) {
  if (index < 0 || index >= (int)(sizeof(g_game_thumbnails) / sizeof(g_game_thumbnails[0]))) return NULL;
  game_thumbnail_t *entry = &g_game_thumbnails[index];
  if (entry->attempted) return entry->ref;
  entry->attempted = true;

  const char *relative = slot->module ? slot->module->info.thumbnail : NULL;
  if (!relative || !*relative) return NULL;

  char path[GAME_HOST_MAX_PATH];
  snprintf(path, sizeof(path), "data/games/%s/%s", slot->id, relative);
  FILE *file = fs_open(path, "rb");
  if (!file) return NULL;
  fclose(file);

  entry->texture = renderer_load_texture(gfx, path);
  if (entry->texture) {
    entry->ref = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
        entry->texture->sampler, entry->texture->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  }
  return entry->ref;
}

// Returns true when a game was chosen. Switching is safe here precisely because
// nothing is open: worlds and inputs belong to whichever game made them, so the
// start screen is the one place the choice can still change freely.
static bool render_splash_game_picker(ui_handler_t *ui, float width) {
  (void)width;
  game_host_t *host = &ui->gfx_handler->game_host;
  bool chosen = false;

  igTextColored((ImVec4){0.85f, 0.90f, 1.00f, 1.00f}, "%s", "Games");
  igTextDisabled("Everything the editor simulates and draws comes from the game you pick.");
  igSpacing();

  // A grid of whole-card click targets drawn straight to the draw list, with no
  // nested buttons.
  const ImVec2 grid_avail = igGetContentRegionAvail();
  const float card_width = 320.0f;
  const float card_margin = 5.0f;
  int columns = (int)(grid_avail.x / (card_width + card_margin * 2.0f));
  if (columns < 1) columns = 1;

  igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, (ImVec2){card_margin, card_margin});
  if (igBeginTable("SplashGameGrid", columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0, 0}, 0)) {
    for (int i = 0; i < host->count; ++i) {
      const game_module_slot_t *slot = &host->slots[i];
      if (!slot->usable) continue;

      igTableNextColumn();
      igPushID_Int(i);

      struct ImTextureRef_c *thumbnail = splash_game_thumbnail(ui->gfx_handler, slot, i);
      float thumbnail_aspect = 9.0f / 16.0f;
      if (i >= 0 && i < (int)(sizeof(g_game_thumbnails) / sizeof(g_game_thumbnails[0]))) {
        const texture_t *texture = g_game_thumbnails[i].texture;
        if (texture && texture->width > 0) thumbnail_aspect = (float)texture->height / (float)texture->width;
      }

      const ImVec2 cursor_pos = igGetCursorScreenPos();
      float actual_card_w = igGetColumnWidth(-1) - 4.0f;
      if (actual_card_w < 110.0f) actual_card_w = card_width;
      const float actual_thumb_h = actual_card_w * thumbnail_aspect;
      const float total_item_h = actual_thumb_h;

      const ImVec2 card_min = cursor_pos;
      const ImVec2 card_max = {cursor_pos.x + actual_card_w, cursor_pos.y + total_item_h};
      ImDrawList *draw_list = igGetWindowDrawList();

      const bool clicked = igInvisibleButton("##game_card", (ImVec2){actual_card_w, total_item_h}, 0);
      const bool hovered = igIsItemHovered(0);

      const ImU32 bg_color = hovered ? IM_COL32(35, 46, 68, 245) : IM_COL32(22, 26, 36, 230);
      ImDrawList_AddRectFilled(draw_list, card_min, card_max, bg_color, 8.0f, ImDrawFlags_None);

      const ImVec2 thumb_min = card_min;
      const ImVec2 thumb_max = {card_min.x + actual_card_w, card_min.y + actual_thumb_h};
      if (thumbnail) {
        ImDrawList_AddImageRounded(draw_list, *thumbnail, thumb_min, thumb_max, (ImVec2){0, 0}, (ImVec2){1, 1}, 0xFFFFFFFF, 8.0f,
                                   ImDrawFlags_RoundCornersAll);
      } else {
        ImDrawList_AddRectFilled(draw_list, thumb_min, thumb_max, IM_COL32(18, 22, 30, 240), 8.0f, ImDrawFlags_RoundCornersAll);
        const char *status_txt = "No Image";
        const ImVec2 txt_sz = igCalcTextSize(status_txt, NULL, false, -1.0f);
        const ImVec2 txt_pos = {thumb_min.x + actual_card_w * 0.5f - txt_sz.x * 0.5f,
                                thumb_min.y + actual_thumb_h * 0.5f - txt_sz.y * 0.5f};
        ImDrawList_AddText_Vec2(draw_list, txt_pos, IM_COL32(140, 150, 170, 255), status_txt, NULL);
      }

      const ImU32 border_color = hovered ? IM_COL32(90, 175, 255, 255) : IM_COL32(48, 56, 75, 140);
      ImDrawList_AddRect(draw_list, card_min, card_max, border_color, 8.0f, ImDrawFlags_None, hovered ? 1.8f : 1.0f);

      if (clicked) {
        if (i == host->active || gfx_activate_game(ui->gfx_handler, i)) {
          snprintf(ui->preferred_game_id, sizeof(ui->preferred_game_id), "%s", slot->id);
          config_save(ui);
          chosen = true;
        }
      }
      igPopID();
    }
    igEndTable();
  }
  igPopStyleVar(1);

  // Anything that failed to load is listed too, so a broken module is visible
  // rather than silently absent.
  bool any_usable = false;
  for (int i = 0; i < host->count; ++i) {
    if (host->slots[i].usable) {
      any_usable = true;
      continue;
    }
    igTextDisabled("%s could not be loaded: %s", host->slots[i].path, host->slots[i].error);
  }
  if (!any_usable) igTextDisabled("No game modules found in games/.");

  return chosen;
}

static void render_splash_variant_selector(game_host_t *host) {
  const ft_game_module *module = host->module;
  if (!module || !module->constraints.variants || module->constraints.variant_count <= 1) return;

  const char *current = game_host_variant(host);
  const ft_game_variant *current_variant = &module->constraints.variants[0];
  for (uint32_t i = 0; i < module->constraints.variant_count; ++i) {
    if (current && strcmp(module->constraints.variants[i].id, current) == 0) {
      current_variant = &module->constraints.variants[i];
      break;
    }
  }

  igAlignTextToFramePadding();
  igTextDisabled("Game mode");
  igSameLine(0.0f, 10.0f);
  igSetNextItemWidth(220.0f);
  if (igBeginCombo("##SplashGameMode", current_variant->display_name, 0)) {
    for (uint32_t i = 0; i < module->constraints.variant_count; ++i) {
      const ft_game_variant *variant = &module->constraints.variants[i];
      if (igSelectable_Bool(variant->display_name, variant == current_variant, 0, (ImVec2){0, 0}))
        game_host_set_variant(host, variant->id);
    }
    igEndCombo();
  }
}

static void render_splash_screen(ui_handler_t *ui) {
  if (!igIsPopupOpen_Str("Splash Screen", ImGuiPopupFlags_None)) {
    igOpenPopup_Str("Splash Screen", ImGuiPopupFlags_None);
  }

  ImVec2 center;
  ImGuiViewport *viewport = igGetMainViewport();
  center.x = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
  center.y = viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
  igSetNextWindowPos(center, ImGuiCond_Always, (ImVec2){0.5f, 0.5f});

  float win_w = viewport->WorkSize.x * 0.88f;
  if (win_w < 920.0f) win_w = 920.0f;
  if (win_w > 1220.0f) win_w = 1220.0f;

  float win_h = viewport->WorkSize.y * 0.86f;
  if (win_h < 660.0f) win_h = 660.0f;
  if (win_h > 780.0f) win_h = 780.0f;

  igSetNextWindowSize((ImVec2){win_w, win_h}, ImGuiCond_Always);

  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;

  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){24.0f, 24.0f});
  igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){14.0f, 14.0f});
  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 12.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.5f);

  if (igBeginPopupModal("Splash Screen", NULL, window_flags)) {
    float sidebar_w = 250.0f;

    // Left sidebar column
    igBeginChild_Str("SplashSidebar", (ImVec2){sidebar_w, 0}, false, ImGuiWindowFlags_NoScrollbar);
    {
      igSpacing();
      igPushFont(ui->font, 48.0f);
      igTextColored((ImVec4){0.35f, 0.75f, 1.00f, 1.00f}, "%s", "FrameTee");
      igPopFont();
      igTextDisabled("A TAS Engine and Editor");

      igSpacing();
      igSeparator();
      igSpacing();

      igPushStyleVar_Vec2(ImGuiStyleVar_ButtonTextAlign, (ImVec2){0.10f, 0.5f});
      igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 6.0f);

      if (ui->splash_stage == SPLASH_STAGE_START && igButton(ICON_FA_ARROW_LEFT "  Games", (ImVec2){170, 42}))
        ui->splash_stage = SPLASH_STAGE_GAME;

      const ft_game_module *level_game = ui->gfx_handler->game_host.module;
      const char *level_ext = level_game ? level_game->constraints.level_extension : NULL;
      if (ui->splash_stage == SPLASH_STAGE_START && level_ext) {
        char level_label[64];
        snprintf(level_label, sizeof(level_label), ICON_FA_MAP "  Load Local %s",
                 level_game->constraints.level_extension ? level_game->constraints.level_extension : "Level");
        if (igButton(level_label, (ImVec2){170, 42})) {
          nfdu8char_t *out_path;
          nfdu8filteritem_t filters[] = {{level_game->constraints.level_filter_name, level_ext}};
          nfdopendialogu8args_t args = {0};
          args.filterList = filters;
          args.filterCount = 1;
          nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
          if (result == NFD_OKAY) {
            on_level_load_path(ui->gfx_handler, out_path);
            NFD_FreePathU8(out_path);
            splash_on_level_picked(ui);
            igCloseCurrentPopup();
          }
        }
      }

      if (igButton(ICON_FA_FOLDER_OPEN "  Load Project", (ImVec2){170, 42})) {
        nfdu8char_t *out_path;
        nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
        nfdopendialogu8args_t args = {0};
        args.filterList = filters;
        args.filterCount = 1;
        nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
        if (result == NFD_OKAY) {
          load_project(ui, out_path);
          NFD_FreePathU8(out_path);
          ui->show_splash = false;
          igCloseCurrentPopup();
        }
      }

      igPopStyleVar(2);

      if (ui->num_recent_projects > 0) {
        igSpacing();
        igSeparator();
        igSpacing();

        igTextColored((ImVec4){0.70f, 0.75f, 0.85f, 1.00f}, "%s", ICON_FA_CLOCK " Recent Projects");
        igSpacing();

        igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, 1.0f);
        igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 5.0f);
        igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){0.12f, 0.14f, 0.18f, 0.60f});
        igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){0.22f, 0.28f, 0.38f, 0.85f});
        igPushStyleVar_Vec2(ImGuiStyleVar_ButtonTextAlign, (ImVec2){0.05f, 0.5f});

        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0.0f, 0.0f});
        igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){0.0f, 6.0f});
        if (igBeginChild_Str("RecentProjectsList", (ImVec2){0.0f, 0.0f}, false, ImGuiWindowFlags_None)) {
          for (int i = 0; i < ui->num_recent_projects; i++) {
            const char *path = ui->recent_projects[i];
            const char *filename = strrchr(path, '/');
            if (!filename) filename = strrchr(path, '\\');
            if (!filename) filename = path;
            else filename++;

            char item_lbl[1050];
            snprintf(item_lbl, sizeof(item_lbl), "%s  %s", ICON_FA_FILE, filename);

            if (igButton(item_lbl, (ImVec2){0.0f, 32.0f})) {
              load_project(ui, path);
              ui->show_splash = false;
              igCloseCurrentPopup();
            }
            if (igIsItemHovered(ImGuiHoveredFlags_None)) {
              igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8.0f, 6.0f});
              if (igBeginTooltip()) {
                igPushTextWrapPos(380.0f);
                igTextUnformatted(path, NULL);
                igPopTextWrapPos();
                igEndTooltip();
              }
              igPopStyleVar(1);
            }
          }
        }
        igEndChild();
        igPopStyleVar(2);

        igPopStyleVar(3);
        igPopStyleColor(2);
      }
    }
    igEndChild();

    igSameLine(0, 18.0f);

    // Right main panel: the game picker first, then whatever the chosen game
    // wants a run to start from.
    igBeginChild_Str("SplashMainPanel", (ImVec2){0, 0}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImVec2 avail = igGetContentRegionAvail();
      if (ui->splash_stage == SPLASH_STAGE_GAME) {
        if (render_splash_game_picker(ui, avail.x)) ui->splash_stage = SPLASH_STAGE_START;
      } else {
        render_splash_variant_selector(&ui->gfx_handler->game_host);

        ft_ui_frame frame = {0};
        frame.struct_size = sizeof(frame);
        frame.slot = FT_UI_SPLASH;
        frame.tick = ui->timeline.current_tick;
        frame.player = ui->timeline.selected_player_track_index;
        engine_api_fill_state(&frame.state);
        gh_ui(&ui->gfx_handler->game_host, &frame);

        // A game with no start screen of its own still gets a usable one: the
        // sidebar can open a level or a project.
        if (!game_provides_splash(&ui->gfx_handler->game_host))
          igTextDisabled("%s has no start screen. Open a level or a project from the left.",
                         ui->plugin_context.active_game_id);
      }
    }
    igEndChild();

    // clicking the backdrop puts you back on the current project.
    if (ui->gfx_handler->level != NULL && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsAnyItemActive() &&
        !igIsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      ui->show_splash = false;
      igCloseCurrentPopup();
    }

    igEndPopup();
  }

  igPopStyleVar(4);
}

void ui_render(ui_handler_t *ui) {
  interaction_update_recording_input(ui);
  render_menu_bar(ui);

  keybinds_process_inputs(ui);
  interaction_handle_playback_and_shortcuts(&ui->timeline);
  setup_docking(ui);
  plugin_manager_update_all(&ui->plugin_manager);

  if (ui->show_fps) {
    if (igBeginMainMenuBar()) {
      ImVec2 region_avail = igGetContentRegionAvail();
      ImGuiIO *io = igGetIO_Nil();
      char fps_text[64];
      snprintf(fps_text, sizeof(fps_text), "FPS: %.1f (%.2f ms)", io->Framerate, 1000.0f / io->Framerate);
      ImVec2 fps_size = igCalcTextSize(fps_text, NULL, false, 0.0f);
      igSetCursorPosX(igGetCursorPosX() + region_avail.x - fps_size.x);
      igText("%s", fps_text);
      igEndMainMenuBar();
    }
  }

  if (ui->show_timeline) {
    if (!ui->timeline.ui) ui->timeline.ui = ui;
    render_timeline(ui);
    render_player_manager(ui);
    render_snippet_editor_panel(ui);
  }

  keybinds_render_settings_window(ui);
  undo_manager_render_history_window(&ui->undo_manager);
  render_timeline_events_window(ui);
  if (ui->show_plugin_manager) {
    plugin_manager_render_ui(&ui->plugin_manager, &ui->show_plugin_manager);
  }
  entity_inspector_render(&ui->entity_inspector);
  // Game-owned panels may reference game-owned GPU resources. Keep them out
  // of the frame in which the splash can replace the active game.
  if (ui->gfx_handler->level != NULL && !ui->show_splash) {
    render_game_ui_slot(ui, FT_UI_PANELS, ui->timeline.selected_player_track_index);
    // Only for a game that does not place the editor in a panel of its own.
    starting_state_render_window(ui);
  }

  render_new_project_prompt(ui);

  // with nothing loaded the splash is the only thing to show, otherwise it is up because
  // "New Project" raised it and the user can still dismiss it
  if (ui->gfx_handler->level == NULL || ui->show_splash) {
    render_splash_screen(ui);
  }
}

#define WORD_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c"
#define WORD_TO_BINARY(word)                                                                                                      \
  ((word) & 0x8000 ? '1' : '0'), ((word) & 0x4000 ? '1' : '0'), ((word) & 0x2000 ? '1' : '0'), ((word) & 0x1000 ? '1' : '0'),     \
      ((word) & 0x0800 ? '1' : '0'), ((word) & 0x0400 ? '1' : '0'), ((word) & 0x0200 ? '1' : '0'), ((word) & 0x0100 ? '1' : '0'), \
      ((word) & 0x0080 ? '1' : '0'), ((word) & 0x0040 ? '1' : '0'), ((word) & 0x0020 ? '1' : '0'), ((word) & 0x0010 ? '1' : '0'), \
      ((word) & 0x0008 ? '1' : '0'), ((word) & 0x0004 ? '1' : '0'), ((word) & 0x0002 ? '1' : '0'), ((word) & 0x0001 ? '1' : '0')

// draw the recording overlay text in the top-right corner
static void draw_recording_overlay(ImVec2 start) {
  const char *text = "Recording... (ESC to Stop, F4 to Discard)";
  ImVec2 text_size = igCalcTextSize(text, NULL, false, 0.0f);
  ImVec2 avail = igGetContentRegionAvail();

  ImVec2 text_pos = {start.x + avail.x - text_size.x - 20.0f, start.y};
  ImDrawList_AddText_Vec2(igGetWindowDrawList(), text_pos, IM_COL32(255, 50, 50, 255), text, NULL);
}

// draw the telemetry text (character stats and input state)
static void draw_character_inspector(ui_handler_t *ui, ImVec2 start) {
  igPushFont(ui->font, 25.f * gfx_get_ui_scale());

  // set the absolute y position, but let imgui handle x boundaries
  igSetCursorScreenPos((ImVec2){start.x, start.y});

  // push a 10px layout margin for the left side
  igIndent(10.0f);

  // The readout is the game's: it formats the lines, the editor draws them.
  {
    game_host_t *host = &ui->gfx_handler->game_host;
    const ft_world *world = model_world_at_tick(&ui->timeline, ui->timeline.current_tick);
    const int local_index = model_group_local_track_index(&ui->timeline, ui->timeline.selected_player_track_index);

    float speed_scale = ui->timeline.is_reversing ? 2.0f : 1.0f;
    float alpha = fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / (ui->timeline.playback_speed * speed_scale)), 1.f);
    if (ui->timeline.is_reversing) alpha = 1.f - alpha;

    enum { MAX_STATUS_LINES = 24,
           STATUS_LINE_SIZE = 128 };
    static char lines[MAX_STATUS_LINES][STATUS_LINE_SIZE];
    const unsigned count = gh_status_lines(host, world, local_index, alpha, &lines[0][0], MAX_STATUS_LINES, STATUS_LINE_SIZE);
    for (unsigned i = 0; i < count; ++i)
      igText("%s", lines[i]);
  }

  // input state
  input_record_t Input = ui->timeline.player_tracks[ui->timeline.selected_player_track_index].current_input;
  if (!ui->timeline.recording) {
    int group_index = model_track_group_index(&ui->timeline, ui->timeline.selected_player_track_index);
    Input = model_get_input_at_tick(&ui->timeline, ui->timeline.selected_player_track_index,
                                    model_group_playhead_tick(&ui->timeline, group_index));
  }

  igText("");
  igText("Input:");
  {
    // One line per field the game declared, which is also exactly what the
    // snippet editor edits and what gets written to the project.
    game_host_t *host = &ui->gfx_handler->game_host;
    const ft_input_schema *schema = game_input_schema(host);
    for (uint32_t i = 0; schema && i < schema->field_count; ++i) {
      const ft_input_field *field = &schema->fields[i];
      if (field->flags & FT_INPUT_FLAG_INTERNAL) continue;
      const char *label = field->display_name ? field->display_name : field->id;

      if (field->kind == FT_INPUT_VEC2) {
        const ft_vec2 value = engine_input_get_vec2(host, &Input, (int)i);
        igText("%s: %.0f, %.0f", label, value.x, value.y);
      } else if (field->kind == FT_INPUT_FLOAT) {
        igText("%s: %.3f", label, engine_input_get_float(host, &Input, (int)i));
      } else if (field->kind == FT_INPUT_ENUM && field->enum_labels) {
        const long long value = engine_input_get(host, &Input, (int)i);
        const long long index = value - field->min_value;
        igText("%s: %s", label, (index >= 0 && index < field->enum_count) ? field->enum_labels[index] : "?");
      } else {
        igText("%s: %lld", label, engine_input_get(host, &Input, (int)i));
      }
    }
  }

  // restore layout defaults
  igUnindent(10.0f);
  igPopFont();
}

bool ui_render_late(ui_handler_t *ui) {
  bool hovered = false;

  if (!ui->gfx_handler->offscreen_initialized || ui->gfx_handler->offscreen_texture == NULL)
    return false;

  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0, 0});
  igBegin("Viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  // render the main game viewport texture
  ImVec2 start = igGetCursorScreenPos();
  const ImVec2 viewport_content_pos = start;
  *(ImVec2_c *)&ui->gfx_handler->viewport[0] = igGetContentRegionAvail();
  // A capture is only worth comparing against another one taken the same way,
  // and the window manager is free to ignore the size the window asked for --
  // it may tile it, or half it. Pinning the drawn region here fixes both the
  // aspect the camera projects with and the pixels --screenshot reads back,
  // whatever the window ended up being.
  {
    const char *forced = getenv("FRAMETEE_VIEWPORT_SIZE");
    if (forced != NULL) {
      int w = 0, h = 0;
      if (sscanf(forced, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        ui->gfx_handler->viewport[0] = (float)w;
        ui->gfx_handler->viewport[1] = (float)h;
      }
    }
  }

  ImVec2 img_size = {ui->gfx_handler->viewport[0], ui->gfx_handler->viewport[1]};
  ImVec2 uv0 = {0, 0};
  ImVec2 uv1 = {1.0f, 1.0f};
  if (ui->gfx_handler->offscreen_width > 0 && ui->gfx_handler->offscreen_height > 0) {
    uv1.x = (float)ui->gfx_handler->viewport[0] / ui->gfx_handler->offscreen_width;
    uv1.y = (float)ui->gfx_handler->viewport[1] / ui->gfx_handler->offscreen_height;
  }
  igImage(*ui->gfx_handler->offscreen_texture, img_size, uv0, uv1);
  igPopStyleVar(1);

  *(ImVec2_c *)&ui->viewport_window_pos = igGetWindowPos();
  ui->viewport_hovered = igIsWindowHovered(0);
  ui->viewport_focused = igIsWindowFocused(0);
  hovered = ui->viewport_hovered;

  start.x += 10.0f;
  start.y += 10.0f;

  // handle raycast/click interaction
  if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
    ImGuiIO *io = igGetIO_Nil();
    float mx = io->MousePos.x - viewport_content_pos.x;
    float my = io->MousePos.y - viewport_content_pos.y;
    float wx, wy;
    screen_to_world(ui->gfx_handler, mx, my, &wx, &wy);

    const ft_world *world = model_world_at_tick(&ui->timeline, ui->timeline.current_tick);
    game_host_t *host = &ui->gfx_handler->game_host;

    float speed_scale = ui->timeline.is_reversing ? 2.0f : 1.0f;
    float intra = fminf((igGetTime() - ui->timeline.last_update_time) / (1.f / (ui->timeline.playback_speed * speed_scale)), 1.f);
    if (ui->timeline.is_reversing) intra = 1.f - intra;

    const bool selected_entity =
        entity_inspector_pick(&ui->entity_inspector, world, ui->gfx_handler, intra, mx, my);

    if (!selected_entity) {
      int best_match = -1;
      float best_dist = 1.5f;

      // Picking a player only needs its position, which every game reports the
      // same way. Every visible group is drawn into the same viewport, so all
      // of them are searched.
      for (int group_index = 0; group_index < ui->timeline.group_count; ++group_index) {
        if (!ui->timeline.groups[group_index]->visible) continue;
        const ft_world *group_world = model_group_world_at_tick(&ui->timeline, group_index, ui->timeline.current_tick);
        if (!group_world) continue;

        const int players = gh_world_player_count(host, group_world);
        for (int local_index = 0; local_index < players; ++local_index) {
          ft_player_view view;
          if (!gh_world_player_view(host, group_world, local_index, &view)) continue;
          const float dx = view.position.x - wx;
          const float dy = view.position.y - wy;
          const float dist = sqrtf(dx * dx + dy * dy);
          if (dist < best_dist) {
            best_dist = dist;
            best_match = model_group_track_index(&ui->timeline, group_index, local_index);
          }
        }
      }

      if (best_match != -1) {
        interaction_select_track(&ui->timeline, best_match);
        interaction_select_track(&ui->timeline, -1);
      }
    }
  }

  // draw overlays & menus
  if (ui->timeline.recording) {
    draw_recording_overlay(start);
  }

  if ((hovered || ui->timeline.recording) && input_key_pressed(GLFW_KEY_TAB, false)) {
    ui->show_timeline = !ui->show_timeline;
  }

  if (ui->timeline.selected_player_track_index >= 0) {
    draw_character_inspector(ui, start);
  }

  igEnd();
  return hovered;
}

void ui_post_level_load(ui_handler_t *ui) {
  // Anything derived from a level is derived by the game that loaded it; the
  // editor only resets what it owns.
  entity_inspector_clear(&ui->entity_inspector);
  ui->timeline.current_tick = 0;
  ui->timeline.event_count = 0;
}

void ui_cleanup(ui_handler_t *ui) {
  config_save(ui);
  plugin_manager_shutdown(&ui->plugin_manager);
  snippet_editor_cleanup();
  undo_manager_cleanup(&ui->undo_manager);
  timeline_cleanup(&ui->timeline);
  keybinds_cleanup(&ui->keybinds);
  extern bool g_is_headless;
  if (!g_is_headless) {
    NFD_Quit();
  }
}

bool ui_icon_button(ui_handler_t *ui, const char *icon, ImVec2 size) {
  ImGuiWindow *window = igGetCurrentWindow();
  if (window->SkipItems) return false;

  ImGuiContext *g = igGetCurrentContext();
  const ImGuiStyle *style = &g->Style;
  const ImGuiID id = igGetID_Str(icon);

  float dpi = gfx_get_ui_scale();
  ImVec2 pos = window->DC.CursorPos;

  if (size.x <= 0.0f) size.x = 30.0f * dpi;
  if (size.y <= 0.0f) size.y = igGetFrameHeight();

  const ImRect bb = {pos, {pos.x + size.x, pos.y + size.y}};
  igItemSize_Vec2(size, style->FramePadding.y);
  if (!igItemAdd(bb, id, NULL, 0)) return false;

  bool hovered, held;
  bool pressed = igButtonBehavior(bb, id, &hovered, &held, 0);

  // Render button background & border
  ImGuiCol col_idx = held ? ImGuiCol_ButtonActive : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
  ImU32 col = igGetColorU32_Col(col_idx, 1.0f);
  igRenderFrame(bb.Min, bb.Max, col, true, style->FrameRounding);

  ImFont *font = (ui && ui->icon_font) ? ui->icon_font : igGetFont();

  if (ui && ui->icon_font) {
    igPushFont(ui->icon_font, 0.0f);
  }

  float font_size = igGetFontSize();
  float max_font_size = size.y * 0.55f;
  if (font_size > max_font_size) {
    font_size = max_font_size;
  }

  ImVec2 text_size = {0};
  if (font) {
    text_size = ImFont_CalcTextSizeA(font, font_size, 3.402823466e+38F, -1.0f, icon, NULL, NULL);
  } else {
    text_size = igCalcTextSize(icon, NULL, false, -1.0f);
  }

  if (ui && ui->icon_font) {
    igPopFont();
  }

  // Calculate top-left for AddText so icon visual center matches button frame center
  ImVec2 center = {(bb.Min.x + bb.Max.x) * 0.5f, (bb.Min.y + bb.Max.y) * 0.5f};
  ImVec2 text_pos = {
      roundf(center.x - text_size.x * 0.5f),
      roundf(center.y - text_size.y * 0.5f)};

  ImDrawList_AddText_FontPtr(
      igGetWindowDrawList(),
      font,
      font_size,
      text_pos,
      igGetColorU32_Col(ImGuiCol_Text, 1.0f),
      icon,
      NULL,
      0.0f,
      NULL);

  return pressed;
}

bool ui_quick_save(ui_handler_t *ui) {
  if (ui->current_project_path[0] != '\0') {
    return save_project(ui, ui->current_project_path);
  } else {
    nfdu8char_t *save_path = NULL;
    nfdu8filteritem_t filters[] = {{"TAS Project", "tasp"}};
    char default_file_name[256];
    project_default_file_name(ui, default_file_name, sizeof(default_file_name));
    nfdresult_t result = NFD_SaveDialogU8(&save_path, filters, 1, NULL, default_file_name);
    if (result == NFD_OKAY) {
      bool ok = save_project(ui, save_path);
      NFD_FreePathU8(save_path);
      return ok;
    }
    return false;
  }
}

void ui_request_new_project(ui_handler_t *ui) {
  // Starting over is the other moment nothing is open, so the game may change.
  ui->splash_stage = SPLASH_STAGE_GAME;
  if (ui->has_unsaved_changes) {
    ui->show_new_project_prompt = true;
    return;
  }
  ui->show_splash = true;
}

void ui_mark_unsaved(ui_handler_t *ui) {
  if (ui) {
    ui->has_unsaved_changes = true;
  }
}

void timeline_mark_unsaved(timeline_state_t *ts) {
  if (ts && ts->ui) {
    ts->ui->has_unsaved_changes = true;
  }
}

void ui_check_auto_save(ui_handler_t *ui) {
  if (!ui->auto_save_enabled) return;
  if (ui->current_project_path[0] == '\0') return;
  if (!ui->has_unsaved_changes) return;

  double now = glfwGetTime();
  if (ui->last_auto_save_time <= 0.0) {
    ui->last_auto_save_time = now;
    return;
  }

  if (now - ui->last_auto_save_time >= (double)ui->auto_save_interval_sec) {
    log_info(LOG_SOURCE, "Auto-saving project to '%s'...", ui->current_project_path);
    save_project(ui, ui->current_project_path);
    ui->last_auto_save_time = now;
  }
}

// Draws a control for every setting the active game publishes, grouped by the
// headings it gave them, and saves on change.
void ui_render_game_settings(ui_handler_t *ui) {
  game_host_t *host = &ui->gfx_handler->game_host;
  const unsigned count = gh_setting_count(host);
  if (count == 0) return;

  igSeparator();
  const char *current_group = NULL;
  for (unsigned i = 0; i < count; ++i) {
    const ft_setting_desc *desc = gh_setting_desc(host, i);
    ft_value value;
    if (!desc || !gh_setting_get(host, i, &value)) continue;

    if (desc->group && (!current_group || strcmp(current_group, desc->group) != 0)) {
      current_group = desc->group;
      igTextDisabled("%s", desc->group);
    }

    const char *label = desc->display_name ? desc->display_name : desc->id;
    bool changed = false;
    switch (value.kind) {
    case FT_VALUE_BOOL:
      changed = igCheckbox(label, &value.as.b);
      break;
    case FT_VALUE_INT: {
      int v = (int)value.as.i;
      changed = igDragInt(label, &v, 1.f, (int)desc->min_value, (int)desc->max_value, "%d", 0);
      value.as.i = v;
      break;
    }
    case FT_VALUE_FLOAT: {
      float v = (float)value.as.f;
      changed = igDragFloat(label, &v, 0.02f, (float)desc->min_value, (float)desc->max_value, "%.2f", 0);
      value.as.f = v;
      break;
    }
    default:
      break;
    }

    if (desc->description && igIsItemHovered(0)) igSetTooltip("%s", desc->description);
    if (changed) {
      gh_setting_set(host, i, &value);
      config_save(ui);
    }
  }
}
