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

static void recording_snippet_vector_init(recording_snippet_vector_t *vec) {
  vec->snippets = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

// Note: This only frees the array of pointers, not the snippets themselves,
// as they are owned by the tracks.
static void recording_snippet_vector_free(recording_snippet_vector_t *vec) {
  if (vec->snippets) {
    free(vec->snippets);
  }
  recording_snippet_vector_init(vec);
}

static void recording_snippet_vector_add(recording_snippet_vector_t *vec, input_snippet_t *snippet) {
  if (vec->count >= vec->capacity) {
    int new_capacity = vec->capacity == 0 ? 4 : vec->capacity * 2;
    input_snippet_t **new_snippets = realloc(vec->snippets, sizeof(input_snippet_t *) * new_capacity);
    if (!new_snippets)
      return;
    vec->snippets = new_snippets;
    vec->capacity = new_capacity;
  }
  vec->snippets[vec->count++] = snippet;
}

static void recording_snippet_vector_clear(recording_snippet_vector_t *vec) { vec->count = 0; }

// Initializes or resets a vector to a clean state.
static void snippet_id_vector_init(snippet_id_vector_t *vec) {
  vec->ids = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

// Frees the memory used by the vector.
static void snippet_id_vector_free(snippet_id_vector_t *vec) {
  if (vec->ids) {
    free(vec->ids);
  }
  // Reset to initial state
  snippet_id_vector_init(vec);
}

// Adds an ID to the vector, resizing if necessary.
static void snippet_id_vector_add(snippet_id_vector_t *vec, int snippet_id) {
  // Grow the vector if it's full
  if (vec->count >= vec->capacity) {
    int new_capacity = vec->capacity == 0 ? 8 : vec->capacity * 2; // Start with 8, then double
    int *new_ids = realloc(vec->ids, sizeof(int) * new_capacity);
    if (!new_ids) {
      // Handle allocation failure if necessary (e.g., log an error)
      return;
    }
    vec->ids = new_ids;
    vec->capacity = new_capacity;
  }
  // Add the new ID
  vec->ids[vec->count++] = snippet_id;
}

// Removes an ID from the vector. Returns true if found and removed.
static bool snippet_id_vector_remove(snippet_id_vector_t *vec, int snippet_id) {
  int pos = -1;
  for (int i = 0; i < vec->count; ++i) {
    if (vec->ids[i] == snippet_id) {
      pos = i;
      break;
    }
  }

  if (pos >= 0) {
    // Shift elements to fill the gap
    if (pos < vec->count - 1) {
      memmove(&vec->ids[pos], &vec->ids[pos + 1], (vec->count - pos - 1) * sizeof(int));
    }
    vec->count--;
    return true;
  }
  return false;
}

// Checks if an ID exists in the vector.
static bool snippet_id_vector_contains(const snippet_id_vector_t *vec, int snippet_id) {
  for (int i = 0; i < vec->count; ++i) {
    if (vec->ids[i] == snippet_id) {
      return true;
    }
  }
  return false;
}

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

  track->snippets[track->snippet_count] = *snippet;
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

static bool is_snippet_selected(timeline_state_t *ts, int snippet_id) {
  return snippet_id_vector_contains(&ts->selected_snippets, snippet_id);
}

static void clear_selection(timeline_state_t *ts) {
  ts->selected_snippets.count = 0; // Much simpler!
  ts->selected_snippet_id = -1;
  ts->selected_player_track_index = -1;
}

static void add_snippet_to_selection(timeline_state_t *ts, int snippet_id, int track_index) {
  if (snippet_id < 0)
    return;
  if (is_snippet_selected(ts, snippet_id))
    return;

  snippet_id_vector_add(&ts->selected_snippets, snippet_id);
  ts->selected_snippet_id = snippet_id;
  ts->selected_player_track_index = track_index;
}

static void remove_snippet_from_selection(timeline_state_t *ts, int snippet_id) {
  if (snippet_id_vector_remove(&ts->selected_snippets, snippet_id)) {
    if (ts->selected_snippets.count == 0) {
      ts->selected_snippet_id = -1;
      ts->selected_player_track_index = -1;
    } else {
      // Update selected_snippet_id to the last item in the list
      ts->selected_snippet_id = ts->selected_snippets.ids[ts->selected_snippets.count - 1];
    }
  }
}

static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb, float scroll_y) {
  float rect_min_x = fminf(rect.Min.x, rect.Max.x);
  float rect_max_x = fmaxf(rect.Min.x, rect.Max.x);

  int rect_start_tick = screen_x_to_tick(ts, rect_min_x, timeline_bb.Min.x);
  int rect_end_tick = screen_x_to_tick(ts, rect_max_x, timeline_bb.Min.x);

  // Ensure min/max ordering for tick bounds
  int start_tick = imin(rect_start_tick, rect_end_tick);
  int end_tick = imax(rect_start_tick, rect_end_tick);

  // Convert selection rect Y from screen space to content space
  float content_rect_min_y = rect.Min.y - timeline_bb.Min.y + scroll_y;
  float content_rect_max_y = rect.Max.y - timeline_bb.Min.y + scroll_y;

  clear_selection(ts);
  for (int ti = 0; ti < ts->player_track_count; ++ti) {
    player_track_t *track = &ts->player_tracks[ti];

    // Calculate track bounds in content space
    float track_top = (float)ti * ts->track_height;
    float track_bottom = track_top + ts->track_height;

    // Vertical check in CONTENT SPACE: Does the track overlap with the selection rect?
    bool track_is_selected_y = (track_top < content_rect_max_y && track_bottom > content_rect_min_y);

    if (track_is_selected_y) {
      for (int si = 0; si < track->snippet_count; ++si) {
        input_snippet_t *snip = &track->snippets[si];
        // Horizontal check in TICK SPACE
        bool snippet_is_selected_x = (snip->start_tick < end_tick && snip->end_tick > start_tick);
        if (snippet_is_selected_x) {
          add_snippet_to_selection(ts, snip->id, ti);
        }
      }
    }
  }
}

/* comparator for qsort: pointers to input_snippet_t* */
static int compare_snippets_by_start_tick(const void *p1, const void *p2) {
  const input_snippet_t *const *a = (const input_snippet_t *const *)p1;
  const input_snippet_t *const *b = (const input_snippet_t *const *)p2;
  if ((*a)->start_tick < (*b)->start_tick)
    return -1;
  if ((*a)->start_tick > (*b)->start_tick)
    return 1;
  return 0;
}

void do_merge_selected_snippets(timeline_state_t *ts) {
  if (ts->selected_snippets.count < 2)
    return;

  bool merged_something = false;
  int earliest_tick = INT_MAX;

  for (int ti = 0; ti < ts->player_track_count; ++ti) {
    player_track_t *track = &ts->player_tracks[ti];

    /* First pass: count selected snippets on this track */
    int candidate_count = 0;
    for (int i = 0; i < track->snippet_count; ++i) {
      if (is_snippet_selected(ts, track->snippets[i].id)) {
        ++candidate_count;
      }
    }
    if (candidate_count < 2)
      continue;

    /* Allocate exact-size array for candidates */
    input_snippet_t **candidates = malloc(sizeof(*candidates) * candidate_count);
    if (!candidates) {
      /* allocation failed; skip this track */
      continue;
    }

    /* Fill candidates */
    int idx = 0;
    for (int i = 0; i < track->snippet_count; ++i) {
      if (is_snippet_selected(ts, track->snippets[i].id)) {
        candidates[idx++] = &track->snippets[i];
      }
    }

    /* sort by start tick */
    qsort(candidates, candidate_count, sizeof(input_snippet_t *), compare_snippets_by_start_tick);

    /* ids to remove: allocate exactly candidate_count (worst-case) */
    int *ids_to_remove = malloc(sizeof(*ids_to_remove) * candidate_count);
    if (!ids_to_remove) {
      free(candidates);
      continue;
    }
    int remove_count = 0;

    /* Iterate and merge adjacent pairs */
    for (int i = 0; i < candidate_count - 1; ++i) {
      input_snippet_t *a = candidates[i];
      input_snippet_t *b = candidates[i + 1];

      if (a->end_tick == b->start_tick) { /* adjacent */
        /* Attempt to allocate new buffer for A safely */
        int old_a_duration = a->input_count;
        int b_duration = b->input_count;
        int new_duration = old_a_duration + b_duration;

        /* safe realloc pattern */
        SPlayerInput *tmp = realloc(a->inputs, sizeof(SPlayerInput) * (size_t)new_duration);
        if (!tmp && new_duration > 0) {
          /* allocation failed: skip this particular merge (don't corrupt state) */
          /* You might want to log this error. */
          continue;
        }
        a->inputs = tmp; /* tmp may be NULL only if new_duration == 0, which is OK */
        /* copy B's inputs (if any) */
        if (b_duration > 0 && b->inputs != NULL) {
          memcpy(&a->inputs[old_a_duration], b->inputs, sizeof(SPlayerInput) * (size_t)b_duration);
          /* free B's buffer (we copied contents) and null it so remove won't double-free */
          free(b->inputs);
          b->inputs = NULL;
        }
        /* update A's metadata */
        a->end_tick = b->end_tick;
        a->input_count = new_duration;
        if (a->start_tick < earliest_tick)
          earliest_tick = a->start_tick;

        /* mark B for removal */
        ids_to_remove[remove_count++] = b->id;

        /* ensure b no longer claims inputs */
        b->input_count = 0;

        /* chain merges: make next "previous" be the merged 'a' */
        candidates[i + 1] = a;

        merged_something = true;
      }
    }

    /* remove all merged snippets for this track */
    if (remove_count > 0) {
      for (int i = 0; i < remove_count; ++i) {
        remove_snippet_from_track(ts, track, ids_to_remove[i]);
      }
    }

    free(ids_to_remove);
    free(candidates);
  }

  if (merged_something) {
    clear_selection(ts);
    if (earliest_tick != INT_MAX)
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

void do_add_snippet(timeline_state_t *ts) {
  // Add snippet at current tick to selected track (or first track)
  int track_idx = ts->selected_player_track_index >= 0 ? ts->selected_player_track_index
                                                       : (ts->player_track_count > 0 ? 0 : -1);
  if (track_idx >= 0) {
    input_snippet_t snip = create_empty_snippet(ts, ts->current_tick, 50);
    add_snippet_to_track(&ts->player_tracks[track_idx], &snip);
  }
}

void do_split_selected_snippets(timeline_state_t *ts) {
  if (ts->selected_snippets.count > 0) {
    int cnt = ts->selected_snippets.count;
    if (cnt <= 0)
      return;

    // Dynamically allocate a copy of the original IDs
    int *originals = malloc(sizeof(int) * cnt);
    if (!originals)
      return; // Allocation failed
    memcpy(originals, ts->selected_snippets.ids, sizeof(int) * cnt);

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
            int offset = split_tick - snippet->start_tick;
            int right_count = old_end - split_tick;

            // Manually create the 'right' snippet to avoid the double allocation that was causing the leak.
            input_snippet_t right;
            right.id = ts->next_snippet_id++;
            right.start_tick = split_tick;
            right.end_tick = old_end;
            right.inputs = NULL;
            right.input_count = 0;

            if (right_count > 0) {
              right.inputs = malloc(sizeof(SPlayerInput) * right_count);
              memcpy(right.inputs, snippet->inputs + offset, right_count * sizeof(SPlayerInput));
              right.input_count = right_count;
            }
            resize_snippet_inputs(ts, snippet, offset);
            snippet->end_tick = split_tick;
            add_snippet_to_track(track, &right);
            snippet_id_vector_add(&ts->selected_snippets, right.id);
          }
        }
      }
    }
    free(originals);
  }
}

void do_delete_selected_snippets(timeline_state_t *ts) {
  int cnt = ts->selected_snippets.count;
  if (cnt <= 0)
    return;

  int *originals = malloc(sizeof(int) * cnt);
  if (!originals)
    return;
  memcpy(originals, ts->selected_snippets.ids, sizeof(int) * cnt);

  for (int i = 0; i < cnt; ++i) {
    int sid = originals[i];
    for (int ti = 0; ti < ts->player_track_count; ++ti) {
      player_track_t *track = &ts->player_tracks[ti];
      remove_snippet_from_track(ts, track, sid);
    }
  }
  clear_selection(ts);
  free(originals);
}

// Global keyboard shortcuts (moved out of popup so they always work)
static void process_global_shortcuts(timeline_state_t *ts) {
  ImGuiIO *io = igGetIO_Nil();
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_A, true)) {
    do_add_snippet(ts);
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_R, true)) {
    do_split_selected_snippets(ts);
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_D, true)) {
    do_delete_selected_snippets(ts);
  }
  if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_M, true)) {
    do_merge_selected_snippets(ts);
  }
}

void timeline_update_inputs(timeline_state_t *ts, gfx_handler_t *gfx) {
  if (!ts->recording || ts->recording_snippets.count == 0)
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

  // If we are recording, extend ALL targeted snippets.
  if (ts->recording && ts->recording_snippets.count > 0) {
    for (int i = 0; i < ts->recording_snippets.count; i++) {
      input_snippet_t *snippet = ts->recording_snippets.snippets[i];

      // Ensure we don't record backwards over the start of a snippet
      if (ts->current_tick < snippet->start_tick) {
        // Prevent playhead from moving before the recording start
        ts->current_tick = snippet->start_tick;
        continue;
      }

      // If playhead is past the end, extend the snippet
      if (ts->current_tick > snippet->end_tick) {
        resize_snippet_inputs(ts, snippet, ts->current_tick - snippet->start_tick);
        if (snippet->input_count > 0) {
          // All extended snippets get the same input for this tick
          snippet->inputs[snippet->input_count - 1] = ts->recording_input;
        }
      }
    }
  }
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
                      int desired_start_tick, bool dry_run) {
  if (source_track_idx < 0 || source_track_idx >= ts->player_track_count || target_track_idx < 0 ||
      target_track_idx >= ts->player_track_count) {
    return false; // Invalid track indices
  }

  player_track_t *source_track = &ts->player_tracks[source_track_idx];
  player_track_t *target_track = &ts->player_tracks[target_track_idx];

  // Find the original snippet in the source track
  int snippet_idx_in_source = -1;
  input_snippet_t *original_snippet = NULL;
  for (int i = 0; i < source_track->snippet_count; i++) {
    if (source_track->snippets[i].id == snippet_id) {
      snippet_idx_in_source = i;
      original_snippet = &source_track->snippets[i];
      break;
    }
  }

  if (snippet_idx_in_source == -1 || !original_snippet) {
    return false; // Snippet not found
  }

  bool is_duplicating = igGetIO_Nil()->KeyAlt;
  int duration = original_snippet->end_tick - original_snippet->start_tick;
  int old_start_tick = original_snippet->start_tick;
  int new_start_tick = desired_start_tick;

  // Ensure new position is not before tick 0
  if (new_start_tick < 0)
    new_start_tick = 0;
  int new_end_tick = new_start_tick + duration;

  // Check for overlaps in the target track at the *proposed* new position
  for (int i = 0; i < target_track->snippet_count; ++i) {
    input_snippet_t *other = &target_track->snippets[i];

    // If MOVING, we can ignore collision with any part of the selection, as it's all moving together.
    // If DUPLICATING, the originals are obstacles, so we must check collision against them.
    if (!is_duplicating && is_snippet_selected(ts, other->id)) {
      continue;
    }

    // Check for overlap
    if (new_start_tick < other->end_tick && new_end_tick > other->start_tick) {
      return false; // Overlap detected
    }
  }

  // If this is a dry run, we've succeeded if we reached here without detecting an overlap.
  if (dry_run) {
    return true;
  }

  if (is_duplicating) {
    // DUPLICATION LOGIC
    input_snippet_t new_snip;
    // Start with a struct copy, then modify what's needed
    new_snip = *original_snippet;
    new_snip.id = ts->next_snippet_id++;
    new_snip.start_tick = new_start_tick;
    new_snip.end_tick = new_end_tick;

    // Deep copy the inputs array
    new_snip.inputs = NULL;
    new_snip.input_count = 0;
    copy_snippet_inputs(&new_snip, original_snippet);

    add_snippet_to_track(target_track, &new_snip);
  } else {
    // MOVE LOGIC
    if (source_track_idx == target_track_idx) {
      // Move within the same track: update in place
      original_snippet->start_tick = new_start_tick;
      original_snippet->end_tick = new_end_tick;
      recalc_ts(ts, imin(new_start_tick, old_start_tick));
    } else {
      // Move to a different track: add to target, remove from source (transfer ownership of inputs)
      input_snippet_t moved_snippet_data = *original_snippet;
      moved_snippet_data.start_tick = new_start_tick;
      moved_snippet_data.end_tick = new_end_tick;
      add_snippet_to_track(target_track, &moved_snippet_data);

      // To prevent free_snippet_inputs, null out the pointer in the original struct before removing
      original_snippet->inputs = NULL;
      original_snippet->input_count = 0;
      remove_snippet_from_track(ts, source_track, snippet_id);
    }
    // Update selection state to follow the moved item
    ts->selected_snippet_id = snippet_id;
    ts->selected_player_track_index = target_track_idx;
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

  // In render_timeline_controls()

  igSameLine(0, 20);
  bool was_recording = ts->recording;
  if (igButton(ts->recording ? "Stop Recording" : "Record", (ImVec2){0, 0})) {
    ts->recording = !ts->recording;

    // If we just STOPPED recording, handle cleanup.
    if (!ts->recording && ts->recording_snippets.count > 0) {
      // Create a temporary copy of pointers because removing snippets can invalidate the original array
      input_snippet_t **temps = malloc(ts->recording_snippets.count * sizeof(input_snippet_t *));
      if (temps) {
        memcpy(temps, ts->recording_snippets.snippets,
               ts->recording_snippets.count * sizeof(input_snippet_t *));
        int temp_count = ts->recording_snippets.count;

        for (int i = 0; i < temp_count; i++) {
          input_snippet_t *snippet = temps[i];
          if (snippet->input_count == 0) {
            // Find and remove the empty snippet
            for (int ti = 0; ti < ts->player_track_count; ++ti) {
              if (remove_snippet_from_track(ts, &ts->player_tracks[ti], snippet->id)) {
                break;
              }
            }
          }
        }
        free(temps);
      }
      recording_snippet_vector_clear(&ts->recording_snippets);
    }
  }

  // If we just STARTED recording...
  if (!was_recording && ts->recording) {
    recording_snippet_vector_clear(&ts->recording_snippets);
    bool can_record = false;

    if (ts->selected_snippets.count == 0) {
      // CASE 1: CREATE a new snippet
      int track_idx = ts->player_track_count > 0 ? 0 : -1; // Default to track 0
      if (track_idx != -1) {
        player_track_t *track = &ts->player_tracks[track_idx];
        if (!check_for_overlap(track, ts->current_tick, ts->current_tick + 1, -1)) {
          input_snippet_t new_snip = create_empty_snippet(ts, ts->current_tick, 1);
          add_snippet_to_track(track, &new_snip);
          // Add the new snippet to the recording list
          recording_snippet_vector_add(&ts->recording_snippets, &track->snippets[track->snippet_count - 1]);
          can_record = true;
        }
      }
    } else {
      // CASE 2: EXTEND selected snippet(s)
      int reference_end_tick = -1;
      bool all_share_end_tick = true;
      input_snippet_t **candidates = malloc(ts->selected_snippets.count * sizeof(input_snippet_t *));
      int candidate_count = 0;

      if (candidates) {
        // First pass: Find all snippets and check if they share an end_tick
        for (int i = 0; i < ts->selected_snippets.count; i++) {
          int sid = ts->selected_snippets.ids[i];
          for (int ti = 0; ti < ts->player_track_count; ti++) {
            input_snippet_t *s = find_snippet_by_id(&ts->player_tracks[ti], sid);
            if (s) {
              if (reference_end_tick == -1)
                reference_end_tick = s->end_tick;
              if (s->end_tick != reference_end_tick) {
                all_share_end_tick = false;
              }
              candidates[candidate_count++] = s;
              break;
            }
          }
          if (!all_share_end_tick)
            break;
        }

        if (all_share_end_tick && candidate_count > 0) {
          // Second pass: Check for overlaps for all candidates
          bool any_overlap = false;
          ts->current_tick = reference_end_tick; // Move playhead to the end
          for (int i = 0; i < candidate_count; i++) {
            for (int ti = 0; ti < ts->player_track_count; ti++) {
              if (find_snippet_by_id(&ts->player_tracks[ti], candidates[i]->id)) {
                if (check_for_overlap(&ts->player_tracks[ti], ts->current_tick, ts->current_tick + 1,
                                      candidates[i]->id)) {
                  any_overlap = true;
                }
                break;
              }
            }
            if (any_overlap)
              break;
          }

          if (!any_overlap) {
            // All good! Add all candidates to the final recording list
            for (int i = 0; i < candidate_count; i++) {
              recording_snippet_vector_add(&ts->recording_snippets, candidates[i]);
            }
            can_record = true;
          }
        }
        free(candidates);
      }
    }

    // If any check failed, abort the recording
    if (!can_record) {
      ts->recording = false;
      recording_snippet_vector_clear(&ts->recording_snippets);
    }
  }

  if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
    ts->recording = false;
    recording_snippet_vector_clear(&ts->recording_snippets);
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

  if (io->ConfigFlags & ImGuiConfigFlags_NoMouse)
    return;
  if (ts->recording)
    return;

  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  // Zoom with mouse wheel
  if (is_timeline_hovered && io->MouseWheel != 0) {
    int mouse_tick_before_zoom = screen_x_to_tick(ts, mouse_pos.x, timeline_bb.Min.x);
    float zoom_delta = io->KeyCtrl * io->MouseWheel * 0.1f * ts->zoom; // Scale zoom delta by current zoom
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
  bool is_item_hovered = false;
  if (!ts->recording) {
    // Invisible button to capture mouse interaction for this snippet
    igSetCursorScreenPos((ImVec2){snippet_start_x, track_top + 2.0f});
    char _snippet_id_buf[64];
    snprintf(_snippet_id_buf, sizeof(_snippet_id_buf), "snippet_%d_%d", track_index, snippet->id);
    is_item_hovered = igInvisibleButton(
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
      ts->drag_state.drag_offset_y = io->MousePos.y - (track_top + 2.0f);
      ts->drag_state.dragged_snippet_id = snippet->id;

      // If clicked snippet wasn't selected, make sure it's selected for the drag
      if (!is_snippet_selected(ts, snippet->id)) {
        clear_selection(ts);
        add_snippet_to_selection(ts, snippet->id, track_index);
      }
    }
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
  if (!ts->recording && igIsMouseClicked_Bool(ImGuiMouseButton_Right, 0) &&
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

  if (!ts->recording && igIsMouseClicked_Bool(ImGuiMouseButton_Right, 0) &&
      igIsMouseHoveringRect((ImVec2){timeline_bb.Min.x, track_top}, (ImVec2){timeline_bb.Max.x, track_bottom},
                            true)) {
    igOpenPopup_Str("RightClickMenu", 0);
    ts->selected_player_track_index = track_index;
  }

  if (!ts->recording && igBeginPopup("RightClickMenu", ImGuiPopupFlags_AnyPopupLevel)) {
    if (igMenuItem_Bool("Add Snippet", "Ctrl+a", false, true)) {
      do_add_snippet(ts);
    }

    if (igMenuItem_Bool("Merge Snippets", "Ctrl+m", false, ts->selected_snippets.count > 1)) {
      do_merge_selected_snippets(ts);
    }

    if (igMenuItem_Bool("Split Snippet", "Ctrl+r", false, ts->selected_snippet_id != -1)) {
      do_split_selected_snippets(ts);
    }

    if (igMenuItem_Bool("Delete Snippet", "Ctrl+d", false, ts->selected_snippets.count > 0)) {
      do_delete_selected_snippets(ts);
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
void draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb,
                       float tracks_area_scroll_y) {
  ImGuiIO *io = igGetIO_Nil();
  if (!ts->drag_state.active)
    return;

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

  const float inner_pad = 2.0f;

  const float stride = ts->track_height;

  // Calculate the target track index for the primary dragged snippet based on its desired visual top position
  float clicked_snippet_preview_visual_top_y = io->MousePos.y - ts->drag_state.drag_offset_y;
  float content_y = clicked_snippet_preview_visual_top_y - timeline_bb.Min.y + tracks_area_scroll_y;
  int base_index = (int)floorf(((content_y - inner_pad) / stride) + 0.5f);

  if (base_index < 0)
    base_index = 0;
  if (base_index >= ts->player_track_count)
    base_index = ts->player_track_count - 1;
  // For each selected snippet: keep original relative track offset to clicked snippet.
  for (int si = 0; si < ts->selected_snippets.count; ++si) {
    int sid = ts->selected_snippets.ids[si];
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
      continue;
    if (target_index >= ts->player_track_count)
      continue;

    // compute preview ticks and screen Xs (snapping applied to clicked item only; group preserves relative
    // offsets)
    int duration = s->end_tick - s->start_tick;
    int preview_start = s->start_tick + delta_ticks;
    int preview_end = preview_start + duration;

    float preview_min_x = tick_to_screen_x(ts, preview_start, timeline_bb.Min.x);
    float preview_max_x = tick_to_screen_x(ts, preview_end, timeline_bb.Min.x);

    // compute vertical placement for this snippet using target_index
    float target_track_content_top = (float)target_index * stride;
    float target_track_top = timeline_bb.Min.y + target_track_content_top - tracks_area_scroll_y;
    const float snippet_h = ts->track_height - inner_pad * 2.0f;
    float preview_min_y = target_track_top + inner_pad;
    float preview_max_y = preview_min_y + snippet_h;

    ImVec2 preview_min = {preview_min_x, preview_min_y};
    ImVec2 preview_max = {preview_max_x, preview_max_y};

    bool overlaps = false;
    player_track_t *target_track = &ts->player_tracks[target_index];
    for (int i = 0; i < target_track->snippet_count; ++i) {
      input_snippet_t *other = &target_track->snippets[i];
      // Ignore collision with any snippet that is part of the current selection.
      if (is_snippet_selected(ts, other->id)) {
        continue;
      }
      // Check for overlap with any non-selected snippet.
      if (preview_start < other->end_tick && preview_end > other->start_tick) {
        overlaps = true;
        break;
      }
    }

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
  if (!ts->recording)
    process_global_shortcuts(ts);

  if (ts->recording && igIsKeyPressed_Bool(ImGuiKey_F, false)) {
    for (int i = 0; i < ts->recording_snippets.count; i++) {
      input_snippet_t *snippet = ts->recording_snippets.snippets[i];
      if (ts->current_tick >= snippet->start_tick) {
        resize_snippet_inputs(ts, snippet, (ts->current_tick - snippet->start_tick) + 1);
        snippet->end_tick = ts->current_tick;
        if (snippet->input_count > 0)
          snippet->inputs[snippet->input_count - 1] = ts->recording_input;
      }
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

    // checks for multi-snippet recording
    if (ts->recording && ts->recording_snippets.count > 0) {
      if (reverse) {
        // Don't play in reverse behind ANY of the recording snippets' start points.
        for (int i = 0; i < ts->recording_snippets.count; i++) {
          if (ts->current_tick < ts->recording_snippets.snippets[i]->start_tick) {
            record = false;
            break;
          }
        }
      } else {
        // Don't record over any other snippets.
        bool overlap_found = false;
        // For each snippet we are actively recording...
        for (int i = 0; i < ts->recording_snippets.count; i++) {
          input_snippet_t *rec_snip = ts->recording_snippets.snippets[i];

          // Find its track
          player_track_t *parent_track = NULL;
          for (int ti = 0; ti < ts->player_track_count; ti++) {
            if (find_snippet_by_id(&ts->player_tracks[ti], rec_snip->id)) {
              parent_track = &ts->player_tracks[ti];
              break;
            }
          }

          if (parent_track) {
            // Check for collision on its track with any other snippet...
            for (int j = 0; j < parent_track->snippet_count; j++) {
              input_snippet_t *other = &parent_track->snippets[j];

              // that isn't also being recorded.
              bool is_also_recording = false;
              for (int k = 0; k < ts->recording_snippets.count; k++) {
                if (other->id == ts->recording_snippets.snippets[k]->id) {
                  is_also_recording = true;
                  break;
                }
              }
              if (is_also_recording)
                continue;

              // Now perform the overlap check at the current playhead position
              if (ts->current_tick < other->end_tick && (ts->current_tick + 1) > other->start_tick) {
                overlap_found = true;
                break;
              }
            }
          }
          if (overlap_found)
            break;
        }
        if (overlap_found) {
          record = false;
        }
      }
    }

    // Accumulate elapsed time and advance ticks as needed
    if (record)
      while (elapsed_time >= tick_interval) {
        advance_tick(ts, reverse ? -1 : 1);
        elapsed_time -= tick_interval;
        ts->last_update_time = current_time - elapsed_time;
      }
    else {
      ts->is_playing = 0;
      ts->last_update_time = current_time;
    }
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
    if (!ts->recording && is_header_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, 0)) {
      ts->is_header_dragging = true;
      int mouse_tick = screen_x_to_tick(ts, io->MousePos.x, header_bb.Min.x);
      ts->current_tick = fmax(0, mouse_tick); // Clamp
    }

    // Handle header drag: If header dragging is active AND left mouse button is held down
    if (!ts->recording && ts->is_header_dragging && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
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

      // Interaction logic that must happen AFTER items are submitted
      // Handle Mouse Click for Selection Box Start
      // This is done after the child window is drawn so `igIsAnyItemHovered()` is accurate.
      bool is_timeline_area_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);
      if (!ts->recording && igIsMouseClicked_Bool(ImGuiMouseButton_Left, 0) && is_timeline_area_hovered &&
          !igIsAnyItemHovered()) {
        ts->selection_box_active = true;
        ts->selection_box_start = io->MousePos;
        ts->selection_box_end = io->MousePos;
        if (!io->KeyShift) {
          clear_selection(ts);
        }
      }

      // Handle dragging and drawing for the selection box
      if (!ts->recording && ts->selection_box_active && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
        ts->selection_box_end = io->MousePos;
        ImVec2 a = ts->selection_box_start, b = ts->selection_box_end;
        ImRect rect = {{fminf(a.x, b.x), fminf(a.y, b.y)}, {fmaxf(a.x, b.x), fmaxf(a.y, b.y)}};
        ImDrawList_AddRectFilled(overlay_draw_list, rect.Min, rect.Max, IM_COL32(100, 150, 240, 80), 0.0f, 0);
        ImDrawList_AddRect(overlay_draw_list, rect.Min, rect.Max, IM_COL32(100, 150, 240, 180), 0.0f, 0,
                           1.0f);
      }

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

      // Handle Mouse Release for Selection Box
      if (ts->selection_box_active && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
        ImVec2 a = ts->selection_box_start;
        ImVec2 b = ts->selection_box_end;
        ImRect rect =
            (ImRect){.Min = {fminf(a.x, b.x), fminf(a.y, b.y)}, .Max = {fmaxf(a.x, b.x), fmaxf(a.y, b.y)}};
        select_snippets_in_rect(ts, rect, timeline_bb, tracks_area_scroll_y);
        ts->selection_box_active = false;
      }

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

        // Calculate the target track index consistently with the drag preview logic
        float clicked_snippet_drop_visual_top_y = mouse_pos.y - ts->drag_state.drag_offset_y;
        float content_y = clicked_snippet_drop_visual_top_y - timeline_bb.Min.y + tracks_area_scroll_y;
        const float inner_pad = 2.0f; // Padding inside track where snippet is drawn
        int target_track_idx = (int)floorf(((content_y - inner_pad) / ts->track_height) + 0.5f);

        // Clamp the target track index to valid bounds
        if (target_track_idx < 0)
          target_track_idx = 0;
        if (target_track_idx >= ts->player_track_count)
          target_track_idx = ts->player_track_count - 1;
        int source_base_track_idx = ts->drag_state.source_track_index;
        int track_delta = target_track_idx - source_base_track_idx;
        int tick_delta = final_drop_tick_clicked - clicked_snippet->start_tick;

        if (ts->selected_snippets.count > 0) { // Should always be true for a snippet drag
          bool can_move_all = true;
          // PRE-FLIGHT CHECK
          // First, check if all selected snippets can be moved without conflict.
          for (int i = 0; i < ts->selected_snippets.count; ++i) {
            int sid = ts->selected_snippets.ids[i];
            input_snippet_t *s = NULL;
            int s_track_idx = -1;
            for (int ti = 0; ti < ts->player_track_count; ++ti) {
              s = find_snippet_by_id(&ts->player_tracks[ti], sid);
              if (s) {
                s_track_idx = ti;
                break;
              }
            }
            if (!s) { // Should not happen
              can_move_all = false;
              break;
            }
            int new_track_idx = s_track_idx + track_delta;
            // Clamp for check consistency
            if (new_track_idx < 0) {
              can_move_all = false;
              break;
            }
            if (new_track_idx >= ts->player_track_count) {
              can_move_all = false;
              break;
            }
            int new_start_tick = s->start_tick + tick_delta;

            // Perform a "dry run" move to check for validity without changing state.
            if (!try_move_snippet(ts, sid, s_track_idx, new_track_idx, new_start_tick, true)) {
              can_move_all = false;
              break;
            }
          }

          // PERFORM MOVE
          // If the pre-flight check passed for all snippets, perform the actual moves.
          if (can_move_all) {
            // Use a copy of IDs, as duplication might alter the selection state.
            int count = ts->selected_snippets.count;
            int *original_ids = malloc(count * sizeof(int));
            memcpy(original_ids, ts->selected_snippets.ids, count * sizeof(int));

            for (int i = 0; i < count; ++i) {
              int sid = original_ids[i];
              // Find snippet and its source track again to perform the move
              // (This is quick and safer than caching pointers that might be invalidated)
              input_snippet_t *s = NULL;
              int s_track_idx = -1;
              for (int ti = 0; ti < ts->player_track_count; ++ti) {
                s = find_snippet_by_id(&ts->player_tracks[ti], sid);
                if (s) {
                  s_track_idx = ti;
                  break;
                }
              }

              int new_track_idx = s_track_idx + track_delta;
              int new_start_tick = s->start_tick + tick_delta;
              try_move_snippet(ts, sid, s_track_idx, new_track_idx, new_start_tick, false);
            }
            free(original_ids);
          }
        } else {
          try_move_snippet(ts, ts->drag_state.dragged_snippet_id, ts->drag_state.source_track_index,
                           target_track_idx, final_drop_tick_clicked, false);
        }
        ts->drag_state.active = false;
        ts->drag_state.source_track_index = -1;
        ts->drag_state.source_snippet_index = -1;
        ts->drag_state.dragged_snippet_id = -1;
      }

      // Draw Playhead
      draw_playhead(ts, overlay_draw_list, timeline_bb, timeline_bb.Min.y - 12);
      draw_drag_preview(ts, overlay_draw_list, timeline_bb, tracks_area_scroll_y);
    }
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
  ts->drag_state.drag_offset_y = 0.0f;

  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  // Initialize unique snippet ID counter
  ts->next_snippet_id = 1; // Start IDs from 1

  // Initialize Players/Tracks
  ts->player_track_count = 0;
  ts->player_tracks = NULL;

  snippet_id_vector_init(&ts->selected_snippets);

  recording_snippet_vector_init(&ts->recording_snippets);
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
  ts->drag_state.drag_offset_y = 0.0f;

  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  v_destroy(&ts->vec);
  wc_free(&ts->previous_world);

  snippet_id_vector_free(&ts->selected_snippets);
  recording_snippet_vector_free(&ts->recording_snippets);
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
