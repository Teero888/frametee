#include "snippet_editor.h"
#include "cimgui.h"
#include "timeline.h"
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
} SnippetEditorState;

static SnippetEditorState editor_state = {.last_selected_row = -1, .active_snippet_id = -1};

static void reset_editor_state() {
  memset(editor_state.selected_rows, 0, sizeof(editor_state.selected_rows));
  editor_state.selection_count = 0;
  editor_state.last_selected_row = -1;
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

// --- RESTORED: Bulk Edit Panel ---
static const char *weapon_options[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};

static void render_bulk_edit_panel(timeline_state_t *ts, input_snippet_t *snippet) {
  igSeparatorText("Bulk Edit Selected Ticks");
  if (editor_state.selection_count == 0) {
    igTextDisabled("Select one or more rows to enable bulk editing.");
    return;
  }
  igText("%d tick(s) selected.", editor_state.selection_count);
  igSpacing();
  int earliest_tick = -1;
  const char *dir_opts[] = {"Left", "Neutral", "Right"};
  int dir_idx = editor_state.bulk_dir + 1;
  if (igCombo_Str_arr("Direction", &dir_idx, dir_opts, 3, 3)) {
    editor_state.bulk_dir = dir_idx - 1;
  }
  igSameLine(0, 5);
  if (igButton("Set Dir", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++) {
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Direction = editor_state.bulk_dir;
    }
  }
  igPushItemWidth(80);
  igInputInt("Target X Start", &editor_state.bulk_target_x_start, 0, 0, 0);
  igSameLine(0, 5);
  igInputInt("End##TX", &editor_state.bulk_target_x_end, 0, 0, 0);
  igPopItemWidth();
  igSameLine(0, 5);
  if (igButton("Ramp TX", (ImVec2){0, 0})) {
    int start_idx, end_idx;
    get_selection_bounds(&start_idx, &end_idx);
    earliest_tick = start_idx;
    int range = (end_idx - start_idx);
    if (range <= 0) {
      if (start_idx != -1)
        snippet->inputs[start_idx].m_TargetX = editor_state.bulk_target_x_start;
    } else {
      for (int i = start_idx; i <= end_idx; i++) {
        if (editor_state.selected_rows[i]) {
          float t = (float)(i - start_idx) / range;
          snippet->inputs[i].m_TargetX =
              (int16_t)(editor_state.bulk_target_x_start * (1.0f - t) + editor_state.bulk_target_x_end * t);
        }
      }
    }
  }
  igPushItemWidth(80);
  igInputInt("Target Y Start", &editor_state.bulk_target_y_start, 0, 0, 0);
  igSameLine(0, 5);
  igInputInt("End##TY", &editor_state.bulk_target_y_end, 0, 0, 0);
  igPopItemWidth();
  igSameLine(0, 5);
  if (igButton("Ramp TY", (ImVec2){0, 0})) {
    int start_idx, end_idx;
    get_selection_bounds(&start_idx, &end_idx);
    earliest_tick = start_idx;
    int range = (end_idx - start_idx);
    if (range <= 0) {
      if (start_idx != -1)
        snippet->inputs[start_idx].m_TargetY = editor_state.bulk_target_y_start;
    } else {
      for (int i = start_idx; i <= end_idx; i++) {
        if (editor_state.selected_rows[i]) {
          float t = (float)(i - start_idx) / range;
          snippet->inputs[i].m_TargetY =
              (int16_t)(editor_state.bulk_target_y_start * (1.0f - t) + editor_state.bulk_target_y_end * t);
        }
      }
    }
  }
  igCombo_Str_arr("Weapon", &editor_state.bulk_weapon, weapon_options, 6, 4);
  igSameLine(0, 5);
  if (igButton("Set Wpn", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++) {
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_WantedWeapon = editor_state.bulk_weapon;
    }
  }
  igSeparator();
  if (igButton("Set Jump ON", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Jump = 1;
  }
  igSameLine(0, 5);
  if (igButton("Set Jump OFF", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Jump = 0;
  }
  igSameLine(0, 20);
  if (igButton("Set Fire ON", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Fire = 1;
  }
  igSameLine(0, 5);
  if (igButton("Set Fire OFF", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Fire = 0;
  }
  igSameLine(0, 20);
  if (igButton("Set Hook ON", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Hook = 1;
  }
  igSameLine(0, 5);
  if (igButton("Set Hook OFF", (ImVec2){0, 0})) {
    get_selection_bounds(&earliest_tick, NULL);
    for (int i = 0; i < snippet->input_count; i++)
      if (editor_state.selected_rows[i])
        snippet->inputs[i].m_Hook = 0;
  }
  if (earliest_tick != -1) {
    recalc_ts(ts, snippet->start_tick + earliest_tick);
  }
}

void render_snippet_editor_panel(timeline_state_t *ts) {
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

    float footer_height = igGetStyle()->ItemSpacing.y + 160;
    igBeginChild_Str("InputsScroll", (ImVec2){0, -footer_height}, false,
                     ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

    if (igBeginTable("InputsTable", 9, flags, (ImVec2){0, 0}, 0)) {
      igTableSetupScrollFreeze(1, 1);
      igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
      igTableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
      igTableSetupColumn("TX", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
      igTableSetupColumn("TY", ImGuiTableColumnFlags_WidthFixed, 0.0f, 3);
      igTableSetupColumn("J", ImGuiTableColumnFlags_WidthFixed, 0.0f, 4);
      igTableSetupColumn("F", ImGuiTableColumnFlags_WidthFixed, 0.0f, 5);
      igTableSetupColumn("H", ImGuiTableColumnFlags_WidthFixed, 0.0f, 6);
      igTableSetupColumn("Wpn", ImGuiTableColumnFlags_WidthFixed, 0.0f, 7);
      igTableSetupColumn("Tele", ImGuiTableColumnFlags_WidthFixed, 0.0f, 8);
      igTableHeadersRow();

      // Stop painting when the mouse button is released anywhere
      if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
        editor_state.is_painting = false;
      }

      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, snippet->input_count, 0);
      while (ImGuiListClipper_Step(clipper)) {
        for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; ++i) {
          SPlayerInput *inp = &snippet->inputs[i];

          igTableNextRow(0, 0);

          // MODIFICATION START: Use direct table background color modification.
          // This is more robust than using the style stack and fixes the off-by-one bug.
          if (editor_state.selected_rows[i]) {
            ImU32 selection_color = igGetColorU32_Col(ImGuiCol_HeaderActive, 0.6f);
            igTableSetBgColor(ImGuiTableBgTarget_RowBg0, selection_color, -1);
            igTableSetBgColor(ImGuiTableBgTarget_RowBg1, selection_color, -1);
          }
          // MODIFICATION END

          igTableSetColumnIndex(0);
          char label[32];
          snprintf(label, 32, "%d", snippet->start_tick + i);
          // We make the selectable transparent because we are handling the background color ourselves.
          igPushStyleColor_U32(ImGuiCol_Header, 0);
          igPushStyleColor_U32(ImGuiCol_HeaderHovered, 0);
          igPushStyleColor_U32(ImGuiCol_HeaderActive, 0);
          if (igSelectable_Bool(label, editor_state.selected_rows[i],
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                (ImVec2){0, 0})) {
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
              memset(editor_state.selected_rows, 0, sizeof(editor_state.selected_rows));
              editor_state.selected_rows[i] = true;
            }
            editor_state.last_selected_row = i;
            ts->current_tick = snippet->start_tick + i;
            editor_state.selection_count = 0;
            for (int k = 0; k < snippet->input_count; k++)
              if (editor_state.selected_rows[k])
                editor_state.selection_count++;
          }
          igPopStyleColor(3);

          bool needs_recalc = false;
          int recalc_tick = snippet->start_tick + i;

          // Direction
          igTableSetColumnIndex(1);
          igPushID_Int(i * 10 + 1);

          const char *dir_text;
          ImVec4 dir_color;
          switch (inp->m_Direction) {
          case -1:
            dir_text = "L";
            dir_color = (ImVec4){0.6f, 0.8f, 1.0f, 1.0f}; // Lighter Blue
            break;
          case 1:
            dir_text = "R";
            dir_color = (ImVec4){1.0f, 0.6f, 0.6f, 1.0f}; // Lighter Red
            break;
          default:
            dir_text = "N";
            dir_color = (ImVec4){0.9f, 0.9f, 0.9f, 1.0f}; // Lighter Grey
            break;
          }
          igPushStyleColor_Vec4(ImGuiCol_Text, dir_color);

          igSetNextItemAllowOverlap();
          igButton(dir_text, (ImVec2){25, 0});

          igPopStyleColor(1); // Pop the text color

          if (igIsItemClicked(ImGuiMouseButton_Left)) {
            editor_state.is_painting = true;
            editor_state.painting_column = 1;
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
              inp->m_Direction = editor_state.painting_value;
              needs_recalc = true;
            }
          }
          igPopID();

          // Target X/Y
          igTableSetColumnIndex(2);
          igPushID_Int(i * 10 + 2);
          igSetNextItemWidth(50);
          int temp_tx = inp->m_TargetX;
          if (igInputInt("##TX", &temp_tx, 0, 0, 0)) {
            inp->m_TargetX = (int16_t)temp_tx;
            needs_recalc = true;
          }
          igPopID();
          igTableSetColumnIndex(3);
          igPushID_Int(i * 10 + 3);
          igSetNextItemWidth(50);
          int temp_ty = inp->m_TargetY;
          if (igInputInt("##TY", &temp_ty, 0, 0, 0)) {
            inp->m_TargetY = (int16_t)temp_ty;
            needs_recalc = true;
          }
          igPopID();

          // Booleans
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
            igInvisibleButton("##bool_interaction", (ImVec2){-1, igGetFrameHeight()}, 0);

            ImVec2 r_min, r_max;
            igGetItemRectMin(&r_min);
            igGetItemRectMax(&r_max);
            ImDrawList_AddRectFilled(igGetWindowDrawList(), r_min, r_max, *val ? c_on : c_off, 2.0f, 0);

            if (igIsItemClicked(ImGuiMouseButton_Left)) {
              editor_state.is_painting = true;
              editor_state.painting_column = current_column;
              *val = !*val;
              editor_state.painting_value = *val;
              needs_recalc = true;
            }
            if (editor_state.is_painting && editor_state.painting_column == current_column &&
                igIsMouseHoveringRect(r_min, r_max, false)) {
              if (*val != editor_state.painting_value) {
                *val = editor_state.painting_value;
                needs_recalc = true;
              }
            }
            igPopID();
          }

          // Weapon
          igTableSetColumnIndex(7);
          igPushID_Int(i * 10 + 7);
          const char *wi[] = {"Hm", "Gn", "Sg", "Gr", "Ls", "Nj"};
          if (igButton(wi[inp->m_WantedWeapon], (ImVec2){30, 0})) {
            inp->m_WantedWeapon = (inp->m_WantedWeapon + 1) % 6;
            needs_recalc = true;
          }
          if (igIsItemClicked(ImGuiMouseButton_Right)) {
            inp->m_WantedWeapon = (inp->m_WantedWeapon + 5) % 6;
            needs_recalc = true;
          }
          igPopID();

          // Teleport
          igTableSetColumnIndex(8);
          igPushID_Int(i * 10 + 8);
          igSetNextItemWidth(40);
          int temp_tele = inp->m_TeleOut;
          if (igInputInt("##Tele", &temp_tele, 0, 0, 0)) {
            inp->m_TeleOut = (uint8_t)temp_tele;
            needs_recalc = true;
          }
          igPopID();

          if (needs_recalc)
            recalc_ts(ts, recalc_tick);

          // We no longer need to pop row colors here as we are using igTableSetBgColor.
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
      igEndTable();
    }

    igEndChild();
    render_bulk_edit_panel(ts, snippet);
  }
  igEnd();
}
