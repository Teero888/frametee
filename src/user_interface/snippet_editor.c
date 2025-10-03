#include "snippet_editor.h"
#include "cimgui.h"
#include "timeline.h"
#include "user_interface.h"
#include <float.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_INPUTS 8192

typedef struct {
  bool selected_rows[MAX_INPUTS];
  int selection_count;
  int last_selected_row;
  int active_snippet_id;

  // State for "painting" inputs with mouse drag
  bool is_painting;
  int painting_column;
  int painting_value;

  int bulk_dir;
  int bulk_target_x_start, bulk_target_x_end;
  int bulk_target_y_start, bulk_target_y_end;
  int bulk_weapon;

  SPlayerInput *clipboard_inputs;
  int clipboard_count;

  // State for tracking changes for a single undoable action (like painting or a bulk edit)
  bool action_in_progress;
  SPlayerInput *action_before_states; // Stores the 'before' state of inputs that were changed
  int *action_changed_indices;        // Stores the indices of inputs that were changed
  int action_changed_count;
  int action_changed_capacity;

  // State for single input text edit undo/redo
  bool text_edit_in_progress;
  SPlayerInput before_text_edit_state;
  int text_edit_index;
} SnippetEditorState;

static SnippetEditorState editor_state = {.last_selected_row = -1, .active_snippet_id = -1};

static void reset_editor_state() {
  memset(editor_state.selected_rows, 0, sizeof(editor_state.selected_rows));
  editor_state.selection_count = 0;
  editor_state.last_selected_row = -1;
  editor_state.action_in_progress = false;
  editor_state.text_edit_in_progress = false;
  free(editor_state.action_before_states);
  free(editor_state.action_changed_indices);
  editor_state.action_before_states = NULL;
  editor_state.action_changed_indices = NULL;
  editor_state.action_changed_count = 0;
}

static void get_selection_bounds(int *start, int *end) {
  if (start)
    *start = -1;
  if (end)
    *end = -1;
  for (int i = 0; i < MAX_INPUTS; i++) {
    if (editor_state.selected_rows[i]) {
      if (start && *start == -1)
        *start = i;
      if (end)
        *end = i;
    }
  }
}

// UNDO/REDO ACTION MANAGEMENT for Painting and Bulk Edits

// Begins tracking a new multi-input change.
static void begin_action() {
  if (editor_state.action_in_progress)
    return;

  // Clean up any old data, just in case.
  free(editor_state.action_before_states);
  free(editor_state.action_changed_indices);

  editor_state.action_before_states = NULL;
  editor_state.action_changed_indices = NULL;
  editor_state.action_changed_count = 0;
  editor_state.action_changed_capacity = 0;
  editor_state.action_in_progress = true;
}

// Before changing an input at `index`, this function must be called to save its "before" state.
static void record_change_if_new(input_snippet_t *snippet, int index) {
  if (!editor_state.action_in_progress)
    return;

  // Check if this index's "before" state has already been saved for this action.
  for (int i = 0; i < editor_state.action_changed_count; i++) {
    if (editor_state.action_changed_indices[i] == index) {
      return; // Already recorded.
    }
  }

  // Grow the tracking arrays if they are full.
  if (editor_state.action_changed_count >= editor_state.action_changed_capacity) {
    int new_capacity =
        editor_state.action_changed_capacity == 0 ? 16 : editor_state.action_changed_capacity * 2;
    editor_state.action_changed_indices =
        realloc(editor_state.action_changed_indices, sizeof(int) * new_capacity);
    editor_state.action_before_states =
        realloc(editor_state.action_before_states, sizeof(SPlayerInput) * new_capacity);
    editor_state.action_changed_capacity = new_capacity;
  }

  // Save the "before" state and the index.
  editor_state.action_before_states[editor_state.action_changed_count] = snippet->inputs[index];
  editor_state.action_changed_indices[editor_state.action_changed_count] = index;
  editor_state.action_changed_count++;
}

// Finishes the action, creates the undo command, and registers it.
static void end_action(ui_handler_t *ui, input_snippet_t *snippet) {
  if (!editor_state.action_in_progress || editor_state.action_changed_count == 0) {
    editor_state.action_in_progress = false; // Ensure state is reset
    return;
  }

  SPlayerInput *after_states = malloc(sizeof(SPlayerInput) * editor_state.action_changed_count);
  if (!after_states) { /* TODO: Handle malloc failure */
    return;
  }

  for (int i = 0; i < editor_state.action_changed_count; i++) {
    int idx = editor_state.action_changed_indices[i];
    after_states[i] = snippet->inputs[idx];
  }

  undo_command_t *cmd = create_edit_inputs_command(snippet, editor_state.action_changed_indices,
                                                   editor_state.action_changed_count,
                                                   editor_state.action_before_states, after_states);
  undo_manager_register_command(&ui->undo_manager, cmd);

  // We only free the temporary 'after_states' buffer.
  free(after_states);

  editor_state.action_before_states = NULL; // Null out pointers to prevent double-free
  editor_state.action_changed_indices = NULL;
  editor_state.action_in_progress = false;
}

static const char *weapon_options[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};

// Bulk Edit Panel
static void render_bulk_edit_panel(ui_handler_t *ui, input_snippet_t *snippet) {
  timeline_state_t *ts = &ui->timeline;
  igSeparatorText("Bulk Edit Selected Ticks");
  if (editor_state.selection_count == 0) {
    igTextDisabled("Select one or more rows to enable bulk editing.");
    return;
  }
  igText("%d tick(s) selected.", editor_state.selection_count);
  igSpacing();
  int earliest_tick = -1;

  // use a two-column table for a clean, aligned layout.
  if (igBeginTable("BulkEditLayout", 2, ImGuiTableFlags_SizingFixedFit, (ImVec2){0, 0}, 0)) {
    igTableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
    igTableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);

    // Direction
    igTableNextRow(0, 0);
    igTableSetColumnIndex(0);
    igText("Direction");
    igTableSetColumnIndex(1);
    const char *dir_opts[] = {"Left", "Neutral", "Right"};
    int dir_idx = editor_state.bulk_dir + 1;
    igPushItemWidth(-FLT_MIN);
    if (igCombo_Str_arr("##Direction", &dir_idx, dir_opts, 3, 3)) {
      editor_state.bulk_dir = dir_idx - 1;
    }
    igPopItemWidth();
    igSameLine(0, 5);
    if (igButton("Set##Dir", (ImVec2){0, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++) {
        if (editor_state.selected_rows[i]) {
          snippet->inputs[i].m_Direction = editor_state.bulk_dir;
        }
      }
      end_action(ui, snippet);
    }

    // Weapon
    igTableNextRow(0, 0);
    igTableSetColumnIndex(0);
    igText("Weapon");
    igTableSetColumnIndex(1);
    igPushItemWidth(-FLT_MIN);
    igCombo_Str_arr("##Weapon", &editor_state.bulk_weapon, weapon_options, 6, 4);
    igPopItemWidth();
    igSameLine(0, 5);
    if (igButton("Set##Wpn", (ImVec2){0, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++) {
        if (editor_state.selected_rows[i])
          snippet->inputs[i].m_WantedWeapon = editor_state.bulk_weapon;
      }
      end_action(ui, snippet);
    }

    igEndTable();
  }

  igSeparator();

  // use a more structured layout for binary state buttons
  if (igBeginTable("BulkEditActions", 3, ImGuiTableFlags_SizingStretchSame, (ImVec2){0, 0}, 0)) {
    igTableNextRow(0, 0);
    igTableSetColumnIndex(0);
    if (igButton("Set Jump ON", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Jump = 1;
        }
      end_action(ui, snippet);
    }
    igTableSetColumnIndex(1);
    if (igButton("Set Fire ON", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Fire = 1;
        }
      end_action(ui, snippet);
    }
    igTableSetColumnIndex(2);
    if (igButton("Set Hook ON", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Hook = 1;
        }
      end_action(ui, snippet);
    }
    igTableNextRow(0, 0);
    igTableSetColumnIndex(0);
    if (igButton("Set Jump OFF", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Jump = 0;
        }
      end_action(ui, snippet);
    }
    igTableSetColumnIndex(1);
    if (igButton("Set Fire OFF", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Fire = 0;
        }
      end_action(ui, snippet);
    }
    igTableSetColumnIndex(2);
    if (igButton("Set Hook OFF", (ImVec2){-1, 0})) {
      begin_action();
      get_selection_bounds(&earliest_tick, NULL);
      for (int i = 0; i < snippet->input_count; i++)
        if (editor_state.selected_rows[i]) {
          record_change_if_new(snippet, i);
          snippet->inputs[i].m_Hook = 0;
        }
      end_action(ui, snippet);
    }
    igEndTable();
  }

  if (earliest_tick != -1) {
    // All actions now create undo commands, which already call recalc.
    recalc_ts(ts, snippet->start_tick + earliest_tick);
  }
}

void render_snippet_editor_panel(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (igBegin("Snippet Editor", NULL, 0)) {
    if (ts->selected_snippet_id == -1) {
      igText("No snippet selected.");
      igEnd();
      return;
    }

    input_snippet_t *snippet = NULL;
    for (int i = 0; i < ts->player_track_count; i++) {
      snippet = find_snippet_by_id(&ts->player_tracks[i], ts->selected_snippet_id);
      if (snippet)
        break;
    }

    if (!snippet) {
      igText("Selected snippet not found.");
      igEnd();
      return;
    }
    if (editor_state.active_snippet_id != snippet->id) {
      reset_editor_state();
      editor_state.active_snippet_id = snippet->id;
    }
    if (snippet->input_count > MAX_INPUTS) {
      igText("Error: Snippet has too many inputs (%d) to edit.", snippet->input_count);
      igEnd();
      return;
    }

    igText("Editing Snippet ID: %d (%d inputs)", snippet->id, snippet->input_count);
    igTextDisabled("Hint: Click+Drag to 'paint' inputs. Use Ctrl+Click and Shift+Click to select rows.");

    float footer_height = igGetStyle()->ItemSpacing.y + 220;
    igBeginChild_Str("InputsScroll", (ImVec2){0, -footer_height}, false,
                     ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame;

    if (igBeginTable("InputsTable", 9, flags, (ImVec2){0, 0}, 0)) {
      // The user can still resize them, but they will start out evenly spaced.
      igTableSetupScrollFreeze(1, 1);
      igTableSetupColumn("Tick", ImGuiTableColumnFlags_None, 0.0f, 0);
      igTableSetupColumn("Dir", ImGuiTableColumnFlags_None, 0.0f, 1);
      igTableSetupColumn("TX", ImGuiTableColumnFlags_None, 0.0f, 2);
      igTableSetupColumn("TY", ImGuiTableColumnFlags_None, 0.0f, 3);
      igTableSetupColumn("J", ImGuiTableColumnFlags_None, 0.0f, 4);
      igTableSetupColumn("F", ImGuiTableColumnFlags_None, 0.0f, 5);
      igTableSetupColumn("H", ImGuiTableColumnFlags_None, 0.0f, 6);
      igTableSetupColumn("Wpn", ImGuiTableColumnFlags_None, 0.0f, 7);
      igTableSetupColumn("Tele", ImGuiTableColumnFlags_None, 0.0f, 8);
      igTableHeadersRow();

      if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
        editor_state.is_painting = false;
        if (editor_state.action_in_progress) {
          end_action(ui, snippet);
        }
      }

      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, snippet->input_count, 0);
      while (ImGuiListClipper_Step(clipper)) {
        for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; ++i) {
          SPlayerInput *inp = &snippet->inputs[i];

          igTableNextRow(0, 0);

          if (editor_state.selected_rows[i]) {
            ImU32 selection_color = igGetColorU32_Col(ImGuiCol_HeaderHovered, 0.6f);
            igTableSetBgColor(ImGuiTableBgTarget_RowBg0, selection_color, -1);
            igTableSetBgColor(ImGuiTableBgTarget_RowBg1, selection_color, -1);
          }

          igTableSetColumnIndex(0);
          char label[32];
          snprintf(label, 32, "%d", snippet->start_tick + i);

          char selectable_id[32];
          snprintf(selectable_id, 32, "##Selectable%d", i);

          // We make the selectable transparent because we are handling the background color ourselves.
          igPushStyleColor_U32(ImGuiCol_Header, 0);
          igPushStyleColor_U32(ImGuiCol_HeaderHovered, 0);
          igPushStyleColor_U32(ImGuiCol_HeaderActive, 0);

          // By using a label-less selectable and giving it a proper height, its hitbox
          // will correctly span the entire row thanks to the SpanAllColumns flag.
          if (igSelectable_Bool(selectable_id, editor_state.selected_rows[i],
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                (ImVec2){0, igGetFrameHeight()})) {
            // ... (selection logic remains exactly the same)
            ImGuiIO *io = igGetIO_Nil();
            if (io->KeyCtrl) {
              editor_state.selected_rows[i] = !editor_state.selected_rows[i];
            } else if (io->KeyShift && editor_state.last_selected_row != -1) {
              int start = editor_state.last_selected_row < i ? editor_state.last_selected_row : i;
              int end = editor_state.last_selected_row > i ? editor_state.last_selected_row : i;
              memset(editor_state.selected_rows, 0, sizeof(editor_state.selected_rows));
              for (int j = start; j <= end; j++)
                editor_state.selected_rows[j] = true;
            } else {
              const bool was_only_selection =
                  editor_state.selected_rows[i] && editor_state.selection_count == 1;
              memset(editor_state.selected_rows, 0, sizeof(editor_state.selected_rows));
              if (!was_only_selection) {
                editor_state.selected_rows[i] = true;
              }
            }
            editor_state.last_selected_row = i;
            ts->current_tick = snippet->start_tick + i;
            editor_state.selection_count = 0;
            for (int k = 0; k < snippet->input_count; k++)
              if (editor_state.selected_rows[k])
                editor_state.selection_count++;
          }
          igPopStyleColor(3);

          // Now, draw the text on top of the selectable area in the same cell.
          igSameLine(0.0f, 4.0f);
          igTextUnformatted(label, NULL);

          bool needs_recalc = false;
          int recalc_tick = snippet->start_tick + i;

          // Direction
          igTableSetColumnIndex(1);
          igPushID_Int(i * 10 + 1);
          // ... (color logic is fine)
          const char *dir_text;
          ImVec4 dir_color;
          switch (inp->m_Direction) {
          case -1:
            dir_text = "L";
            dir_color = (ImVec4){0.6f, 0.8f, 1.0f, 1.0f};
            break;
          case 1:
            dir_text = "R";
            dir_color = (ImVec4){1.0f, 0.6f, 0.6f, 1.0f};
            break;
          default:
            dir_text = "N";
            dir_color = (ImVec4){0.9f, 0.9f, 0.9f, 1.0f};
            break;
          }
          igPushStyleColor_Vec4(ImGuiCol_Text, dir_color);
          igSetNextItemAllowOverlap();
          igButton(dir_text, (ImVec2){-1, 0});
          igPopStyleColor(1);
          if (igIsItemClicked(ImGuiMouseButton_Left)) {
            // Now that a selection is guaranteed, begin the action.
            begin_action();
            editor_state.is_painting = true;
            editor_state.painting_column = 1;
            record_change_if_new(snippet, i); // Record state before changing
            inp->m_Direction = (inp->m_Direction + 1 + 1) % 3 - 1;
            editor_state.painting_value = inp->m_Direction;
            needs_recalc = true;
          }
          ImVec2 dir_min, dir_max;
          igGetItemRectMin(&dir_min);
          igGetItemRectMax(&dir_max);
          if (editor_state.is_painting && editor_state.painting_column == 1 &&
              igIsMouseHoveringRect(dir_min, dir_max, false)) {
            if (inp->m_Direction != editor_state.painting_value) {
              record_change_if_new(snippet, i);
              inp->m_Direction = editor_state.painting_value;
              needs_recalc = true;
            }
          }
          igPopID();

          // Target X/Y
          igTableSetColumnIndex(2);
          igPushID_Int(i * 10 + 2);
          igPushItemWidth(-FLT_MIN);
          int temp_tx = inp->m_TargetX;
          if (igIsItemActivated()) {
            editor_state.text_edit_in_progress = true;
            editor_state.before_text_edit_state = *inp;
            editor_state.text_edit_index = i;
          }
          if (igInputInt("##TX", &temp_tx, 0, 0, 0)) {
            inp->m_TargetX = (int16_t)temp_tx;
            needs_recalc = true;
          }
          if (igIsItemDeactivatedAfterEdit()) {
            if (editor_state.text_edit_in_progress && editor_state.text_edit_index == i) {
              undo_command_t *cmd = create_edit_inputs_command(snippet, &editor_state.text_edit_index, 1,
                                                               &editor_state.before_text_edit_state, inp);
              undo_manager_register_command(&ui->undo_manager, cmd);
            }
            editor_state.text_edit_in_progress = false;
          }
          igPopItemWidth();
          igPopID();
          igTableSetColumnIndex(3);
          igPushID_Int(i * 10 + 3);
          igPushItemWidth(-FLT_MIN);
          int temp_ty = inp->m_TargetY;
          if (igIsItemActivated()) {
            editor_state.text_edit_in_progress = true;
            editor_state.before_text_edit_state = *inp;
            editor_state.text_edit_index = i;
          }
          if (igInputInt("##TY", &temp_ty, 0, 0, 0)) {
            inp->m_TargetY = (int16_t)temp_ty;
            needs_recalc = true;
          }
          if (igIsItemDeactivatedAfterEdit()) {
            if (editor_state.text_edit_in_progress && editor_state.text_edit_index == i) {
              undo_command_t *cmd = create_edit_inputs_command(snippet, &editor_state.text_edit_index, 1,
                                                               &editor_state.before_text_edit_state, inp);
              undo_manager_register_command(&ui->undo_manager, cmd);
            }
            editor_state.text_edit_in_progress = false;
          }
          igPopItemWidth();
          igPopID();

          // Booleans (J, F, H)
          for (int j = 0; j < 3; j++) {
            int current_column = 4 + j;
            igTableSetColumnIndex(current_column);
            igPushID_Int(i * 10 + current_column);
            uint8_t *val = (j == 0) ? &inp->m_Jump : (j == 1) ? &inp->m_Fire : &inp->m_Hook;
            ImU32 c_on = (j == 0)   ? igGetColorU32_Vec4((ImVec4){0.4f, 0.7f, 1.0f, 1.0f})
                         : (j == 1) ? igGetColorU32_Vec4((ImVec4){1.0f, 0.4f, 0.4f, 1.0f})
                                    : igGetColorU32_Vec4((ImVec4){0.8f, 0.8f, 0.8f, 1.0f});
            ImU32 c_off = igGetColorU32_Vec4((ImVec4){0.2f, 0.2f, 0.2f, 1.0f});
            igSetNextItemAllowOverlap();
            // This was already correct, using -1 for width.
            igInvisibleButton("##bool_interaction", (ImVec2){-1, igGetFrameHeight()}, 0);
            ImVec2 r_min, r_max;
            igGetItemRectMin(&r_min);
            igGetItemRectMax(&r_max);
            ImDrawList_AddRectFilled(igGetWindowDrawList(), r_min, r_max, *val ? c_on : c_off, 2.0f, 0);
            if (igIsItemClicked(ImGuiMouseButton_Left)) {
              begin_action();
              editor_state.is_painting = true;
              editor_state.painting_column = current_column;
              record_change_if_new(snippet, i);
              *val = !*val;
              editor_state.painting_value = *val;
              needs_recalc = true;
            }
            if (editor_state.is_painting && editor_state.painting_column == current_column &&
                igIsMouseHoveringRect(r_min, r_max, false)) {
              if (*val != editor_state.painting_value) {
                record_change_if_new(snippet, i);
                *val = editor_state.painting_value;
                needs_recalc = true;
              }
            }
            igPopID();
          }

          // Weapon
          igTableSetColumnIndex(7);
          igPushID_Int(i * 10 + 7);
          const char *wi[] = {"Hm", "Gn", "Sg", "Gr", "Ls"};
          if (igButton(wi[inp->m_WantedWeapon], (ImVec2){-1, 0})) {
            begin_action();
            record_change_if_new(snippet, i);
            inp->m_WantedWeapon = (inp->m_WantedWeapon + 1) % (NUM_WEAPONS - 1);
            end_action(ui, snippet);
            needs_recalc = true;
          }
          if (igIsItemClicked(ImGuiMouseButton_Right)) {
            begin_action();
            record_change_if_new(snippet, i);
            inp->m_WantedWeapon = (inp->m_WantedWeapon + NUM_WEAPONS - 2) % (NUM_WEAPONS - 1);
            end_action(ui, snippet);
            needs_recalc = true;
          }
          igPopID();

          // Teleport
          igTableSetColumnIndex(8);
          igPushID_Int(i * 10 + 8);
          igPushItemWidth(-FLT_MIN);
          int temp_tele = inp->m_TeleOut;
          if (igIsItemActivated()) {
            editor_state.text_edit_in_progress = true;
            editor_state.before_text_edit_state = *inp;
            editor_state.text_edit_index = i;
          }
          if (igInputInt("##Tele", &temp_tele, 0, 0, 0)) {
            inp->m_TeleOut = (uint8_t)temp_tele;
            needs_recalc = true;
          }
          if (igIsItemDeactivatedAfterEdit()) {
            if (editor_state.text_edit_in_progress && editor_state.text_edit_index == i) {
              undo_command_t *cmd = create_edit_inputs_command(snippet, &editor_state.text_edit_index, 1,
                                                               &editor_state.before_text_edit_state, inp);
              undo_manager_register_command(&ui->undo_manager, cmd);
            }
            editor_state.text_edit_in_progress = false;
          }
          igPopItemWidth();
          igPopID();

          if (needs_recalc)
            recalc_ts(ts, recalc_tick);
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
      igEndTable();
    }

    // Keybind Handling
    if (editor_state.selection_count > 0) {
      bool changed = false;
      int earliest_tick = -1;

      ImGuiIO *io = igGetIO_Nil();
      bool needs_recalc = false;

      // Deselect all with Escape
      if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) && editor_state.selection_count > 0) {
        reset_editor_state();
      }

      // Copy with Ctrl+C
      if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_C, false) && editor_state.selection_count > 0) {
        if (editor_state.clipboard_inputs) {
          free(editor_state.clipboard_inputs);
        }
        editor_state.clipboard_count = editor_state.selection_count;
        editor_state.clipboard_inputs =
            (SPlayerInput *)malloc(editor_state.clipboard_count * sizeof(SPlayerInput));
        if (editor_state.clipboard_inputs) {
          int clipboard_idx = 0;
          for (int i = 0; i < snippet->input_count && clipboard_idx < editor_state.clipboard_count; i++) {
            if (editor_state.selected_rows[i]) {
              editor_state.clipboard_inputs[clipboard_idx++] = snippet->inputs[i];
            }
          }
        }
      }

      // Paste with Ctrl+V
      if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_V, false) && editor_state.clipboard_count > 0 &&
          editor_state.selection_count > 0) {
        begin_action();
        get_selection_bounds(&earliest_tick, NULL);
        if (earliest_tick != -1) {
          int clipboard_idx = 0;
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i]) {
              record_change_if_new(snippet, i);
              snippet->inputs[i] =
                  editor_state.clipboard_inputs[clipboard_idx % editor_state.clipboard_count];
              clipboard_idx++;
            }
          }
          end_action(ui, snippet);
          needs_recalc = true;
        }
      }
      if (!igIsWindowHovered(0)) {
        if (igIsKeyPressed_Bool(ImGuiKey_A, true)) {
          begin_action();
          get_selection_bounds(&earliest_tick, NULL);
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i] && snippet->inputs[i].m_Direction > -1) {
              record_change_if_new(snippet, i);
              snippet->inputs[i].m_Direction--;
              changed = true;
            }
          }
          if (changed)
            end_action(ui, snippet);
          else
            editor_state.action_in_progress = false;
        }

        if (igIsKeyPressed_Bool(ImGuiKey_D, true)) {
          begin_action();
          if (!changed)
            get_selection_bounds(&earliest_tick, NULL);
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i] && snippet->inputs[i].m_Direction < 1) {
              record_change_if_new(snippet, i);
              snippet->inputs[i].m_Direction++;
              changed = true;
            }
          }
          if (changed)
            end_action(ui, snippet);
          else
            editor_state.action_in_progress = false;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_Space, true)) {
          begin_action(snippet);
          get_selection_bounds(&earliest_tick, NULL);
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i]) {
              record_change_if_new(snippet, i);
              snippet->inputs[i].m_Jump ^= 1;
              changed = true;
            }
          }
          if (changed)
            end_action(ui, snippet);
          else
            editor_state.action_in_progress = false;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_Q, false)) {
          begin_action();
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i]) {
              record_change_if_new(snippet, i);
              snippet->inputs[i].m_Fire ^= 1;
              changed = true;
            }
          }
          if (changed)
            end_action(ui, snippet);
          else
            editor_state.action_in_progress = false;
        }
        // Mouse right click is for context menus, so avoiding it for keybinds.
        if (igIsKeyPressed_Bool(ImGuiKey_E, false)) {
          begin_action();
          for (int i = 0; i < snippet->input_count; i++) {
            if (editor_state.selected_rows[i] && snippet->inputs[i].m_Direction < 1) {
              snippet->inputs[i].m_Hook ^= 1;
              changed = true;
            }
          }
          if (changed)
            end_action(ui, snippet);
          else
            editor_state.action_in_progress = false;
        }
      }

      if (changed && earliest_tick != -1) {
        recalc_ts(ts, snippet->start_tick + earliest_tick);
      }
    }

    igEndChild();
    render_bulk_edit_panel(ui, snippet);
  }
  igEnd();
}
