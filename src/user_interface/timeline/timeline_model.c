#include "timeline_model.h"
#include <limits.h>
#include <renderer/graphics_backend.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>

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

void model_init(timeline_state_t *ts, struct ui_handler *ui) {
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

  ts->dummy_action_priority[0] = DUMMY_ACTION_COPY;
  ts->dummy_action_priority[1] = DUMMY_ACTION_INPUTS;

  snippet_id_vector_init(&ts->selected_snippets);
}

void model_cleanup(timeline_state_t *ts) {
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      model_free_snippet_inputs(&track->snippets[j]);
    }
    for (int j = 0; j < track->recording_snippet_count; ++j) {
      model_free_snippet_inputs(&track->recording_snippets[j]);
    }
    free(track->snippets);
    free(track->recording_snippets);
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

int model_find_available_layer(const player_track_t *track, int start_tick, int end_tick, int exclude_snippet_id) {
  for (int layer = 0; layer < MAX_SNIPPET_LAYERS; ++layer) {
    bool layer_is_free = true;
    for (int i = 0; i < track->snippet_count; ++i) {
      const input_snippet_t *other = &track->snippets[i];
      if (other->id == exclude_snippet_id) continue;
      if (other->layer != layer) continue;
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

  qsort(snippets, count, sizeof(input_snippet_t *), compare_snippets_by_start_tick_p);

  for (int i = 0; i < count; i++) {
    input_snippet_t *current = snippets[i];
    int start_tick = current->start_tick;
    int end_tick = current->end_tick;

    current->layer = 0;

    for (int layer = 0; layer < MAX_SNIPPET_LAYERS; ++layer) {
      bool layer_is_free = true;
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
  if (track->snippet_count >= track->snippet_capacity) {
    track->snippet_capacity = track->snippet_capacity == 0 ? 8 : track->snippet_capacity * 2;
    track->snippets = realloc(track->snippets, sizeof(input_snippet_t) * track->snippet_capacity);
  }
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

    if (track->snippet_count == 0) {
      free(track->snippets);
      track->snippets = NULL;
      track->snippet_capacity = 0;
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

  int preserve_count = (old_count < new_duration) ? old_count : new_duration;
  if (snippet->end_tick <= ts->current_tick) model_recalc_physics(ts, snippet->start_tick + preserve_count);
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

  if (wc_add_character(&ts->vec.data[0], num) == NULL) return NULL;
  wc_add_character(&ts->previous_world, num);
  if (ph) {
    wc_add_character(&ph->world, num);
  }

  int old_count = ts->player_track_count;
  int new_count = old_count + num;
  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * new_count);

  for (int i = 0; i < num; i++) {
    player_track_t *new_track = &ts->player_tracks[old_count + i];
    memset(new_track, 0, sizeof(player_track_t));
    new_track->dummy_copy_flags = COPY_ALL;
  }

  ts->player_track_count = new_count;
  model_recalc_physics(ts, 0);

  return &ts->player_tracks[old_count];
}

void model_remove_track_logic(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  wc_remove_character(&ts->vec.data[0], track_index);
  wc_remove_character(&ts->previous_world, track_index);
  if (ts->ui && ts->ui->gfx_handler) {
    wc_remove_character(&ts->ui->gfx_handler->physics_handler.world, track_index);
  }

  player_track_t *track = &ts->player_tracks[track_index];
  for (int i = 0; i < track->snippet_count; ++i) {
    model_free_snippet_inputs(&track->snippets[i]);
  }
  free(track->snippets);

  for (int i = 0; i < track->recording_snippet_count; ++i) {
    model_free_snippet_inputs(&track->recording_snippets[i]);
  }
  free(track->recording_snippets);

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

  ts->vec.current_size = 1;
  model_recalc_physics(ts, 0);
}

static void wc_insert_character_at_index(SWorldCore *pWorld, int index) {
  if (!wc_add_character(pWorld, 1)) return;

  if (index < pWorld->m_NumCharacters - 1) {
    SCharacterCore new_char = pWorld->m_pCharacters[pWorld->m_NumCharacters - 1];
    memmove(&pWorld->m_pCharacters[index + 1], &pWorld->m_pCharacters[index], sizeof(SCharacterCore) * (pWorld->m_NumCharacters - 1 - index));
    pWorld->m_pCharacters[index] = new_char;

    STeeLink new_link = pWorld->m_Accelerator.m_pTeeList[pWorld->m_NumCharacters - 1];
    memmove(&pWorld->m_Accelerator.m_pTeeList[index + 1], &pWorld->m_Accelerator.m_pTeeList[index],
            sizeof(STeeLink) * (pWorld->m_NumCharacters - 1 - index));
    pWorld->m_Accelerator.m_pTeeList[index] = new_link;
  }

  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    pWorld->m_pCharacters[i].m_Id = i;
    pWorld->m_Accelerator.m_pTeeList[i].m_TeeId = i;
  }

  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    if (i == index) continue;
    SCharacterCore *pChar = &pWorld->m_pCharacters[i];
    if (pChar->m_HookedPlayer >= index) {
      pChar->m_HookedPlayer++;
    }
  }

  int size = pWorld->m_pCollision->m_MapData.width * pWorld->m_pCollision->m_MapData.height;
  memset(pWorld->m_Accelerator.m_pGrid->m_pTeeGrid, -1, size * sizeof(int));
  pWorld->m_Accelerator.hash = 0;
}

void model_insert_track_physics(timeline_state_t *ts, int track_index) {
  wc_insert_character_at_index(&ts->vec.data[0], track_index);
  wc_insert_character_at_index(&ts->previous_world, track_index);
  if (ts->ui && ts->ui->gfx_handler) {
    wc_insert_character_at_index(&ts->ui->gfx_handler->physics_handler.world, track_index);
  }
  ts->vec.current_size = 1;
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

// Recording & Merging

void model_insert_snippet_into_recording_track(player_track_t *track, const input_snippet_t *snippet) {
  if (track->recording_snippet_count >= track->recording_snippet_capacity) {
    track->recording_snippet_capacity = track->recording_snippet_capacity == 0 ? 8 : track->recording_snippet_capacity * 2;
    track->recording_snippets = realloc(track->recording_snippets, sizeof(input_snippet_t) * track->recording_snippet_capacity);
  }
  track->recording_snippets[track->recording_snippet_count] = *snippet;
  track->recording_snippet_count++;
}

void model_apply_input_to_main_buffer(timeline_state_t *ts, player_track_t *track, int tick, const SPlayerInput *input) {
  input_snippet_t *overlapping_snippet = NULL;
  for (int j = 0; j < track->snippet_count; ++j) {
    if (track->snippets[j].is_active && tick >= track->snippets[j].start_tick && tick < track->snippets[j].end_tick) {
      overlapping_snippet = &track->snippets[j];
      break;
    }
  }
  if (overlapping_snippet) {
    overlapping_snippet->inputs[tick - overlapping_snippet->start_tick] = *input;
    return;
  }

  input_snippet_t *before = NULL;
  input_snippet_t *after = NULL;
  for (int j = 0; j < track->snippet_count; ++j) {
    if (track->snippets[j].is_active && track->snippets[j].end_tick == tick) before = &track->snippets[j];
    if (track->snippets[j].is_active && track->snippets[j].start_tick == tick + 1) after = &track->snippets[j];
  }

  if (before && after) {
    int old_before_duration = before->input_count;
    int after_duration = after->input_count;
    model_resize_snippet_inputs(ts, before, old_before_duration + 1 + after_duration);
    before->inputs[old_before_duration] = *input;
    memcpy(&before->inputs[old_before_duration + 1], after->inputs, sizeof(SPlayerInput) * after_duration);
    model_remove_snippet_from_track(ts, track, after->id);
    model_compact_layers_for_track(track);
  } else if (before) {
    model_resize_snippet_inputs(ts, before, before->input_count + 1);
    before->inputs[before->input_count - 1] = *input;
  } else if (after) {
    int old_duration = after->input_count;
    after->inputs = realloc(after->inputs, sizeof(SPlayerInput) * (old_duration + 1));
    memmove(&after->inputs[1], &after->inputs[0], sizeof(SPlayerInput) * old_duration);
    after->inputs[0] = *input;
    after->input_count++;
    after->start_tick--;
  } else {
    input_snippet_t new_snippet = {0};
    new_snippet.id = ts->next_snippet_id++;
    new_snippet.start_tick = tick;
    new_snippet.end_tick = tick + 1;
    new_snippet.is_active = true;
    new_snippet.input_count = 1;
    new_snippet.inputs = calloc(1, sizeof(SPlayerInput));
    new_snippet.inputs[0] = *input;
    new_snippet.layer = model_find_available_layer(track, tick, tick + 1, -1);
    if (new_snippet.layer == -1) new_snippet.layer = 0;
    model_insert_snippet_into_track(track, &new_snippet);
    model_compact_layers_for_track(track);
  }
}

void model_clear_all_recording_buffers(timeline_state_t *ts) {
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->recording_snippet_count; ++j) {
      model_free_snippet_inputs(&track->recording_snippets[j]);
    }
    free(track->recording_snippets);
    track->recording_snippets = NULL;
    track->recording_snippet_count = 0;
    track->recording_snippet_capacity = 0;
  }
}

// Physics & Playback

void model_recalc_physics(timeline_state_t *ts, int tick) {
  ts->vec.current_size = imin(ts->vec.current_size, imax(tick / 50 + 1, 1));
  if (ts->previous_world.m_GameTick > tick) {
    ts->previous_world.m_GameTick = INT_MAX;
  }
}

SPlayerInput model_get_input_at_tick(const timeline_state_t *ts, int track_index, int tick) {
  const player_track_t *track = &ts->player_tracks[track_index];
  SPlayerInput last_valid_input = {.m_TargetY = -1};
  int last_input_tick = -1;

  if (ts->recording) {
    for (int i = 0; i < track->recording_snippet_count; ++i) {
      const input_snippet_t *snippet = &track->recording_snippets[i];
      if (snippet->is_active) {
        if (tick >= snippet->start_tick && tick < snippet->end_tick) return snippet->inputs[tick - snippet->start_tick];
        if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
          last_input_tick = snippet->end_tick - 1;
          last_valid_input = snippet->inputs[snippet->input_count - 1];
        }
      }
    }
  }

  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active) {
      if (tick >= snippet->start_tick && tick < snippet->end_tick) return snippet->inputs[tick - snippet->start_tick];
      if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
        last_input_tick = snippet->end_tick - 1;
        last_valid_input = snippet->inputs[snippet->input_count - 1];
      }
    }
  }

  if (tick > last_input_tick && last_input_tick != -1) return last_valid_input;
  return (SPlayerInput){.m_TargetY = -1};
}

void model_advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, 0);

  if (ts->recording) {
    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track_t *track = &ts->player_tracks[i];
      input_snippet_t *active_rec_snip = NULL;

      if (track->recording_snippet_count > 0) {
        active_rec_snip = &track->recording_snippets[track->recording_snippet_count - 1];
      }

      if (active_rec_snip) {
        // Calculate where the current tick is relative to the start of the snippet
        int relative_tick = ts->current_tick - active_rec_snip->start_tick;

        // ONLY append if we are past the end of the current recording snippet.
        // DO NOT overwrite if we are rewinding/scrubbing inside the snippet.
        if (relative_tick > active_rec_snip->input_count) {
          int old_count = active_rec_snip->input_count;
          int needed = relative_tick;
          model_resize_snippet_inputs(ts, active_rec_snip, needed);

          // Fill the gap (which should be 'steps' wide) with the current input
          for (int s = old_count; s < needed; ++s) {
            active_rec_snip->inputs[s] = track->current_input;
          }

          if (i == ts->selected_player_track_index) {
            ts->ui->recording_mouse_pos[0] = track->current_input.m_TargetX;
            ts->ui->recording_mouse_pos[1] = track->current_input.m_TargetY;
          }
        }
      }
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
      if ((uint32_t)cache_index >= ts->vec.current_size) {
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
  for (uint32_t i = 0; i < t->max_size; ++i)
    wc_free(&t->data[i]);
  free(t->data);
  t->current_size = 0;
  t->max_size = 0;
}

static void v_push(physics_v_t *t, SWorldCore *world) {
  ++t->current_size;
  if (t->current_size > t->max_size) {
    t->max_size *= 2;
    SWorldCore *new_data = realloc(t->data, t->max_size * sizeof(SWorldCore));
    if (new_data) {
      t->data = new_data;
      for (uint32_t i = 0; i < t->current_size - 1; ++i) {
        for (int j = 0; j < t->data[i].m_NumCharacters; ++j) {
          t->data[i].m_pCharacters[j].m_pWorld = &t->data[i];
        }
        for (int type = 0; type < NUM_WORLD_ENTTYPES; ++type) {
          for (SEntity *ent = t->data[i].m_apFirstEntityTypes[type]; ent; ent = ent->m_pNextTypeEntity) {
            ent->m_pWorld = &t->data[i];
          }
        }
      }
    }

    for (uint32_t i = t->max_size / 2; i < t->max_size; ++i)
      t->data[i] = wc_empty();
  }
  wc_copy_world(&t->data[t->current_size - 1], world);
}
