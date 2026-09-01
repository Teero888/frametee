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
#include <system/config.h>
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

void prediction_line_default(prediction_line_t *line, int index) {
  if (!line) return;
  memset(line, 0, sizeof(*line));
  if (index == 0)
    snprintf(line->name, sizeof(line->name), "Timeline inputs");
  else
    snprintf(line->name, sizeof(line->name), "Alternative %d", index);
  const int color = index >= 0 ? index % (int)(sizeof(s_line_colors) / sizeof(s_line_colors[0])) : 0;
  memcpy(line->color, s_line_colors[color], sizeof(line->color));
  line->enabled = true;
  line->use_timeline_inputs = index == 0;
}

void prediction_settings_default(prediction_settings_t *settings) {
  if (!settings) return;
  memset(settings, 0, sizeof(*settings));
  settings->enabled = true;
  settings->length = 100;
  settings->thickness = 0.05f;
  settings->line_count = 1;
  prediction_line_default(&settings->lines[0], 0);
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

typedef struct resolved_color_rule_t {
  const prediction_color_rule_t *rule;
  uint32_t property;
  ft_value_kind kind;
  bool valid;
} resolved_color_rule_t;

typedef struct color_rule_runtime_t {
  double previous;
  bool have_previous;
  bool condition;
} color_rule_runtime_t;

static bool color_rule_property_supported(ft_value_kind kind) {
  return kind == FT_VALUE_BOOL || kind == FT_VALUE_INT || kind == FT_VALUE_FLOAT || kind == FT_VALUE_VEC2 ||
         kind == FT_VALUE_VEC3;
}

static const ft_prop_desc *first_color_rule_property(const ft_entity_class *player_class) {
  if (!player_class) return NULL;
  for (uint32_t i = 0; i < player_class->prop_count; ++i) {
    const ft_prop_desc *property = &player_class->props[i];
    if (property->id && color_rule_property_supported(property->kind)) return property;
  }
  return NULL;
}

static int player_property_find(const ft_entity_class *player_class, const char *id) {
  if (!player_class || !id || !*id) return -1;
  for (uint32_t property = 0; property < player_class->prop_count; ++property)
    if (player_class->props[property].id && strcmp(player_class->props[property].id, id) == 0) return (int)property;
  return -1;
}

static bool color_rule_value(game_host_t *host, const ft_world *world, int player,
                             const resolved_color_rule_t *resolved, double *out) {
  if (!resolved || !resolved->valid || !out) return false;
  ft_value value;
  if (!gh_entity_prop_get(host, world, FT_ENTITY_CLASS_PLAYER, player, resolved->property, &value) ||
      value.kind != resolved->kind)
    return false;

  switch (value.kind) {
  case FT_VALUE_BOOL: *out = value.as.b ? 1.0 : 0.0; break;
  case FT_VALUE_INT: *out = (double)value.as.i; break;
  case FT_VALUE_FLOAT: *out = value.as.f; break;
  case FT_VALUE_VEC2:
    if (resolved->rule->component == PREDICTION_COMPONENT_X) *out = value.as.v.x;
    else if (resolved->rule->component == PREDICTION_COMPONENT_Y) *out = value.as.v.y;
    else if (resolved->rule->component == PREDICTION_COMPONENT_MAGNITUDE)
      *out = hypot((double)value.as.v.x, (double)value.as.v.y);
    else
      return false;
    break;
  case FT_VALUE_VEC3:
    if (resolved->rule->component == PREDICTION_COMPONENT_X) *out = value.as.v3.x;
    else if (resolved->rule->component == PREDICTION_COMPONENT_Y) *out = value.as.v3.y;
    else if (resolved->rule->component == PREDICTION_COMPONENT_Z) *out = value.as.v3.z;
    else if (resolved->rule->component == PREDICTION_COMPONENT_MAGNITUDE)
      *out = sqrt((double)value.as.v3.x * value.as.v3.x + (double)value.as.v3.y * value.as.v3.y +
                  (double)value.as.v3.z * value.as.v3.z);
    else
      return false;
    break;
  default: return false;
  }
  return isfinite(*out);
}

static bool color_rule_equal(const resolved_color_rule_t *resolved, double a, double b) {
  if (resolved->kind == FT_VALUE_BOOL || resolved->kind == FT_VALUE_INT) return a == b;
  const double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
  return fabs(a - b) <= scale * 1e-6;
}

static bool color_rule_condition(const resolved_color_rule_t *resolved, double value) {
  switch (resolved->rule->comparison) {
  case PREDICTION_COMPARE_EQUAL: return color_rule_equal(resolved, value, resolved->rule->target);
  case PREDICTION_COMPARE_LESS: return value < resolved->rule->target;
  case PREDICTION_COMPARE_GREATER: return value > resolved->rule->target;
  case PREDICTION_COMPARE_CHANGED: return false;
  }
  return false;
}

static void resolve_color_rules(game_host_t *host, const prediction_line_t *line,
                                resolved_color_rule_t resolved[MAX_PREDICTION_COLOR_RULES]) {
  memset(resolved, 0, sizeof(*resolved) * MAX_PREDICTION_COLOR_RULES);
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  for (int i = 0; i < line->color_rule_count && i < MAX_PREDICTION_COLOR_RULES; ++i) {
    const prediction_color_rule_t *rule = &line->color_rules[i];
    resolved[i].rule = rule;
    if (!rule->enabled) continue;
    const int property = player_property_find(player_class, rule->property_id);
    if (property < 0) continue;
    const ft_prop_desc *desc = &player_class->props[property];
    if (!color_rule_property_supported(desc->kind)) continue;
    const bool scalar = desc->kind == FT_VALUE_BOOL || desc->kind == FT_VALUE_INT || desc->kind == FT_VALUE_FLOAT;
    const bool component_ok = scalar ? rule->component == PREDICTION_COMPONENT_VALUE
                                     : rule->component >= PREDICTION_COMPONENT_X &&
                                           rule->component <= PREDICTION_COMPONENT_MAGNITUDE &&
                                           !(desc->kind == FT_VALUE_VEC2 && rule->component == PREDICTION_COMPONENT_Z);
    if (!component_ok || rule->comparison < PREDICTION_COMPARE_EQUAL ||
        rule->comparison > PREDICTION_COMPARE_CHANGED || !isfinite(rule->target))
      continue;
    resolved[i].property = (uint32_t)property;
    resolved[i].kind = desc->kind;
    resolved[i].valid = true;
  }
}

static void initialize_player_color(game_host_t *host, const ft_world *world, int player,
                                    const prediction_line_t *line, const resolved_color_rule_t *resolved,
                                    color_rule_runtime_t *runtime, float color[4]) {
  memcpy(color, line->color, sizeof(line->color));
  for (int rule_index = 0; rule_index < line->color_rule_count && rule_index < MAX_PREDICTION_COLOR_RULES;
       ++rule_index) {
    const resolved_color_rule_t *rule = &resolved[rule_index];
    double value;
    if (!color_rule_value(host, world, player, rule, &value)) continue;
    color_rule_runtime_t *state = &runtime[rule_index];
    state->previous = value;
    state->have_previous = true;
    if (rule->rule->comparison == PREDICTION_COMPARE_CHANGED) continue;
    state->condition = color_rule_condition(rule, value);
    if (state->condition) memcpy(color, rule->rule->color, sizeof(rule->rule->color));
  }
}

static void update_player_color(game_host_t *host, const ft_world *world, int player, const prediction_line_t *line,
                                const resolved_color_rule_t *resolved, color_rule_runtime_t *runtime, float color[4]) {
  for (int rule_index = 0; rule_index < line->color_rule_count && rule_index < MAX_PREDICTION_COLOR_RULES;
       ++rule_index) {
    const resolved_color_rule_t *rule = &resolved[rule_index];
    color_rule_runtime_t *state = &runtime[rule_index];
    double value;
    if (!color_rule_value(host, world, player, rule, &value)) {
      state->have_previous = false;
      state->condition = false;
      continue;
    }

    bool triggered = false;
    if (rule->rule->comparison == PREDICTION_COMPARE_CHANGED) {
      triggered = state->have_previous && !color_rule_equal(rule, value, state->previous);
    } else {
      const bool condition = color_rule_condition(rule, value);
      triggered = condition && !state->condition;
      state->condition = condition;
    }
    state->previous = value;
    state->have_previous = true;
    if (triggered) memcpy(color, rule->rule->color, sizeof(rule->rule->color));
  }
}

void prediction_render_group(ui_handler_t *ui, int group_index, const ft_world *previous, const ft_world *current, float alpha) {
  if (!ui || !current) return;
  timeline_state_t *timeline = &ui->timeline;
  prediction_settings_t *settings = &timeline->prediction;
  if (!settings->enabled || settings->length <= 0 || group_index < 0 || group_index >= timeline->group_count) return;
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
  float(*active_colors)[4] = calloc((size_t)players, sizeof(*active_colors));
  color_rule_runtime_t *rule_runtime =
      calloc((size_t)players * MAX_PREDICTION_COLOR_RULES, sizeof(*rule_runtime));
  line_segment_t *segments = malloc(sizeof(*segments) * (size_t)length * (size_t)selected);
  if (!packed_inputs || !held_inputs || !positions || !positions3 || !have_position || !active_colors ||
      !rule_runtime || !segments)
    goto cleanup;

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
    memset(rule_runtime, 0, sizeof(*rule_runtime) * (size_t)players * MAX_PREDICTION_COLOR_RULES);
    resolved_color_rule_t resolved_rules[MAX_PREDICTION_COLOR_RULES];
    resolve_color_rules(host, line, resolved_rules);
    for (int player = 0; player < players; ++player) {
      const int track = model_group_track_index(timeline, group_index, player);
      if (track < 0 || !timeline->player_tracks[track].prediction_enabled) continue;
      initialize_player_color(host, world, player, line, resolved_rules,
                              rule_runtime + (size_t)player * MAX_PREDICTION_COLOR_RULES, active_colors[player]);
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
        update_player_color(host, world, player, line, resolved_rules,
                            rule_runtime + (size_t)player * MAX_PREDICTION_COLOR_RULES, active_colors[player]);

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
            vec4 color = {active_colors[player][0], active_colors[player][1], active_colors[player][2],
                          active_colors[player][3]};
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
          memcpy(segment->color, active_colors[player], sizeof(segment->color));
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
  free(rule_runtime);
  free(active_colors);
  free(have_position);
  free(positions3);
  free(positions);
  free(held_inputs);
  free(packed_inputs);
}

static void add_prediction_line(timeline_state_t *timeline) {
  prediction_settings_t *settings = &timeline->prediction;
  const int index = settings->line_count;
  if (index < 1 || index >= MAX_PREDICTION_LINES) return;
  settings->line_count = index + 1;
  prediction_line_default(&settings->lines[index], index);
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

static const ft_prop_desc *color_rule_property(const ft_entity_class *player_class,
                                               const prediction_color_rule_t *rule, int *out_index) {
  const int index = player_property_find(player_class, rule->property_id);
  if (out_index) *out_index = index;
  return index >= 0 ? &player_class->props[index] : NULL;
}

static void color_rule_select_property(prediction_color_rule_t *rule, const ft_prop_desc *property) {
  if (!rule || !property || !property->id) return;
  snprintf(rule->property_id, sizeof(rule->property_id), "%s", property->id);
  rule->component = property->kind == FT_VALUE_VEC2 || property->kind == FT_VALUE_VEC3
                        ? PREDICTION_COMPONENT_MAGNITUDE
                        : PREDICTION_COMPONENT_VALUE;
  rule->comparison = PREDICTION_COMPARE_EQUAL;
  rule->target = property->kind == FT_VALUE_BOOL ? 1.0 : 0.0;
}

static bool add_color_rule(prediction_line_t *line, int line_index, const ft_entity_class *player_class) {
  if (!line || !player_class || line->color_rule_count < 0 ||
      line->color_rule_count >= MAX_PREDICTION_COLOR_RULES)
    return false;
  const ft_prop_desc *property = first_color_rule_property(player_class);
  if (!property) return false;

  prediction_color_rule_t *rule = &line->color_rules[line->color_rule_count];
  memset(rule, 0, sizeof(*rule));
  color_rule_select_property(rule, property);
  const int palette = (line_index + line->color_rule_count + 1) %
                      (int)(sizeof(s_line_colors) / sizeof(s_line_colors[0]));
  memcpy(rule->color, s_line_colors[palette], sizeof(rule->color));
  rule->enabled = true;
  ++line->color_rule_count;
  return true;
}

static bool render_rule_property(prediction_color_rule_t *rule, const ft_entity_class *player_class) {
  int property_index = -1;
  const ft_prop_desc *property = color_rule_property(player_class, rule, &property_index);
  char missing[96];
  const char *preview = NULL;
  if (property)
    preview = property->display_name ? property->display_name : property->id;
  else {
    snprintf(missing, sizeof(missing), "%s%s", rule->property_id[0] ? "Missing: " : "Select property",
             rule->property_id);
    preview = missing;
  }

  bool changed = false;
  if (igBeginCombo("Property", preview, 0)) {
    const char *last_group = NULL;
    for (uint32_t i = 0; i < player_class->prop_count; ++i) {
      const ft_prop_desc *candidate = &player_class->props[i];
      if (!candidate->id || !color_rule_property_supported(candidate->kind)) continue;
      if (candidate->group && (!last_group || strcmp(last_group, candidate->group) != 0)) {
        igSeparatorText(candidate->group);
        last_group = candidate->group;
      }
      const bool selected = (int)i == property_index;
      if (igSelectable_Bool(candidate->display_name ? candidate->display_name : candidate->id, selected, 0,
                            (ImVec2){0.f, 0.f})) {
        color_rule_select_property(rule, candidate);
        changed = true;
      }
    }
    igEndCombo();
  }
  return changed;
}

static bool render_rule_component(prediction_color_rule_t *rule, ft_value_kind kind) {
  if (kind != FT_VALUE_VEC2 && kind != FT_VALUE_VEC3) return false;
  static const prediction_rule_component_t components[] = {
      PREDICTION_COMPONENT_X, PREDICTION_COMPONENT_Y, PREDICTION_COMPONENT_Z, PREDICTION_COMPONENT_MAGNITUDE};
  static const char *names[] = {"X", "Y", "Z", "Magnitude"};
  const int count = kind == FT_VALUE_VEC2 ? 3 : 4;
  int selected_index = count - 1;
  for (int i = 0; i < count; ++i) {
    const prediction_rule_component_t component = kind == FT_VALUE_VEC2 && i == 2 ? PREDICTION_COMPONENT_MAGNITUDE
                                                                                   : components[i];
    if (rule->component == component) selected_index = i;
  }

  bool changed = false;
  if (igBeginCombo("Component", names[kind == FT_VALUE_VEC2 && selected_index == 2 ? 3 : selected_index], 0)) {
    for (int i = 0; i < count; ++i) {
      const prediction_rule_component_t component = kind == FT_VALUE_VEC2 && i == 2 ? PREDICTION_COMPONENT_MAGNITUDE
                                                                                     : components[i];
      const int name_index = kind == FT_VALUE_VEC2 && i == 2 ? 3 : i;
      if (igSelectable_Bool(names[name_index], rule->component == component, 0, (ImVec2){0.f, 0.f})) {
        rule->component = component;
        changed = true;
      }
    }
    igEndCombo();
  }
  return changed;
}

static bool render_rule_comparison(prediction_color_rule_t *rule, ft_value_kind kind) {
  bool changed = false;
  if (kind == FT_VALUE_BOOL) {
    if (rule->comparison != PREDICTION_COMPARE_EQUAL && rule->comparison != PREDICTION_COMPARE_CHANGED) {
      rule->comparison = PREDICTION_COMPARE_EQUAL;
      rule->target = rule->target != 0.0 ? 1.0 : 0.0;
      changed = true;
    }
    const char *preview = rule->comparison == PREDICTION_COMPARE_CHANGED
                              ? "Changes"
                              : (rule->target != 0.0 ? "Becomes true" : "Becomes false");
    if (igBeginCombo("Trigger", preview, 0)) {
      if (igSelectable_Bool("Becomes true", rule->comparison == PREDICTION_COMPARE_EQUAL && rule->target != 0.0, 0,
                            (ImVec2){0.f, 0.f})) {
        rule->comparison = PREDICTION_COMPARE_EQUAL;
        rule->target = 1.0;
        changed = true;
      }
      if (igSelectable_Bool("Becomes false", rule->comparison == PREDICTION_COMPARE_EQUAL && rule->target == 0.0, 0,
                            (ImVec2){0.f, 0.f})) {
        rule->comparison = PREDICTION_COMPARE_EQUAL;
        rule->target = 0.0;
        changed = true;
      }
      if (igSelectable_Bool("Changes", rule->comparison == PREDICTION_COMPARE_CHANGED, 0, (ImVec2){0.f, 0.f})) {
        rule->comparison = PREDICTION_COMPARE_CHANGED;
        changed = true;
      }
      igEndCombo();
    }
    return changed;
  }

  static const char *names[] = {"Equals", "Drops below", "Rises above", "Changes"};
  int comparison = (int)rule->comparison;
  if (comparison < 0 || comparison > PREDICTION_COMPARE_CHANGED) comparison = PREDICTION_COMPARE_EQUAL;
  if (igBeginCombo("Trigger", names[comparison], 0)) {
    for (int i = 0; i <= PREDICTION_COMPARE_CHANGED; ++i) {
      if (igSelectable_Bool(names[i], comparison == i, 0, (ImVec2){0.f, 0.f})) {
        rule->comparison = (prediction_rule_comparison_t)i;
        changed = true;
      }
    }
    igEndCombo();
  }
  if (rule->comparison != PREDICTION_COMPARE_CHANGED) {
    const char *format = kind == FT_VALUE_INT ? "%.0f" : "%.6g";
    if (igInputDouble("Value", &rule->target, 0.0, 0.0, format, 0)) {
      if (!isfinite(rule->target)) rule->target = 0.0;
      changed = true;
    }
  }
  return changed;
}

static bool render_color_rules(prediction_line_t *line, int line_index, const ft_entity_class *player_class) {
  bool changed = false;
  if (!igTreeNode_Str("Color triggers")) return false;
  igTextWrapped("A trigger changes this player's line colour from that segment onward. Later rules win when they "
                "trigger on the same tick.");

  int remove = -1;
  for (int rule_index = 0; rule_index < line->color_rule_count; ++rule_index) {
    prediction_color_rule_t *rule = &line->color_rules[rule_index];
    igPushID_Int(rule_index);
    changed |= igCheckbox("##enabled", &rule->enabled);
    igSameLine(0.f, 6.f);
    igText("Trigger %d", rule_index + 1);
    igSameLine(0.f, 6.f);
    if (igButton(ICON_FA_TRASH "##rule", (ImVec2){0.f, 0.f})) remove = rule_index;

    if (player_class) changed |= render_rule_property(rule, player_class);
    const ft_prop_desc *property = player_class ? color_rule_property(player_class, rule, NULL) : NULL;
    if (property) {
      changed |= render_rule_component(rule, property->kind);
      changed |= render_rule_comparison(rule, property->kind);
    } else {
      igTextDisabled("This property is not exposed by the active game.");
    }
    changed |= igColorEdit4("Colour", rule->color,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    if (rule_index + 1 < line->color_rule_count) igSeparator();
    igPopID();
  }

  if (remove >= 0) {
    memmove(&line->color_rules[remove], &line->color_rules[remove + 1],
            sizeof(line->color_rules[0]) * (size_t)(line->color_rule_count - remove - 1));
    memset(&line->color_rules[--line->color_rule_count], 0, sizeof(line->color_rules[0]));
    changed = true;
  }

  const bool can_add = first_color_rule_property(player_class) &&
                       line->color_rule_count < MAX_PREDICTION_COLOR_RULES;
  if (!can_add) igBeginDisabled(true);
  if (igButton(ICON_FA_PLUS " Add color trigger", (ImVec2){0.f, 0.f}) &&
      add_color_rule(line, line_index, player_class))
    changed = true;
  if (!can_add) igEndDisabled();
  igTreePop();
  return changed;
}

void prediction_render_menu(timeline_state_t *timeline) {
  if (!timeline || !timeline->ui) return;
  prediction_settings_t *settings = &timeline->prediction;
  const float dpi = gfx_get_ui_scale();

  const bool open = ui_icon_button(timeline->ui, ICON_FA_ROUTE, (ImVec2){30.f * dpi, 0.f});
  if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("Prediction lines");
  if (open) igOpenPopup_Str("PredictionMenu", ImGuiPopupFlags_None);

  igSetNextWindowSizeConstraints((ImVec2){360.f * dpi, 0.f}, (ImVec2){620.f * dpi, 720.f * dpi}, NULL, NULL);
  if (!igBeginPopup("PredictionMenu", ImGuiWindowFlags_AlwaysAutoResize)) return;

  bool config_changed = false;
  bool project_changed = false;
  config_changed |= igCheckbox("Show prediction", &settings->enabled);
  igSetNextItemWidth(180.f * dpi);
  config_changed |= igSliderInt("Ticks ahead", &settings->length, 1, 2000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSetNextItemWidth(180.f * dpi);
  config_changed |= igSliderFloat("Line width", &settings->thickness, 0.01f, 0.30f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
  igTextDisabled("Prediction settings and line definitions are saved per game.");

  igSeparatorText("Lines");
  int remove_line = -1;
  const ft_input_schema *schema = game_input_schema(&timeline->ui->gfx_handler->game_host);
  const ft_entity_class *player_class =
      gh_entity_class(&timeline->ui->gfx_handler->game_host, FT_ENTITY_CLASS_PLAYER);
  for (int line_index = 0; line_index < settings->line_count; ++line_index) {
    prediction_line_t *line = &settings->lines[line_index];
    igPushID_Int(line_index);
    config_changed |= igCheckbox("##enabled", &line->enabled);
    igSameLine(0.f, 6.f * dpi);
    igSetNextItemWidth(180.f * dpi);
    config_changed |= igInputText("##name", line->name, sizeof(line->name), 0, NULL, NULL);
    igSameLine(0.f, 6.f * dpi);
    config_changed |= igColorEdit4("##color", line->color,
                                   ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                       ImGuiColorEditFlags_NoLabel);
    if (!line->use_timeline_inputs) {
      igSameLine(0.f, 6.f * dpi);
      if (igButton(ICON_FA_TRASH, (ImVec2){0.f, 0.f})) remove_line = line_index;
      if (schema && igTreeNode_Str("Inputs")) {
        for (uint32_t control = 0; control < schema->control_count && control < 64; ++control) {
          igPushID_Int((int)control);
          bool selected = (line->controls & (UINT64_C(1) << control)) != 0;
          const char *label = schema->controls[control].display_name ? schema->controls[control].display_name
                                                                    : schema->controls[control].id;
          if (igCheckbox(label, &selected)) {
            if (selected) line->controls |= UINT64_C(1) << control;
            else line->controls &= ~(UINT64_C(1) << control);
            config_changed = true;
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
    config_changed |= render_color_rules(line, line_index, player_class);
    igPopID();
  }
  if (remove_line > 0) {
    remove_prediction_line(timeline, remove_line);
    config_changed = true;
  }
  if (settings->line_count < MAX_PREDICTION_LINES && igButton(ICON_FA_PLUS " Add input line", (ImVec2){0.f, 0.f})) {
    add_prediction_line(timeline);
    config_changed = true;
  }

  igSeparatorText("Groups and tracks");
  if (igButton("All", (ImVec2){0.f, 0.f})) {
    for (int group = 0; group < timeline->group_count; ++group) timeline->groups[group]->prediction_enabled = true;
    for (int track = 0; track < timeline->player_track_count; ++track) timeline->player_tracks[track].prediction_enabled = true;
    project_changed = true;
  }
  igSameLine(0.f, 6.f * dpi);
  if (igButton("None", (ImVec2){0.f, 0.f})) {
    for (int group = 0; group < timeline->group_count; ++group) timeline->groups[group]->prediction_enabled = false;
    for (int track = 0; track < timeline->player_track_count; ++track) timeline->player_tracks[track].prediction_enabled = false;
    project_changed = true;
  }
  for (int group_index = 0; group_index < timeline->group_count; ++group_index) {
    timeline_group_t *group = timeline->groups[group_index];
    igPushID_Int(1000 + group_index);
    project_changed |= igCheckbox(group->name, &group->prediction_enabled);
    igIndent(18.f * dpi);
    for (int track_index = 0; track_index < timeline->player_track_count; ++track_index) {
      player_track_t *track = &timeline->player_tracks[track_index];
      if (track->group_index != group_index) continue;
      igPushID_Int(track_index);
      project_changed |= igCheckbox(track->name[0] ? track->name : "Track", &track->prediction_enabled);
      igPopID();
    }
    igUnindent(18.f * dpi);
    igPopID();
  }

  if (config_changed) {
    timeline->ui->configured_prediction = *settings;
    config_save(timeline->ui);
  }
  if (project_changed) timeline_mark_unsaved(timeline);
  igEndPopup();
}
