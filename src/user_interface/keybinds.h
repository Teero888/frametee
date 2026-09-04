#ifndef KEYBINDS_H
#define KEYBINDS_H

#include <stdbool.h>
#include <system/include_cimgui.h>
#include <types.h>

struct game_host_t;

typedef enum {
  // playback controls
  ACTION_PLAY_PAUSE,
  ACTION_REWIND_HOLD,
  ACTION_PREV_FRAME,
  ACTION_NEXT_FRAME,
  ACTION_INC_TPS,
  ACTION_DEC_TPS,

  // timeline editing
  ACTION_SELECT_ALL,
  ACTION_DELETE_SNIPPET,
  ACTION_SPLIT_SNIPPET,
  ACTION_MERGE_SNIPPETS,
  ACTION_TOGGLE_SNIPPET_ACTIVE,

  // general application
  ACTION_TOGGLE_FULLSCREEN,
  ACTION_UNDO,
  ACTION_REDO,
  ACTION_SAVE_PROJECT,
  ACTION_OPEN_PROJECT,
  ACTION_SAVE_PROJECT_AS,
  ACTION_OPEN_CONTROLS,

  // track switching
  ACTION_SWITCH_TRACK_1,
  ACTION_SWITCH_TRACK_2,
  ACTION_SWITCH_TRACK_3,
  ACTION_SWITCH_TRACK_4,
  ACTION_SWITCH_TRACK_5,
  ACTION_SWITCH_TRACK_6,
  ACTION_SWITCH_TRACK_7,
  ACTION_SWITCH_TRACK_8,
  ACTION_SWITCH_TRACK_9,
  ACTION_CYCLE_TRACK_UP,
  ACTION_CYCLE_TRACK_DOWN,

  // recording
  ACTION_TRIM_SNIPPET,
  ACTION_CANCEL_RECORDING,
  ACTION_TOGGLE_LINKED_COPY,

  // camera
  ACTION_ZOOM_IN,
  ACTION_ZOOM_OUT,
  // Steps through the camera modes the active game offers, the engine's freecam
  // and top-down view included. The toolbar button does the same thing.
  ACTION_CYCLE_CAMERA_MODE,
  // 3D only: drives the freecam while that camera mode is selected. These are
  // held rather than tapped, so they are read with keybinds_is_action_held.
  ACTION_FREECAM_FORWARD,
  ACTION_FREECAM_BACK,
  ACTION_FREECAM_LEFT,
  ACTION_FREECAM_RIGHT,
  ACTION_FREECAM_UP,
  ACTION_FREECAM_DOWN,
  ACTION_FREECAM_FAST,

  ACTION_ENGINE_COUNT,
  ACTION_GAME_FIRST = ACTION_ENGINE_COUNT,
  ACTION_LINKED_GAME_FIRST = ACTION_GAME_FIRST + 64,
  ACTION_LINKED_EXTRA_FIRST = ACTION_LINKED_GAME_FIRST + 64,
  ACTION_COUNT = ACTION_LINKED_EXTRA_FIRST + 64
} action_t;

struct key_combo_t {
  ImGuiKey key;
  bool ctrl;
  bool alt;
  bool shift;
};

struct action_info_t {
  char identifier[96];
  char name[64];
  char category[64];
};

struct keybind_entry_t {
  action_t action_id;
  key_combo_t combo;
};

struct keybind_manager_t {
  // Static info about actions
  action_info_t action_infos[ACTION_COUNT];
  int action_count;
  int game_action_count;
  int linked_action_count;

  // Dynamic list of bindings
  keybind_entry_t *bindings;
  int bind_count;
  int bind_capacity;

  // The Controls window. Every other setting is edited inline in the Settings
  // menu; a rebindable action list is a table, which a menu cannot hold.
  bool show_settings_window;

  // state for the ui when re-binding a key
  bool is_waiting_for_input;
  action_t action_to_rebind;
  int rebind_index; // -1 if adding new, otherwise index in global list (or logic specific index)
};

void keybinds_init(keybind_manager_t *manager);
void keybinds_bind_game(keybind_manager_t *manager, const struct game_host_t *host);
void keybinds_cleanup(keybind_manager_t *manager);
bool keybinds_parse_combo(const char *text, key_combo_t *out);
void keybinds_process_inputs(ui_handler_t *ui);
void keybinds_render_settings_window(ui_handler_t *ui);
const char *keybind_get_combo_string(const key_combo_t *combo);

bool is_key_combo_pressed(const key_combo_t *combo, bool repeat);
bool is_key_combo_down(const key_combo_t *combo);
bool is_key_combo_held(const key_combo_t *combo);

// Helper functions for multiple bindings
void keybinds_add(keybind_manager_t *kb, action_t action, key_combo_t combo);
void keybinds_remove(keybind_manager_t *kb, int index); // Index in the global array
void keybinds_clear_action(keybind_manager_t *kb, action_t action);
bool keybinds_is_action_pressed(keybind_manager_t *kb, action_t action, bool repeat);
bool keybinds_is_action_down(keybind_manager_t *kb, action_t action);
// Held, but tolerant of modifiers the bind did not ask for. For actions that
// run continuously and may overlap with a modifier bind, such as moving a
// freecam while sprinting.
bool keybinds_is_action_held(keybind_manager_t *kb, action_t action);
int keybinds_get_count_for_action(keybind_manager_t *kb, action_t action);
keybind_entry_t *keybinds_get_binding_for_action(keybind_manager_t *kb, action_t action, int n); // Get n-th binding for action
int keybinds_get_global_index_for_action(keybind_manager_t *kb, action_t action, int n);
action_t keybinds_game_action(unsigned control_index);
action_t keybinds_linked_game_action(unsigned control_index);
action_t keybinds_linked_extra_action(unsigned action_index);
#endif // KEYBINDS_H
