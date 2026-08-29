#include "timeline_model.h"
#include <engine/game_host.h>
#include <engine/input_record.h>
#include <engine/int_math.h>
#include <limits.h>
#include <math.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/input_effects.h>
#include <user_interface/timeline_events.h>
#include <user_interface/user_interface.h>

#define DEFAULT_TRACK_HEIGHT 60.f

// Forward Declarations for Static Helpers
static void v_init(timeline_state_t *ts, physics_v_t *t, int world_index);
static void v_destroy(timeline_state_t *ts, physics_v_t *t);
static void v_push(timeline_state_t *ts, physics_v_t *t, const ft_world *world, int world_index);

// The host the timeline simulates through. Every world in here belongs to the
// active game; the engine only creates, copies and destroys them.
static game_host_t *model_host(const timeline_state_t *ts) {
  return (ts && ts->ui && ts->ui->gfx_handler) ? &ts->ui->gfx_handler->game_host : NULL;
}

// New sorting helper for the compaction algorithm
static int compare_snippets_by_start_tick_p(const void *a, const void *b) {
  const input_snippet_t *snip_a = *(const input_snippet_t **)a;
  const input_snippet_t *snip_b = *(const input_snippet_t **)b;
  return snip_a->start_tick - snip_b->start_tick;
}

static const float s_group_colors[][4] = {
    {0.25f, 0.55f, 0.95f, 1.0f},
    {0.95f, 0.72f, 0.10f, 1.0f},
    {0.45f, 0.82f, 0.28f, 1.0f},
    {0.88f, 0.35f, 0.70f, 1.0f},
    {0.20f, 0.80f, 0.78f, 1.0f},
    {0.95f, 0.38f, 0.25f, 1.0f},
};

static void group_runtime_init(timeline_state_t *ts, timeline_group_t *group, int world_index) {
  game_host_t *host = model_host(ts);
  const ft_level *level = ts->ui->gfx_handler->level;
  v_init(ts, &group->vec, world_index);
  group->initial_world = gh_world_create(host, level, 0, world_index);
  group->previous_world = gh_world_create(host, level, 0, world_index);
  group->prev_world_cached = gh_world_create(host, level, 0, world_index);
  group->world_cached = gh_world_create(host, level, 0, world_index);
  group->cached_tick = -1;
}

static void group_runtime_cleanup(timeline_state_t *ts, timeline_group_t *group) {
  if (!group) return;
  game_host_t *host = model_host(ts);
  prediction_group_cleanup(ts, group);
  v_destroy(ts, &group->vec);
  gh_world_destroy(host, group->initial_world);
  gh_world_destroy(host, group->previous_world);
  gh_world_destroy(host, group->prev_world_cached);
  gh_world_destroy(host, group->world_cached);
  group->initial_world = group->previous_world = group->prev_world_cached = group->world_cached = NULL;
}

timeline_group_t *model_add_group(timeline_state_t *ts, const char *name) {
  if (!ts) return NULL;
  timeline_group_t *group = calloc(1, sizeof(*group));
  if (!group) return NULL;

  int index = ts->group_count;
  snprintf(group->name, sizeof(group->name), "%s", name && name[0] ? name : "Group");
  memcpy(group->color, s_group_colors[index % (int)(sizeof(s_group_colors) / sizeof(s_group_colors[0]))], sizeof(group->color));
  group->visible = true;
  group->export_enabled = true;
  group->prediction_enabled = true;
  group_runtime_init(ts, group, index);

  timeline_group_t **groups = realloc(ts->groups, sizeof(*groups) * (size_t)(index + 1));
  if (!groups) {
    group_runtime_cleanup(ts, group);
    free(group);
    return NULL;
  }
  ts->groups = groups;
  ts->groups[index] = group;
  ts->group_count++;

  return group;
}

int model_track_group_index(const timeline_state_t *ts, int track_index) {
  if (!ts || track_index < 0 || track_index >= ts->player_track_count) return -1;
  int group = ts->player_tracks[track_index].group_index;
  return group >= 0 && group < ts->group_count ? group : -1;
}

int model_group_track_count(const timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return 0;
  int count = 0;
  for (int i = 0; i < ts->player_track_count; ++i)
    if (ts->player_tracks[i].group_index == group_index) ++count;
  return count;
}

int model_group_track_index(const timeline_state_t *ts, int group_index, int local_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count || local_index < 0) return -1;
  for (int i = 0, local = 0; i < ts->player_track_count; ++i) {
    if (ts->player_tracks[i].group_index != group_index) continue;
    if (local++ == local_index) return i;
  }
  return -1;
}

int model_group_local_track_index(const timeline_state_t *ts, int track_index) {
  int group_index = model_track_group_index(ts, track_index);
  if (group_index < 0) return -1;
  int local = 0;
  for (int i = 0; i < track_index; ++i)
    if (ts->player_tracks[i].group_index == group_index) ++local;
  return local;
}

int model_group_playhead_tick(const timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return 0;
  return imax(0, ts->current_tick - ts->groups[group_index]->start_offset);
}

int model_get_min_global_tick(const timeline_state_t *ts) {
  if (!ts) return 0;
  int min_tick = 0;
  for (int group_index = 0; group_index < ts->group_count; ++group_index)
    min_tick = imin(min_tick, ts->groups[group_index]->start_offset);
  return min_tick;
}

int model_clamp_global_tick_for_group(const timeline_state_t *ts, int group_index, int tick) {
  int min_tick = ts && group_index >= 0 && group_index < ts->group_count ? ts->groups[group_index]->start_offset : 0;
  return imax(tick, min_tick);
}

void model_set_active_group(timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;
  if (ts->recording && group_index != ts->active_group_index) return;
  ts->active_group_index = group_index;
}

void model_reset_groups_for_level(timeline_state_t *ts) {
  if (!ts || !ts->ui || !ts->ui->gfx_handler) return;
  game_host_t *host = model_host(ts);
  const ft_level *level = ts->ui->gfx_handler->level;
  if (!level) return;

  // A new level invalidates every world in the project, so each group is given
  // a fresh set built from it, sized to the tracks that group already owns.
  for (int i = 0; i < ts->group_count; ++i) {
    timeline_group_t *group = ts->groups[i];
    const int players = model_group_track_count(ts, i);

    group_runtime_cleanup(ts, group);
    group_runtime_init(ts, group, i);

    // A fixed-cast game has already created its required players. Dynamic
    // games start lower and need only the difference represented by tracks.
    const int existing = gh_world_player_count(host, group->initial_world);
    for (int p = existing; p < players; ++p)
      gh_world_add_player(host, group->initial_world, -1, NULL);
    gh_world_copy(host, group->previous_world, group->initial_world);
    if (group->vec.data && group->vec.data[0]) gh_world_copy(host, group->vec.data[0], group->initial_world);
    group->vec.current_size = 1;
    group->cached_tick = -1;
  }
}

// Initialization and Cleanup

void model_init(timeline_state_t *ts, ui_handler_t *ui) {
  ts->ui = ui;

  prediction_settings_default(&ts->prediction);

  int tps = (ui && ui->gfx_handler) ? game_ticks_per_second(&ui->gfx_handler->game_host) : 50;
  if (tps <= 0) tps = 50;
  ts->gui_playback_speed = tps;
  ts->playback_speed = tps;
  ts->zoom = 1.0f;
  ts->track_height = DEFAULT_TRACK_HEIGHT;
  ts->selected_player_track_index = -1;
  ts->context_menu_snippet_id = -1;
  ts->active_snippet_id = -1;
  ts->pending_single_select_id = -1;
  ts->header_drag_group_index = -1;
  ts->next_snippet_id = 1;
  ts->active_group_index = 0;

  ts->drag_state.drag_infos = NULL;
  ts->drag_state.initial_mouse_pos = (ImVec2){0, 0};

  ts->events = NULL;
  ts->event_count = 0;
  ts->event_capacity = 0;

  snippet_id_vector_init(&ts->selected_snippets);
  model_add_group(ts, "Group 1");
  model_sync_tracks_to_world(ts, 0);
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

  if (ts->events) {
    free(ts->events);
  }

  free(ts->recording_snippets.snippets);

  for (int i = 0; i < ts->group_count; ++i) {
    group_runtime_cleanup(ts, ts->groups[i]);
    free(ts->groups[i]);
  }
  free(ts->groups);
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
      int group_index = model_track_group_index(ts, i);
      int offset = group_index >= 0 ? ts->groups[group_index]->start_offset : 0;
      if (track->snippets[j].end_tick + offset > max_tick) {
        max_tick = track->snippets[j].end_tick + offset;
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
  // Catches snippets built by callers that only fill in the window fields.
  model_snippet_normalize(&track->snippets[track->snippet_count]);
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

  // This rewrites the buffer as a whole, so the window has to be the buffer first.
  model_snippet_flatten(snippet);

  int old_count = snippet->input_count;
  input_record_t *grown = realloc(snippet->inputs, sizeof(input_record_t) * (size_t)new_duration);
  if (!grown) return;
  snippet->inputs = grown;
  if (new_duration > old_count) {
    memset(&snippet->inputs[old_count], 0, (new_duration - old_count) * sizeof(input_record_t));
  }

  snippet->input_count = new_duration;
  snippet->end_tick = snippet->start_tick + new_duration;
  snippet->source_offset = 0;
  snippet->source_count = new_duration;
  input_effects_snippet_discard_cache(snippet);

  if (!ts->recording) {
    input_effects_invalidate(ts);
    if (snippet->end_tick <= ts->current_tick) model_reset_physics_cache(ts);
  }
}

void model_free_snippet_inputs(input_snippet_t *snippet) {
  free(snippet->inputs);
  input_effects_snippet_cleanup(snippet);
  snippet->inputs = NULL;
  snippet->input_count = 0;
  snippet->source_offset = 0;
  snippet->source_count = 0;
}

void model_snippet_clone(input_snippet_t *dest, const input_snippet_t *src) {
  *dest = *src;
  input_effects_snippet_clone(dest, src);
  // The whole source travels with the copy, not just the visible window, so a cloned snippet can be
  // widened back out to everything the original held.
  if (src->inputs && src->source_count > 0) {
    dest->inputs = malloc(src->source_count * sizeof(input_record_t));
    memcpy(dest->inputs, src->inputs, src->source_count * sizeof(input_record_t));
  } else {
    dest->inputs = NULL;
    dest->source_offset = 0;
    dest->source_count = 0;
  }
}

void model_snippet_normalize(input_snippet_t *snippet) {
  if (!snippet) return;
  if (snippet->source_offset < 0) snippet->source_offset = 0;
  if (snippet->source_count < snippet->source_offset + snippet->input_count) {
    snippet->source_count = snippet->source_offset + snippet->input_count;
  }
  snippet->end_tick = snippet->start_tick + snippet->input_count;
}

void model_snippet_flatten(input_snippet_t *snippet) {
  if (!snippet) return;
  model_snippet_normalize(snippet);
  if (!snippet->inputs) return;
  if (snippet->source_offset == 0 && snippet->source_count == snippet->input_count) return;

  if (snippet->input_count <= 0) {
    model_free_snippet_inputs(snippet);
    return;
  }

  input_record_t *flat = malloc(sizeof(input_record_t) * snippet->input_count);
  if (!flat) return;
  memcpy(flat, snippet_window(snippet), sizeof(input_record_t) * snippet->input_count);
  free(snippet->inputs);
  snippet->inputs = flat;
  snippet->source_offset = 0;
  snippet->source_count = snippet->input_count;
  input_effects_snippet_discard_cache(snippet);
}

bool model_trim_snippet(timeline_state_t *ts, input_snippet_t *snippet, int new_start_tick, int new_end_tick) {
  if (!snippet) return false;
  model_snippet_normalize(snippet);

  if (new_start_tick < 0) new_start_tick = 0;
  if (new_end_tick <= new_start_tick) return false;
  if (new_start_tick == snippet->start_tick && new_end_tick == snippet->end_tick) return false;

  int new_count = new_end_tick - new_start_tick;
  int new_offset = snippet->source_offset - (snippet->start_tick - new_start_tick);

  // Pulling an edge past the retained source is allowed; it invents blank ticks there.
  if (new_offset < 0) {
    int pad = -new_offset;
    input_record_t *grown = realloc(snippet->inputs, sizeof(input_record_t) * (snippet->source_count + pad));
    if (!grown) return false;
    snippet->inputs = grown;
    if (snippet->source_count > 0) memmove(&snippet->inputs[pad], snippet->inputs, sizeof(input_record_t) * snippet->source_count);
    memset(snippet->inputs, 0, sizeof(input_record_t) * pad);
    snippet->source_count += pad;
    snippet->source_offset += pad;
    new_offset = 0;
  }

  if (new_offset + new_count > snippet->source_count) {
    int pad = new_offset + new_count - snippet->source_count;
    input_record_t *grown = realloc(snippet->inputs, sizeof(input_record_t) * (snippet->source_count + pad));
    if (!grown) return false;
    snippet->inputs = grown;
    memset(&snippet->inputs[snippet->source_count], 0, sizeof(input_record_t) * pad);
    snippet->source_count += pad;
  }

  int earliest = snippet->start_tick < new_start_tick ? snippet->start_tick : new_start_tick;

  snippet->source_offset = new_offset;
  snippet->input_count = new_count;
  snippet->start_tick = new_start_tick;
  snippet->end_tick = new_end_tick;

  model_recalc_physics(ts, earliest);
  return true;
}

static player_track_t *insert_track_rows(timeline_state_t *ts, int group_index, int num) {
  if (num <= 0) return NULL;
  if (group_index < 0 || group_index >= ts->group_count) return NULL;

  int existing_group_count = model_group_track_count(ts, group_index);

  int insert_index = ts->player_track_count;
  for (int i = ts->player_track_count - 1; i >= 0; --i) {
    if (ts->player_tracks[i].group_index == group_index) {
      insert_index = i + 1;
      break;
    }
  }
  int new_count = ts->player_track_count + num;
  player_track_t *grown = realloc(ts->player_tracks, sizeof(player_track_t) * (size_t)new_count);
  if (!grown) return NULL;
  ts->player_tracks = grown;
  memmove(&ts->player_tracks[insert_index + num], &ts->player_tracks[insert_index],
          sizeof(player_track_t) * (size_t)(ts->player_track_count - insert_index));

  // Realloc/memmove changes the address of inline string storage.
  for (int i = 0; i < ts->player_track_count; ++i)
    model_rebind_starting_strings(&ts->player_tracks[i].starting_config);

  for (int i = 0; i < num; i++) {
    player_track_t *new_track = &ts->player_tracks[insert_index + i];
    memset(new_track, 0, sizeof(player_track_t));
    snprintf(new_track->name, sizeof(new_track->name), "Track %d", existing_group_count + i + 1);
    // A fresh track carries no profile. The game fills one in the first time
    // its panel is used, and reads "none" as its own defaults until then.
    new_track->group_index = group_index;
    new_track->linked_source_player = 0;
    const ft_input_schema *schema = game_input_schema(model_host(ts));
    if (schema) {
      for (uint32_t field = 0; field < schema->field_count && field < 64; ++field)
        if ((schema->fields[field].flags & (FT_INPUT_FLAG_INTERNAL | FT_INPUT_FLAG_EDITOR_HIDDEN)) == 0)
          new_track->linked_copy_fields |= UINT64_C(1) << field;
    }
    new_track->export_enabled = true;
    new_track->prediction_enabled = true;
  }

  ts->player_track_count = new_count;
  return &ts->player_tracks[insert_index];
}

player_track_t *model_add_new_track(timeline_state_t *ts, int num) {
  if (!ts || num <= 0) return NULL;
  if (ts->active_group_index < 0 || ts->active_group_index >= ts->group_count) return NULL;

  game_host_t *host = model_host(ts);
  timeline_group_t *group = ts->groups[ts->active_group_index];
  const int track_count = model_group_track_count(ts, ts->active_group_index);
  const int world_count = gh_world_player_count(host, group->initial_world);

  // Players a game creates as part of world_create do not need (and for a
  // fixed cast cannot accept) world_add_player. Claim those players with rows
  // first, then ask a dynamic game to create only the remaining players.
  const int unclaimed = world_count > track_count ? world_count - track_count : 0;
  const int players_to_add = num > unclaimed ? num - unclaimed : 0;
  int added = 0;
  for (; added < players_to_add; ++added) {
    if (gh_world_add_player(host, group->initial_world, -1, NULL) >= 0) continue;
    while (added-- > 0)
      gh_world_remove_player(host, group->initial_world, gh_world_player_count(host, group->initial_world) - 1);
    return NULL;
  }

  player_track_t *rows = insert_track_rows(ts, ts->active_group_index, num);
  if (!rows) {
    while (added-- > 0)
      gh_world_remove_player(host, group->initial_world, gh_world_player_count(host, group->initial_world) - 1);
    return NULL;
  }
  model_recalc_physics(ts, 0);
  return rows;
}

void model_sync_tracks_to_world(timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;
  const int tracks = model_group_track_count(ts, group_index);
  const int players = gh_world_player_count(model_host(ts), ts->groups[group_index]->initial_world);
  if (players <= tracks) return;

  const int old_active = ts->active_group_index;
  ts->active_group_index = group_index;
  player_track_t *first = model_add_new_track(ts, players - tracks);
  ts->active_group_index = old_active;
  if (first && ts->selected_player_track_index < 0)
    ts->selected_player_track_index = (int)(first - ts->player_tracks);
}

void model_remove_track_logic(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  int group_index = model_track_group_index(ts, track_index);
  int local_index = model_group_local_track_index(ts, track_index);
  if (group_index >= 0 && local_index >= 0)
    gh_world_remove_player(model_host(ts), ts->groups[group_index]->initial_world, local_index);

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
    for (int i = 0; i < ts->player_track_count; ++i)
      model_rebind_starting_strings(&ts->player_tracks[i].starting_config);
  }

  if (ts->selected_player_track_index == track_index) ts->selected_player_track_index = -1;
  else if (ts->selected_player_track_index > track_index) ts->selected_player_track_index--;

  model_recalc_physics(ts, 0);
}

void model_insert_track_physics(timeline_state_t *ts, int track_index) {
  const int group_index = model_track_group_index(ts, track_index);
  const int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return;
  // Inserting rather than appending is the game's problem: only it knows what
  // renumbering a player means for the rest of its world.
  gh_world_add_player(model_host(ts), ts->groups[group_index]->initial_world, local_index, NULL);
  ts->groups[group_index]->vec.current_size = 1;
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

// Recording & Merging

void model_insert_snippet_into_recording_track(player_track_t *track, const input_snippet_t *snippet) {
  if (track->recording_snippet_count >= track->recording_snippet_capacity) {
    track->recording_snippet_capacity = track->recording_snippet_capacity == 0 ? 8 : track->recording_snippet_capacity * 2;
    track->recording_snippets = realloc(track->recording_snippets, sizeof(input_snippet_t) * track->recording_snippet_capacity);
  }
  track->recording_snippets[track->recording_snippet_count] = *snippet;
  track->recording_snippet_count++;
}

void model_apply_input_to_main_buffer(timeline_state_t *ts, player_track_t *track, int tick, const input_record_t *input) {
  input_snippet_t *overlapping_snippet = NULL;
  for (int j = 0; j < track->snippet_count; ++j) {
    if (track->snippets[j].is_active && tick >= track->snippets[j].start_tick && tick < track->snippets[j].end_tick) {
      overlapping_snippet = &track->snippets[j];
      break;
    }
  }
  if (overlapping_snippet) {
    snippet_window(overlapping_snippet)[tick - overlapping_snippet->start_tick] = *input;
    return;
  }

  input_snippet_t *before = NULL;
  input_snippet_t *after = NULL;
  for (int j = 0; j < track->snippet_count; ++j) {
    if (track->snippets[j].is_active && track->snippets[j].end_tick == tick) before = &track->snippets[j];
    if (track->snippets[j].is_active && track->snippets[j].start_tick == tick + 1) after = &track->snippets[j];
  }

  if (before && after && input_effect_stack_equal(before->effects, before->effect_count, after->effects, after->effect_count)) {
    int old_before_duration = before->input_count;
    int after_duration = after->input_count;
    // Swallowing `after` into `before` rewrites both buffers, so read the window before resizing.
    input_record_t *after_window = snippet_window(after);
    model_resize_snippet_inputs(ts, before, old_before_duration + 1 + after_duration);
    before->inputs[old_before_duration] = *input;
    memcpy(&before->inputs[old_before_duration + 1], after_window, sizeof(input_record_t) * after_duration);
    model_remove_snippet_from_track(ts, track, after->id);
    model_compact_layers_for_track(track);
  } else if (before) {
    model_resize_snippet_inputs(ts, before, before->input_count + 1);
    before->inputs[before->input_count - 1] = *input;
  } else if (after) {
    // Recording into the tick before a snippet just widens its window by one.
    if (model_trim_snippet(ts, after, after->start_tick - 1, after->end_tick)) {
      snippet_window(after)[0] = *input;
    }
  } else {
    input_snippet_t new_snippet = {0};
    new_snippet.id = ts->next_snippet_id++;
    new_snippet.start_tick = tick;
    new_snippet.end_tick = tick + 1;
    new_snippet.is_active = true;
    new_snippet.input_count = 1;
    new_snippet.inputs = calloc(1, sizeof(input_record_t));
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

void model_reset_physics_cache(timeline_state_t *ts) {
  if (!ts) return;
  game_host_t *host = model_host(ts);
  if (!host) return;
  ts->current_tick = imax(ts->current_tick, model_get_min_global_tick(ts));
  for (int i = 0; i < ts->group_count; ++i) {
    timeline_group_t *group = ts->groups[i];
    group->vec.current_size = 1;
    group->cached_tick = -1;
    gh_world_copy(host, group->previous_world, group->initial_world);
    if (group->vec.data && group->vec.data[0]) gh_world_copy(host, group->vec.data[0], group->initial_world);
  }
}

void model_recalc_physics(timeline_state_t *ts, int tick) {
  (void)tick;
  input_effects_invalidate(ts);
  model_reset_physics_cache(ts);
}

input_record_t model_get_input_at_tick(const timeline_state_t *ts, int track_index, int tick) {
  if (!ts->recording && !ts->input_effects_rebuilding) input_effects_ensure((timeline_state_t *)ts);
  const player_track_t *track = &ts->player_tracks[track_index];
  input_record_t blank;
  engine_input_default(model_host(ts), &blank);
  input_record_t last_valid_input = blank;
  int last_input_tick = -1;

  if (ts->recording) {
    for (int i = 0; i < track->recording_snippet_count; ++i) {
      const input_snippet_t *snippet = &track->recording_snippets[i];
      if (snippet->is_active) {
        const input_record_t *inputs = input_effects_snippet_window(snippet);
        if (tick >= snippet->start_tick && tick < snippet->end_tick) return inputs[tick - snippet->start_tick];
        if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
          last_input_tick = snippet->end_tick - 1;
          last_valid_input = inputs[snippet->input_count - 1];
        }
      }
    }
  }

  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active) {
      const input_record_t *inputs = input_effects_snippet_window(snippet);
      if (tick >= snippet->start_tick && tick < snippet->end_tick) return inputs[tick - snippet->start_tick];
      if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
        last_input_tick = snippet->end_tick - 1;
        last_valid_input = inputs[snippet->input_count - 1];
      }
    }
  }

  if (tick > last_input_tick && last_input_tick != -1) {
    // Stateful controls carry beyond the end of a snippet, but one-shot
    // requests must only exist on the authored tick.
    engine_input_reset_triggers(model_host(ts), &last_valid_input);
    return last_valid_input;
  }
  return blank;
}

void model_advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, model_get_min_global_tick(ts));

  if (ts->recording) {
    int group_tick = model_group_playhead_tick(ts, ts->active_group_index);
    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track_t *track = &ts->player_tracks[i];
      if (track->group_index != ts->active_group_index) continue;
      if (i != ts->selected_player_track_index &&
          (!game_has_cap(&ts->ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS) || !track->is_linked)) continue;
      input_snippet_t *active_rec_snip = NULL;
      if (track->recording_snippet_count > 0) active_rec_snip = &track->recording_snippets[track->recording_snippet_count - 1];
      if (active_rec_snip) {
        // Calculate where the current tick is relative to the start of the snippet
        int relative_tick = group_tick - active_rec_snip->start_tick;

        // ONLY append if we are past the end of the current recording snippet.
        // DO NOT overwrite if we are rewinding/scrubbing inside the snippet.
        if (relative_tick > active_rec_snip->input_count) {
          int old_count = active_rec_snip->input_count;
          int needed = relative_tick;
          model_resize_snippet_inputs(ts, active_rec_snip, needed);

          // Fill the first new tick with the current input, then consume any
          // one-shot fields. This also keeps a lag frame that advances several
          // ticks from turning one press into several trigger ticks.
          input_record_t sampled_input = track->current_input;
          for (int s = old_count; s < needed; ++s) {
            active_rec_snip->inputs[s] = sampled_input;
            engine_input_reset_triggers(model_host(ts), &sampled_input);
          }
          track->current_input = sampled_input;
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

// Advances `world` to `target_tick`, feeding each player the input the timeline
// holds for it. This is the whole of the engine's simulation: pick a starting
// point, step, cache. What a step does is entirely the game's business.
static void simulate_to(timeline_state_t *ts, int group_index, ft_world *world, int target_tick) {
  game_host_t *host = model_host(ts);
  timeline_group_t *group = ts->groups[group_index];
  const int step = 50;

  const int player_count = gh_world_player_count(host, world);
  const size_t input_size = game_input_size(host);
  uint8_t *inputs = player_count > 0 ? calloc((size_t)player_count, input_size) : NULL;

  while (gh_world_tick(host, world) < target_tick) {
    const int current_sim_tick = gh_world_tick(host, world);

    for (int p = 0; p < player_count; ++p) {
      const int track_index = model_group_track_index(ts, group_index, p);
      input_record_t record;
      if (track_index >= 0) record = model_get_input_at_tick(ts, track_index, current_sim_tick);
      else engine_input_default(host, &record);
      memcpy(inputs + (size_t)p * input_size, record.bytes, input_size);
    }

    // The ABI promises the game a tightly packed array using its own record
    // size, not the editor's larger per-tick storage wrapper.
    gh_world_step(host, world, inputs, (unsigned)player_count);

    if (gh_world_tick(host, world) % step == 0) {
      const int cache_index = gh_world_tick(host, world) / step;
      if ((uint32_t)cache_index >= group->vec.current_size) v_push(ts, &group->vec, world, group_index);
      else gh_world_copy(host, group->vec.data[cache_index], world);
    }
  }

  free(inputs);
}

// Picks the cheapest starting point for reaching `tick`: the cached world when
// it is already there, the nearest periodic snapshot when jumping or rewinding,
// and otherwise wherever the last request left off.
static void seed_world(timeline_state_t *ts, int group_index, ft_world *out_world, int tick) {
  game_host_t *host = model_host(ts);
  timeline_group_t *group = ts->groups[group_index];
  const int step = 50;
  const int previous_tick = gh_world_tick(host, group->previous_world);

  if (tick < previous_tick || (tick - previous_tick) > 100) {
    int base_index = (tick - 1) / step;
    if (base_index > (int)group->vec.current_size - 1) base_index = (int)group->vec.current_size - 1;
    if (base_index < 0) base_index = 0;
    gh_world_copy(host, out_world, group->vec.data[base_index]);
  } else {
    gh_world_copy(host, out_world, group->previous_world);
  }
}

const ft_world *model_group_world_at_tick(timeline_state_t *ts, int group_index, int tick) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return NULL;
  /* Rebuild effects before seeding a simulation world. Rebuilding may ask the
   * timeline for reference worlds and reset its caches; doing that lazily from
   * model_get_input_at_tick would overwrite a simulation already in progress. */
  if (!ts->recording && !ts->input_effects_rebuilding) input_effects_ensure(ts);
  game_host_t *host = model_host(ts);
  timeline_group_t *group = ts->groups[group_index];
  tick = imax(0, tick - group->start_offset);
  ts->simulation_group_index = group_index;

  if (group->cached_tick == tick) return group->world_cached;

  seed_world(ts, group_index, group->world_cached, tick);
  simulate_to(ts, group_index, group->world_cached, tick);
  gh_world_copy(host, group->previous_world, group->world_cached);
  group->cached_tick = tick;
  return group->world_cached;
}

void model_group_world_pair(timeline_state_t *ts, int group_index, int tick, const ft_world **out_prev, const ft_world **out_cur) {
  if (out_prev) *out_prev = NULL;
  if (out_cur) *out_cur = NULL;
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;

  /* See model_group_world_at_tick: effects must never rebuild from inside the
   * simulation that is consuming their output. */
  if (!ts->recording && !ts->input_effects_rebuilding) input_effects_ensure(ts);

  game_host_t *host = model_host(ts);
  timeline_group_t *group = ts->groups[group_index];
  const int local_tick = imax(0, tick - group->start_offset);

  if (local_tick <= 0) {
    const ft_world *world = model_group_world_at_tick(ts, group_index, tick);
    if (out_prev) *out_prev = world;
    if (out_cur) *out_cur = world;
    return;
  }

  if (group->cached_tick == local_tick - 1) {
    // Fast path for sequential forward playback: the current world becomes previous,
    // and we only simulate the 1 single new tick!
    ts->simulation_group_index = group_index;
    gh_world_copy(host, group->prev_world_cached, group->world_cached);
    simulate_to(ts, group_index, group->world_cached, local_tick);
    gh_world_copy(host, group->previous_world, group->world_cached);
    group->cached_tick = local_tick;
  } else if (group->cached_tick != local_tick) {
    // Reach the tick before the wanted one first, keep it, then take the last
    // step. That gives both worlds for interpolation from one simulation run.
    ts->simulation_group_index = group_index;
    seed_world(ts, group_index, group->world_cached, local_tick - 1);
    simulate_to(ts, group_index, group->world_cached, local_tick - 1);
    gh_world_copy(host, group->prev_world_cached, group->world_cached);
    simulate_to(ts, group_index, group->world_cached, local_tick);
    gh_world_copy(host, group->previous_world, group->world_cached);
    group->cached_tick = local_tick;
  } else if (gh_world_tick(host, group->prev_world_cached) != local_tick - 1) {
    // A direct world lookup (camera, inspector, prediction, etc.) may have
    // filled the current-tick cache without filling its interpolation partner.
    // Rebuild only the missing previous world and leave the current one intact.
    ts->simulation_group_index = group_index;
    seed_world(ts, group_index, group->prev_world_cached, local_tick - 1);
    simulate_to(ts, group_index, group->prev_world_cached, local_tick - 1);
  }

  if (out_prev) *out_prev = group->prev_world_cached;
  if (out_cur) *out_cur = group->world_cached;
}

const ft_world *model_world_at_tick(timeline_state_t *ts, int tick) {
  return model_group_world_at_tick(ts, ts->active_group_index, tick);
}

void model_apply_starting_config(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  game_host_t *host = model_host(ts);
  player_track_t *track = &ts->player_tracks[track_index];
  starting_config_t *sc = &track->starting_config;
  const int group_index = model_track_group_index(ts, track_index);
  const int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return;

  ft_world *world = ts->groups[group_index]->initial_world;
  if (local_index >= gh_world_player_count(host, world)) return;
  if (!sc->enabled) return;

  // Each override names a property the game published. The engine never has to
  // know what any of them mean.
  for (int i = 0; i < sc->override_count; ++i) {
    const int prop = model_find_player_prop(host, sc->overrides[i].prop_id);
    if (prop < 0) continue;
    gh_entity_prop_set(host, world, FT_ENTITY_CLASS_PLAYER, local_index, (unsigned)prop, &sc->overrides[i].value);
  }
  model_recalc_physics(ts, 0);
}

// Puts a group's starting world back the way the level made it, then re-applies
// whatever overrides are still enabled on its tracks.
//
// Overrides are written straight into the starting world, so turning one off
// cannot simply "unset" it: the only thing that knows what the level said is
// the level. Rebuilding from it and re-applying the rest is what restores a
// pristine start.
void model_rebuild_group_start(timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;
  game_host_t *host = model_host(ts);
  const ft_level *level = ts->ui->gfx_handler->level;
  if (!level) return;

  timeline_group_t *group = ts->groups[group_index];
  ft_world *pristine = gh_world_create(host, level, 0, group_index);
  if (!pristine) return;

  const int tracks = model_group_track_count(ts, group_index);
  for (int p = gh_world_player_count(host, pristine); p < tracks; ++p)
    gh_world_add_player(host, pristine, -1, NULL);
  while (gh_world_player_count(host, pristine) > tracks)
    gh_world_remove_player(host, pristine, gh_world_player_count(host, pristine) - 1);

  gh_world_copy(host, group->initial_world, pristine);
  gh_world_destroy(host, pristine);

  for (int track_index = 0; track_index < ts->player_track_count; ++track_index) {
    if (ts->player_tracks[track_index].group_index != group_index) continue;
    if (ts->player_tracks[track_index].starting_config.enabled) model_apply_starting_config(ts, track_index);
  }
  model_recalc_physics(ts, 0);
}

void model_rebind_starting_strings(starting_config_t *config) {
  if (!config) return;
  int count = config->override_count;
  if (count < 0) count = 0;
  if (count > MAX_STARTING_OVERRIDES) count = MAX_STARTING_OVERRIDES;
  for (int i = 0; i < count; ++i) {
    starting_override_t *override = &config->overrides[i];
    if (override->value.kind == FT_VALUE_STRING)
      override->value.as.s = override->string_value;
  }
}

player_track_t *model_clone_track_to_group(timeline_state_t *ts, int track_index, int group_index, int *out_track_index) {
  if (!ts || track_index < 0 || track_index >= ts->player_track_count || group_index < 0 || group_index >= ts->group_count) return NULL;

  const player_track_t *source = &ts->player_tracks[track_index];
  player_track_t copy = *source;
  model_rebind_starting_strings(&copy.starting_config);
  copy.snippets = NULL;
  copy.snippet_capacity = source->snippet_count;
  copy.recording_snippets = NULL;
  copy.recording_snippet_count = 0;
  copy.recording_snippet_capacity = 0;
  if (source->snippet_count > 0) {
    copy.snippets = calloc((size_t)source->snippet_count, sizeof(input_snippet_t));
    if (!copy.snippets) return NULL;
    for (int i = 0; i < source->snippet_count; ++i) {
      model_snippet_clone(&copy.snippets[i], &source->snippets[i]);
      copy.snippets[i].id = ts->next_snippet_id++;
    }
  }
  copy.group_index = group_index;
  snprintf(copy.name, sizeof(copy.name), "%.*s copy", (int)sizeof(copy.name) - 6,
           source->name[0] ? source->name : "Track");

  int old_active = ts->active_group_index;
  ts->active_group_index = group_index;
  player_track_t *new_track = model_add_new_track(ts, 1);
  ts->active_group_index = old_active;
  if (!new_track) {
    for (int i = 0; i < copy.snippet_count; ++i)
      model_free_snippet_inputs(&copy.snippets[i]);
    free(copy.snippets);
    return NULL;
  }

  int new_index = (int)(new_track - ts->player_tracks);
  // model_add_new_track created the corresponding character and a pointer-free default row.
  *new_track = copy;
  model_rebind_starting_strings(&new_track->starting_config);
  if (out_track_index) *out_track_index = new_index;
  if (copy.starting_config.enabled) model_apply_starting_config(ts, new_index);
  model_recalc_physics(ts, 0);
  return new_track;
}

bool model_remove_group(timeline_state_t *ts, int group_index) {
  if (!ts || ts->recording || ts->group_count <= 1 || group_index < 0 || group_index >= ts->group_count) return false;

  for (int i = ts->player_track_count - 1; i >= 0; --i) {
    if (ts->player_tracks[i].group_index == group_index) model_remove_track_logic(ts, i);
  }

  group_runtime_cleanup(ts, ts->groups[group_index]);
  free(ts->groups[group_index]);
  memmove(&ts->groups[group_index], &ts->groups[group_index + 1],
          sizeof(*ts->groups) * (size_t)(ts->group_count - group_index - 1));
  ts->group_count--;
  ts->groups = realloc(ts->groups, sizeof(*ts->groups) * (size_t)ts->group_count);

  for (int i = 0; i < ts->player_track_count; ++i)
    if (ts->player_tracks[i].group_index > group_index) ts->player_tracks[i].group_index--;
  for (int i = 0; i < ts->event_count;) {
    if (ts->events[i].group_index == group_index) {
      memmove(&ts->events[i], &ts->events[i + 1], sizeof(timeline_event_t) * (size_t)(ts->event_count - i - 1));
      ts->event_count--;
      continue;
    }
    if (ts->events[i].group_index > group_index) ts->events[i].group_index--;
    ++i;
  }

  if (ts->active_group_index == group_index) ts->active_group_index = 0;
  else if (ts->active_group_index > group_index) ts->active_group_index--;
  if (group_index == 0 && ts->group_count > 0) ts->groups[0]->start_offset = 0;
  model_recalc_physics(ts, 0);
  return true;
}

static int model_find_group_race_start(timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return -1;

  game_host_t *host = model_host(ts);
  const int tps = game_ticks_per_second(host);

  // Simulate far enough past the last authored input to catch a start that only
  // happens once the run gets going.
  int max_local_tick = tps * 10;
  for (int track_index = 0; track_index < ts->player_track_count; ++track_index) {
    if (ts->player_tracks[track_index].group_index != group_index) continue;
    const player_track_t *track = &ts->player_tracks[track_index];
    for (int snippet_index = 0; snippet_index < track->snippet_count; ++snippet_index)
      max_local_tick = imax(max_local_tick, track->snippets[snippet_index].end_tick + tps * 10);
  }

  // Index -1 marks a scratch world: it is simulated to answer a question, not
  // shown, so a game must not raise effects into a visible world's state.
  ft_world *world = gh_world_create(host, ts->ui->gfx_handler->level, 0, -1);
  if (!world) return -1;
  gh_world_copy(host, world, ts->groups[group_index]->initial_world);

  const int players = gh_world_player_count(host, world);
  const size_t input_size = game_input_size(host);
  uint8_t *inputs = players > 0 ? calloc((size_t)players, input_size) : NULL;
  int race_start = -1;

  while (gh_world_tick(host, world) <= max_local_tick && race_start < 0) {
    for (int local_index = 0; local_index < players; ++local_index) {
      ft_player_view view;
      if (!gh_world_player_view(host, world, local_index, &view)) continue;
      // run_start_tick is the game-agnostic notion the editor aligns on.
      if (view.run_start_tick >= 0 && (race_start < 0 || view.run_start_tick < race_start)) race_start = view.run_start_tick;
    }
    if (race_start >= 0 || gh_world_tick(host, world) == max_local_tick) break;

    const int input_tick = gh_world_tick(host, world);
    for (int local_index = 0; local_index < players; ++local_index) {
      const int track_index = model_group_track_index(ts, group_index, local_index);
      input_record_t record;
      if (track_index >= 0) record = model_get_input_at_tick(ts, track_index, input_tick);
      else engine_input_default(host, &record);
      memcpy(inputs + (size_t)local_index * input_size, record.bytes, input_size);
    }
    gh_world_step(host, world, inputs, (unsigned)players);
  }

  free(inputs);
  gh_world_destroy(host, world);
  return race_start;
}

void model_align_group_starts(timeline_state_t *ts) {
  if (!ts || ts->group_count < 2) return;
  int *starts = malloc(sizeof(int) * (size_t)ts->group_count);
  if (!starts) return;

  // Simulate fresh worlds rather than consulting the viewport caches. This makes alignment work
  // after edits, imports, and rewinds even when a group's cached world predates the race start.
  for (int group_index = 0; group_index < ts->group_count; ++group_index)
    starts[group_index] = model_find_group_race_start(ts, group_index);

  // Group 1 owns the controlled playhead, so its race start is the natural alignment anchor.
  // Signed offsets are required when another group reaches the start line later than Group 1.
  int anchor_group = starts[0] >= 0 ? 0 : -1;
  for (int group_index = 1; anchor_group < 0 && group_index < ts->group_count; ++group_index)
    if (starts[group_index] >= 0) anchor_group = group_index;

  if (anchor_group >= 0) {
    int anchor_tick = starts[anchor_group] + (anchor_group == 0 ? 0 : ts->groups[anchor_group]->start_offset);
    for (int group_index = 1; group_index < ts->group_count; ++group_index) {
      if (starts[group_index] >= 0)
        ts->groups[group_index]->start_offset = anchor_tick - starts[group_index];
    }
    ts->groups[0]->start_offset = 0;
    model_recalc_physics(ts, 0);
  }
  free(starts);
}

// Static Physics Vector Helpers
static void v_init(timeline_state_t *ts, physics_v_t *t, int world_index) {
  t->current_size = 1;
  t->max_size = 1;
  t->data = calloc(1, sizeof(ft_world *));
  if (t->data) t->data[0] = gh_world_create(model_host(ts), ts->ui->gfx_handler->level, 0, world_index);
}

static void v_destroy(timeline_state_t *ts, physics_v_t *t) {
  game_host_t *host = model_host(ts);
  for (uint32_t i = 0; i < t->max_size; ++i)
    gh_world_destroy(host, t->data[i]);
  free(t->data);
  t->data = NULL;
  t->current_size = 0;
  t->max_size = 0;
}

// Worlds are handles now, so growing the ring no longer has to repair any
// interior pointers: nothing moves when the array of pointers is reallocated.
static void v_push(timeline_state_t *ts, physics_v_t *t, const ft_world *world, int world_index) {
  game_host_t *host = model_host(ts);
  ++t->current_size;
  if (t->current_size > t->max_size) {
    const uint32_t old_max = t->max_size;
    t->max_size *= 2;
    ft_world **new_data = realloc(t->data, t->max_size * sizeof(ft_world *));
    if (!new_data) {
      t->current_size = old_max;
      t->max_size = old_max;
      return;
    }
    t->data = new_data;
    for (uint32_t i = old_max; i < t->max_size; ++i)
      t->data[i] = gh_world_create(host, ts->ui->gfx_handler->level, 0, world_index);
  }
  gh_world_copy(host, t->data[t->current_size - 1], world);
}

int model_find_player_prop(game_host_t *host, const char *prop_id) {
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  if (!player_class || !prop_id) return -1;
  for (uint32_t i = 0; i < player_class->prop_count; ++i) {
    if (player_class->props[i].id && strcmp(player_class->props[i].id, prop_id) == 0) return (int)i;
  }
  return -1;
}
