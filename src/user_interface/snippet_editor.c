// The snippet editor: a tick-by-tick matrix of a snippet's inputs.
//
// Every column here is generated from the active game's input schema. It used
// to have a hardcoded column per DDNet button, which is why the editor could
// only ever edit DDNet. Now a game that declares three buttons gets three
// columns, and one that declares a steering axis gets a slider, without the
// editor knowing what any of them mean.

#include "snippet_editor.h"

#include "input_cleaner.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <engine/input_record.h>
#include <float.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

#define MAX_INPUTS 100000

static struct {
  int snippet_id;
  bool *selected_rows;
  int selected_capacity;
  int last_clicked_row;

  // Cleaning choices are shared by snippets using the same input schema, so
  // moving between snippets does not make the user select the fields again.
  bool *clean_fields;
  int clean_field_capacity;
  const ft_input_schema *clean_schema;

  // One undoable action covers a whole drag or bulk edit, so the before-state
  // is captured once when it starts.
  bool action_in_progress;
  input_record_t *action_before_states;
  int action_before_count;

  // Bulk edit target: which schema field, and the value to write.
  int bulk_field;
  long long bulk_value;
  float bulk_float_value;
  ft_vec2 bulk_vec2_value;
  char clean_status[192];
} editor_state = {.snippet_id = -1, .last_clicked_row = -1, .bulk_field = 0};

static void ensure_selection_capacity(int count) {
  if (count <= editor_state.selected_capacity) return;
  bool *grown = realloc(editor_state.selected_rows, (size_t)count * sizeof(bool));
  if (!grown) return;
  memset(grown + editor_state.selected_capacity, 0, (size_t)(count - editor_state.selected_capacity) * sizeof(bool));
  editor_state.selected_rows = grown;
  editor_state.selected_capacity = count;
}

static void clear_row_selection(void) {
  if (editor_state.selected_rows) memset(editor_state.selected_rows, 0, (size_t)editor_state.selected_capacity * sizeof(bool));
  editor_state.last_clicked_row = -1;
}

static bool ensure_clean_field_selection(const ft_input_schema *schema) {
  if ((int)schema->field_count > editor_state.clean_field_capacity) {
    bool *grown = realloc(editor_state.clean_fields, schema->field_count * sizeof(*grown));
    if (!grown) return false;
    editor_state.clean_fields = grown;
    editor_state.clean_field_capacity = (int)schema->field_count;
  }

  if (editor_state.clean_schema != schema) {
    for (uint32_t i = 0; i < schema->field_count; ++i)
      editor_state.clean_fields[i] = !(schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL);
    editor_state.clean_schema = schema;
  }
  return true;
}

static void reset_editor_state(void) {
  clear_row_selection();
  editor_state.action_in_progress = false;
  free(editor_state.action_before_states);
  editor_state.action_before_states = NULL;
  editor_state.action_before_count = 0;
  editor_state.clean_status[0] = '\0';
}

void snippet_editor_reset(void) {
  reset_editor_state();
  editor_state.snippet_id = -1;
  editor_state.clean_schema = NULL;
}

void snippet_editor_cleanup(void) {
  snippet_editor_reset();
  free(editor_state.selected_rows);
  editor_state.selected_rows = NULL;
  editor_state.selected_capacity = 0;
  free(editor_state.clean_fields);
  editor_state.clean_fields = NULL;
  editor_state.clean_field_capacity = 0;
}

static bool begin_action(const input_snippet_t *snippet) {
  if (editor_state.action_in_progress) return editor_state.action_before_states != NULL;
  editor_state.action_in_progress = true;
  editor_state.action_before_count = snippet->input_count;
  free(editor_state.action_before_states);
  editor_state.action_before_states = malloc(sizeof(input_record_t) * (size_t)snippet->input_count);
  if (!editor_state.action_before_states) {
    editor_state.action_in_progress = false;
    editor_state.action_before_count = 0;
    return false;
  }
  memcpy(editor_state.action_before_states, snippet_window(snippet), sizeof(input_record_t) * (size_t)snippet->input_count);
  return true;
}

static void cancel_action(void) {
  editor_state.action_in_progress = false;
  free(editor_state.action_before_states);
  editor_state.action_before_states = NULL;
  editor_state.action_before_count = 0;
}

// Closes the action, turning the whole edit into one undo entry.
static void end_action(ui_handler_t *ui, input_snippet_t *snippet) {
  if (!editor_state.action_in_progress) return;
  editor_state.action_in_progress = false;
  editor_state.clean_status[0] = '\0';

  if (editor_state.action_before_states && editor_state.action_before_count == snippet->input_count) {
    // Every tick in the snippet is handed to the undo command; it stores the
    // before and after states so redo is exact.
    int *indices = malloc(sizeof(int) * (size_t)snippet->input_count);
    if (indices) {
      for (int i = 0; i < snippet->input_count; ++i) indices[i] = i;
      undo_command_t *command = create_edit_inputs_command(snippet, indices, snippet->input_count, editor_state.action_before_states,
                                                           snippet_window(snippet));
      if (command) undo_manager_register_command(&ui->undo_manager, command);
      free(indices);
    }
  }
  free(editor_state.action_before_states);
  editor_state.action_before_states = NULL;
  editor_state.action_before_count = 0;

  model_recalc_physics(&ui->timeline, snippet->start_tick);
  ui_mark_unsaved(ui);
}

static void draw_input_cleaning(ui_handler_t *ui, const ft_input_schema *schema, input_snippet_t *snippet, int track_index) {
  if (!igCollapsingHeader_TreeNodeFlags("Input cleaning", ImGuiTreeNodeFlags_DefaultOpen)) return;
  if (!ensure_clean_field_selection(schema)) {
    igTextDisabled("Could not allocate input cleaning choices.");
    return;
  }

  if (igButton("All##clean", (ImVec2){0.f, 0.f})) {
    for (uint32_t i = 0; i < schema->field_count; ++i)
      editor_state.clean_fields[i] = !(schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL);
  }
  igSameLine(0.f, 6.f);
  if (igButton("None##clean", (ImVec2){0.f, 0.f})) {
    for (uint32_t i = 0; i < schema->field_count; ++i) editor_state.clean_fields[i] = false;
  }

  int selected_count = 0;
  int visible_count = 0;
  for (uint32_t i = 0; i < schema->field_count; ++i)
    if (!(schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL)) ++visible_count;
  const int columns = visible_count < 3 ? visible_count : 3;
  if (columns > 0 && igBeginTable("CleanInputFields", columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0.f, 0.f}, 0.f)) {
    for (uint32_t i = 0; i < schema->field_count; ++i) {
      const ft_input_field *field = &schema->fields[i];
      if (field->flags & FT_INPUT_FLAG_INTERNAL) continue;
      igTableNextColumn();
      igPushID_Int((int)i);
      igCheckbox(field->display_name ? field->display_name : field->id, &editor_state.clean_fields[i]);
      igPopID();
      if (editor_state.clean_fields[i]) ++selected_count;
    }
    igEndTable();
  }

  timeline_state_t *timeline = &ui->timeline;
  const bool can_clean = snippet->is_active && !timeline->recording && selected_count > 0;
  if (!can_clean) igBeginDisabled(true);
  if (igButton("Clean selected", (ImVec2){0.f, 0.f})) {
    input_clean_result_t result;
    if (!begin_action(snippet)) {
      snprintf(editor_state.clean_status, sizeof(editor_state.clean_status), "Could not allocate an undo snapshot.");
    } else if (!input_cleaner_clean_snippet(ui, track_index, snippet, editor_state.clean_fields, schema->field_count, &result)) {
      cancel_action();
      snprintf(editor_state.clean_status, sizeof(editor_state.clean_status), "This game's run state cannot be compared.");
    } else if (result.changed_values == 0) {
      cancel_action();
      snprintf(editor_state.clean_status, sizeof(editor_state.clean_status), "Already clean (%llu simulations).",
               (unsigned long long)result.simulations);
    } else {
      end_action(ui, snippet);
      snprintf(editor_state.clean_status, sizeof(editor_state.clean_status),
               "Cleaned %d value%s in %d row%s (%d pass%s, %llu simulations).", result.changed_values,
               result.changed_values == 1 ? "" : "s", result.changed_rows, result.changed_rows == 1 ? "" : "s", result.passes,
               result.passes == 1 ? "" : "es", (unsigned long long)result.simulations);
    }
  }
  if (!can_clean) igEndDisabled();
  if (editor_state.clean_status[0]) igTextDisabled("%s", editor_state.clean_status);
}

// One cell of the matrix, drawn according to what kind of field it is.
static bool draw_field_cell(game_host_t *host, const ft_input_field *field, int field_index, input_record_t *record, int row) {
  bool changed = false;
  igPushID_Int(row * 64 + field_index);

  switch (field->kind) {
  case FT_INPUT_BOOL: {
    bool value = engine_input_get(host, record, field_index) != 0;
    if (igCheckbox("##v", &value)) {
      engine_input_set(host, record, field_index, value ? 1 : 0);
      changed = true;
    }
    break;
  }
  case FT_INPUT_ENUM: {
    int value = (int)engine_input_get(host, record, field_index) - field->min_value;
    if (field->enum_labels && field->enum_count > 0) {
      igPushItemWidth(-FLT_MIN);
      if (igCombo_Str_arr("##v", &value, field->enum_labels, (int)field->enum_count, -1)) {
        engine_input_set(host, record, field_index, field->min_value + value);
        changed = true;
      }
      igPopItemWidth();
    }
    break;
  }
  case FT_INPUT_INT: {
    int value = (int)engine_input_get(host, record, field_index);
    igPushItemWidth(-FLT_MIN);
    if (igDragInt("##v", &value, 1.f, field->min_value, field->max_value ? field->max_value : 0, "%d", 0)) {
      engine_input_set(host, record, field_index, value);
      changed = true;
    }
    igPopItemWidth();
    break;
  }
  case FT_INPUT_VEC2: {
    ft_vec2 value = engine_input_get_vec2(host, record, field_index);
    float components[2] = {value.x, value.y};
    igPushItemWidth(-FLT_MIN);
    if (igDragFloat2("##v", components, 1.f, field->min_float, field->max_float, "%.0f", 0)) {
      engine_input_set_vec2(host, record, field_index, (ft_vec2){components[0], components[1]});
      changed = true;
    }
    igPopItemWidth();
    break;
  }
  case FT_INPUT_FLOAT: {
    float value = engine_input_get_float(host, record, field_index);
    igPushItemWidth(-FLT_MIN);
    if (igDragFloat("##v", &value, 0.01f, field->min_float, field->max_float, "%.3f", 0)) {
      engine_input_set_float(host, record, field_index, value);
      changed = true;
    }
    igPopItemWidth();
    break;
  }
  default: break;
  }

  igPopID();
  return changed;
}

// Bulk edit: pick a field, pick a value, apply it to every selected row.
static void draw_bulk_edit(ui_handler_t *ui, game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet) {
  if (!igCollapsingHeader_TreeNodeFlags("Bulk edit", 0)) return;

  if (editor_state.bulk_field >= (int)schema->field_count) editor_state.bulk_field = 0;
  const ft_input_field *field = &schema->fields[editor_state.bulk_field];

  if (igBeginCombo("Field", field->display_name ? field->display_name : field->id, 0)) {
    for (uint32_t i = 0; i < schema->field_count; ++i) {
      if (schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL) continue;
      const bool selected = (int)i == editor_state.bulk_field;
      if (igSelectable_Bool(schema->fields[i].display_name ? schema->fields[i].display_name : schema->fields[i].id, selected, 0,
                            (ImVec2){0, 0})) {
        editor_state.bulk_field = (int)i;
        editor_state.bulk_value = 0;
        editor_state.bulk_float_value = 0.f;
        editor_state.bulk_vec2_value = (ft_vec2){0.f, 0.f};
      }
    }
    igEndCombo();
  }

  switch (field->kind) {
  case FT_INPUT_BOOL: {
    bool value = editor_state.bulk_value != 0;
    if (igCheckbox("Value", &value)) editor_state.bulk_value = value ? 1 : 0;
    break;
  }
  case FT_INPUT_ENUM: {
    int value = (int)editor_state.bulk_value - field->min_value;
    if (field->enum_labels && igCombo_Str_arr("Value", &value, field->enum_labels, (int)field->enum_count, -1))
      editor_state.bulk_value = field->min_value + value;
    break;
  }
  case FT_INPUT_FLOAT:
    igDragFloat("Value", &editor_state.bulk_float_value, 0.01f, field->min_float, field->max_float, "%.3f", 0);
    break;
  case FT_INPUT_VEC2: {
    float value[2] = {editor_state.bulk_vec2_value.x, editor_state.bulk_vec2_value.y};
    if (igDragFloat2("Value", value, 0.01f, field->min_float, field->max_float, "%.3f", 0))
      editor_state.bulk_vec2_value = (ft_vec2){value[0], value[1]};
    break;
  }
  default: {
    int value = (int)editor_state.bulk_value;
    if (igDragInt("Value", &value, 1.f, field->min_value, field->max_value, "%d", 0)) editor_state.bulk_value = value;
    break;
  }
  }

  if (igButton("Apply to selection", (ImVec2){0, 0})) {
    begin_action(snippet);
    for (int i = 0; i < snippet->input_count; ++i) {
      if (i >= editor_state.selected_capacity || !editor_state.selected_rows[i]) continue;
      input_record_t *record = &snippet_window(snippet)[i];
      if (field->kind == FT_INPUT_FLOAT)
        engine_input_set_float(host, record, editor_state.bulk_field, editor_state.bulk_float_value);
      else if (field->kind == FT_INPUT_VEC2)
        engine_input_set_vec2(host, record, editor_state.bulk_field, editor_state.bulk_vec2_value);
      else
        engine_input_set(host, record, editor_state.bulk_field, editor_state.bulk_value);
    }
    end_action(ui, snippet);
  }
}

void render_snippet_editor_panel(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_input_schema *schema = game_input_schema(host);

  if (igBegin("Snippet Editor", NULL, 0)) {
    if (!schema) {
      igTextDisabled("No game is active.");
      igEnd();
      return;
    }

    // The timeline's selection is the source of truth; active_snippet_id only
    // follows it. Reading that field directly showed an empty editor whenever a
    // snippet was picked normally.
    if (ts->selected_snippets.count == 0) {
      igTextDisabled("No snippet selected.");
      igEnd();
      return;
    }
    if (ts->selected_snippets.count > 1) {
      igText("Multiple snippets selected.");
      igTextDisabled("The snippet editor only works with a single selection.");
      igEnd();
      return;
    }
    ts->active_snippet_id = ts->selected_snippets.ids[0];

    int track_index = -1;
    input_snippet_t *snippet = model_find_snippet_by_id(ts, ts->active_snippet_id, &track_index);
    if (!snippet) {
      igTextDisabled("Selected snippet not found.");
      igEnd();
      return;
    }
    if (snippet->input_count > MAX_INPUTS) {
      igText("Snippet has too many inputs (%d) to edit.", snippet->input_count);
      igEnd();
      return;
    }

    if (editor_state.snippet_id != snippet->id) {
      editor_state.snippet_id = snippet->id;
      reset_editor_state();
    }
    ensure_selection_capacity(snippet->input_count);

    igText("Track %d, ticks %d..%d", track_index, snippet->start_tick, snippet->end_tick);
    igSameLine(0, 12.f);
    if (igButton("Select all", (ImVec2){0.f, 0.f})) {
      for (int i = 0; i < snippet->input_count && i < editor_state.selected_capacity; ++i) editor_state.selected_rows[i] = true;
    }
    igSameLine(0, 6.f);
    if (igButton("Select none", (ImVec2){0.f, 0.f})) clear_row_selection();

    draw_input_cleaning(ui, schema, snippet, track_index);
    draw_bulk_edit(ui, host, schema, snippet);
    igSeparator();

    // One column per non-internal field, plus the tick number.
    int columns = 1;
    for (uint32_t i = 0; i < schema->field_count; ++i)
      if (!(schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL)) ++columns;

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_Resizable;
    if (igBeginTable("SnippetInputs", columns, flags, (ImVec2){0, 0}, 0.f)) {
      igTableSetupScrollFreeze(1, 1);
      igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 64.f, 0);
      for (uint32_t i = 0; i < schema->field_count; ++i) {
        if (schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL) continue;
        igTableSetupColumn(schema->fields[i].display_name ? schema->fields[i].display_name : schema->fields[i].id,
                           ImGuiTableColumnFlags_WidthFixed, schema->fields[i].kind == FT_INPUT_VEC2 ? 140.f : 90.f, 0);
      }
      igTableHeadersRow();

      // Only the visible rows are built, so a snippet of any length costs the
      // same to draw.
      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, snippet->input_count, -1.f);
      while (ImGuiListClipper_Step(clipper)) {
        for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
          igTableNextRow(0, 0.f);
          igTableSetColumnIndex(0);

          char label[32];
          snprintf(label, sizeof(label), "%d", snippet->start_tick + row);
          const bool selected = row < editor_state.selected_capacity && editor_state.selected_rows[row];
          if (igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, (ImVec2){0, 0})) {
            const ImGuiIO *io = igGetIO_Nil();
            if (io->KeyShift && editor_state.last_clicked_row >= 0) {
              const int from = row < editor_state.last_clicked_row ? row : editor_state.last_clicked_row;
              const int to = row < editor_state.last_clicked_row ? editor_state.last_clicked_row : row;
              for (int i = from; i <= to && i < editor_state.selected_capacity; ++i) editor_state.selected_rows[i] = true;
            } else {
              if (!io->KeyCtrl) memset(editor_state.selected_rows, 0, (size_t)editor_state.selected_capacity * sizeof(bool));
              if (row < editor_state.selected_capacity) editor_state.selected_rows[row] = !selected || io->KeyCtrl;
              editor_state.last_clicked_row = row;
            }
          }

          bool row_changed = false;
          int column = 1;
          for (uint32_t i = 0; i < schema->field_count; ++i) {
            if (schema->fields[i].flags & FT_INPUT_FLAG_INTERNAL) continue;
            igTableSetColumnIndex(column++);
            if (draw_field_cell(host, &schema->fields[i], (int)i, &snippet_window(snippet)[row], row)) row_changed = true;
          }

          if (row_changed) {
            begin_action(snippet);
            // Editing continues while the widget is held; the action closes on
            // release so a drag is one undo step.
            if (!igIsAnyItemActive()) end_action(ui, snippet);
          }
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
      igEndTable();
    }

    if (editor_state.action_in_progress && !igIsAnyItemActive()) end_action(ui, snippet);
  }
  igEnd();
}
