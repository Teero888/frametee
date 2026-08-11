#include "timeline_model.h"
#include "ddnet_physics/collision.h"
#include "ddnet_physics/gamecore.h"
#include "ddnet_physics/vmath.h"
#include <limits.h>
#include <math.h>
#include <particles/particle_system.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>
#include <user_interface/widgets/hsl_colorpicker.h>
#include <user_interface/net_events.h>

#define DEFAULT_TRACK_HEIGHT 60.f

// Forward Declarations for Static Helpers
static void v_init(physics_v_t *t);
static void v_destroy(physics_v_t *t);
static void v_push(physics_v_t *t, SWorldCore *world);

static void ui_particle_callback(mvec2 pos, int type, int cid, void *user_data) {
  timeline_state_t *ts = (timeline_state_t *)user_data;
  ui_handler_t *ui = ts->ui;
  if (ts->simulation_group_index < 0 || ts->simulation_group_index >= ts->group_count) return;
  particle_system_t *ps = &ts->groups[ts->simulation_group_index]->particle_system;
  vec2 p = {vgetx(pos), vgety(pos)};

  vec2 zero_vel = {0, -1};
  float default_alpha = 1.0f;
  float time_passed = 0.0f;

  if (type == PARTICLE_TYPE_SMOKE) particles_create_smoke(ps, p, zero_vel, default_alpha, time_passed);
  else if (type == PARTICLE_TYPE_PLAYER_SPAWN) particles_create_player_spawn(ps, p, default_alpha);
  else if (type == PARTICLE_TYPE_PLAYER_DEATH) {
    // TODO: the coloring is different on ddnet i can't figure it out
    // TODO: this is also buggy since we don't re-push color when the player color changes
    vec4 col = {1, 1, 1, 1};
    int track_index = model_group_track_index(ts, ts->simulation_group_index, cid);
    if (track_index >= 0 && ui->timeline.player_tracks[track_index].player_info.use_custom_color)
      packed_hsl_to_rgb(ui->timeline.player_tracks[track_index].player_info.color_body, col);
    particles_create_player_death(ps, p, col);
  } else if (type == PARTICLE_TYPE_AIR_JUMP) particles_create_air_jump(ps, p, default_alpha);
  else if (type == PARTICLE_TYPE_BULLET_TRAIL) particles_create_bullet_trail(ps, p, default_alpha, time_passed);
  else if (type == PARTICLE_TYPE_BULLET_STARS) particles_create_star(ps, p);
  else if (type == PARTICLE_TYPE_EXPLOSION) particles_create_explosion(ps, p);
  else if (type == PARTICLE_TYPE_HAMMER_HIT) particles_create_hammer_hit(ps, p, default_alpha);
  else if (type == PARTICLE_TYPE_CONFETTI) particles_create_confetti(ps, p, default_alpha);
}

static void ui_damage_indicator_callback(mvec2 pos, float angle, int amount, int cid, void *user_data) {
  (void)cid;
  timeline_state_t *ts = (timeline_state_t *)user_data;
  if (ts->simulation_group_index < 0 || ts->simulation_group_index >= ts->group_count) return;
  particle_system_t *ps = &ts->groups[ts->simulation_group_index]->particle_system;
  vec2 p = {vgetx(pos), vgety(pos)};
  const float pi = 3.14159265358979323846f;
  const float center = 3.0f * pi / 2.0f + angle;
  const float start = center - pi / 3.0f;
  const float end = center + pi / 3.0f;
  for (int i = 0; i < amount; ++i) {
    const float indicator_angle = start + (end - start) * (float)(i + 1) / (float)(amount + 1);
    vec2 dir = {cosf(indicator_angle), sinf(indicator_angle)};
    particles_create_damage_ind(ps, p, dir, 1.0f);
  }
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

static void group_runtime_init(timeline_group_t *group) {
  v_init(&group->vec);
  group->initial_world = wc_empty();
  group->previous_world = wc_empty();
  group->prev_world_cached = wc_empty();
  group->world_cached = wc_empty();
  group->cached_tick = -1;
  particle_system_init(&group->particle_system);
}

static void group_runtime_cleanup(timeline_group_t *group) {
  if (!group) return;
  v_destroy(&group->vec);
  wc_free(&group->initial_world);
  wc_free(&group->previous_world);
  wc_free(&group->prev_world_cached);
  wc_free(&group->world_cached);
  particle_system_cleanup(&group->particle_system);
}

timeline_group_t *model_add_group(timeline_state_t *ts, const char *name) {
  if (!ts) return NULL;
  timeline_group_t *group = calloc(1, sizeof(*group));
  if (!group) return NULL;

  int index = ts->group_count;
  snprintf(group->name, sizeof(group->name), "%s", name && name[0] ? name : "Group");
  memcpy(group->color, s_group_colors[index % (int)(sizeof(s_group_colors) / sizeof(s_group_colors[0]))], sizeof(group->color));
  group->visible = true;
  group->demo_export_enabled = true;
  group_runtime_init(group);

  timeline_group_t **groups = realloc(ts->groups, sizeof(*groups) * (size_t)(index + 1));
  if (!groups) {
    group_runtime_cleanup(group);
    free(group);
    return NULL;
  }
  ts->groups = groups;
  ts->groups[index] = group;
  ts->group_count++;

  if (ts->ui && ts->ui->gfx_handler && ts->ui->gfx_handler->physics_handler.loaded) {
    wc_copy_world(&group->initial_world, &ts->ui->gfx_handler->physics_handler.world);
    wc_copy_world(&group->vec.data[0], &group->initial_world);
    wc_copy_world(&group->previous_world, &group->initial_world);
  }
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

void model_reset_groups_for_map(timeline_state_t *ts) {
  if (!ts || !ts->ui || !ts->ui->gfx_handler) return;
  SWorldCore *base = &ts->ui->gfx_handler->physics_handler.world;
  for (int i = 0; i < ts->group_count; ++i) {
    timeline_group_t *group = ts->groups[i];
    wc_copy_world(&group->initial_world, base);
    wc_copy_world(&group->vec.data[0], base);
    wc_copy_world(&group->previous_world, base);
    wc_free(&group->prev_world_cached);
    wc_free(&group->world_cached);
    group->prev_world_cached = wc_empty();
    group->world_cached = wc_empty();
    group->vec.current_size = 1;
    group->cached_tick = -1;
    particle_system_reset(&group->particle_system);
  }
}

// Initialization and Cleanup

void model_init(timeline_state_t *ts, ui_handler_t *ui) {
  ts->ui = ui;

  ts->gui_playback_speed = 50;
  ts->playback_speed = 50;
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

  ts->dummy_action_priority[0] = DUMMY_ACTION_COPY;
  ts->dummy_action_priority[1] = DUMMY_ACTION_INPUTS;

  ts->net_events = NULL;
  ts->net_event_count = 0;
  ts->net_event_capacity = 0;

  snippet_id_vector_init(&ts->selected_snippets);
  model_add_group(ts, "Group 1");
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

  if (ts->net_events) {
    free(ts->net_events);
  }

  for (int i = 0; i < ts->group_count; ++i) {
    group_runtime_cleanup(ts->groups[i]);
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
  snippet->source_offset = 0;
  snippet->source_count = new_duration;

  int preserve_count = (old_count < new_duration) ? old_count : new_duration;
  if (snippet->end_tick <= ts->current_tick) model_recalc_physics(ts, snippet->start_tick + preserve_count);
}

void model_free_snippet_inputs(input_snippet_t *snippet) {
  free(snippet->inputs);
  snippet->inputs = NULL;
  snippet->input_count = 0;
  snippet->source_offset = 0;
  snippet->source_count = 0;
}

void model_snippet_clone(input_snippet_t *dest, const input_snippet_t *src) {
  *dest = *src;
  // The whole source travels with the copy, not just the visible window, so a cloned snippet can be
  // widened back out to everything the original held.
  if (src->inputs && src->source_count > 0) {
    dest->inputs = malloc(src->source_count * sizeof(SPlayerInput));
    memcpy(dest->inputs, src->inputs, src->source_count * sizeof(SPlayerInput));
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

  SPlayerInput *flat = malloc(sizeof(SPlayerInput) * snippet->input_count);
  if (!flat) return;
  memcpy(flat, snippet_window(snippet), sizeof(SPlayerInput) * snippet->input_count);
  free(snippet->inputs);
  snippet->inputs = flat;
  snippet->source_offset = 0;
  snippet->source_count = snippet->input_count;
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
    SPlayerInput *grown = realloc(snippet->inputs, sizeof(SPlayerInput) * (snippet->source_count + pad));
    if (!grown) return false;
    snippet->inputs = grown;
    if (snippet->source_count > 0) memmove(&snippet->inputs[pad], snippet->inputs, sizeof(SPlayerInput) * snippet->source_count);
    memset(snippet->inputs, 0, sizeof(SPlayerInput) * pad);
    snippet->source_count += pad;
    snippet->source_offset += pad;
    new_offset = 0;
  }

  if (new_offset + new_count > snippet->source_count) {
    int pad = new_offset + new_count - snippet->source_count;
    SPlayerInput *grown = realloc(snippet->inputs, sizeof(SPlayerInput) * (snippet->source_count + pad));
    if (!grown) return false;
    snippet->inputs = grown;
    memset(&snippet->inputs[snippet->source_count], 0, sizeof(SPlayerInput) * pad);
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

player_track_t *model_add_new_track(timeline_state_t *ts, physics_handler_t *ph, int num) {
  (void)ph;
  if (num <= 0) return NULL;
  if (ts->active_group_index < 0 || ts->active_group_index >= ts->group_count) return NULL;

  timeline_group_t *group = ts->groups[ts->active_group_index];
  if (wc_add_character(&group->initial_world, num) == NULL) return NULL;
  int existing_group_count = model_group_track_count(ts, ts->active_group_index);

  int insert_index = ts->player_track_count;
  for (int i = ts->player_track_count - 1; i >= 0; --i) {
    if (ts->player_tracks[i].group_index == ts->active_group_index) {
      insert_index = i + 1;
      break;
    }
  }
  int new_count = ts->player_track_count + num;
  ts->player_tracks = realloc(ts->player_tracks, sizeof(player_track_t) * new_count);
  memmove(&ts->player_tracks[insert_index + num], &ts->player_tracks[insert_index],
          sizeof(player_track_t) * (size_t)(ts->player_track_count - insert_index));

  for (int i = 0; i < num; i++) {
    player_track_t *new_track = &ts->player_tracks[insert_index + i];
    memset(new_track, 0, sizeof(player_track_t));
    snprintf(new_track->name, sizeof(new_track->name), "Track %d", existing_group_count + i + 1);
    new_track->group_index = ts->active_group_index;
    new_track->dummy_copy_flags = COPY_ALL;
    new_track->demo_export_enabled = true;
    new_track->demo_ping = 0;
  }

  ts->player_track_count = new_count;
  model_recalc_physics(ts, 0);

  return &ts->player_tracks[insert_index];
}

void model_remove_track_logic(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  int group_index = model_track_group_index(ts, track_index);
  int local_index = model_group_local_track_index(ts, track_index);
  if (group_index >= 0 && local_index >= 0)
    wc_remove_character(&ts->groups[group_index]->initial_world, local_index);

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
  int group_index = model_track_group_index(ts, track_index);
  int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return;
  wc_insert_character_at_index(&ts->groups[group_index]->initial_world, local_index);
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

void model_apply_input_to_main_buffer(timeline_state_t *ts, player_track_t *track, int tick, const SPlayerInput *input) {
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

  if (before && after) {
    int old_before_duration = before->input_count;
    int after_duration = after->input_count;
    // Swallowing `after` into `before` rewrites both buffers, so read the window before resizing.
    SPlayerInput *after_window = snippet_window(after);
    model_resize_snippet_inputs(ts, before, old_before_duration + 1 + after_duration);
    before->inputs[old_before_duration] = *input;
    memcpy(&before->inputs[old_before_duration + 1], after_window, sizeof(SPlayerInput) * after_duration);
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
  (void)tick;
  ts->current_tick = imax(ts->current_tick, model_get_min_global_tick(ts));
  for (int i = 0; i < ts->group_count; ++i) {
    timeline_group_t *group = ts->groups[i];
    group->vec.current_size = 1;
    group->cached_tick = -1;
    wc_copy_world(&group->previous_world, &group->initial_world);
    wc_copy_world(&group->vec.data[0], &group->initial_world);
    particle_system_reset(&group->particle_system);
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
        if (tick >= snippet->start_tick && tick < snippet->end_tick) return snippet_window(snippet)[tick - snippet->start_tick];
        if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
          last_input_tick = snippet->end_tick - 1;
          last_valid_input = snippet_window(snippet)[snippet->input_count - 1];
        }
      }
    }
  }

  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active) {
      if (tick >= snippet->start_tick && tick < snippet->end_tick) return snippet_window(snippet)[tick - snippet->start_tick];
      if (snippet->end_tick <= tick && snippet->end_tick - 1 > last_input_tick && snippet->input_count > 0) {
        last_input_tick = snippet->end_tick - 1;
        last_valid_input = snippet_window(snippet)[snippet->input_count - 1];
      }
    }
  }

  if (tick > last_input_tick && last_input_tick != -1) return last_valid_input;
  return (SPlayerInput){.m_TargetY = -1};
}

void model_advance_tick(timeline_state_t *ts, int steps) {
  ts->current_tick = imax(ts->current_tick + steps, model_get_min_global_tick(ts));

  if (ts->recording) {
    int group_tick = model_group_playhead_tick(ts, ts->active_group_index);
    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track_t *track = &ts->player_tracks[i];
      if (track->group_index != ts->active_group_index) continue;
      if (i != ts->selected_player_track_index && !track->is_dummy) continue;
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

          // Fill the gap (which should be 'steps' wide) with the current input
          for (int s = old_count; s < needed; ++s) {
            active_rec_snip->inputs[s] = track->current_input;
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

void model_get_group_world_state_at_tick(timeline_state_t *ts, int group_index, int tick, SWorldCore *out_world, bool effects) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;
  timeline_group_t *group = ts->groups[group_index];
  tick = imax(0, tick - group->start_offset);
  ts->simulation_group_index = group_index;

  if (group->cached_tick == tick) {
    wc_copy_world(out_world, &group->world_cached);
    return;
  }
  const int step = 50;
  particle_system_t *ps = &group->particle_system;

  // Jump or Rewind Logic
  if (tick < group->previous_world.m_GameTick || (tick - group->previous_world.m_GameTick) > 100) {
    int raw_index = (tick - 1) / step;
    // Go back one extra snapshot to ensure we re-simulate recent particles
    // that might have expired in the future state we are rewinding from.
    // TODO: this doesnt solve the real issue of long lasting particles not being rendered when reversing
    if (effects && raw_index > 0) raw_index--;
    int base_index = imin(raw_index, group->vec.current_size - 1);
    if (base_index < 0) base_index = 0;
    wc_copy_world(out_world, &group->vec.data[base_index]);

    if (effects) {
      double snapshot_time = (double)out_world->m_GameTick / 50.0;
      particle_system_prune_by_time(ps, snapshot_time);

      ps->current_time = snapshot_time;
      ps->last_simulated_tick = out_world->m_GameTick - 1;
    }
  } else {
    wc_copy_world(out_world, &group->previous_world);
  }

  out_world->user_data = ts;

  while (out_world->m_GameTick < tick) {
    int current_sim_tick = out_world->m_GameTick;

    bool is_new_logic_tick = (effects && current_sim_tick > ps->last_simulated_tick);

    if (is_new_logic_tick) {
      out_world->particle = ui_particle_callback;
      out_world->damage_indicator = ui_damage_indicator_callback;
      ps->current_time = (double)current_sim_tick / 50.0;
      ps->rng_seed = current_sim_tick;
    } else {
      out_world->particle = NULL;
      out_world->damage_indicator = NULL;
    }

    for (int p = 0; p < out_world->m_NumCharacters; ++p) {
      int track_index = model_group_track_index(ts, group_index, p);
      SPlayerInput input = track_index >= 0 ? model_get_input_at_tick(ts, track_index, current_sim_tick) : (SPlayerInput){.m_TargetY = -1};
      cc_on_input(&out_world->m_pCharacters[p], &input);
    }

    wc_tick(out_world);

    // Auto-generate finish events during recording if option is enabled
    if (ts->recording && ts->ui->auto_generate_finish_events) {
      for (int p = 0; p < out_world->m_NumCharacters; ++p) {
        SCharacterCore *pChar = &out_world->m_pCharacters[p];
        if (pChar->m_StartTick != -1 && pChar->m_FinishTick == out_world->m_GameTick) {
          int track_index = model_group_track_index(ts, group_index, p);
          if (track_index >= 0) timeline_add_finish_events_for_character(ts, out_world->m_GameTick, pChar, track_index);
          timeline_mark_unsaved(ts);
        }
      }
    }

    // other effects
    if (is_new_logic_tick) {
      if (out_world->m_GameTick % 5 == 0) {
        for (int p = 0; p < out_world->m_NumCharacters; ++p) {
          SCharacterCore *core = &out_world->m_pCharacters[p];
          if (core->m_FreezeTime > 0) {
            vec2 p = {vgetx(core->m_Pos), vgety(core->m_Pos)};
            particles_create_freezing_flakes(ps, p, (vec2){32.0f, 32.0f}, 1.0f);
          }
        }
      }
      if (!out_world->m_UniqueRace) {
        for (int i = 0; i < ts->ui->num_ninja_pickups; ++i) {
          int p = ts->ui->ninja_pickup_indices[i];
          vec2 pos = {vgetx(ts->ui->pickup_positions[p]), vgety(ts->ui->pickup_positions[p])};
          particles_create_powerup_shine(ps, pos, (vec2){96, 18}, 1.0f);
        }
      }
    }

    if (is_new_logic_tick)
      ps->last_simulated_tick = current_sim_tick;

    if (out_world->m_GameTick % step == 0) {
      int cache_index = out_world->m_GameTick / step;
      if ((uint32_t)cache_index >= group->vec.current_size) v_push(&group->vec, out_world);
      else wc_copy_world(&group->vec.data[cache_index], out_world);
    }
  }

  out_world->particle = NULL;
  out_world->damage_indicator = NULL;
  wc_copy_world(&group->previous_world, out_world);
}

void model_get_group_world_state_pair(timeline_state_t *ts, int group_index, int tick, SWorldCore *out_prev_world, SWorldCore *out_world, bool effects) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return;
  timeline_group_t *group = ts->groups[group_index];
  int local_tick = imax(0, tick - group->start_offset);
  if (local_tick <= 0) {
    model_get_group_world_state_at_tick(ts, group_index, tick, out_prev_world, effects);
    model_get_group_world_state_at_tick(ts, group_index, tick, out_world, effects);
    return;
  }

  // Fast path: if the cached pair is already at 'tick'
  if (group->cached_tick == local_tick) {
    wc_copy_world(out_prev_world, &group->prev_world_cached);
    wc_copy_world(out_world, &group->world_cached);
    return;
  }

  // Fast path: playing forward by 1 tick (cached_tick == tick - 1)
  if (group->cached_tick == local_tick - 1) {
    // Current world at tick-1 becomes prev_world
    wc_copy_world(&group->prev_world_cached, &group->world_cached);
    wc_copy_world(out_prev_world, &group->prev_world_cached);

    // Simulate 1 tick from cached_world (which is at tick-1) to reach tick
    ts->simulation_group_index = group_index;
    group->world_cached.user_data = ts;
    int current_sim_tick = group->world_cached.m_GameTick;
    particle_system_t *ps = &group->particle_system;

    bool is_new_logic_tick = (effects && current_sim_tick > ps->last_simulated_tick);
    if (is_new_logic_tick) {
      group->world_cached.particle = ui_particle_callback;
      group->world_cached.damage_indicator = ui_damage_indicator_callback;
      ps->current_time = (double)current_sim_tick / 50.0;
      ps->rng_seed = current_sim_tick;
    } else {
      group->world_cached.particle = NULL;
      group->world_cached.damage_indicator = NULL;
    }

    for (int p = 0; p < group->world_cached.m_NumCharacters; ++p) {
      int track_index = model_group_track_index(ts, group_index, p);
      SPlayerInput input = track_index >= 0 ? model_get_input_at_tick(ts, track_index, current_sim_tick) : (SPlayerInput){.m_TargetY = -1};
      cc_on_input(&group->world_cached.m_pCharacters[p], &input);
    }

    wc_tick(&group->world_cached);

    if (is_new_logic_tick) {
      if (group->world_cached.m_GameTick % 5 == 0) {
        for (int p = 0; p < group->world_cached.m_NumCharacters; ++p) {
          SCharacterCore *core = &group->world_cached.m_pCharacters[p];
          if (core->m_FreezeTime > 0) {
            vec2 pos = {vgetx(core->m_Pos), vgety(core->m_Pos)};
            particles_create_freezing_flakes(ps, pos, (vec2){32.0f, 32.0f}, 1.0f);
          }
        }
      }
      if (!group->world_cached.m_UniqueRace) {
        for (int i = 0; i < ts->ui->num_ninja_pickups; ++i) {
          int p = ts->ui->ninja_pickup_indices[i];
          vec2 pos = {vgetx(ts->ui->pickup_positions[p]), vgety(ts->ui->pickup_positions[p])};
          particles_create_powerup_shine(ps, pos, (vec2){96, 18}, 1.0f);
        }
      }
      ps->last_simulated_tick = current_sim_tick;
    }

    group->world_cached.particle = NULL;
    group->world_cached.damage_indicator = NULL;
    wc_copy_world(out_world, &group->world_cached);
    group->cached_tick = local_tick;
    return;
  }

  // Full path: calculate state at tick - 1 and state at tick
  model_get_group_world_state_at_tick(ts, group_index, tick - 1, &group->prev_world_cached, effects);
  model_get_group_world_state_at_tick(ts, group_index, tick, &group->world_cached, effects);
  group->cached_tick = local_tick;

  wc_copy_world(out_prev_world, &group->prev_world_cached);
  wc_copy_world(out_world, &group->world_cached);
}

void model_get_world_state_at_tick(timeline_state_t *ts, int tick, SWorldCore *out_world, bool effects) {
  model_get_group_world_state_at_tick(ts, ts->active_group_index, tick, out_world, effects);
}

void model_get_world_state_pair(timeline_state_t *ts, int tick, SWorldCore *out_prev_world, SWorldCore *out_world, bool effects) {
  model_get_group_world_state_pair(ts, ts->active_group_index, tick, out_prev_world, out_world, effects);
}

void model_apply_starting_config(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  player_track_t *track = &ts->player_tracks[track_index];
  starting_config_t *sc = &track->starting_config;
  int group_index = model_track_group_index(ts, track_index);
  int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0 ||
      local_index >= ts->groups[group_index]->initial_world.m_NumCharacters) return;

  // Update the initial world state
  SCharacterCore *core = &ts->groups[group_index]->initial_world.m_pCharacters[local_index];
  core->m_Pos = vec2_init(sc->position[0] + 200 * 32, sc->position[1] + 200 * 32);
  core->m_PrevPos = vec2_init(sc->position[0] + 200 * 32, sc->position[1] + 200 * 32);
  core->m_Vel = vec2_init(sc->velocity[0], sc->velocity[1]);
  core->m_ActiveWeapon = sc->active_weapon;
  for (int i = 0; i < NUM_WEAPONS; ++i)
    core->m_aWeaponGot[i] = sc->has_weapons[i];
  if (core->m_aWeaponGot[WEAPON_NINJA]) {
    core->m_Ninja.m_ActivationTick = core->m_pWorld->m_GameTick;
    core->m_ActiveWeapon = WEAPON_NINJA;
  }
  cc_calc_indices(core);
  model_recalc_physics(ts, 0);
}

player_track_t *model_clone_track_to_group(timeline_state_t *ts, int track_index, int group_index, int *out_track_index) {
  if (!ts || track_index < 0 || track_index >= ts->player_track_count || group_index < 0 || group_index >= ts->group_count) return NULL;

  const player_track_t *source = &ts->player_tracks[track_index];
  player_track_t copy = *source;
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
  snprintf(copy.name, sizeof(copy.name), "%s copy", source->name[0] ? source->name : "Track");

  int old_active = ts->active_group_index;
  ts->active_group_index = group_index;
  player_track_t *new_track = model_add_new_track(ts, NULL, 1);
  ts->active_group_index = old_active;
  if (!new_track) {
    for (int i = 0; i < copy.snippet_count; ++i) model_free_snippet_inputs(&copy.snippets[i]);
    free(copy.snippets);
    return NULL;
  }

  int new_index = (int)(new_track - ts->player_tracks);
  // model_add_new_track created the corresponding character and a pointer-free default row.
  *new_track = copy;
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

  group_runtime_cleanup(ts->groups[group_index]);
  free(ts->groups[group_index]);
  memmove(&ts->groups[group_index], &ts->groups[group_index + 1],
          sizeof(*ts->groups) * (size_t)(ts->group_count - group_index - 1));
  ts->group_count--;
  ts->groups = realloc(ts->groups, sizeof(*ts->groups) * (size_t)ts->group_count);

  for (int i = 0; i < ts->player_track_count; ++i)
    if (ts->player_tracks[i].group_index > group_index) ts->player_tracks[i].group_index--;
  for (int i = 0; i < ts->net_event_count;) {
    if (ts->net_events[i].group_index == group_index) {
      memmove(&ts->net_events[i], &ts->net_events[i + 1], sizeof(net_event_t) * (size_t)(ts->net_event_count - i - 1));
      ts->net_event_count--;
      continue;
    }
    if (ts->net_events[i].group_index > group_index) ts->net_events[i].group_index--;
    ++i;
  }

  if (ts->active_group_index == group_index) ts->active_group_index = 0;
  else if (ts->active_group_index > group_index) ts->active_group_index--;
  if (group_index == 0 && ts->group_count > 0) ts->groups[0]->start_offset = 0;
  model_recalc_physics(ts, 0);
  return true;
}

static int model_find_group_race_start(const timeline_state_t *ts, int group_index) {
  if (!ts || group_index < 0 || group_index >= ts->group_count) return -1;

  int max_local_tick = GAME_TICK_SPEED * 10;
  for (int track_index = 0; track_index < ts->player_track_count; ++track_index) {
    if (ts->player_tracks[track_index].group_index != group_index) continue;
    const player_track_t *track = &ts->player_tracks[track_index];
    for (int snippet_index = 0; snippet_index < track->snippet_count; ++snippet_index)
      max_local_tick = imax(max_local_tick, track->snippets[snippet_index].end_tick + GAME_TICK_SPEED * 10);
  }

  SWorldCore world = wc_empty();
  wc_copy_world(&world, &ts->groups[group_index]->initial_world);
  int race_start = -1;
  while (world.m_GameTick <= max_local_tick && race_start < 0) {
    for (int local_index = 0; local_index < world.m_NumCharacters; ++local_index) {
      const SCharacterCore *character = &world.m_pCharacters[local_index];
      if (character->m_StartTick >= 0 && (race_start < 0 || character->m_StartTick < race_start))
        race_start = character->m_StartTick;
    }
    if (race_start >= 0 || world.m_GameTick == max_local_tick) break;

    int input_tick = world.m_GameTick;
    for (int local_index = 0; local_index < world.m_NumCharacters; ++local_index) {
      int track_index = model_group_track_index(ts, group_index, local_index);
      SPlayerInput input = track_index >= 0 ? model_get_input_at_tick(ts, track_index, input_tick) : (SPlayerInput){.m_TargetY = -1};
      cc_on_input(&world.m_pCharacters[local_index], &input);
    }
    wc_tick(&world);
  }
  wc_free(&world);
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
