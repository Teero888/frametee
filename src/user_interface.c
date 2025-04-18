#include "user_interface.h"
#include "cimgui.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TPS 50
#define MIN_TIMELINE_ZOOM 0.05f
#define MAX_TIMELINE_ZOOM 20.0f
#define SNAP_THRESHOLD_PX 5.0f // Snap threshold in pixels
#define DEFAULT_TRACK_HEIGHT 40.f

// --- Docking Setup ---
// Call this once after ImGui context creation and before the main loop if you want a specific layout.
// Alternatively, call it within ui_render on the first frame.
void setup_docking(ui_handler *ui) {
  ImGuiID main_dockspace_id = igGetID_Str("MainDockSpace");

  // Ensure the dockspace covers the entire viewport initially
  ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos(viewport->WorkPos, ImGuiCond_Always, (ImVec2){0.0f, 0.0f});
  igSetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  igSetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove;
  host_window_flags |=
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0.0f, 0.0f});
  igBegin("DockSpace Host Window", NULL,
          host_window_flags); // pass null for p_open to prevent closing the host window
  igPopStyleVar(3);

  // create the main dockspace
  igDockSpace(main_dockspace_id, (ImVec2){0.0f, 0.0f}, ImGuiDockNodeFlags_PassthruCentralNode,
              NULL); // Passthru allows seeing background
  igEnd();

  // -- Build the initial layout programmatically (optional, but good for setup) --
  // This needs to be done *after* the DockSpace call, often on the first frame or after a reset.
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // Split the main dockspace: Timeline at bottom, rest on top
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom = igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.30f, NULL,
                                                    &dock_id_top); // Timeline takes 30%

    // Split the top area: Player list on left, properties on right
    ImGuiID dock_id_left;
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.80f, NULL,
                                                   &dock_id_left); // Player list takes 20% (1.0 - 0.8)
    // The remaining central node of the top split (where dock_id_right was created) will be left empty by
    // default with PassthruCentralNode

    // Assign windows to docks
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    // igDockBuilderDockWindow("Player List", dock_id_left);
    // igDockBuilderDockWindow("Properties", dock_id_right);

    igDockBuilderFinish(main_dockspace_id);
  }
}

// --- Timeline Window ---

// Converts screen X position to timeline tick
int screen_x_to_tick(const timeline_state *ts, float screen_x, float timeline_start_x) {
  return ts->view_start_tick + (int)((screen_x - timeline_start_x) / ts->zoom);
}

// Converts timeline tick to screen X position
float tick_to_screen_x(const timeline_state *ts, int tick, float timeline_start_x) {
  return timeline_start_x + (tick - ts->view_start_tick) * ts->zoom;
}

// Finds a snippet by its ID within a track
input_snippet *find_snippet_by_id(player_track *track, int snippet_id) {
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      return &track->snippets[i];
    }
  }
  return NULL; // Not found
}

// Finds a snippet by its ID and track index
input_snippet *find_snippet_by_id_and_track(timeline_state *ts, int snippet_id, int track_idx) {
  if (track_idx < 0 || track_idx >= ts->player_track_count)
    return NULL;
  return find_snippet_by_id(&ts->player_tracks[track_idx], snippet_id);
}

// Calculates a snapped tick position based on nearby snippet edges
// Considers snapping both the start and end of the dragged snippet.
int calculate_snapped_tick(const timeline_state *ts, int desired_start_tick, int dragged_snippet_duration,
                           int exclude_snippet_id) {
  int snapped_start_tick = desired_start_tick; // Default to no snapping
  float snap_threshold_ticks = SNAP_THRESHOLD_PX / ts->zoom;
  float min_distance = snap_threshold_ticks + 1; // Initialize min_distance > threshold

  // The potential tick that would become the *start* of the dragged snippet after snapping
  int candidate_snapped_start_tick = desired_start_tick;

  // --- Check snapping to other snippet edges ---
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet *other = &track->snippets[j];
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
bool check_for_overlap(const player_track *track, int start_tick, int end_tick, int exclude_snippet_id) {
  if (start_tick >= end_tick)
    return false; // Invalid range

  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet *other = &track->snippets[i];
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
bool remove_snippet_from_track(player_track *track, int snippet_id) {
  int found_idx = -1;
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1) {
    // Shift elements to fill the gap
    memmove(&track->snippets[found_idx], &track->snippets[found_idx + 1],
            (track->snippet_count - found_idx - 1) * sizeof(input_snippet));
    track->snippet_count--;
    // Reallocate memory (optional, but good practice if removing often)
    track->snippets = realloc(track->snippets, sizeof(input_snippet) * track->snippet_count);
    // Note: realloc could fail, but for simplicity in this example, we omit error handling.
    return true;
  }
  return false; // Snippet not found
}

void add_snippet_to_track(player_track *track, const input_snippet *snippet) {
  track->snippets = realloc(track->snippets, sizeof(input_snippet) * (track->snippet_count + 1));
  if (track->snippets == NULL) {
    return;
  }
  track->snippets[track->snippet_count] = *snippet; // Copy the snippet data
  track->snippet_count++;
}

// Attempts to move a snippet to a new position and track, checking for overlaps
// Returns true if the move was successful, false otherwise.
bool try_move_snippet(timeline_state *ts, int snippet_id, int source_track_idx, int target_track_idx,
                      int desired_start_tick) {
  if (source_track_idx < 0 || source_track_idx >= ts->player_track_count || target_track_idx < 0 ||
      target_track_idx >= ts->player_track_count) {
    return false; // Invalid track indices
  }

  player_track *source_track = &ts->player_tracks[source_track_idx];
  player_track *target_track = &ts->player_tracks[target_track_idx];

  // Find the snippet in the source track
  int snippet_idx_in_source = -1;
  input_snippet snippet_to_move = {0}; // Data to copy if move is valid
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

  // No overlap, the move is valid. Perform the data modification.
  if (source_track_idx == target_track_idx) {
    // Moving within the same track - just update the position
    source_track->snippets[snippet_idx_in_source].start_tick = new_start_tick;
    source_track->snippets[snippet_idx_in_source].end_tick = new_end_tick;
    // Keep selected state consistent
    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = source_track_idx;
  } else {
    // Moving to a different track - remove from source, add to target
    if (remove_snippet_from_track(source_track, snippet_id)) {
      // Update the snippet data with the new position before adding to the target
      snippet_to_move.start_tick = new_start_tick;
      snippet_to_move.end_tick = new_end_tick;
      add_snippet_to_track(target_track, &snippet_to_move);

      // Update selected state
      ts->selected_snippet_id = snippet_id;
      ts->selected_player_track_index = target_track_idx;
    } else {
      // Should not happen if remove_snippet_from_track works correctly
      return false; // Failed to remove from source
    }
  }

  return true; // Move successful
}

// --- Rendering and Interaction Functions ---

void render_timeline_controls(ui_handler *ui) {
  timeline_state *ts = &ui->timeline;

  float controls_height = igGetTextLineHeightWithSpacing() * 2.0f + 20.0f;
  igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){8, 4});
  igPushItemWidth(100);

  if (igDragInt("Current Tick", &ts->current_tick, 1, 0, 100000, "%d", ImGuiSliderFlags_None)) {
    if (ts->current_tick < 0)
      ts->current_tick = 0;
    // Optional: Center view on current tick if dragged
    // ts->view_start_tick = ts->current_tick - (int)((igGetWindowSize_Nil()->x * 0.5f) / ts->zoom);
    // if (ts->view_start_tick < 0) ts->view_start_tick = 0;
  }

  igSameLine(0, 8);
  if (igButton("|<", (ImVec2){30, 0}))
    ts->current_tick = 0;
  igSameLine(0, 4);
  if (igArrowButton("<<", ImGuiDir_Left))
    ts->current_tick = (ts->current_tick >= 50) ? ts->current_tick - 50 : 0;
  igSameLine(0, 4);
  if (igButton("Play", (ImVec2){50, 0})) { /* TODO: Playback logic */
  }
  igSameLine(0, 4);
  if (igArrowButton(">>", ImGuiDir_Right))
    ts->current_tick += 50;
  igSameLine(0, 4);
  if (igButton(">|", (ImVec2){30, 0})) { /* TODO: Go to end */
  } // Need to know total duration/end tick

  igSameLine(0, 20);
  igText("Zoom:");
  igSameLine(0, 4);
  igSetNextItemWidth(150);
  // Store mouse position for zoom center before slider potentially changes layout
  ImVec2 mouse_pos = igGetIO_Nil()->MousePos;
  ImVec2 window_pos;
  igGetWindowPos(&window_pos);
  ImVec2 window_size;
  igGetWindowSize(&window_size);
  float controls_height_before_slider =
      igGetTextLineHeightWithSpacing() * 2.0f + 20.0f; // Approx height before this line
  float header_height = igGetTextLineHeightWithSpacing();
  ImVec2 timeline_start_pos = {window_pos.x, window_pos.y + controls_height_before_slider + header_height};
  float timeline_area_width = window_size.x;

  float old_zoom = ts->zoom;
  if (igSliderFloat("##Zoom", &ts->zoom, MIN_TIMELINE_ZOOM, MAX_TIMELINE_ZOOM, "%.2f",
                    ImGuiSliderFlags_Logarithmic)) {
    ts->zoom = fmaxf(MIN_TIMELINE_ZOOM, fminf(MAX_TIMELINE_ZOOM, ts->zoom)); // Clamp
    // Adjust view_start_tick to zoom towards the mouse position if timeline area is hovered
    // This logic is also in handle_timeline_interaction, could potentially be unified or this is removed.
    // For simplicity here, just clamp the view start tick after zoom
    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  }
  igPopItemWidth();
  igPopStyleVar(1); // Pop ItemSpacing
}

void handle_timeline_interaction(ui_handler *ui, ImRect timeline_bb) {
  timeline_state *ts = &ui->timeline;
  ImGuiIO *io = igGetIO_Nil();
  ImVec2 mouse_pos = io->MousePos;

  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  // Zoom with mouse wheel
  if (is_timeline_hovered && io->MouseWheel != 0) {
    int mouse_tick_before_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    float zoom_delta = io->MouseWheel * 0.1f * ts->zoom; // Scale zoom delta by current zoom
    float old_zoom = ts->zoom;
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

void draw_timeline_header(ui_handler *ui, ImDrawList *draw_list, ImRect timeline_bb, float header_y) {
  const timeline_state *ts = &ui->timeline;

  ImU32 tick_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.7f);
  ImU32 tick_sec_col = igGetColorU32_Col(ImGuiCol_Text, 0.9f);
  ImU32 tick_text_col = igGetColorU32_Col(ImGuiCol_Text, 1.0f);

  float header_height = igGetTextLineHeightWithSpacing();
  float timeline_area_width = timeline_bb.Max.x - timeline_bb.Min.x;

  // Determine tick step and label step based on zoom
  // These values are heuristic and can be adjusted
  int min_pixels_per_tick_label = 60; // Minimum pixel spacing between major labels
  float pixels_per_sec = ts->zoom * TPS;

  int label_tick_step; // Step for major labels (e.g., seconds or bigger units)
  int tick_step;       // Step for minor ticks

  if (pixels_per_sec < min_pixels_per_tick_label * 0.5) { // Less than half min_pixels per second
    // Labels based on seconds, but maybe skip some seconds
    int sec_step = (int)ceilf(min_pixels_per_tick_label / pixels_per_sec);
    if (sec_step <= 1)
      sec_step = 1; // Every second if enough space
    else if (sec_step <= 2)
      sec_step = 2;
    else if (sec_step <= 5)
      sec_step = 5;
    else if (sec_step <= 10)
      sec_step = 10;
    else
      sec_step = (int)ceilf(sec_step / 10.0f) * 10; // 10, 20, 30, ...
    label_tick_step = sec_step * TPS;

    // Minor ticks within the label step
    if (ts->zoom < 0.1f)
      tick_step = label_tick_step / 10; // E.g., 10 ticks per label block
    else if (ts->zoom < 0.5f)
      tick_step = label_tick_step / 5;
    else
      tick_step = label_tick_step / 2;
    if (tick_step == 0)
      tick_step = 1;

  } else { // Enough space to potentially label individual ticks or smaller groups
    // Labels based on tick counts
    label_tick_step = (int)ceilf(min_pixels_per_tick_label / ts->zoom);
    if (label_tick_step <= 1)
      label_tick_step = 1;
    else if (label_tick_step <= 2)
      label_tick_step = 2;
    else if (label_tick_step <= 5)
      label_tick_step = 5;
    else if (label_tick_step <= 10)
      label_tick_step = 10;
    else if (label_tick_step <= 25)
      label_tick_step = 25;
    else if (label_tick_step <= 50)
      label_tick_step = 50;
    else if (label_tick_step <= 100)
      label_tick_step = 100;
    else
      label_tick_step = (int)ceilf(label_tick_step / 100.0f) * 100; // 100, 200, ...

    // Minor ticks within the label step
    if (ts->zoom > 5.0f)
      tick_step = 1; // Every tick
    else if (ts->zoom > 2.0f)
      tick_step = 2; // Every 2 ticks
    else if (ts->zoom > 1.0f)
      tick_step = 5; // Every 5 ticks
    else
      tick_step = label_tick_step / 5; // E.g., 5 minor ticks between labels
    if (tick_step == 0)
      tick_step = 1;
  }

  // Ensure label_tick_step is a multiple of tick_step if possible
  if (label_tick_step < tick_step)
    label_tick_step = tick_step;
  else if (label_tick_step % tick_step != 0)
    label_tick_step = (label_tick_step / tick_step + 1) * tick_step;

  int max_visible_ticks = (int)(timeline_area_width / ts->zoom) + 2; // +2 for safety margins
  int view_start_tick_rounded =
      (ts->view_start_tick / tick_step) * tick_step; // Start drawing from a tick_step multiple
  int view_end_tick = ts->view_start_tick + max_visible_ticks;

  for (int tick = view_start_tick_rounded; tick <= view_end_tick; tick += tick_step) {
    if (tick < 0)
      continue;

    float x = tick_to_screen_x(ts, tick, timeline_bb.Min.x);

    // Skip drawing ticks far off-screen
    if (x < timeline_bb.Min.x - 10 || x > timeline_bb.Max.x + 10)
      continue;

    bool is_label_marker = (tick % label_tick_step == 0);
    bool is_second_marker = (tick != 0 && TPS > 0 && tick % (int)TPS == 0); // Mark full seconds if TPS > 0

    ImU32 current_tick_col = tick_col;
    float line_height = header_height * 0.4f; // Minor tick height

    if (is_second_marker) {
      current_tick_col = tick_sec_col;
      line_height = header_height; // Second markers are full height
    } else if (is_label_marker) {
      current_tick_col = tick_col;
      line_height = header_height * 0.75f; // Label markers are taller than minor
    }

    // Draw tick line
    ImDrawList_AddLine(draw_list, (ImVec2){x, header_y + header_height - line_height},
                       (ImVec2){x, header_y + header_height}, current_tick_col, 1.0f);

    // Draw label text
    if (is_label_marker) {
      char label[64];
      if (is_second_marker) {
        snprintf(label, sizeof(label), "%ds", tick / (int)TPS);
      } else {
        snprintf(label, sizeof(label), "%d", tick);
      }

      ImVec2 text_size;
      igCalcTextSize(&text_size, label, NULL, false, 0);
      // Adjust text position slightly to the right of the tick mark and vertically centered
      ImVec2 text_pos = {x + 3.0f, header_y + (header_height - text_size.y) * 0.5f};

      // Prevent labels from drawing over the left window edge
      if (text_pos.x < timeline_bb.Min.x + 3)
        text_pos.x = timeline_bb.Min.x + 3;
      // Prevent labels from drawing too close to the right window edge
      if (text_pos.x + text_size.x > timeline_bb.Max.x - 3)
        text_pos.x = timeline_bb.Max.x - text_size.x - 3;
      if (text_pos.x < timeline_bb.Min.x)
        continue; // Don't draw if completely off screen

      ImDrawList_AddText_Vec2(draw_list, text_pos, tick_text_col, label, NULL);
    }
  }
}

void render_input_snippet(ui_handler *ui, int track_index, int snippet_index, input_snippet *snippet,
                          ImDrawList *draw_list, float track_top, float track_bottom, ImRect timeline_bb) {
  timeline_state *ts = &ui->timeline;
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
  ImRect snippet_bb = {snippet_min, snippet_max};

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

  // Optional: Draw snippet name/ID text (ensure it fits and is visible)
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

void render_player_track(ui_handler *ui, int track_index, player_track *track, ImDrawList *draw_list,
                         ImRect timeline_bb, float track_top, float track_bottom) {
  timeline_state *ts = &ui->timeline;
  ImGuiIO *io = igGetIO_Nil();

  // Draw track background
  ImU32 track_bg_col = (track_index % 2 == 0) ? igGetColorU32_Col(ImGuiCol_FrameBg, 1.0f)
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
    render_input_snippet(ui, track_index, j, &track->snippets[j], draw_list, track_top, track_bottom,
                         timeline_bb);
  }

  // Optional: Track label
  // Add text offset from the left edge, vertically centered
  char track_label[64];
  snprintf(track_label, sizeof(track_label), "Track %d", track_index + 1);
  ImVec2 text_size;
  igCalcTextSize(&text_size, track_label, NULL, false, 0);
  ImVec2 text_pos = {timeline_bb.Min.x + 10.0f, track_top + (ts->track_height - text_size.y) * 0.5f};
  ImDrawList_AddText_Vec2(draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 0.7f), track_label, NULL);
}

// Renamed parameter for clarity
void draw_playhead(ui_handler *ui, ImDrawList *draw_list, ImRect timeline_bb, float playhead_start_y) {
  const timeline_state *ts = &ui->timeline;

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

void draw_drag_preview(ui_handler *ui, ImDrawList *overlay_draw_list, ImRect timeline_bb) {
  timeline_state *ts = &ui->timeline;
  ImGuiIO *io = igGetIO_Nil();

  if (!ts->drag_state.active)
    return;

  // Get the snippet being dragged
  // You need to find the snippet data using ts->drag_state.dragged_snippet_id
  player_track *source_track =
      &ts->player_tracks[ts->drag_state.source_track_index]; // Or find by ID across all? Finding by ID might
                                                             // be safer if indices shift. Let's find by ID.
  input_snippet *dragged_snippet = NULL;
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
void render_timeline(ui_handler *ui) {
  timeline_state *ts = &ui->timeline;
  ImGuiIO *io = igGetIO_Nil();

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 4.0f);

  if (igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());
    igPopStyleVar(2);

    // --- Controls ---
    render_timeline_controls(ui);

    // --- Layout Calculations for Header and Timeline Area ---
    float header_height = igGetTextLineHeightWithSpacing();

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
    draw_timeline_header(ui, draw_list, header_bb, header_bb_min.y);

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
        handle_timeline_interaction(ui, timeline_bb);
      }

      // --- Draw Timeline Tracks and Snippets ---
      // ... (clip rect and track drawing loop using timeline_bb) ...
      igPushClipRect(timeline_bb.Min, timeline_bb.Max, true);
      float current_track_y = timeline_bb.Min.y;
      for (int i = 0; i < ts->player_track_count; ++i) {
        if (current_track_y >= timeline_bb.Max.y)
          break;
        player_track *track = &ts->player_tracks[i];
        float track_top = current_track_y;
        float track_bottom = current_track_y + ts->track_height;
        float clamped_track_bottom = fminf(track_bottom, timeline_bb.Max.y);
        if (clamped_track_bottom > track_top) {
          render_player_track(ui, i, track, draw_list, timeline_bb, track_top, clamped_track_bottom);
        }
        current_track_y += ts->track_height;
      }
      igPopClipRect();

      // --- Draw Scrollbar ---
      ImS64 max_tick = 0;
      for (int i = 0; i < ts->player_track_count; ++i) {
        player_track *track = &ts->player_tracks[i];
        for (int j = 0; j < track->snippet_count; ++j) {
          if (track->snippets[j].end_tick > max_tick) {
            max_tick = track->snippets[j].end_tick;
          }
        }
      }
      // Add padding (10% of max_tick) and ensure minimum duration
      max_tick = (ImS64)(max_tick * 1.1f);
      if (max_tick < 100)
        max_tick = 100;

      // Calculate visible ticks based on window width
      float timeline_width = timeline_bb.Max.x - timeline_bb.Min.x;
      ImS64 visible_ticks = (ImS64)(timeline_width / ts->zoom);

      // Render scrollbar
      ImRect scrollbar_bb = {timeline_bb.Min.x, timeline_bb.Max.y, timeline_bb.Max.x,
                             timeline_bb.Max.y + scrollbar_height};
      igPushID_Str("TimelineScrollbar");
      ImS64 scroll_v = (ImS64)ts->view_start_tick;
      if (igScrollbarEx(scrollbar_bb, igGetID_Str("TimelineScrollbar"), ImGuiAxis_X, &scroll_v, visible_ticks,
                        max_tick, ImDrawFlags_RoundCornersBottom)) {
        ts->view_start_tick = (int)scroll_v;
      }
      if (ts->view_start_tick < 0)
        ts->view_start_tick = 0;
      igPopID();

      // --- Handle Mouse Release -> Commit Drag/Drop (for SNIPPETS) ---
      // This only happens if a snippet drag was started and header is not being dragged
      if (ts->drag_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left) && !ts->is_header_dragging) {
        ImVec2 mouse_pos = igGetIO_Nil()->MousePos;

        // Get the snippet data needed for duration and ID
        player_track *source_track =
            &ts->player_tracks[ts->drag_state.source_track_index]; // Get source track using the index stored
                                                                   // at drag start Find the snippet by ID in
                                                                   // the source track to get its duration and
                                                                   // confirm it still exists there
        input_snippet *snippet_to_move = find_snippet_by_id(source_track, ts->drag_state.dragged_snippet_id);
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

        // Call try_move_snippet with correct parameters
        // try_move_snippet(ts, snippet_id, source_track_idx, target_track_idx, desired_start_tick);
        bool move_success =
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
      draw_playhead(ui, draw_list, timeline_bb,
                    timeline_bb.Min.y); // Playhead starts at the top Y of the timeline_bb area

    } // End if(timeline_bb has positive dimensions)

    // --- Draw Drag Preview (on overlay) ---
    // This uses the overlay draw list and needs the timeline_bb for positioning
    draw_drag_preview(ui, overlay_draw_list, timeline_bb);
  }
  igEnd(); // End Timeline window
}

// Helper to add a new empty track
player_track *add_new_track(timeline_state *timeline) {
  timeline->player_tracks =
      realloc(timeline->player_tracks, sizeof(player_track) * (timeline->player_track_count + 1));
  player_track *new_track = &timeline->player_tracks[timeline->player_track_count];
  new_track->snippets = NULL; // Initialize as empty dynamic array
  new_track->snippet_count = 0;
  timeline->player_track_count++;
  return new_track;
}

// --- Timeline Window End ---

void ui_init(ui_handler *ui) {
  ui->show_timeline = true;

  timeline_state *ts = &ui->timeline;
  memset(ts, 0, sizeof(timeline_state));
  // Initialize Timeline State variables
  ts->current_tick = 0;
  ts->view_start_tick = 0;
  ts->zoom = 1.0f; // 1 pixel per tick initially
  ts->track_height = DEFAULT_TRACK_HEIGHT;
  ts->selected_player_track_index = -1; // Nothing selected initially
  ts->selected_snippet_id = -1;

  // Initialize Drag State
  ts->drag_state.active = false;
  ts->drag_state.source_track_index = -1;
  ts->drag_state.source_snippet_index = -1;
  ts->drag_state.dragged_snippet_id = -1;
  ts->drag_state.drag_offset_ticks = 0;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0}; // Or some invalid value

  // Initialize unique snippet ID counter
  ts->next_snippet_id = 1; // Start IDs from 1

  // Initialize Players/Tracks (Example: Add two tracks)
  ts->player_track_count = 0; // Start with 0 tracks
  ts->player_tracks = NULL;   // Will be allocated by adding tracks

  // Add Player 0 (Track 0)
  player_track *p0 = add_new_track(ts);

  // Add example snippets to Player 0 (Track 0)
  input_snippet temp_snippet; // Use a temporary struct

  // Example Snippet 1 (Track 0)
  temp_snippet.id = ts->next_snippet_id++; // Assign unique ID and increment counter
  temp_snippet.start_tick = 50;
  temp_snippet.end_tick = 150;
  add_snippet_to_track(p0, &temp_snippet); // Use the helper function

  // Example Snippet 2 (Track 0)
  temp_snippet.id = ts->next_snippet_id++; // Assign unique ID
  temp_snippet.start_tick = 200;
  temp_snippet.end_tick = 220;
  add_snippet_to_track(p0, &temp_snippet);

  // Add Player 1 (Track 1)
  player_track *p1 = add_new_track(ts);

  // Add example snippet to Player 1 (Track 1)
  // Example Snippet 1 (Track 1) - This ID will now be unique
  temp_snippet.id = ts->next_snippet_id++; // Assign unique ID
  temp_snippet.start_tick = 100;
  temp_snippet.end_tick = 250;
  add_snippet_to_track(p1, &temp_snippet);

  // Add more tracks and snippets as needed...

  // Note: MAX_SNIPPETS_PER_PLAYER check is no longer needed here if add_snippet_to_track handles realloc.
  // If you want a hard limit, add the check inside add_snippet_to_track.
}

void ui_render(ui_handler *ui) {
  setup_docking(ui);
  render_timeline(ui);
}

void ui_cleanup(ui_handler *ui) { free(ui->timeline.player_tracks); }
