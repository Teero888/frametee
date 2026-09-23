#include "input_lanes.h"

#include "widgets/imcol.h"
#include <math.h>
#include <stdio.h>

// Discrete integer fields with at most this many values get one row per value,
// which reads much better than a graph for things like a -1/0/1 direction.
#define ROWS_MAX_VALUES 12

static const float lane_palette[][3] = {
    {0.40f, 0.70f, 1.00f}, {1.00f, 0.60f, 0.35f}, {0.45f, 0.90f, 0.50f}, {0.90f, 0.45f, 0.80f},
    {0.95f, 0.85f, 0.35f}, {0.45f, 0.90f, 0.90f}, {1.00f, 0.45f, 0.45f}, {0.70f, 0.60f, 1.00f},
};

static ImU32 lane_color(const ft_input_field *field, int index) {
  if (field->color.a > 0.f) return igColorConvertFloat4ToU32((ImVec4){field->color.r, field->color.g, field->color.b, 1.f});
  const float *c = lane_palette[index % (int)(sizeof(lane_palette) / sizeof(lane_palette[0]))];
  return igColorConvertFloat4ToU32((ImVec4){c[0], c[1], c[2], 1.f});
}

static ImU32 with_alpha(ImU32 color, float alpha) {
  ImU32 a = (ImU32)(alpha * 255.f + 0.5f);
  return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

static void float_range(const ft_input_field *field, double *min, double *max) {
  *min = field->min_float;
  *max = field->max_float;
  if (*max <= *min) {
    *min = -1.0;
    *max = 1.0;
  }
}

static int add_lane(input_lane_t *out, int count, int max_lanes, input_lane_t lane) {
  if (count >= max_lanes) return count;
  out[count] = lane;
  return count + 1;
}

int input_lanes_build(const ft_input_schema *schema, const bool *vec2_xy, const bool *hidden, float scale, input_lane_t *out,
                      int max_lanes) {
  if (!schema) return 0;
  // Rows carry a text label, so they follow the font rather than a fixed size.
  const float bars_h = 20.f * scale, row_h = igGetFontSize() + 2.f * scale, curve_h = 90.f * scale;
  int count = 0;
  for (uint32_t i = 0; i < schema->field_count && i < INPUT_LANES_MAX_FIELDS; ++i) {
    const ft_input_field *field = &schema->fields[i];
    if (field->flags & (FT_INPUT_FLAG_INTERNAL | FT_INPUT_FLAG_EDITOR_HIDDEN)) continue;
    if (hidden && hidden[i]) continue;
    input_lane_t lane = {.field_index = (int)i, .field = field, .channel = INPUT_LANE_VALUE, .color = lane_color(field, (int)i)};

    switch (field->kind) {
    case FT_INPUT_BOOL:
      lane.style = INPUT_LANE_BARS;
      lane.min = 0;
      lane.max = 1;
      lane.height = bars_h;
      break;
    case FT_INPUT_ENUM:
    case FT_INPUT_INT: {
      int values = field->kind == FT_INPUT_ENUM ? (int)field->enum_count : field->max_value - field->min_value + 1;
      lane.min = field->min_value;
      lane.max = field->kind == FT_INPUT_ENUM ? field->min_value + values - 1 : field->max_value;
      if (lane.max <= lane.min) lane.max = lane.min + 100;
      if (values >= 2 && values <= ROWS_MAX_VALUES) {
        lane.style = INPUT_LANE_ROWS;
        lane.row_count = values;
        lane.height = fmaxf(bars_h, row_h * (float)values);
      } else {
        lane.style = INPUT_LANE_CURVE;
        lane.height = curve_h;
      }
      break;
    }
    case FT_INPUT_FLOAT:
      lane.style = INPUT_LANE_CURVE;
      float_range(field, &lane.min, &lane.max);
      lane.height = curve_h;
      break;
    case FT_INPUT_VEC2:
      lane.style = INPUT_LANE_CURVE;
      lane.height = curve_h;
      if (vec2_xy && vec2_xy[i]) {
        float_range(field, &lane.min, &lane.max);
        lane.channel = INPUT_LANE_X;
        count = add_lane(out, count, max_lanes, lane);
        lane.channel = INPUT_LANE_Y;
      } else {
        lane.channel = INPUT_LANE_ANGLE;
        lane.min = -180.0;
        lane.max = 180.0;
      }
      break;
    default:
      continue;
    }
    count = add_lane(out, count, max_lanes, lane);
  }
  return count;
}

double input_lane_read(game_host_t *host, const input_lane_t *lane, const input_record_t *record) {
  const ft_input_field *field = lane->field;
  switch (field->kind) {
  case FT_INPUT_FLOAT: return engine_input_get_float(host, record, lane->field_index);
  case FT_INPUT_VEC2: {
    ft_vec2 v = engine_input_get_vec2(host, record, lane->field_index);
    if (lane->channel == INPUT_LANE_X) return v.x;
    if (lane->channel == INPUT_LANE_Y) return v.y;
    if (v.x == 0.f && v.y == 0.f) return 0.0;
    // Screen y points down, so up is negative y; flip it so 90 degrees means up.
    // Negating a zero y would give -0, and atan2(-0, x < 0) is -180: straight
    // left must read as +180 so it is drawn at the top of the lane.
    const double up = v.y == 0.f ? 0.0 : -(double)v.y;
    return atan2(up, (double)v.x) * 180.0 / M_PI;
  }
  default: return (double)engine_input_get(host, record, lane->field_index);
  }
}

void input_lane_write(game_host_t *host, const input_lane_t *lane, input_record_t *record, double value) {
  const ft_input_field *field = lane->field;
  switch (field->kind) {
  case FT_INPUT_FLOAT: engine_input_set_float(host, record, lane->field_index, (float)value); break;
  case FT_INPUT_VEC2: {
    ft_vec2 v = engine_input_get_vec2(host, record, lane->field_index);
    if (lane->channel == INPUT_LANE_X) {
      v.x = (float)value;
    } else if (lane->channel == INPUT_LANE_Y) {
      v.y = (float)value;
    } else {
      // Rotating keeps the length. A zero vector has no length to keep, so it
      // gets a sensible one rather than staying at the origin.
      double length = hypot(v.x, v.y);
      if (length < 1e-3) length = 100.0;
      const double radians = value * M_PI / 180.0;
      v.x = (float)(length * cos(radians));
      v.y = (float)(-length * sin(radians));
      // -180 and +180 are the same vector, which reads back as +180. Nudge
      // the bottom end just below straight left so it stays at the bottom.
      // Wide ranges are stored as whole numbers by games like DDNet, so the
      // nudge has to survive rounding there.
      if (value < 0.0 && v.x < 0.f && fabsf(v.y) < 0.5f) {
        const double range = (double)field->max_float - (double)field->min_float;
        v.y = range >= 100.0 ? 1.f : fmaxf(fabsf(v.y), (float)(length * 1e-3));
      }
    }
    engine_input_set_vec2(host, record, lane->field_index, v);
    break;
  }
  default: engine_input_set(host, record, lane->field_index, llround(value)); break;
  }
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int row_of_value(const input_lane_t *lane, double value) {
  // Enums list top to bottom in declaration order; plain numbers put the
  // largest value on top, like a graph.
  int v = (int)llround(value);
  int row = lane->field->kind == FT_INPUT_ENUM ? v - (int)lane->min : (int)lane->max - v;
  return row < 0 ? 0 : (row >= lane->row_count ? lane->row_count - 1 : row);
}

static double value_of_row(const input_lane_t *lane, int row) {
  return lane->field->kind == FT_INPUT_ENUM ? lane->min + row : lane->max - row;
}

static float curve_y(const input_lane_t *lane, float top, double value) {
  const float pad = 3.f;
  const double t = (clampd(value, lane->min, lane->max) - lane->min) / (lane->max - lane->min);
  return top + pad + (float)(1.0 - t) * (lane->height - 2.f * pad);
}

double input_lane_value_at_y(const input_lane_t *lane, float top, float y) {
  if (lane->style == INPUT_LANE_BARS) return 1.0;
  if (lane->style == INPUT_LANE_ROWS) {
    int row = (int)floorf((y - top) / (lane->height / (float)lane->row_count));
    row = row < 0 ? 0 : (row >= lane->row_count ? lane->row_count - 1 : row);
    return value_of_row(lane, row);
  }
  const float pad = 3.f;
  const double t = 1.0 - clampd((y - top - pad) / (lane->height - 2.f * pad), 0.0, 1.0);
  double value = lane->min + t * (lane->max - lane->min);
  if (lane->field->kind == FT_INPUT_INT || lane->field->kind == FT_INPUT_ENUM) return (double)llround(value);
  // Snap to a step that suits the range: whole pixels for aim, tenths of a
  // degree for angles, finer for small float ranges.
  const double range = lane->max - lane->min;
  const double step = lane->channel == INPUT_LANE_ANGLE ? 0.1 : (range >= 100.0 ? 1.0 : range / 1000.0);
  return round(value / step) * step;
}

void input_lane_format(const input_lane_t *lane, double value, char *out, size_t out_size) {
  const ft_input_field *field = lane->field;
  switch (field->kind) {
  case FT_INPUT_BOOL: snprintf(out, out_size, "%s", value != 0.0 ? "on" : "off"); break;
  case FT_INPUT_ENUM: {
    const long long index = llround(value) - field->min_value;
    if (field->enum_labels && index >= 0 && index < (long long)field->enum_count)
      snprintf(out, out_size, "%s", field->enum_labels[index]);
    else
      snprintf(out, out_size, "%lld", llround(value));
    break;
  }
  case FT_INPUT_INT: snprintf(out, out_size, "%lld", llround(value)); break;
  case FT_INPUT_VEC2:
    if (lane->channel == INPUT_LANE_ANGLE)
      snprintf(out, out_size, "%.1f\xC2\xB0", value);
    else
      snprintf(out, out_size, "%.0f", value);
    break;
  default: snprintf(out, out_size, "%.3f", value); break;
  }
}

void input_lane_row_label(const input_lane_t *lane, int row, char *out, size_t out_size) {
  input_lane_format(lane, value_of_row(lane, row), out, out_size);
}

float input_lane_tick_x(const input_lane_view_t *view, int index) { return view->x0 + (float)index * view->px_per_tick; }

int input_lane_tick_at(const input_lane_view_t *view, float x) { return (int)floorf((x - view->x0) / view->px_per_tick); }

void input_lane_visible_range(const input_lane_view_t *view, int *first, int *last) {
  int a = input_lane_tick_at(view, view->clip_min_x);
  int b = input_lane_tick_at(view, view->clip_max_x);
  *first = a < 0 ? 0 : a;
  *last = b >= view->count ? view->count - 1 : b;
}

static double field_default(const input_lane_t *lane) {
  return lane->field->kind == FT_INPUT_FLOAT ? lane->field->default_float : lane->field->default_value;
}

static void draw_runs(ImDrawList *dl, game_host_t *host, const input_lane_t *lane, const input_record_t *records,
                      const input_lane_view_t *view, float top, int first, int last) {
  const float pad = 2.f;
  const float row_h = lane->style == INPUT_LANE_ROWS ? lane->height / (float)lane->row_count : lane->height;
  const bool edge_marks = lane->field->flags & (FT_INPUT_FLAG_LATCHED | FT_INPUT_FLAG_TRIGGER);
  const double def = field_default(lane);
  const ImU32 text_color = IM_COL32(15, 15, 18, 255);

  // Neighbouring ticks with the same value become one rectangle, which keeps
  // a zoomed-out 100k-tick snippet as cheap to draw as a zoomed-in one.
  int start = first;
  double run_value = input_lane_read(host, lane, &records[first]);
  for (int i = first + 1; i <= last + 1; ++i) {
    const double value = i <= last ? input_lane_read(host, lane, &records[i]) : NAN;
    if (i <= last && value == run_value) continue;

    const float x0 = input_lane_tick_x(view, start), x1 = input_lane_tick_x(view, i);
    const float gap = view->px_per_tick >= 4.f ? 1.f : 0.f;
    if (lane->style == INPUT_LANE_BARS) {
      if (run_value != 0.0) {
        ImDrawList_AddRectFilled(dl, (ImVec2){x0 + gap, top + pad}, (ImVec2){x1 - gap, top + lane->height - pad}, lane->color, 2.f, 0);
        if (edge_marks) ImDrawList_AddRectFilled(dl, (ImVec2){x0, top}, (ImVec2){x0 + 2.f, top + lane->height}, IM_COL32_WHITE, 0.f, 0);
      }
    } else {
      const int row = row_of_value(lane, run_value);
      const float y0 = top + (float)row * row_h;
      // The resting value is still drawn, just faintly, so the lane never
      // looks empty while making the interesting parts stand out.
      const bool resting = run_value == def;
      ImDrawList_AddRectFilled(dl, (ImVec2){x0 + gap, y0 + 1.f}, (ImVec2){x1 - gap, y0 + row_h - 1.f},
                               resting ? with_alpha(lane->color, 0.25f) : lane->color, 2.f, 0);
      char label[64];
      input_lane_format(lane, run_value, label, sizeof(label));
      const ImVec2 size = igCalcTextSize(label, NULL, false, 0.f);
      if (!resting && size.x + 6.f < x1 - x0 && size.y <= row_h + 2.f)
        ImDrawList_AddText_Vec2(dl, (ImVec2){fmaxf(x0, view->clip_min_x) + 3.f, y0 + (row_h - size.y) * 0.5f}, text_color, label, NULL);
    }
    start = i;
    run_value = value;
  }
}

static void draw_curve(ImDrawList *dl, game_host_t *host, const input_lane_t *lane, const input_record_t *records,
                       const input_lane_view_t *view, float top, int first, int last) {
  const double zero = clampd(0.0, lane->min, lane->max);
  const float base_y = curve_y(lane, top, zero);
  const ImU32 fill = with_alpha(lane->color, 0.22f);
  ImDrawList_AddLine(dl, (ImVec2){view->clip_min_x, base_y}, (ImVec2){view->clip_max_x, base_y}, igGetColorU32_Col(ImGuiCol_Separator, 0.6f), 1.f);

  // Zoomed out, several ticks share a pixel column; drawing their min..max as
  // one bar keeps spikes visible without drawing every tick.
  const int bucket = view->px_per_tick >= 1.f ? 1 : (int)ceilf(1.f / view->px_per_tick);
  float prev_y = 0.f;
  double prev_value = 0.0;
  bool have_prev = false;
  for (int i = first; i <= last; i += bucket) {
    const int end = i + bucket - 1 <= last ? i + bucket - 1 : last;
    double lo = INFINITY, hi = -INFINITY, value = 0.0;
    for (int j = i; j <= end; ++j) {
      value = input_lane_read(host, lane, &records[j]);
      lo = fmin(lo, value);
      hi = fmax(hi, value);
    }
    const float x0 = input_lane_tick_x(view, i), x1 = input_lane_tick_x(view, end + 1);
    const float y_lo = curve_y(lane, top, lo), y_hi = curve_y(lane, top, hi);
    const float y = curve_y(lane, top, value);
    ImDrawList_AddRectFilled(dl, (ImVec2){x0, fminf(base_y, y_hi)}, (ImVec2){x1, fmaxf(base_y, y_lo)}, fill, 0.f, 0);
    if (bucket > 1) {
      ImDrawList_AddLine(dl, (ImVec2){x0, y_hi}, (ImVec2){x0, y_lo + 1.f}, lane->color, 1.f);
    } else {
      // An angle crossing +-180 is a tiny turn, not a sweep across the lane.
      const bool wraps = lane->channel == INPUT_LANE_ANGLE && fabs(value - prev_value) > 180.0;
      if (have_prev && !wraps) ImDrawList_AddLine(dl, (ImVec2){x0, prev_y}, (ImVec2){x0, y}, lane->color, 1.5f);
      ImDrawList_AddLine(dl, (ImVec2){x0, y}, (ImVec2){x1, y}, lane->color, 2.f);
    }
    prev_y = y;
    prev_value = value;
    have_prev = true;
  }

  // The range is labelled inside the graph, at its left edge, so the lane
  // header only has to fit the field's name.
  char hi[32], lo[32];
  input_lane_format(lane, lane->max, hi, sizeof(hi));
  input_lane_format(lane, lane->min, lo, sizeof(lo));
  const ImU32 dim = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.9f);
  const float x = view->clip_min_x + 4.f;
  ImDrawList_AddText_Vec2(dl, (ImVec2){x, top + 1.f}, dim, hi, NULL);
  ImDrawList_AddText_Vec2(dl, (ImVec2){x, top + lane->height - igGetFontSize() - 1.f}, dim, lo, NULL);
}

void input_lane_draw(ImDrawList *dl, game_host_t *host, const input_lane_t *lane, const input_record_t *records,
                     const input_lane_view_t *view, float top) {
  if (view->count <= 0) return;
  int first, last;
  input_lane_visible_range(view, &first, &last);
  if (first > last) return;

  if (lane->style == INPUT_LANE_ROWS) {
    // Faint row separators so a value's row is easy to find.
    const float row_h = lane->height / (float)lane->row_count;
    for (int r = 1; r < lane->row_count; ++r)
      ImDrawList_AddLine(dl, (ImVec2){view->clip_min_x, top + r * row_h}, (ImVec2){view->clip_max_x, top + r * row_h},
                         igGetColorU32_Col(ImGuiCol_Separator, 0.25f), 1.f);
  }
  if (lane->style == INPUT_LANE_CURVE)
    draw_curve(dl, host, lane, records, view, top, first, last);
  else
    draw_runs(dl, host, lane, records, view, top, first, last);
}
