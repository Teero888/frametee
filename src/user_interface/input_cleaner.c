#include "input_cleaner.h"

#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <engine/engine_api.h>
#include <engine/game_host.h>
#include <engine/input_record.h>
#include <renderer/graphics_backend.h>
#include <stdlib.h>
#include <string.h>

typedef struct clean_context_t {
  timeline_state_t *timeline;
  game_host_t *host;
  int group_index;
  int local_player;
  int player_count;
  int authored_end_tick;
  int horizon_tick;
  int lookahead_ticks;
  size_t input_size;
  input_snippet_t *snippet;
  input_record_t *saved_cells;
  int edit_first_row;
  int edit_last_row;
  ft_world *reference_world;
  ft_world *candidate_world;
  unsigned char *reference_inputs;
  unsigned char *candidate_inputs;
  input_clean_result_t result;
} clean_context_t;

static bool float_equal(float a, float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static bool field_equal(game_host_t *host, const ft_input_field *field, int field_index, const input_record_t *a,
                        const input_record_t *b) {
  switch (field->kind) {
  case FT_INPUT_FLOAT:
    return float_equal(engine_input_get_float(host, a, field_index), engine_input_get_float(host, b, field_index));
  case FT_INPUT_VEC2: {
    const ft_vec2 left = engine_input_get_vec2(host, a, field_index);
    const ft_vec2 right = engine_input_get_vec2(host, b, field_index);
    return float_equal(left.x, right.x) && float_equal(left.y, right.y);
  }
  default:
    return engine_input_get(host, a, field_index) == engine_input_get(host, b, field_index);
  }
}

static void copy_field(game_host_t *host, const ft_input_field *field, int field_index, input_record_t *destination,
                       const input_record_t *source) {
  switch (field->kind) {
  case FT_INPUT_FLOAT:
    engine_input_set_float(host, destination, field_index, engine_input_get_float(host, source, field_index));
    break;
  case FT_INPUT_VEC2:
    engine_input_set_vec2(host, destination, field_index, engine_input_get_vec2(host, source, field_index));
    break;
  default:
    engine_input_set(host, destination, field_index, engine_input_get(host, source, field_index));
    break;
  }
}

static void record_changed_fields(clean_context_t *context, const ft_input_schema *schema, const input_record_t *record,
                                  const input_record_t *default_record, const bool *selected_fields,
                                  uint32_t selected_field_count, bool *changed_fields) {
  for (uint32_t field_index = 0; field_index < schema->field_count; ++field_index) {
    if ((selected_fields && (field_index >= selected_field_count || !selected_fields[field_index])) ||
        changed_fields[field_index] ||
        field_equal(context->host, &schema->fields[field_index], (int)field_index, record, default_record))
      continue;
    changed_fields[field_index] = true;
    ++context->result.changed_fields;
  }
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

// Both simulations read the currently cleaned timeline. The reference briefly
// restores the original record under this tick when it belongs to the range
// being tested; the candidate keeps that range at its canonical defaults.
static void fill_inputs(clean_context_t *context, int tick) {
  for (int player = 0; player < context->player_count; ++player) {
    const int track_index = model_group_track_index(context->timeline, context->group_index, player);
    input_record_t candidate;
    input_record_t reference;
    if (track_index >= 0) {
      candidate = model_get_input_at_tick(context->timeline, track_index, tick);
      if (player == context->local_player) {
        int reference_row = -1;
        if (tick >= context->snippet->start_tick && tick < context->snippet->end_tick) {
          const int row = tick - context->snippet->start_tick;
          if (row >= context->edit_first_row && row <= context->edit_last_row) reference_row = row;
        } else if (tick >= context->snippet->end_tick &&
                   context->edit_last_row == context->snippet->input_count - 1) {
          // Stateful values in the final record carry past a snippet until a
          // newer one takes over. Restoring it here reproduces that reference
          // behavior; triggers are still reset by model_get_input_at_tick.
          reference_row = context->edit_last_row;
        }

        if (reference_row >= 0) {
          input_record_t *cell = &snippet_window(context->snippet)[reference_row];
          const input_record_t candidate_cell = *cell;
          *cell = context->saved_cells[reference_row];
          reference = model_get_input_at_tick(context->timeline, track_index, tick);
          *cell = candidate_cell;
        } else {
          reference = candidate;
        }
      } else {
        reference = candidate;
      }
    } else {
      engine_input_default(context->host, &candidate);
      reference = candidate;
    }
    memcpy(context->candidate_inputs + (size_t)player * context->input_size, candidate.bytes, context->input_size);
    memcpy(context->reference_inputs + (size_t)player * context->input_size, reference.bytes, context->input_size);
  }
}

static bool candidate_preserves_run(clean_context_t *context, const ft_world *source, int candidate_tick) {
  gh_world_copy(context->host, context->reference_world, source);
  gh_world_copy(context->host, context->candidate_world, source);
  ++context->result.simulations;

  for (int tick = candidate_tick; tick < context->horizon_tick; ++tick) {
    fill_inputs(context, tick);
    gh_world_step(context->host, context->reference_world, context->reference_inputs, (unsigned)context->player_count);
    gh_world_step(context->host, context->candidate_world, context->candidate_inputs, (unsigned)context->player_count);
    ++context->result.simulated_ticks;
    if (!gh_world_run_equal(context->host, context->reference_world, context->candidate_world)) return false;
  }
  return true;
}

typedef struct clean_job_t {
  clean_context_t *context;
  const ft_input_schema *schema;
  const input_record_t *default_record;
  bool *changed_rows;
  bool *changed_fields;
  const bool *selected_fields;
  uint32_t selected_field_count;
  int selected_count;
  bool pass_changed;
} clean_job_t;

static bool field_selected(const clean_job_t *job, uint32_t field_index) {
  return !job->selected_fields ||
         (field_index < job->selected_field_count && job->selected_fields[field_index]);
}

static int selected_record_change_count(const clean_job_t *job, const input_record_t *record) {
  int count = 0;
  for (uint32_t index = 0; index < job->schema->field_count; ++index)
    if (field_selected(job, index) &&
        !field_equal(job->context->host, &job->schema->fields[index], (int)index, record,
                     job->default_record))
      ++count;
  return count;
}

// field_index == -1 means every selected non-default field in the record.
static int row_change_count(const clean_job_t *job, int row, int field_index) {
  const input_record_t *record = &snippet_window(job->context->snippet)[row];
  if (field_index < 0) return selected_record_change_count(job, record);
  return field_equal(job->context->host, &job->schema->fields[field_index], field_index, record,
                     job->default_record)
             ? 0
             : 1;
}

static bool try_clean_range(clean_job_t *job, int first_row, int last_row, int field_index) {
  clean_context_t *context = job->context;
  input_snippet_t *snippet = context->snippet;
  input_record_t *cells = snippet_window(snippet);
  int changes = 0;
  for (int row = first_row; row <= last_row; ++row) changes += row_change_count(job, row, field_index);
  if (changes == 0) return true;

  const int candidate_tick = snippet->start_tick + first_row;
  const int global_tick = candidate_tick + context->timeline->groups[context->group_index]->start_offset;
  const ft_world *source = model_group_world_at_tick(context->timeline, context->group_index, global_tick);
  if (!source) return false;

  for (int row = first_row; row <= last_row; ++row) {
    context->saved_cells[row] = cells[row];
    if (field_index < 0) {
      for (uint32_t index = 0; index < job->schema->field_count; ++index)
        if (field_selected(job, index))
          copy_field(context->host, &job->schema->fields[index], (int)index, &cells[row],
                     job->default_record);
    } else {
      copy_field(context->host, &job->schema->fields[field_index], field_index, &cells[row], job->default_record);
    }
  }
  context->edit_first_row = first_row;
  context->edit_last_row = last_row;
  context->horizon_tick = context->authored_end_tick;
  const int last_changed_tick = snippet->start_tick + last_row;
  if (context->lookahead_ticks > 0 &&
      last_changed_tick < context->authored_end_tick - context->lookahead_ticks)
    context->horizon_tick = last_changed_tick + context->lookahead_ticks;

  if (!candidate_preserves_run(context, source, candidate_tick)) {
    memcpy(&cells[first_row], &context->saved_cells[first_row],
           sizeof(*cells) * (size_t)(last_row - first_row + 1));
    return false;
  }

  context->result.changed_values += changes;
  for (int row = first_row; row <= last_row; ++row) {
    const int row_changes = field_index < 0
                                ? selected_record_change_count(job, &context->saved_cells[row])
                                : !field_equal(context->host, &job->schema->fields[field_index], field_index,
                                               &context->saved_cells[row], job->default_record);
    if (row_changes == 0) continue;
    if (!job->changed_rows[row]) {
      job->changed_rows[row] = true;
      ++context->result.changed_rows;
    }
    if (field_index < 0)
      record_changed_fields(context, job->schema, &context->saved_cells[row], job->default_record,
                            job->selected_fields, job->selected_field_count, job->changed_fields);
  }
  if (field_index >= 0 && !job->changed_fields[field_index]) {
    job->changed_fields[field_index] = true;
    ++context->result.changed_fields;
  }
  job->pass_changed = true;
  return true;
}

// Test large held stretches as one edit. If a range contains a necessary
// activation, split it from the end backwards until redundant prefixes,
// suffixes, or individual ticks can be removed independently.
static void clean_range(clean_job_t *job, int first_row, int last_row, int field_index) {
  while (first_row <= last_row && row_change_count(job, first_row, field_index) == 0) ++first_row;
  while (first_row <= last_row && row_change_count(job, last_row, field_index) == 0) --last_row;
  if (first_row > last_row || try_clean_range(job, first_row, last_row, field_index) || first_row == last_row) return;

  const int middle = first_row + (last_row - first_row) / 2;
  clean_range(job, middle + 1, last_row, field_index);
  clean_range(job, first_row, middle, field_index);
}

static void clean_field_runs(clean_job_t *job, int field_index) {
  int row = job->context->snippet->input_count - 1;
  while (row >= 0) {
    while (row >= 0 && row_change_count(job, row, field_index) == 0) --row;
    if (row < 0) break;
    const int last_row = row;
    while (row > 0 && row_change_count(job, row - 1, field_index) != 0) --row;
    const int first_row = row;
    clean_range(job, first_row, last_row, field_index);
    row = first_row - 1;
  }
}

static void clean_context_destroy(clean_context_t *context) {
  gh_world_destroy(context->host, context->reference_world);
  gh_world_destroy(context->host, context->candidate_world);
  free(context->reference_inputs);
  free(context->candidate_inputs);
  free(context->saved_cells);
  memset(context, 0, sizeof(*context));
}

bool input_cleaner_clean_snippet(ui_handler_t *ui, int track_index, input_snippet_t *snippet, const bool *clean_fields,
                                 uint32_t clean_field_count, input_clean_result_t *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!ui || !ui->gfx_handler || !ui->gfx_handler->level || !snippet || !snippet->is_active || snippet->input_count <= 0)
    return false;

  timeline_state_t *timeline = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_input_schema *schema = game_input_schema(host);
  const int group_index = model_track_group_index(timeline, track_index);
  const int local_player = model_group_local_track_index(timeline, track_index);
  if (!schema || schema->field_count == 0 || group_index < 0 || local_player < 0 ||
      (clean_fields && clean_field_count < schema->field_count))
    return false;

  int selected_count = 0;
  for (uint32_t field_index = 0; field_index < schema->field_count; ++field_index)
    if (!clean_fields || clean_fields[field_index]) ++selected_count;
  if (selected_count == 0) return true;

  clean_context_t context = {.timeline = timeline,
                             .host = host,
                             .group_index = group_index,
                             .local_player = local_player,
                             .snippet = snippet,
                             .input_size = game_input_size(host)};

  context.authored_end_tick = group_authored_end(timeline, group_index, snippet->end_tick);
  context.horizon_tick = context.authored_end_tick;
  context.lookahead_ticks = gh_input_clean_lookahead_ticks(host);

  const int global_start = snippet->start_tick + timeline->groups[group_index]->start_offset;
  const ft_world *start = model_group_world_at_tick(timeline, group_index, global_start);
  if (!start) return false;
  context.player_count = gh_world_player_count(host, start);
  if (local_player >= context.player_count) return false;

  context.reference_world = gh_world_create(host, ui->gfx_handler->level, context.player_count, -1);
  context.candidate_world = gh_world_create(host, ui->gfx_handler->level, context.player_count, -1);
  context.saved_cells = malloc(sizeof(*context.saved_cells) * (size_t)snippet->input_count);
  if (context.player_count > 0) {
    context.reference_inputs = malloc((size_t)context.player_count * context.input_size);
    context.candidate_inputs = malloc((size_t)context.player_count * context.input_size);
  }
  if (!context.reference_world || !context.candidate_world || !context.saved_cells ||
      (context.player_count > 0 && (!context.reference_inputs || !context.candidate_inputs))) {
    clean_context_destroy(&context);
    return false;
  }
  gh_world_copy(host, context.reference_world, start);
  gh_world_copy(host, context.candidate_world, start);
  if (!gh_world_run_equal(host, context.reference_world, context.candidate_world)) {
    clean_context_destroy(&context);
    return false;
  }

  input_record_t default_record;
  engine_input_default(host, &default_record);
  bool *changed_rows = calloc((size_t)snippet->input_count, sizeof(*changed_rows));
  bool *changed_fields = calloc(schema->field_count, sizeof(*changed_fields));
  if (!changed_rows || !changed_fields) {
    free(changed_rows);
    free(changed_fields);
    clean_context_destroy(&context);
    return false;
  }

  const bool previous_effects = engine_api_set_presentation_effects(false);
  bool pass_changed;
  do {
    ++context.result.passes;
    clean_job_t job = {.context = &context,
                       .schema = schema,
                       .default_record = &default_record,
                       .changed_rows = changed_rows,
                       .changed_fields = changed_fields,
                       .selected_fields = clean_fields,
                       .selected_field_count = clean_field_count,
                       .selected_count = selected_count};

    // Some controls are redundant only as a group (for example two opposing
    // controls in the same tick). Try ranges of whole records before asking
    // whether their individual fields can be removed.
    model_recalc_physics(timeline, global_start);
    if (job.selected_count > 1) clean_range(&job, 0, snippet->input_count - 1, -1);

    // Reverse schema order gives DDNet hook a pass before fire. Reverse tick
    // ranges separate useful later activations from earlier held input.
    for (int field_index = (int)schema->field_count - 1; field_index >= 0; --field_index) {
      if (!field_selected(&job, (uint32_t)field_index)) continue;
      model_recalc_physics(timeline, global_start);
      clean_field_runs(&job, field_index);
    }
    pass_changed = job.pass_changed;
  } while (pass_changed);

  engine_api_set_presentation_effects(previous_effects);
  model_recalc_physics(timeline, global_start);
  if (out) *out = context.result;
  free(changed_rows);
  free(changed_fields);
  clean_context_destroy(&context);
  return true;
}
