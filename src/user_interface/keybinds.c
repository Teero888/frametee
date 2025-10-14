#include "keybinds.h"
#include "cimgui.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <limits.h>
#include <string.h>

// check if a key combination is pressed for single-press actions
bool is_key_combo_pressed(const key_combo_t *combo, bool repeat) {
  if (combo->key == ImGuiKey_None) return false;

  ImGuiIO *io = igGetIO_Nil();
  if (combo->ctrl != io->KeyCtrl || combo->alt != io->KeyAlt || combo->shift != io->KeyShift) {
    return false;
  }

  return igIsKeyPressed_Bool(combo->key, repeat);
}

// check if a key combination is held down
bool is_key_combo_down(const key_combo_t *combo) {
  if (combo->key == ImGuiKey_None) return false;

  ImGuiIO *io = igGetIO_Nil();
  if (combo->ctrl != io->KeyCtrl || combo->alt != io->KeyAlt || combo->shift != io->KeyShift) {
    return false;
  }

  return igIsKeyDown_Nil(combo->key);
}

// This buffer is used by keybind_get_combo_string to avoid repeated allocations.
static char combo_string_buffer[128];
const char *keybind_get_combo_string(const key_combo_t *combo) {
  if (combo->key == ImGuiKey_None) {
    return "Not Bound";
  }

  combo_string_buffer[0] = '\0';
  if (combo->ctrl) strcat(combo_string_buffer, "Ctrl+");
  if (combo->alt) strcat(combo_string_buffer, "Alt+");
  if (combo->shift) strcat(combo_string_buffer, "Shift+");

  const char *key_name = igGetKeyName(combo->key);
  if (key_name) {
    strcat(combo_string_buffer, key_name);
  } else {
    strcat(combo_string_buffer, "Unknown");
  }
  return combo_string_buffer;
}

void keybinds_init(keybind_manager_t *manager) {
  memset(manager, 0, sizeof(keybind_manager_t));
  manager->show_settings_window = false;

  // Playback
  manager->bindings[ACTION_PLAY_PAUSE] = (keybind_t){"Play/Pause", "Playback", {ImGuiKey_X, false, false, false}};
  manager->bindings[ACTION_REWIND_HOLD] = (keybind_t){"Rewind (Hold)", "Playback", {ImGuiKey_C, false, false, false}};
  manager->bindings[ACTION_PREV_FRAME] = (keybind_t){"Previous Frame", "Playback", {ImGuiKey_MouseX1, false, false, false}};
  manager->bindings[ACTION_NEXT_FRAME] = (keybind_t){"Next Frame", "Playback", {ImGuiKey_MouseX2, false, false, false}};
  manager->bindings[ACTION_INC_TPS] = (keybind_t){"Increase TPS", "Playback", {ImGuiKey_UpArrow, false, false, false}};
  manager->bindings[ACTION_DEC_TPS] = (keybind_t){"Next Frame", "Playback", {ImGuiKey_DownArrow, false, false, false}};

  // Timeline Editing
  manager->bindings[ACTION_SELECT_ALL] = (keybind_t){"Select all Snippets", "Timeline", {ImGuiKey_A, true, false, false}};
  manager->bindings[ACTION_DELETE_SNIPPET] = (keybind_t){"Delete Snippet", "Timeline", {ImGuiKey_Delete, false, false, false}};
  manager->bindings[ACTION_SPLIT_SNIPPET] = (keybind_t){"Split Snippet", "Timeline", {ImGuiKey_R, true, false, false}};
  manager->bindings[ACTION_MERGE_SNIPPETS] = (keybind_t){"Merge Snippets", "Timeline", {ImGuiKey_M, true, false, false}};
  manager->bindings[ACTION_TOGGLE_SNIPPET_ACTIVE] = (keybind_t){"Toggle Snippet Active", "Timeline", {ImGuiKey_A, false, false, false}};

  // General
  manager->bindings[ACTION_UNDO] = (keybind_t){"Undo", "General", {ImGuiKey_Z, true, false, false}};
  manager->bindings[ACTION_REDO] = (keybind_t){"Redo", "General", {ImGuiKey_Y, true, false, false}};

  // Recording
  manager->bindings[ACTION_TRIM_SNIPPET] = (keybind_t){"Trim Recording", "Recording", {ImGuiKey_F, false, false, false}};
  manager->bindings[ACTION_LEFT] = (keybind_t){"Move Left", "Recording", {ImGuiKey_A, false, false, false}};
  manager->bindings[ACTION_RIGHT] = (keybind_t){"Move Right", "Recording", {ImGuiKey_D, false, false, false}};
  manager->bindings[ACTION_JUMP] = (keybind_t){"Jump", "Recording", {ImGuiKey_Space, false, false, false}};
  manager->bindings[ACTION_KILL] = (keybind_t){"Kill", "Recording", {ImGuiKey_K, false, false, false}};
  manager->bindings[ACTION_FIRE] = (keybind_t){"Fire weapon", "Recording", {ImGuiKey_MouseLeft, false, false, false}};
  manager->bindings[ACTION_HOOK] = (keybind_t){"Hook", "Recording", {ImGuiKey_MouseRight, false, false, false}};
  manager->bindings[ACTION_HAMMER] = (keybind_t){"Switch to hammer", "Recording", {ImGuiKey_1, false, false, false}};
  manager->bindings[ACTION_GUN] = (keybind_t){"Switch to gun", "Recording", {ImGuiKey_2, false, false, false}};
  manager->bindings[ACTION_SHOTGUN] = (keybind_t){"Switch to shotgun", "Recording", {ImGuiKey_3, false, false, false}};
  manager->bindings[ACTION_GRENADE] = (keybind_t){"Switch to grenade", "Recording", {ImGuiKey_4, false, false, false}};
  manager->bindings[ACTION_LASER] = (keybind_t){"Switch to laser", "Recording", {ImGuiKey_5, false, false, false}};

  // Dummy
  manager->bindings[ACTION_DUMMY_FIRE] = (keybind_t){"Dummy Fire", "Dummy", {ImGuiKey_MouseLeft, false, false, false}};
  manager->bindings[ACTION_TOGGLE_DUMMY_COPY] = (keybind_t){"Toggle dummy copy", "Dummy", {ImGuiKey_R, false, false, false}};

  // Camera
  manager->bindings[ACTION_ZOOM_IN] = (keybind_t){"Zoom in", "Camera", {ImGuiKey_W, false, false, false}};
  manager->bindings[ACTION_ZOOM_OUT] = (keybind_t){"Zoom out", "Camera", {ImGuiKey_S, false, false, false}};

  // Track Switching
  manager->bindings[ACTION_SWITCH_TRACK_1] = (keybind_t){"Switch to Track 1", "Tracks", {ImGuiKey_1, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_2] = (keybind_t){"Switch to Track 2", "Tracks", {ImGuiKey_2, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_3] = (keybind_t){"Switch to Track 3", "Tracks", {ImGuiKey_3, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_4] = (keybind_t){"Switch to Track 4", "Tracks", {ImGuiKey_4, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_5] = (keybind_t){"Switch to Track 5", "Tracks", {ImGuiKey_5, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_6] = (keybind_t){"Switch to Track 6", "Tracks", {ImGuiKey_6, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_7] = (keybind_t){"Switch to Track 7", "Tracks", {ImGuiKey_7, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_8] = (keybind_t){"Switch to Track 8", "Tracks", {ImGuiKey_8, false, true, false}};
  manager->bindings[ACTION_SWITCH_TRACK_9] = (keybind_t){"Switch to Track 9", "Tracks", {ImGuiKey_9, false, true, false}};
}

void keybinds_process_inputs(ui_handler_t *ui) {
  if (igIsAnyItemActive()) return;

  timeline_state_t *ts = &ui->timeline;
  keybind_manager_t *kb = &ui->keybinds;
  undo_command_t *cmd = NULL;

  if (is_key_combo_pressed(&kb->bindings[ACTION_PLAY_PAUSE].combo, false)) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
    }
  }

  if (is_key_combo_pressed(&kb->bindings[ACTION_SELECT_ALL].combo, false)) {
    interaction_clear_selection(ts);
    ts->active_snippet_id = -1;
    for (int i = 0; i < ts->player_track_count; i++) {
      for (int j = 0; j < ts->player_tracks[i].snippet_count; j++) {
        interaction_add_snippet_to_selection(ts, ts->player_tracks[i].snippets[j].id, i);
      }
    }
  }
  if (is_key_combo_pressed(&kb->bindings[ACTION_DELETE_SNIPPET].combo, false)) cmd = commands_create_delete_selected(ui);
  if (is_key_combo_pressed(&kb->bindings[ACTION_SPLIT_SNIPPET].combo, false)) cmd = commands_create_split_selected(ui);
  if (is_key_combo_pressed(&kb->bindings[ACTION_MERGE_SNIPPETS].combo, false)) cmd = commands_create_merge_selected(ui);

  if (is_key_combo_pressed(&kb->bindings[ACTION_TOGGLE_SNIPPET_ACTIVE].combo, false)) {
    if (ts->selected_snippets.count > 0) {
      int earliest_tick_to_recalc = INT_MAX;
      bool changed = false;

      for (int i = 0; i < ts->selected_snippets.count; ++i) {
        int snippet_id = ts->selected_snippets.ids[i];
        int track_idx;
        input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, &track_idx);

        if (snippet) {
          if (snippet->start_tick < earliest_tick_to_recalc) {
            earliest_tick_to_recalc = snippet->start_tick;
          }
          changed = true;

          if (snippet->is_active) {
            snippet->is_active = false;
          } else {
            // This function correctly deactivates conflicting snippets on the same track.
            model_activate_snippet(ts, track_idx, snippet_id);
          }
        }
      }

      if (changed && earliest_tick_to_recalc != INT_MAX) {
        model_recalc_physics(ts, earliest_tick_to_recalc);
      }
    }
  }

  if (is_key_combo_pressed(&kb->bindings[ACTION_UNDO].combo, false)) undo_manager_undo(&ui->undo_manager, ts);
  if (is_key_combo_pressed(&kb->bindings[ACTION_REDO].combo, false)) undo_manager_redo(&ui->undo_manager, ts);

  for (int i = 0; i < 9; i++) {
    if (is_key_combo_pressed(&kb->bindings[ACTION_SWITCH_TRACK_1 + i].combo, false)) {
      int new_index = imin(i, ts->player_track_count - 1);
      if (ts->recording && ts->selected_player_track_index != new_index) {
        timeline_switch_recording_target(ts, new_index);
      }
      ts->selected_player_track_index = new_index;
      break;
    }
  }

  if (cmd) {
    undo_manager_register_command(&ui->undo_manager, cmd);
  }

  // actions that can be held down (repeating)
  if (is_key_combo_pressed(&kb->bindings[ACTION_PREV_FRAME].combo, true)) {
    ts->is_playing = false;
    model_advance_tick(ts, -1);
  }
  if (is_key_combo_pressed(&kb->bindings[ACTION_NEXT_FRAME].combo, true)) {
    ts->is_playing = false;
    model_advance_tick(ts, 1);
  }
  if (is_key_combo_pressed(&kb->bindings[ACTION_INC_TPS].combo, true)) {
    ++ts->gui_playback_speed;
  }
  if (is_key_combo_pressed(&kb->bindings[ACTION_DEC_TPS].combo, true)) {
    --ts->gui_playback_speed;
  }
  if (is_key_combo_pressed(&kb->bindings[ACTION_TOGGLE_DUMMY_COPY].combo, false)) ts->dummy_copy_input ^= 1;
}

static bool is_modifier_key(ImGuiKey key) {
  return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_ReservedForModCtrl ||
         key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModSuper;
}

static void render_keybind_button(keybind_manager_t *manager, action_t action_id) {
  keybind_t *binding = &manager->bindings[action_id];
  igPushID_Int(action_id);

  const char *button_label;
  if (manager->is_waiting_for_input && manager->action_to_rebind == action_id) {
    button_label = "[ waiting ]";
  } else {
    button_label = keybind_get_combo_string(&binding->combo);
  }

  if (igButton(button_label, (ImVec2){160.0f, 0})) {
    manager->is_waiting_for_input = true;
    manager->action_to_rebind = action_id;
  }

  igSameLine(0, 6.0f);
  if (igButton("Clear", (ImVec2){0, 0})) {
    binding->combo = (key_combo_t){ImGuiKey_None, false, false, false};
  }

  igPopID();
}

void keybinds_render_settings_window(keybind_manager_t *manager) {
  if (!manager->show_settings_window) return;

  igSetNextWindowSize((ImVec2){600, 500}, ImGuiCond_FirstUseEver);
  if (igBegin("Keybind Settings", &manager->show_settings_window, 0)) {

    igSetNextWindowPos((ImVec2){igGetIO_Nil()->DisplaySize.x * 0.5f, igGetIO_Nil()->DisplaySize.y * 0.5f}, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f});
    if (manager->is_waiting_for_input) igOpenPopup_Str("RebindKeyPopup", ImGuiPopupFlags_AnyPopupLevel);

    if (igBeginPopupModal("RebindKeyPopup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
      igText("Press any key combination to bind to '%s'", manager->bindings[manager->action_to_rebind].name);
      igSeparator();
      igText("Press ESC to cancel.");

      ImGuiIO *io = igGetIO_Nil();
      if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        manager->is_waiting_for_input = false;
        igCloseCurrentPopup();
      } else {
        for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
          if (key == ImGuiKey_Escape || is_modifier_key(key)) continue;
          if (igIsKeyPressed_Bool(key, false)) {
            keybind_t *binding = &manager->bindings[manager->action_to_rebind];
            binding->combo.key = key;
            binding->combo.ctrl = io->KeyCtrl;
            binding->combo.alt = io->KeyAlt;
            binding->combo.shift = io->KeyShift;
            manager->is_waiting_for_input = false;
            igCloseCurrentPopup();
            break;
          }
        }
      }
      igEndPopup();
    }

    igText("Click a keybind to change it, or click 'Clear' to unbind it.");
    igSeparator();

    const char *categories[] = {"Playback", "Timeline", "General", "Recording", "Dummy", "Camera", "Tracks"};
    int num_categories = sizeof(categories) / sizeof(categories[0]);

    for (int cat_idx = 0; cat_idx < num_categories; ++cat_idx) {
      const char *current_category = categories[cat_idx];

      ImGuiTreeNodeFlags flags = (strcmp(current_category, "Tracks") == 0) ? 0 : ImGuiTreeNodeFlags_DefaultOpen;

      if (igCollapsingHeader_TreeNodeFlags(current_category, flags)) {
        if (igBeginTable("KeybindsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg, (ImVec2){0, 0}, 0)) {
          igTableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
          igTableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 240.0f, 0);

          for (int i = 0; i < ACTION_COUNT; i++) {
            keybind_t *binding = &manager->bindings[i];
            if (strcmp(binding->category, current_category) == 0) {
              igTableNextRow(0, 0);
              igTableSetColumnIndex(0);

              float frame_height = igGetFrameHeight();
              float text_height = igGetTextLineHeight();
              float vertical_offset = (frame_height - text_height) * 0.5f;
              igSetCursorPosY(igGetCursorPosY() + vertical_offset);
              igSetCursorPosX(igGetCursorPosX() + 5.0f);
              igTextUnformatted(binding->name, NULL);

              igTableSetColumnIndex(1);
              render_keybind_button(manager, (action_t)i);
            }
          }
          igEndTable();
        }
      }
    }
  }
  igEnd();
}
