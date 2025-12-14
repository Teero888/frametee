#ifndef KEYBINDS_H
#define KEYBINDS_H

#include <stdbool.h>
#include <system/include_cimgui.h>

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

  // recording
  ACTION_TRIM_SNIPPET,
  ACTION_CANCEL_RECORDING,
  ACTION_FIRE,
  ACTION_HOOK,
  ACTION_LEFT,
  ACTION_RIGHT,
  ACTION_JUMP,
  ACTION_HAMMER,
  ACTION_GUN,
  ACTION_SHOTGUN,
  ACTION_GRENADE,
  ACTION_LASER,
  ACTION_KILL,

  // dummy binds
  ACTION_DUMMY_LEFT,
  ACTION_DUMMY_RIGHT,
  ACTION_DUMMY_JUMP,
  ACTION_DUMMY_FIRE,
  ACTION_DUMMY_HOOK,
  ACTION_DUMMY_AIM,
  ACTION_TOGGLE_DUMMY_COPY,

  // camera
  ACTION_ZOOM_IN,
  ACTION_ZOOM_OUT,

  ACTION_COUNT
} action_t;

typedef struct {
  ImGuiKey key;
  bool ctrl;
  bool alt;
  bool shift;
} key_combo_t;

typedef struct {
  const char *identifier;
  const char *name;
  const char *category;
} action_info_t;

typedef struct {
  action_t action_id;
  key_combo_t combo;
} keybind_entry_t;

typedef struct {
  // Static info about actions
  action_info_t action_infos[ACTION_COUNT];

  // Dynamic list of bindings
  keybind_entry_t *bindings;
  int bind_count;
  int bind_capacity;

  bool show_settings_window;

  // state for the ui when re-binding a key
  bool is_waiting_for_input;
  action_t action_to_rebind;
  int rebind_index; // -1 if adding new, otherwise index in global list (or logic specific index)
} keybind_manager_t;

struct ui_handler;

void keybinds_init(keybind_manager_t *manager);
void keybinds_process_inputs(struct ui_handler *ui);
void keybinds_render_settings_window(struct ui_handler *ui);
const char *keybind_get_combo_string(const key_combo_t *combo);

bool is_key_combo_pressed(const key_combo_t *combo, bool repeat);
bool is_key_combo_down(const key_combo_t *combo);

// Helper functions for multiple bindings
void keybinds_add(keybind_manager_t *kb, action_t action, key_combo_t combo);
void keybinds_remove(keybind_manager_t *kb, int index); // Index in the global array
void keybinds_clear_action(keybind_manager_t *kb, action_t action);
bool keybinds_is_action_pressed(keybind_manager_t *kb, action_t action, bool repeat);
bool keybinds_is_action_down(keybind_manager_t *kb, action_t action);
int keybinds_get_count_for_action(keybind_manager_t *kb, action_t action);
keybind_entry_t *keybinds_get_binding_for_action(keybind_manager_t *kb, action_t action, int n); // Get n-th binding for action
int keybinds_get_global_index_for_action(keybind_manager_t *kb, action_t action, int n);
#endif // KEYBINDS_H