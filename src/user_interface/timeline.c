#include "timeline.h"
#include "../../libs/symbols.h"
#include "cimgui.h"
#include "widgets/imcol.h"
#include <GLFW/glfw3.h>
#include <limits.h>
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

// converts screen x position to timeline tick
int screen_x_to_tick(const timeline_state_t *ts, float screen_x, float timeline_start_x) {
  return ts->view_start_tick + (int)((screen_x - timeline_start_x) / ts->zoom);
}

// converts timeline tick to screen x position
float tick_to_screen_x(const timeline_state_t *ts, int tick, float timeline_start_x) {
  return timeline_start_x + (tick - ts->view_start_tick) * ts->zoom;
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

void resize_snippet_inputs(timeline_state_t *t, input_snippet_t *snippet, int new_duration) {
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
  snippet->end_tick = snippet->start_tick + new_duration;

  // recalculate rendering
  if (new_duration < snippet->input_count && snippet->end_tick <= t->current_tick)
    recalc_ts(t, snippet->start_tick - 1);

  snippet->input_count = new_duration;
}

// removes a snippet from a track by id
// returns true if removed, false if not found
bool remove_snippet_from_track(timeline_state_t *t, player_track_t *track, int snippet_id) {
  int found_idx = -1;
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1) {
    free_snippet_inputs(&track->snippets[found_idx]);

    recalc_ts(t, track->snippets[found_idx].start_tick);

    memmove(&track->snippets[found_idx], &track->snippets[found_idx + 1],
            (track->snippet_count - found_idx - 1) * sizeof(input_snippet_t));
    track->snippet_count--;
    track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * track->snippet_count);

    return true;
  }
  return false;
}

// Multi-selection and utilities
static bool is_snippet_selected(timeline_state_t *ts, int snippet_id) {
  for (int i = 0; i < ts->selected_snippet_count; ++i)
    if (ts->selected_snippet_ids[i] == snippet_id)
      return true;
  return false;
}
static void clear_selection(timeline_state_t *ts) {
  ts->selected_snippet_count = 0;
  ts->selected_snippet_id = -1;
  ts->selected_player_track_index = -1;
}
static void add_snippet_to_selection(timeline_state_t *ts, int snippet_id, int track_index) {
  if (snippet_id < 0)
    return;
  if (is_snippet_selected(ts, snippet_id))
    return;
  int cap = (int)(sizeof(ts->selected_snippet_ids) / sizeof(ts->selected_snippet_ids[0]));
  if (ts->selected_snippet_count < cap) {
    ts->selected_snippet_ids[ts->selected_snippet_count++] = snippet_id;
    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = track_index;
  }
}
static void remove_snippet_from_selection(timeline_state_t *ts, int snippet_id) {
  int pos = -1;
  for (int i = 0; i < ts->selected_snippet_count; ++i) {
    if (ts->selected_snippet_ids[i] == snippet_id) {
      pos = i;
      break;
    }
  }
  if (pos >= 0) {
    for (int i = pos; i < ts->selected_snippet_count - 1; ++i)
      ts->selected_snippet_ids[i] = ts->selected_snippet_ids[i + 1];
    ts->selected_snippet_count--;
    if (ts->selected_snippet_count == 0) {
      ts->selected_snippet_id = -1;
      ts->selected_player_track_index = -1;
    } else {
      ts->selected_snippet_id = ts->selected_snippet_ids[ts->selected_snippet_count - 1];
    }
  }
}
static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb) {
  clear_selection(ts);
  float track_top = timeline_bb.Min.y;
  for (int ti = 0; ti < ts->player_track_count; ++ti) {
    player_track_t *track = &ts->player_tracks[ti];
    float th = ts->track_height;
    float track_bottom = track_top + th;
    for (int si = 0; si < track->snippet_count; ++si) {
      input_snippet_t *snip = &track->snippets[si];
      float x1 = tick_to_screen_x(ts, snip->start_tick, timeline_bb.Min.x);
      float x2 = tick_to_screen_x(ts, snip->end_tick, timeline_bb.Min.x);
      ImRect snip_rect = (ImRect){.Min = {x1, track_top + 4}, .Max = {x2, track_bottom - 4}};
      // overlap test
      if (!(snip_rect.Max.x < rect.Min.x || snip_rect.Min.x > rect.Max.x || snip_rect.Max.y < rect.Min.y ||
            snip_rect.Min.y > rect.Max.y)) {
        add_snippet_to_selection(ts, snip->id, ti);
      }
    }
    track_top = track_bottom + 4;
  }
}

// Comparison function for qsort to sort snippets by their start time
static int compare_snippets_by_start_tick(const void *a, const void *b) {
  input_snippet_t *snip_a = *(input_snippet_t **)a;
  input_snippet_t *snip_b = *(input_snippet_t **)b;
  if (snip_a->start_tick < snip_b->start_tick)
    return -1;
  if (snip_a->start_tick > snip_b->start_tick)
    return 1;
  return 0;
}

// Merges selected snippets on a given track that are adjacent
static void merge_selected_snippets(timeline_state_t *ts, int track_index) {
  if (ts->selected_snippet_count < 2 || track_index < 0)
    return;

  player_track_t *track = &ts->player_tracks[track_index];
  input_snippet_t *candidates[256];
  int candidate_count = 0;

  // Gather all selected snippets that are on the target track
  for (int i = 0; i < ts->selected_snippet_count; ++i) {
    input_snippet_t *snip = find_snippet_by_id(track, ts->selected_snippet_ids[i]);
    if (snip) {
      candidates[candidate_count++] = snip;
    }
  }

  if (candidate_count < 2)
    return;

  // Sort the snippets by start_tick to easily find adjacent ones
  qsort(candidates, candidate_count, sizeof(input_snippet_t *), compare_snippets_by_start_tick);

  int ids_to_remove[256];
  int remove_count = 0;
  int earliest_tick = INT_MAX;

  // Iterate and merge
  for (int i = 0; i < candidate_count - 1; ++i) {
    input_snippet_t *a = candidates[i];
    input_snippet_t *b = candidates[i + 1];

    if (a->end_tick == b->start_tick) { // Found an adjacent pair
      int old_a_duration = a->input_count;
      int b_duration = b->input_count;
      int new_duration = old_a_duration + b_duration;

      // Append B's inputs to A
      a->inputs = realloc(a->inputs, sizeof(SPlayerInput) * new_duration);
      memcpy(&a->inputs[old_a_duration], b->inputs, sizeof(SPlayerInput) * b_duration);

      // Update A's properties
      a->end_tick = b->end_tick;
      a->input_count = new_duration;
      if (a->start_tick < earliest_tick)
        earliest_tick = a->start_tick;

      // Mark B for removal
      ids_to_remove[remove_count++] = b->id;

      // Nullify B's inputs to prevent double-free when it's removed
      b->inputs = NULL;
      b->input_count = 0;

      // IMPORTANT: For chained merges (A+B, then result+C), make the next
      // iteration's "previous" snippet the newly merged 'a'.
      candidates[i + 1] = a;
    }
  }

  if (remove_count > 0) {
    // Remove all the snippets that were merged
    for (int i = 0; i < remove_count; ++i) {
      remove_snippet_from_track(ts, track, ids_to_remove[i]);
    }
    clear_selection(ts);
    recalc_ts(ts, earliest_tick);
  }
}

// Auto-scroll playhead into view if playing
static void auto_scroll_playhead_if_needed(timeline_state_t *ts, ImRect timeline_bb) {
  if (!ts->is_playing || !ts->auto_scroll_playhead)
    return;
  float play_x = tick_to_screen_x(ts, ts->current_tick, timeline_bb.Min.x);
  float left = timeline_bb.Min.x;
  float right = timeline_bb.Max.x;
  float margin = 50.0f;
  if (play_x < left + margin) {
    float ticks = (left + margin - play_x) / ts->zoom;
    ts->view_start_tick -= (int)ceilf(ticks);
    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  } else if (play_x > right - margin) {
    float ticks = (play_x - (right - margin)) / ts->zoom;
    ts->view_start_tick += (int)ceilf(ticks);
  }
}
// Global keyboard shortcuts (moved out of popup so they always work)
static void process_global_shortcuts(timeline_state_t *ts) {
  ImGuiIO *io = igGetIO_Nil();
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_A, true)) {
    // Add snippet at current tick to selected track (or first track)
    int track_idx = ts->selected_player_track_index >= 0 ? ts->selected_player_track_index
                                                         : (ts->player_track_count > 0 ? 0 : -1);
    if (track_idx >= 0) {
      input_snippet_t snip = create_empty_snippet(ts, ts->current_tick, 50);
      add_snippet_to_track(&ts->player_tracks[track_idx], &snip);
    }
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_R, true)) {
    // Split all selected snippets at current_tick
    if (ts->selected_snippet_count > 0) {
      int originals[256];
      int cnt = ts->selected_snippet_count;
      for (int i = 0; i < cnt; ++i)
        originals[i] = ts->selected_snippet_ids[i];
      for (int i = 0; i < cnt; ++i) {
        int sid = originals[i];
        // find snippet
        for (int ti = 0; ti < ts->player_track_count; ++ti) {
          player_track_t *track = &ts->player_tracks[ti];
          for (int s = 0; s < track->snippet_count; ++s) {
            input_snippet_t *snippet = &track->snippets[s];
            if (snippet->id == sid) {
              int split_tick = ts->current_tick;
              if (split_tick <= snippet->start_tick || split_tick >= snippet->end_tick)
                continue;
              int old_end = snippet->end_tick;
              input_snippet_t right = create_empty_snippet(ts, split_tick, old_end - split_tick);
              int offset = split_tick - snippet->start_tick;
              int right_count = old_end - split_tick;
              if (right_count > 0) {
                right.inputs = malloc(sizeof(SPlayerInput) * right_count);
                memcpy(right.inputs, snippet->inputs + offset, right_count * sizeof(SPlayerInput));
                right.input_count = right_count;
              }
              resize_snippet_inputs(ts, snippet, offset);
              snippet->end_tick = split_tick;
              add_snippet_to_track(track, &right);
            }
          }
        }
      }
    }
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_D, true)) {
    // Delete all selected snippets
    if (ts->selected_snippet_count > 0) {
      int originals[256];
      int cnt = ts->selected_snippet_count;
      for (int i = 0; i < cnt; ++i)
        originals[i] = ts->selected_snippet_ids[i];
      for (int i = 0; i < cnt; ++i) {
        int sid = originals[i];
        for (int ti = 0; ti < ts->player_track_count; ++ti) {
          player_track_t *track = &ts->player_tracks[ti];
          remove_snippet_from_track(ts, track, sid);
        }
      }
      clear_selection(ts);
    }
  }
}

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

void recalc_ts(timeline_state_t *ts, int tick) {
  ts->vec.current_size = imin(ts->vec.current_size, imax((tick - 1) / 50, 1));
  ts->previous_world.m_GameTick = INT_MAX;
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

// finds a snippet by its id within a track
input_snippet_t *find_snippet_by_id(player_track_t *track, int snippet_id) {
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      return &track->snippets[i];
    }
  }
  return NULL;
}

void advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, 0);

  // record new tick
  if (ts->recording_snippet && ts->current_tick < ts->recording_snippet->start_tick)
    ts->current_tick -= steps;
  if (ts->recording_snippet && ts->current_tick > ts->recording_snippet->end_tick) {
    resize_snippet_inputs(ts, ts->recording_snippet, ts->current_tick - ts->recording_snippet->start_tick);
    ts->recording_snippet->end_tick = ts->current_tick;
    if (ts->recording_snippet->input_count > 0)
      ts->recording_snippet->inputs[ts->recording_snippet->input_count - 1] = ts->recording_input;
  }
}

// finds a snippet by its id and track index
input_snippet_t *find_snippet_by_id_and_track(timeline_state_t *ts, int snippet_id, int track_idx) {
  if (track_idx < 0 || track_idx >= ts->player_track_count)
    return NULL;
  return find_snippet_by_id(&ts->player_tracks[track_idx], snippet_id);
}

// calculates a snapped tick position based on nearby snippet edges
// considers snapping both the start and end of the dragged snippet.
int calculate_snapped_tick(const timeline_state_t *ts, int desired_start_tick, int dragged_snippet_duration,
                           int exclude_snippet_id) {
  int snapped_start_tick = desired_start_tick; // Default to no snapping
  float snap_threshold_ticks = SNAP_THRESHOLD_PX / ts->zoom;
  float min_distance = snap_threshold_ticks + 1; // Initialize min_distance > threshold

  // The potential tick that would become the *start* of the dragged snippet after snapping
  int candidate_snapped_start_tick = desired_start_tick;

  // Check snapping to other snippet edges
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

        float dist_start_to_other_edge = fabsf((float)(desired_start_tick - other_edge_tick));
        if (dist_start_to_other_edge < min_distance) {
          min_distance = dist_start_to_other_edge;
          candidate_snapped_start_tick = other_edge_tick; // The new start tick is the other snippet's edge
        }

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

  // Check snapping to tick 0
  float dist_start_to_zero = fabsf((float)(desired_start_tick - 0));
  if (dist_start_to_zero < min_distance) {
    min_distance = dist_start_to_zero;
    candidate_snapped_start_tick = 0; // Snap start to 0
  }

  // If the minimum distance found is within the snap threshold, apply the snap.
  // Otherwise, the snippet does not snap, and we return the original desired tick.
  if (min_distance <= snap_threshold_ticks) {
    snapped_start_tick = candidate_snapped_start_tick;
  } else {
    snapped_start_tick = desired_start_tick; // No snap point within threshold
  }

  return snapped_start_tick;
}

// checks if a snippet range overlaps with any snippets in a track (excluding one)
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

// attempts to move a snippet to a new position and track, checking for overlaps
// returns true if the move was successful, false otherwise.
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
  int old_start_tick = snippet_to_move.start_tick;
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
    resize_snippet_inputs(ts, sn, new_duration);
    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = source_track_idx;

    if (igGetIO_Nil()->KeyAlt) {
      input_snippet_t new_snip = snippet_to_move;
      new_snip.id = ts->next_snippet_id++;
      new_snip.inputs = NULL;
      new_snip.input_count = 0;
      copy_snippet_inputs(&new_snip, &snippet_to_move);
      add_snippet_to_track(target_track, &new_snip);
    } else
      recalc_ts(ts, imin(new_start_tick, old_start_tick));
  } else {
    // cross track deep copy
    snippet_to_move.start_tick = new_start_tick;
    snippet_to_move.end_tick = new_end_tick;

    input_snippet_t new_snip = snippet_to_move;
    if (igGetIO_Nil()->KeyAlt)
      new_snip.id = ts->next_snippet_id++;
    new_snip.inputs = NULL;
    new_snip.input_count = 0;
    copy_snippet_inputs(&new_snip, &snippet_to_move);
    add_snippet_to_track(target_track, &new_snip);

    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = target_track_idx;

    // dont remove when we press alt. keybind for duplication
    if (!igGetIO_Nil()->KeyAlt)
      remove_snippet_from_track(ts, source_track, snippet_id);
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

// rendering and interaction functions
void render_timeline_controls(timeline_state_t *ts) {
  igPushItemWidth(100);

  if (igDragInt("Current Tick", &ts->current_tick, 1, 0, 100000, "%d", ImGuiSliderFlags_None)) {
    if (ts->current_tick < 0)
      ts->current_tick = 0;
  }

  if ((igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true) || igIsKeyPressed_Bool(ImGuiKey_MouseX1, true)) &&
      ts->current_tick > 0) {
    ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
    ts->is_playing = false;
    advance_tick(ts, -1);
  }
  if (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true) || igIsKeyPressed_Bool(ImGuiKey_MouseX2, true)) {
    ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
    ts->is_playing = false;
    advance_tick(ts, 1);
  }

  if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true)) {
    ts->gui_playback_speed = imax(--ts->playback_speed, 1);
  }
  if (igIsKeyPressed_Bool(ImGuiKey_UpArrow, true)) {
    ++ts->gui_playback_speed;
  }

  igSameLine(0, 8);
  if (igButton(ICON_KI_STEP_BACKWARD, (ImVec2){30, 0}))
    ts->current_tick = 0;
  igSameLine(0, 4);
  if (igButton(ICON_KI_BACKWARD, (ImVec2){30, 0}))
    advance_tick(ts, -ts->playback_speed);
  igSameLine(0, 4);
  if (igButton(ts->is_playing ? ICON_KI_PAUSE : ICON_KI_CARET_RIGHT, (ImVec2){50, 0})) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime();
    }
  }
  igSameLine(0, 4);
  if (igButton(ICON_KI_FORWARD, (ImVec2){30, 0}))
    advance_tick(ts, ts->playback_speed);
  igSameLine(0, 4);
  if (igButton(ICON_KI_STEP_FORWARD, (ImVec2){30, 0})) {
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
    if (ts->view_start_tick < 0)
      ts->view_start_tick = 0;
  }
  igSameLine(0, 20);
  igText("Playback Speed:");
  igSameLine(0, 4);
  igSetNextItemWidth(150);
  igSliderInt("##Speed", &ts->gui_playback_speed, 1, 100, "%d", ImGuiSliderFlags_None);

  igSameLine(0, 20);
  bool was_recording = ts->recording;
  if (igButton(ts->recording ? "Stop Recording" : "Record", (ImVec2){0, 0})) {
    // Simply toggle the recording state. The logic to handle the consequences is below.
    ts->recording = !ts->recording;

    // If we just STOPPED recording, handle cleanup of the recording snippet.
    if (!ts->recording && ts->recording_snippet) {
      if (ts->recording_snippet->input_count == 0) {
        // Find the track the (now empty) snippet was on to remove it.
        // This requires iterating as the index isn't stored.
        for (int i = 0; i < ts->player_track_count; ++i) {
          if (find_snippet_by_id(&ts->player_tracks[i], ts->recording_snippet->id)) {
            remove_snippet_from_track(ts, &ts->player_tracks[i], ts->recording_snippet->id);
            break;
          }
        }
      }
      ts->recording_snippet = NULL;
    }
  }

  // If we just STARTED recording, decide whether to extend or create a new snippet.
  if (!was_recording && ts->recording) {
    if (ts->selected_player_track_index < 0) {
      ts->recording = false; // Abort: Cannot record without a selected track.
    } else {
      player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
      input_snippet_t *sel_snip =
          (ts->selected_snippet_id >= 0) ? find_snippet_by_id(track, ts->selected_snippet_id) : NULL;

      if (sel_snip) {
        // --- EXTEND a selected snippet ---
        ts->current_tick = sel_snip->end_tick; // Move playhead to the end

        // Check if another snippet is in the way at the new position
        bool is_in_way = check_for_overlap(track, ts->current_tick, ts->current_tick + 1, sel_snip->id);

        if (is_in_way) {
          ts->recording = false; // Abort: Can't record over another snippet
        } else {
          ts->recording_snippet = sel_snip; // Target the selected snippet for recording
        }
      } else {
        // --- CREATE a new snippet ---
        bool is_in_way = check_for_overlap(track, ts->current_tick, ts->current_tick + 1, -1);

        if (is_in_way) {
          ts->recording = false; // Abort: Can't record over an existing snippet
        } else {
          input_snippet_t new_snip = create_empty_snippet(ts, ts->current_tick, 1);
          add_snippet_to_track(track, &new_snip);
          ts->recording_snippet = &track->snippets[track->snippet_count - 1];
        }
      }
    }
  }

  if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
    ts->recording = false;
    ts->recording_snippet = NULL;
  }

  if (ts->recording) {
    igSameLine(0, 10);
    igTextColored((ImVec4){1.0f, 0.2f, 0.2f, 1.0f}, ICON_KI_REC);
  }

  igPopItemWidth();
}

void handle_timeline_interaction(timeline_state_t *ts, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  ImVec2 mouse_pos = io->MousePos;
  ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());

  if (io->ConfigFlags & ImGuiConfigFlags_NoMouse)
    return;

  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  //  Selection rectangle start (moved here so snippets are already created & hovered state is valid)
  // Only start a selection box if the user clicked empty timeline area (not over a snippet/UI item)
  if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, 0) && is_timeline_hovered && !igIsAnyItemHovered()) {
    ts->selection_box_active = true;
    ts->selection_box_start = mouse_pos;
    ts->selection_box_end = mouse_pos;
    if (!igGetIO_Nil()->KeyShift) {
      clear_selection(ts); // replace unless shift is held
    }
  }

  if (ts->selection_box_active) {
    if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      ts->selection_box_end = mouse_pos;
      // draw translucent selection rect in the foreground overlay so it is above tracks/snippets
      ImVec2 a = ts->selection_box_start;
      ImVec2 b = ts->selection_box_end;
      ImVec2 min = (ImVec2){fminf(a.x, b.x), fminf(a.y, b.y)};
      ImVec2 max = (ImVec2){fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
      ImDrawList_AddRectFilled(overlay_draw_list, min, max, IM_COL32(100, 150, 240, 80), 0.0f,
                               ImDrawFlags_None);
      ImDrawList_AddRect(overlay_draw_list, min, max, IM_COL32(100, 150, 240, 180), 0.0f, ImDrawFlags_None,
                         0.0f);
    } else {
      // mouse released -> finalize selection
      ImVec2 a = ts->selection_box_start;
      ImVec2 b = ts->selection_box_end;
      ImRect rect =
          (ImRect){.Min = {fminf(a.x, b.x), fminf(a.y, b.y)}, .Max = {fmaxf(a.x, b.x), fmaxf(a.y, b.y)}};
      select_snippets_in_rect(ts, rect, timeline_bb);
      ts->selection_box_active = false;
    }
  }

  // Zoom with mouse wheel
  if (is_timeline_hovered && io->MouseWheel != 0) {
    int mouse_tick_before_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    float zoom_delta = io->MouseWheel * 0.1f * ts->zoom; // Scale zoom delta by current zoom
    ts->zoom = fmaxf(MIN_TIMELINE_ZOOM, fminf(MAX_TIMELINE_ZOOM, ts->zoom + zoom_delta));

    // Adjust view_start_tick so that the position under the mouse stays at the same tick
    int mouse_tick_after_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    int tick_delta = mouse_tick_before_zoom - mouse_tick_after_zoom;
    ts->view_start_tick += tick_delta;
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

// helper: pick nice tick step that gives enough pixel spacing
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

  // Invisible button to capture mouse interaction for this snippet
  igSetCursorScreenPos((ImVec2){snippet_start_x, track_top + 2.0f});
  char _snippet_id_buf[64];
  snprintf(_snippet_id_buf, sizeof(_snippet_id_buf), "snippet_%d_%d", track_index, snippet->id);
  bool is_item_hovered = igInvisibleButton(
      _snippet_id_buf, (ImVec2){snippet_end_x - snippet_start_x, track_bottom - track_top - 4.0f},
      ImGuiButtonFlags_MouseButtonLeft);
  bool is_item_active = igIsItemActive(); // True while button is held or being dragged
  bool is_item_clicked = igIsItemClicked(ImGuiMouseButton_Left);

  // Handle selection (multi-select support)
  bool was_selected = is_snippet_selected(ts, snippet->id);
  if (is_item_clicked && !ts->drag_state.active) {
    if (io->KeyShift) {
      // toggle selection
      if (was_selected) {
        remove_snippet_from_selection(ts, snippet->id);
      } else {
        add_snippet_to_selection(ts, snippet->id, track_index);
      }
    } else {
      if (!was_selected) {
        clear_selection(ts);
        add_snippet_to_selection(ts, snippet->id, track_index);
      }
    }
  }

  // Initiate drag for single or multi-selection
  if (is_item_active && igIsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !ts->drag_state.active) {
    ts->drag_state.active = true;
    ts->drag_state.source_track_index = track_index;
    ts->drag_state.source_snippet_index = snippet_index;
    ts->drag_state.initial_mouse_pos = io->MousePos;
    int mouse_tick_at_click = screen_x_to_tick(ts, ts->drag_state.initial_mouse_pos.x, timeline_bb.Min.x);
    ts->drag_state.drag_offset_ticks = mouse_tick_at_click - snippet->start_tick;
    ts->drag_state.dragged_snippet_id = snippet->id;

    // If clicked snippet wasn't selected, make sure it's selected for the drag
    if (!is_snippet_selected(ts, snippet->id)) {
      clear_selection(ts);
      add_snippet_to_selection(ts, snippet->id, track_index);
    }

    // // ALT duplicates selection on drag start (Premiere-like)
    // if (io->KeyAlt) {
    //   // set dragged id to first selected duplicate (best-effort)
    //   if (ts->selected_snippet_count > 0) {
    //     ts->drag_state.dragged_snippet_id = ts->selected_snippet_ids[0];
    //   }
    // }
  }

  // Draw Snippet
  bool is_selected = is_snippet_selected(ts, snippet->id);
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
  if (snippet_max.x - snippet_min.x > text_size.x + 8.0f) {
    ImDrawList_AddText_Vec2(draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 1.0f), label, NULL);
  }

  // Context menu (right-click)
  if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, 0) &&
      igIsMouseHoveringRect((ImVec2){snippet_min.x, snippet_min.y}, (ImVec2){snippet_max.x, snippet_max.y},
                            true)) {
    igOpenPopup_Str("RightClickMenu", 0);
    ts->selected_player_track_index = track_index;
  }
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
    igOpenPopup_Str("RightClickMenu", 0);
    ts->selected_player_track_index = track_index;
  }

  if (igBeginPopup("RightClickMenu", ImGuiPopupFlags_AnyPopupLevel)) {
    if (igMenuItem_Bool("Add Snippet (Ctrl+a)", NULL, false, true)) {
      int track_idx = ts->selected_player_track_index;
      input_snippet_t snip = create_empty_snippet(ts, ts->current_tick, 50);
      add_snippet_to_track(&ts->player_tracks[track_idx], &snip);
    }

    // Check if the current selection is a valid candidate for merging
    bool can_merge = false;
    if (ts->selected_snippet_count > 1 && ts->selected_player_track_index >= 0) {
      player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
      input_snippet_t *sorted_selection[256];
      int count = 0;
      for (int i = 0; i < ts->selected_snippet_count; ++i) {
        input_snippet_t *snip = find_snippet_by_id(track, ts->selected_snippet_ids[i]);
        if (snip)
          sorted_selection[count++] = snip;
      }
      if (count > 1) {
        qsort(sorted_selection, count, sizeof(input_snippet_t *), compare_snippets_by_start_tick);
        for (int i = 0; i < count - 1; ++i) {
          if (sorted_selection[i]->end_tick == sorted_selection[i + 1]->start_tick) {
            can_merge = true;
            break;
          }
        }
      }
    }

    if (igMenuItem_Bool("Merge Snippets", "Ctrl+M", false, can_merge)) {
      merge_selected_snippets(ts, ts->selected_player_track_index);
    }

    if (igMenuItem_Bool("Split Snippet (Ctrl+r)", NULL, false, ts->selected_snippet_id != -1)) {
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
          resize_snippet_inputs(ts, snippet, offset);
          snippet->end_tick = split_tick;

          // Insert the right-hand snippet into the track
          add_snippet_to_track(track, &right);
        }
      }
    }

    if (igMenuItem_Bool("Delete Snippet (Ctrl+d)", NULL, false, ts->selected_snippet_count > 0)) {
      if (ts->selected_snippet_count > 0) { // Redundant check, but safe
        int originals[256];
        int cnt = ts->selected_snippet_count;
        for (int i = 0; i < cnt; ++i)
          originals[i] = ts->selected_snippet_ids[i];

        for (int i = 0; i < cnt; ++i) {
          int sid = originals[i];
          for (int ti = 0; ti < ts->player_track_count; ++ti) {
            remove_snippet_from_track(ts, &ts->player_tracks[ti], sid);
          }
        }
        clear_selection(ts);
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

  // FIX 2: Manually clip the preview drawing to the timeline's bounding box.
  // This prevents the preview from rendering outside the designated track area.
  ImDrawList_PushClipRect(overlay_draw_list, timeline_bb.Min, timeline_bb.Max, true);

  // Find clicked/primary snippet and its source track index
  input_snippet_t *clicked_snippet = NULL;
  int clicked_track_idx = -1;
  for (int i = 0; i < ts->player_track_count; ++i) {
    clicked_snippet = find_snippet_by_id(&ts->player_tracks[i], ts->drag_state.dragged_snippet_id);
    if (clicked_snippet) {
      clicked_track_idx = i;
      break;
    }
  }
  if (!clicked_snippet) {
    ImDrawList_PopClipRect(overlay_draw_list); // Pop clip rect before returning
    return;
  }

  // compute delta ticks for the clicked snippet (snapping applied same as drag start)
  int clicked_duration = clicked_snippet->end_tick - clicked_snippet->start_tick;
  int mouse_tick = screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  int desired_start_tick_clicked = mouse_tick - ts->drag_state.drag_offset_ticks;
  int snapped_start_tick_clicked =
      calculate_snapped_tick(ts, desired_start_tick_clicked, clicked_duration, clicked_snippet->id);
  int delta_ticks = snapped_start_tick_clicked - clicked_snippet->start_tick;

  // FIX 1: The stride calculation was incorrect. The tracks are rendered contiguously
  // with no gap. Using just track_height for the stride fixes the accumulating Y-offset bug.
  const float inner_pad = 2.0f;
  const float snippet_h = ts->track_height - inner_pad * 2.0f;
  const float stride = ts->track_height; // The track layout has no gap.

  // Compute where inside the clicked snippet the user grabbed (based on drag start mouse Y).
  // This lets the clicked snippet remain "under" the cursor as you move between tracks.
  float computed_clicked_track_top = timeline_bb.Min.y + (float)clicked_track_idx * stride;
  float click_offset_into_visual =
      ts->drag_state.initial_mouse_pos.y - (computed_clicked_track_top + inner_pad);
  if (click_offset_into_visual < 0.0f)
    click_offset_into_visual = 0.0f;
  if (click_offset_into_visual > snippet_h)
    click_offset_into_visual = snippet_h;

  // Compute base index so clicked snippet will be placed respecting the mouse Y.
  // We find the base_index (track index for clicked snippet) derived from mouse Y and click offset.
  float base_f = (io->MousePos.y - inner_pad - click_offset_into_visual - timeline_bb.Min.y) / stride;
  int base_index = (int)floorf(base_f + 0.5f);
  if (base_index < 0)
    base_index = 0;
  if (base_index >= ts->player_track_count)
    base_index = ts->player_track_count - 1;

  // For each selected snippet: keep original relative track offset to clicked snippet.
  for (int si = 0; si < ts->selected_snippet_count; ++si) {
    int sid = ts->selected_snippet_ids[si];
    input_snippet_t *s = NULL;
    int s_track_idx = -1;
    for (int ti = 0; ti < ts->player_track_count; ++ti) {
      s = find_snippet_by_id(&ts->player_tracks[ti], sid);
      if (s) {
        s_track_idx = ti;
        break;
      }
    }
    if (!s)
      continue;

    // keep relative track offset
    int rel_offset = s_track_idx - clicked_track_idx;
    int target_index = base_index + rel_offset;
    if (target_index < 0)
      target_index = 0;
    if (target_index >= ts->player_track_count)
      target_index = ts->player_track_count - 1;

    // compute preview ticks and screen Xs (snapping applied to clicked item only; group preserves relative
    // offsets)
    int duration = s->end_tick - s->start_tick;
    int preview_start = s->start_tick + delta_ticks;
    int preview_end = preview_start + duration;

    float preview_min_x = tick_to_screen_x(ts, preview_start, timeline_bb.Min.x);
    float preview_max_x = tick_to_screen_x(ts, preview_end, timeline_bb.Min.x);

    // compute vertical placement for this snippet using target_index
    float target_track_top = timeline_bb.Min.y + (float)target_index * stride;
    float preview_min_y = target_track_top + inner_pad;
    float preview_max_y = preview_min_y + snippet_h;

    // safety clamp (though clipping rect should handle this visually)
    if (preview_min_y < timeline_bb.Min.y + inner_pad)
      preview_min_y = timeline_bb.Min.y + inner_pad;
    if (preview_max_y > timeline_bb.Max.y - inner_pad)
      preview_max_y = timeline_bb.Max.y - inner_pad;

    ImVec2 preview_min = {preview_min_x, preview_min_y};
    ImVec2 preview_max = {preview_max_x, preview_max_y};

    bool overlaps = check_for_overlap(&ts->player_tracks[target_index], preview_start, preview_end, s->id);

    ImU32 fill = overlaps ? IM_COL32(200, 80, 80, 90) : IM_COL32(100, 150, 240, 90);
    ImDrawList_AddRectFilled(overlay_draw_list, preview_min, preview_max, fill, 4.0f,
                             ImDrawFlags_RoundCornersAll);
    ImDrawList_AddRect(overlay_draw_list, preview_min, preview_max,
                       igGetColorU32_Col(ImGuiCol_NavWindowingHighlight, overlaps ? 0.9f : 0.8f), 4.0f,
                       ImDrawFlags_RoundCornersAll, 1.5f);

    // label
    char label[64];
    snprintf(label, sizeof(label), "ID: %d", s->id);
    ImVec2 text_size;
    igCalcTextSize(&text_size, label, NULL, false, 0);
    ImVec2 text_pos = {(preview_min.x + preview_max.x) * 0.5f - text_size.x * 0.5f,
                       (preview_min.y + preview_max.y) * 0.5f - text_size.y * 0.5f};
    if (preview_max.x - preview_min.x > text_size.x + 4.0f) {
      ImDrawList_AddText_Vec2(overlay_draw_list, text_pos, igGetColorU32_Col(ImGuiCol_Text, 1.0f), label,
                              NULL);
    }
  }

  ImDrawList_PopClipRect(overlay_draw_list); // Don't forget to pop the clip rect!
}

// main render function
// main render function
void render_timeline(timeline_state_t *ts) {
  ImGuiIO *io = igGetIO_Nil();
  ts->playback_speed = ts->gui_playback_speed;
  // Ensure global shortcuts are handled even when the right-click popup isn't open
  process_global_shortcuts(ts);

  if (ts->recording && igIsKeyPressed_Bool(ImGuiKey_F, false)) {
    if (ts->recording_snippet && ts->current_tick >= ts->recording_snippet->start_tick) {
      resize_snippet_inputs(ts, ts->recording_snippet,
                            (ts->current_tick - ts->recording_snippet->start_tick) + 1);
      ts->recording_snippet->end_tick = ts->current_tick;
      if (ts->recording_snippet->input_count > 0)
        ts->recording_snippet->inputs[ts->recording_snippet->input_count - 1] = ts->recording_input;
    }
  }
  bool reverse = igIsKeyDown_Nil(ImGuiKey_C);
  if (reverse)
    ts->playback_speed *= 2.f;

  if (igIsKeyPressed_Bool(ImGuiKey_C, false)) {
    ts->is_playing = 0;
    ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
  }

  if (igIsKeyPressed_Bool(ImGuiKey_X, ImGuiInputFlags_Repeat)) {
    ts->is_playing ^= 1;
    if (ts->is_playing) {
      ts->last_update_time = igGetTime() - (1.f / ts->playback_speed);
    }
  }

  if ((ts->is_playing || reverse) && ts->playback_speed > 0.0f) {
    double current_time = igGetTime();
    double elapsed_time = current_time - ts->last_update_time;
    double tick_interval = 1.0 / ((double)ts->playback_speed); // Time per tick in seconds

    bool record = true;
    // make sure to reverse all the way no matter the step size here

    // don't record behind recording input snippet
    if (reverse && ts->recording && ts->current_tick < ts->recording_snippet->start_tick)
      record = false;
    if (ts->recording && check_for_overlap(&ts->player_tracks[ts->selected_player_track_index],
                                           ts->current_tick, ts->current_tick + 1, ts->recording_snippet->id))
      record = false;

    // Accumulate elapsed time and advance ticks as needed
    if (record)
      while (elapsed_time >= tick_interval) {
        advance_tick(ts, reverse ? -1 : 1);
        elapsed_time -= tick_interval;
        ts->last_update_time = current_time - elapsed_time;
      }
    else
      ts->last_update_time = current_time;
  }

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 4.0f);

  if (igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());
    igPopStyleVar(2);

    // Controls
    render_timeline_controls(ts);

    // Layout Calculations for Header and Timeline Area
    float header_height = igGetTextLineHeightWithSpacing() + 15;

    // Calculate the available space below the controls for the header and tracks
    ImVec2 available_space_below_controls;
    igGetContentRegionAvail(&available_space_below_controls);

    // Define the bounding box for the Header area (vertical space where ticks/labels go)
    ImVec2 header_bb_min;
    igGetCursorScreenPos(&header_bb_min); // Cursor after controls
    ImVec2 header_bb_max = {header_bb_min.x + available_space_below_controls.x,
                            header_bb_min.y + header_height};
    ImRect header_bb = {header_bb_min, header_bb_max};

    // Handle Mouse Interaction on Header
    bool is_header_hovered = !(io->ConfigFlags & ImGuiConfigFlags_NoMouse) &&
                             igIsMouseHoveringRect(header_bb.Min, header_bb.Max, true);

    // Start header drag: If header is hovered AND left mouse button is initially clicked
    if (is_header_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, 0)) {
      ts->is_header_dragging = true;
      int mouse_tick = screen_x_to_tick(ts, io->MousePos.x, header_bb.Min.x);
      ts->current_tick = fmax(0, mouse_tick); // Clamp
    }

    // Handle header drag: If header dragging is active AND left mouse button is held down
    if (ts->is_header_dragging && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      float mouse_screen_x = io->MousePos.x;
      int mouse_tick = screen_x_to_tick(ts, mouse_screen_x, header_bb.Min.x);
      ts->current_tick = mouse_tick;
      if (ts->current_tick < 0) {
        ts->current_tick = 0;
      }
    }

    // End header drag: If header dragging is active AND left mouse button is released
    if (ts->is_header_dragging && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
      ts->is_header_dragging = false;
    }

    // Draw Header (Ticks)
    draw_timeline_header(ts, draw_list, header_bb, header_bb_min.y);

    // Move the cursor down by the height of the header to position for the tracks
    igDummy((ImVec2){available_space_below_controls.x, header_height});

    // Calculate the bounding box for the Timeline Tracks area (below the header)
    ImVec2 timeline_start_pos;
    igGetCursorScreenPos(&timeline_start_pos); // Cursor after header
    ImVec2 available_space_for_tracks;
    igGetContentRegionAvail(&available_space_for_tracks);
    // Reserve space for the horizontal scrollbar
    float scrollbar_height = igGetStyle()->ScrollbarSize;
    available_space_for_tracks.y -= scrollbar_height;

    ImVec2 timeline_end_pos = (ImVec2){timeline_start_pos.x + available_space_for_tracks.x,
                                       timeline_start_pos.y + available_space_for_tracks.y};
    ImRect timeline_bb = (ImRect){timeline_start_pos, timeline_end_pos};

    // Ensure timeline_bb has positive dimensions
    if (timeline_bb.Max.x > timeline_bb.Min.x && timeline_bb.Max.y > timeline_bb.Min.y) {

      // Handle Pan/Zoom on the entire timeline area before drawing the child window
      if (!ts->is_header_dragging) {
        handle_timeline_interaction(ts, timeline_bb);
      }

      // Position and begin a child window. This provides the vertical scrollbar.
      igSetCursorScreenPos(timeline_bb.Min);
      ImVec2 child_size = {timeline_bb.Max.x - timeline_bb.Min.x, timeline_bb.Max.y - timeline_bb.Min.y};
      igBeginChild_Str("TracksArea", child_size, false, ImGuiWindowFlags_None);

      // We'll use the parent window's draw list so rendering is layered correctly.
      ImDrawList *draw_list_for_tracks = igGetWindowDrawList();

      float tracks_area_scroll_y = igGetScrollY();

      // Use ImGuiListClipper for high-performance scrolling of many tracks
      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, ts->player_track_count, ts->track_height);
      while (ImGuiListClipper_Step(clipper)) {
        for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
          // Manually set the cursor position for each track. This is crucial for
          // ensuring that mouse interactions (clicks, popups) work correctly.
          igSetCursorPosY(i * ts->track_height);

          // Get the absolute screen position for where the track will be drawn
          ImVec2 track_screen_pos;
          igGetCursorScreenPos(&track_screen_pos);

          ImVec2 avail;
          igGetContentRegionAvail(&avail);
          igDummy((ImVec2){avail.x, ts->track_height});

          player_track_t *track = &ts->player_tracks[i];
          float track_top = track_screen_pos.y;
          float track_bottom = track_top + ts->track_height;

          igPushID_Int(i);
          // Render the track. The outer `timeline_bb` is passed for correct
          // horizontal (tick-to-pixel) calculations.
          render_player_track(ts, i, track, draw_list_for_tracks, timeline_bb, track_top, track_bottom);
          igPopID();
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
      igEndChild(); // End the scrollable tracks area

      // Draw Horizontal Scrollbar
      ImS64 max_tick = get_max_timeline_tick(ts);
      float timeline_width = timeline_bb.Max.x - timeline_bb.Min.x;
      ImS64 visible_ticks = (ImS64)(timeline_width / ts->zoom);

      if (ts->is_playing) {
        ImS64 view_end_tick = (ImS64)ts->view_start_tick + visible_ticks;
        if ((ImS64)ts->current_tick < (ImS64)ts->view_start_tick || (ImS64)ts->current_tick > view_end_tick) {
          ts->view_start_tick = ts->current_tick - (int)(visible_ticks);
        }
      }

      max_tick = (ImS64)(max_tick * 1.4f);
      if (max_tick < 100)
        max_tick = 100;

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

      // Handle Mouse Release for Snippet Drag & Drop
      if (ts->drag_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left) && !ts->is_header_dragging) {
        ImVec2 mouse_pos = io->MousePos;
        player_track_t *source_track = &ts->player_tracks[ts->drag_state.source_track_index];
        input_snippet_t *clicked_snippet =
            find_snippet_by_id(source_track, ts->drag_state.dragged_snippet_id);
        if (!clicked_snippet) {
          printf("Error: Dragged snippet ID %d not found in expected source track %d on mouse release!\n",
                 ts->drag_state.dragged_snippet_id, ts->drag_state.source_track_index);
          ts->drag_state.active = false;
          ts->drag_state.source_track_index = -1;
          ts->drag_state.source_snippet_index = -1;
          ts->drag_state.dragged_snippet_id = -1;
          return;
        }
        int clicked_duration = clicked_snippet->end_tick - clicked_snippet->start_tick;
        int mouse_tick_at_release = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
        int desired_drop_tick_clicked = mouse_tick_at_release - ts->drag_state.drag_offset_ticks;
        int final_drop_tick_clicked = calculate_snapped_tick(ts, desired_drop_tick_clicked, clicked_duration,
                                                             ts->drag_state.dragged_snippet_id);

        // Correct target track calculation needs to account for vertical scroll
        float track_y_at_release_in_area = mouse_pos.y - timeline_bb.Min.y;
        float scrolled_content_y = track_y_at_release_in_area + tracks_area_scroll_y;
        int target_track_idx = (int)(scrolled_content_y / ts->track_height);

        if (target_track_idx < 0)
          target_track_idx = 0;
        if (target_track_idx >= ts->player_track_count)
          target_track_idx = ts->player_track_count - 1;

        int source_base_track_idx = ts->drag_state.source_track_index;
        int track_delta = target_track_idx - source_base_track_idx;
        int tick_delta = final_drop_tick_clicked - clicked_snippet->start_tick;

        if (ts->selected_snippet_count > 0) {
          int originals[256];
          int cnt = ts->selected_snippet_count;
          for (int i = 0; i < cnt; ++i)
            originals[i] = ts->selected_snippet_ids[i];
          for (int i = 0; i < cnt; ++i) {
            int sid = originals[i];
            input_snippet_t *s = NULL;
            int s_track_idx = -1;
            for (int ti = 0; ti < ts->player_track_count; ++ti) {
              s = find_snippet_by_id(&ts->player_tracks[ti], sid);
              if (s) {
                s_track_idx = ti;
                break;
              }
            }
            if (!s)
              continue;
            int new_track_idx = s_track_idx + track_delta;
            if (new_track_idx < 0)
              new_track_idx = 0;
            if (new_track_idx >= ts->player_track_count)
              new_track_idx = ts->player_track_count - 1;
            int new_start_tick = s->start_tick + tick_delta;
            try_move_snippet(ts, sid, s_track_idx, new_track_idx, new_start_tick);
          }
        } else {
          try_move_snippet(ts, ts->drag_state.dragged_snippet_id, ts->drag_state.source_track_index,
                           target_track_idx, final_drop_tick_clicked);
        }
        ts->drag_state.active = false;
        ts->drag_state.source_track_index = -1;
        ts->drag_state.source_snippet_index = -1;
        ts->drag_state.dragged_snippet_id = -1;
      }

      // Draw Playhead
      draw_playhead(ts, overlay_draw_list, timeline_bb, timeline_bb.Min.y - 12);
    }

    // Draw Drag Preview (on overlay)
    draw_drag_preview(ts, overlay_draw_list, timeline_bb);
  }
  igEnd(); // End Timeline window
}

// helper to add a new empty track(s)
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

  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * new_count);

  // Initialize each new track slot
  for (int i = 0; i < num; i++) {
    player_track_t *new_track = &ts->player_tracks[old_count + i];
    new_track->snippets = NULL;
    new_track->snippet_count = 0;

    // Reset player info
    memset(new_track->player_info.name, 0, sizeof(new_track->player_info.name));
    memset(new_track->player_info.clan, 0, sizeof(new_track->player_info.clan));
    new_track->player_info.skin = 0;
    new_track->player_info.color_feet = 0;
    new_track->player_info.color_body = 0;
    new_track->player_info.use_custom_color = 0;
  }

  ts->player_track_count = new_count;

  // update size so it recalculates
  ts->vec.current_size = 1;

  // Update timeline copies with the new world
  wc_copy_world(&ts->vec.data[0], &ph->world);
  wc_copy_world(&ts->previous_world, &ph->world);

  // Return a pointer to the first new track
  return &ts->player_tracks[old_count];
}

void timeline_init(timeline_state_t *ts) {
  timeline_cleanup(ts);
  memset(ts, 0, sizeof(timeline_state_t));

  v_init(&ts->vec);
  ts->previous_world = wc_empty();

  // Initialize Timeline State variables
  ts->gui_playback_speed = 50;
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
  ts->playback_speed = 50;
  ts->gui_playback_speed = 50;
  ts->last_update_time = 0.0;

  // reset drag state
  ts->drag_state.active = false;
  ts->drag_state.source_track_index = -1;
  ts->drag_state.source_snippet_index = -1;
  ts->drag_state.dragged_snippet_id = -1;
  ts->drag_state.drag_offset_ticks = 0;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  v_destroy(&ts->vec);
  wc_free(&ts->previous_world);
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
    for (int i = t->max_size / 2; i < t->max_size; ++i)
      t->data[i] = wc_empty();
  }
  wc_copy_world(&t->data[t->current_size - 1], world);
}

void v_destroy(physics_v_t *t) {
  for (int i = 0; i < t->max_size; ++i)
    wc_free(&t->data[i]);
  free(t->data);
  t->current_size = 0;
  t->max_size = 0;
}
