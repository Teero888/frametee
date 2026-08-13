#include <engine/int_math.h>
#include "keybinds.h"
#include <frametee/icons.h>
#include <GLFW/glfw3.h>
#include <system/input.h>
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <limits.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/config.h>
#include <system/include_cimgui.h>

// Binds read GLFW directly. Imgui trickles its event queue, holding back every mouse position
// queued behind a key or wheel event, which put input several frames behind the hardware at high
// polling rates. Imgui is still asked whether a text field is focused so typing cannot fire binds.
static bool combo_blocked_by_ui(const key_combo_t *combo) {
  // Mouse binds are left ungated: the viewport is itself an imgui window, so WantCaptureMouse is
  // true whenever the cursor is over it and gating on it would stop mouse-bound game controls.
  if (input_glfw_button_from_imgui(combo->key) != -1) return false;

  // Only typing blocks keyboard binds. Deliberately not WantCaptureKeyboard, which is also true
  // whenever any widget is merely active, e.g. while dragging a snippet or scrubbing the ruler.
  return igGetIO_Nil()->WantTextInput;
}

static bool combo_modifiers_match(const key_combo_t *combo) {
  return combo->ctrl == input_ctrl_down() && combo->alt == input_alt_down() && combo->shift == input_shift_down();
}

// check if a key combination is pressed for single-press actions
bool is_key_combo_pressed(const key_combo_t *combo, bool repeat) {
  if (combo->key == ImGuiKey_None) return false;
  if (!combo_modifiers_match(combo) || combo_blocked_by_ui(combo)) return false;

  int button = input_glfw_button_from_imgui(combo->key);
  if (button != -1) return input_mouse_pressed(button);

  return input_key_pressed(input_glfw_key_from_imgui(combo->key), repeat);
}

// check if a key combination is held down
bool is_key_combo_down(const key_combo_t *combo) {
  if (combo->key == ImGuiKey_None) return false;
  if (!combo_modifiers_match(combo) || combo_blocked_by_ui(combo)) return false;

  int button = input_glfw_button_from_imgui(combo->key);
  if (button != -1) return input_mouse_down(button);

  return input_key_down(input_glfw_key_from_imgui(combo->key));
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
  snprintf(kb->action_infos[action].identifier, sizeof(kb->action_infos[action].identifier), "%s", id ? id : "");
  snprintf(kb->action_infos[action].name, sizeof(kb->action_infos[action].name), "%s", name ? name : "");
  snprintf(kb->action_infos[action].category, sizeof(kb->action_infos[action].category), "%s", cat ? cat : "");
}

void keybinds_init(keybind_manager_t *manager) {
  memset(manager, 0, sizeof(keybind_manager_t));
  manager->action_count = ACTION_ENGINE_COUNT;
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
  set_action_info(manager, ACTION_SAVE_PROJECT, "save_project", "Quick Save Project", "General");

  set_action_info(manager, ACTION_TRIM_SNIPPET, "trim_snippet", "Trim Recording", "Recording");
  set_action_info(manager, ACTION_CANCEL_RECORDING, "cancel_recording", "Cancel Recording", "Recording");
  set_action_info(manager, ACTION_TOGGLE_LINKED_COPY, "toggle_linked_copy", "Toggle Linked Input Copy", "Recording");
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
  set_action_info(manager, ACTION_CYCLE_TRACK_UP, "cycle_track_up", "Cycle Track Up", "Tracks");
  set_action_info(manager, ACTION_CYCLE_TRACK_DOWN, "cycle_track_down", "Cycle Track Down", "Tracks");

  // Default Bindings
  keybinds_add(manager, ACTION_PLAY_PAUSE, (key_combo_t){ImGuiKey_X, false, false, false});
  keybinds_add(manager, ACTION_REWIND_HOLD, (key_combo_t){ImGuiKey_C, false, false, false});
  keybinds_add(manager, ACTION_PREV_FRAME, (key_combo_t){ImGuiKey_MouseX1, false, false, false});
  keybinds_add(manager, ACTION_PREV_FRAME, (key_combo_t){ImGuiKey_LeftArrow, false, false, false});
  keybinds_add(manager, ACTION_NEXT_FRAME, (key_combo_t){ImGuiKey_MouseX2, false, false, false});
  keybinds_add(manager, ACTION_NEXT_FRAME, (key_combo_t){ImGuiKey_RightArrow, false, false, false});
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
  keybinds_add(manager, ACTION_SAVE_PROJECT, (key_combo_t){ImGuiKey_S, true, false, false});

  keybinds_add(manager, ACTION_TRIM_SNIPPET, (key_combo_t){ImGuiKey_F, false, false, false});
  keybinds_add(manager, ACTION_CANCEL_RECORDING, (key_combo_t){ImGuiKey_F4, false, false, false});
  keybinds_add(manager, ACTION_TOGGLE_LINKED_COPY, (key_combo_t){ImGuiKey_R, false, false, false});
  keybinds_add(manager, ACTION_ZOOM_IN, (key_combo_t){ImGuiKey_Equal, false, false, false});
  keybinds_add(manager, ACTION_ZOOM_OUT, (key_combo_t){ImGuiKey_Minus, false, false, false});

  for (int i = 0; i < 9; ++i) {
    keybinds_add(manager, ACTION_SWITCH_TRACK_1 + i, (key_combo_t){ImGuiKey_1 + i, false, true, false});
  }

  // default track cycling binds (both PageUp/Down and Alt+Up/Down)
  keybinds_add(manager, ACTION_CYCLE_TRACK_UP, (key_combo_t){ImGuiKey_PageUp, false, false, false});
  keybinds_add(manager, ACTION_CYCLE_TRACK_UP, (key_combo_t){ImGuiKey_UpArrow, false, true, false});
  keybinds_add(manager, ACTION_CYCLE_TRACK_DOWN, (key_combo_t){ImGuiKey_PageDown, false, false, false});
  keybinds_add(manager, ACTION_CYCLE_TRACK_DOWN, (key_combo_t){ImGuiKey_DownArrow, false, true, false});
}

bool keybinds_parse_combo(const char *text, key_combo_t *out) {
  if (!text || !out) return false;
  *out = (key_combo_t){.key = ImGuiKey_None};
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "%s", text);
  for (char *part = strtok(buffer, "+"); part; part = strtok(NULL, "+")) {
    if (strcmp(part, "Ctrl") == 0) out->ctrl = true;
    else if (strcmp(part, "Alt") == 0) out->alt = true;
    else if (strcmp(part, "Shift") == 0) out->shift = true;
    else {
      for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const char *name = igGetKeyName(key);
        if (name && strcmp(name, part) == 0) {
          out->key = key;
          break;
        }
      }
    }
  }
  return out->key != ImGuiKey_None;
}

action_t keybinds_game_action(unsigned control_index) { return (action_t)(ACTION_GAME_FIRST + control_index); }
action_t keybinds_linked_game_action(unsigned control_index) { return (action_t)(ACTION_LINKED_GAME_FIRST + control_index); }
action_t keybinds_linked_extra_action(unsigned action_index) { return (action_t)(ACTION_LINKED_EXTRA_FIRST + action_index); }

void keybinds_bind_game(keybind_manager_t *manager, const game_host_t *host) {
  if (!manager) return;
  for (int action = ACTION_GAME_FIRST; action < ACTION_COUNT; ++action) {
    keybinds_clear_action(manager, (action_t)action);
    memset(&manager->action_infos[action], 0, sizeof(manager->action_infos[action]));
  }
  manager->game_action_count = 0;
  manager->linked_action_count = 0;
  manager->action_count = ACTION_ENGINE_COUNT;
  if (!host || !game_host_ready(host)) return;

  const ft_input_schema *schema = game_input_schema(host);
  if (!schema || !schema->controls) return;
  unsigned count = schema->control_count;
  if (count > (unsigned)(ACTION_COUNT - ACTION_GAME_FIRST)) count = ACTION_COUNT - ACTION_GAME_FIRST;
  for (unsigned i = 0; i < count; ++i) {
    const ft_input_control *control = &schema->controls[i];
    action_t action = keybinds_game_action(i);
    char identifier[96];
    snprintf(identifier, sizeof(identifier), "game_%s_%s", game_host_active_id(host), control->id);
    set_action_info(manager, action, identifier, control->display_name,
                    control->category && *control->category ? control->category : host->module->info.display_name);
    key_combo_t combo;
    if (keybinds_parse_combo(control->default_binding, &combo)) keybinds_add(manager, action, combo);
  }
  manager->game_action_count = (int)count;
  manager->action_count = ACTION_GAME_FIRST + (int)count;

  if (!game_has_cap(host, FT_CAP_LINKED_INPUTS)) return;

  for (unsigned i = 0; i < count; ++i) {
    const ft_input_control *control = &schema->controls[i];
    const action_t action = keybinds_linked_game_action(i);
    char identifier[96];
    char name[64];
    char category[64];
    snprintf(identifier, sizeof(identifier), "game_%s_linked_%s", game_host_active_id(host), control->id);
    snprintf(name, sizeof(name), "Linked: %s", control->display_name);
    snprintf(category, sizeof(category), "%s Linked Inputs", host->module->info.display_name);
    set_action_info(manager, action, identifier, name, category);
    key_combo_t combo;
    if (keybinds_parse_combo(control->linked_default_binding, &combo)) keybinds_add(manager, action, combo);
  }

  unsigned linked_count = host->module->linked_action_count;
  if (linked_count > 64) linked_count = 64;
  for (unsigned i = 0; i < linked_count; ++i) {
    const ft_linked_action *desc = &host->module->linked_actions[i];
    const action_t action = keybinds_linked_extra_action(i);
    char identifier[96];
    char category[64];
    snprintf(identifier, sizeof(identifier), "game_%s_linked_%s", game_host_active_id(host), desc->id);
    snprintf(category, sizeof(category), "%s Linked Inputs", host->module->info.display_name);
    set_action_info(manager, action, identifier, desc->display_name, category);
    key_combo_t combo;
    if (keybinds_parse_combo(desc->default_binding, &combo)) keybinds_add(manager, action, combo);
  }
  manager->linked_action_count = (int)linked_count;
  manager->action_count = ACTION_LINKED_EXTRA_FIRST + (int)linked_count;
}

void keybinds_cleanup(keybind_manager_t *manager) {
  if (!manager) return;
  free(manager->bindings);
  manager->bindings = NULL;
  manager->bind_count = 0;
  manager->bind_capacity = 0;
}

void keybinds_process_inputs(ui_handler_t *ui) {
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
      interaction_select_track(ts, new_index);
      break;
    }
  }

  if (keybinds_is_action_pressed(kb, ACTION_CYCLE_TRACK_UP, false)) {
    if (ts->player_track_count > 0) {
      int cur = ts->selected_player_track_index;
      if (cur < 0) cur = 0;
      int new_index = cur - 1;
      if (new_index < 0) {
        new_index = ts->player_track_count - 1;
      }
      if (ts->recording && ts->selected_player_track_index != new_index) {
        timeline_switch_recording_target(ts, new_index);
      }
      interaction_select_track(ts, new_index);
    }
  }
  if (keybinds_is_action_pressed(kb, ACTION_CYCLE_TRACK_DOWN, false)) {
    if (ts->player_track_count > 0) {
      int cur = ts->selected_player_track_index;
      if (cur < 0) cur = 0;
      int new_index = cur + 1;
      if (new_index >= ts->player_track_count) {
        new_index = 0;
      }
      if (ts->recording && ts->selected_player_track_index != new_index) {
        timeline_switch_recording_target(ts, new_index);
      }
      interaction_select_track(ts, new_index);
    }
  }

  if (keybinds_is_action_pressed(kb, ACTION_PREV_FRAME, true)) {
    ts->is_playing = false;
    interaction_update_mouse(ts);
    model_advance_tick(ts, -1);
  }
  if (keybinds_is_action_pressed(kb, ACTION_NEXT_FRAME, true)) {
    ts->is_playing = false;
    interaction_apply_linked_inputs(ui);
    interaction_update_mouse(ts);
    model_advance_tick(ts, 1);
    // Pull the world for the new tick so the game gets a chance to run its
    // own per-tick effects before the frame is drawn.
    model_world_at_tick(ts, ts->current_tick);
  }
  if (keybinds_is_action_pressed(kb, ACTION_INC_TPS, true)) {
    ++ts->gui_playback_speed;
  }
  if (keybinds_is_action_pressed(kb, ACTION_DEC_TPS, true)) {
    --ts->gui_playback_speed;
  }
  if (game_has_cap(&ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS) &&
      keybinds_is_action_pressed(kb, ACTION_TOGGLE_LINKED_COPY, false)) {
    ts->linked_copy_input ^= 1;
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
  if (keybinds_is_action_pressed(kb, ACTION_SAVE_PROJECT, false)) ui_quick_save(ui);

  if (cmd) {
    undo_manager_register_command(&ui->undo_manager, cmd);
  }
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
static void render_keybind_entry(ui_handler_t *ui, keybind_manager_t *manager, action_t action_id) {
  int count = keybinds_get_count_for_action(manager, action_id);
  float dpi_scale = gfx_get_ui_scale();

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

    if (igButton(button_label, (ImVec2){120.0f * dpi_scale, 0})) {
      manager->is_waiting_for_input = true;
      manager->action_to_rebind = action_id;
      manager->rebind_index = global_idx;
    }

    igSameLine(0, 6.0f * dpi_scale);
    if (ui_icon_button(ui, ICON_FA_TRASH, (ImVec2){30.f * dpi_scale, 0})) {
      keybinds_remove(manager, global_idx);
      config_save(ui);
      // We removed an item, so we must stop iterating since indices shifted
      igPopID();
      break;
    }
    if (igIsItemHovered(0)) igSetTooltip("Remove keybind");

    // Only put on same line if not the last one, wrapping handled by table or layout
    igSameLine(0, 6.0f * dpi_scale);
    igPopID();
  }

  // "Add" button
  igPushID_Int(action_id * 1000 + 999);
  if (manager->is_waiting_for_input && manager->action_to_rebind == action_id && manager->rebind_index == -1) {
    if (igButton("[ press key ]", (ImVec2){100.0f * dpi_scale, 0})) {
      // Cancel add
      manager->is_waiting_for_input = false;
      manager->rebind_index = -2;
    }
  } else {
    if (igButton("+", (ImVec2){30.0f * dpi_scale, 0})) {
      manager->is_waiting_for_input = true;
      manager->action_to_rebind = action_id;
      manager->rebind_index = -1; // New binding
    }
  }
  igPopID();
}

void keybinds_render_settings_window(ui_handler_t *ui) {
  keybind_manager_t *manager = &ui->keybinds;
  if (!manager->show_settings_window) return;

  igSetNextWindowSize((ImVec2){600, 500}, ImGuiCond_FirstUseEver);
  if (igBegin("Controls", &manager->show_settings_window, 0)) {

    igSetNextWindowPos((ImVec2){igGetIO_Nil()->DisplaySize.x * 0.5f, igGetIO_Nil()->DisplaySize.y * 0.5f}, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f});
    if (manager->is_waiting_for_input) igOpenPopup_Str("RebindKeyPopup", ImGuiPopupFlags_AnyPopupLevel);

    if (igBeginPopupModal("RebindKeyPopup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (manager->rebind_index == -1) igText("Press keys to add binding for '%s'", manager->action_infos[manager->action_to_rebind].name);
      else igText("Press keys to replace binding for '%s'", manager->action_infos[manager->action_to_rebind].name);

      igSeparator();
      igText("Press ESC to cancel.");

      // Capture through GLFW as well, so what gets bound is exactly what the binds will later read.
      if (input_key_pressed(GLFW_KEY_ESCAPE, false)) {
        manager->is_waiting_for_input = false;
        igCloseCurrentPopup();
      } else {
        ImGuiKey key = input_capture_pressed_key();
        {
          if (key != ImGuiKey_None) {
            key_combo_t new_combo;
            new_combo.key = key;
            new_combo.ctrl = input_ctrl_down();
            new_combo.alt = input_alt_down();
            new_combo.shift = input_shift_down();

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
            config_save(ui);

            manager->is_waiting_for_input = false;
            igCloseCurrentPopup();
          }
        }
      }
      igEndPopup();
    }

    igText("Click '+' to add a binding. Click trash icon to remove.");
    igSeparator();

    if (engine_input_cursor_field() >= 0 && igCollapsingHeader_TreeNodeFlags("Aim Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (igDragFloat("Sensitivity", &ui->mouse_sens, 0.5f, 1.0f, 1000.0f, "%.1f", 0)) config_save(ui);
      if (igDragFloat("Max Distance", &ui->mouse_max_distance, 1.0f, 0.0f, 2000.0f, "%.1f", 0)) config_save(ui);
    }

    for (int cat_idx = 0; cat_idx < manager->action_count; ++cat_idx) {
      const char *current_category = manager->action_infos[cat_idx].category;
      if (!*current_category) continue;
      bool already_rendered = false;
      for (int previous = 0; previous < cat_idx; ++previous) {
        if (strcmp(manager->action_infos[previous].category, current_category) == 0) already_rendered = true;
      }
      if (already_rendered) continue;

      ImGuiTreeNodeFlags flags = (strcmp(current_category, "Tracks") == 0) ? 0 : ImGuiTreeNodeFlags_DefaultOpen;

      if (igCollapsingHeader_TreeNodeFlags(current_category, flags)) {
        if (igBeginTable("KeybindsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg, (ImVec2){0, 0}, 0)) {
          igTableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
          igTableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);

          for (int i = 0; i < manager->action_count; i++) {
            if (strcmp(manager->action_infos[i].category, current_category) == 0) {
              igTableNextRow(0, 0);
              igTableSetColumnIndex(0);

              igAlignTextToFramePadding();
              igTextUnformatted(manager->action_infos[i].name, NULL);

              igTableSetColumnIndex(1);
              render_keybind_entry(ui, manager, (action_t)i);
            }
          }
          igEndTable();
        }
      }
    }
  }
  igEnd();
}
