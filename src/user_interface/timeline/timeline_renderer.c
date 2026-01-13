#include "timeline_renderer.h"
#include "renderer/graphics_backend.h"
#include "timeline_interaction.h"
#include "timeline_model.h"
#include <math.h>
#include <stdio.h>
#include <symbols.h>
#include <system/include_cimgui.h>
#include <user_interface/widgets/imcol.h>

#define MIN_TIMELINE_ZOOM 0.05f
#define MAX_TIMELINE_ZOOM 20.0f
#define TPS 50

// Forward Declarations for Static Render Helpers
static void render_input_snippet(timeline_state_t *ts, player_track_t *track, input_snippet_t *snippet, ImDrawList *draw_list, ImRect timeline_bb,
                                 float track_top, bool is_recording_snippet);
static void render_player_track(timeline_state_t *ts, int track_index, ImDrawList *draw_list, ImRect timeline_bb, float track_top, float track_bottom,
                                bool is_selected);
static double choose_nice_tick_step(double pixels_per_tick, double min_label_spacing);

// Coordinate Conversion
int renderer_screen_x_to_tick(const timeline_state_t *ts, float screen_x, float timeline_start_x) {
  if (fabsf(ts->zoom) < 1e-6f) return ts->view_start_tick;
  return ts->view_start_tick + (int)roundf((screen_x - timeline_start_x) / ts->zoom);
}

float renderer_tick_to_screen_x(const timeline_state_t *ts, int tick, float timeline_start_x) {
  return timeline_start_x + (tick - ts->view_start_tick) * ts->zoom;
}

float renderer_get_track_screen_y(const timeline_state_t *ts, ImRect timeline_bb, int track_index, float scroll_y) {
  float dpi_scale = gfx_get_ui_scale();
  float padding_y = igGetStyle()->WindowPadding.y;
  float item_spacing_y = igGetStyle()->ItemSpacing.y;
  float total_row_height = (ts->track_height * dpi_scale) + item_spacing_y;
  return timeline_bb.Min.y + padding_y + (float)track_index * total_row_height - scroll_y;
}

int renderer_screen_y_to_track_index(const timeline_state_t *ts, ImRect timeline_bb, float screen_y, float scroll_y) {
  float dpi_scale = gfx_get_ui_scale();
  float padding_y = igGetStyle()->WindowPadding.y;
  float item_spacing_y = igGetStyle()->ItemSpacing.y;
  float total_row_height = (ts->track_height * dpi_scale) + item_spacing_y;

  float content_y = screen_y - (timeline_bb.Min.y + padding_y) + scroll_y;
  if (content_y < 0) return -1;

  int track_index = (int)floorf(content_y / total_row_height);
  return (track_index >= ts->player_track_count) ? -1 : track_index;
}

// Main Rendering Functions

void renderer_draw_controls(timeline_state_t *ts) {
  float dpi_scale = gfx_get_ui_scale();
  igPushItemWidth(100 * dpi_scale);
  if (igDragInt("Current Tick", &ts->current_tick, 1, 0, 100000, "%d", ImGuiSliderFlags_None)) {
    if (ts->current_tick < 0) ts->current_tick = 0;
  }
  igPopItemWidth();

  igSameLine(0, 8 * dpi_scale);
  if (igButton(ICON_KI_STEP_BACKWARD, (ImVec2){30 * dpi_scale, 0})) ts->current_tick = 0;
  igSameLine(0, 4 * dpi_scale);
  if (igButton(ICON_KI_BACKWARD, (ImVec2){30 * dpi_scale, 0})) model_advance_tick(ts, -ts->playback_speed);
  igSameLine(0, 4 * dpi_scale);
  if (igButton(ts->is_playing ? ICON_KI_PAUSE : ICON_KI_CARET_RIGHT, (ImVec2){50 * dpi_scale, 0})) {
    ts->is_playing = !ts->is_playing;
    if (ts->is_playing) {
      if (ts->recording && ts->recording_snippets.count > 0) {
        input_snippet_t *recording_snippet = ts->recording_snippets.snippets[0];
        if (recording_snippet) ts->current_tick = recording_snippet->end_tick;
      }
      ts->last_update_time = igGetTime();
    }
  }
  igSameLine(0, 4 * dpi_scale);
  if (igButton(ICON_KI_FORWARD, (ImVec2){30 * dpi_scale, 0})) model_advance_tick(ts, ts->playback_speed);
  igSameLine(0, 4 * dpi_scale);
  if (igButton(ICON_KI_STEP_FORWARD, (ImVec2){30 * dpi_scale, 0})) {
    ts->current_tick = model_get_max_timeline_tick(ts);
  }

  igSameLine(0, 20 * dpi_scale);
  igText("Zoom:");
  igSameLine(0, 4 * dpi_scale);
  igSetNextItemWidth(150 * dpi_scale);
  igSliderFloat("##Zoom", &ts->zoom, MIN_TIMELINE_ZOOM, MAX_TIMELINE_ZOOM, "%.2f", ImGuiSliderFlags_Logarithmic);

  igSameLine(0, 20 * dpi_scale);
  igText("Playback Speed:");
  igSameLine(0, 4 * dpi_scale);
  igSetNextItemWidth(150 * dpi_scale);
  igSliderInt("##Speed", &ts->gui_playback_speed, 1, 100, "%d", ImGuiSliderFlags_None);

  igSameLine(0, 20 * dpi_scale);
  if (igButton(ts->recording ? "Stop Recording" : "Record", (ImVec2){75 * dpi_scale, 0})) {
    interaction_toggle_recording(ts);
  }

  if (ts->recording) {
    igSameLine(0, 10 * dpi_scale);
    igTextColored((ImVec4){1.0f, 0.2f, 0.2f, 1.0f}, ICON_KI_REC);
  }
}

static double choose_nice_tick_step(double pixels_per_tick, double min_label_spacing) {
  static const double nice_steps[] = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000};
  int count = sizeof(nice_steps) / sizeof(nice_steps[0]);
  for (int i = 0; i < count; i++) {
    if (nice_steps[i] * pixels_per_tick >= min_label_spacing) {
      return nice_steps[i];
    }
  }
  return nice_steps[count - 1];
}

void renderer_draw_header(timeline_state_t *ts, ImDrawList *draw_list, ImRect header_bb) {
  float dpi_scale = gfx_get_ui_scale();
  ImU32 tick_minor_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.25f);
  ImU32 tick_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.7f);
  ImU32 tick_major_col = igGetColorU32_Col(ImGuiCol_Text, 0.9f);
  ImU32 tick_text_col = igGetColorU32_Col(ImGuiCol_Text, 1.0f);

  ImDrawList_PushClipRect(draw_list, header_bb.Min, header_bb.Max, true);

  float header_height = header_bb.Max.y - header_bb.Min.y;
  int start_tick = renderer_screen_x_to_tick(ts, header_bb.Min.x, header_bb.Min.x);
  int end_tick = renderer_screen_x_to_tick(ts, header_bb.Max.x, header_bb.Min.x);

  // Pass 1: Draw a faint line for every single tick if they are at least 1px apart
  if (ts->zoom >= 1.0f) {
    for (int tick = start_tick; tick <= end_tick; tick++) {
      if (tick < 0) continue;
      float x = renderer_tick_to_screen_x(ts, tick, header_bb.Min.x);
      ImDrawList_AddLine(draw_list, (ImVec2){x, header_bb.Max.y - header_height * 0.25f}, (ImVec2){x, header_bb.Max.y}, tick_minor_col, 1.0f * dpi_scale);
    }
  }

  // Pass 2: Draw adaptive major ticks and labels for readability
  double tick_step = choose_nice_tick_step(ts->zoom, 80.0f * dpi_scale);
  double start_tick_major = floor((double)start_tick / tick_step) * tick_step;

  for (double tick_d = start_tick_major; tick_d <= end_tick; tick_d += tick_step) {
    int tick = (int)tick_d;
    if (tick < 0) continue;
    float x = renderer_tick_to_screen_x(ts, tick, header_bb.Min.x);

    bool is_sec_marker = (tick % 50) == 0;
    ImU32 col = is_sec_marker ? tick_major_col : tick_col;
    float line_height = is_sec_marker ? header_height * 0.5f : header_height * 0.3f;

    ImDrawList_AddLine(draw_list, (ImVec2){x, header_bb.Max.y - line_height}, (ImVec2){x, header_bb.Max.y}, col, 1.0f * dpi_scale);

    // Format labels based on the time scale
    char label[64];
    if (tick < 50) {
      snprintf(label, sizeof(label), "%d", tick);
    } else if (tick < 3000) { // Under 1 minute
      snprintf(label, sizeof(label), "%.1fs", tick / 50.0);
    } else { // Over 1 minute
      int total_secs = tick / 50;
      int mins = total_secs / 60;
      int secs = total_secs % 60;
      snprintf(label, sizeof(label), "%d:%02d", mins, secs);
    }

    ImVec2 text_size;
    igCalcTextSize(&text_size, label, NULL, false, 0);
    ImVec2 text_pos = {x - text_size.x * 0.5f, header_bb.Min.y + 2.0f * dpi_scale};

    ImDrawList_AddText_Vec2(draw_list, text_pos, tick_text_col, label, NULL);
  }

  // Draw markers for net events
  ImU32 event_marker_col = IM_COL32(255, 200, 0, 255);
  for (int i = 0; i < ts->net_event_count; ++i) {
    net_event_t *ev = &ts->net_events[i];
    float x = renderer_tick_to_screen_x(ts, ev->tick, header_bb.Min.x);
    if (x >= header_bb.Min.x && x <= header_bb.Max.x) {
      ImVec2 p1 = {x - 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale};
      ImVec2 p2 = {x + 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale};
      ImVec2 p3 = {x, header_bb.Max.y - 4 * dpi_scale};
      ImDrawList_AddTriangleFilled(draw_list, p1, p2, p3, event_marker_col);

      // Optional: Hover tooltip for the event
      if (igIsMouseHoveringRect((ImVec2){x - 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale}, (ImVec2){x + 4 * dpi_scale, header_bb.Max.y - 4 * dpi_scale}, true)) {
        igBeginTooltip();
        if (ev->type == NET_EVENT_KILLMSG) {
          igText("KillMsg: %d killed %d with %d", ev->killer, ev->victim, ev->weapon);
        } else {
          igText("%s: %s", ev->type == NET_EVENT_CHAT ? "Chat" : "Broadcast", ev->message);
        }
        igEndTooltip();
      }
    }
  }

  ImDrawList_PopClipRect(draw_list);
}

void renderer_draw_playhead_line(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect) {
  float dpi_scale = gfx_get_ui_scale();
  float playhead_x = renderer_tick_to_screen_x(ts, ts->current_tick, timeline_rect.Min.x);
  if (playhead_x >= timeline_rect.Min.x && playhead_x <= timeline_rect.Max.x) {
    ImDrawList_AddLine(draw_list, (ImVec2){playhead_x, timeline_rect.Min.y}, (ImVec2){playhead_x, timeline_rect.Max.y},
                       igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f), 2.0f * dpi_scale);
  }
}

void renderer_draw_playhead_handle(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect, ImRect header_bb) {
  float dpi_scale = gfx_get_ui_scale();
  float playhead_x = renderer_tick_to_screen_x(ts, ts->current_tick, timeline_rect.Min.x);

  if (playhead_x >= timeline_rect.Min.x && playhead_x <= timeline_rect.Max.x) {
    ImVec2 head_bottom = {playhead_x + 0.5f, header_bb.Max.y + 0.5f};
    ImVec2 head_top_left = {(head_bottom.x - 6.0f * dpi_scale) + 0.5f, head_bottom.y - 10.0f * dpi_scale + 0.5f};
    ImVec2 head_top_right = {(head_bottom.x + 6.0f * dpi_scale) - 0.5f, head_bottom.y - 10.0f * dpi_scale + 0.5f};
    ImDrawList_AddTriangleFilled(draw_list, head_top_left, head_top_right, head_bottom, igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f));

    ImDrawList_AddLine(draw_list, (ImVec2){playhead_x, header_bb.Max.y - 5.0f * dpi_scale}, (ImVec2){playhead_x, header_bb.Max.y},
                       igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f), 2.0f * dpi_scale);
  }
}

void renderer_draw_tracks_area(timeline_state_t *ts, ImRect timeline_bb) {
  float dpi_scale = gfx_get_ui_scale();
  float track_header_width = 120.0f * dpi_scale;
  ImDrawList *draw_list = igGetWindowDrawList();

  ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
  float total_row_height = (ts->track_height * dpi_scale) + igGetStyle()->ItemSpacing.y;
  ImGuiListClipper_Begin(clipper, ts->player_track_count, total_row_height);
  while (ImGuiListClipper_Step(clipper)) {
    for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
      ImVec2 row_start_pos;
      igGetCursorScreenPos(&row_start_pos);
      player_track_t *track = &ts->player_tracks[i];

      // Render Track Info Panel (Left)
      bool is_track_selected = (ts->selected_player_track_index == i);
      ImU32 header_bg_col = track->is_dummy ? igGetColorU32_Col(ImGuiCol_CheckMark, 0.6f) : igGetColorU32_Col(ImGuiCol_FrameBg, 0.8f);

      ImVec2 header_rect_min = row_start_pos;
      ImVec2 header_rect_max = {row_start_pos.x + track_header_width, row_start_pos.y + (ts->track_height * dpi_scale)};
      ImDrawList_AddRectFilled(draw_list, header_rect_min, header_rect_max, header_bg_col, 0.0f, 0);
      ImDrawList_AddLine(draw_list, (ImVec2){header_rect_max.x, header_rect_min.y}, header_rect_max, igGetColorU32_Col(ImGuiCol_Border, 0.5f), 1.0f * dpi_scale);

      igPushID_Int(i);

      // Draw track name. Add a prefix for dummy tracks for visual feedback.
      igSetCursorScreenPos((ImVec2){row_start_pos.x + 8.0f * dpi_scale, row_start_pos.y + ((ts->track_height * dpi_scale) - igGetTextLineHeight()) * 0.5f});
      if (track->is_dummy) {
        igTextDisabled("[D]");
        igSameLine(0, 4.0f * dpi_scale);
      }
      igText("Track %d", i + 1);

      // Add a single invisible button over the header for interactions.
      igSetCursorScreenPos(row_start_pos);
      igInvisibleButton("##track_header_interact", (ImVec2){track_header_width, ts->track_height * dpi_scale}, 0);

      // Handle interactions: double-click to toggle dummy, single-click to select.
      if (igIsItemHovered(0)) {
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
          if (igGetIO_Nil()->KeyShift) {
            for (int t = 0; t < ts->player_track_count; ++t)
              ts->player_tracks[t].is_dummy ^= 1;
          } else track->is_dummy = !track->is_dummy;

        } else if (igIsItemClicked(ImGuiMouseButton_Left)) {
          interaction_select_track(ts, i);
        }
      }

      if (igBeginPopupContextItem("TrackSettings", 1)) {
        if (track->is_dummy) {
          igText("Copy Settings");
          igSeparator();
          int flags = track->dummy_copy_flags;
          bool dir = (flags & COPY_DIRECTION) != 0;
          bool target = (flags & COPY_TARGET) != 0;
          bool jump = (flags & COPY_JUMP) != 0;
          bool fire = (flags & COPY_FIRE) != 0;
          bool hook = (flags & COPY_HOOK) != 0;
          bool weapon = (flags & COPY_WEAPON) != 0;

          if (igCheckbox("Direction", &dir)) flags ^= COPY_DIRECTION;
          if (igCheckbox("Target", &target)) flags ^= COPY_TARGET;
          if (igCheckbox("Jump", &jump)) flags ^= COPY_JUMP;
          if (igCheckbox("Fire", &fire)) flags ^= COPY_FIRE;
          if (igCheckbox("Hook", &hook)) flags ^= COPY_HOOK;
          if (igCheckbox("Weapon", &weapon)) flags ^= COPY_WEAPON;

          bool mirror_x = (flags & COPY_MIRROR_X) != 0;
          bool mirror_y = (flags & COPY_MIRROR_Y) != 0;
          if (igCheckbox("Mirror Aim X (and Dir)", &mirror_x)) flags ^= COPY_MIRROR_X;
          if (igCheckbox("Mirror Aim Y", &mirror_y)) flags ^= COPY_MIRROR_Y;

          track->dummy_copy_flags = flags;
        } else {
          igTextDisabled("Not a dummy track");
          igTextDisabled("Double-click header to toggle");
        }
        igEndPopup();
      }

      igPopID();

      // Render Track Snippets (Right)
      float track_top = row_start_pos.y;
      float track_bottom = track_top + (ts->track_height * dpi_scale);
      render_player_track(ts, i, draw_list, timeline_bb, track_top, track_bottom, is_track_selected);

      igSetCursorScreenPos(row_start_pos);
      ImVec2 avail;
      igGetContentRegionAvail(&avail);
      igDummy((ImVec2){avail.x, ts->track_height * dpi_scale});
    }
  }
  ImGuiListClipper_End(clipper);
  ImGuiListClipper_destroy(clipper);
}

void renderer_draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb, float tracks_area_scroll_y) {
  float dpi_scale = gfx_get_ui_scale();
  if (!ts->drag_state.active) return;

  int snapped_start_tick_clicked, base_track_index;
  interaction_calculate_drag_destination(ts, timeline_bb, tracks_area_scroll_y, &snapped_start_tick_clicked, &base_track_index);

  input_snippet_t *clicked_snippet = model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, NULL);
  if (!clicked_snippet) return;

  int delta_ticks = snapped_start_tick_clicked - clicked_snippet->start_tick;

  // Determine which tracks are affected by the drag operation.
  bool affected_tracks[256] = {false};
  for (int i = 0; i < ts->drag_state.drag_info_count; ++i) {
    dragged_snippet_info_t *d_info = &ts->drag_state.drag_infos[i];
    int s_track_idx;
    input_snippet_t *s = model_find_snippet_by_id(ts, d_info->snippet_id, &s_track_idx);
    if (!s) continue;

    if (s_track_idx < 256) affected_tracks[s_track_idx] = true;
    int new_track_idx = base_track_index + d_info->track_offset;
    if (new_track_idx >= 0 && new_track_idx < 256) affected_tracks[new_track_idx] = true;
  }

  // For each affected track, build a hypothetical layout and solve it.
  for (int track_idx = 0; track_idx < ts->player_track_count; ++track_idx) {
    if (track_idx >= 256 || !affected_tracks[track_idx]) continue;

    player_track_t *track = &ts->player_tracks[track_idx];

    // Count how many snippets will be on this track to allocate memory.
    int hypothetical_count = 0;
    for (int i = 0; i < track->snippet_count; ++i) {
      if (!interaction_is_snippet_selected(ts, track->snippets[i].id)) {
        hypothetical_count++;
      }
    }
    for (int i = 0; i < ts->drag_state.drag_info_count; ++i) {
      dragged_snippet_info_t *d_info = &ts->drag_state.drag_infos[i];
      int new_track_idx = base_track_index + d_info->track_offset;
      if (new_track_idx == track_idx) hypothetical_count++;
    }

    if (hypothetical_count == 0) continue;

    // Create the list of hypothetical snippets and a pointer list for the solver.
    input_snippet_t *hypothetical_snippets = malloc(hypothetical_count * sizeof(input_snippet_t));
    input_snippet_t **solver_list = malloc(hypothetical_count * sizeof(input_snippet_t *));
    if (!hypothetical_snippets || !solver_list) {
      free(hypothetical_snippets);
      free(solver_list);
      continue;
    }

    int current_idx = 0;
    // Add existing, non-dragged snippets.
    for (int i = 0; i < track->snippet_count; ++i) {
      if (!interaction_is_snippet_selected(ts, track->snippets[i].id)) {
        hypothetical_snippets[current_idx] = track->snippets[i];
        solver_list[current_idx] = &hypothetical_snippets[current_idx];
        current_idx++;
      }
    }
    // Add dragged snippets with their new proposed times.
    for (int i = 0; i < ts->drag_state.drag_info_count; ++i) {
      dragged_snippet_info_t *d_info = &ts->drag_state.drag_infos[i];
      int new_track_idx = base_track_index + d_info->track_offset;
      if (new_track_idx == track_idx) {
        input_snippet_t *original = model_find_snippet_by_id(ts, d_info->snippet_id, NULL);
        if (original) {
          hypothetical_snippets[current_idx] = *original;
          hypothetical_snippets[current_idx].start_tick += delta_ticks;
          hypothetical_snippets[current_idx].end_tick += delta_ticks;
          solver_list[current_idx] = &hypothetical_snippets[current_idx];
          current_idx++;
        }
      }
    }

    // Run the solver to get the new, correct layers.
    timeline_solve_snippet_layers(solver_list, hypothetical_count);

    // Draw the previews for the dragged snippets using the solved layout.
    ImDrawList_PushClipRect(overlay_draw_list, timeline_bb.Min, timeline_bb.Max, true);
    for (int i = 0; i < hypothetical_count; ++i) {
      input_snippet_t *preview_snip = &hypothetical_snippets[i];
      if (interaction_is_snippet_selected(ts, preview_snip->id)) {
        // Calculate stack size for correct height, based on the hypothetical layout
        int stack_size = 0;
        for (int j = 0; j < hypothetical_count; ++j) {
          input_snippet_t *other = &hypothetical_snippets[j];
          if (preview_snip->start_tick < other->end_tick && preview_snip->end_tick > other->start_tick) {
            if (other->layer >= stack_size) stack_size = other->layer + 1;
          }
        }

        float sub_lane_height = (ts->track_height * dpi_scale) / (float)fmax(1, stack_size);
        float preview_min_x = renderer_tick_to_screen_x(ts, preview_snip->start_tick, timeline_bb.Min.x);
        float preview_max_x = renderer_tick_to_screen_x(ts, preview_snip->end_tick, timeline_bb.Min.x);
        float target_track_top = renderer_get_track_screen_y(ts, timeline_bb, track_idx, tracks_area_scroll_y);
        float preview_min_y = target_track_top + preview_snip->layer * sub_lane_height + 2.0f * dpi_scale;
        float preview_max_y = preview_min_y + sub_lane_height - 4.0f * dpi_scale;

        ImU32 fill;
        if (igGetIO_Nil()->KeyAlt) {
          fill = IM_COL32(100, 240, 150, 90); // Green
        } else {
          fill = IM_COL32(100, 150, 240, 90); // Blue
        }
        ImDrawList_AddRectFilled(overlay_draw_list, (ImVec2){preview_min_x, preview_min_y}, (ImVec2){preview_max_x, preview_max_y}, fill, 4.0f * dpi_scale,
                                 ImDrawFlags_RoundCornersAll);
      }
    }
    ImDrawList_PopClipRect(overlay_draw_list);

    free(hypothetical_snippets);
    free(solver_list);
  }
}

void renderer_draw_selection_box(timeline_state_t *ts, ImDrawList *overlay_draw_list) {
  float dpi_scale = gfx_get_ui_scale();
  if (!ts->selection_box_active) return;
  ImRect rect = {{fminf(ts->selection_box_start.x, ts->selection_box_end.x), fminf(ts->selection_box_start.y, ts->selection_box_end.y)},
                 {fmaxf(ts->selection_box_start.x, ts->selection_box_end.x), fmaxf(ts->selection_box_start.y, ts->selection_box_end.y)}};
  ImDrawList_AddRectFilled(overlay_draw_list, rect.Min, rect.Max, IM_COL32(100, 150, 240, 80), 0.0f, 0);
  ImDrawList_AddRect(overlay_draw_list, rect.Min, rect.Max, IM_COL32(100, 150, 240, 180), 0.0f, 0, 1.0f * dpi_scale);
}

// Static Render Helpers

static void render_player_track(timeline_state_t *ts, int track_index, ImDrawList *draw_list, ImRect timeline_bb, float track_top, float track_bottom,
                                bool is_selected) {
  float dpi_scale = gfx_get_ui_scale();
  player_track_t *track = &ts->player_tracks[track_index];

  ImU32 track_bg_col;
  if (is_selected) {
    track_bg_col = igGetColorU32_Col(ImGuiCol_FrameBgHovered, 1.0f);
  } else {
    track_bg_col = (track_index % 2 == 0) ? igGetColorU32_Col(ImGuiCol_TitleBg, 1.0f) : igGetColorU32_Col(ImGuiCol_WindowBg, 1.0f);
  }

  ImDrawList_AddRectFilled(draw_list, (ImVec2){timeline_bb.Min.x, track_top}, (ImVec2){timeline_bb.Max.x, track_bottom}, track_bg_col, 0.0f, 0);
  ImDrawList_AddLine(draw_list, (ImVec2){timeline_bb.Min.x, track_bottom}, (ImVec2){timeline_bb.Max.x, track_bottom},
                     igGetColorU32_Col(ImGuiCol_Border, 0.3f), 1.0f * dpi_scale);

  for (int j = 0; j < track->snippet_count; ++j) {
    render_input_snippet(ts, track, &track->snippets[j], draw_list, timeline_bb, track_top, false);
  }

  if (ts->recording) {
    for (int j = 0; j < track->recording_snippet_count; ++j) {
      render_input_snippet(ts, track, &track->recording_snippets[j], draw_list, timeline_bb, track_top, true);
    }
  }
}

static void render_input_snippet(timeline_state_t *ts, player_track_t *track, input_snippet_t *snippet, ImDrawList *draw_list, ImRect timeline_bb,
                                 float track_top, bool is_recording_snippet) {
  float dpi_scale = gfx_get_ui_scale();
  float start_x = renderer_tick_to_screen_x(ts, snippet->start_tick, timeline_bb.Min.x);
  float end_x = renderer_tick_to_screen_x(ts, snippet->end_tick, timeline_bb.Min.x);
  if (end_x < timeline_bb.Min.x || start_x > timeline_bb.Max.x) return;

  int stack_size = model_get_stack_size_at_tick_range(track, snippet->start_tick, snippet->end_tick);
  float sub_lane_height = (ts->track_height * dpi_scale) / (float)fmax(1, stack_size);

  ImVec2 min = {fmaxf(start_x, timeline_bb.Min.x), track_top + snippet->layer * sub_lane_height + 2.0f * dpi_scale};
  ImVec2 max = {fminf(end_x, timeline_bb.Max.x), min.y + sub_lane_height - 4.0f * dpi_scale};
  if (max.y <= min.y) return;

  bool is_selected = interaction_is_snippet_selected(ts, snippet->id);
  ImU32 color;
  if (is_recording_snippet) {
    color = IM_COL32(255, 30, 0, 100);
  } else {
    color = snippet->is_active
                ? (is_selected ? igGetColorU32_Col(ImGuiCol_HeaderActive, 1.0f) : igGetColorU32_Col(ImGuiCol_Button, 0.8f))
                : (is_selected ? igGetColorU32_Vec4((ImVec4){0.45f, 0.45f, 0.45f, 1.0f}) : igGetColorU32_Vec4((ImVec4){0.25f, 0.25f, 0.25f, 0.9f}));
  }

  ImDrawList_AddRectFilled(draw_list, min, max, color, 4.0f * dpi_scale, ImDrawFlags_RoundCornersAll);
  ImDrawList_AddRect(draw_list, min, max,
                     is_selected ? igGetColorU32_Col(ImGuiCol_NavWindowingHighlight, 1.0f) : igGetColorU32_Col(ImGuiCol_Border, 0.6f), 4.0f * dpi_scale,
                     ImDrawFlags_RoundCornersAll, (is_selected ? 2.0f : 1.0f) * dpi_scale);
}
