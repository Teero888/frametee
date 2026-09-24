// The snippet editor: a snippet's inputs as lanes, ticks running left to right.
//
// Every lane comes from the active game's input schema (see input_lanes.c), so
// the editor never knows what a field means. Editing is done by painting over
// lanes rather than by poking one widget per tick: drag across a button lane to
// press or release it, across a value lane to draw it. A tick range picked on
// the ruler is the target for copy, paste, reset and the inspector below.

#include "snippet_editor.h"

#include "input_lanes.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_interaction.h"
#include "timeline/timeline_model.h"
#include "timeline/timeline_recordings.h"
#include "user_interface.h"
#include "widgets/imcol.h"
#include <engine/game_host.h>
#include <engine/input_record.h>
#include <float.h>
#include <frametee/icons.h>
#include <limits.h>
#include <math.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

static struct {
  int snippet_id;

  // View: ticks are snippet-relative indices, view_start is the index at the
  // left edge of the lanes.
  float px_per_tick;
  float view_start;
  bool fit_pending;
  bool follow_playhead;
  int last_playhead_index;

  // Tick range selection, inclusive, in snippet-relative indices.
  bool has_selection;
  int selection_anchor, selection_end;
  bool scrubbing; // dragging the playhead along the ruler
  bool selecting; // Ctrl+dragging out a tick range, on the ruler or a lane

  // Zoom requests from the toolbar, applied where the lane width is known.
  float zoom_request; // factor, 0 for none
  bool zoom_to_selection_pending;

  // Painting on a lane. The lane is remembered by field and channel because
  // the lane array is rebuilt every frame.
  bool painting;
  int paint_field;
  input_lane_channel_t paint_channel;
  double paint_value; // bars and rows paint one value for the whole stroke
  int press_index, last_index;
  double press_value, last_value;
  bool paint_line; // Shift: a straight segment from the press point

  // Right-click menu target.
  int menu_field;
  input_lane_channel_t menu_channel;
  int menu_index;

  // Per-field lane options.
  bool vec2_xy[INPUT_LANES_MAX_FIELDS];
  // Graph lane heights the user dragged to, before UI scaling; 0 is the default.
  float curve_height[INPUT_LANES_MAX_FIELDS][4];
  bool hidden[INPUT_LANES_MAX_FIELDS];

  // One undoable action covers a whole stroke or widget drag, so the
  // before-state is captured once when it starts.
  bool action_in_progress;
  input_record_t *action_before_states;
  int action_before_count;
  // Lowest index changed this frame, so the physics recomputes live while a
  // stroke is still in progress.
  int dirty_from;

  input_record_t *clipboard;
  int clipboard_count;


  // Viewport aim picking: the next world click aims these ticks at it.
  bool picking_position;
  int pick_snippet_id;
  int pick_first, pick_last;

  // A demo snippet is shown read-only, through a stand-in holding the input its player most likely
  // held (see draw_playback_view). tick_flags are its ft_recording_tick_flags, one per tick.
  bool read_only;
  const uint8_t *tick_flags;
} ed = {.snippet_id = -1, .follow_playhead = true, .dirty_from = INT_MAX, .pick_snippet_id = -1};

// The stand-in for the demo snippet in view, see demo_stand_in.
static struct {
  int snippet_id, recording_tick, count;
  input_record_t *records;
  uint8_t *flags;
  input_snippet_t stand_in;
} demo = {.snippet_id = -1};

#define PLAYHEAD_COLOR IM_COL32(255, 90, 90, 255)

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool snippet_editor_is_picking(void) { return ed.picking_position; }

static void snippet_editor_cancel_pick(void) {
  ed.picking_position = false;
  ed.pick_snippet_id = -1;
}

static void reset_editor_state(void) {
  snippet_editor_cancel_pick();
  ed.has_selection = false;
  ed.scrubbing = false;
  ed.selecting = false;
  ed.zoom_request = 0.f;
  ed.zoom_to_selection_pending = false;
  ed.painting = false;
  ed.action_in_progress = false;
  free(ed.action_before_states);
  ed.action_before_states = NULL;
  ed.action_before_count = 0;
  ed.dirty_from = INT_MAX;
  ed.fit_pending = true;
  ed.last_playhead_index = INT_MIN;
}

void snippet_editor_reset(void) {
  reset_editor_state();
  ed.snippet_id = -1;
}

void snippet_editor_cleanup(void) {
  snippet_editor_reset();
  free(demo.records);
  free(demo.flags);
  demo.records = NULL;
  demo.flags = NULL;
  demo.snippet_id = -1;
  free(ed.clipboard);
  ed.clipboard = NULL;
  ed.clipboard_count = 0;
}

void snippet_editor_open(ui_handler_t *ui, int snippet_id) {
  if (!ui) return;
  if (model_find_snippet_by_id(&ui->timeline, snippet_id, NULL)) {
    interaction_clear_selection(&ui->timeline);
    interaction_add_snippet_to_selection(&ui->timeline, snippet_id);
    ui->timeline.active_snippet_id = snippet_id;
  }
  ui->show_snippet_editor_window = true;
  ui->focus_snippet_editor_window = true;
}

// ---------------------------------------------------------------------------
// Undo

static bool begin_action(const input_snippet_t *snippet) {
  if (ed.read_only) return false; // a demo's lanes only show what happened
  if (ed.action_in_progress) return ed.action_before_states != NULL;
  ed.action_in_progress = true;
  ed.action_before_count = snippet->input_count;
  free(ed.action_before_states);
  ed.action_before_states = malloc(sizeof(input_record_t) * (size_t)snippet->input_count);
  if (!ed.action_before_states) {
    ed.action_in_progress = false;
    ed.action_before_count = 0;
    return false;
  }
  memcpy(ed.action_before_states, snippet_window(snippet), sizeof(input_record_t) * (size_t)snippet->input_count);
  return true;
}

// Closes the action, turning the whole edit into one undo entry.
static void end_action(ui_handler_t *ui, input_snippet_t *snippet) {
  if (!ed.action_in_progress) return;
  ed.action_in_progress = false;
  const bool same_length = ed.action_before_states && ed.action_before_count == snippet->input_count;
  int first_changed = snippet->input_count;
  if (same_length) {
    for (int i = 0; i < snippet->input_count; ++i) {
      if (memcmp(&ed.action_before_states[i], &snippet_window(snippet)[i], sizeof(input_record_t)) != 0) {
        first_changed = i;
        break;
      }
    }
  }

  if (same_length && first_changed < snippet->input_count) {
    // Every tick in the snippet is handed to the undo command; it stores the
    // before and after states so redo is exact.
    int *indices = malloc(sizeof(int) * (size_t)snippet->input_count);
    if (indices) {
      for (int i = 0; i < snippet->input_count; ++i)
        indices[i] = i;
      undo_command_t *command =
          create_edit_inputs_command(snippet, indices, snippet->input_count, ed.action_before_states, snippet_window(snippet));
      if (command) undo_manager_register_command(&ui->undo_manager, command);
      free(indices);
    }
    model_recalc_snippet_physics(&ui->timeline, snippet, snippet->start_tick + first_changed);
    ui_mark_unsaved(ui);
  }
  free(ed.action_before_states);
  ed.action_before_states = NULL;
  ed.action_before_count = 0;
}

bool snippet_editor_take_world_click(ui_handler_t *ui, float world_x, float world_y) {
  if (!ed.picking_position) return false;

  const int snippet_id = ed.pick_snippet_id;
  snippet_editor_cancel_pick();
  if (!ui) return true;

  int track_index = -1;
  timeline_state_t *ts = &ui->timeline;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, &track_index);
  if (!snippet) return true;
  game_host_t *host = &ui->gfx_handler->game_host;

  const int aim_field = engine_input_cursor_field();
  if (aim_field < 0) return true;
  const int group_index = model_track_group_index(ts, track_index);
  const int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return true;

  // The snippet may have been trimmed between arming the pick and the click.
  const int first = ed.pick_first < 0 ? 0 : ed.pick_first;
  const int last = ed.pick_last < snippet->input_count ? ed.pick_last : snippet->input_count - 1;
  if (first > last || !begin_action(snippet)) return true;

  for (int row = first; row <= last; ++row) {
    const int global_tick = snippet->start_tick + row + ts->groups[group_index]->start_offset;
    const ft_world *world = model_group_world_at_tick(ts, group_index, global_tick);
    if (!world) continue;
    ft_player_view player;
    if (!gh_world_player_view(host, world, local_index, &player)) continue;

    ft_vec2 aim = {
        roundf((world_x - player.position.x) * 32.f),
        roundf((world_y - player.position.y) * 32.f),
    };
    if (aim.x == 0.f && aim.y == 0.f) aim.x = 1.f;
    engine_input_set_vec2(host, &snippet_window(snippet)[row], aim_field, aim);
  }

  end_action(ui, snippet);
  return true;
}

static void mark_dirty(int index) {
  if (index < ed.dirty_from) ed.dirty_from = index;
}

// ---------------------------------------------------------------------------
// Field helpers

static bool field_editable(const ft_input_schema *schema, int field) {
  if (field < 0 || field >= (int)schema->field_count) return false;
  if (schema->fields[field].flags & (FT_INPUT_FLAG_INTERNAL | FT_INPUT_FLAG_EDITOR_HIDDEN)) return false;
  return field >= INPUT_LANES_MAX_FIELDS || !ed.hidden[field];
}

static const char *field_name(const ft_input_field *field) { return field->display_name ? field->display_name : field->id; }

static void copy_field(game_host_t *host, const ft_input_field *field, int index, const input_record_t *src, input_record_t *dst) {
  switch (field->kind) {
  case FT_INPUT_FLOAT: engine_input_set_float(host, dst, index, engine_input_get_float(host, src, index)); break;
  case FT_INPUT_VEC2: engine_input_set_vec2(host, dst, index, engine_input_get_vec2(host, src, index)); break;
  default: engine_input_set(host, dst, index, engine_input_get(host, src, index)); break;
  }
}

static bool field_equal(game_host_t *host, const ft_input_field *field, int index, const input_record_t *a, const input_record_t *b) {
  switch (field->kind) {
  case FT_INPUT_FLOAT: return engine_input_get_float(host, a, index) == engine_input_get_float(host, b, index);
  case FT_INPUT_VEC2: {
    ft_vec2 va = engine_input_get_vec2(host, a, index), vb = engine_input_get_vec2(host, b, index);
    return va.x == vb.x && va.y == vb.y;
  }
  default: return engine_input_get(host, a, index) == engine_input_get(host, b, index);
  }
}

static const input_lane_t *find_lane(const input_lane_t *lanes, int count, int field, input_lane_channel_t channel) {
  for (int i = 0; i < count; ++i)
    if (lanes[i].field_index == field && lanes[i].channel == channel) return &lanes[i];
  return NULL;
}

// ---------------------------------------------------------------------------
// Edits. Each one assumes an action is open and records what it touched.

static void selection_bounds(int *first, int *last) {
  *first = ed.selection_anchor < ed.selection_end ? ed.selection_anchor : ed.selection_end;
  *last = ed.selection_anchor < ed.selection_end ? ed.selection_end : ed.selection_anchor;
}

// Writes a straight segment from (a, va) to (b, vb). Bars and rows pass the
// same value at both ends, which makes this a plain fill.
static void write_segment(game_host_t *host, const input_lane_t *lane, input_record_t *records, int a, double va, int b, double vb) {
  const int from = a < b ? a : b, to = a < b ? b : a;
  for (int i = from; i <= to; ++i) {
    const double t = a == b ? 1.0 : (double)(i - a) / (double)(b - a);
    double value = va + (vb - va) * t;
    if (lane->style != INPUT_LANE_CURVE || lane->field->kind == FT_INPUT_INT) value = round(value);
    input_record_t before = records[i];
    input_lane_write(host, lane, &records[i], value);
    if (memcmp(&before, &records[i], sizeof(before)) != 0) mark_dirty(i);
  }
}

static void reset_range(game_host_t *host, const ft_input_schema *schema, input_record_t *records, int first, int last, int only_field) {
  input_record_t defaults;
  memset(&defaults, 0, sizeof(defaults));
  engine_input_default(host, &defaults);
  for (int i = first; i <= last; ++i) {
    for (uint32_t f = 0; f < schema->field_count; ++f) {
      if (!field_editable(schema, (int)f) || (only_field >= 0 && (int)f != only_field)) continue;
      if (field_equal(host, &schema->fields[f], (int)f, &records[i], &defaults)) continue;
      copy_field(host, &schema->fields[f], (int)f, &defaults, &records[i]);
      mark_dirty(i);
    }
  }
}

static void copy_selection(const input_snippet_t *snippet) {
  if (!ed.has_selection) return;
  int first, last;
  selection_bounds(&first, &last);
  const int count = last - first + 1;
  input_record_t *copy = realloc(ed.clipboard, sizeof(input_record_t) * (size_t)count);
  if (!copy) return;
  memcpy(copy, snippet_window(snippet) + first, sizeof(input_record_t) * (size_t)count);
  ed.clipboard = copy;
  ed.clipboard_count = count;
}

// Pastes at `at`, clipped to the snippet. Only fields the editor shows are
// written, so a hidden lane is never changed behind the user's back.
static void paste_clipboard(game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet, int at, int only_field) {
  if (!ed.clipboard || at < 0 || at >= snippet->input_count) return;
  input_record_t *records = snippet_window(snippet);
  for (int i = 0; i < ed.clipboard_count && at + i < snippet->input_count; ++i) {
    for (uint32_t f = 0; f < schema->field_count; ++f) {
      if (!field_editable(schema, (int)f) || (only_field >= 0 && (int)f != only_field)) continue;
      if (field_equal(host, &schema->fields[f], (int)f, &ed.clipboard[i], &records[at + i])) continue;
      copy_field(host, &schema->fields[f], (int)f, &ed.clipboard[i], &records[at + i]);
      mark_dirty(at + i);
    }
  }
  ed.has_selection = true;
  ed.selection_anchor = at;
  ed.selection_end = clampi(at + ed.clipboard_count - 1, at, snippet->input_count - 1);
}

// ---------------------------------------------------------------------------
// View

static void begin_selection(int index) {
  ed.selecting = true;
  ed.has_selection = true;
  ed.selection_anchor = ed.selection_end = index;
}

static void set_playhead(timeline_state_t *ts, int group, const input_snippet_t *snippet, int index) {
  if (ts->recording || group < 0 || group >= ts->group_count) return;
  ts->is_playing = false;
  ts->current_tick = model_clamp_global_tick_for_group(ts, group, ts->groups[group]->start_offset + snippet->start_tick + index);
}

static float min_zoom(int count, float width) { return fminf(1.f, width / (float)(count > 0 ? count : 1)); }

static void clamp_view(int count, float width) {
  const float scale = gfx_get_ui_scale();
  const float lo = min_zoom(count, width), hi = 48.f * scale;
  ed.px_per_tick = fmaxf(lo, fminf(hi, ed.px_per_tick));
  // A little room past the end so the last tick is never jammed against the edge.
  const float visible = width / ed.px_per_tick;
  const float max_start = fmaxf(0.f, (float)count - visible * 0.9f);
  ed.view_start = fmaxf(0.f, fminf(max_start, ed.view_start));
}

static void zoom_at(float factor, float anchor_x, float canvas_x, int count, float width) {
  const float tick = ed.view_start + (anchor_x - canvas_x) / ed.px_per_tick;
  ed.px_per_tick *= factor;
  clamp_view(count, width);
  ed.view_start = tick - (anchor_x - canvas_x) / ed.px_per_tick;
  clamp_view(count, width);
}

static int nice_step(float px_per_tick, float min_spacing) {
  static const int steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 25000, 50000, 100000};
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
    if ((float)steps[i] * px_per_tick >= min_spacing) return steps[i];
  return 250000;
}

// Scrolls while a drag holds the mouse past either edge of the lanes.
// The speed grows with how far past the edge the mouse is, up to a cap, and is
// measured in pixels so it feels the same at every zoom level.
static void edge_scroll(float mouse_x, float canvas_x, float width, int count) {
  const float dt = igGetIO_Nil()->DeltaTime;
  const float scale = gfx_get_ui_scale();
  float over = 0.f;
  if (mouse_x < canvas_x) over = mouse_x - canvas_x;
  if (mouse_x > canvas_x + width) over = mouse_x - (canvas_x + width);
  if (over == 0.f) return;
  const float max_over = 120.f * scale;
  over = fmaxf(-max_over, fminf(max_over, over));
  ed.view_start += over * 5.f * dt / ed.px_per_tick;
  clamp_view(count, width);
}

// Tick under the mouse, held to what is on screen. A drag that leaves the
// lanes stops at their edge and edge_scroll brings more ticks into view,
// instead of jumping straight to a tick that is not visible.
static int pointer_index(const input_lane_view_t *view, float mouse_x) {
  const float right = fminf(view->clip_max_x, input_lane_tick_x(view, view->count)) - 0.5f;
  const float x = fmaxf(view->clip_min_x, fminf(right, mouse_x));
  return clampi(input_lane_tick_at(view, x), 0, view->count - 1);
}

// ---------------------------------------------------------------------------
// Inspector: exact values for the selection, or for the tick under the playhead.

static void inspector_apply(game_host_t *host, input_snippet_t *snippet, int first, int last, const ft_input_field *field, int index,
                            long long value, float fvalue) {
  begin_action(snippet);
  input_record_t *records = snippet_window(snippet);
  for (int i = first; i <= last; ++i) {
    input_record_t before = records[i];
    if (field->kind == FT_INPUT_FLOAT)
      engine_input_set_float(host, &records[i], index, fvalue);
    else
      engine_input_set(host, &records[i], index, value);
    if (memcmp(&before, &records[i], sizeof(before)) != 0) mark_dirty(i);
  }
}

static void inspector_vec2(game_host_t *host, input_snippet_t *snippet, int first, int last, const ft_input_field *field, int index,
                           bool mixed, float indent) {
  input_record_t *records = snippet_window(snippet);
  ft_vec2 v = engine_input_get_vec2(host, &records[first], index);
  float xy[2] = {v.x, v.y};
  const input_lane_t angle_lane = {.field_index = index, .field = field, .channel = INPUT_LANE_ANGLE};
  float angle = (float)input_lane_read(host, &angle_lane, &records[first]);
  float length = hypotf(v.x, v.y);

  // X, Y, angle and length share one row; the pick button follows when the
  // field has one, dropping to its own line if the window is narrow.
  const ImGuiStyle *style = igGetStyle();
  const bool can_pick = index == engine_input_cursor_field();
  const char *pick_label = ed.picking_position && ed.pick_snippet_id == snippet->id ? ICON_FA_XMARK " Cancel pick"
                                                                                    : ICON_FA_CROSSHAIRS " Pick in viewport";
  const float pick_w = igCalcTextSize(pick_label, NULL, true, 0.f).x + style->FramePadding.x * 2.f;
  const float avail = igGetContentRegionAvail().x;
  const float row_room = avail - style->ItemSpacing.x * 3.f - (can_pick ? pick_w + style->ItemSpacing.x : 0.f);
  const float unit = fminf(130.f * gfx_get_ui_scale(), row_room / 4.f);
  const bool pick_wraps = unit < 55.f * gfx_get_ui_scale();
  const float w = pick_wraps ? (avail - style->ItemSpacing.x * 3.f) / 4.f : unit;

  igPushItemFlag(ImGuiItemFlags_MixedValue, mixed);
  igSetNextItemWidth(w * 2.f + style->ItemSpacing.x);
  const bool xy_changed = igDragFloat2("##xy", xy, 1.f, field->min_float, field->max_float, "%.0f", 0);
  igSameLine(0, -1.f);
  igSetNextItemWidth(w);
  const bool angle_changed = igDragFloat("##angle", &angle, 0.1f, -180.f, 180.f, "%.1f\xC2\xB0", 0);
  igSetItemTooltip("0\xC2\xB0 is right, 90\xC2\xB0 is up");
  igSameLine(0, -1.f);
  igSetNextItemWidth(w);
  const bool length_changed = igDragFloat("##length", &length, 1.f, 0.f, field->max_float > 0.f ? field->max_float * 1.5f : FLT_MAX,
                                          "len %.0f", 0);
  igPopItemFlag();

  // The field the recording cursor fills can also be aimed at a point in the
  // viewport, for the same ticks this inspector is showing.
  if (can_pick) {
    if (pick_wraps)
      igSetCursorPosX(indent);
    else
      igSameLine(0, -1.f);
    if (igButton(pick_label, (ImVec2){0, 0})) {
      if (ed.picking_position && ed.pick_snippet_id == snippet->id) {
        snippet_editor_cancel_pick();
      } else {
        ed.picking_position = true;
        ed.pick_snippet_id = snippet->id;
        ed.pick_first = first;
        ed.pick_last = last;
      }
    }
  }

  if (!xy_changed && !angle_changed && !length_changed) return;

  begin_action(snippet);
  for (int i = first; i <= last; ++i) {
    input_record_t before = records[i];
    if (xy_changed) {
      engine_input_set_vec2(host, &records[i], index, (ft_vec2){xy[0], xy[1]});
    } else if (angle_changed) {
      input_lane_write(host, &angle_lane, &records[i], angle);
    } else {
      ft_vec2 cur = engine_input_get_vec2(host, &records[i], index);
      const float cur_len = hypotf(cur.x, cur.y);
      const ft_vec2 dir = cur_len > 1e-3f ? (ft_vec2){cur.x / cur_len, cur.y / cur_len} : (ft_vec2){1.f, 0.f};
      engine_input_set_vec2(host, &records[i], index, (ft_vec2){dir.x * length, dir.y * length});
    }
    if (memcmp(&before, &records[i], sizeof(before)) != 0) mark_dirty(i);
  }
}

// The inspector is laid out as rows sharing one name column: a row per vector
// field, a row per value field, then every button as a checkbox in one row
// that wraps. Its shape depends only on the schema and the width, so the
// height is known before drawing and the lanes can be sized around it.
typedef struct {
  float label_w;  // name column; buttons carry their own names
  float value_w;  // value widgets match the width of a vector's X / Y pair
  float row_gap;  // at least a few pixels, so rows never touch
  int vec2_count, value_count, bool_lines;
} inspector_layout_t;

static float bool_item_width(const ft_input_field *field) {
  return igGetFrameHeight() + igGetStyle()->ItemInnerSpacing.x + igCalcTextSize(field_name(field), NULL, false, 0.f).x;
}

static float bool_gap(void) { return 20.f * gfx_get_ui_scale(); }

static inspector_layout_t inspector_layout(const ft_input_schema *schema, float width) {
  const float scale = gfx_get_ui_scale();
  const ImGuiStyle *style = igGetStyle();
  inspector_layout_t layout = {.row_gap = fmaxf(style->ItemSpacing.y, 6.f * scale)};
  for (uint32_t f = 0; f < schema->field_count; ++f) {
    if (!field_editable(schema, (int)f)) continue;
    const ft_input_field *field = &schema->fields[f];
    if (field->kind == FT_INPUT_BOOL) continue;
    layout.label_w = fmaxf(layout.label_w, igCalcTextSize(field_name(field), NULL, false, 0.f).x);
    if (field->kind == FT_INPUT_VEC2)
      ++layout.vec2_count;
    else
      ++layout.value_count;
  }
  if (layout.label_w > 0.f) layout.label_w += style->ItemSpacing.x * 2.f;
  const float room = fmaxf(1.f, width - layout.label_w);
  layout.value_w = fminf(260.f * scale + style->ItemSpacing.x, room);

  // Buttons flow left to right and wrap, exactly as draw_inspector places them.
  float x = 0.f;
  for (uint32_t f = 0; f < schema->field_count; ++f) {
    if (!field_editable(schema, (int)f) || schema->fields[f].kind != FT_INPUT_BOOL) continue;
    const float w = bool_item_width(&schema->fields[f]);
    if (layout.bool_lines == 0 || x + bool_gap() + w > room) {
      ++layout.bool_lines;
      x = w;
    } else {
      x += bool_gap() + w;
    }
  }
  return layout;
}

static float inspector_height(const ft_input_schema *schema, float width) {
  const inspector_layout_t layout = inspector_layout(schema, width);
  // A vector row may wrap its pick button onto a second line when narrow.
  const int vec2_lines = layout.vec2_count * (width - layout.label_w < 420.f * gfx_get_ui_scale() ? 2 : 1);
  const int rows = 1 + vec2_lines + layout.value_count + layout.bool_lines;
  return (igGetFrameHeight() + layout.row_gap) * (float)rows + layout.row_gap;
}

static void inspector_value(game_host_t *host, input_snippet_t *snippet, int first, int last, const ft_input_field *field, int index,
                            bool mixed, float width) {
  input_record_t *records = snippet_window(snippet);
  igSetNextItemWidth(width);
  switch (field->kind) {
  case FT_INPUT_ENUM: {
    const long long current = engine_input_get(host, &records[first], index) - field->min_value;
    const char *preview = mixed ? "(mixed)"
                          : (field->enum_labels && current >= 0 && current < (long long)field->enum_count) ? field->enum_labels[current]
                                                                                                        : "?";
    if (igBeginCombo("##v", preview, 0)) {
      for (uint32_t e = 0; e < field->enum_count && field->enum_labels; ++e) {
        if (igSelectable_Bool(field->enum_labels[e], !mixed && (long long)e == current, 0, (ImVec2){0, 0}))
          inspector_apply(host, snippet, first, last, field, index, field->min_value + (long long)e, 0.f);
      }
      igEndCombo();
    }
    break;
  }
  case FT_INPUT_INT: {
    int value = (int)engine_input_get(host, &records[first], index);
    const float speed = field->max_value - field->min_value > 1000 ? 16.f : 0.1f;
    if (igDragInt("##v", &value, speed, field->min_value, field->max_value, "%d", ImGuiSliderFlags_AlwaysClamp))
      inspector_apply(host, snippet, first, last, field, index, value, 0.f);
    break;
  }
  case FT_INPUT_FLOAT: {
    float value = engine_input_get_float(host, &records[first], index);
    if (igDragFloat("##v", &value, 0.01f, field->min_float, field->max_float, "%.3f", 0))
      inspector_apply(host, snippet, first, last, field, index, 0, value);
    break;
  }
  default: break;
  }
}

static bool range_mixed(game_host_t *host, const ft_input_field *field, int index, const input_record_t *records, int first, int last) {
  for (int i = first + 1; i <= last; ++i)
    if (!field_equal(host, field, index, &records[first], &records[i])) return true;
  return false;
}

// Starts a row: the field's name in the name column, the cursor after it.
static void inspector_row_label(const ft_input_field *field, float x0, float label_w) {
  igAlignTextToFramePadding();
  igTextUnformatted(field_name(field), NULL);
  if (field->description) igSetItemTooltip("%s", field->description);
  igSameLine(0.f, 0.f);
  igSetCursorPosX(x0 + label_w);
}

static void draw_inspector(game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet, int playhead_index) {
  int first = -1, last = -1;
  if (ed.has_selection) {
    selection_bounds(&first, &last);
  } else if (playhead_index >= 0 && playhead_index < snippet->input_count) {
    first = last = playhead_index;
  }

  const inspector_layout_t layout = inspector_layout(schema, igGetContentRegionAvail().x);
  igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){igGetStyle()->ItemSpacing.x, layout.row_gap});
  igDummy((ImVec2){0.f, 0.f});
  igAlignTextToFramePadding();
  if (first < 0)
    igTextDisabled("Move the playhead into the snippet, or Ctrl+drag to select ticks.");
  else if (first == last)
    igText("Tick %d", snippet->start_tick + first);
  else
    igText("Ticks %d \xE2\x80\x93 %d  (%d selected)", snippet->start_tick + first, snippet->start_tick + last, last - first + 1);
  if (first < 0) {
    igPopStyleVar(1);
    return;
  }

  input_record_t *records = snippet_window(snippet);
  const float x0 = igGetCursorPosX();

  for (uint32_t f = 0; f < schema->field_count; ++f) {
    const ft_input_field *field = &schema->fields[f];
    if (!field_editable(schema, (int)f) || field->kind != FT_INPUT_VEC2) continue;
    igPushID_Int((int)f);
    inspector_row_label(field, x0, layout.label_w);
    inspector_vec2(host, snippet, first, last, field, (int)f, range_mixed(host, field, (int)f, records, first, last), x0 + layout.label_w);
    igPopID();
  }

  for (uint32_t f = 0; f < schema->field_count; ++f) {
    const ft_input_field *field = &schema->fields[f];
    if (!field_editable(schema, (int)f) || field->kind == FT_INPUT_VEC2 || field->kind == FT_INPUT_BOOL) continue;
    igPushID_Int((int)f);
    inspector_row_label(field, x0, layout.label_w);
    const bool mixed = range_mixed(host, field, (int)f, records, first, last);
    igPushItemFlag(ImGuiItemFlags_MixedValue, mixed);
    inspector_value(host, snippet, first, last, field, (int)f, mixed, layout.value_w);
    igPopItemFlag();
    igPopID();
  }

  // Buttons: one checkbox each, named beside the box, wrapping like text.
  const float row_start = x0 + layout.label_w;
  const float room = igGetContentRegionAvail().x - layout.label_w;
  float used = 0.f;
  bool any = false;
  for (uint32_t f = 0; f < schema->field_count; ++f) {
    const ft_input_field *field = &schema->fields[f];
    if (!field_editable(schema, (int)f) || field->kind != FT_INPUT_BOOL) continue;
    const float w = bool_item_width(field);
    if (any && used + bool_gap() + w <= room) {
      igSameLine(0.f, bool_gap());
      used += bool_gap() + w;
    } else {
      igSetCursorPosX(row_start);
      used = w;
    }
    any = true;
    igPushID_Int((int)f);
    const bool mixed = range_mixed(host, field, (int)f, records, first, last);
    bool value = engine_input_get(host, &records[first], (int)f) != 0;
    igPushItemFlag(ImGuiItemFlags_MixedValue, mixed);
    if (igCheckbox(field_name(field), &value)) inspector_apply(host, snippet, first, last, field, (int)f, value ? 1 : 0, 0.f);
    igPopItemFlag();
    if (field->description) igSetItemTooltip("%s", field->description);
    igPopID();
  }
  igPopStyleVar(1);
}

// ---------------------------------------------------------------------------
// Toolbar

static void draw_help_marker(void) {
  static const char *const rows[][2] = {
      {"Drag a lane", "Draw (Shift: straight line)"},
      {"Ctrl+drag", "Select ticks"},
      {"Drag the ruler", "Move the playhead"},
      {"Right-click", "Fill, interpolate, paste"},
      {"Ctrl+Wheel", "Zoom"},
      {"Shift+Wheel", "Scroll"},
      {"Enter", "Zoom to selection"},
      {"Delete", "Reset selection"},
  };
  igTextDisabled(ICON_FA_CIRCLE_QUESTION);
  if (!igBeginItemTooltip()) return;
  if (igBeginTable("##help", 2, ImGuiTableFlags_SizingFixedFit, (ImVec2){0, 0}, 0.f)) {
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
      igTableNextRow(0, 0.f);
      igTableNextColumn();
      igTextDisabled("%s", rows[i][0]);
      igTableNextColumn();
      igTextUnformatted(rows[i][1], NULL);
    }
    igEndTable();
  }
  igEndTooltip();
}

static void draw_toolbar(const ft_input_schema *schema, const input_snippet_t *snippet, int track_index) {
  igAlignTextToFramePadding();
  igText("Track %d  \xC2\xB7  ticks %d \xE2\x80\x93 %d  (%d)", track_index + 1, snippet->start_tick, snippet->end_tick, snippet->input_count);
  if (snippet->effect_count > 0) {
    igSameLine(0, 12.f);
    igTextDisabled("%d effect%s applied on top", snippet->effect_count, snippet->effect_count == 1 ? "" : "s");
    igSetItemTooltip("These lanes show the recorded input; the snippet's input effects still change it during playback.");
  }

  igSameLine(0, 16.f);
  if (igButton(ICON_FA_MAGNIFYING_GLASS_MINUS, (ImVec2){0, 0})) ed.zoom_request = 1.f / 1.5f;
  igSameLine(0, 4.f);
  if (igButton(ICON_FA_MAGNIFYING_GLASS_PLUS, (ImVec2){0, 0})) ed.zoom_request = 1.5f;
  igSameLine(0, 4.f);
  if (igButton(ICON_FA_EXPAND " Fit", (ImVec2){0, 0})) ed.fit_pending = true;
  igSameLine(0, 4.f);
  igBeginDisabled(!ed.has_selection);
  if (igButton(ICON_FA_MAGNIFYING_GLASS_ARROW_RIGHT " Zoom to selection", (ImVec2){0, 0})) ed.zoom_to_selection_pending = true;
  igEndDisabled();
  igSameLine(0, 8.f);
  igCheckbox("Follow playhead", &ed.follow_playhead);
  igSameLine(0, 8.f);
  if (igButton(ICON_FA_LAYER_GROUP " Lanes", (ImVec2){0, 0})) igOpenPopup_Str("##lanes_popup", 0);
  if (igBeginPopup("##lanes_popup", 0)) {
    for (uint32_t f = 0; f < schema->field_count && f < INPUT_LANES_MAX_FIELDS; ++f) {
      const ft_input_field *field = &schema->fields[f];
      if (field->flags & (FT_INPUT_FLAG_INTERNAL | FT_INPUT_FLAG_EDITOR_HIDDEN)) continue;
      bool shown = !ed.hidden[f];
      if (igCheckbox(field_name(field), &shown)) ed.hidden[f] = !shown;
      if (field->kind == FT_INPUT_VEC2) {
        igSameLine(0, 12.f);
        igPushID_Int((int)f);
        bool xy = ed.vec2_xy[f];
        if (igCheckbox("as X / Y", &xy)) ed.vec2_xy[f] = xy;
        igPopID();
      }
    }
    igEndPopup();
  }
  igSameLine(0, 8.f);
  draw_help_marker();
}

// ---------------------------------------------------------------------------
// Lanes

typedef struct {
  float canvas_x, canvas_w;
  input_lane_view_t view;
  int hovered_index; // tick under the mouse, -1 when not over the lanes
} lane_frame_t;

static void lane_context_menu(game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet, const input_lane_t *lanes,
                              int lane_count, int playhead_index) {
  if (!igBeginPopup("##lane_menu", 0)) return;
  const input_lane_t *lane = find_lane(lanes, lane_count, ed.menu_field, ed.menu_channel);
  input_record_t *records = snippet_window(snippet);
  if (!lane || ed.menu_index < 0 || ed.menu_index >= snippet->input_count) {
    igCloseCurrentPopup();
    igEndPopup();
    return;
  }

  const double value = input_lane_read(host, lane, &records[ed.menu_index]);
  char text[64];
  input_lane_format(lane, value, text, sizeof(text));
  igTextDisabled("%s at tick %d: %s", field_name(lane->field), snippet->start_tick + ed.menu_index, text);
  igSeparator();

  int first = 0, last = -1;
  if (ed.has_selection) selection_bounds(&first, &last);
  char label[128];
  snprintf(label, sizeof(label), "Fill selection with %s", text);
  if (igMenuItem_Bool(label, NULL, false, ed.has_selection)) {
    begin_action(snippet);
    write_segment(host, lane, records, first, value, last, value);
  }
  if (lane->style == INPUT_LANE_CURVE &&
      igMenuItem_Bool("Interpolate selection", NULL, false, ed.has_selection && last - first >= 2)) {
    begin_action(snippet);
    write_segment(host, lane, records, first, input_lane_read(host, lane, &records[first]), last,
                  input_lane_read(host, lane, &records[last]));
  }
  snprintf(label, sizeof(label), "Reset %s in selection", field_name(lane->field));
  if (igMenuItem_Bool(label, NULL, false, ed.has_selection)) {
    begin_action(snippet);
    reset_range(host, schema, records, first, last, lane->field_index);
  }
  igSeparator();

  const int paste_at = ed.has_selection ? first : clampi(playhead_index, 0, snippet->input_count - 1);
  if (igMenuItem_Bool("Copy selection", "Ctrl+C", false, ed.has_selection)) copy_selection(snippet);
  if (igMenuItem_Bool("Paste", "Ctrl+V", false, ed.clipboard != NULL)) {
    begin_action(snippet);
    paste_clipboard(host, schema, snippet, paste_at, -1);
  }
  snprintf(label, sizeof(label), "Paste %s only", field_name(lane->field));
  if (igMenuItem_Bool(label, NULL, false, ed.clipboard != NULL)) {
    begin_action(snippet);
    paste_clipboard(host, schema, snippet, paste_at, lane->field_index);
  }
  if (igMenuItem_Bool("Reset selection", "Delete", false, ed.has_selection)) {
    begin_action(snippet);
    reset_range(host, schema, records, first, last, -1);
  }
  igSeparator();
  if (igMenuItem_Bool("Zoom to selection", "Enter", false, ed.has_selection)) ed.zoom_to_selection_pending = true;
  if (igMenuItem_Bool("Clear selection", "Esc", false, ed.has_selection)) ed.has_selection = false;
  if (igMenuItem_Bool("Select all", "Ctrl+A", false, true)) {
    ed.has_selection = true;
    ed.selection_anchor = 0;
    ed.selection_end = snippet->input_count - 1;
  }
  igEndPopup();
}

static void start_paint(game_host_t *host, input_snippet_t *snippet, const input_lane_t *lane, float top, int index, float mouse_y) {
  if (!begin_action(snippet)) return;
  input_record_t *records = snippet_window(snippet);
  ed.painting = true;
  ed.paint_field = lane->field_index;
  ed.paint_channel = lane->channel;
  ed.paint_line = igGetIO_Nil()->KeyShift;
  // Pressing on a held button releases instead, so one gesture both adds and
  // removes presses without a mode switch.
  if (lane->style == INPUT_LANE_BARS)
    ed.paint_value = input_lane_read(host, lane, &records[index]) != 0.0 ? 0.0 : 1.0;
  else
    ed.paint_value = input_lane_value_at_y(lane, top, mouse_y);
  ed.press_index = ed.last_index = index;
  ed.press_value = ed.last_value = ed.paint_value;
  write_segment(host, lane, records, index, ed.paint_value, index, ed.paint_value);
}

static void continue_paint(game_host_t *host, input_snippet_t *snippet, const input_lane_t *lane, float top, int index, float mouse_y) {
  input_record_t *records = snippet_window(snippet);
  const double value = lane->style == INPUT_LANE_CURVE ? input_lane_value_at_y(lane, top, mouse_y) : ed.paint_value;
  if (ed.paint_line) {
    // Redraw the line from scratch each frame so dragging back shortens it.
    if (ed.action_before_states && ed.action_before_count == snippet->input_count) {
      const int lo = ed.press_index < ed.last_index ? ed.press_index : ed.last_index;
      const int hi = ed.press_index < ed.last_index ? ed.last_index : ed.press_index;
      memcpy(records + lo, ed.action_before_states + lo, sizeof(input_record_t) * (size_t)(hi - lo + 1));
      mark_dirty(lo);
    }
    write_segment(host, lane, records, ed.press_index, ed.press_value, index, value);
  } else if (index != ed.last_index || value != ed.last_value) {
    // Fill between frames so a fast drag leaves no gaps.
    write_segment(host, lane, records, ed.last_index, ed.last_value, index, value);
  }
  ed.last_index = index;
  ed.last_value = value;
}

static float lane_name_width(const input_lane_t *lane) {
  const float w = igCalcTextSize(field_name(lane->field), NULL, false, 0.f).x;
  return lane->channel == INPUT_LANE_X || lane->channel == INPUT_LANE_Y ? w + igCalcTextSize(" X", NULL, false, 0.f).x : w;
}

// Wide enough that no name runs into its row labels.
static float lanes_header_width(const input_lane_t *lanes, int count) {
  const float scale = gfx_get_ui_scale();
  const float pad = 6.f * scale, gap = 12.f * scale;
  float width = 110.f * scale;
  for (int l = 0; l < count; ++l) {
    const input_lane_t *lane = &lanes[l];
    float need = pad + lane_name_width(lane) + pad;
    if (lane->style == INPUT_LANE_ROWS) {
      float labels = 0.f;
      for (int r = 0; r < lane->row_count; ++r) {
        char label[64];
        input_lane_row_label(lane, r, label, sizeof(label));
        labels = fmaxf(labels, igCalcTextSize(label, NULL, false, 0.f).x);
      }
      need += gap + labels;
    }
    width = fmaxf(width, need);
  }
  return width;
}

static void draw_lane_header(const input_lane_t *lane, ImVec2 min, float width) {
  ImDrawList *dl = igGetWindowDrawList();
  const float pad = 6.f * gfx_get_ui_scale();
  ImDrawList_AddRectFilled(dl, min, (ImVec2){min.x + 3.f, min.y + lane->height}, lane->color, 0.f, 0);

  char name[96];
  const char *suffix = lane->channel == INPUT_LANE_X ? " X"
                                                                           : lane->channel == INPUT_LANE_Y ? " Y"
                                                                                                           : "";
  snprintf(name, sizeof(name), "%s%s", field_name(lane->field), suffix);
  const float font = igGetFontSize();
  const float name_y = lane->style == INPUT_LANE_ROWS ? min.y + 2.f : min.y + (lane->height - font) * 0.5f;
  ImDrawList_AddText_Vec2(dl, (ImVec2){min.x + pad, fminf(name_y, min.y + lane->height - font)}, igGetColorU32_Col(ImGuiCol_Text, 1.f),
                          name, NULL);

  // Row labels sit at the right edge of the header, next to their rows.
  if (lane->style == INPUT_LANE_ROWS) {
    const float row_h = lane->height / (float)lane->row_count;
    for (int r = 0; r < lane->row_count; ++r) {
      char label[64];
      input_lane_row_label(lane, r, label, sizeof(label));
      const ImVec2 size = igCalcTextSize(label, NULL, false, 0.f);
      ImDrawList_AddText_Vec2(dl, (ImVec2){min.x + width - size.x - pad, min.y + r * row_h + (row_h - size.y) * 0.5f},
                              igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f), label, NULL);
    }
  }
}

static void draw_lanes(ui_handler_t *ui, game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet, int group,
                       int playhead_index, float height) {
  const float scale = gfx_get_ui_scale();
  const ImGuiIO *io = igGetIO_Nil();
  timeline_state_t *ts = &ui->timeline;
  input_record_t *records = snippet_window(snippet);
  const int count = snippet->input_count;

  input_lane_t lanes[INPUT_LANES_MAX];
  const int lane_count = input_lanes_build(schema, ed.vec2_xy, ed.hidden, scale, lanes, INPUT_LANES_MAX);
  for (int l = 0; l < lane_count; ++l) {
    const float custom = lanes[l].style == INPUT_LANE_CURVE && lanes[l].field_index < INPUT_LANES_MAX_FIELDS
                             ? ed.curve_height[lanes[l].field_index][lanes[l].channel]
                             : 0.f;
    if (custom > 0.f) lanes[l].height = custom * scale;
  }
  const float header_w = fminf(lanes_header_width(lanes, lane_count), igGetContentRegionAvail().x * 0.4f);

  const ImVec2 origin = igGetCursorScreenPos();
  const float avail_w = igGetContentRegionAvail().x;
  lane_frame_t frame = {.canvas_x = origin.x + header_w, .canvas_w = fmaxf(50.f, avail_w - header_w), .hovered_index = -1};

  if (ed.fit_pending) {
    ed.px_per_tick = frame.canvas_w / (float)(count > 0 ? count : 1);
    ed.view_start = 0.f;
    ed.fit_pending = false;
  }
  clamp_view(count, frame.canvas_w);

  if (ed.zoom_to_selection_pending && ed.has_selection) {
    int first, last;
    selection_bounds(&first, &last);
    const float span = (float)(last - first + 1);
    ed.px_per_tick = frame.canvas_w / (span * 1.1f);
    clamp_view(count, frame.canvas_w);
    ed.view_start = (float)first - (frame.canvas_w / ed.px_per_tick - span) * 0.5f;
    clamp_view(count, frame.canvas_w);
  }
  ed.zoom_to_selection_pending = false;
  if (ed.zoom_request != 0.f) {
    // Zoom around the playhead when it is in view, otherwise around the middle.
    float anchor = frame.canvas_x + frame.canvas_w * 0.5f;
    const float playhead_x = frame.canvas_x + ((float)playhead_index - ed.view_start) * ed.px_per_tick;
    if (playhead_index >= 0 && playhead_index <= count && playhead_x >= frame.canvas_x && playhead_x <= frame.canvas_x + frame.canvas_w)
      anchor = playhead_x;
    zoom_at(ed.zoom_request, anchor, frame.canvas_x, count, frame.canvas_w);
    ed.zoom_request = 0.f;
  }

  // Keep the playhead in view when it moves, without fighting the user
  // scrolling elsewhere while it stands still.
  // Never during a drag in the editor: the drag scrolls the view itself.
  const bool dragging = ed.scrubbing || ed.selecting || ed.painting;
  if (ed.follow_playhead && !dragging && playhead_index != ed.last_playhead_index && playhead_index >= 0 && playhead_index < count) {
    const float visible = frame.canvas_w / ed.px_per_tick;
    if ((float)playhead_index < ed.view_start || (float)playhead_index + 1.f > ed.view_start + visible) {
      ed.view_start = (float)playhead_index - visible * 0.2f;
      clamp_view(count, frame.canvas_w);
    }
  }
  ed.last_playhead_index = playhead_index;

  frame.view = (input_lane_view_t){.x0 = frame.canvas_x - ed.view_start * ed.px_per_tick,
                                   .px_per_tick = ed.px_per_tick,
                                   .count = count,
                                   .clip_min_x = frame.canvas_x,
                                   .clip_max_x = frame.canvas_x + frame.canvas_w};
  const input_lane_view_t *view = &frame.view;
  const float view_end_x = fminf(frame.view.clip_max_x, input_lane_tick_x(view, count));

  // ---- Ruler
  ImDrawList *dl = igGetWindowDrawList();
  const float ruler_h = igGetFrameHeight();
  const ImVec2 ruler_min = {frame.canvas_x, origin.y}, ruler_max = {frame.canvas_x + frame.canvas_w, origin.y + ruler_h};
  ImDrawList_AddRectFilled(dl, ruler_min, ruler_max, igGetColorU32_Col(ImGuiCol_FrameBg, 1.f), 0.f, 0);
  ImDrawList_PushClipRect(dl, ruler_min, ruler_max, true);
  {
    const int step = nice_step(ed.px_per_tick, 70.f * scale);
    int first, last;
    input_lane_visible_range(view, &first, &last);
    // Label ticks on round timeline numbers, not round offsets into the snippet.
    int t = ((snippet->start_tick + first) / step) * step - snippet->start_tick;
    for (; t <= last + step; t += step) {
      if (t < 0) continue;
      const float x = input_lane_tick_x(view, t);
      ImDrawList_AddLine(dl, (ImVec2){x, ruler_max.y - 6.f * scale}, (ImVec2){x, ruler_max.y}, igGetColorU32_Col(ImGuiCol_Text, 0.6f), 1.f);
      char label[32];
      snprintf(label, sizeof(label), "%d", snippet->start_tick + t);
      ImDrawList_AddText_Vec2(dl, (ImVec2){x + 3.f, ruler_min.y + 1.f}, igGetColorU32_Col(ImGuiCol_Text, 0.8f), label, NULL);
    }
  }
  ImDrawList_PopClipRect(dl);

  igSetCursorScreenPos(ruler_min);
  igInvisibleButton("##ruler", (ImVec2){frame.canvas_w, ruler_h}, 0);
  {
    const int index = pointer_index(view, io->MousePos.x);
    if (igIsItemActivated()) {
      if (io->KeyShift && ed.has_selection) {
        ed.selecting = true;
      } else if (io->KeyCtrl) {
        begin_selection(index);
      } else {
        ed.scrubbing = true;
      }
    }
    // Dragging here moves the playhead, like the timeline's ruler does.
    if (ed.scrubbing && igIsItemActive()) {
      edge_scroll(io->MousePos.x, frame.canvas_x, frame.canvas_w, count);
      if (index != playhead_index) set_playhead(ts, group, snippet, index);
    } else {
      ed.scrubbing = false;
    }
    if (igIsItemHovered(0)) frame.hovered_index = index;
  }

  // ---- Lanes, in a child so a game with many fields scrolls vertically.
  const float lanes_top = origin.y + ruler_h + 2.f;
  igSetCursorScreenPos((ImVec2){origin.x, lanes_top});
  const float lanes_h = fmaxf(60.f * scale, height - ruler_h - 2.f);
  if (igBeginChild_Str("##lanes", (ImVec2){avail_w, lanes_h}, 0, ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList *ldl = igGetWindowDrawList();
    const ImVec2 child_min = igGetWindowPos();
    const ImVec2 child_size = igGetWindowSize();
    const ImVec2 child_max = {child_min.x + child_size.x, child_min.y + child_size.y};
    const float gap = 3.f * scale;
    const ImU32 lane_bg = igGetColorU32_Col(ImGuiCol_FrameBg, 0.55f);

    float y = igGetCursorScreenPos().y;
    const float content_top = y;
    bool open_menu = false;
    for (int l = 0; l < lane_count; ++l) {
      const input_lane_t *lane = &lanes[l];
      const float top = y;
      igPushID_Int(lane->field_index * 4 + (int)lane->channel);

      draw_lane_header(lane, (ImVec2){child_min.x, top}, header_w - 4.f * scale);

      ImDrawList_AddRectFilled(ldl, (ImVec2){frame.canvas_x, top}, (ImVec2){view_end_x, top + lane->height}, lane_bg, 3.f, 0);
      ImDrawList_PushClipRect(ldl, (ImVec2){frame.canvas_x, child_min.y}, (ImVec2){frame.canvas_x + frame.canvas_w, child_max.y}, true);
      input_lane_draw(ldl, host, lane, records, view, top);
      ImDrawList_PopClipRect(ldl);

      igSetCursorScreenPos((ImVec2){frame.canvas_x, top});
      igInvisibleButton("##lane", (ImVec2){frame.canvas_w, lane->height}, ImGuiButtonFlags_MouseButtonLeft);
      const int index = pointer_index(view, io->MousePos.x);
      const bool is_paint_lane = ed.painting && ed.paint_field == lane->field_index && ed.paint_channel == lane->channel;

      if (igIsItemActivated()) {
        if (io->KeyCtrl)
          begin_selection(index);
        else
          start_paint(host, snippet, lane, top, index, io->MousePos.y);
      }
      if (is_paint_lane && igIsItemActive()) {
        edge_scroll(io->MousePos.x, frame.canvas_x, frame.canvas_w, count);
        continue_paint(host, snippet, lane, top, index, io->MousePos.y);
      } else if (is_paint_lane) {
        ed.painting = false;
      }

      if (igIsItemHovered(0)) {
        frame.hovered_index = index;
        if (!ed.painting) {
          char text[64];
          input_lane_format(lane, input_lane_read(host, lane, &records[index]), text, sizeof(text));
          igSetTooltip("Tick %d  \xC2\xB7  %s: %s", snippet->start_tick + index, field_name(lane->field), text);
        }
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) {
          ed.menu_field = lane->field_index;
          ed.menu_channel = lane->channel;
          ed.menu_index = index;
          open_menu = true;
        }
      }
      // Graph lanes get a grip under them: finer tuning wants a taller graph.
      float lane_gap = gap;
      if (lane->style == INPUT_LANE_CURVE && lane->field_index < INPUT_LANES_MAX_FIELDS) {
        lane_gap = 8.f * scale;
        const ImVec2 grip_min = {child_min.x, top + lane->height};
        igSetCursorScreenPos(grip_min);
        igInvisibleButton("##resize", (ImVec2){avail_w, lane_gap}, 0);
        float *stored = &ed.curve_height[lane->field_index][lane->channel];
        if (igIsItemActive() && io->MouseDelta.y != 0.f) {
          const float height = fmaxf(40.f * scale, fminf(480.f * scale, lane->height + io->MouseDelta.y));
          *stored = height / scale;
        }
        if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) *stored = 0.f;
        const bool hot = igIsItemHovered(0) || igIsItemActive();
        if (hot) {
          igSetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        const float mid = grip_min.y + lane_gap * 0.5f;
        ImDrawList_AddLine(ldl, (ImVec2){frame.canvas_x + frame.canvas_w * 0.5f - 18.f * scale, mid},
                           (ImVec2){frame.canvas_x + frame.canvas_w * 0.5f + 18.f * scale, mid},
                           igGetColorU32_Col(hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator, hot ? 1.f : 0.7f), 2.f * scale);
      }
      igPopID();
      y = top + lane->height + lane_gap;
    }
    // Extends the child's content so it knows how far it can scroll.
    igSetCursorScreenPos((ImVec2){child_min.x, y});
    igDummy((ImVec2){1.f, 1.f});

    // Overlays across every lane: tick grid, selection, hover and playhead.
    ImDrawList_PushClipRect(ldl, (ImVec2){frame.canvas_x, child_min.y}, (ImVec2){frame.canvas_x + frame.canvas_w, child_max.y}, true);
    const float overlay_bottom = fminf(child_max.y, y);
    if (ed.px_per_tick >= 8.f * scale) {
      int first, last;
      input_lane_visible_range(view, &first, &last);
      for (int t = first; t <= last + 1; ++t) {
        const float x = input_lane_tick_x(view, t);
        ImDrawList_AddLine(ldl, (ImVec2){x, content_top}, (ImVec2){x, overlay_bottom}, igGetColorU32_Col(ImGuiCol_Separator, 0.18f), 1.f);
      }
    }
    if (ed.tick_flags) {
      // Ticks the demo had to guess are tinted amber, ticks its player is not in are dimmed.
      int first, last;
      input_lane_visible_range(view, &first, &last);
      last = last < count - 1 ? last : count - 1;
      for (int t = first < 0 ? 0 : first; t <= last;) {
        const uint8_t kind = ed.tick_flags[t] & (FT_RECORDING_TICK_PRESENT | FT_RECORDING_TICK_APPROXIMATED);
        int end = t + 1;
        while (end <= last && (ed.tick_flags[end] & (FT_RECORDING_TICK_PRESENT | FT_RECORDING_TICK_APPROXIMATED)) == kind) ++end;
        const ImU32 tint = !(kind & FT_RECORDING_TICK_PRESENT)       ? IM_COL32(0, 0, 0, 110)
                           : (kind & FT_RECORDING_TICK_APPROXIMATED) ? IM_COL32(255, 170, 40, 45)
                                                                     : 0;
        if (tint) {
          const float x0 = input_lane_tick_x(view, t);
          const float x1 = fmaxf(input_lane_tick_x(view, end), x0 + 1.f);
          ImDrawList_AddRectFilled(ldl, (ImVec2){x0, child_min.y}, (ImVec2){x1, child_max.y}, tint, 0.f, 0);
        }
        t = end;
      }
    }
    if (ed.has_selection) {
      int first, last;
      selection_bounds(&first, &last);
      const ImVec2 a = {input_lane_tick_x(view, first), child_min.y}, b = {input_lane_tick_x(view, last + 1), child_max.y};
      ImDrawList_AddRectFilled(ldl, a, b, IM_COL32(110, 160, 255, 40), 0.f, 0);
      ImDrawList_AddRect(ldl, a, b, IM_COL32(110, 160, 255, 140), 0.f, 0, 1.f);
    }
    if (frame.hovered_index >= 0 && ed.px_per_tick >= 3.f)
      ImDrawList_AddRectFilled(ldl, (ImVec2){input_lane_tick_x(view, frame.hovered_index), child_min.y},
                               (ImVec2){input_lane_tick_x(view, frame.hovered_index + 1), child_max.y}, IM_COL32(255, 255, 255, 18), 0.f, 0);
    if (playhead_index >= 0 && playhead_index <= count) {
      // The line marks the start of the tick being played; its column is tinted.
      const float x = input_lane_tick_x(view, playhead_index);
      if (playhead_index < count && ed.px_per_tick >= 3.f)
        ImDrawList_AddRectFilled(ldl, (ImVec2){x, child_min.y}, (ImVec2){input_lane_tick_x(view, playhead_index + 1), child_max.y},
                                 IM_COL32(255, 90, 90, 28), 0.f, 0);
      const float px = roundf(x);
      ImDrawList_AddRectFilled(ldl, (ImVec2){px - 1.f, child_min.y}, (ImVec2){px + 1.f, child_max.y}, PLAYHEAD_COLOR, 0.f, 0);
    }
    ImDrawList_PopClipRect(ldl);

    // Opened out here: inside the lane loop it would be keyed to that lane's ID.
    if (open_menu && !ed.read_only) igOpenPopup_Str("##lane_menu", 0);
    // A click on empty space, below the lanes or on their names, drops the selection.
    if (igIsWindowHovered(0) && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsAnyItemHovered() && !io->KeyCtrl &&
        !io->KeyShift)
      ed.has_selection = false;
    lane_context_menu(host, schema, snippet, lanes, lane_count, playhead_index);

    // Wheel: Ctrl zooms around the mouse, Shift or a horizontal wheel pans,
    // plain wheel scrolls the lanes up and down.
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
      if (io->MouseWheel != 0.f && io->KeyCtrl) {
        zoom_at(powf(1.2f, io->MouseWheel), io->MousePos.x, frame.canvas_x, count, frame.canvas_w);
      } else if (io->MouseWheel != 0.f && io->KeyShift) {
        ed.view_start -= io->MouseWheel * 60.f * scale / ed.px_per_tick;
      } else if (io->MouseWheel != 0.f) {
        igSetScrollY_Float(igGetScrollY() - io->MouseWheel * 3.f * igGetFontSize());
      }
      if (io->MouseWheelH != 0.f) ed.view_start -= io->MouseWheelH * 60.f * scale / ed.px_per_tick;
      if (igIsMouseDragging(ImGuiMouseButton_Middle, 0.f)) ed.view_start -= io->MouseDelta.x / ed.px_per_tick;
      clamp_view(count, frame.canvas_w);
    }
  }
  igEndChild();

  // The selection drag follows the mouse wherever it goes, so it is updated
  // once here rather than by whichever lane it started on.
  if (ed.selecting) {
    if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      edge_scroll(io->MousePos.x, frame.canvas_x, frame.canvas_w, count);
      ed.selection_end = pointer_index(view, io->MousePos.x);
    } else {
      ed.selecting = false;
    }
  }

  // Playhead marker on the ruler, drawn last so it sits on top of the labels.
  if (playhead_index >= 0 && playhead_index <= count) {
    // Snapped to a whole pixel with a whole-pixel half width, so the 2 px
    // line and the triangle's tip share one centre. The stem runs through the
    // ruler into the lanes, so the two read as one marker.
    const float px = roundf(input_lane_tick_x(view, playhead_index));
    if (px >= ruler_min.x && px <= ruler_max.x) {
      const float s = fmaxf(4.f, roundf(5.f * scale));
      const float base_y = ruler_max.y - s - 1.f;
      ImDrawList_AddRectFilled(dl, (ImVec2){px - 1.f, base_y}, (ImVec2){px + 1.f, lanes_top}, PLAYHEAD_COLOR, 0.f, 0);
      ImDrawList_AddTriangleFilled(dl, (ImVec2){px - s, base_y}, (ImVec2){px + s, base_y}, (ImVec2){px, base_y + s + 1.f}, PLAYHEAD_COLOR);
    }
  }

  // ---- Horizontal scrollbar for the tick range.
  const float bar_h = 8.f * scale;
  const ImVec2 bar_min = {frame.canvas_x, igGetCursorScreenPos().y + 2.f};
  igSetCursorScreenPos(bar_min);
  igInvisibleButton("##hscroll", (ImVec2){frame.canvas_w, bar_h}, 0);
  const float visible = frame.canvas_w / ed.px_per_tick;
  const float total = fmaxf((float)count, visible);
  const float thumb_x0 = bar_min.x + frame.canvas_w * (ed.view_start / total);
  const float thumb_x1 = bar_min.x + frame.canvas_w * fminf(1.f, (ed.view_start + visible) / total);
  if (igIsItemActive()) {
    if (igIsItemActivated() && (io->MousePos.x < thumb_x0 || io->MousePos.x > thumb_x1))
      ed.view_start = (io->MousePos.x - bar_min.x) / frame.canvas_w * total - visible * 0.5f;
    else
      ed.view_start += io->MouseDelta.x / frame.canvas_w * total;
    clamp_view(count, frame.canvas_w);
  }
  ImDrawList_AddRectFilled(dl, bar_min, (ImVec2){bar_min.x + frame.canvas_w, bar_min.y + bar_h}, igGetColorU32_Col(ImGuiCol_ScrollbarBg, 1.f),
                           bar_h * 0.5f, 0);
  ImDrawList_AddRectFilled(dl, (ImVec2){thumb_x0, bar_min.y}, (ImVec2){fmaxf(thumb_x1, thumb_x0 + 6.f), bar_min.y + bar_h},
                           igGetColorU32_Col(igIsItemActive() ? ImGuiCol_ScrollbarGrabActive
                                             : igIsItemHovered(0) ? ImGuiCol_ScrollbarGrabHovered
                                                                  : ImGuiCol_ScrollbarGrab,
                                             1.f),
                           bar_h * 0.5f, 0);
}

// ---------------------------------------------------------------------------
// Keys, only while the editor has focus so they never fight the timeline's.

static void handle_keys(game_host_t *host, const ft_input_schema *schema, input_snippet_t *snippet, int playhead_index, bool recording) {
  const ImGuiIO *io = igGetIO_Nil();
  if (io->WantTextInput || igIsAnyItemActive()) return;
  // Not while recording: a game may bind Enter as a control (SM64's Start).
  if (!recording && ed.has_selection && (igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false)))
    ed.zoom_to_selection_pending = true;
  input_record_t *records = snippet_window(snippet);
  int first = 0, last = -1;
  if (ed.has_selection) selection_bounds(&first, &last);

  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_A, false)) {
    ed.has_selection = true;
    ed.selection_anchor = 0;
    ed.selection_end = snippet->input_count - 1;
  }
  if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) ed.has_selection = false;
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_C, false)) copy_selection(snippet);
  if (ed.read_only) return; // copying out of a demo is all there is
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_X, false) && ed.has_selection) {
    copy_selection(snippet);
    begin_action(snippet);
    reset_range(host, schema, records, first, last, -1);
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_V, false) && ed.clipboard) {
    begin_action(snippet);
    paste_clipboard(host, schema, snippet, ed.has_selection ? first : clampi(playhead_index, 0, snippet->input_count - 1), -1);
  }
  if ((igIsKeyPressed_Bool(ImGuiKey_Delete, false) || igIsKeyPressed_Bool(ImGuiKey_Backspace, false)) && ed.has_selection) {
    begin_action(snippet);
    reset_range(host, schema, records, first, last, -1);
  }
}

// ---------------------------------------------------------------------------

// A demo snippet holds no inputs, but its lanes show the input its player most likely held, read
// from the recording, so it can be looked at and copied from. Building that walks every tick of the
// demo, so it is kept until the snippet or its window onto the recording changes.
static input_snippet_t *demo_stand_in(ui_handler_t *ui, const input_snippet_t *snippet) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const timeline_recording_t *recording = recordings_find(ts, snippet->recording_id);
  int recording_tick;
  if (!recording || !recording->handle || !recordings_snippet_tick(ts, snippet, snippet->start_tick, &recording_tick)) return NULL;
  const int count = snippet->input_count;
  if (demo.snippet_id != snippet->id || demo.recording_tick != recording_tick || demo.count != count || !demo.records) {
    free(demo.records);
    free(demo.flags);
    demo.records = calloc((size_t)count, sizeof(*demo.records));
    demo.flags = calloc((size_t)count, 1);
    demo.snippet_id = -1;
    if (!demo.records || !demo.flags) return NULL;
    for (int i = 0; i < count; ++i) {
      engine_input_default(host, &demo.records[i]);
      gh_recording_input(host, recording->handle, snippet->recording_player, recording_tick + i, demo.records[i].bytes);
    }
    gh_recording_tick_flags(host, recording->handle, snippet->recording_player, recording_tick, (unsigned)count, demo.flags);
    demo.snippet_id = snippet->id;
    demo.recording_tick = recording_tick;
    demo.count = count;
  }
  demo.stand_in = *snippet;
  demo.stand_in.kind = SNIPPET_INPUT;
  demo.stand_in.inputs = demo.records;
  demo.stand_in.source_offset = 0;
  demo.stand_in.source_count = count;
  demo.stand_in.effect_count = 0;
  demo.stand_in.effect_cache_valid = false;
  return &demo.stand_in;
}

static void draw_demo_banner(ui_handler_t *ui, const input_snippet_t *snippet) {
  const timeline_recording_t *recording = recordings_find(&ui->timeline, snippet->recording_id);
  igTextColored((ImVec4){0.45f, 0.65f, 1.f, 1.f}, ICON_FA_FILM);
  igSameLine(0, 6.f);
  igText("%s", recording ? recording->name : "Missing demo");
  igSameLine(0, 10.f);
  igTextDisabled("read-only: shows the input the demo's player most likely held. Ctrl+C copies ticks.");
  igSameLine(0, 10.f);
  igTextColored((ImVec4){1.f, 0.67f, 0.16f, 1.f}, "amber");
  igSetItemTooltip("Ticks the demo does not show exactly; they were rebuilt by guessing.");
}

void render_snippet_editor_panel(ui_handler_t *ui) {
  ui->snippet_editor_focused = false;
  if (!ui->show_snippet_editor_window) return;
  if (ui->focus_snippet_editor_window) {
    igSetNextWindowFocus();
    ui->focus_snippet_editor_window = false;
  }
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_input_schema *schema = game_input_schema(host);

  if (!igBegin("Snippet Editor", &ui->show_snippet_editor_window, 0)) {
    igEnd();
    return;
  }
  if (!schema) {
    igTextDisabled("No game is active.");
    igEnd();
    return;
  }

  // The timeline's selection is the source of truth; active_snippet_id only
  // follows it. Reading that field directly showed an empty editor whenever a
  // snippet was picked normally.
  if (ts->selected_snippets.count == 0) {
    igTextDisabled("Select a snippet on the timeline to edit its inputs.");
    igEnd();
    return;
  }
  if (ts->selected_snippets.count > 1) {
    igText("Multiple snippets selected.");
    igTextDisabled("The snippet editor works on one snippet at a time.");
    igEnd();
    return;
  }
  ts->active_snippet_id = ts->selected_snippets.ids[0];

  int track_index = -1;
  input_snippet_t *snippet = model_find_snippet_by_id(ts, ts->active_snippet_id, &track_index);
  if (!snippet || track_index < 0) {
    igTextDisabled("Selected snippet not found.");
    igEnd();
    return;
  }
  if (snippet->input_count <= 0) {
    igTextDisabled("This snippet has no ticks.");
    igEnd();
    return;
  }
  ed.read_only = snippet_is_playback(snippet);
  ed.tick_flags = NULL;
  if (ed.read_only) {
    input_snippet_t *stand_in = demo_stand_in(ui, snippet);
    if (!stand_in) {
      const timeline_recording_t *recording = recordings_find(ts, snippet->recording_id);
      float progress = 0.f;
      if (recording && recordings_status((timeline_recording_t *)recording, &progress, NULL, 0) == RECORDING_LOADING)
        igTextDisabled(ICON_FA_FILM "  Loading '%s', %.0f%%", recording->name, progress * 100.f);
      else
        igTextDisabled(ICON_FA_FILM "  The demo this snippet replays could not be opened.");
      igEnd();
      return;
    }
    draw_demo_banner(ui, snippet);
    snippet = stand_in;
    ed.tick_flags = demo.flags;
  }

  if (ed.snippet_id != snippet->id) {
    ed.snippet_id = snippet->id;
    reset_editor_state();
  }
  // Trimming or undo can shrink the snippet under a stored selection.
  if (ed.has_selection) {
    ed.selection_anchor = clampi(ed.selection_anchor, 0, snippet->input_count - 1);
    ed.selection_end = clampi(ed.selection_end, 0, snippet->input_count - 1);
  }

  ui->snippet_editor_focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  const int group = model_track_group_index(ts, track_index);
  const int playhead_index = model_group_playhead_tick(ts, group) - snippet->start_tick;

  draw_toolbar(schema, snippet, track_index);
  igSeparator();

  const float scale = gfx_get_ui_scale();
  const float scrollbar_h = 8.f * scale + 4.f + igGetStyle()->ItemSpacing.y;
  const float inspector_h = inspector_height(schema, igGetContentRegionAvail().x);
  const float lanes_h = igGetContentRegionAvail().y - inspector_h - scrollbar_h - igGetStyle()->ItemSpacing.y * 2.f - 1.f;
  draw_lanes(ui, host, schema, snippet, group, playhead_index, lanes_h);

  igSeparator();
  if (igBeginChild_Str("##inspector", (ImVec2){0, inspector_h}, 0, 0)) {
    igBeginDisabled(ed.read_only);
    draw_inspector(host, schema, snippet, playhead_index);
    igEndDisabled();
  }
  igEndChild();

  // The same for the window's own blank areas, around the toolbar and ruler.
  if (igIsWindowHovered(0) && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsAnyItemHovered()) ed.has_selection = false;

  if (ui->snippet_editor_focused) handle_keys(host, schema, snippet, playhead_index, ts->recording);

  // Recompute from the first changed tick right away, so the viewport shows
  // the effect of a stroke while it is still being drawn.
  if (ed.read_only) ed.dirty_from = INT_MAX; // only the stand-in could have changed
  if (ed.dirty_from != INT_MAX) {
    model_recalc_snippet_physics(ts, snippet, snippet->start_tick + ed.dirty_from);
    ui_mark_unsaved(ui);
    ed.dirty_from = INT_MAX;
  }
  if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) ed.painting = false;
  if (ed.action_in_progress && !ed.painting && !igIsAnyItemActive()) end_action(ui, snippet);

  igEnd();
}
