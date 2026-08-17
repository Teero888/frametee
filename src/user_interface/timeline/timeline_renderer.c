#include <engine/int_math.h>
#include <engine/prediction.h>
#include "timeline_renderer.h"
#include "renderer/graphics_backend.h"
#include "timeline_commands.h"
#include "timeline_interaction.h"
#include "timeline_model.h"
#include "../timeline_events.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <frametee/icons.h>
#include <system/include_cimgui.h>
#include <user_interface/undo_redo.h>
#include <user_interface/widgets/imcol.h>

#define MIN_TIMELINE_ZOOM 0.05f
#define MAX_TIMELINE_ZOOM 20.0f
#define TPS 50

typedef struct {
  bool active;
  int track_index;
  char before[MAX_TRACK_NAME];
} RendererTrackNameEditUndo;

static RendererTrackNameEditUndo g_track_name_edit_undo;

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

float renderer_get_track_row_height(const timeline_state_t *ts) {
  return (ts->track_height * gfx_get_ui_scale()) + igGetStyle()->ItemSpacing.y;
}

float renderer_get_track_screen_y(const timeline_state_t *ts, int track_index) {
  return ts->tracks_origin_y + (float)track_index * renderer_get_track_row_height(ts);
}

int renderer_screen_y_to_track_index(const timeline_state_t *ts, float screen_y) {
  float content_y = screen_y - ts->tracks_origin_y;
  if (content_y < 0) return -1;

  int track_index = (int)floorf(content_y / renderer_get_track_row_height(ts));
  return (track_index >= ts->player_track_count) ? -1 : track_index;
}

// Playheads are drawn from left to right. When their handles overlap, this leaves the rightmost
// playhead in front. The unclamped tick breaks ties at tick zero, where several groups can share
// the same visible position while approaching it from different offsets.
static int renderer_next_playhead_group(const timeline_state_t *ts, int previous_group) {
  int next_group = -1;
  int previous_offset = previous_group >= 0 ? ts->groups[previous_group]->start_offset : 0;

  for (int group_index = 0; group_index < ts->group_count; ++group_index) {
    int offset = ts->groups[group_index]->start_offset;
    // Unclamped playhead ticks are current_tick - start_offset. Visit distinct offsets from
    // largest to smallest to draw their playheads from left to right and deduplicate groups whose
    // offset is exactly equal.
    if (previous_group >= 0 && offset >= previous_offset) continue;

    if (next_group < 0) {
      next_group = group_index;
      continue;
    }

    int next_offset = ts->groups[next_group]->start_offset;
    if (offset > next_offset || (offset == next_offset && group_index < next_group)) next_group = group_index;
  }
  return next_group;
}

int renderer_hit_test_playhead_handle(const timeline_state_t *ts, ImRect header_bb, ImVec2 position) {
  float dpi_scale = gfx_get_ui_scale();
  float handle_top = header_bb.Max.y - 11.0f * dpi_scale;
  if (position.y < handle_top || position.y > header_bb.Max.y + 1.0f * dpi_scale) return -1;

  int frontmost_group = -1;
  float hit_half_width = 7.0f * dpi_scale;
  for (int group_index = renderer_next_playhead_group(ts, -1); group_index >= 0;
       group_index = renderer_next_playhead_group(ts, group_index)) {
    float x = renderer_tick_to_screen_x(ts, model_group_playhead_tick(ts, group_index), header_bb.Min.x);
    if (x < header_bb.Min.x || x > header_bb.Max.x) continue;
    if (fabsf(position.x - x) <= hit_half_width) frontmost_group = group_index;
  }
  return frontmost_group;
}

int renderer_find_nearest_playhead(const timeline_state_t *ts, ImRect header_bb, float position_x) {
  int nearest_group = -1;
  float nearest_distance = 0.0f;
  for (int group_index = renderer_next_playhead_group(ts, -1); group_index >= 0;
       group_index = renderer_next_playhead_group(ts, group_index)) {
    float x = renderer_tick_to_screen_x(ts, model_group_playhead_tick(ts, group_index), header_bb.Min.x);
    float distance = fabsf(position_x - x);
    // Render order is back-to-front, so an equal-distance candidate encountered later is the
    // visible one and must also win selection.
    if (nearest_group < 0 || distance <= nearest_distance) {
      nearest_group = group_index;
      nearest_distance = distance;
    }
  }
  return nearest_group;
}

// Main Rendering Functions

void renderer_draw_controls(timeline_state_t *ts) {
  float dpi_scale = gfx_get_ui_scale();
  float btn_gap = 6.0f * dpi_scale;

  igPushItemWidth(80 * dpi_scale);
  igDragInt("##CurrentTick", &ts->current_tick, 1, model_get_min_global_tick(ts), 100000000, "Tick %d", ImGuiSliderFlags_AlwaysClamp);
  igPopItemWidth();

  igSameLine(0, 10 * dpi_scale);
  if (ui_icon_button(ts->ui, ICON_FA_BACKWARD_STEP, (ImVec2){30 * dpi_scale, 0}))
    ts->current_tick = model_get_min_global_tick(ts);

  igSameLine(0, btn_gap);
  if (ui_icon_button(ts->ui, ICON_FA_BACKWARD, (ImVec2){30 * dpi_scale, 0})) model_advance_tick(ts, -ts->playback_speed);

  igSameLine(0, btn_gap);
  if (ui_icon_button(ts->ui, ts->is_playing ? ICON_FA_PAUSE : ICON_FA_PLAY, (ImVec2){45 * dpi_scale, 0})) {
    ts->is_playing = !ts->is_playing;
    if (ts->is_playing) {
      if (ts->recording && ts->recording_snippets.count > 0) {
        input_snippet_t *recording_snippet = ts->recording_snippets.snippets[0];
        if (recording_snippet)
          ts->current_tick = recording_snippet->end_tick + ts->groups[ts->active_group_index]->start_offset;
      }
      ts->last_update_time = igGetTime();
    }
  }

  igSameLine(0, btn_gap);
  if (ui_icon_button(ts->ui, ICON_FA_FORWARD, (ImVec2){30 * dpi_scale, 0})) model_advance_tick(ts, ts->playback_speed);

  igSameLine(0, btn_gap);
  if (ui_icon_button(ts->ui, ICON_FA_FORWARD_STEP, (ImVec2){30 * dpi_scale, 0})) {
    ts->current_tick = model_get_max_timeline_tick(ts);
  }

  igSameLine(0, 12 * dpi_scale);
  igText("Zoom");
  igSameLine(0, 4 * dpi_scale);
  igSetNextItemWidth(75 * dpi_scale);
  igSliderFloat("##Zoom", &ts->zoom, MIN_TIMELINE_ZOOM, MAX_TIMELINE_ZOOM, "%.2f", ImGuiSliderFlags_Logarithmic);

  igSameLine(0, 12 * dpi_scale);
  igText("Speed");
  igSameLine(0, 4 * dpi_scale);
  igSetNextItemWidth(75 * dpi_scale);
  const int active_tps = game_ticks_per_second(&ts->ui->gfx_handler->game_host);
  const int max_speed = active_tps > 50 ? active_tps * 2 : 100;
  igSliderInt("##Speed", &ts->gui_playback_speed, 1, max_speed, "%d", ImGuiSliderFlags_None);

  igSameLine(0, 14 * dpi_scale);

  // Cycles the camera modes the active game declared. The button lights up
  // whenever the current mode is one the game drives rather than the user.
  camera_t *camera = &ts->ui->gfx_handler->renderer.camera;
  game_host_t *camera_host = &ts->ui->gfx_handler->game_host;
  const unsigned mode_count = game_camera_mode_count(camera_host);
  const ft_camera_mode *mode = game_camera_mode(camera_host, camera->mode);
  const bool directed = mode && (mode->flags & FT_CAMERA_MODE_DIRECTED) != 0;

  if (directed) {
    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){1., 0.48f, 0.1f, 1.0f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){1., 0.58f, 0.1f, 1.0f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){1., 0.68f, 0.1f, 1.0f});
  }
  if (ui_icon_button(ts->ui, ICON_FA_VIDEO, (ImVec2){30 * dpi_scale, 0}) && mode_count > 0) {
    camera->mode = (camera->mode + 1) % mode_count;
  }
  if (igIsItemHovered(ImGuiHoveredFlags_None)) {
    const ft_camera_mode *next = game_camera_mode(camera_host, mode_count ? (camera->mode + 1) % mode_count : 0);
    igSetTooltip("Camera: %s%s\nClick for %s", mode ? mode->display_name : "?", directed ? " (active)" : "",
                 next ? next->display_name : "?");
  }
  if (directed) {
    igPopStyleColor(3);
  }

  igSameLine(0, btn_gap);

  prediction_render_menu(ts);

  igSameLine(0, btn_gap);

  if (igButton(ts->recording ? "Recording..." : "Record", (ImVec2){125 * dpi_scale, 0})) {
    interaction_toggle_recording(ts);
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

    // Format labels based on the time scale. Seconds come from the active game's
    // tick rate, so a 60 Hz game reads correctly on the same ruler as a 50 Hz one.
    const int tps = game_ticks_per_second(&ts->ui->gfx_handler->game_host);
    char label[64];
    if (tick < tps) {
      snprintf(label, sizeof(label), "%d", tick);
    } else if (tick < tps * 60) { // Under 1 minute
      snprintf(label, sizeof(label), "%.1fs", (double)tick / (double)tps);
    } else { // Over 1 minute
      int total_secs = tick / tps;
      int mins = total_secs / 60;
      int secs = total_secs % 60;
      snprintf(label, sizeof(label), "%d:%02d", mins, secs);
    }

    ImVec2 text_size = igCalcTextSize(label, NULL, false, 0);
    ImVec2 text_pos = {x - text_size.x * 0.5f, header_bb.Min.y + 2.0f * dpi_scale};

    ImDrawList_AddText_Vec2(draw_list, text_pos, tick_text_col, label, NULL);
  }

  // Draw markers for events reported by the active game.
  for (int i = 0; i < ts->event_count; ++i) {
    timeline_event_t *ev = &ts->events[i];
    if (ev->group_index < 0 || ev->group_index >= ts->group_count) continue;
    int global_tick = ev->tick + ts->groups[ev->group_index]->start_offset;
    if (global_tick < start_tick || global_tick > end_tick) continue;
    timeline_group_t *group = ts->groups[ev->group_index];
    ImU32 event_marker_col = igColorConvertFloat4ToU32((ImVec4){group->color[0], group->color[1], group->color[2], 1.0f});
    float x = renderer_tick_to_screen_x(ts, global_tick, header_bb.Min.x);
    if (x >= header_bb.Min.x && x <= header_bb.Max.x) {
      ImVec2 p1 = {x - 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale};
      ImVec2 p2 = {x + 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale};
      ImVec2 p3 = {x, header_bb.Max.y - 4 * dpi_scale};
      ImDrawList_AddTriangleFilled(draw_list, p1, p2, p3, event_marker_col);

      // Optional: Hover tooltip for the event
      if (igIsMouseHoveringRect((ImVec2){x - 4 * dpi_scale, header_bb.Max.y - 12 * dpi_scale}, (ImVec2){x + 4 * dpi_scale, header_bb.Max.y - 4 * dpi_scale}, true)) {
        igBeginTooltip();
        igText("Group: %s", group->name);
        timeline_event_tooltip_content(ev);
        igEndTooltip();
      }
    }
  }

  ImDrawList_PopClipRect(draw_list);
}

void renderer_draw_playhead_line(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect) {
  if (ts->player_track_count <= 0) return;
  float dpi_scale = gfx_get_ui_scale();
  float y0 = renderer_get_track_screen_y(ts, 0);
  float y1 = renderer_get_track_screen_y(ts, ts->player_track_count - 1) + ts->track_height * dpi_scale;
  for (int group_index = renderer_next_playhead_group(ts, -1); group_index >= 0;
       group_index = renderer_next_playhead_group(ts, group_index)) {
    int playhead_tick = model_group_playhead_tick(ts, group_index);

    float playhead_x = renderer_tick_to_screen_x(ts, playhead_tick, timeline_rect.Min.x);
    if (playhead_x < timeline_rect.Min.x || playhead_x > timeline_rect.Max.x) continue;
    ImU32 color = group_index == 0 ? igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f)
                                  : igGetColorU32_Vec4((ImVec4){ts->groups[group_index]->color[0], ts->groups[group_index]->color[1],
                                                                ts->groups[group_index]->color[2], 0.95f});
    ImDrawList_AddLine(draw_list, (ImVec2){playhead_x, y0}, (ImVec2){playhead_x, y1}, color, 2.0f * dpi_scale);
  }
}

void renderer_draw_playhead_handle(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect, ImRect header_bb) {
  float dpi_scale = gfx_get_ui_scale();
  for (int group_index = renderer_next_playhead_group(ts, -1); group_index >= 0;
       group_index = renderer_next_playhead_group(ts, group_index)) {
    int playhead_tick = model_group_playhead_tick(ts, group_index);
    float group_x = renderer_tick_to_screen_x(ts, playhead_tick, timeline_rect.Min.x);
    if (group_x < timeline_rect.Min.x || group_x > timeline_rect.Max.x) continue;
    ImU32 color = group_index == 0 ? igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f)
                                  : igGetColorU32_Vec4((ImVec4){ts->groups[group_index]->color[0], ts->groups[group_index]->color[1],
                                                                ts->groups[group_index]->color[2], 0.95f});
    ImVec2 head_bottom = {group_x + 0.5f, header_bb.Max.y + 0.5f};
    ImVec2 head_top_left = {(head_bottom.x - 6.0f * dpi_scale) + 0.5f, head_bottom.y - 10.0f * dpi_scale + 0.5f};
    ImVec2 head_top_right = {(head_bottom.x + 6.0f * dpi_scale) - 0.5f, head_bottom.y - 10.0f * dpi_scale + 0.5f};
    ImDrawList_AddTriangleFilled(draw_list, head_top_left, head_top_right, head_bottom, color);
    ImDrawList_AddLine(draw_list, (ImVec2){group_x, header_bb.Max.y - 5.0f * dpi_scale}, (ImVec2){group_x, header_bb.Max.y}, color,
                       2.0f * dpi_scale);
  }
}

void renderer_draw_tracks_area(timeline_state_t *ts, ImRect timeline_bb) {
  float dpi_scale = gfx_get_ui_scale();
  float track_header_width = 120.0f * dpi_scale;
  ImDrawList *draw_list = igGetWindowDrawList();
  int pending_clone_track = -1;
  int pending_clone_group = -1;

  // The rows are laid out from this origin, and hit testing derives the same positions from it.
  ts->tracks_origin_y = igGetCursorScreenPos().y;

  ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
  float total_row_height = renderer_get_track_row_height(ts);
  ImGuiListClipper_Begin(clipper, ts->player_track_count, total_row_height);
  while (ImGuiListClipper_Step(clipper)) {
    for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
      // Pin the row instead of letting the cursor accumulate: ImGui truncates it after every
      ImVec2 row_start_pos = {igGetCursorScreenPos().x, renderer_get_track_screen_y(ts, i)};
      igSetCursorScreenPos(row_start_pos);
      player_track_t *track = &ts->player_tracks[i];
      timeline_group_t *group = ts->groups[track->group_index];
      const bool supports_linked = game_has_cap(&ts->ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS);
      if (!supports_linked) track->is_linked = false;

      // Render Track Info Panel (Left)
      bool is_track_selected = (ts->selected_player_track_index == i);
      ImU32 header_bg_col = track->is_linked ? igGetColorU32_Col(ImGuiCol_TextLink, 0.6f) : igGetColorU32_Col(ImGuiCol_FrameBg, 0.8f);

      ImVec2 header_rect_min = row_start_pos;
      ImVec2 header_rect_max = {row_start_pos.x + track_header_width, row_start_pos.y + (ts->track_height * dpi_scale)};
      ImDrawList_AddRectFilled(draw_list, header_rect_min, header_rect_max, header_bg_col, 0.0f, 0);
      ImDrawList_AddLine(draw_list, (ImVec2){header_rect_max.x, header_rect_min.y}, header_rect_max, igGetColorU32_Col(ImGuiCol_Border, 0.5f), 1.0f * dpi_scale);

      // The colored claw is the visual group binding from the issue mockup. Its caps only appear
      // on the first/last row, so adjacent rows read as one physics instance without spending a
      // separate header row.
      ImU32 group_color = igGetColorU32_Vec4((ImVec4){group->color[0], group->color[1], group->color[2], 1.0f});
      float claw_x = row_start_pos.x + 3.0f * dpi_scale;
      ImDrawList_AddLine(draw_list, (ImVec2){claw_x, header_rect_min.y}, (ImVec2){claw_x, header_rect_max.y}, group_color, 4.0f * dpi_scale);
      bool first_in_group = i == 0 || ts->player_tracks[i - 1].group_index != track->group_index;
      bool last_in_group = i == ts->player_track_count - 1 || ts->player_tracks[i + 1].group_index != track->group_index;
      if (first_in_group)
        ImDrawList_AddLine(draw_list, (ImVec2){claw_x, header_rect_min.y + 2.0f * dpi_scale},
                           (ImVec2){claw_x + 14.0f * dpi_scale, header_rect_min.y + 2.0f * dpi_scale}, group_color, 4.0f * dpi_scale);
      if (last_in_group)
        ImDrawList_AddLine(draw_list, (ImVec2){claw_x, header_rect_max.y - 2.0f * dpi_scale},
                           (ImVec2){claw_x + 14.0f * dpi_scale, header_rect_max.y - 2.0f * dpi_scale}, group_color, 4.0f * dpi_scale);

      igPushID_Int(i);

      // Draw track name. Linked tracks get a compact visual marker.
      igSetCursorScreenPos((ImVec2){row_start_pos.x + 20.0f * dpi_scale, row_start_pos.y + ((ts->track_height * dpi_scale) - igGetTextLineHeight()) * 0.5f});
      if (track->is_linked) {
        igTextDisabled("[L]");
        igSameLine(0, 4.0f * dpi_scale);
      }
      igText("%s", track->name[0] ? track->name : "Track");

      // Add a single invisible button over the header for interactions.
      igSetCursorScreenPos(row_start_pos);
      igInvisibleButton("##track_header_interact", (ImVec2){track_header_width, ts->track_height * dpi_scale}, 0);

      // Handle interactions: double-click to toggle linking, single-click to select.
      if (igIsItemHovered(0)) {
        if (supports_linked && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
          if (igGetIO_Nil()->KeyShift) {
            for (int t = 0; t < ts->player_track_count; ++t)
              if (ts->player_tracks[t].group_index == track->group_index) ts->player_tracks[t].is_linked ^= 1;
          } else track->is_linked = !track->is_linked;

        } else if (igIsItemClicked(ImGuiMouseButton_Left)) {
          interaction_select_track(ts, i);
        }
      }

      if (igBeginPopupContextItem("TrackSettings", 1)) {
        char name_before_frame[MAX_TRACK_NAME];
        memcpy(name_before_frame, track->name, sizeof(name_before_frame));
        igSetNextItemWidth(180.0f * dpi_scale);
        bool name_changed = igInputText("Name", track->name, sizeof(track->name), ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        if (igIsItemActivated()) {
          g_track_name_edit_undo.active = true;
          g_track_name_edit_undo.track_index = i;
          memcpy(g_track_name_edit_undo.before, name_before_frame, sizeof(g_track_name_edit_undo.before));
        }
        if (name_changed) timeline_mark_unsaved(ts);
        if (igIsItemDeactivatedAfterEdit() && g_track_name_edit_undo.active && g_track_name_edit_undo.track_index == i) {
          undo_command_t *command = commands_create_track_name_change(ts->ui, i, g_track_name_edit_undo.before);
          if (command) undo_manager_register_command(&ts->ui->undo_manager, command);
          g_track_name_edit_undo.active = false;
        }
        if (ts->group_count > 1 && igBeginMenu("Clone to group", !ts->recording)) {
          for (int target_group = 0; target_group < ts->group_count; ++target_group) {
            if (target_group == track->group_index) continue;
            if (igMenuItem_Bool(ts->groups[target_group]->name, NULL, false, true)) {
              pending_clone_track = i;
              pending_clone_group = target_group;
            }
          }
          igEndMenu();
        }
        if (supports_linked) {
          igSeparator();
          if (track->is_linked) {
            game_host_t *host = &ts->ui->gfx_handler->game_host;
            const ft_input_schema *schema = game_input_schema(host);
            const int player_count = model_group_track_count(ts, track->group_index);
            const int target_player = model_group_local_track_index(ts, i);
            if (track->linked_source_player < 0 || track->linked_source_player >= player_count ||
                track->linked_source_player == target_player) {
              track->linked_source_player = target_player == 0 && player_count > 1 ? 1 : 0;
            }

            char source_label[96] = "No source";
            const int source_track = model_group_track_index(ts, track->group_index, track->linked_source_player);
            if (source_track >= 0)
              snprintf(source_label, sizeof(source_label), "%d: %s", track->linked_source_player + 1,
                       ts->player_tracks[source_track].name);
            if (igBeginCombo("Source", source_label, 0)) {
              for (int local = 0; local < player_count; ++local) {
                if (local == target_player) continue;
                const int candidate = model_group_track_index(ts, track->group_index, local);
                if (candidate < 0) continue;
                char label[96];
                snprintf(label, sizeof(label), "%d: %s", local + 1, ts->player_tracks[candidate].name);
                if (igSelectable_Bool(label, local == track->linked_source_player, 0, (ImVec2){0, 0})) {
                  track->linked_source_player = local;
                  timeline_mark_unsaved(ts);
                }
              }
              igEndCombo();
            }

            igText("Copied fields");
            igSeparator();
            bool has_mirror_x = false;
            bool has_mirror_y = false;
            if (schema) {
              for (uint32_t field_index = 0; field_index < schema->field_count && field_index < 64; ++field_index) {
                const ft_input_field *field = &schema->fields[field_index];
                if (field->flags & FT_INPUT_FLAG_INTERNAL) continue;
                bool copied = (track->linked_copy_fields & (UINT64_C(1) << field_index)) != 0;
                if (igCheckbox(field->display_name ? field->display_name : field->id, &copied)) {
                  track->linked_copy_fields ^= UINT64_C(1) << field_index;
                  timeline_mark_unsaved(ts);
                }
                has_mirror_x |= (field->flags & FT_INPUT_FLAG_MIRROR_X) != 0;
                has_mirror_y |= (field->flags & FT_INPUT_FLAG_MIRROR_Y) != 0;
              }
            }

            if (has_mirror_x || has_mirror_y) igSeparator();
            if (has_mirror_x) {
              bool mirror = (track->linked_transform_flags & FT_LINKED_MIRROR_X) != 0;
              if (igCheckbox("Mirror horizontally", &mirror)) {
                track->linked_transform_flags ^= FT_LINKED_MIRROR_X;
                timeline_mark_unsaved(ts);
              }
            }
            if (has_mirror_y) {
              bool mirror = (track->linked_transform_flags & FT_LINKED_MIRROR_Y) != 0;
              if (igCheckbox("Mirror vertically", &mirror)) {
                track->linked_transform_flags ^= FT_LINKED_MIRROR_Y;
                timeline_mark_unsaved(ts);
              }
            }
          } else {
            igTextDisabled("Not a linked track");
            igTextDisabled("Double-click header to toggle");
          }
        }
        igEndPopup();
      }

      igPopID();

      // Render Track Snippets (Right)
      float track_top = row_start_pos.y;
      float track_bottom = track_top + (ts->track_height * dpi_scale);
      render_player_track(ts, i, draw_list, timeline_bb, track_top, track_bottom, is_track_selected);

      igSetCursorScreenPos(row_start_pos);
      ImVec2 avail = igGetContentRegionAvail();
      igDummy((ImVec2){avail.x, ts->track_height * dpi_scale});
    }
  }
  ImGuiListClipper_End(clipper);
  ImGuiListClipper_destroy(clipper);
  if (pending_clone_track >= 0) {
    timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
    if (before && model_clone_track_to_group(ts, pending_clone_track, pending_clone_group, NULL)) {
      undo_command_t *command = commands_create_timeline_data_change(ts->ui, before, "Clone Track to Group");
      if (command) undo_manager_register_command(&ts->ui->undo_manager, command);
    } else commands_free_timeline_data_snapshot(before);
  }
}

void renderer_draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb) {
  float dpi_scale = gfx_get_ui_scale();
  if (!ts->drag_state.active) return;

  int snapped_start_tick_clicked, base_track_index;
  interaction_calculate_drag_destination(ts, timeline_bb, &snapped_start_tick_clicked, &base_track_index);

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
        float target_track_top = renderer_get_track_screen_y(ts, track_idx);
        float preview_min_y = target_track_top + preview_snip->layer * sub_lane_height + 2.0f * dpi_scale;
        float preview_max_y = preview_min_y + sub_lane_height - 4.0f * dpi_scale;

        timeline_group_t *target_group = ts->groups[track->group_index];
        ImU32 fill = igGetColorU32_Vec4((ImVec4){target_group->color[0], target_group->color[1], target_group->color[2], 0.35f});
        ImDrawList_AddRectFilled(overlay_draw_list, (ImVec2){preview_min_x, preview_min_y}, (ImVec2){preview_max_x, preview_max_y}, fill, 4.0f * dpi_scale,
                                 ImDrawFlags_RoundCornersAll);
        if (igGetIO_Nil()->KeyAlt)
          ImDrawList_AddRect(overlay_draw_list, (ImVec2){preview_min_x, preview_min_y}, (ImVec2){preview_max_x, preview_max_y},
                             IM_COL32(100, 240, 150, 210), 4.0f * dpi_scale, ImDrawFlags_RoundCornersAll, 2.0f * dpi_scale);
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
  timeline_group_t *group = ts->groups[track->group_index];
  ImVec4 selected_color = {
      group->color[0] + (1.0f - group->color[0]) * 0.24f,
      group->color[1] + (1.0f - group->color[1]) * 0.24f,
      group->color[2] + (1.0f - group->color[2]) * 0.24f,
      1.0f,
  };
  ImVec4 selected_border = {
      group->color[0] + (1.0f - group->color[0]) * 0.52f,
      group->color[1] + (1.0f - group->color[1]) * 0.52f,
      group->color[2] + (1.0f - group->color[2]) * 0.52f,
      1.0f,
  };
  ImU32 color;
  if (is_recording_snippet) {
    color = IM_COL32(255, 30, 0, 100);
  } else {
    ImVec4 group_color = {group->color[0], group->color[1], group->color[2], 0.86f};
    color = snippet->is_active
                ? (is_selected ? igGetColorU32_Vec4(selected_color) : igGetColorU32_Vec4(group_color))
                : (is_selected ? igGetColorU32_Vec4((ImVec4){group->color[0] * 0.55f, group->color[1] * 0.55f, group->color[2] * 0.55f, 1.0f})
                               : igGetColorU32_Vec4((ImVec4){0.25f, 0.25f, 0.25f, 0.9f}));
  }

  ImDrawList_AddRectFilled(draw_list, min, max, color, 4.0f * dpi_scale, ImDrawFlags_RoundCornersAll);
  ImDrawList_AddRect(draw_list, min, max,
                     is_selected ? igGetColorU32_Vec4(selected_border) : igGetColorU32_Col(ImGuiCol_Border, 0.6f), 4.0f * dpi_scale,
                     ImDrawFlags_RoundCornersAll, (is_selected ? 2.0f : 1.0f) * dpi_scale);
}
