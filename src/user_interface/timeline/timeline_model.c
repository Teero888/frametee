#include "timeline_model.h"
#include "../user_interface.h" // For ui_handler_t
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TRACK_HEIGHT 60.f

// Forward Declarations for Static Helpers
static void v_init(physics_v_t *t);
static void v_destroy(physics_v_t *t);
static void v_push(physics_v_t *t, SWorldCore *world);

// New sorting helper for the compaction algorithm
static int compare_snippets_by_start_tick_p(const void *a, const void *b) {
  const input_snippet_t *snip_a = *(const input_snippet_t **)a;
  const input_snippet_t *snip_b = *(const input_snippet_t **)b;
  return snip_a->start_tick - snip_b->start_tick;
}

// Initialization and Cleanup

void model_init(timeline_state_t *ts, ui_handler_t *ui) {
  ts->ui = ui;
  v_init(&ts->vec);
  ts->previous_world = wc_empty();

  ts->gui_playback_speed = 50;
  ts->playback_speed = 50;
  ts->zoom = 1.0f;
  ts->track_height = DEFAULT_TRACK_HEIGHT;
  ts->selected_player_track_index = -1;
  ts->context_menu_snippet_id = -1;
  ts->active_snippet_id = -1;
  ts->next_snippet_id = 1;

  ts->drag_state.drag_infos = NULL;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  snippet_id_vector_init(&ts->selected_snippets);
}

void model_cleanup(timeline_state_t *ts) {
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      model_free_snippet_inputs(&track->snippets[j]);
    }
    free(track->snippets);
  }
  free(ts->player_tracks);

  if (ts->drag_state.drag_infos) {
    free(ts->drag_state.drag_infos);
  }

  v_destroy(&ts->vec);
  wc_free(&ts->previous_world);
  snippet_id_vector_free(&ts->selected_snippets);

  memset(ts, 0, sizeof(timeline_state_t));
}

// Snippet ID Vector Helpers

void snippet_id_vector_init(snippet_id_vector_t *vec) {
  vec->ids = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

void snippet_id_vector_free(snippet_id_vector_t *vec) {
  free(vec->ids);
  snippet_id_vector_init(vec);
}

void snippet_id_vector_add(snippet_id_vector_t *vec, int snippet_id) {
  if (vec->count >= vec->capacity) {
    int new_capacity = vec->capacity == 0 ? 8 : vec->capacity * 2;
    vec->ids = realloc(vec->ids, sizeof(int) * new_capacity);
    if (!vec->ids) return;
    vec->capacity = new_capacity;
  }
  vec->ids[vec->count++] = snippet_id;
}

bool snippet_id_vector_remove(snippet_id_vector_t *vec, int snippet_id) {
  for (int i = 0; i < vec->count; ++i) {
    if (vec->ids[i] == snippet_id) {
      if (i < vec->count - 1) {
        memmove(&vec->ids[i], &vec->ids[i + 1], (vec->count - i - 1) * sizeof(int));
      }
      vec->count--;
      return true;
    }
  }
  return false;
}

bool snippet_id_vector_contains(const snippet_id_vector_t *vec, int snippet_id) {
  for (int i = 0; i < vec->count; ++i) {
    if (vec->ids[i] == snippet_id) {
      return true;
    }
  }
  return false;
}

// Finders

input_snippet_t *model_find_snippet_in_track(player_track_t *track, int snippet_id) {
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      return &track->snippets[i];
    }
  }
  return NULL;
}

input_snippet_t *model_find_snippet_by_id(timeline_state_t *ts, int snippet_id, int *out_track_index) {
  for (int i = 0; i < ts->player_track_count; ++i) {
    input_snippet_t *snippet = model_find_snippet_in_track(&ts->player_tracks[i], snippet_id);
    if (snippet) {
      if (out_track_index) *out_track_index = i;
      return snippet;
    }
  }
  return NULL;
}

int model_find_available_layer(const timeline_state_t *ts, const player_track_t *track, int start_tick, int end_tick, int exclude_snippet_id) {
  // Find the lowest layer that is free for [start_tick, end_tick).
  // IMPORTANT: this MUST consider *all* snippets when computing collisions.
  // Previously selected snippets were being skipped which caused overlapping/stacking issues.
  for (int layer = 0; layer < MAX_SNIPPET_LAYERS; ++layer) {
    bool layer_is_free = true;
    for (int i = 0; i < track->snippet_count; ++i) {
      const input_snippet_t *other = &track->snippets[i];
      if (other->id == exclude_snippet_id) continue;
      if (other->layer != layer) continue;
      // Overlap test: interval intersects
      if (start_tick < other->end_tick && end_tick > other->start_tick) {
        layer_is_free = false;
        break;
      }
    }
    if (layer_is_free) return layer;
  }
  return -1;
}

int model_get_stack_size_at_tick_range(const player_track_t *track, int start_tick, int end_tick) {
  int max_layer = 0;
  for (int i = 0; i < track->snippet_count; i++) {
    const input_snippet_t *other = &track->snippets[i];
    if (start_tick < other->end_tick && end_tick > other->start_tick) {
      if (other->layer > max_layer) {
        max_layer = other->layer;
      }
    }
  }
  return max_layer + 1;
}

int model_get_max_timeline_tick(timeline_state_t *ts) {
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

// Data Modification

void timeline_solve_snippet_layers(input_snippet_t **snippets, int count) {
  if (count <= 1) {
    if (count == 1 && snippets[0]) snippets[0]->layer = 0;
    return;
  }

  // Sort the provided snippet pointers by start time
  qsort(snippets, count, sizeof(input_snippet_t *), compare_snippets_by_start_tick_p);

  // Greedily assign the lowest possible layer to each one
  for (int i = 0; i < count; i++) {
    input_snippet_t *current = snippets[i];
    int start_tick = current->start_tick;
    int end_tick = current->end_tick;

    current->layer = 0; // Default to layer 0

    for (int layer = 0; layer < MAX_SNIPPET_LAYERS; ++layer) {
      bool layer_is_free = true;
      // Check for collision against snippets that have already been placed in this pass
      for (int j = 0; j < i; j++) {
        input_snippet_t *other = snippets[j];
        if (other->layer == layer) {
          if (start_tick < other->end_tick && end_tick > other->start_tick) {
            layer_is_free = false;
            break;
          }
        }
      }
      if (layer_is_free) {
        current->layer = layer;
        break;
      }
    }
  }
}

void model_insert_snippet_into_track(player_track_t *track, const input_snippet_t *snippet) {
  track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * (track->snippet_count + 1));
  track->snippets[track->snippet_count] = *snippet;
  track->snippet_count++;
}

bool model_remove_snippet_from_track(timeline_state_t *ts, player_track_t *track, int snippet_id) {
  int found_idx = -1;
  for (int i = 0; i < track->snippet_count; ++i) {
    if (track->snippets[i].id == snippet_id) {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1) {
    int removed_start_tick = track->snippets[found_idx].start_tick;
    model_free_snippet_inputs(&track->snippets[found_idx]);

    memmove(&track->snippets[found_idx], &track->snippets[found_idx + 1], (track->snippet_count - found_idx - 1) * sizeof(input_snippet_t));
    track->snippet_count--;

    if (track->snippet_count > 0) track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * track->snippet_count);
    else {
      free(track->snippets);
      track->snippets = NULL;
    }

    model_recalc_physics(ts, removed_start_tick);

    return true;
  }
  return false;
}

void model_resize_snippet_inputs(timeline_state_t *ts, input_snippet_t *snippet, int new_duration) {
  if (new_duration <= 0) {
    model_free_snippet_inputs(snippet);
    snippet->start_tick = snippet->end_tick;
    return;
  }
  if (snippet->input_count == new_duration) return;

  int old_count = snippet->input_count;
  snippet->inputs = realloc(snippet->inputs, sizeof(SPlayerInput) * new_duration);
  if (!snippet->inputs) {
    snippet->input_count = 0;
    return;
  }
  if (new_duration > old_count) {
    memset(&snippet->inputs[old_count], 0, (new_duration - old_count) * sizeof(SPlayerInput));
  }

  snippet->input_count = new_duration;
  snippet->end_tick = snippet->start_tick + new_duration;

  if (snippet->end_tick <= ts->current_tick) model_recalc_physics(ts, snippet->start_tick - 1);
}

void model_free_snippet_inputs(input_snippet_t *snippet) {
  free(snippet->inputs);
  snippet->inputs = NULL;
  snippet->input_count = 0;
}

void model_snippet_clone(input_snippet_t *dest, const input_snippet_t *src) {
  *dest = *src;
  dest->input_count = src->input_count;
  if (src->inputs && src->input_count > 0) {
    dest->inputs = malloc(src->input_count * sizeof(SPlayerInput));
    memcpy(dest->inputs, src->inputs, src->input_count * sizeof(SPlayerInput));
  } else {
    dest->inputs = NULL;
  }
}

player_track_t *model_add_new_track(timeline_state_t *ts, ph_t *ph, int num) {
  if (num <= 0) return NULL;
  if (ph && wc_add_character(&ph->world, num) == NULL) return NULL;

  int old_count = ts->player_track_count;
  int new_count = old_count + num;
  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * new_count);

  for (int i = 0; i < num; i++) {
    player_track_t *new_track = &ts->player_tracks[old_count + i];
    memset(new_track, 0, sizeof(player_track_t));
  }

  ts->player_track_count = new_count;
  ts->vec.current_size = 1;
  if (ph) {
    wc_copy_world(&ts->vec.data[0], &ph->world);
    wc_copy_world(&ts->previous_world, &ph->world);
  }
  return &ts->player_tracks[old_count];
}

void model_remove_track_logic(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  player_track_t *track = &ts->player_tracks[track_index];
  for (int i = 0; i < track->snippet_count; ++i) {
    model_free_snippet_inputs(&track->snippets[i]);
  }
  free(track->snippets);

  if (track_index < ts->player_track_count - 1) {
    memmove(&ts->player_tracks[track_index], &ts->player_tracks[track_index + 1],
            (ts->player_track_count - track_index - 1) * sizeof(player_track_t));
  }

  ts->player_track_count--;
  if (ts->player_track_count == 0) {
    free(ts->player_tracks);
    ts->player_tracks = NULL;
  } else {
    ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * ts->player_track_count);
  }

  if (ts->selected_player_track_index == track_index) ts->selected_player_track_index = -1;
  else if (ts->selected_player_track_index > track_index) ts->selected_player_track_index--;

  model_recalc_physics(ts, 0);
}

void model_compact_layers_for_track(player_track_t *track) {
  if (track->snippet_count == 0) return;

  input_snippet_t **all_snippets = malloc(track->snippet_count * sizeof(input_snippet_t *));
  if (!all_snippets) return;

  for (int i = 0; i < track->snippet_count; i++) {
    all_snippets[i] = &track->snippets[i];
  }

  timeline_solve_snippet_layers(all_snippets, track->snippet_count);

  free(all_snippets);
}

// Physics & Playback

void model_recalc_physics(timeline_state_t *ts, int tick) {
  ts->vec.current_size = imin(ts->vec.current_size, imax((tick - 1) / 50, 1));
  ts->previous_world.m_GameTick = INT_MAX;
}

SPlayerInput model_get_input_at_tick(const timeline_state_t *ts, int track_index, int tick) {
  const player_track_t *track = &ts->player_tracks[track_index];
  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active && tick >= snippet->start_tick && tick < snippet->end_tick) return snippet->inputs[tick - snippet->start_tick];
  }
  return (SPlayerInput){.m_TargetY = -1};
}

void model_advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, 0);

  if (ts->recording && ts->current_tick > ts->recording_snippets.snippets[0]->end_tick) {
    for (int i = 0; i < ts->recording_snippets.count; ++i) {
      input_snippet_t *snippet = ts->recording_snippets.snippets[i];
      if (!snippet) continue;
      model_resize_snippet_inputs(ts, snippet, snippet->input_count + steps);
      for (int s = snippet->input_count - 1; s >= snippet->input_count - steps; --s)
        snippet->inputs[s] = ts->recording_input;
      ts->current_tick = snippet->end_tick;
    }
  }
}

void model_activate_snippet(timeline_state_t *ts, int track_index, int snippet_id_to_activate) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  player_track_t *track = &ts->player_tracks[track_index];
  input_snippet_t *target_snippet = model_find_snippet_in_track(track, snippet_id_to_activate);
  if (!target_snippet || target_snippet->is_active) return;

  for (int i = 0; i < track->snippet_count; ++i) {
    input_snippet_t *other = &track->snippets[i];
    if (other->id != snippet_id_to_activate && target_snippet->start_tick < other->end_tick && target_snippet->end_tick > other->start_tick) {
      other->is_active = false;
    }
  }

  target_snippet->is_active = true;
  model_recalc_physics(ts, target_snippet->start_tick);
}

void model_get_world_state_at_tick(timeline_state_t *ts, int tick, SWorldCore *out_world) {
  const int step = 50;

  if (tick < ts->previous_world.m_GameTick) {
    int base_index = imin((tick - 1) / step, ts->vec.current_size - 1);
    if (base_index < 0) base_index = 0;
    wc_copy_world(out_world, &ts->vec.data[base_index]);
  } else {
    wc_copy_world(out_world, &ts->previous_world);
  }

  while (out_world->m_GameTick < tick) {
    for (int p = 0; p < out_world->m_NumCharacters; ++p) {
      SPlayerInput input = model_get_input_at_tick(ts, p, out_world->m_GameTick);
      cc_on_input(&out_world->m_pCharacters[p], &input);
    }
    wc_tick(out_world);
    if (out_world->m_GameTick % step == 0) {
      int cache_index = out_world->m_GameTick / step;
      if (cache_index >= ts->vec.current_size) {
        v_push(&ts->vec, out_world);
      } else {
        wc_copy_world(&ts->vec.data[cache_index], out_world);
      }
    }
  }
  wc_copy_world(&ts->previous_world, out_world);
}

// Static Physics Vector Helpers
static void v_init(physics_v_t *t) {
  t->current_size = 1;
  t->max_size = 1;
  t->data = calloc(1, sizeof(SWorldCore));
  t->data[0] = wc_empty();
}

static void v_destroy(physics_v_t *t) {
  for (int i = 0; i < t->max_size; ++i)
    wc_free(&t->data[i]);
  free(t->data);
  t->current_size = 0;
  t->max_size = 0;
}

static void v_push(physics_v_t *t, SWorldCore *world) {
  ++t->current_size;
  if (t->current_size > t->max_size) {
    t->max_size *= 2;
    t->data = realloc(t->data, t->max_size * sizeof(SWorldCore));
    for (int i = t->max_size / 2; i < t->max_size; ++i)
      t->data[i] = wc_empty();
  }
  wc_copy_world(&t->data[t->current_size - 1], world);
}
