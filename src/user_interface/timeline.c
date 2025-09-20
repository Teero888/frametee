#include "timeline.h"
#include "cimgui.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TPS 50
#define MIN_TIMELINE_ZOOM 0.05f
#define MAX_TIMELINE_ZOOM 20.0f
#define SNAP_THRESHOLD_PX 5.0f // Snap threshold in pixels
#define DEFAULT_TRACK_HEIGHT 40.f

#include "../renderer/graphics_backend.h"

void timeline_update_inputs(timeline_state_t *ts, gfx_handler_t *gfx) {
  if (!ts->recording || !ts->recording_snippet)
    return;

  ts->recording_input.m_Direction = igIsKeyDown_Nil(ImGuiKey_D) - igIsKeyDown_Nil(ImGuiKey_A);
  ts->recording_input.m_Jump = igIsKeyDown_Nil(ImGuiKey_Space);
  ts->recording_input.m_Hook = igIsMouseDown_Nil(ImGuiMouseButton_Right);

  ts->recording_input.m_TargetX += (int)gfx->raw_mouse.dx;
  ts->recording_input.m_TargetY += (int)gfx->raw_mouse.dy;
  gfx->raw_mouse.dx = gfx->raw_mouse.dy = 0.0;

  if (vlength(vec2_init(ts->recording_input.m_TargetX, ts->recording_input.m_TargetY)) > 500.f) {
    mvec2 n = vnormalize(vec2_init(ts->recording_input.m_TargetX, ts->recording_input.m_TargetY));
    ts->recording_input.m_TargetX = vgetx(n) * 500.f;
    ts->recording_input.m_TargetY = vgety(n) * 500.f;
  }
  // printf("%d,%d\n", ts->recording_input.m_TargetX, ts->recording_input.m_TargetY);

  ts->recording_input.m_Fire = igIsMouseDown_Nil(ImGuiMouseButton_Left);
  ts->recording_input.m_WantedWeapon = igIsKeyDown_Nil(ImGuiKey_1)   ? 0
                                       : igIsKeyDown_Nil(ImGuiKey_2) ? 1
                                       : igIsKeyDown_Nil(ImGuiKey_3) ? 2
                                       : igIsKeyDown_Nil(ImGuiKey_4) ? 3
                                       : igIsKeyDown_Nil(ImGuiKey_5) ? 4
                                                                     : ts->recording_input.m_WantedWeapon;
}

SPlayerInput get_input(const timeline_state_t *ts, int track_index, int tick) {
  const player_track_t *track = &ts->player_tracks[track_index];
  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (tick < snippet->end_tick && tick >= snippet->start_tick)
      return snippet->inputs[tick - snippet->start_tick];
  }
  return (SPlayerInput){.m_TargetY = -1};
}

void init_snippet_inputs(input_snippet_t *snippet) {
  int duration = snippet->end_tick - snippet->start_tick;
  if (duration <= 0) {
    snippet->inputs = NULL;
    snippet->input_count = 0;
    return;
  }
  snippet->inputs = calloc(duration, sizeof(SPlayerInput));
  snippet->input_count = duration;
}

void copy_snippet_inputs(input_snippet_t *dest, const input_snippet_t *src) {
  dest->input_count = src->input_count;
  if (src->inputs && src->input_count > 0) {
    dest->inputs = malloc(src->input_count * sizeof(SPlayerInput));
    memcpy(dest->inputs, src->inputs, src->input_count * sizeof(SPlayerInput));
  } else {
    dest->inputs = NULL;
  }
}

void free_snippet_inputs(input_snippet_t *snippet) {
  if (snippet->inputs) {
    free(snippet->inputs);
    snippet->inputs = NULL;
  }
  snippet->input_count = 0;
}

void resize_snippet_inputs(input_snippet_t *snippet, int new_duration) {
  if (new_duration <= 0) {
    free_snippet_inputs(snippet);
    snippet->start_tick = snippet->end_tick;
    return;
  }
  if (snippet->input_count == new_duration)
    return;
  snippet->inputs = realloc(snippet->inputs, sizeof(SPlayerInput) * new_duration);
  if (!snippet->inputs) {
    snippet->input_count = 0;
    return;
  }
  if (new_duration > snippet->input_count) {
    memset(&snippet->inputs[snippet->input_count], 0,
           (new_duration - snippet->input_count) * sizeof(SPlayerInput));
  }
  snippet->input_count = new_duration;
}

// Converts screen X position to timeline tick
int screen_x_to_tick(const timeline_state_t *ts, float screen_x, float timeline_start_x) {
  return ts->view_start_tick + (int)((screen_x - timeline_start_x) / ts->zoom);
}

// Converts timeline tick to screen X position
float tick_to_screen_x(const timeline_state_t *ts, int tick, float timeline_start_x) {
  return timeline_start_x + (tick - ts->view_start_tick) * ts->zoom;
}

// Finds a snippet by its ID within a track
input_snippet_t *find_snippet_by_id(player_track_t *track, int snippet_id) {
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      return &track->snippets[i];
    }
  }
  return NULL; // Not found
}

void advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, 0);

  // record new tick
  if (ts->recording_snippet && ts->current_tick > ts->recording_snippet->start_tick) {
    resize_snippet_inputs(ts->recording_snippet, ts->current_tick - ts->recording_snippet->start_tick);
    ts->recording_snippet->end_tick = ts->current_tick;
    if (ts->recording_snippet->input_count > 0)
      ts->recording_snippet->inputs[ts->recording_snippet->input_count - 1] = ts->recording_input;
  }
}

// Finds a snippet by its ID and track index
input_snippet_t *find_snippet_by_id_and_track(timeline_state_t *ts, int snippet_id, int track_idx) {
  if (track_idx < 0 || track_idx >= ts->player_track_count)
    return NULL;
  return find_snippet_by_id(&ts->player_tracks[track_idx], snippet_id);
}

// Calculates a snapped tick position based on nearby snippet edges
// Considers snapping both the start and end of the dragged snippet.
int calculate_snapped_tick(const timeline_state_t *ts, int desired_start_tick, int dragged_snippet_duration,
                           int exclude_snippet_id) {
  int snapped_start_tick = desired_start_tick; // Default to no snapping
  float snap_threshold_ticks = SNAP_THRESHOLD_PX / ts->zoom;
  float min_distance = snap_threshold_ticks + 1; // Initialize min_distance > threshold

  // The potential tick that would become the *start* of the dragged snippet after snapping
  int candidate_snapped_start_tick = desired_start_tick;

  // --- Check snapping to other snippet edges ---
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *other = &track->snippets[j];
      if (other->id == exclude_snippet_id)
        continue; // Don't snap to the snippet itself

      // Potential snap targets from the other snippet's edges
      int other_snap_targets[] = {other->start_tick, other->end_tick};

      for (int k = 0; k < 2; ++k) {
        int other_edge_tick = other_snap_targets[k];

        // Option 1: Snap the dragged snippet's START to the other snippet's edge
        float dist_start_to_other_edge = fabsf((float)(desired_start_tick - other_edge_tick));
        if (dist_start_to_other_edge < min_distance) {
          min_distance = dist_start_to_other_edge;
          candidate_snapped_start_tick = other_edge_tick; // The new start tick is the other snippet's edge
        }

        // Option 2: Snap the dragged snippet's END to the other snippet's edge
        int desired_end_tick = desired_start_tick + dragged_snippet_duration;
        float dist_end_to_other_edge = fabsf((float)(desired_end_tick - other_edge_tick));
        // Check if this snap possibility is closer than the current minimum distance found so far
        if (dist_end_to_other_edge < min_distance) {
          min_distance = dist_end_to_other_edge;
          // If the END snaps here, the START must be this edge tick minus the duration
          candidate_snapped_start_tick = other_edge_tick - dragged_snippet_duration;
        }
      }
    }
  }

  // --- Check snapping to tick 0 ---
  // Option 1: Snap the dragged snippet's START to tick 0
  float dist_start_to_zero = fabsf((float)(desired_start_tick - 0));
  if (dist_start_to_zero < min_distance) {
    min_distance = dist_start_to_zero;
    candidate_snapped_start_tick = 0; // Snap start to 0
  }

  // Option 2: Snap the dragged snippet's END to tick 0 (implies start is negative, only useful if duration is
  // 0) Usually, snapping the start to 0 is sufficient for the left boundary. Omitting snapping end to 0. int
  // desired_end_tick = desired_start_tick + dragged_snippet_duration; // Already calculated float
  // dist_end_to_zero = fabsf((float)(desired_end_tick - 0)); if (dist_end_to_zero < min_distance) {
  //     min_distance = dist_end_to_zero;
  //     candidate_snapped_start_tick = 0 - dragged_snippet_duration; // Snap end to 0
  // }

  // If the minimum distance found is within the snap threshold, apply the snap.
  // Otherwise, the snippet does not snap, and we return the original desired tick.
  if (min_distance <= snap_threshold_ticks) {
    snapped_start_tick = candidate_snapped_start_tick;
  } else {
    snapped_start_tick = desired_start_tick; // No snap point within threshold
  }

  return snapped_start_tick;
}

// Checks if a snippet range overlaps with any snippets in a track (excluding one)
bool check_for_overlap(const player_track_t *track, int start_tick, int end_tick, int exclude_snippet_id) {
  if (start_tick >= end_tick)
    return false; // Invalid range

  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet_t *other = &track->snippets[i];
    if (other->id == exclude_snippet_id)
      continue; // Don't check against ourselves

    // Check for overlap: [start_tick, end_tick) and [other->start_tick, other->end_tick)
    // Overlap occurs if (start1 < end2 && end1 > start2)
    if (start_tick < other->end_tick && end_tick > other->start_tick) {
      return true; // Overlap detected
    }
  }
  return false; // No overlap
}

// Removes a snippet from a track by ID
// Returns true if removed, false if not found
bool remove_snippet_from_track(player_track_t *track, int snippet_id) {
  int found_idx = -1;
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1) {
    free_snippet_inputs(&track->snippets[found_idx]);
    memmove(&track->snippets[found_idx], &track->snippets[found_idx + 1],
            (track->snippet_count - found_idx - 1) * sizeof(input_snippet_t));
    track->snippet_count--;
    track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * track->snippet_count);
    return true;
  }
  return false;
}

void add_snippet_to_track(player_track_t *track, const input_snippet_t *snippet) {
  track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * (track->snippet_count + 1));
  if (track->snippets == NULL) {
    return;
  }
  input_snippet_t *dest = &track->snippets[track->snippet_count];
  *dest = *snippet; // shallow copy of fields
  if (snippet->input_count > 0 && snippet->inputs) {
    dest->inputs = malloc(snippet->input_count * sizeof(SPlayerInput));
    memcpy(dest->inputs, snippet->inputs, snippet->input_count * sizeof(SPlayerInput));
  } else {
    dest->inputs = NULL;
    dest->input_count = 0;
  }
  track->snippet_count++;
}

// Attempts to move a snippet to a new position and track, checking for overlaps
// Returns true if the move was successful, false otherwise.
bool try_move_snippet(timeline_state_t *ts, int snippet_id, int source_track_idx, int target_track_idx,
                      int desired_start_tick) {
  if (source_track_idx < 0 || source_track_idx >= ts->player_track_count || target_track_idx < 0 ||
      target_track_idx >= ts->player_track_count) {
    return false; // Invalid track indices
  }

  player_track_t *source_track = &ts->player_tracks[source_track_idx];
  player_track_t *target_track = &ts->player_tracks[target_track_idx];

  // Find the snippet in the source track
  int snippet_idx_in_source = -1;
  input_snippet_t snippet_to_move = {0}; // Data to copy if move is valid
  for (int i = 0; i < source_track->snippet_count; ++i) {
    if (source_track->snippets[i].id == snippet_id) {
      snippet_idx_in_source = i;
      snippet_to_move = source_track->snippets[i]; // Copy the data
      break;
    }
  }

  if (snippet_idx_in_source == -1) {
    return false; // Snippet not found in source track (shouldn't happen if drag_state is correct)
  }

  int duration = snippet_to_move.end_tick - snippet_to_move.start_tick;
  int new_start_tick = desired_start_tick;
  int new_end_tick = new_start_tick + duration;

  // Ensure new position is not before tick 0
  if (new_start_tick < 0) {
    new_start_tick = 0;
    new_end_tick = new_start_tick + duration;
  }

  // Check for overlaps in the target track at the *proposed* new position
  // If source and target are the same track, exclude the snippet itself from the overlap check
  int exclude_id = (source_track_idx == target_track_idx) ? snippet_id : -1;

  if (check_for_overlap(target_track, new_start_tick, new_end_tick, exclude_id)) {
    // Overlap detected at the desired position, cannot move here
    return false;
  }

  if (source_track_idx == target_track_idx) {
    // Update in place
    input_snippet_t *sn = &source_track->snippets[snippet_idx_in_source];
    sn->start_tick = new_start_tick;
    sn->end_tick = new_end_tick;
    int new_duration = new_end_tick - new_start_tick;
    resize_snippet_inputs(sn, new_duration);
    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = source_track_idx;
  } else {
    // Cross track: deep copy
    if (remove_snippet_from_track(source_track, snippet_id)) {
      snippet_to_move.start_tick = new_start_tick;
      snippet_to_move.end_tick = new_end_tick;

      input_snippet_t new_snip = snippet_to_move;
      new_snip.inputs = NULL;
      new_snip.input_count = 0;
      copy_snippet_inputs(&new_snip, &snippet_to_move);
      add_snippet_to_track(target_track, &new_snip);

      ts->selected_snippet_id = snippet_id;
      ts->selected_player_track_index = target_track_idx;

      free_snippet_inputs(&snippet_to_move);
    } else {
      return false;
    }
  }

  return true;
}

int get_max_timeline_tick(timeline_state_t *ts) {
  int max_tick = 0;
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      if (track->snippets[j].end_tick > max_tick) {
        max_tick = track->snippets[j].end_tick;
      }
    }
  }
  return max_tick;
}

// --- Rendering and Interaction Functions ---
void render_timeline_controls(timeline_state_t *ts) {
  igPushItemWidth(100);

  if (igDragInt("Current Tick", &ts->current_tick, 1, 0, 100000, "%d", ImGuiSliderFlags_None)) {
    if (ts->current_tick < 0)
      ts->current_tick = 0;
  }

  if ((igShortcut_Nil(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal) ||
       igShortcut_Nil(ImGuiKey_MouseX1, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) &&
      ts->current_tick > 0) {
    ts->is_playing = false;
    advance_tick(ts, -1);
  }
  if (igShortcut_Nil(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal) ||
      igShortcut_Nil(ImGuiKey_MouseX2, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) {
    ts->is_playing = false;
    advance_tick(ts, 1);
  }

  if (igShortcut_Nil(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) {
    ts->playback_speed = imax(--ts->playback_speed, 1);
  }
  if (igShortcut_Nil(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) {
    ++ts->playback_speed;
  }

  igSameLine(0, 8);
  if (igButton("|<", (ImVec2){30, 0}))
    ts->current_tick = 0;
  igSameLine(0, 4);
  if (igArrowButton("<<", ImGuiDir_Left))
    ts->current_tick = (ts->current_tick >= 50) ? ts->current_tick - 50 : 0;
  igSameLine(0, 4);
  if (igButton(ts->is_playing ? "Pause" : "Play", (ImVec2){50, 0})) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime();
    }
  }
  igSameLine(0, 4);
  if (igArrowButton(">>", ImGuiDir_Right))
    ts->current_tick += 50;
  igSameLine(0, 4);
  if (igButton(">|", (ImVec2){30, 0})) {
    ts->current_tick = get_max_timeline_tick(ts);
  }

  igSameLine(0, 20);
  igText("Zoom:");
  igSameLine(0, 4);
  igSetNextItemWidth(150);
  ImVec2 window_pos;
  igGetWindowPos(&window_pos);
  ImVec2 window_size;
  igGetWindowSize(&window_size);
  if (igSliderFloat("##Zoom", &ts->zoom, MIN_TIMELINE_ZOOM, MAX_TIMELINE_ZOOM, "%.2f",
                    ImGuiSliderFlags_Logarithmic)) {
    ts->zoom = fmaxf(MIN_TIMELINE_ZOOM, fminf(MAX_TIMELINE_ZOOM, ts->zoom)); // Clamp
    // Adjust view_start_tick to zoom towards the mouse position if timeline area is hovered
    // This logic is also in handle_timeline_interaction, could potentially be unified or this is removed.
    // For simplicity here, just clamp the view start tick after zoom
    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  }
  igSameLine(0, 20);
  igText("Playback Speed:");
  igSameLine(0, 4);
  igSetNextItemWidth(150);
  igSliderInt("##Speed", &ts->playback_speed, 1, 100, "%d", ImGuiSliderFlags_None);

  igSameLine(0, 20);
  bool was_recording = ts->recording;
  if (igButton(ts->recording ? "Stop Recording" : "Record", (ImVec2){0, 0})) {
    bool is_in_way = false;
    if (ts->selected_player_track_index >= 0) {
      const player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
      for (int i = 0; i < track->snippet_count; ++i) {
        const input_snippet_t *snippet = &track->snippets[i];
        if (ts->current_tick < snippet->end_tick && ts->current_tick >= snippet->start_tick)
          is_in_way = true;
      }
      if (!is_in_way) {
        ts->recording = !ts->recording;
        if (!ts->recording)
          ts->recording_snippet = NULL;
      }
    }
  }
  if (!was_recording && ts->recording) {
    input_snippet_t new_snip = create_empty_snippet(ts, ts->current_tick, 1);
    add_snippet_to_track(&ts->player_tracks[ts->selected_player_track_index], &new_snip);
    ts->recording_snippet =
        &ts->player_tracks[ts->selected_player_track_index]
             .snippets[ts->player_tracks[ts->selected_player_track_index].snippet_count - 1];
  }
  if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
    ts->recording = false;
    ts->recording_snippet = NULL;
  }

  if (ts->recording) {
    igSameLine(0, 10);
    igTextColored((ImVec4){1.0f, 0.2f, 0.2f, 1.0f}, "REC");
  }

  igPopItemWidth();
}

void handle_timeline_interaction(timeline_state_t *ts, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  ImVec2 mouse_pos = io->MousePos;

  if (io->ConfigFlags & ImGuiConfigFlags_NoMouse)
    return;

  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  // Zoom with mouse wheel
  if (is_timeline_hovered && io->MouseWheel != 0) {
    int mouse_tick_before_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    float zoom_delta = io->MouseWheel * 0.1f * ts->zoom; // Scale zoom delta by current zoom
    ts->zoom = fmaxf(MIN_TIMELINE_ZOOM, fminf(MAX_TIMELINE_ZOOM, ts->zoom + zoom_delta));

    // Adjust view start tick to keep the tick under the mouse cursor stable
    int mouse_tick_after_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    ts->view_start_tick += (mouse_tick_before_zoom - mouse_tick_after_zoom);

    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  }

  // Pan with middle mouse button drag
  if (is_timeline_hovered && igIsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    ImVec2 drag_delta;
    igGetMouseDragDelta(&drag_delta, ImGuiMouseButton_Middle, 0.0f);
    igResetMouseDragDelta(ImGuiMouseButton_Middle);
    // Convert pixel delta to tick delta, scaled by zoom
    int tick_delta = (int)(-drag_delta.x / ts->zoom);
    ts->view_start_tick += tick_delta;
    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  }
}

// Helper: Pick nice tick step that gives enough pixel spacing
static double choose_nice_tick_step(double pixels_per_tick, double min_label_spacing) {
  // steps expressed in TICKS (since 50 ticks = 1 sec)
  static const double nice_steps[] = {
      1,   2,   5,                                         // very detailed (sub-second ticks)
      10,  25,  50,                                        // half-sec, multiple ticks
      100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000 // minutes range (if zoomed way out)
  };

  int count = sizeof(nice_steps) / sizeof(nice_steps[0]);
  for (int i = 0; i < count; i++) {
    double step = nice_steps[i];
    if (step * pixels_per_tick >= min_label_spacing) {
      return step;
    }
  }
  return nice_steps[count - 1];
}

void draw_timeline_header(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_bb, float header_y) {
  ImU32 tick_minor_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.25f); // light/faint line every tick
  ImU32 tick_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.7f);        // adaptive tick
  ImU32 tick_major_col = igGetColorU32_Col(ImGuiCol_Text, 0.9f);          // 1-second or bigger
  ImU32 tick_text_col = igGetColorU32_Col(ImGuiCol_Text, 1.0f);

  float header_height = igGetTextLineHeightWithSpacing();
  float timeline_area_width = timeline_bb.Max.x - timeline_bb.Min.x;
  float pixels_per_tick = ts->zoom;
  float min_label_spacing = 60.0f;

  // adaptive step in ticks
  double tick_step = choose_nice_tick_step(pixels_per_tick, min_label_spacing);

  int max_visible_ticks = (int)(timeline_area_width / pixels_per_tick) + 2;
  int start_tick = (int)floor(ts->view_start_tick);
  int end_tick = ts->view_start_tick + max_visible_ticks;

  // pass 1: draw EVERY TICK as faint grey
  for (int tick = start_tick; tick <= end_tick; tick++) {
    if (tick < 0)
      continue;

    float x = tick_to_screen_x(ts, tick, timeline_bb.Min.x);
    if (x < timeline_bb.Min.x - 10 || x > timeline_bb.Max.x + 10)
      continue;

    float line_height = header_height * 0.25f;
    ImDrawList_AddLine(draw_list, (ImVec2){x, header_y + header_height - line_height},
                       (ImVec2){x, header_y + header_height}, tick_minor_col, 1.0f);
  }

  // pass 2: draw adaptive major ticks & labels
  double start_tick_major = floor((double)ts->view_start_tick / tick_step) * tick_step;
  for (double tick = start_tick_major; tick <= end_tick; tick += tick_step) {
    if (tick < 0)
      continue;

    float x = tick_to_screen_x(ts, tick, timeline_bb.Min.x);
    if (x < timeline_bb.Min.x - 10 || x > timeline_bb.Max.x + 10)
      continue;

    bool is_sec_marker = fmod(tick, 50.0) < 1e-6; // 50 ticks = full second
    ImU32 col = is_sec_marker ? tick_major_col : tick_col;
    float line_height = is_sec_marker ? header_height * 0.6f : header_height * 0.4f;

    ImDrawList_AddLine(draw_list, (ImVec2){x, header_y + header_height - line_height},
                       (ImVec2){x, header_y + header_height}, col, 1.0f);

    // labels only for adaptive major ticks
    char label[64];
    if (tick < 50) { // show raw ticks in fine zoom
      snprintf(label, sizeof(label), "%.0f", tick);
    } else if (tick < 3000) { // under a minute, show sec
      snprintf(label, sizeof(label), "%.1fs", tick / 50.0);
    } else if (tick < 180000) { // under an hour
      int total_secs = (int)(tick / 50.0);
      int mins = total_secs / 60;
      int secs = total_secs % 60;
      snprintf(label, sizeof(label), "%d:%02d", mins, secs);
    } else {
      int total_secs = (int)(tick / 50.0);
      int hours = total_secs / 3600;
      int mins = (total_secs % 3600) / 60;
      snprintf(label, sizeof(label), "%dh%02dm", hours, mins);
    }

    ImVec2 text_size;
    igCalcTextSize(&text_size, label, NULL, false, 0);
    // draw BELOW the ticks (avoids collision with vertical lines)
    ImVec2 text_pos = {x - text_size.x * 0.5f, header_y + header_height + 2};

    // clamp horizontally within the timeline bounds
    if (text_pos.x < timeline_bb.Min.x + 2)
      text_pos.x = timeline_bb.Min.x + 2;
    if (text_pos.x + text_size.x > timeline_bb.Max.x - 2)
      text_pos.x = timeline_bb.Max.x - text_size.x - 2;

    ImDrawList_AddText_Vec2(draw_list, text_pos, tick_text_col, label, NULL);
  }
}

void render_input_snippet(timeline_state_t *ts, int track_index, int snippet_index, input_snippet_t *snippet,
                          ImDrawList *draw_list, float track_top, float track_bottom, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();

  float snippet_start_x = tick_to_screen_x(ts, snippet->start_tick, timeline_bb.Min.x);
  float snippet_end_x = tick_to_screen_x(ts, snippet->end_tick, timeline_bb.Min.x);

  // Clamp snippet drawing to visible timeline area
  float draw_start_x = fmaxf(snippet_start_x, timeline_bb.Min.x);
  float draw_end_x = fminf(snippet_end_x, timeline_bb.Max.x);

  // Don't draw if snippet is completely outside the visible area
  if (draw_start_x >= draw_end_x)
    return;

  ImVec2 snippet_min = {draw_start_x, track_top + 2.0f};
  ImVec2 snippet_max = {draw_end_x, track_bottom - 2.0f};

  if (track_bottom - track_top - 4.0f <= 0)
    return;

  // --- Snippet Interaction ---
  igPushID_Int(snippet->id); // Use snippet ID for unique ImGui ID
  // We use the actual bounds for the invisible button, even if drawing is clamped
  igSetCursorScreenPos((ImVec2){snippet_start_x, track_top + 2.0f});
  bool is_item_hovered =
      igInvisibleButton("snippet", (ImVec2){snippet_end_x - snippet_start_x, track_bottom - track_top - 4.0f},
                        ImGuiButtonFlags_MouseButtonLeft);
  bool is_item_active = igIsItemActive(); // True while button is held or being dragged
  bool is_item_clicked = igIsItemClicked(ImGuiMouseButton_Left);

  // Handle selection
  if (is_item_clicked && !ts->drag_state.active) { // Only select if not already dragging
    ts->selected_snippet_id = snippet->id;
    ts->selected_player_track_index = track_index;
  }

  // Initiate drag
  if (is_item_active && igIsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !ts->drag_state.active) {
    ts->drag_state.active = true;
    ts->drag_state.source_track_index = track_index;
    ts->drag_state.source_snippet_index =
        snippet_index; // Store index for potential later use if needed, though ID is safer for lookup
    ts->drag_state.initial_mouse_pos = io->MousePos;
    // Calculate the offset in ticks from the snippet's start to the mouse click point
    int mouse_tick_at_click = screen_x_to_tick(ts, ts->drag_state.initial_mouse_pos.x, timeline_bb.Min.x);
    ts->drag_state.drag_offset_ticks = mouse_tick_at_click - snippet->start_tick;

    ts->drag_state.dragged_snippet_id = snippet->id; // Set the ID of the snippet being dragged!

    // Clear selection if a different snippet was selected
    if (ts->selected_snippet_id != snippet->id) {
      ts->selected_snippet_id = -1;
      ts->selected_player_track_index = -1;
    }
  }

  // --- Draw Snippet ---
  bool is_selected = (snippet->id == ts->selected_snippet_id);
  // Use is_item_hovered for visual hover feedback, not the function parameter which might be clipped
  ImU32 snippet_col =
      is_selected
          ? igGetColorU32_Col(ImGuiCol_HeaderActive, 1.0f)
          : (is_item_hovered ? igGetColorU32_Col(ImGuiCol_ButtonHovered, 1.0f)
                             : igGetColorU32_Col(ImGuiCol_Button, 0.8f)); // Use Button for default color

  ImU32 snippet_border_col = is_selected ? igGetColorU32_Col(ImGuiCol_NavWindowingHighlight, 1.0f)
                                         : igGetColorU32_Col(ImGuiCol_Border, 0.6f);
  float border_thickness = is_selected ? 2.0f : 1.0f;

  ImDrawList_AddRectFilled(draw_list, snippet_min, snippet_max, snippet_col, 4.0f,
                           ImDrawFlags_RoundCornersAll);
  ImDrawList_AddRect(draw_list, snippet_min, snippet_max, snippet_border_col, 4.0f,
                     ImDrawFlags_RoundCornersAll, border_thickness);

  char label[64];
  snprintf(label, sizeof(label), "ID: %d", snippet->id);
  ImVec2 text_size;
  igCalcTextSize(&text_size, label, NULL, false, 0);
  // Calculate text position, centered vertically, slightly offset horizontally
  ImVec2 text_pos = {(snippet_min.x + snippet_max.x) * 0.5f - text_size.x * 0.5f,
                     (snippet_min.y + snippet_max.y) * 0.5f - text_size.y * 0.5f};

  // Only draw text if snippet is wide enough and text is within visible bounds
  if (snippet_max.x - snippet_min.x > text_size.x + 5.0f) {
    ImDrawList_AddText_Vec2(draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 1.0f), label, NULL);
  }

  // Tooltip on hover
  if (is_item_hovered) {
    igSetTooltip("Snippet ID: %d\nStart: %d End: %d", snippet->id, snippet->start_tick, snippet->end_tick);
  }

  igPopID();
}

void render_player_track(timeline_state_t *ts, int track_index, player_track_t *track, ImDrawList *draw_list,
                         ImRect timeline_bb, float track_top, float track_bottom) {
  // Draw track background
  ImU32 track_bg_col = (track_index % 2 == 0) ? igGetColorU32_Col(ImGuiCol_TitleBg, 1.0f)
                                              : igGetColorU32_Col(ImGuiCol_WindowBg, 1.0f);
  // Add some alpha to see grid/ticks underneath
  track_bg_col = igGetColorU32_U32(track_bg_col, 0.95f);

  ImDrawList_AddRectFilled(draw_list, (ImVec2){timeline_bb.Min.x, track_top},
                           (ImVec2){timeline_bb.Max.x, track_bottom}, track_bg_col, 0.0f,
                           ImDrawFlags_None); // No rounding here

  // Draw track border/separator
  ImDrawList_AddLine(draw_list, (ImVec2){timeline_bb.Min.x, track_bottom},
                     (ImVec2){timeline_bb.Max.x, track_bottom}, igGetColorU32_Col(ImGuiCol_Border, 0.3f),
                     1.0f);

  // Draw Snippets for this track
  for (int j = 0; j < track->snippet_count; ++j) {
    render_input_snippet(ts, track_index, j, &track->snippets[j], draw_list, track_top, track_bottom,
                         timeline_bb);
  }

  // Track label
  // Add text offset from the left edge, vertically centered
  char track_label[64];
  snprintf(track_label, sizeof(track_label), "Track %d", track_index + 1);
  ImVec2 text_size;
  igCalcTextSize(&text_size, track_label, NULL, false, 0);
  ImVec2 text_pos = {timeline_bb.Min.x + 10.0f, track_top + (ts->track_height - text_size.y) * 0.5f};
  ImDrawList_AddText_Vec2(draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 0.7f), track_label, NULL);

  ImGuiIO *io = igGetIO_Nil();
  if (io->ConfigFlags & ImGuiConfigFlags_NoMouse) {
    return;
  }

  if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, 0) &&
      igIsMouseHoveringRect((ImVec2){timeline_bb.Min.x, track_top}, (ImVec2){timeline_bb.Max.x, track_bottom},
                            true)) {

    ImVec2 mouse_pos = igGetIO_Nil()->MousePos;

    igOpenPopup_Str("RightClickMenu", 0);
    ts->selected_player_track_index = track_index;
  }

  if (igBeginPopup("RightClickMenu", ImGuiPopupFlags_AnyPopupLevel)) {
    if (igMenuItem_Bool("Add Snippet (Ctrl+a)", NULL, false, true) ||
        igShortcut_Nil(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteGlobal)) {
      int track_idx = ts->selected_player_track_index;
      input_snippet_t snip = create_empty_snippet(ts, ts->current_tick, 50);
      add_snippet_to_track(&ts->player_tracks[track_idx], &snip);
    }

    if (igMenuItem_Bool("Split Snippet (Ctrl+r)", NULL, false, true) ||
        igShortcut_Nil(ImGuiMod_Ctrl | ImGuiKey_R, ImGuiInputFlags_RouteGlobal)) {
      if (ts->selected_player_track_index >= 0 && ts->selected_snippet_id >= 0) {
        player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
        input_snippet_t *snippet = find_snippet_by_id(track, ts->selected_snippet_id);
        if (snippet && ts->current_tick > snippet->start_tick && ts->current_tick < snippet->end_tick) {
          int old_end = snippet->end_tick;
          int split_tick = ts->current_tick;

          // Create right-hand snippet
          input_snippet_t right = create_empty_snippet(ts, split_tick, old_end - split_tick);

          // Copy the inputs after the split point
          int offset = split_tick - snippet->start_tick;
          if (offset < snippet->input_count) {
            int right_count = snippet->input_count - offset;
            right.inputs = malloc(right_count * sizeof(SPlayerInput));
            memcpy(right.inputs, snippet->inputs + offset, right_count * sizeof(SPlayerInput));
            right.input_count = right_count;
          }

          // Adjust original (left-hand) snippet
          resize_snippet_inputs(snippet, offset);
          snippet->end_tick = split_tick;

          // Insert the right-hand snippet into the track
          add_snippet_to_track(track, &right);
        }
      }
    }

    if (igMenuItem_Bool("Delete Snippet (Ctrl+d)", NULL, false, true) ||
        igShortcut_Nil(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) {
      if (ts->selected_player_track_index >= 0 && ts->selected_snippet_id >= 0) {
        player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
        if (remove_snippet_from_track(track, ts->selected_snippet_id)) {
          ts->selected_snippet_id = -1;
        }
      }
    }
    igEndPopup();
  }
}

void draw_playhead(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_bb, float playhead_start_y) {
  float playhead_x = tick_to_screen_x(ts, ts->current_tick, timeline_bb.Min.x);

  // Draw playhead only if it's within the horizontal bounds of the timeline area
  if (playhead_x >= timeline_bb.Min.x && playhead_x <= timeline_bb.Max.x) {
    // Draw a vertical line from the calculated start Y down to the bottom of the timeline area
    ImDrawList_AddLine(draw_list, (ImVec2){playhead_x, playhead_start_y}, // Use the passed start Y
                       (ImVec2){playhead_x, timeline_bb.Max.y},           // End Y is bottom of timeline area
                       igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f), 2.0f);

    // Position the triangle at the playhead_start_y
    ImVec2 head_center = {playhead_x + 0.5, playhead_start_y};
    ImDrawList_AddTriangleFilled(draw_list, (ImVec2){head_center.x - 5, head_center.y},
                                 (ImVec2){head_center.x + 5, head_center.y},
                                 (ImVec2){head_center.x, head_center.y + 8}, // Pointing down
                                 igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f));
  }
}

void draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();

  if (!ts->drag_state.active)
    return;

  input_snippet_t *dragged_snippet = NULL;
  // Find the snippet being dragged using its ID from the drag state
  for (int i = 0; i < ts->player_track_count; ++i) {
    dragged_snippet = find_snippet_by_id(&ts->player_tracks[i], ts->drag_state.dragged_snippet_id);
    if (dragged_snippet)
      break;
  }
  if (!dragged_snippet)
    return; // Should not happen if drag_state is valid

  int duration = dragged_snippet->end_tick - dragged_snippet->start_tick; // Calculate duration

  // Calculate the desired start tick based on mouse position and original offset
  int mouse_tick = screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  int desired_start_tick = mouse_tick - ts->drag_state.drag_offset_ticks;

  // Calculate the snapped start tick - PASS DURATION
  int snapped_start_tick = calculate_snapped_tick(ts, desired_start_tick, duration,
                                                  dragged_snippet->id); // Pass duration and exclude ID
  int snapped_end_tick = snapped_start_tick + duration;

  // Ensure the snapped position is not before tick 0
  if (snapped_start_tick < 0) {
    snapped_start_tick = 0;
    snapped_end_tick = snapped_start_tick + duration;
  }

  // Determine the potential target track based on mouse Y position
  // ... (calculate potential_target_track_idx) ...
  float track_y = io->MousePos.y - timeline_bb.Min.y;
  int potential_target_track_idx = (int)(track_y / ts->track_height);
  if (potential_target_track_idx < 0)
    potential_target_track_idx = 0;
  if (potential_target_track_idx >= ts->player_track_count)
    potential_target_track_idx = ts->player_track_count - 1;

  // Calculate screen position for the preview rectangle on the potential target track
  // ... (calculate preview_min, preview_max) ...
  float preview_start_x = tick_to_screen_x(ts, snapped_start_tick, timeline_bb.Min.x);
  float preview_end_x = tick_to_screen_x(ts, snapped_end_tick, timeline_bb.Min.x);
  float preview_track_top = timeline_bb.Min.y + potential_target_track_idx * ts->track_height;
  float preview_track_bottom = preview_track_top + ts->track_height;
  ImVec2 preview_min = {preview_start_x, preview_track_top + 2.0f};
  ImVec2 preview_max = {preview_end_x, preview_track_bottom - 2.0f};

  // Check if the preview position overlaps in the potential target track
  bool overlaps = check_for_overlap(&ts->player_tracks[potential_target_track_idx], snapped_start_tick,
                                    snapped_end_tick, dragged_snippet->id);

  // ... (draw preview rectangle based on overlaps) ...
  ImU32 preview_col =
      overlaps ? igGetColorU32_Col(ImGuiCol_PlotLinesHovered, 0.5f) // Use a different color for invalid drop
               : igGetColorU32_Col(ImGuiCol_DragDropTarget, 0.6f);  // Standard drag drop color
  ImDrawList_AddRectFilled(overlay_draw_list, preview_min, preview_max, preview_col, 4.0f,
                           ImDrawFlags_RoundCornersAll);
  ImDrawList_AddRect(overlay_draw_list, preview_min, preview_max,
                     igGetColorU32_Col(ImGuiCol_NavWindowingHighlight, 0.8f), 4.0f,
                     ImDrawFlags_RoundCornersAll, 1.5f);

  // ... (draw text on preview) ...
  char label[64];
  snprintf(label, sizeof(label), "ID: %d", dragged_snippet->id);
  ImVec2 text_size;
  igCalcTextSize(&text_size, label, NULL, false, 0);
  ImVec2 text_pos = {(preview_min.x + preview_max.x) * 0.5f - text_size.x * 0.5f,
                     (preview_min.y + preview_max.y) * 0.5f - text_size.y * 0.5f};
  if (preview_max.x - preview_min.x > text_size.x + 5.0f) {
    ImDrawList_AddText_Vec2(overlay_draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 1.0f), label, NULL);
  }

} // End draw_drag_preview

// --- Main Render Function ---
void render_timeline(timeline_state_t *ts) {
  ImGuiIO *io = igGetIO_Nil();

  // Advance timeline state
  if (igIsKeyPressed_Bool(ImGuiKey_C, false)) {
    ts->is_playing = 0;
    ts->last_update_time = igGetTime();
  }

  if (igIsKeyPressed_Bool(ImGuiKey_X, ImGuiInputFlags_Repeat)) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime();
    }
  }

  bool reverse = igIsKeyDown_Nil(ImGuiKey_C);
  if ((ts->is_playing || reverse) && ts->playback_speed > 0.0f) {
    double current_time = igGetTime();
    double elapsed_time = current_time - ts->last_update_time;
    double tick_interval = 1.0 / (double)ts->playback_speed; // Time per tick in seconds

    // Accumulate elapsed time and advance ticks as needed
    while (elapsed_time >= tick_interval) {
      advance_tick(ts, reverse ? -1 : 1);
      elapsed_time -= tick_interval;
      ts->last_update_time = current_time - elapsed_time;
    }
  }

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 4.0f);

  if (igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());
    igPopStyleVar(2);

    // --- Controls ---
    render_timeline_controls(ts);

    // --- Layout Calculations for Header and Timeline Area ---
    float header_height = igGetTextLineHeightWithSpacing() + 12;

    // Calculate the available space below the controls for the header and tracks
    ImVec2 available_space_below_controls;
    igGetContentRegionAvail(&available_space_below_controls);

    // Define the bounding box for the Header area (vertical space where ticks/labels go)
    ImVec2 header_bb_min;
    igGetCursorScreenPos(&header_bb_min); // Cursor after controls
    ImVec2 header_bb_max = {header_bb_min.x + available_space_below_controls.x,
                            header_bb_min.y + header_height};
    ImRect header_bb = {header_bb_min, header_bb_max};

    // --- Handle Mouse Interaction on Header ---
    bool is_header_hovered = igIsMouseHoveringRect(header_bb.Min, header_bb.Max, true);

    // Start header drag: If header is hovered AND left mouse button is initially clicked
    if (is_header_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, 0)) {
      ts->is_header_dragging = true;
      int mouse_tick = screen_x_to_tick(ts, io->MousePos.x, header_bb.Min.x);
      ts->current_tick = fmax(0, mouse_tick); // Clamp
    }

    // Handle header drag: If header dragging is active AND left mouse button is held down
    // This continues the drag even if the mouse leaves the header area
    if (ts->is_header_dragging && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      // Get the current mouse X position
      float mouse_screen_x = io->MousePos.x;

      // Convert the mouse X position to a timeline tick, using the header's left edge as the origin
      int mouse_tick = screen_x_to_tick(ts, mouse_screen_x, header_bb.Min.x);

      // Update the current tick
      ts->current_tick = mouse_tick;

      // Clamp current tick to be non-negative
      if (ts->current_tick < 0) {
        ts->current_tick = 0;
      }
    }

    // End header drag: If header dragging is active AND left mouse button is released
    if (ts->is_header_dragging && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
      ts->is_header_dragging = false;
    }

    // --- Draw Header (Ticks) ---
    // Draw the ticks and labels within the header area
    draw_timeline_header(ts, draw_list, header_bb, header_bb_min.y);

    // Move the cursor down by the height of the header to position for the tracks
    igDummy((ImVec2){available_space_below_controls.x, header_height});

    // Calculate the bounding box for the Timeline Tracks area (below the header)
    ImVec2 timeline_start_pos;
    igGetCursorScreenPos(&timeline_start_pos); // Cursor after header
    ImVec2 available_space_for_tracks;
    igGetContentRegionAvail(&available_space_for_tracks);
    // Reserve space for the scrollbar
    float scrollbar_height = igGetStyle()->ScrollbarSize;
    available_space_for_tracks.y -= scrollbar_height;

    ImVec2 timeline_end_pos = (ImVec2){timeline_start_pos.x + available_space_for_tracks.x,
                                       timeline_start_pos.y + available_space_for_tracks.y};
    ImRect timeline_bb = (ImRect){timeline_start_pos, timeline_end_pos};

    // Ensure timeline_bb has positive dimensions
    if (timeline_bb.Max.x > timeline_bb.Min.x && timeline_bb.Max.y > timeline_bb.Min.y) {

      // --- Handle Mouse Interaction for Pan/Zoom on Timeline Area (Tracks) ---
      // This interaction should apply to the track area, not the header.
      // Make sure this doesn't conflict with header dragging if both are active
      if (!ts->is_header_dragging) { // Only pan/zoom tracks if not currently dragging header
        handle_timeline_interaction(ts, timeline_bb);
      }

      // --- Draw Timeline Tracks and Snippets ---
      // clip rect and track drawing loop using timeline_bb
      igPushClipRect(timeline_bb.Min, timeline_bb.Max, true);
      float current_track_y = timeline_bb.Min.y;
      for (int i = 0; i < ts->player_track_count; ++i) {
        if (current_track_y >= timeline_bb.Max.y)
          break;
        player_track_t *track = &ts->player_tracks[i];
        float track_top = current_track_y;
        float track_bottom = current_track_y + ts->track_height;
        float clamped_track_bottom = fminf(track_bottom, timeline_bb.Max.y);
        if (clamped_track_bottom > track_top) {
          igPushID_Int(i);
          render_player_track(ts, i, track, draw_list, timeline_bb, track_top, clamped_track_bottom);
          igPopID();
        }
        current_track_y += ts->track_height;
      }
      igPopClipRect();

      // --- Draw Scrollbar ---
      ImS64 max_tick = get_max_timeline_tick(ts);

      // Calculate visible ticks based on window width
      float timeline_width = timeline_bb.Max.x - timeline_bb.Min.x;
      ImS64 visible_ticks = (ImS64)(timeline_width / ts->zoom);

      // Adjust view during playback to follow current_tick
      if (ts->is_playing) {
        // Define the visible range
        ImS64 view_end_tick = (ImS64)ts->view_start_tick + visible_ticks;
        // Check if current_tick is outside the visible range
        if ((ImS64)ts->current_tick < (ImS64)ts->view_start_tick || (ImS64)ts->current_tick > view_end_tick) {
          // Center current_tick in the visible range
          ts->view_start_tick = ts->current_tick - (int)(visible_ticks);
        }
      }

      // Add padding (40% of max_tick) and ensure minimum duration
      max_tick = (ImS64)(max_tick * 1.4f);
      if (max_tick < 100)
        max_tick = 100;

      // Render scrollbar
      ImRect scrollbar_bb = {{timeline_bb.Min.x, timeline_bb.Max.y},
                             {timeline_bb.Max.x, timeline_bb.Max.y + scrollbar_height}};
      igPushID_Str("TimelineScrollbar");
      ImS64 scroll_v = (ImS64)ts->view_start_tick;
      if (igScrollbarEx(scrollbar_bb, igGetID_Str("TimelineScrollbar"), ImGuiAxis_X, &scroll_v, visible_ticks,
                        max_tick, ImDrawFlags_RoundCornersBottom)) {
        ts->view_start_tick = (int)scroll_v;
      }
      if (ts->view_start_tick > max_tick - visible_ticks)
        ts->view_start_tick = (int)(max_tick - visible_ticks);
      if (ts->view_start_tick < 0)
        ts->view_start_tick = 0;
      igPopID();

      // --- Handle Mouse Release -> Commit Drag/Drop (for SNIPPETS) ---
      // This only happens if a snippet drag was started and header is not being dragged
      if (ts->drag_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left) && !ts->is_header_dragging) {
        ImVec2 mouse_pos = igGetIO_Nil()->MousePos;

        // Get the snippet data needed for duration and ID
        player_track_t *source_track =
            &ts->player_tracks[ts->drag_state.source_track_index]; // Get source track using the index stored
                                                                   // at drag start Find the snippet by ID in
                                                                   // the source track to get its duration and
                                                                   // confirm it still exists there
        input_snippet_t *snippet_to_move =
            find_snippet_by_id(source_track, ts->drag_state.dragged_snippet_id);
        if (!snippet_to_move) {
          // Snippet not found in the source track it supposedly started in? This indicates a data
          // inconsistency. Clear drag state and abort.
          printf("Error: Dragged snippet ID %d not found in expected source track %d on mouse release!\n",
                 ts->drag_state.dragged_snippet_id, ts->drag_state.source_track_index);
          ts->drag_state.active = false; // Clear invalid drag state
          ts->drag_state.source_track_index = -1;
          ts->drag_state.source_snippet_index = -1;
          ts->drag_state.dragged_snippet_id = -1;
          return; // Exit handler
        }
        int duration = snippet_to_move->end_tick - snippet_to_move->start_tick; // Calculate duration

        // Calculate the potential drop tick based on mouse position and original offset
        int mouse_tick_at_release = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
        int desired_drop_tick = mouse_tick_at_release - ts->drag_state.drag_offset_ticks;

        // Apply snapping to the drop tick - PASS DURATION
        int final_drop_tick =
            calculate_snapped_tick(ts, desired_drop_tick, duration,
                                   ts->drag_state.dragged_snippet_id); // Pass duration and exclude ID

        // Determine the target track index based on mouse Y position
        float track_y_at_release_in_area = mouse_pos.y - timeline_bb.Min.y;
        int target_track_idx = (int)(track_y_at_release_in_area / ts->track_height);
        // Clamp target track index to valid range
        if (target_track_idx < 0)
          target_track_idx = 0;
        if (target_track_idx >= ts->player_track_count)
          target_track_idx = ts->player_track_count - 1;

        // Perform the move if valid
        int snippet_id_to_move = ts->drag_state.dragged_snippet_id; // Get ID from state
        int source_track_idx = ts->drag_state.source_track_index;   // Get source track index from state

        try_move_snippet(ts, snippet_id_to_move, source_track_idx, target_track_idx, final_drop_tick);

        // Clear drag state regardless of success
        ts->drag_state.active = false;
        ts->drag_state.source_track_index = -1;
        ts->drag_state.source_snippet_index = -1; // This index is now likely invalid
        ts->drag_state.dragged_snippet_id = -1;
        // drag_offset_ticks and initial_mouse_pos are irrelevant after release
      } // End if snippet drag active and mouse released

      // --- Draw Playhead ---
      // Draw the playhead line over the track area, from its top to its bottom
      draw_playhead(ts, draw_list, timeline_bb,
                    timeline_bb.Min.y - 12); // Playhead starts at the top Y of the timeline_bb area

    } // End if(timeline_bb has positive dimensions)

    // --- Draw Drag Preview (on overlay) ---
    // This uses the overlay draw list and needs the timeline_bb for positioning
    draw_drag_preview(ts, overlay_draw_list, timeline_bb);
  }
  igEnd(); // End Timeline window
}

// Helper to add a new empty track(s)
player_track_t *add_new_track(timeline_state_t *ts, ph_t *ph, int num) {
    if (num <= 0) {
        return NULL; // pointless to add 0 tracks
    }

    int old_num_chars = 0;
    if (ph) {
        old_num_chars = ph->world.m_NumCharacters;

        // Call bulk character spawner
        SCharacterCore *pFirstChar = wc_add_character(&ph->world, num);
        if (!pFirstChar) {
            return NULL; // failed spawning
        }

        // Double check world count grew
        if (ph->world.m_NumCharacters <= old_num_chars) {
            return NULL;
        }
    }

    // Allocate space for num new tracks in timeline
    int old_count = ts->player_track_count;
    int new_count = old_count + num;

    ts->player_tracks = realloc(ts->player_tracks,
                                sizeof(player_track_t) * new_count);

    // Initialize each new track slot
    for (int i = 0; i < num; i++) {
        player_track_t *new_track = &ts->player_tracks[old_count + i];
        new_track->snippets = NULL; 
        new_track->snippet_count = 0;

        // Reset player info
        memset(new_track->player_info.name, 0, sizeof(new_track->player_info.name));
        memset(new_track->player_info.clan, 0, sizeof(new_track->player_info.clan));
        new_track->player_info.skin = 0;
        memset(new_track->player_info.color, 0, 3 * sizeof(float));
        new_track->player_info.use_custom_color = 0;
    }

    ts->player_track_count = new_count;

    // Update timeline copies with the new world 
    wc_free(&ts->vec.data[0]);
    ts->vec.data[0] = wc_empty();
    wc_copy_world(&ts->vec.data[0], &ph->world);

    wc_free(&ts->previous_world);
    ts->previous_world = wc_empty();
    wc_copy_world(&ts->previous_world, &ph->world);

    // Return a pointer to the *first* new track (similar to wc_add_character behavior)
    return &ts->player_tracks[old_count];
}

void timeline_init(timeline_state_t *ts) {
  timeline_cleanup(ts);
  memset(ts, 0, sizeof(timeline_state_t));

  v_init(&ts->vec);
  ts->previous_world = wc_empty();

  // Initialize Timeline State variables
  ts->playback_speed = 50;
  ts->is_playing = 0;
  ts->current_tick = 0;
  ts->view_start_tick = 0;
  ts->zoom = 1.0f; // 1 pixel per tick initially
  ts->track_height = DEFAULT_TRACK_HEIGHT;
  ts->selected_player_track_index = -1; // Nothing selected initially
  ts->selected_snippet_id = -1;
  ts->last_update_time = 0.f; // Initialize for playback timing

  // Initialize Drag State
  ts->drag_state.active = false;
  ts->drag_state.source_track_index = -1;
  ts->drag_state.source_snippet_index = -1;
  ts->drag_state.dragged_snippet_id = -1;
  ts->drag_state.drag_offset_ticks = 0;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  // Initialize unique snippet ID counter
  ts->next_snippet_id = 1; // Start IDs from 1

  // Initialize Players/Tracks
  ts->player_track_count = 0;
  ts->player_tracks = NULL;
}

void timeline_cleanup(timeline_state_t *ts) {
  // free each track's snippets and reset track data
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    if (track->snippets) {
      for (int j = 0; j < track->snippet_count; ++j) {
        free_snippet_inputs(&track->snippets[j]);
      }
      free(track->snippets);
      track->snippets = NULL;
    }
    track->snippet_count = 0;
  }

  // free the tracks array
  if (ts->player_tracks) {
    free(ts->player_tracks);
    ts->player_tracks = NULL;
  }
  ts->player_track_count = 0;

  // reset timeline state
  ts->current_tick = 0;
  ts->zoom = 1.0f;
  ts->view_start_tick = 0;
  ts->track_height = 0.0f;
  ts->selected_snippet_id = -1;
  ts->selected_player_track_index = -1;
  ts->next_snippet_id = 1;
  ts->is_header_dragging = false;
  ts->is_playing = false;
  ts->playback_speed = 0;
  ts->last_update_time = 0.0;

  // reset drag state
  ts->drag_state.active = false;
  ts->drag_state.source_track_index = -1;
  ts->drag_state.source_snippet_index = -1;
  ts->drag_state.dragged_snippet_id = -1;
  ts->drag_state.drag_offset_ticks = 0;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};
}

input_snippet_t create_empty_snippet(timeline_state_t *ts, int start_tick, int duration) {
  if (duration <= 0)
    duration = 1;
  input_snippet_t s;
  s.id = ts->next_snippet_id++;
  s.start_tick = start_tick;
  s.end_tick = start_tick + duration;
  s.inputs = NULL;
  s.input_count = 0;
  init_snippet_inputs(&s);
  return s;
}

void v_init(physics_v_t *t) {
  t->current_size = 1;
  t->max_size = 1;
  t->data = calloc(1, sizeof(SWorldCore));
  t->data[0] = wc_empty();
}

void v_push(physics_v_t *t, SWorldCore *world) {
  ++t->current_size;
  if (t->current_size > t->max_size) {
    t->max_size *= 2;
    t->data = realloc(t->data, t->max_size * sizeof(SWorldCore));
  }
  t->data[t->current_size - 1] = wc_empty();
  wc_copy_world(&t->data[t->current_size - 1], world);
}
