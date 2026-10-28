#include "prediction.h"
#include "entity_match.h"

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
#include <user_interface/timeline/timeline_recordings.h>
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
    {0.25f, 0.72f, 1.00f, 0.78f},
    {1.00f, 0.48f, 0.18f, 0.82f},
    {0.35f, 0.95f, 0.48f, 0.82f},
    {0.95f, 0.35f, 0.72f, 0.82f},
    {0.75f, 0.48f, 1.00f, 0.82f},
    {1.00f, 0.85f, 0.25f, 0.82f},
    {0.20f, 0.90f, 0.85f, 0.82f},
    {1.00f, 0.35f, 0.35f, 0.82f},
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
  settings->width_px = 3.f;
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
    world = gh_world_create_flags(host, timeline->ui->gfx_handler->level, players, -1, FT_WORLD_MARKS);
    group->prediction_worlds[line_index] = world;
  }
  if (world) gh_world_copy(host, world, source);
  return world;
}

static int selected_player_count(const timeline_state_t *timeline, const int *tracks, int players) {
  int selected = 0;
  for (int player = 0; player < players; ++player) {
    const int track = tracks[player];
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
  case FT_VALUE_BOOL:
    *out = value.as.b ? 1.0 : 0.0;
    break;
  case FT_VALUE_INT:
    *out = (double)value.as.i;
    break;
  case FT_VALUE_FLOAT:
    *out = value.as.f;
    break;
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
  default:
    return false;
  }
  return isfinite(*out);
}

static bool color_rule_equal(const resolved_color_rule_t *resolved, double a, double b) {
  if (resolved->kind == FT_VALUE_BOOL || resolved->kind == FT_VALUE_INT) return a == b;
  const double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
  return fabs(a - b) <= scale * 1e-6;
}

static bool color_rule_is_velocity(const prediction_color_rule_t *rule) {
  return rule && strcmp(rule->property_id, "velocity") == 0;
}

static bool color_rule_changed(const resolved_color_rule_t *resolved, double value, double previous) {
  if (color_rule_equal(resolved, value, previous)) return false;
  if (!color_rule_is_velocity(resolved->rule)) return true;
  switch (resolved->rule->change_mode) {
  case PREDICTION_CHANGE_ANY:
    return true;
  case PREDICTION_CHANGE_INCREASING:
    return fabs(value) > fabs(previous);
  case PREDICTION_CHANGE_DECREASING:
    return fabs(value) < fabs(previous);
  }
  return true;
}

static bool color_rule_condition(const resolved_color_rule_t *resolved, double value) {
  switch (resolved->rule->comparison) {
  case PREDICTION_COMPARE_EQUAL:
    return color_rule_equal(resolved, value, resolved->rule->target);
  case PREDICTION_COMPARE_LESS:
    return value < resolved->rule->target;
  case PREDICTION_COMPARE_GREATER:
    return value > resolved->rule->target;
  case PREDICTION_COMPARE_CHANGED:
  case PREDICTION_COMPARE_SCALE:
    return false;
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
    if (!component_ok || rule->comparison < PREDICTION_COMPARE_EQUAL || rule->comparison > PREDICTION_COMPARE_SCALE ||
        !isfinite(rule->target))
      continue;
    if (rule->comparison == PREDICTION_COMPARE_SCALE && (desc->kind == FT_VALUE_BOOL || !isfinite(rule->from))) continue;
    resolved[i].property = (uint32_t)property;
    resolved[i].kind = desc->kind;
    resolved[i].valid = true;
  }
}

static void update_player_color(game_host_t *host, const ft_world *world, int player, const prediction_line_t *line,
                                const resolved_color_rule_t *resolved, color_rule_runtime_t *runtime, float color[4]) {
  // Re-evaluate every segment so an expired rule reveals the next matching
  // rule (or the base colour), instead of leaving its colour latched.
  memcpy(color, line->color, sizeof(line->color));
  for (int rule_index = 0; rule_index < line->color_rule_count && rule_index < MAX_PREDICTION_COLOR_RULES;
       ++rule_index) {
    const resolved_color_rule_t *rule = &resolved[rule_index];
    color_rule_runtime_t *state = &runtime[rule_index];
    double value;
    if (!color_rule_value(host, world, player, rule, &value)) {
      state->have_previous = false;
      continue;
    }

    if (rule->rule->comparison == PREDICTION_COMPARE_SCALE) {
      const double span = rule->rule->target - rule->rule->from;
      double amount = span != 0.0 ? (value - rule->rule->from) / span : (value >= rule->rule->target ? 1.0 : 0.0);
      amount = fmin(fmax(amount, 0.0), 1.0) * rule->rule->color[3];
      for (int c = 0; c < 3; ++c) color[c] = fminf(color[c] + (float)amount * rule->rule->color[c], 1.f);
      state->previous = value;
      state->have_previous = true;
      continue;
    }

    bool triggered = false;
    if (rule->rule->comparison == PREDICTION_COMPARE_CHANGED) {
      triggered = state->have_previous && color_rule_changed(rule, value, state->previous);
    } else {
      triggered = color_rule_condition(rule, value);
    }
    state->previous = value;
    state->have_previous = true;
    if (triggered) memcpy(color, rule->rule->color, sizeof(rule->rule->color));
  }
}

// What a line draws goes in layers, from the bottom: how far the explosions at
// the ends of entities' paths reach, then the rings around that, ropes, the
// players' paths, the dark edges under entities' paths, the entities' paths,
// and dots.
enum { LAYER_REACH, LAYER_RING, LAYER_ROPES, LAYER_PLAYERS, LAYER_RIMS, LAYER_ENTITIES, LAYER_DOTS };

typedef struct prediction_draw_t {
  gfx_handler_t *gfx;
  int line_index;
  vec4 color;      // the line's
  float width_px;  // the players' paths
  float pixel;     // world units a pixel covers
  stroke_point_t *scratch;
  uint32_t scratch_cap;
} prediction_draw_t;

static float layer_z(const prediction_draw_t *d, int layer) {
  return PREDICTION_Z + (float)d->line_index * 0.001f + (float)layer * 0.0001f;
}

// A path, a step at a time. A point's colour is that of the segment after it.
typedef struct stroke_t {
  stroke_point_t *points;
  uint32_t count, cap;
} stroke_t;

static void stroke_add(stroke_t *stroke, ft_vec2 position, const float color[4]) {
  if (stroke->count == stroke->cap) {
    const uint32_t cap = stroke->cap ? stroke->cap * 2 : 64;
    stroke_point_t *grown = realloc(stroke->points, sizeof(*grown) * cap);
    if (!grown) return;
    stroke->points = grown;
    stroke->cap = cap;
  }
  if (stroke->count > 0) memcpy(stroke->points[stroke->count - 1].color, color, sizeof(float) * 4);
  stroke_point_t *point = &stroke->points[stroke->count++];
  point->pos[0] = position.x;
  point->pos[1] = position.y;
  memcpy(point->color, color, sizeof(float) * 4);
}

static void draw_stroke(prediction_draw_t *d, int layer, const stroke_t *stroke, float width_px) {
  if (stroke->count >= 2) renderer_submit_stroke(d->gfx, layer_z(d, layer), stroke->points, stroke->count, width_px, false);
}

// The same path in one colour, under it.
static void draw_stroke_in(prediction_draw_t *d, int layer, const stroke_t *stroke, const float color[4], float width_px) {
  if (stroke->count < 2) return;
  if (stroke->count > d->scratch_cap) {
    stroke_point_t *grown = realloc(d->scratch, sizeof(*grown) * stroke->count);
    if (!grown) return;
    d->scratch = grown;
    d->scratch_cap = stroke->count;
  }
  for (uint32_t i = 0; i < stroke->count; ++i) {
    d->scratch[i].pos[0] = stroke->points[i].pos[0];
    d->scratch[i].pos[1] = stroke->points[i].pos[1];
    memcpy(d->scratch[i].color, color, sizeof(float) * 4);
  }
  renderer_submit_stroke(d->gfx, layer_z(d, layer), d->scratch, stroke->count, width_px, false);
}

// A circle of `radius` world units around `center`: filled, or a ring ring_px wide.
static void draw_circle(prediction_draw_t *d, int layer, ft_vec2 center, float radius, const float color[4], float ring_px) {
  enum { MAX_CIRCLE_POINTS = 128 };
  const float radius_px = radius / d->pixel;
  int count = (int)(radius_px * 0.5f);
  if (count < 16) count = 16;
  if (count > MAX_CIRCLE_POINTS) count = MAX_CIRCLE_POINTS;
  stroke_point_t points[MAX_CIRCLE_POINTS];
  vec2 outline[MAX_CIRCLE_POINTS];
  for (int i = 0; i < count; ++i) {
    const float angle = (float)i * 6.28318531f / (float)count;
    outline[i][0] = points[i].pos[0] = center.x + cosf(angle) * radius;
    outline[i][1] = points[i].pos[1] = center.y + sinf(angle) * radius;
    memcpy(points[i].color, color, sizeof(float) * 4);
  }
  vec4 fill; // (the renderer copies a colour with aligned loads)
  memcpy(fill, color, sizeof(fill));
  if (ring_px > 0.f) renderer_submit_stroke(d->gfx, layer_z(d, layer), points, (uint32_t)count, ring_px, true);
  else renderer_submit_polygon(d->gfx, layer_z(d, layer), outline, (uint32_t)count, fill);
}

// Everything else a game publishes besides its players (DDNet's projectiles,
// lasers and hooks) is followed through the prediction too. Nothing about them
// is known here beyond what the class lists: a class with a `from` and a
// `position` is a beam, drawn as it stands at every step, and one with only a
// `position` leaves a trail, which goes on from step to step with the same
// entity as entity_trail_match finds it. A trail that begins during the
// prediction starts from the entity's `origin`, where the class has one (a
// hook's tee), rather than a step's travel away from it. How they look is the
// game's (gh_entity_trail), down to where a trail ends and how that is marked.
#define ENTITY_TRAIL_REACH 4.f // tiles an entity may move in a step and still be the same one
#define ENTITY_MAX_IDENTITY 8

typedef struct entity_trace_t {
  unsigned entity_class;
  int position, from; // property indices; from < 0 for a trail
  int origin;         // property index, < 0 for none
  int identity[ENTITY_MAX_IDENTITY];
  int identity_count;
  // The class at the previous step: where each entity was, for a beam where it
  // started, and for a trail how far it moved, its identity and its path.
  ft_vec2 *last;
  ft_vec2 *last_from;
  ft_vec2 *last_motion;
  uint64_t *last_identity;
  int *last_path;
  int last_count;
} entity_trace_t;

// The path an entity leaves, drawn once it ends.
typedef struct entity_path_t {
  stroke_t stroke;
  ft_entity_trail style; // as the entity last had it
  // the rope drawn last, which a rope that has not moved does not draw again
  ft_vec2 rope_from, rope_to;
  bool roped;
} entity_path_t;

typedef struct entity_traces_t {
  entity_trace_t *traces;
  int count;
  entity_path_t *paths;
  int path_count, path_cap;
} entity_traces_t;

static int entity_vec2_prop(const ft_entity_class *entity_class, const char *id) {
  for (uint32_t i = 0; i < entity_class->prop_count; ++i)
    if (entity_class->props[i].id && strcmp(entity_class->props[i].id, id) == 0 && entity_class->props[i].kind == FT_VALUE_VEC2)
      return (int)i;
  return -1;
}

static void entity_traces_init(game_host_t *host, entity_traces_t *t) {
  memset(t, 0, sizeof(*t));
  const unsigned classes = gh_entity_class_count(host);
  if (classes <= 1) return;
  t->traces = calloc(classes, sizeof(*t->traces));
  if (!t->traces) return;
  for (unsigned c = 0; c < classes; ++c) {
    if (c == FT_ENTITY_CLASS_PLAYER) continue;
    const ft_entity_class *entity_class = gh_entity_class(host, c);
    if (!entity_class) continue;
    const int position = entity_vec2_prop(entity_class, "position");
    if (position < 0) continue;
    entity_trace_t *trace = &t->traces[t->count++];
    *trace = (entity_trace_t){.entity_class = c,
                              .position = position,
                              .from = entity_vec2_prop(entity_class, "from"),
                              .origin = entity_vec2_prop(entity_class, "origin")};
    for (uint32_t i = 0; i < entity_class->prop_count && trace->identity_count < ENTITY_MAX_IDENTITY; ++i)
      if (entity_class->props[i].flags & FT_PROP_IDENTITY) trace->identity[trace->identity_count++] = (int)i;
  }
}

// The entity's look, the line's own unless the game has one for it.
static void entity_style(game_host_t *host, const ft_world *world, unsigned entity_class, int entity, ft_entity_trail *out) {
  if (!gh_entity_trail(host, world, entity_class, entity, out)) memset(out, 0, sizeof(*out));
}

// A colour of an entity's look as this line draws it: the game's, faded with
// the line, or for none the line's own, a little fainter than the players'.
static void entity_color(const prediction_draw_t *d, ft_color color, float out[4]) {
  if (color.a <= 0.f) {
    memcpy(out, d->color, sizeof(d->color));
    out[3] *= 0.7f;
    return;
  }
  out[0] = color.r;
  out[1] = color.g;
  out[2] = color.b;
  out[3] = color.a * d->color[3];
}

static float entity_width(const prediction_draw_t *d, const ft_entity_trail *style) {
  return (style->width > 0.f ? style->width : 0.75f) * d->width_px;
}

static void entity_dot(prediction_draw_t *d, ft_vec2 at, float diameter_px, ft_color color) {
  vec4 rgba;
  entity_color(d, color, rgba);
  draw_circle(d, LAYER_DOTS, at, diameter_px * 0.5f * d->pixel, rgba, 0.f);
}

static void entity_stroke(prediction_draw_t *d, const stroke_t *stroke, const ft_entity_trail *style) {
  const float width = entity_width(d, style);
  if (style->rim.a > 0.f) {
    vec4 rim;
    entity_color(d, style->rim, rim);
    draw_stroke_in(d, LAYER_RIMS, stroke, rim, width * 2.f);
  }
  draw_stroke(d, LAYER_ENTITIES, stroke, width);
}

// Draws a path and lets it go. One whose entity is gone runs on to where the
// game says its last step took it.
static void entity_path_end(prediction_draw_t *d, entity_path_t *path, bool gone) {
  const ft_entity_trail *style = &path->style;
  if (gone && style->has_next && path->stroke.count > 0) {
    vec4 color;
    entity_color(d, style->color, color);
    stroke_add(&path->stroke, style->next, color);
  }
  if (!style->rope_only) entity_stroke(d, &path->stroke, style);
  free(path->stroke.points);
  memset(&path->stroke, 0, sizeof(path->stroke));
}

// What the step the world just took left to mark: an explosion's reach, a ring
// around it and a dot where it was.
static void draw_marks(game_host_t *host, const ft_world *world, prediction_draw_t *d) {
  enum { MAX_MARKS = 32 };
  ft_world_mark marks[MAX_MARKS];
  uint32_t count = gh_world_marks(host, world, marks, MAX_MARKS);
  if (count > MAX_MARKS) count = MAX_MARKS;
  for (uint32_t i = 0; i < count; ++i) {
    const ft_world_mark *mark = &marks[i];
    vec4 color;
    if (mark->radius > 0.f && mark->fill.a > 0.f) {
      entity_color(d, mark->fill, color);
      draw_circle(d, LAYER_REACH, mark->position, mark->radius, color, 0.f);
    }
    if (mark->radius > 0.f && mark->ring.a > 0.f) {
      entity_color(d, mark->ring, color);
      draw_circle(d, LAYER_RING, mark->position, mark->radius, color, fmaxf(1.f, d->width_px * 0.5f));
    }
    if (mark->dot.a > 0.f) entity_dot(d, mark->position, 0.75f * d->width_px * 3.f, mark->dot);
  }
}

static int entity_path_start(entity_traces_t *t) {
  if (t->path_count == t->path_cap) {
    const int cap = t->path_cap ? t->path_cap * 2 : 32;
    entity_path_t *grown = realloc(t->paths, sizeof(*grown) * (size_t)cap);
    if (!grown) return -1;
    t->paths = grown;
    t->path_cap = cap;
  }
  memset(&t->paths[t->path_count], 0, sizeof(t->paths[0]));
  return t->path_count++;
}

static void hash_bytes(uint64_t *hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; ++i) *hash = (*hash ^ bytes[i]) * UINT64_C(0x100000001b3);
}

// The entity's identity properties folded into one value; 0 when it has none or one cannot be read.
static uint64_t entity_identity(game_host_t *host, const ft_world *world, const entity_trace_t *trace, int entity) {
  if (trace->identity_count == 0) return 0;
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (int i = 0; i < trace->identity_count; ++i) {
    ft_value value;
    if (!gh_entity_prop_get(host, world, trace->entity_class, entity, (unsigned)trace->identity[i], &value)) return 0;
    hash_bytes(&hash, &value.kind, sizeof(value.kind));
    switch (value.kind) {
    case FT_VALUE_BOOL: hash_bytes(&hash, &value.as.b, sizeof(value.as.b)); break;
    case FT_VALUE_INT: hash_bytes(&hash, &value.as.i, sizeof(value.as.i)); break;
    case FT_VALUE_FLOAT: hash_bytes(&hash, &value.as.f, sizeof(value.as.f)); break;
    case FT_VALUE_VEC2: hash_bytes(&hash, &value.as.v, sizeof(value.as.v)); break;
    case FT_VALUE_VEC3: hash_bytes(&hash, &value.as.v3, sizeof(value.as.v3)); break;
    default: break;
    }
  }
  return hash ? hash : 1;
}

// Takes the entities as they are in `world`; `draw` is false for the first
// look, which only gives the trails their starting points.
static void entity_traces_step(game_host_t *host, const ft_world *world, entity_traces_t *t, prediction_draw_t *d, bool draw) {
  for (int i = 0; i < t->count; ++i) {
    entity_trace_t *trace = &t->traces[i];
    const int count = gh_entity_count(host, world, trace->entity_class);
    const bool beam = trace->from >= 0;
    ft_vec2 *now = count > 0 ? malloc(sizeof(*now) * (size_t)count) : NULL;
    ft_vec2 *now_from = count > 0 && beam ? malloc(sizeof(*now_from) * (size_t)count) : NULL;
    ft_vec2 *now_motion = count > 0 && !beam ? calloc((size_t)count, sizeof(*now_motion)) : NULL;
    uint64_t *now_identity = count > 0 && !beam && trace->identity_count > 0 ? malloc(sizeof(*now_identity) * (size_t)count) : NULL;
    int *now_path = count > 0 && !beam ? malloc(sizeof(*now_path) * (size_t)count) : NULL;
    // the entity each trail entry was read from
    int *now_entity = count > 0 && !beam ? malloc(sizeof(*now_entity) * (size_t)count) : NULL;
    int now_count = 0;
    for (int e = 0; now && e < count; ++e) {
      ft_value value;
      if (!gh_entity_prop_get(host, world, trace->entity_class, e, (unsigned)trace->position, &value) || value.kind != FT_VALUE_VEC2) continue;
      const ft_vec2 position = value.as.v;
      if (beam) {
        if (!now_from || !gh_entity_prop_get(host, world, trace->entity_class, e, (unsigned)trace->from, &value) || value.kind != FT_VALUE_VEC2)
          continue;
        const ft_vec2 from = value.as.v;
        now_from[now_count] = from;
        now[now_count++] = position;
        if (!draw) continue;
        // A beam stays as it is for a few steps: drawn once.
        bool drawn = false;
        for (int k = 0; k < trace->last_count && !drawn; ++k)
          drawn = trace->last_from[k].x == from.x && trace->last_from[k].y == from.y && trace->last[k].x == position.x && trace->last[k].y == position.y;
        if (drawn || (from.x == position.x && from.y == position.y)) continue;
        ft_entity_trail style;
        entity_style(host, world, trace->entity_class, e, &style);
        vec4 color;
        entity_color(d, style.color, color);
        stroke_t stroke = {0};
        stroke_add(&stroke, from, color);
        stroke_add(&stroke, position, color);
        entity_stroke(d, &stroke, &style);
        if (style.dot > 0.f) entity_dot(d, position, entity_width(d, &style) * style.dot, style.color);
        free(stroke.points);
        continue;
      }
      if (now_identity) now_identity[now_count] = entity_identity(host, world, trace, e);
      if (now_entity) now_entity[now_count] = e;
      now[now_count++] = position;
    }

    if (!beam) {
      int *match = now_count > 0 ? malloc(sizeof(*match) * (size_t)now_count) : NULL;
      bool *taken = trace->last_count > 0 ? calloc((size_t)trace->last_count, sizeof(*taken)) : NULL;
      if (match && now_motion && now_path && now_entity)
        entity_trail_match(trace->last, trace->last_motion, trace->last_identity, trace->last_count, now, now_identity,
                           now_count, ENTITY_TRAIL_REACH, match);
      else if (match)
        for (int e = 0; e < now_count; ++e) match[e] = -1;
      for (int e = 0; match && now_path && now_entity && e < now_count; ++e) {
        int path = match[e] >= 0 ? trace->last_path[match[e]] : -1;
        if (match[e] >= 0) {
          if (taken) taken[match[e]] = true;
          now_motion[e] = (ft_vec2){now[e].x - trace->last[match[e]].x, now[e].y - trace->last[match[e]].y};
        }
        ft_entity_trail style;
        entity_style(host, world, trace->entity_class, now_entity[e], &style);
        vec4 color;
        entity_color(d, style.color, color);
        if (path < 0) {
          path = entity_path_start(t);
          ft_value origin;
          if (path >= 0 && draw && trace->origin >= 0 &&
              gh_entity_prop_get(host, world, trace->entity_class, now_entity[e], (unsigned)trace->origin, &origin) &&
              origin.kind == FT_VALUE_VEC2)
            stroke_add(&t->paths[path].stroke, origin.as.v, color);
        }
        now_path[e] = path;
        if (path < 0) continue;
        entity_path_t *entity_path = &t->paths[path];
        entity_path->style = style;
        if (draw && style.rope &&
            !(entity_path->roped && entity_path->rope_from.x == style.anchor.x && entity_path->rope_from.y == style.anchor.y &&
              entity_path->rope_to.x == now[e].x && entity_path->rope_to.y == now[e].y)) {
          stroke_t rope = {0};
          stroke_add(&rope, style.anchor, color);
          stroke_add(&rope, now[e], color);
          draw_stroke(d, LAYER_ROPES, &rope, entity_width(d, &style));
          free(rope.points);
          entity_path->rope_from = style.anchor;
          entity_path->rope_to = now[e];
          entity_path->roped = true;
        }
        // (at rest, e.g. a hook that holds on: no corner for nothing)
        const stroke_point_t *tail = entity_path->stroke.count > 0 ? &entity_path->stroke.points[entity_path->stroke.count - 1] : NULL;
        if (!tail || tail->pos[0] != now[e].x || tail->pos[1] != now[e].y) stroke_add(&entity_path->stroke, now[e], color);
      }
      // The entities that were and are not anymore.
      for (int k = 0; k < trace->last_count; ++k)
        if ((!taken || !taken[k]) && trace->last_path[k] >= 0) entity_path_end(d, &t->paths[trace->last_path[k]], true);
      free(taken);
      free(match);
    }
    free(now_entity);
    free(trace->last);
    free(trace->last_from);
    free(trace->last_motion);
    free(trace->last_identity);
    free(trace->last_path);
    trace->last = now;
    trace->last_from = now_from;
    trace->last_motion = now_motion;
    trace->last_identity = now_identity;
    trace->last_path = now_path;
    trace->last_count = now_path || beam ? now_count : 0;
  }
}

// Draws the paths of the entities still about when the prediction ends.
static void entity_traces_finish(entity_traces_t *t, prediction_draw_t *d) {
  for (int i = 0; i < t->count; ++i) {
    entity_trace_t *trace = &t->traces[i];
    if (trace->from >= 0 || !trace->last_path) continue;
    for (int k = 0; k < trace->last_count; ++k)
      if (trace->last_path[k] >= 0) entity_path_end(d, &t->paths[trace->last_path[k]], false);
  }
}

static void entity_traces_free(entity_traces_t *t) {
  for (int i = 0; i < t->count; ++i) {
    free(t->traces[i].last);
    free(t->traces[i].last_from);
    free(t->traces[i].last_motion);
    free(t->traces[i].last_identity);
    free(t->traces[i].last_path);
  }
  for (int i = 0; i < t->path_count; ++i) free(t->paths[i].stroke.points);
  free(t->traces);
  free(t->paths);
  memset(t, 0, sizeof(*t));
}

void prediction_render_group(ui_handler_t *ui, int group_index, const ft_world *previous, const ft_world *current, float alpha) {
  if (!ui || !current) return;
  timeline_state_t *timeline = &ui->timeline;
  prediction_settings_t *settings = &timeline->prediction;
  // Whether prediction is drawn at all is the caller's: the viewport and a
  // video each decide for themselves.
  if (settings->length <= 0 || group_index < 0 || group_index >= timeline->group_count) return;
  timeline_group_t *group = timeline->groups[group_index];
  if (!group->prediction_enabled) return;

  game_host_t *host = &ui->gfx_handler->game_host;
  const int players = gh_world_player_count(host, current);
  if (players <= 0) return;
  // each player's track, looked up once rather than per player per step
  int *tracks = malloc(sizeof(*tracks) * (size_t)players);
  if (!tracks) return;
  model_group_tracks(timeline, group_index, tracks, players);
  const int selected = selected_player_count(timeline, tracks, players);
  const size_t input_size = game_input_size(host);
  if (selected <= 0 || input_size == 0) {
    free(tracks);
    return;
  }

  int length = settings->length;
  if (length > 2000) length = 2000;
  const int safe_length = MAX_PREDICTION_SEGMENTS / selected;
  if (length > safe_length) length = safe_length;
  if (length <= 0) {
    free(tracks);
    return;
  }

  // A 3D game's prediction is a line through the world, not a stripe on a map.
  const bool is_3d = game_is_3d(host);
  // The width is in pixels, in 2D and 3D alike: world units differ from game to
  // game by orders of magnitude, so any width in them looks different in each.
  const float width_px = settings->width_px > 0.f ? settings->width_px : 3.f;

  uint8_t *packed_inputs = calloc((size_t)players, input_size);
  input_record_t *held_inputs = calloc((size_t)players, sizeof(*held_inputs));
  stroke_t *strokes = calloc((size_t)players, sizeof(*strokes));
  vec3 *positions3 = calloc((size_t)players, sizeof(*positions3));
  bool *have_position = calloc((size_t)players, sizeof(*have_position));
  float (*active_colors)[4] = calloc((size_t)players, sizeof(*active_colors));
  color_rule_runtime_t *rule_runtime =
      calloc((size_t)players * MAX_PREDICTION_COLOR_RULES, sizeof(*rule_runtime));
  ft_player_playback *playback = calloc((size_t)players, sizeof(*playback));
  // (the same everywhere in a 2D view)
  prediction_draw_t draw = {.gfx = ui->gfx_handler, .width_px = width_px,
                            .pixel = is_3d ? 0.f : renderer_world_per_pixel(ui->gfx_handler, (vec2){0.f, 0.f})};
  if (!packed_inputs || !held_inputs || !strokes || !positions3 || !have_position || !active_colors || !rule_runtime ||
      !playback)
    goto cleanup;

  alpha = fmaxf(0.f, fminf(alpha, 1.f));
  for (int player = 0; player < players; ++player) {
    const int track = tracks[player];
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
    draw.line_index = line_index;
    memcpy(draw.color, line->color, sizeof(draw.color));

    memset(have_position, 0, sizeof(*have_position) * (size_t)players);
    memset(rule_runtime, 0, sizeof(*rule_runtime) * (size_t)players * MAX_PREDICTION_COLOR_RULES);
    resolved_color_rule_t resolved_rules[MAX_PREDICTION_COLOR_RULES];
    resolve_color_rules(host, line, resolved_rules);
    for (int player = 0; player < players; ++player) {
      strokes[player].count = 0;
      const int track = tracks[player];
      if (track < 0 || !timeline->player_tracks[track].prediction_enabled) continue;
      update_player_color(host, world, player, line, resolved_rules,
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
      ft_vec2 position = current_view.position;
      if (previous && gh_world_player_view(host, previous, player, &previous_view)) {
        position.x = previous_view.position.x + (current_view.position.x - previous_view.position.x) * alpha;
        position.y = previous_view.position.y + (current_view.position.y - previous_view.position.y) * alpha;
      }
      stroke_add(&strokes[player], position, active_colors[player]);
      have_position[player] = true;
    }

    entity_traces_t entities;
    entity_traces_init(host, &entities);
    if (!is_3d) entity_traces_step(host, world, &entities, &draw, false);
    for (int step = 0; step < length; ++step) {
      // Recorded players replay their recording here too, so predicted paths meet them where they
      // really are.
      const int step_tick = gh_world_tick(host, world);
      bool replaying = false;
      for (int player = 0; player < players; ++player) {
        const int track = tracks[player];
        memset(&playback[player], 0, sizeof(playback[player]));
        if (track >= 0 && recordings_playback_at_tick(timeline, &timeline->player_tracks[track], step_tick, &playback[player]))
          replaying = true;
      }
      for (int player = 0; player < players; ++player) {
        const int track = tracks[player];
        input_record_t input;
        if (track >= 0) {
          if (!line->use_timeline_inputs && timeline->player_tracks[track].prediction_enabled) {
            input = held_inputs[player];
            // (a one-shot press only on the tick at the playhead)
            if (step > 0) engine_input_reset_triggers(host, &input);
            apply_controls(host, &input, line->controls);
          } else {
            input = interaction_predict_input(ui, world, track);
          }
        } else {
          engine_input_default(host, &input);
        }
        memcpy(packed_inputs + (size_t)player * input_size, input.bytes, input_size);
      }
      gh_world_step_playback(host, world, packed_inputs, replaying ? playback : NULL, (unsigned)players);
      if (!is_3d) {
        entity_traces_step(host, world, &entities, &draw, true);
        draw_marks(host, world, &draw);
      }

      for (int player = 0; player < players; ++player) {
        const int track = tracks[player];
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
            renderer_submit_line3_px(ui->gfx_handler, positions3[player], next, color, width_px);
          }
          glm_vec3_copy(next, positions3[player]);
          have_position[player] = true;
          continue;
        }

        // A player whose position goes missing breaks its path there.
        ft_player_view view;
        if (!gh_world_player_view(host, world, player, &view)) {
          draw_stroke(&draw, LAYER_PLAYERS, &strokes[player], width_px);
          strokes[player].count = 0;
          continue;
        }
        stroke_add(&strokes[player], view.position, active_colors[player]);
      }
    }
    if (!is_3d) {
      for (int player = 0; player < players; ++player) draw_stroke(&draw, LAYER_PLAYERS, &strokes[player], width_px);
      entity_traces_finish(&entities, &draw);
    }
    entity_traces_free(&entities);
  }

cleanup:
  free(tracks);
  free(playback);
  free(draw.scratch);
  free(rule_runtime);
  free(active_colors);
  free(have_position);
  free(positions3);
  if (strokes)
    for (int player = 0; player < players; ++player) free(strokes[player].points);
  free(strokes);
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
  rule->change_mode = PREDICTION_CHANGE_ANY;
  rule->target = property->kind == FT_VALUE_BOOL ? 1.0 : 0.0;
  rule->from = 0.0;
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

// A trigger's number, dragged: in whole steps for an integer property, and
// within the property's own range where it has one.
static bool rule_number(const char *label, double *value, const ft_prop_desc *property) {
  const bool scalar = property->kind == FT_VALUE_INT || property->kind == FT_VALUE_FLOAT;
  const bool bounded = scalar && property->min_value < property->max_value;
  const ImGuiSliderFlags flags = bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
  bool changed;
  if (property->kind == FT_VALUE_INT) {
    int number = (int)fmax(fmin(round(*value), (double)INT_MAX), (double)INT_MIN);
    changed = igDragInt(label, &number, 0.25f, bounded ? (int)property->min_value : 0, bounded ? (int)property->max_value : 0, "%d",
                        flags);
    if (changed) *value = number;
  } else {
    changed = igDragScalar(label, ImGuiDataType_Double, value, 0.1f, bounded ? &property->min_value : NULL,
                           bounded ? &property->max_value : NULL, "%.6g", flags);
  }
  if (!isfinite(*value)) *value = 0.0;
  return changed;
}

static bool render_rule_comparison(prediction_color_rule_t *rule, const ft_prop_desc *property) {
  const ft_value_kind kind = property->kind;
  bool changed = false;
  if (kind == FT_VALUE_BOOL) {
    if (rule->comparison != PREDICTION_COMPARE_EQUAL && rule->comparison != PREDICTION_COMPARE_CHANGED) {
      rule->comparison = PREDICTION_COMPARE_EQUAL;
      rule->target = rule->target != 0.0 ? 1.0 : 0.0;
      changed = true;
    }
    const char *preview = rule->comparison == PREDICTION_COMPARE_CHANGED
                              ? "Changes"
                              : (rule->target != 0.0 ? "Is true" : "Is false");
    if (igBeginCombo("Trigger", preview, 0)) {
      if (igSelectable_Bool("Is true", rule->comparison == PREDICTION_COMPARE_EQUAL && rule->target != 0.0, 0,
                            (ImVec2){0.f, 0.f})) {
        rule->comparison = PREDICTION_COMPARE_EQUAL;
        rule->target = 1.0;
        changed = true;
      }
      if (igSelectable_Bool("Is false", rule->comparison == PREDICTION_COMPARE_EQUAL && rule->target == 0.0, 0,
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

  static const char *names[] = {"Equals", "Is below", "Is above", "Changes", "Scales"};
  int comparison = (int)rule->comparison;
  if (comparison < 0 || comparison > PREDICTION_COMPARE_SCALE) comparison = PREDICTION_COMPARE_EQUAL;
  if (igBeginCombo("Trigger", names[comparison], 0)) {
    for (int i = 0; i <= PREDICTION_COMPARE_SCALE; ++i) {
      if (igSelectable_Bool(names[i], comparison == i, 0, (ImVec2){0.f, 0.f})) {
        rule->comparison = (prediction_rule_comparison_t)i;
        changed = true;
      }
    }
    igEndCombo();
  }
  if (rule->comparison == PREDICTION_COMPARE_SCALE) {
    changed |= rule_number("From", &rule->from, property);
    changed |= rule_number("To", &rule->target, property);
  } else if (rule->comparison != PREDICTION_COMPARE_CHANGED) {
    changed |= rule_number("Value", &rule->target, property);
  }
  return changed;
}

static bool render_rule_change_mode(prediction_color_rule_t *rule) {
  static const char *names[] = {
      "Any",
      "Increasing",
      "Decreasing",
  };

  bool changed = false;
  int mode = (int)rule->change_mode;

  if (mode < PREDICTION_CHANGE_ANY || mode > PREDICTION_CHANGE_DECREASING) {
    mode = PREDICTION_CHANGE_ANY;
    rule->change_mode = PREDICTION_CHANGE_ANY;
    changed = true;
  }

  if (igBeginCombo("Change type", names[mode], 0)) {
    for (int i = PREDICTION_CHANGE_ANY; i <= PREDICTION_CHANGE_DECREASING; ++i) {
      if (igSelectable_Bool(names[i], mode == i, 0, (ImVec2){0.f, 0.f})) {
        rule->change_mode = (prediction_change_mode_t)i;
        changed = true;
      }
    }

    igEndCombo();
  }

  return changed;
}

// Ultraforce's colours: blue while the player has a jump and dark red once it
// has none, and greener the faster it goes. Takes the game's "can_jump" and
// "velocity", or whichever of them it has.
static bool speed_and_jump_rules(prediction_line_t *line, const ft_entity_class *player_class) {
  const int jump = player_property_find(player_class, "can_jump");
  const int velocity = player_property_find(player_class, "velocity");
  const bool has_jump = jump >= 0 && player_class->props[jump].kind == FT_VALUE_BOOL;
  const bool has_velocity =
      velocity >= 0 && (player_class->props[velocity].kind == FT_VALUE_VEC2 || player_class->props[velocity].kind == FT_VALUE_VEC3);
  if (!has_jump && !has_velocity) return false;

  const float alpha = line->color[3];
  line->color_rule_count = 0;
  memset(line->color_rules, 0, sizeof(line->color_rules));
  if (has_jump) {
    for (int can = 1; can >= 0; --can) {
      prediction_color_rule_t *rule = &line->color_rules[line->color_rule_count++];
      color_rule_select_property(rule, &player_class->props[jump]);
      rule->target = can;
      memcpy(rule->color, can ? (float[4]){0.f, 0.f, 1.f, alpha} : (float[4]){0.75f, 0.f, 0.f, alpha}, sizeof(rule->color));
      rule->enabled = true;
    }
  }
  if (has_velocity) {
    // (DDNet's units a tick: a tee runs at 10, and hooks or grenades take it past 20)
    prediction_color_rule_t *rule = &line->color_rules[line->color_rule_count++];
    color_rule_select_property(rule, &player_class->props[velocity]);
    rule->comparison = PREDICTION_COMPARE_SCALE;
    rule->from = 0.0;
    rule->target = 75.0;
    memcpy(rule->color, (float[4]){0.f, 1.f, 0.f, 1.f}, sizeof(rule->color));
    rule->enabled = true;
  }
  return true;
}

static bool render_color_rules(prediction_line_t *line, int line_index, const ft_entity_class *player_class) {
  bool changed = false;
  if (!igTreeNode_Str("Color triggers")) return false;

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
      changed |= render_rule_comparison(rule, property);

      if (rule->comparison == PREDICTION_COMPARE_CHANGED && color_rule_is_velocity(rule))
        changed |= render_rule_change_mode(rule);
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
  igSameLine(0.f, 6.f);
  const bool can_preset =
      player_property_find(player_class, "can_jump") >= 0 || player_property_find(player_class, "velocity") >= 0;
  if (!can_preset) igBeginDisabled(true);
  if (igButton("Speed & jump", (ImVec2){0.f, 0.f}) && speed_and_jump_rules(line, player_class)) changed = true;
  if (!can_preset) igEndDisabled();
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
  config_changed |= igDragInt("Ticks ahead", &settings->length, 1.0f, 1, 2000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSetNextItemWidth(180.f * dpi);
  config_changed |= igSliderFloat("Line width", &settings->width_px, 1.f, 12.f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
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
    for (int group = 0; group < timeline->group_count; ++group)
      timeline->groups[group]->prediction_enabled = true;
    for (int track = 0; track < timeline->player_track_count; ++track)
      timeline->player_tracks[track].prediction_enabled = true;
    project_changed = true;
  }
  igSameLine(0.f, 6.f * dpi);
  if (igButton("None", (ImVec2){0.f, 0.f})) {
    for (int group = 0; group < timeline->group_count; ++group)
      timeline->groups[group]->prediction_enabled = false;
    for (int track = 0; track < timeline->player_track_count; ++track)
      timeline->player_tracks[track].prediction_enabled = false;
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
