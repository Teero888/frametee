#include "timeline.h"
#include "../user_interface.h"
#include "renderer/graphics_backend.h"
#include "timeline_interaction.h"
#include "timeline_model.h"
#include "timeline_renderer.h"
#include <engine/int_math.h>
#include <math.h>
#include <string.h>
#include <system/include_cimgui.h>

// The band at either edge of the view that the playhead is followed within.
static float follow_margin(float view_width) { return view_width / 12.f; }

// Keeps the active group's playhead in view by scrolling along with it: once it
// reaches a margin from either edge, the view moves with it and holds it there.
// While playing it is only followed out of view, so a view panned away from a
// playing playhead stays put; a playhead moved on purpose (a transport button,
// the tick field, an event) is brought back into view from wherever it was.
static void follow_playhead(timeline_state_t *ts, float view_width) {
  static int last_tick = -1;
  const int tick = model_group_playhead_tick(ts, ts->active_group_index);
  const int previous = last_tick;
  last_tick = tick;
  if (!ts->ui->follow_playhead || ts->is_header_dragging || tick == previous || ts->zoom <= 0.f) return;

  const int visible = imax(1, (int)(view_width / ts->zoom));
  const int margin = (int)(follow_margin(view_width) / ts->zoom);
  const int low = ts->view_start_tick + margin, high = ts->view_start_tick + visible - margin;
  if (tick >= low && tick <= high) return;
  const bool playing = ts->is_playing || ts->is_reversing || ts->ui->camera_editor.playing;
  const bool was_in_view = previous >= ts->view_start_tick && previous <= ts->view_start_tick + visible;
  if (playing && !was_in_view) return;

  ts->view_start_tick = tick > high ? tick - (visible - margin) : tick - margin;
  if (ts->view_start_tick < 0) ts->view_start_tick = 0;
}

// A playhead held by the mouse inside a margin scrolls the view that way, the
// faster the deeper it is held, so it can be dragged along the whole timeline.
// This is a deliberate act, so it works whether following is on or not.
// Runs before the drag places the playhead under the mouse in the moved view.
static void scroll_with_dragged_playhead(timeline_state_t *ts, ImRect header_bb) {
  static double carry = 0.0; // the part of a tick scrolled but not shown yet
  if (!ts->is_header_dragging || ts->recording || ts->zoom <= 0.f) {
    carry = 0.0;
    return;
  }
  const float margin = follow_margin(header_bb.Max.x - header_bb.Min.x);
  const float mouse_x = igGetIO_Nil()->MousePos.x;
  float depth = 0.f; // -1..1 at the edges, more past them
  if (mouse_x > header_bb.Max.x - margin) depth = (mouse_x - (header_bb.Max.x - margin)) / margin;
  else if (mouse_x < header_bb.Min.x + margin) depth = (mouse_x - (header_bb.Min.x + margin)) / margin;
  if (depth == 0.f || (depth < 0.f && ts->view_start_tick <= 0)) {
    carry = 0.0;
    return;
  }
  depth = fmaxf(-2.f, fminf(2.f, depth));
  const double pixels_per_second = 1500.0 * gfx_get_ui_scale();
  carry += depth * pixels_per_second * igGetIO_Nil()->DeltaTime / ts->zoom;
  const int whole = (int)carry;
  carry -= whole;
  ts->view_start_tick = imax(0, ts->view_start_tick + whole);
}

// While the playhead is held, the margins it scrolls the view from show faintly.
static void draw_follow_margins(timeline_state_t *ts, ImDrawList *draw_list, ImRect area) {
  if (!ts->is_header_dragging || ts->recording) return;
  const float margin = follow_margin(area.Max.x - area.Min.x);
  const ImU32 edge = igGetColorU32_Col(ImGuiCol_SeparatorActive, 0.14f);
  const ImU32 inner = igGetColorU32_Col(ImGuiCol_SeparatorActive, 0.f);
  ImDrawList_PushClipRect(draw_list, area.Min, area.Max, true);
  if (ts->view_start_tick > 0)
    ImDrawList_AddRectFilledMultiColor(draw_list, area.Min, (ImVec2){area.Min.x + margin, area.Max.y}, edge, inner, inner, edge);
  ImDrawList_AddRectFilledMultiColor(draw_list, (ImVec2){area.Max.x - margin, area.Min.y}, area.Max, inner, edge, edge, inner);
  ImDrawList_PopClipRect(draw_list);
}

// Public API Implementation

void timeline_init(ui_handler_t *ui) {
  ui->timeline = (timeline_state_t){0};
  model_init(&ui->timeline, ui);
}

void timeline_cleanup(timeline_state_t *ts) { model_cleanup(ts); }

void render_timeline(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  // No padding: the tracks run to the window's right and bottom edges. The
  // left and the top keep their gap, put back by hand below.
  const float side_gap = 8.f;
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0, 0});

  // The Camera tab shares this dock node; the Timeline is the one to open on.
  if (ui->select_timeline_tab) {
    igSetNextWindowFocus();
    ui->select_timeline_tab = false;
  }
  ui->timeline_window_visible = igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
  ui->timeline_window_focused = ui->timeline_window_visible && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (ui->timeline_window_visible) {
    igPopStyleVar(1);
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());

    // Render top controls
    igSetCursorPosY(igGetCursorPosY() + side_gap);
    igIndent(side_gap);
    renderer_draw_controls(ts);
    igUnindent(side_gap);
    igSeparator();

    // Calculate layout for header and tracks area
    float header_height = igGetTextLineHeightWithSpacing() * 2.0f;
    float track_header_width = renderer_track_header_width();
    ImVec2 content_start_pos = igGetCursorScreenPos();
    ImVec2 available_space = igGetContentRegionAvail();
    content_start_pos.x += side_gap;
    available_space.x -= side_gap;

    // The tracks reach down to the scrollbar, and the scrollbar sits on the
    // window's bottom edge rather than above its padding.
    ImGuiWindow *window = igGetCurrentWindow();
    const float window_bottom = window->Pos.y + window->Size.y - window->WindowBorderSize;
    float scrollbar_height = igGetStyle()->ScrollbarSize;
    available_space.y = fmaxf(0.f, window_bottom - scrollbar_height - content_start_pos.y);

    // Bounding box for the tick marks and labels
    ImRect header_bb = {{content_start_pos.x + track_header_width, content_start_pos.y},
                        {content_start_pos.x + available_space.x, content_start_pos.y + header_height}};
    // Bounding box for the main snippet area (RHS)
    ImRect timeline_bb = {{header_bb.Min.x, header_bb.Max.y}, {header_bb.Max.x, content_start_pos.y + available_space.y}};

    follow_playhead(ts, header_bb.Max.x - header_bb.Min.x);
    scroll_with_dragged_playhead(ts, header_bb);

    // Handle header interaction and render it
    interaction_handle_header(ts, header_bb);
    interaction_set_scrubbing(ts, ts->tick_field_active || (ts->is_header_dragging && !ts->recording));
    renderer_draw_header(ts, draw_list, header_bb);
    igDummy((ImVec2){0, header_height}); // Advance cursor

    // Create a child window for the vertically scrollable track area
    igSetCursorScreenPos((ImVec2){content_start_pos.x, header_bb.Max.y});
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){0, 0});
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0, 0});
    igPushStyleVar_Float(ImGuiStyleVar_ChildBorderSize, 0.0f);
    igBeginChild_Str("TracksArea", (ImVec2){available_space.x, timeline_bb.Max.y - timeline_bb.Min.y}, false, ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleVar(3);

    // Render the track headers and snippets inside the child window
    renderer_draw_tracks_area(ts, timeline_bb);

    // Handle mouse interactions for the main timeline area (panning, selection, drag-drop)
    interaction_handle_timeline_area(ts, timeline_bb);

    // Handle context menu
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false) && igIsWindowHovered(0) && igGetIO_Nil()->MousePos.x >= timeline_bb.Min.x) {
      if (!igIsAnyItemHovered()) ts->context_menu_snippet_id = -1;
      igOpenPopup_Str("TimelineContextMenu", 0);
    }
    interaction_handle_context_menu(ts);

    // Draw playhead line inside the child window so it's on top of snippets but part of the scrollable area
    renderer_draw_playhead_line(ts, igGetWindowDrawList(), timeline_bb);

    igEndChild();

    // Render overlays (drag preview, selection box)
    renderer_draw_selection_box(ts, overlay_draw_list);
    renderer_draw_drag_preview(ts, overlay_draw_list, timeline_bb);

    // Draw playhead handle in the parent window
    renderer_draw_playhead_handle(ts, draw_list, timeline_bb, header_bb);
    draw_follow_margins(ts, overlay_draw_list, (ImRect){header_bb.Min, timeline_bb.Max});

    // Horizontal Scrollbar
    static int last_view_start_tick = -1;
    // (the playhead too: a recording runs on past the last snippet)
    int max_tick = imax(model_get_max_timeline_tick(ts), ts->current_tick);
    float total_width = max_tick * ts->zoom + available_space.x;

    if (ts->view_start_tick != last_view_start_tick) {
      igSetNextWindowScroll((ImVec2){(float)ts->view_start_tick * ts->zoom, 0.0f});
    }

    igSetCursorScreenPos((ImVec2){content_start_pos.x, timeline_bb.Max.y});
    igBeginChild_Str("TimelineScrollbar", (ImVec2){available_space.x, scrollbar_height}, false, ImGuiWindowFlags_HorizontalScrollbar);
    igDummy((ImVec2){total_width, 1.0f});
    if (igIsWindowHovered(0) || igIsWindowFocused(0)) {
      ts->view_start_tick = (int)(igGetScrollX() / ts->zoom);
      if (ts->view_start_tick < 0) ts->view_start_tick = 0;
    }
    last_view_start_tick = ts->view_start_tick;
    igEndChild();

  } else {
    igPopStyleVar(1);
    // (hidden mid-drag: nothing holds the playhead any more)
    ts->is_header_dragging = ts->tick_field_active = false;
    interaction_set_scrubbing(ts, false);
  }
  igEnd();
}

// Other Public Functions

void timeline_switch_recording_target(timeline_state_t *ts, int new_track_index) {
  // This logic is primarily interaction-based
  interaction_switch_recording_target(ts, new_track_index);
}
