#include "prediction.h"

#include <engine/game_host.h>
#include <engine/input_record.h>
#include <frametee/icons.h>
#include <limits.h>
#include <math.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>
#include <user_interface/timeline/timeline_interaction.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline/timeline_types.h>
#include <user_interface/user_interface.h>

enum { MAX_PREDICTION_SEGMENTS = 250000 };
static const float PREDICTION_Z = 50.f;

// Where a player stands in a 3D world. The player view the 2D path reads only
// carries a plane's worth of it, so a game with a volume is asked for the
// property the engine's own camera follows instead.
static bool player_position3(game_host_t *host, const ft_world *world, int player, vec3 out) {
  ft_value value;
  if (!gh_entity_prop_get(host, world, FT_ENTITY_CLASS_PLAYER, player, 0, &value)) return false;
  if (value.kind != FT_VALUE_VEC3) return false;
  out[0] = value.as.v3.x;
  out[1] = value.as.v3.y;
  out[2] = value.as.v3.z;
  return true;
}

static const float s_line_colors[][4] = {
    {0.25f, 0.72f, 1.00f, 0.78f}, {1.00f, 0.48f, 0.18f, 0.82f}, {0.35f, 0.95f, 0.48f, 0.82f},
    {0.95f, 0.35f, 0.72f, 0.82f}, {0.75f, 0.48f, 1.00f, 0.82f}, {1.00f, 0.85f, 0.25f, 0.82f},
    {0.20f, 0.90f, 0.85f, 0.82f}, {1.00f, 0.35f, 0.35f, 0.82f},
};

void prediction_settings_default(prediction_settings_t *settings) {
  if (!settings) return;
  memset(settings, 0, sizeof(*settings));
  settings->enabled = true;
  settings->length = 100;
  settings->thickness = 0.05f;
  settings->line_count = 1;
  prediction_line_t *line = &settings->lines[0];
  snprintf(line->name, sizeof(line->name), "Timeline inputs");
  memcpy(line->color, s_line_colors[0], sizeof(line->color));
  line->enabled = true;
  line->use_timeline_inputs = true;
}

void prediction_group_cleanup(timeline_state_t *timeline, timeline_group_t *group) {
  if (!timeline || !group || !timeline->ui || !timeline->ui->gfx_handler) return;
  game_host_t *host = &timeline->ui->gfx_handler->game_host;
  for (int i = 0; i < MAX_PREDICTION_LINES; ++i) {
    gh_world_destroy(host, group->prediction_worlds[i]);
    group->prediction_worlds[i] = NULL;
  }
}

static void apply_controls(game_host_t *host, input_record_t *input, uint64_t controls) {
  const ft_input_schema *schema = game_input_schema(host);
  if (!schema || controls == 0) return;
  bool reset[256] = {false};
  const uint32_t count = schema->control_count < 64 ? schema->control_count : 64;

  // A selected control owns its field for this alternative. Reset each owned
  // field once, then combine controls such as left/right through FT_CONTROL_ADD.
  for (uint32_t control_index = 0; control_index < count; ++control_index) {
    if ((controls & (UINT64_C(1) << control_index)) == 0) continue;
    const ft_input_control *control = &schema->controls[control_index];
    if (control->field >= schema->field_count || control->field >= 256 || reset[control->field]) continue;
    const ft_input_field *field = &schema->fields[control->field];
    if (field->kind == FT_INPUT_FLOAT)
      engine_input_set_float(host, input, (int)control->field, field->default_float);
    else
      engine_input_set(host, input, (int)control->field, field->default_value);
    reset[control->field] = true;
  }

  for (uint32_t control_index = 0; control_index < count; ++control_index) {
    if ((controls & (UINT64_C(1) << control_index)) == 0) continue;
    const ft_input_control *control = &schema->controls[control_index];
    if (control->field >= schema->field_count) continue;
    const ft_input_field *field = &schema->fields[control->field];
    if (field->kind == FT_INPUT_FLOAT) {
      float value = (float)control->value;
      if (control->flags & FT_CONTROL_ADD) value += engine_input_get_float(host, input, (int)control->field);
      engine_input_set_float(host, input, (int)control->field, value);
    } else {
      int64_t value = control->value;
      if (control->flags & FT_CONTROL_ADD) value += engine_input_get(host, input, (int)control->field);
      engine_input_set(host, input, (int)control->field, value);
    }
  }
}

static ft_world *prediction_world(timeline_state_t *timeline, timeline_group_t *group, int line_index, const ft_world *source) {
  game_host_t *host = &timeline->ui->gfx_handler->game_host;
  const int players = gh_world_player_count(host, source);
  ft_world *world = group->prediction_worlds[line_index];
  if (world && gh_world_player_count(host, world) != players) {
    gh_world_destroy(host, world);
    world = NULL;
  }
  if (!world) {
    world = gh_world_create(host, timeline->ui->gfx_handler->level, players, -1);
    group->prediction_worlds[line_index] = world;
  }
  if (world) gh_world_copy(host, world, source);
  return world;
}

static int selected_player_count(const timeline_state_t *timeline, int group_index, int players) {
  int selected = 0;
  for (int player = 0; player < players; ++player) {
    const int track = model_group_track_index(timeline, group_index, player);
    if (track >= 0 && timeline->player_tracks[track].prediction_enabled) ++selected;
  }
  return selected;
}

void prediction_render_group(ui_handler_t *ui, int group_index, const ft_world *previous, const ft_world *current, float alpha) {
  if (!ui || !current) return;
  timeline_state_t *timeline = &ui->timeline;
  prediction_settings_t *settings = &timeline->prediction;
  if (!settings->enabled || settings->length <= 0 || group_index < 0 || group_index >= timeline->group_count) return;
  if (timeline->is_playing || timeline->is_reversing || timeline->recording) return;
  timeline_group_t *group = timeline->groups[group_index];
  if (!group->prediction_enabled) return;

  game_host_t *host = &ui->gfx_handler->game_host;
  const int players = gh_world_player_count(host, current);
  const int selected = selected_player_count(timeline, group_index, players);
  const size_t input_size = game_input_size(host);
  if (players <= 0 || selected <= 0 || input_size == 0) return;

  int length = settings->length;
  if (length > 2000) length = 2000;
  const int safe_length = MAX_PREDICTION_SEGMENTS / selected;
  if (length > safe_length) length = safe_length;
  if (length <= 0) return;

  // A 3D game's prediction is a line through the world, not a stripe on a map.
  const bool is_3d = game_is_3d(host);
  // The 2D thickness is a fraction of a normalised playfield; in a volume it is
  // metres, and a line thinner than a wheel is one nobody can see.
  const float thickness3 = fmaxf(settings->thickness * 4.f, 0.12f);

  uint8_t *packed_inputs = calloc((size_t)players, input_size);
  input_record_t *held_inputs = calloc((size_t)players, sizeof(*held_inputs));
  ft_vec2 *positions = calloc((size_t)players, sizeof(*positions));
  vec3 *positions3 = calloc((size_t)players, sizeof(*positions3));
  bool *have_position = calloc((size_t)players, sizeof(*have_position));
  line_segment_t *segments = malloc(sizeof(*segments) * (size_t)length * (size_t)selected);
  if (!packed_inputs || !held_inputs || !positions || !positions3 || !have_position || !segments) goto cleanup;

  alpha = fmaxf(0.f, fminf(alpha, 1.f));
  for (int player = 0; player < players; ++player) {
    const int track = model_group_track_index(timeline, group_index, player);
    if (track < 0) {
      engine_input_default(host, &held_inputs[player]);
      continue;
    }
    held_inputs[player] = interaction_predict_input(ui, current, track);
  }

  for (int line_index = 0; line_index < settings->line_count && line_index < MAX_PREDICTION_LINES; ++line_index) {
    const prediction_line_t *line = &settings->lines[line_index];
    if (!line->enabled) continue;
    ft_world *world = prediction_world(timeline, group, line_index, current);
    if (!world) continue;

    memset(have_position, 0, sizeof(*have_position) * (size_t)players);
    for (int player = 0; player < players; ++player) {
      const int track = model_group_track_index(timeline, group_index, player);
      if (track < 0 || !timeline->player_tracks[track].prediction_enabled) continue;
      if (is_3d) {
        vec3 current_pos, previous_pos;
        if (!player_position3(host, current, player, current_pos)) continue;
        glm_vec3_copy(current_pos, positions3[player]);
        if (previous && player_position3(host, previous, player, previous_pos))
          glm_vec3_lerp(previous_pos, current_pos, alpha, positions3[player]);
        have_position[player] = true;
        continue;
      }
      ft_player_view current_view, previous_view;
      if (!gh_world_player_view(host, current, player, &current_view)) continue;
      positions[player] = current_view.position;
      if (previous && gh_world_player_view(host, previous, player, &previous_view)) {
        positions[player].x = previous_view.position.x + (current_view.position.x - previous_view.position.x) * alpha;
        positions[player].y = previous_view.position.y + (current_view.position.y - previous_view.position.y) * alpha;
      }
      have_position[player] = true;
    }

    uint32_t segment_count = 0;
    for (int step = 0; step < length; ++step) {
      for (int player = 0; player < players; ++player) {
        const int track = model_group_track_index(timeline, group_index, player);
        input_record_t input;
        if (track >= 0) {
          if (!line->use_timeline_inputs && timeline->player_tracks[track].prediction_enabled) {
            input = held_inputs[player];
            apply_controls(host, &input, line->controls);
          } else {
            input = interaction_predict_input(ui, world, track);
          }
        } else {
          engine_input_default(host, &input);
        }
        memcpy(packed_inputs + (size_t)player * input_size, input.bytes, input_size);
      }
      gh_world_step(host, world, packed_inputs, (unsigned)players);

      for (int player = 0; player < players; ++player) {
        const int track = model_group_track_index(timeline, group_index, player);
        if (track < 0 || !timeline->player_tracks[track].prediction_enabled) continue;

        if (is_3d) {
          // A 3D world's players move in a volume, and the two-dimensional view
          // the 2D path draws from is a shadow of that with the height thrown
          // away. Drawn flat on a plane, the line ends up nowhere near the run
          // it is predicting, which is what made these look broken in 3D.
          vec3 next;
          if (!player_position3(host, world, player, next)) {
            have_position[player] = false;
            continue;
          }
          if (have_position[player]) {
            vec4 color = {line->color[0], line->color[1], line->color[2], line->color[3]};
            renderer_submit_line3(ui->gfx_handler, positions3[player], next, color, thickness3);
          }
          glm_vec3_copy(next, positions3[player]);
          have_position[player] = true;
          continue;
        }

        ft_player_view view;
        if (!gh_world_player_view(host, world, player, &view)) {
          have_position[player] = false;
          continue;
        }
        if (have_position[player] && segment_count < (uint32_t)(length * selected)) {
          line_segment_t *segment = &segments[segment_count++];
          segment->p1[0] = positions[player].x;
          segment->p1[1] = positions[player].y;
          segment->p2[0] = view.position.x;
          segment->p2[1] = view.position.y;
          memcpy(segment->color, line->color, sizeof(segment->color));
          segment->thickness = settings->thickness;
        }
        positions[player] = view.position;
        have_position[player] = true;
      }
    }
    if (!is_3d)
      renderer_submit_line_batch(ui->gfx_handler, PREDICTION_Z + (float)line_index * 0.001f, segments, segment_count);
  }

cleanup:
  free(segments);
  free(have_position);
  free(positions3);
  free(positions);
  free(held_inputs);
  free(packed_inputs);
}

static void add_prediction_line(timeline_state_t *timeline) {
  prediction_settings_t *settings = &timeline->prediction;
  if (settings->line_count >= MAX_PREDICTION_LINES) return;
  const int index = settings->line_count++;
  prediction_line_t *line = &settings->lines[index];
  memset(line, 0, sizeof(*line));
  snprintf(line->name, sizeof(line->name), "Alternative %d", index);
  memcpy(line->color, s_line_colors[index], sizeof(line->color));
  line->enabled = true;
  timeline_mark_unsaved(timeline);
}

static void remove_prediction_line(timeline_state_t *timeline, int index) {
  prediction_settings_t *settings = &timeline->prediction;
  if (index <= 0 || index >= settings->line_count) return;
  game_host_t *host = &timeline->ui->gfx_handler->game_host;
  for (int group_index = 0; group_index < timeline->group_count; ++group_index) {
    timeline_group_t *group = timeline->groups[group_index];
    gh_world_destroy(host, group->prediction_worlds[index]);
    memmove(&group->prediction_worlds[index], &group->prediction_worlds[index + 1],
            sizeof(group->prediction_worlds[0]) * (size_t)(MAX_PREDICTION_LINES - index - 1));
    group->prediction_worlds[MAX_PREDICTION_LINES - 1] = NULL;
  }
  memmove(&settings->lines[index], &settings->lines[index + 1],
          sizeof(settings->lines[0]) * (size_t)(settings->line_count - index - 1));
  memset(&settings->lines[--settings->line_count], 0, sizeof(settings->lines[0]));
}

void prediction_render_menu(timeline_state_t *timeline) {
  if (!timeline || !timeline->ui) return;
  prediction_settings_t *settings = &timeline->prediction;
  const float dpi = gfx_get_ui_scale();

  if (settings->enabled) {
    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){0.18f, 0.48f, 0.75f, 1.f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){0.24f, 0.58f, 0.88f, 1.f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){0.28f, 0.65f, 0.95f, 1.f});
  }
  const bool open = ui_icon_button(timeline->ui, ICON_FA_ROUTE, (ImVec2){30.f * dpi, 0.f});
  if (settings->enabled) igPopStyleColor(3);
  if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("Prediction lines");
  if (open) igOpenPopup_Str("PredictionMenu", ImGuiPopupFlags_None);

  igSetNextWindowSizeConstraints((ImVec2){360.f * dpi, 0.f}, (ImVec2){620.f * dpi, 720.f * dpi}, NULL, NULL);
  if (!igBeginPopup("PredictionMenu", ImGuiWindowFlags_AlwaysAutoResize)) return;

  bool changed = false;
  changed |= igCheckbox("Show prediction", &settings->enabled);
  igSetNextItemWidth(180.f * dpi);
  changed |= igSliderInt("Ticks ahead", &settings->length, 1, 2000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSetNextItemWidth(180.f * dpi);
  changed |= igSliderFloat("Line width", &settings->thickness, 0.01f, 0.30f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

  igSeparatorText("Lines");
  int remove_line = -1;
  const ft_input_schema *schema = game_input_schema(&timeline->ui->gfx_handler->game_host);
  for (int line_index = 0; line_index < settings->line_count; ++line_index) {
    prediction_line_t *line = &settings->lines[line_index];
    igPushID_Int(line_index);
    changed |= igCheckbox("##enabled", &line->enabled);
    igSameLine(0.f, 6.f * dpi);
    igSetNextItemWidth(180.f * dpi);
    changed |= igInputText("##name", line->name, sizeof(line->name), 0, NULL, NULL);
    igSameLine(0.f, 6.f * dpi);
    changed |= igColorEdit4("##color", line->color,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
    if (!line->use_timeline_inputs) {
      igSameLine(0.f, 6.f * dpi);
      if (igSmallButton(ICON_FA_TRASH)) remove_line = line_index;
      if (schema && igTreeNode_Str("Inputs")) {
        for (uint32_t control = 0; control < schema->control_count && control < 64; ++control) {
          igPushID_Int((int)control);
          bool selected = (line->controls & (UINT64_C(1) << control)) != 0;
          const char *label = schema->controls[control].display_name ? schema->controls[control].display_name
                                                                    : schema->controls[control].id;
          if (igCheckbox(label, &selected)) {
            if (selected) line->controls |= UINT64_C(1) << control;
            else line->controls &= ~(UINT64_C(1) << control);
            changed = true;
          }
          if (schema->controls[control].description && igIsItemHovered(ImGuiHoveredFlags_None))
            igSetTooltip("%s", schema->controls[control].description);
          igPopID();
        }
        igTreePop();
      }
    } else {
      igSameLine(0.f, 6.f * dpi);
      igTextDisabled("authored");
    }
    igPopID();
  }
  if (remove_line > 0) {
    remove_prediction_line(timeline, remove_line);
    changed = true;
  }
  if (settings->line_count < MAX_PREDICTION_LINES && igButton(ICON_FA_PLUS " Add input line", (ImVec2){0.f, 0.f}))
    add_prediction_line(timeline);

  igSeparatorText("Groups and tracks");
  if (igButton("All", (ImVec2){0.f, 0.f})) {
    for (int group = 0; group < timeline->group_count; ++group) timeline->groups[group]->prediction_enabled = true;
    for (int track = 0; track < timeline->player_track_count; ++track) timeline->player_tracks[track].prediction_enabled = true;
    changed = true;
  }
  igSameLine(0.f, 6.f * dpi);
  if (igButton("None", (ImVec2){0.f, 0.f})) {
    for (int group = 0; group < timeline->group_count; ++group) timeline->groups[group]->prediction_enabled = false;
    for (int track = 0; track < timeline->player_track_count; ++track) timeline->player_tracks[track].prediction_enabled = false;
    changed = true;
  }
  for (int group_index = 0; group_index < timeline->group_count; ++group_index) {
    timeline_group_t *group = timeline->groups[group_index];
    igPushID_Int(1000 + group_index);
    changed |= igCheckbox(group->name, &group->prediction_enabled);
    igIndent(18.f * dpi);
    for (int track_index = 0; track_index < timeline->player_track_count; ++track_index) {
      player_track_t *track = &timeline->player_tracks[track_index];
      if (track->group_index != group_index) continue;
      const char *label = track->name[0] ? track->name : track->player_info.name;
      igPushID_Int(track_index);
      changed |= igCheckbox(label && *label ? label : "Player", &track->prediction_enabled);
      igPopID();
    }
    igUnindent(18.f * dpi);
    igPopID();
  }

  if (changed) timeline_mark_unsaved(timeline);
  igEndPopup();
}
