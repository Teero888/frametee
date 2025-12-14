#include "keybinds.h"
#include <renderer/graphics_backend.h>
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <symbols.h>
#include <logger/logger.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

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

// Helper Functions

void keybinds_add(keybind_manager_t *kb, action_t action, key_combo_t combo) {
  if (kb->bind_count >= kb->bind_capacity) {
    kb->bind_capacity = kb->bind_capacity == 0 ? 16 : kb->bind_capacity * 2;
    kb->bindings = realloc(kb->bindings, sizeof(keybind_entry_t) * kb->bind_capacity);
  }
  kb->bindings[kb->bind_count].action_id = action;
  kb->bindings[kb->bind_count].combo = combo;
  kb->bind_count++;
}

void keybinds_remove(keybind_manager_t *kb, int index) {
  if (index < 0 || index >= kb->bind_count) return;
  if (index < kb->bind_count - 1) {
    memmove(&kb->bindings[index], &kb->bindings[index + 1], (kb->bind_count - index - 1) * sizeof(keybind_entry_t));
  }
  kb->bind_count--;
}

void keybinds_clear_action(keybind_manager_t *kb, action_t action) {
  for (int i = kb->bind_count - 1; i >= 0; i--) {
    if (kb->bindings[i].action_id == action) {
      keybinds_remove(kb, i);
    }
  }
}

bool keybinds_is_action_pressed(keybind_manager_t *kb, action_t action, bool repeat) {
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) {
      if (is_key_combo_pressed(&kb->bindings[i].combo, repeat)) return true;
    }
  }
  return false;
}

bool keybinds_is_action_down(keybind_manager_t *kb, action_t action) {
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) {
      if (is_key_combo_down(&kb->bindings[i].combo)) return true;
    }
  }
  return false;
}

int keybinds_get_count_for_action(keybind_manager_t *kb, action_t action) {
  int count = 0;
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) count++;
  }
  return count;
}

keybind_entry_t *keybinds_get_binding_for_action(keybind_manager_t *kb, action_t action, int n) {
  int count = 0;
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) {
      if (count == n) return &kb->bindings[i];
      count++;
    }
  }
  return NULL;
}

int keybinds_get_global_index_for_action(keybind_manager_t *kb, action_t action, int n) {
  int count = 0;
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) {
      if (count == n) return i;
      count++;
    }
  }
  return -1;
}

static void set_action_info(keybind_manager_t *kb, action_t action, const char *id, const char *name, const char *cat) {
    kb->action_infos[action].identifier = id;
    kb->action_infos[action].name = name;
    kb->action_infos[action].category = cat;
}

void keybinds_init(keybind_manager_t *manager) {
  memset(manager, 0, sizeof(keybind_manager_t));
  manager->show_settings_window = false;

  // Initialize Action Infos
  set_action_info(manager, ACTION_PLAY_PAUSE, "play_pause", "Play/Pause", "Playback");
  set_action_info(manager, ACTION_REWIND_HOLD, "rewind_hold", "Rewind (Hold)", "Playback");
  set_action_info(manager, ACTION_PREV_FRAME, "prev_frame", "Previous Frame", "Playback");
  set_action_info(manager, ACTION_NEXT_FRAME, "next_frame", "Next Frame", "Playback");
  set_action_info(manager, ACTION_INC_TPS, "inc_tps", "Increase TPS", "Playback");
  set_action_info(manager, ACTION_DEC_TPS, "dec_tps", "Decrease TPS", "Playback");

  set_action_info(manager, ACTION_SELECT_ALL, "select_all", "Select all Snippets", "Timeline");
  set_action_info(manager, ACTION_DELETE_SNIPPET, "delete_snippet", "Delete Snippet", "Timeline");
  set_action_info(manager, ACTION_SPLIT_SNIPPET, "split_snippet", "Split Snippet", "Timeline");
  set_action_info(manager, ACTION_MERGE_SNIPPETS, "merge_snippets", "Merge Snippets", "Timeline");
  set_action_info(manager, ACTION_TOGGLE_SNIPPET_ACTIVE, "toggle_snippet_active", "Toggle Snippet Active", "Timeline");

  set_action_info(manager, ACTION_TOGGLE_FULLSCREEN, "toggle_fullscreen", "Toggle Fullscreen", "General");
  set_action_info(manager, ACTION_UNDO, "undo", "Undo", "General");
  set_action_info(manager, ACTION_REDO, "redo", "Redo", "General");

  set_action_info(manager, ACTION_TRIM_SNIPPET, "trim_snippet", "Trim Recording", "Recording");
  set_action_info(manager, ACTION_CANCEL_RECORDING, "cancel_recording", "Cancel Recording", "Recording");
  set_action_info(manager, ACTION_LEFT, "move_left", "Move Left", "Recording");
  set_action_info(manager, ACTION_RIGHT, "move_right", "Move Right", "Recording");
  set_action_info(manager, ACTION_JUMP, "jump", "Jump", "Recording");
  set_action_info(manager, ACTION_KILL, "kill", "Kill", "Recording");
  set_action_info(manager, ACTION_FIRE, "fire", "Fire weapon", "Recording");
  set_action_info(manager, ACTION_HOOK, "hook", "Hook", "Recording");
  set_action_info(manager, ACTION_HAMMER, "hammer", "Switch to hammer", "Recording");
  set_action_info(manager, ACTION_GUN, "gun", "Switch to gun", "Recording");
  set_action_info(manager, ACTION_SHOTGUN, "shotgun", "Switch to shotgun", "Recording");
  set_action_info(manager, ACTION_GRENADE, "grenade", "Switch to grenade", "Recording");
  set_action_info(manager, ACTION_LASER, "laser", "Switch to laser", "Recording");

  set_action_info(manager, ACTION_DUMMY_FIRE, "dummy_fire", "Dummy Fire", "Dummy");
  set_action_info(manager, ACTION_TOGGLE_DUMMY_COPY, "toggle_dummy_copy", "Toggle dummy copy", "Dummy");

  set_action_info(manager, ACTION_ZOOM_IN, "zoom_in", "Zoom in", "Camera");
  set_action_info(manager, ACTION_ZOOM_OUT, "zoom_out", "Zoom out", "Camera");

  set_action_info(manager, ACTION_SWITCH_TRACK_1, "switch_track_1", "Switch to Track 1", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_2, "switch_track_2", "Switch to Track 2", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_3, "switch_track_3", "Switch to Track 3", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_4, "switch_track_4", "Switch to Track 4", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_5, "switch_track_5", "Switch to Track 5", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_6, "switch_track_6", "Switch to Track 6", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_7, "switch_track_7", "Switch to Track 7", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_8, "switch_track_8", "Switch to Track 8", "Tracks");
  set_action_info(manager, ACTION_SWITCH_TRACK_9, "switch_track_9", "Switch to Track 9", "Tracks");

  // Default Bindings
  keybinds_add(manager, ACTION_PLAY_PAUSE, (key_combo_t){ImGuiKey_X, false, false, false});
  keybinds_add(manager, ACTION_REWIND_HOLD, (key_combo_t){ImGuiKey_C, false, false, false});
  keybinds_add(manager, ACTION_PREV_FRAME, (key_combo_t){ImGuiKey_MouseX1, false, false, false});
  keybinds_add(manager, ACTION_NEXT_FRAME, (key_combo_t){ImGuiKey_MouseX2, false, false, false});
  keybinds_add(manager, ACTION_INC_TPS, (key_combo_t){ImGuiKey_UpArrow, false, false, false});
  keybinds_add(manager, ACTION_DEC_TPS, (key_combo_t){ImGuiKey_DownArrow, false, false, false});

  keybinds_add(manager, ACTION_SELECT_ALL, (key_combo_t){ImGuiKey_A, true, false, false});
  keybinds_add(manager, ACTION_DELETE_SNIPPET, (key_combo_t){ImGuiKey_Delete, false, false, false});
  keybinds_add(manager, ACTION_SPLIT_SNIPPET, (key_combo_t){ImGuiKey_R, true, false, false});
  keybinds_add(manager, ACTION_MERGE_SNIPPETS, (key_combo_t){ImGuiKey_M, true, false, false});
  keybinds_add(manager, ACTION_TOGGLE_SNIPPET_ACTIVE, (key_combo_t){ImGuiKey_A, false, false, false});

  keybinds_add(manager, ACTION_TOGGLE_FULLSCREEN, (key_combo_t){ImGuiKey_F11, false, false, false});
  keybinds_add(manager, ACTION_UNDO, (key_combo_t){ImGuiKey_Z, true, false, false});
  keybinds_add(manager, ACTION_REDO, (key_combo_t){ImGuiKey_Y, true, false, false});

  keybinds_add(manager, ACTION_TRIM_SNIPPET, (key_combo_t){ImGuiKey_F, false, false, false});
  keybinds_add(manager, ACTION_CANCEL_RECORDING, (key_combo_t){ImGuiKey_F4, false, false, false});
  keybinds_add(manager, ACTION_LEFT, (key_combo_t){ImGuiKey_A, false, false, false});
  keybinds_add(manager, ACTION_RIGHT, (key_combo_t){ImGuiKey_D, false, false, false});
  keybinds_add(manager, ACTION_JUMP, (key_combo_t){ImGuiKey_Space, false, false, false});
  keybinds_add(manager, ACTION_KILL, (key_combo_t){ImGuiKey_K, false, false, false});
  keybinds_add(manager, ACTION_FIRE, (key_combo_t){ImGuiKey_MouseLeft, false, false, false});
  keybinds_add(manager, ACTION_HOOK, (key_combo_t){ImGuiKey_MouseRight, false, false, false});
  keybinds_add(manager, ACTION_HAMMER, (key_combo_t){ImGuiKey_1, false, false, false});
  keybinds_add(manager, ACTION_GUN, (key_combo_t){ImGuiKey_2, false, false, false});
  keybinds_add(manager, ACTION_SHOTGUN, (key_combo_t){ImGuiKey_3, false, false, false});
  keybinds_add(manager, ACTION_GRENADE, (key_combo_t){ImGuiKey_4, false, false, false});
  keybinds_add(manager, ACTION_LASER, (key_combo_t){ImGuiKey_5, false, false, false});

  keybinds_add(manager, ACTION_DUMMY_FIRE, (key_combo_t){ImGuiKey_V, false, false, false});
  keybinds_add(manager, ACTION_TOGGLE_DUMMY_COPY, (key_combo_t){ImGuiKey_R, false, false, false});

  keybinds_add(manager, ACTION_ZOOM_IN, (key_combo_t){ImGuiKey_W, false, false, false});
  keybinds_add(manager, ACTION_ZOOM_OUT, (key_combo_t){ImGuiKey_S, false, false, false});

  for (int i = 0; i < 9; ++i) {
    keybinds_add(manager, ACTION_SWITCH_TRACK_1 + i, (key_combo_t){ImGuiKey_1 + i, false, true, false});
  }
}

void keybinds_process_inputs(struct ui_handler *ui) {
  if (igIsAnyItemActive()) return;

  timeline_state_t *ts = &ui->timeline;
  keybind_manager_t *kb = &ui->keybinds;
  undo_command_t *cmd = NULL;

  if (keybinds_is_action_pressed(kb, ACTION_PLAY_PAUSE, false)) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
    }
  }

  for (int i = 0; i < 9; i++) {
    if (keybinds_is_action_pressed(kb, ACTION_SWITCH_TRACK_1 + i, false)) {
      int new_index = imin(i, ts->player_track_count - 1);
      if (ts->recording && ts->selected_player_track_index != new_index) {
        timeline_switch_recording_target(ts, new_index);
      }
      ts->selected_player_track_index = new_index;
      break;
    }
  }

  if (keybinds_is_action_pressed(kb, ACTION_PREV_FRAME, true)) {
    ts->is_playing = false;
    model_advance_tick(ts, -1);
  }
  if (keybinds_is_action_pressed(kb, ACTION_NEXT_FRAME, true)) {
    ts->is_playing = false;
    interaction_apply_dummy_inputs(ui);
    model_advance_tick(ts, 1);
  }
  if (keybinds_is_action_pressed(kb, ACTION_INC_TPS, true)) {
    ++ts->gui_playback_speed;
  }
  if (keybinds_is_action_pressed(kb, ACTION_DEC_TPS, true)) {
    --ts->gui_playback_speed;
  }
  if (keybinds_is_action_pressed(kb, ACTION_TOGGLE_DUMMY_COPY, false)) {
    ts->dummy_copy_input ^= 1;
  }

  if (ts->recording) return;

  if (keybinds_is_action_pressed(kb, ACTION_SELECT_ALL, false)) {
    interaction_clear_selection(ts);
    ts->active_snippet_id = -1;
    for (int i = 0; i < ts->player_track_count; i++) {
      for (int j = 0; j < ts->player_tracks[i].snippet_count; j++) {
        interaction_add_snippet_to_selection(ts, ts->player_tracks[i].snippets[j].id);
      }
    }
  }
  if (keybinds_is_action_pressed(kb, ACTION_DELETE_SNIPPET, false)) cmd = commands_create_delete_selected(ui);
  if (keybinds_is_action_pressed(kb, ACTION_SPLIT_SNIPPET, false)) cmd = commands_create_split_selected(ui);
  if (keybinds_is_action_pressed(kb, ACTION_MERGE_SNIPPETS, false)) cmd = commands_create_merge_selected(ui);

  if (keybinds_is_action_pressed(kb, ACTION_TOGGLE_SNIPPET_ACTIVE, false)) {
    cmd = commands_create_toggle_selected_snippets_active(ui);
  }

  if (keybinds_is_action_pressed(kb, ACTION_TOGGLE_FULLSCREEN, false)) {
    gfx_toggle_fullscreen(ui->gfx_handler);
  }

  if (keybinds_is_action_pressed(kb, ACTION_UNDO, false)) undo_manager_undo(&ui->undo_manager, ts);
  if (keybinds_is_action_pressed(kb, ACTION_REDO, false)) undo_manager_redo(&ui->undo_manager, ts);

  if (cmd) {
    undo_manager_register_command(&ui->undo_manager, cmd);
  }
}

static bool is_modifier_key(ImGuiKey key) {
  return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
         key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
         key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt ||
         key == ImGuiKey_ReservedForModSuper;
}

// Check for perfect duplicates
static bool has_perfect_duplicate(keybind_manager_t *kb, action_t action, key_combo_t combo) {
  for (int i = 0; i < kb->bind_count; i++) {
    if (kb->bindings[i].action_id == action) {
      key_combo_t *other = &kb->bindings[i].combo;
      if (other->key == combo.key && other->ctrl == combo.ctrl && other->alt == combo.alt && other->shift == combo.shift) {
        return true;
      }
    }
  }
  return false;
}

// Render logic for a single action in the settings window
static void render_keybind_entry(keybind_manager_t *manager, action_t action_id) {
  int count = keybinds_get_count_for_action(manager, action_id);
  
  // Show all existing bindings
  for (int i = 0; i < count; i++) {
    int global_idx = keybinds_get_global_index_for_action(manager, action_id, i);
    keybind_entry_t *binding = &manager->bindings[global_idx];
    
    igPushID_Int(action_id * 1000 + i);

    const char *button_label;
    if (manager->is_waiting_for_input && manager->action_to_rebind == action_id && manager->rebind_index == global_idx) {
      button_label = "[ waiting ]";
    } else {
      button_label = keybind_get_combo_string(&binding->combo);
    }

    if (igButton(button_label, (ImVec2){120.0f, 0})) {
      manager->is_waiting_for_input = true;
      manager->action_to_rebind = action_id;
      manager->rebind_index = global_idx;
    }

    igSameLine(0, 6.0f);
    if (igButton(ICON_KI_TRASH, (ImVec2){0, 0})) {
        keybinds_remove(manager, global_idx);
        // We removed an item, so we must stop iterating since indices shifted
        igPopID();
        break; 
    }
    
    // Only put on same line if not the last one, wrapping handled by table or layout
    igSameLine(0, 6.0f);
    igPopID();
  }

  // "Add" button
  igPushID_Int(action_id * 1000 + 999);
  if (manager->is_waiting_for_input && manager->action_to_rebind == action_id && manager->rebind_index == -1) {
    if (igButton("[ press key ]", (ImVec2){100.0f, 0})) {
        // Cancel add
        manager->is_waiting_for_input = false;
        manager->rebind_index = -2;
    }
  } else {
    if (igButton("+", (ImVec2){30.0f, 0})) {
        manager->is_waiting_for_input = true;
        manager->action_to_rebind = action_id;
        manager->rebind_index = -1; // New binding
    }
  }
  igPopID();
}

void keybinds_render_settings_window(struct ui_handler *ui) {
  keybind_manager_t *manager = &ui->keybinds;
  if (!manager->show_settings_window) return;

  igSetNextWindowSize((ImVec2){600, 500}, ImGuiCond_FirstUseEver);
  if (igBegin("Controls", &manager->show_settings_window, 0)) {

    igSetNextWindowPos((ImVec2){igGetIO_Nil()->DisplaySize.x * 0.5f, igGetIO_Nil()->DisplaySize.y * 0.5f}, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f});
    if (manager->is_waiting_for_input) igOpenPopup_Str("RebindKeyPopup", ImGuiPopupFlags_AnyPopupLevel);

    if (igBeginPopupModal("RebindKeyPopup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (manager->rebind_index == -1)
        igText("Press keys to add binding for '%s'", manager->action_infos[manager->action_to_rebind].name);
      else
        igText("Press keys to replace binding for '%s'", manager->action_infos[manager->action_to_rebind].name);
        
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
            key_combo_t new_combo;
            new_combo.key = key;
            new_combo.ctrl = io->KeyCtrl;
            new_combo.alt = io->KeyAlt;
            new_combo.shift = io->KeyShift;
            
            // Check for perfect duplicate
            if (has_perfect_duplicate(manager, manager->action_to_rebind, new_combo)) {
                 log_warn("Keybinds", "Duplicate binding added.");
            }
            
            if (manager->rebind_index == -1) {
                // Add new
                keybinds_add(manager, manager->action_to_rebind, new_combo);
            } else {
                // Replace
                manager->bindings[manager->rebind_index].combo = new_combo;
            }
            
            manager->is_waiting_for_input = false;
            igCloseCurrentPopup();
            break;
          }
        }
      }
      igEndPopup();
    }

    igText("Click '+' to add a binding. Click trash icon to remove.");
    igSeparator();

    if (igCollapsingHeader_TreeNodeFlags("Mouse Settings", 0)) {
      igDragFloat("Sensitivity", &ui->mouse_sens, 0.5f, 1.0f, 1000.0f, "%.1f", 0);
      igDragFloat("Max Distance", &ui->mouse_max_distance, 1.0f, 0.0f, 2000.0f, "%.1f", 0);
    }

    const char *categories[] = {"Playback", "Timeline", "General", "Recording", "Dummy", "Camera", "Tracks"};
    int num_categories = sizeof(categories) / sizeof(categories[0]);

    for (int cat_idx = 0; cat_idx < num_categories; ++cat_idx) {
      const char *current_category = categories[cat_idx];

      ImGuiTreeNodeFlags flags = (strcmp(current_category, "Tracks") == 0) ? 0 : ImGuiTreeNodeFlags_DefaultOpen;

      if (igCollapsingHeader_TreeNodeFlags(current_category, flags)) {
        if (igBeginTable("KeybindsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg, (ImVec2){0, 0}, 0)) {
          igTableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
          igTableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);

          if (strcmp(current_category, "Dummy") == 0) {
            igTableNextRow(0, 0);
            igTableSetColumnIndex(0);
            igText("Action Priority (Top = First, Bottom = Last/Overwrites)");
            for (int i = 0; i < DUMMY_ACTION_COUNT; ++i) {
                igPushID_Int(1000 + i);
                dummy_action_type_t action = ui->timeline.dummy_action_priority[i];
                const char *name = (action == DUMMY_ACTION_COPY) ? "Copy Input" : "Dummy Fire";
                igText("  %d. %s", i + 1, name);
                igSameLine(0, 10);
                if (i > 0 && igArrowButton("##up", ImGuiDir_Up)) {
                    dummy_action_type_t temp = ui->timeline.dummy_action_priority[i];
                    ui->timeline.dummy_action_priority[i] = ui->timeline.dummy_action_priority[i - 1];
                    ui->timeline.dummy_action_priority[i - 1] = temp;
                }
                igSameLine(0, 10);
                if (i < DUMMY_ACTION_COUNT - 1 && igArrowButton("##down", ImGuiDir_Down)) {
                    dummy_action_type_t temp = ui->timeline.dummy_action_priority[i];
                    ui->timeline.dummy_action_priority[i] = ui->timeline.dummy_action_priority[i + 1];
                    ui->timeline.dummy_action_priority[i + 1] = temp;
                }
                igPopID();
            }
            igSeparator();
          }

          for (int i = 0; i < ACTION_COUNT; i++) {
            if (strcmp(manager->action_infos[i].category, current_category) == 0) {
              igTableNextRow(0, 0);
              igTableSetColumnIndex(0);

              igAlignTextToFramePadding();
              igTextUnformatted(manager->action_infos[i].name, NULL);

              igTableSetColumnIndex(1);
              render_keybind_entry(manager, (action_t)i);
            }
          }
          igEndTable();
        }
      }
    }
  }
  igEnd();
}