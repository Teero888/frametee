#include "input_effects_editor.h"

#include "input_effects.h"
#include <float.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>
#include <user_interface/timeline/timeline_commands.h>
#include <user_interface/timeline/timeline_interaction.h>
#include <user_interface/timeline/timeline_model.h>

static input_snippet_t *selected_snippet(ui_handler_t *ui, int *out_track) {
  timeline_state_t *timeline = &ui->timeline;
  if (timeline->selected_snippets.count != 1) return NULL;
  const int id = timeline->selected_snippets.ids[0];
  input_snippet_t *snippet = model_find_snippet_by_id(timeline, id, out_track);
  if (snippet) ui->effects_snippet_id = snippet->id;
  return snippet;
}

void input_effects_editor_open(ui_handler_t *ui, int snippet_id) {
  if (!ui) return;
  if (model_find_snippet_by_id(&ui->timeline, snippet_id, NULL)) {
    interaction_clear_selection(&ui->timeline);
    interaction_add_snippet_to_selection(&ui->timeline, snippet_id);
    ui->timeline.active_snippet_id = snippet_id;
    ui->effects_snippet_id = snippet_id;
  }
  ui->show_effects_window = true;
  ui->focus_effects_window = true;
}

static bool grow_effect_stack(input_snippet_t *snippet) {
  if (snippet->effect_count >= MAX_SNIPPET_INPUT_EFFECTS) return false;
  if (snippet->effect_count < snippet->effect_capacity) return true;
  int capacity = snippet->effect_capacity > 0 ? snippet->effect_capacity * 2 : 4;
  if (capacity > MAX_SNIPPET_INPUT_EFFECTS) capacity = MAX_SNIPPET_INPUT_EFFECTS;
  input_effect_t *grown = realloc(snippet->effects, sizeof(*grown) * (size_t)capacity);
  if (!grown) return false;
  memset(grown + snippet->effect_capacity, 0, sizeof(*grown) * (size_t)(capacity - snippet->effect_capacity));
  snippet->effects = grown;
  snippet->effect_capacity = capacity;
  return true;
}

static void finish_change(ui_handler_t *ui, input_snippet_t *snippet, input_effect_t *before, int before_count,
                          const char *description) {
  input_effects_refresh(&ui->timeline);
  model_reset_physics_cache(&ui->timeline);
  undo_command_t *command = commands_create_input_effects_change(ui, snippet->id, before, before_count, description);
  if (command) undo_manager_register_command(&ui->undo_manager, command);
  input_effect_stack_destroy(before, before_count);
  ui_mark_unsaved(ui);
}

static bool add_effect(ui_handler_t *ui, input_snippet_t *snippet, unsigned type_index) {
  if (!ui || !snippet || snippet->effect_count >= MAX_SNIPPET_INPUT_EFFECTS) return false;
  input_effect_t *before = input_effect_stack_copy(snippet->effects, snippet->effect_count);
  const int before_count = snippet->effect_count;
  if ((before_count > 0 && !before) || !grow_effect_stack(snippet)) {
    input_effect_stack_destroy(before, before_count);
    return false;
  }
  input_effect_t effect;
  if (!input_effect_init(&ui->gfx_handler->game_host, type_index, &effect)) {
    input_effect_stack_destroy(before, before_count);
    return false;
  }
  snippet->effects[snippet->effect_count++] = effect;
  finish_change(ui, snippet, before, before_count, "Add Input Effect");
  input_effects_editor_open(ui, snippet->id);
  return true;
}

static void render_add_menu(ui_handler_t *ui, input_snippet_t *snippet) {
  game_host_t *host = &ui->gfx_handler->game_host;
  const unsigned count = gh_input_effect_count(host);
  if (count == 0) {
    igTextDisabled("This game does not provide input effects.");
    return;
  }
  for (unsigned i = 0; i < count; ++i) {
    const ft_input_effect_desc *desc = gh_input_effect_desc(host, i);
    if (!desc) continue;
    if (igMenuItem_Bool(desc->display_name, NULL, false, snippet && snippet->effect_count < MAX_SNIPPET_INPUT_EFFECTS))
      add_effect(ui, snippet, i);
  }
}

void input_effects_editor_render_menu(ui_handler_t *ui) {
  if (!igBeginMenu("Effects", true)) return;
  int track = -1;
  input_snippet_t *snippet = selected_snippet(ui, &track);
  (void)track;
  if (igMenuItem_Bool("Open Effects", NULL, false, snippet != NULL)) input_effects_editor_open(ui, snippet->id);
  if (igBeginMenu("Add Effect", !ui->timeline.recording && snippet != NULL &&
                                    snippet->effect_count < MAX_SNIPPET_INPUT_EFFECTS)) {
    render_add_menu(ui, snippet);
    igEndMenu();
  }
  igEndMenu();
}

static bool full_button(const char *label, float width) { return igButton(label, (ImVec2){width, 0.f}); }

static void render_effect(ui_handler_t *ui, input_snippet_t *snippet, int track_index, int effect_index) {
  input_effect_t *effect = &snippet->effects[effect_index];
  game_host_t *host = &ui->gfx_handler->game_host;
  int type_index = -1;
  const ft_input_effect_desc *desc = input_effect_descriptor(host, effect, &type_index);
  igPushID_Int(effect_index);

  char header[160];
  snprintf(header, sizeof(header), "%d. %s%s", effect_index + 1,
           desc ? desc->display_name : effect->type_id, effect->enabled ? "" : " (disabled)");
  const bool open = igCollapsingHeader_TreeNodeFlags(header, ImGuiTreeNodeFlags_DefaultOpen);
  if (!open) {
    igPopID();
    return;
  }

  bool enabled = effect->enabled;
  if (igCheckbox("Enabled", &enabled)) {
    input_effect_t *before = input_effect_stack_copy(snippet->effects, snippet->effect_count);
    if (before) {
      effect->enabled = enabled;
      finish_change(ui, snippet, before, snippet->effect_count,
                    enabled ? "Enable Input Effect" : "Disable Input Effect");
    }
  }
  if (!desc) {
    igTextDisabled("This effect is unavailable in the active game module.");
  } else {
    const int group_index = model_track_group_index(&ui->timeline, track_index);
    ft_input_effect_ui_frame frame = {
        .struct_size = sizeof(frame),
        .track_index = track_index,
        .world_index = group_index,
        .player = model_group_local_track_index(&ui->timeline, track_index),
        .start_tick = snippet->start_tick,
        .end_tick = snippet->end_tick,
    };
    input_effect_t *before = input_effect_stack_copy(snippet->effects, snippet->effect_count);
    const bool changed = gh_input_effect_ui(host, (unsigned)type_index, &frame, effect->parameters,
                                            effect->parameter_size, effect->runtime, effect->runtime_size);
    if (changed && before)
      finish_change(ui, snippet, before, snippet->effect_count, "Configure Input Effect");
    else
      input_effect_stack_destroy(before, snippet->effect_count);
    if (effect->enabled && !effect->runtime_ok) igTextDisabled("The effect could not be evaluated.");
  }

  const float spacing = igGetStyle()->ItemSpacing.x;
  const float width = (igGetContentRegionAvail().x - spacing * 2.f) / 3.f;
  if (effect_index == 0) igBeginDisabled(true);
  const bool move_up = full_button("Move up", width);
  if (effect_index == 0) igEndDisabled();
  igSameLine(0.f, spacing);
  if (effect_index + 1 == snippet->effect_count) igBeginDisabled(true);
  const bool move_down = full_button("Move down", width);
  if (effect_index + 1 == snippet->effect_count) igEndDisabled();
  igSameLine(0.f, spacing);
  const bool remove = full_button("Remove", width);

  if (move_up || move_down || remove) {
    input_effect_t *before = input_effect_stack_copy(snippet->effects, snippet->effect_count);
    if (before) {
      const int before_count = snippet->effect_count;
      if (remove) {
        input_effect_destroy(&snippet->effects[effect_index]);
        memmove(&snippet->effects[effect_index], &snippet->effects[effect_index + 1],
                sizeof(*snippet->effects) * (size_t)(snippet->effect_count - effect_index - 1));
        --snippet->effect_count;
        memset(&snippet->effects[snippet->effect_count], 0, sizeof(*snippet->effects));
      } else {
        const int other = move_up ? effect_index - 1 : effect_index + 1;
        const input_effect_t temporary = snippet->effects[effect_index];
        snippet->effects[effect_index] = snippet->effects[other];
        snippet->effects[other] = temporary;
      }
      igPopID();
      finish_change(ui, snippet, before, before_count, remove ? "Remove Input Effect" : "Reorder Input Effects");
      return;
    }
  }
  igSpacing();
  igPopID();
}

void input_effects_editor_render(ui_handler_t *ui) {
  if (!ui->show_effects_window) return;
  if (ui->focus_effects_window) {
    igSetNextWindowFocus();
    ui->focus_effects_window = false;
  }
  if (igBegin("Effects", &ui->show_effects_window, 0)) {
    int track_index = -1;
    input_snippet_t *snippet = selected_snippet(ui, &track_index);
    if (!snippet) {
      igTextDisabled("Select one snippet to edit its effects.");
      igEnd();
      return;
    }
    igText("Track %d, ticks %d..%d", track_index, snippet->start_tick, snippet->end_tick);
    igTextDisabled("Effects run from top to bottom and never change the authored input.");
    igSeparator();

    input_effects_ensure(&ui->timeline);
    if (ui->timeline.recording) {
      igTextDisabled("Stop recording to edit effects.");
      igBeginDisabled(true);
    }

    if (snippet->effect_count >= MAX_SNIPPET_INPUT_EFFECTS) igBeginDisabled(true);
    if (igButton("Add effect", (ImVec2){-FLT_MIN, 0.f})) igOpenPopup_Str("AddInputEffect", ImGuiPopupFlags_None);
    if (snippet->effect_count >= MAX_SNIPPET_INPUT_EFFECTS) igEndDisabled();
    if (igBeginPopup("AddInputEffect", ImGuiWindowFlags_AlwaysAutoResize)) {
      render_add_menu(ui, snippet);
      igEndPopup();
    }
    igSeparator();

    if (snippet->effect_count == 0) igTextDisabled("No effects applied.");
    for (int i = 0; i < snippet->effect_count; ++i) {
      render_effect(ui, snippet, track_index, i);
      if (i >= snippet->effect_count) break;
    }
    if (ui->timeline.recording) igEndDisabled();
  }
  igEnd();
}
