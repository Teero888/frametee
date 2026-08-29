#include "input_effects.h"

#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/user_interface.h>

typedef struct effect_eval_context_t {
  timeline_state_t *timeline;
  int group_index;
} effect_eval_context_t;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) { return hash_bytes(hash, &value, sizeof(value)); }

static bool effect_input_at_tick(void *user, int32_t player, int32_t tick, void *out_record) {
  effect_eval_context_t *context = user;
  if (!context || !out_record) return false;
  const int track = model_group_track_index(context->timeline, context->group_index, player);
  if (track < 0) return false;
  const input_record_t input = model_get_input_at_tick(context->timeline, track, tick);
  memcpy(out_record, input.bytes, game_input_size(&context->timeline->ui->gfx_handler->game_host));
  return true;
}

static const ft_world *effect_world_at_tick(void *user, int32_t tick) {
  effect_eval_context_t *context = user;
  if (!context || context->group_index < 0 || context->group_index >= context->timeline->group_count) return NULL;
  const int global_tick = tick + context->timeline->groups[context->group_index]->start_offset;
  return model_group_world_at_tick(context->timeline, context->group_index, global_tick);
}

static void effect_reset_simulation(void *user) {
  effect_eval_context_t *context = user;
  if (context) model_reset_physics_cache(context->timeline);
}

static int group_authored_end(const timeline_state_t *timeline, int group_index, int minimum) {
  int end = minimum;
  for (int track_index = 0; track_index < timeline->player_track_count; ++track_index) {
    const player_track_t *track = &timeline->player_tracks[track_index];
    if (track->group_index != group_index) continue;
    for (int snippet_index = 0; snippet_index < track->snippet_count; ++snippet_index) {
      const input_snippet_t *snippet = &track->snippets[snippet_index];
      if (snippet->is_active && snippet->end_tick > end) end = snippet->end_tick;
    }
  }
  return end;
}

void input_effects_invalidate(timeline_state_t *timeline) {
  if (!timeline) return;
  timeline->input_effects_dirty = true;
  ++timeline->input_effect_context_revision;
  if (timeline->input_effect_context_revision == 0) ++timeline->input_effect_context_revision;
}

void input_effects_refresh(timeline_state_t *timeline) {
  if (timeline) timeline->input_effects_dirty = true;
}

const input_record_t *input_effects_snippet_window(const input_snippet_t *snippet) {
  if (snippet && snippet->effect_cache_valid && snippet->effect_inputs &&
      snippet->effect_input_count == snippet->input_count)
    return snippet->effect_inputs;
  return snippet_window(snippet);
}

void input_effect_destroy(input_effect_t *effect) {
  if (!effect) return;
  free(effect->parameters);
  free(effect->runtime);
  memset(effect, 0, sizeof(*effect));
}

bool input_effect_copy(input_effect_t *destination, const input_effect_t *source) {
  if (!destination || !source) return false;
  if ((source->parameter_size > 0 && !source->parameters) || (source->runtime_size > 0 && !source->runtime))
    return false;
  memset(destination, 0, sizeof(*destination));
  *destination = *source;
  destination->parameters = NULL;
  destination->runtime = NULL;
  destination->runtime_ok = false;
  if (source->parameter_size > 0) {
    destination->parameters = malloc(source->parameter_size);
    if (!destination->parameters) goto fail;
    memcpy(destination->parameters, source->parameters, source->parameter_size);
  }
  if (source->runtime_size > 0) {
    destination->runtime = calloc(1, source->runtime_size);
    if (!destination->runtime) goto fail;
  }
  return true;
fail:
  input_effect_destroy(destination);
  return false;
}

void input_effect_stack_destroy(input_effect_t *effects, int count) {
  if (!effects) return;
  for (int i = 0; i < count; ++i)
    input_effect_destroy(&effects[i]);
  free(effects);
}

input_effect_t *input_effect_stack_copy(const input_effect_t *effects, int count) {
  if (!effects || count <= 0) return NULL;
  input_effect_t *copy = calloc((size_t)count, sizeof(*copy));
  if (!copy) return NULL;
  for (int i = 0; i < count; ++i) {
    if (input_effect_copy(&copy[i], &effects[i])) continue;
    input_effect_stack_destroy(copy, count);
    return NULL;
  }
  return copy;
}

bool input_effect_stack_equal(const input_effect_t *left, int left_count, const input_effect_t *right, int right_count) {
  if (left_count != right_count) return false;
  for (int i = 0; i < left_count; ++i) {
    if (strcmp(left[i].type_id, right[i].type_id) != 0 || left[i].enabled != right[i].enabled ||
        left[i].parameter_size != right[i].parameter_size ||
        (left[i].parameter_size > 0 && memcmp(left[i].parameters, right[i].parameters, left[i].parameter_size) != 0))
      return false;
  }
  return true;
}

bool input_effect_init(game_host_t *host, unsigned type_index, input_effect_t *effect) {
  const ft_input_effect_desc *desc = gh_input_effect_desc(host, type_index);
  if (!desc || !effect) return false;
  memset(effect, 0, sizeof(*effect));
  snprintf(effect->type_id, sizeof(effect->type_id), "%s", desc->id);
  effect->enabled = true;
  effect->parameter_size = desc->parameter_size;
  effect->runtime_size = desc->runtime_size;
  if (effect->parameter_size > 0) effect->parameters = calloc(1, effect->parameter_size);
  if (effect->runtime_size > 0) effect->runtime = calloc(1, effect->runtime_size);
  if ((effect->parameter_size > 0 && !effect->parameters) || (effect->runtime_size > 0 && !effect->runtime) ||
      !gh_input_effect_default(host, type_index, effect->parameters, effect->parameter_size)) {
    input_effect_destroy(effect);
    return false;
  }
  return true;
}

const ft_input_effect_desc *input_effect_descriptor(game_host_t *host, const input_effect_t *effect, int *out_index) {
  const int index = effect ? gh_input_effect_find(host, effect->type_id) : -1;
  if (out_index) *out_index = index;
  return index >= 0 ? gh_input_effect_desc(host, (unsigned)index) : NULL;
}

int input_effects_enabled_count(const input_snippet_t *snippet) {
  int count = 0;
  if (snippet)
    for (int i = 0; i < snippet->effect_count; ++i)
      if (snippet->effects[i].enabled) ++count;
  return count;
}

void input_effects_snippet_cleanup(input_snippet_t *snippet) {
  if (!snippet) return;
  input_effect_stack_destroy(snippet->effects, snippet->effect_count);
  free(snippet->effect_inputs);
  for (int i = 0; i < snippet->effect_stage_capacity; ++i) {
    free(snippet->effect_stage_caches[i].inputs);
    free(snippet->effect_stage_caches[i].runtime);
  }
  free(snippet->effect_stage_caches);
  snippet->effects = NULL;
  snippet->effect_count = 0;
  snippet->effect_capacity = 0;
  snippet->effect_inputs = NULL;
  snippet->effect_input_count = 0;
  snippet->effect_cache_valid = false;
  snippet->effect_stage_caches = NULL;
  snippet->effect_stage_capacity = 0;
}

void input_effects_snippet_discard_cache(input_snippet_t *snippet) {
  if (!snippet) return;
  free(snippet->effect_inputs);
  snippet->effect_inputs = NULL;
  snippet->effect_input_count = 0;
  snippet->effect_cache_valid = false;
  for (int i = 0; i < snippet->effect_stage_capacity; ++i)
    snippet->effect_stage_caches[i].valid = false;
}

void input_effects_snippet_clone(input_snippet_t *destination, const input_snippet_t *source) {
  destination->effects = input_effect_stack_copy(source->effects, source->effect_count);
  destination->effect_count = destination->effects ? source->effect_count : 0;
  destination->effect_capacity = destination->effect_count;
  destination->effect_inputs = NULL;
  destination->effect_input_count = 0;
  destination->effect_cache_valid = false;
  destination->effect_stage_caches = NULL;
  destination->effect_stage_capacity = 0;
}

static bool initialize_snippet_cache(input_snippet_t *snippet) {
  snippet->effect_cache_valid = false;
  if (input_effects_enabled_count(snippet) == 0 || snippet->input_count <= 0) return true;
  if (snippet->effect_input_count != snippet->input_count) {
    input_record_t *grown = realloc(snippet->effect_inputs, sizeof(*grown) * (size_t)snippet->input_count);
    if (!grown) {
      snippet->effect_input_count = 0;
      return false;
    }
    snippet->effect_inputs = grown;
    snippet->effect_input_count = snippet->input_count;
  }
  memcpy(snippet->effect_inputs, snippet_window(snippet), sizeof(*snippet->effect_inputs) * (size_t)snippet->input_count);
  snippet->effect_cache_valid = true;
  return true;
}

static uint64_t effect_stage_key(const timeline_state_t *timeline, uint64_t prefix, const input_effect_t *effect,
                                 const input_record_t *inputs, int count, size_t input_size) {
  uint64_t hash = UINT64_C(1469598103934665603);
  hash = hash_u64(hash, timeline->input_effect_context_revision);
  hash = hash_u64(hash, prefix);
  hash = hash_bytes(hash, effect->type_id, strlen(effect->type_id));
  hash = hash_bytes(hash, effect->parameters, effect->parameter_size);
  for (int row = 0; row < count; ++row)
    hash = hash_bytes(hash, inputs[row].bytes, input_size);
  return hash;
}

static uint64_t advance_effect_prefix(uint64_t prefix, const input_snippet_t *snippet, int effect_index,
                                      const input_record_t *inputs, int count, size_t input_size) {
  prefix = hash_u64(prefix, (uint64_t)(uint32_t)snippet->id);
  prefix = hash_u64(prefix, (uint64_t)(uint32_t)effect_index);
  for (int row = 0; row < count; ++row)
    prefix = hash_bytes(prefix, inputs[row].bytes, input_size);
  return prefix;
}

static input_effect_stage_cache_t *effect_stage_cache(input_snippet_t *snippet, int effect_index) {
  if (effect_index >= snippet->effect_stage_capacity) {
    int capacity = snippet->effect_stage_capacity > 0 ? snippet->effect_stage_capacity : 4;
    while (capacity <= effect_index)
      capacity *= 2;
    input_effect_stage_cache_t *grown = realloc(snippet->effect_stage_caches, sizeof(*grown) * (size_t)capacity);
    if (!grown) return NULL;
    memset(grown + snippet->effect_stage_capacity, 0, sizeof(*grown) * (size_t)(capacity - snippet->effect_stage_capacity));
    snippet->effect_stage_caches = grown;
    snippet->effect_stage_capacity = capacity;
  }
  input_effect_stage_cache_t *cache = &snippet->effect_stage_caches[effect_index];
  if (cache->input_count != snippet->input_count) {
    input_record_t *grown = realloc(cache->inputs, sizeof(*grown) * (size_t)snippet->input_count);
    if (!grown) {
      cache->valid = false;
      cache->input_count = 0;
      return NULL;
    }
    cache->inputs = grown;
    cache->input_count = snippet->input_count;
    cache->valid = false;
  }
  return cache;
}

static bool effect_stage_store_runtime(input_effect_stage_cache_t *stage, const input_effect_t *effect) {
  if (effect->runtime_size > 0 && !effect->runtime) return false;
  if (stage->runtime_size != effect->runtime_size) {
    unsigned char *runtime = effect->runtime_size ? realloc(stage->runtime, effect->runtime_size) : NULL;
    if (effect->runtime_size > 0 && !runtime) return false;
    if (effect->runtime_size == 0) free(stage->runtime);
    stage->runtime = runtime;
    stage->runtime_size = effect->runtime_size;
  }
  if (effect->runtime_size > 0) memcpy(stage->runtime, effect->runtime, effect->runtime_size);
  return true;
}

bool input_effects_ensure(timeline_state_t *timeline) {
  if (!timeline || !timeline->input_effects_dirty) return true;
  /* Recording appends and trims temporary snippets every tick. Effects apply
   * only to committed snippets, whose last cache remains valid until commit. */
  if (timeline->recording) return true;
  if (timeline->input_effects_rebuilding) return true;

  timeline->input_effects_rebuilding = true;
  timeline->input_effects_dirty = false;
  bool ok = true;
  for (int track = 0; track < timeline->player_track_count; ++track)
    for (int snippet = 0; snippet < timeline->player_tracks[track].snippet_count; ++snippet)
      if (!initialize_snippet_cache(&timeline->player_tracks[track].snippets[snippet])) ok = false;

  game_host_t *host = &timeline->ui->gfx_handler->game_host;
  const size_t input_size = game_input_size(host);
  uint64_t effect_prefix = UINT64_C(1469598103934665603);
  effect_prefix = hash_u64(effect_prefix, timeline->input_effect_context_revision);
  for (int track_index = 0; track_index < timeline->player_track_count; ++track_index) {
    player_track_t *track = &timeline->player_tracks[track_index];
    const int group_index = track->group_index;
    const int local_player = model_group_local_track_index(timeline, track_index);
    if (group_index < 0 || local_player < 0) continue;
    effect_eval_context_t context = {.timeline = timeline, .group_index = group_index};
    for (int snippet_index = 0; snippet_index < track->snippet_count; ++snippet_index) {
      input_snippet_t *snippet = &track->snippets[snippet_index];
      if (!snippet->effect_cache_valid) continue;
      for (int effect_index = 0; effect_index < snippet->effect_count; ++effect_index) {
        input_effect_t *effect = &snippet->effects[effect_index];
        if (!effect->enabled) continue;
        input_effect_stage_cache_t *stage = effect_stage_cache(snippet, effect_index);
        const uint64_t stage_key = effect_stage_key(timeline, effect_prefix, effect, snippet->effect_inputs,
                                                    snippet->input_count, input_size);
        if (stage && stage->valid && stage->key == stage_key && stage->runtime_size == effect->runtime_size &&
            (effect->runtime_size == 0 || (stage->runtime && effect->runtime))) {
          memcpy(snippet->effect_inputs, stage->inputs, sizeof(*stage->inputs) * (size_t)snippet->input_count);
          if (effect->runtime_size > 0) memcpy(effect->runtime, stage->runtime, effect->runtime_size);
          effect->runtime_ok = true;
          effect_prefix = advance_effect_prefix(effect_prefix, snippet, effect_index, snippet->effect_inputs,
                                                snippet->input_count, input_size);
          continue;
        }

        int type_index = -1;
        const ft_input_effect_desc *desc = input_effect_descriptor(host, effect, &type_index);
        effect->runtime_ok = false;
        if (!desc || desc->parameter_size != effect->parameter_size) {
          ok = false;
          effect_prefix = advance_effect_prefix(effect_prefix, snippet, effect_index, snippet->effect_inputs,
                                                snippet->input_count, input_size);
          continue;
        }
        if (effect->runtime_size != desc->runtime_size) {
          unsigned char *runtime = desc->runtime_size ? calloc(1, desc->runtime_size) : NULL;
          if (desc->runtime_size > 0 && !runtime) {
            ok = false;
            effect_prefix = advance_effect_prefix(effect_prefix, snippet, effect_index, snippet->effect_inputs,
                                                  snippet->input_count, input_size);
            continue;
          }
          free(effect->runtime);
          effect->runtime = runtime;
          effect->runtime_size = desc->runtime_size;
        }

        const int player_count = model_group_track_count(timeline, group_index);
        ft_input_effect_frame frame = {
            .struct_size = sizeof(frame),
            .level = timeline->ui->gfx_handler->level,
            .track_index = track_index,
            .world_index = group_index,
            .player = local_player,
            .start_tick = snippet->start_tick,
            .end_tick = snippet->end_tick,
            .authored_end_tick = group_authored_end(timeline, group_index, snippet->end_tick),
            .player_count = (uint32_t)(player_count > 0 ? player_count : 0),
            .record_count = (uint32_t)snippet->input_count,
            .record_stride = sizeof(input_record_t),
            .timeline_user = &context,
            .input_at_tick = effect_input_at_tick,
            .world_at_tick = effect_world_at_tick,
            .reset_simulation = effect_reset_simulation,
        };
        input_record_t *upstream = malloc(sizeof(*upstream) * (size_t)snippet->input_count);
        if (!upstream) {
          ok = false;
          effect_prefix = advance_effect_prefix(effect_prefix, snippet, effect_index, snippet->effect_inputs,
                                                snippet->input_count, input_size);
          continue;
        }
        memcpy(upstream, snippet->effect_inputs, sizeof(*upstream) * (size_t)snippet->input_count);
        model_reset_physics_cache(timeline);
        effect->runtime_ok = gh_input_effect_apply(host, (unsigned)type_index, &frame, effect->parameters,
                                                   effect->parameter_size, effect->runtime, effect->runtime_size,
                                                   snippet->effect_inputs);
        if (!effect->runtime_ok) {
          memcpy(snippet->effect_inputs, upstream, sizeof(*upstream) * (size_t)snippet->input_count);
          ok = false;
        }
        free(upstream);
        if (effect->runtime_ok && stage && effect_stage_store_runtime(stage, effect)) {
          memcpy(stage->inputs, snippet->effect_inputs, sizeof(*stage->inputs) * (size_t)snippet->input_count);
          stage->key = stage_key;
          stage->valid = true;
        }
        effect_prefix = advance_effect_prefix(effect_prefix, snippet, effect_index, snippet->effect_inputs,
                                              snippet->input_count, input_size);
      }
    }
  }
  timeline->input_effects_rebuilding = false;
  model_reset_physics_cache(timeline);
  return ok;
}
