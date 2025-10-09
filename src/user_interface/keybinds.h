#ifndef KEYBINDS_H
#define KEYBINDS_H

#include "cimgui.h"
#include <stdbool.h>

typedef struct ui_handler ui_handler_t;

typedef enum {
  // playback controls
  ACTION_PLAY_PAUSE,
  ACTION_REWIND_HOLD,
  ACTION_PREV_FRAME,
  ACTION_NEXT_FRAME,

  // timeline editing
  ACTION_SELECT_ALL,
  ACTION_DELETE_SNIPPET,
  ACTION_SPLIT_SNIPPET,
  ACTION_MERGE_SNIPPETS,

  // general application
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

  ACTION_COUNT
} action_t;

typedef struct {
  ImGuiKey key;
  bool ctrl;
  bool alt;
  bool shift;
} key_combo_t;

typedef struct {
  const char *name;
  const char *category;
  key_combo_t combo;
} keybind_t;

typedef struct {
  keybind_t bindings[ACTION_COUNT];
  bool show_settings_window;

  // state for the ui when re-binding a key
  bool is_waiting_for_input;
  action_t action_to_rebind;
} keybind_manager_t;

void keybinds_init(keybind_manager_t *manager);
void keybinds_process_inputs(ui_handler_t *ui);
void keybinds_render_settings_window(keybind_manager_t *manager);
const char *keybind_get_combo_string(const key_combo_t *combo);

bool is_key_combo_pressed(const key_combo_t *combo, bool repeat);
bool is_key_combo_down(const key_combo_t *combo);
#endif // KEYBINDS_H
