#include "dd_input_effects.h"

#include "dd_imgui.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum dd_effect_type {
  DD_EFFECT_CLEAN = 0,
  DD_EFFECT_SMOOTH_TARGET,
  DD_EFFECT_COUNT,
};

enum dd_input_field {
  DD_IN_DIRECTION = 0,
  DD_IN_TARGET,
  DD_IN_JUMP,
  DD_IN_FIRE,
  DD_IN_HOOK,
  DD_IN_WEAPON,
  DD_IN_KILL,
  DD_IN_EYES,
  DD_IN_EMOTE,
  DD_IN_SIT,
  DD_IN_TELE_OUT,
  DD_IN_COUNT,
};

#define DD_CLEANABLE_FIELDS ((UINT64_C(1) << (DD_IN_KILL + 1)) - 1)

typedef struct dd_clean_parameters_t {
  uint64_t fields;
} dd_clean_parameters_t;

typedef struct dd_clean_runtime_t {
  uint32_t changed_values;
  uint32_t changed_rows;
  uint32_t passes;
  uint32_t changed_fields;
  uint64_t simulations;
  uint64_t simulated_ticks;
} dd_clean_runtime_t;

enum dd_smooth_axis { DD_SMOOTH_X = 1u << 0,
                      DD_SMOOTH_Y = 1u << 1,
                      DD_SMOOTH_REQUIRED_AXES = DD_SMOOTH_X | DD_SMOOTH_Y };
enum dd_smooth_action { DD_SMOOTH_FIRE = 1u << 0,
                        DD_SMOOTH_HOOK = 1u << 1,
                        DD_SMOOTH_REQUIRED_ACTIONS = DD_SMOOTH_FIRE | DD_SMOOTH_HOOK };
enum dd_smooth_curve { DD_CURVE_LINEAR = 0,
                       DD_CURVE_SMOOTHSTEP,
                       DD_CURVE_SMOOTHERSTEP };
enum dd_smooth_basis { DD_BASIS_VECTOR_POSITION = 0,
                       DD_BASIS_ANGLE };

typedef struct dd_smooth_parameters_t {
  uint32_t axes;    /* Persisted for effect payload compatibility; both target axes are mandatory. */
  uint32_t actions; /* Persisted for effect payload compatibility; all action anchors are mandatory. */
  uint32_t curve;
  uint32_t basis;
  int32_t maximum_gap;
  float reserved; /* Persisted for effect payload compatibility. */
} dd_smooth_parameters_t;

typedef struct dd_smooth_runtime_t {
  uint32_t changed_rows;
  uint32_t anchors;
} dd_smooth_runtime_t;

static const ft_input_effect_desc effect_descs[DD_EFFECT_COUNT] = {
    [DD_EFFECT_CLEAN] = {
        .struct_size = sizeof(ft_input_effect_desc),
        .id = "clean_inputs",
        .display_name = "Clean inputs",
        .description = "Remove selected inputs that do not change the DDNet run.",
        .parameter_size = sizeof(dd_clean_parameters_t),
        .runtime_size = sizeof(dd_clean_runtime_t),
    },
    [DD_EFFECT_SMOOTH_TARGET] = {
        .struct_size = sizeof(ft_input_effect_desc),
        .id = "smooth_target",
        .display_name = "Smooth target",
        .description = "Interpolate target vector positions or angles between DDNet actions.",
        .parameter_size = sizeof(dd_smooth_parameters_t),
        .runtime_size = sizeof(dd_smooth_runtime_t),
    },
};

static const char *field_names[DD_IN_COUNT] = {
    [DD_IN_DIRECTION] = "Direction",
    [DD_IN_TARGET] = "TargetX / TargetY",
    [DD_IN_JUMP] = "Jump",
    [DD_IN_FIRE] = "Fire",
    [DD_IN_HOOK] = "Hook",
    [DD_IN_WEAPON] = "Weapon",
    [DD_IN_KILL] = "Kill",
};

static SPlayerInput default_input(void) {
  SPlayerInput input;
  memset(&input, 0, sizeof(input));
  input.m_TargetY = -1;
  input.m_WantedWeapon = WEAPON_GUN;
  return input;
}

static bool field_equal(const SPlayerInput *left, const SPlayerInput *right, int field) {
  switch (field) {
  case DD_IN_DIRECTION:
    return left->m_Direction == right->m_Direction;
  case DD_IN_TARGET:
    return left->m_TargetX == right->m_TargetX && left->m_TargetY == right->m_TargetY;
  case DD_IN_JUMP:
    return left->m_Jump == right->m_Jump;
  case DD_IN_FIRE:
    return left->m_Fire == right->m_Fire;
  case DD_IN_HOOK:
    return left->m_Hook == right->m_Hook;
  case DD_IN_WEAPON:
    return left->m_WantedWeapon == right->m_WantedWeapon;
  case DD_IN_KILL:
    return get_flag_kill(left) == get_flag_kill(right);
  case DD_IN_TELE_OUT:
    return left->m_TeleOut == right->m_TeleOut;
  default:
    return true;
  }
}

static void copy_field(SPlayerInput *destination, const SPlayerInput *source, int field) {
  switch (field) {
  case DD_IN_DIRECTION:
    destination->m_Direction = source->m_Direction;
    break;
  case DD_IN_TARGET:
    destination->m_TargetX = source->m_TargetX;
    destination->m_TargetY = source->m_TargetY;
    break;
  case DD_IN_JUMP:
    destination->m_Jump = source->m_Jump;
    break;
  case DD_IN_FIRE:
    destination->m_Fire = source->m_Fire;
    break;
  case DD_IN_HOOK:
    destination->m_Hook = source->m_Hook;
    break;
  case DD_IN_WEAPON:
    destination->m_WantedWeapon = source->m_WantedWeapon;
    break;
  case DD_IN_KILL:
    set_flag_kill(destination, get_flag_kill(source));
    break;
  case DD_IN_TELE_OUT:
    destination->m_TeleOut = source->m_TeleOut;
    break;
  default:
    break;
  }
}

typedef struct clean_context_t {
  const ft_input_effect_frame *frame;
  unsigned char *records;
  SPlayerInput *saved;
  SPlayerInput *candidate_inputs;
  int first_edit;
  int last_edit;
  int horizon;
  SWorldCore candidate_world;
  struct expected_player_t *expected;
  int expected_tick_count;
  dd_clean_runtime_t *runtime;
} clean_context_t;

typedef struct expected_player_t {
  mvec2 position;
  mvec2 velocity;
  int freeze_time;
  bool deep_frozen;
  float race_time;
  int start_tick;
} expected_player_t;

static SPlayerInput *clean_cell(clean_context_t *context, int row) {
  return (SPlayerInput *)(context->records + (size_t)row * context->frame->record_stride);
}

static bool world_matches_expected(const clean_context_t *context, const SWorldCore *world, int tick) {
  const int tick_index = tick - context->frame->start_tick;
  if (tick_index < 0 || tick_index >= context->expected_tick_count ||
      world->m_NumCharacters != (int)context->frame->player_count)
    return false;
  const expected_player_t *expected = context->expected + (size_t)tick_index * context->frame->player_count;
  for (uint32_t i = 0; i < context->frame->player_count; ++i) {
    const SCharacterCore *actual = &world->m_pCharacters[i];
    if (memcmp(&actual->m_Pos, &expected[i].position, sizeof(actual->m_Pos)) != 0 ||
        memcmp(&actual->m_Vel, &expected[i].velocity, sizeof(actual->m_Vel)) != 0 ||
        actual->m_FreezeTime != expected[i].freeze_time || actual->m_DeepFrozen != expected[i].deep_frozen ||
        memcmp(&actual->m_RaceTime, &expected[i].race_time, sizeof(actual->m_RaceTime)) != 0 ||
        actual->m_StartTick != expected[i].start_tick)
      return false;
  }
  return true;
}

static void fill_simulation_inputs(clean_context_t *context, int tick) {
  const ft_input_effect_frame *frame = context->frame;
  const SPlayerInput fallback = default_input();
  for (uint32_t player = 0; player < frame->player_count; ++player) {
    SPlayerInput candidate = fallback;
    if (frame->input_at_tick) frame->input_at_tick(frame->timeline_user, (int32_t)player, tick, &candidate);
    context->candidate_inputs[player] = candidate;
  }
}

static bool candidate_preserves_run(clean_context_t *context, const ft_world *source, int candidate_tick) {
  wc_copy_world(&context->candidate_world, (SWorldCore *)&source->core);
  ++context->runtime->simulations;
  for (int tick = candidate_tick; tick < context->horizon; ++tick) {
    fill_simulation_inputs(context, tick);
    for (uint32_t player = 0; player < context->frame->player_count; ++player) {
      cc_on_input(&context->candidate_world.m_pCharacters[player], &context->candidate_inputs[player]);
    }
    wc_tick(&context->candidate_world);
    ++context->runtime->simulated_ticks;
    if (!world_matches_expected(context, &context->candidate_world, tick + 1)) return false;
  }
  return true;
}

typedef struct clean_job_t {
  clean_context_t *context;
  const SPlayerInput *defaults;
  uint64_t fields;
  bool *changed_rows;
  bool *changed_fields;
  bool pass_changed;
} clean_job_t;

static bool field_selected(const clean_job_t *job, int field) { return (job->fields & (UINT64_C(1) << field)) != 0; }

static int row_change_count(const clean_job_t *job, int row, int field) {
  const SPlayerInput *record = clean_cell(job->context, row);
  if (field >= 0) return field_equal(record, job->defaults, field) ? 0 : 1;
  int count = 0;
  for (int i = 0; i < DD_IN_COUNT; ++i)
    if (field_selected(job, i) && !field_equal(record, job->defaults, i)) ++count;
  return count;
}

static int saved_change_count(const clean_job_t *job, int row, int field) {
  const SPlayerInput *record = &job->context->saved[row];
  if (field >= 0) return field_equal(record, job->defaults, field) ? 0 : 1;
  int count = 0;
  for (int i = 0; i < DD_IN_COUNT; ++i)
    if (field_selected(job, i) && !field_equal(record, job->defaults, i)) ++count;
  return count;
}

static void mark_fast_change(clean_job_t *job, int row, int field) {
  if (!job->changed_rows[row]) {
    job->changed_rows[row] = true;
    ++job->context->runtime->changed_rows;
  }
  if (!job->changed_fields[field]) {
    job->changed_fields[field] = true;
    ++job->context->runtime->changed_fields;
  }
  ++job->context->runtime->changed_values;
  job->pass_changed = true;
}

/* Aim is only consumed while hook or fire is active, so inactive rows can be
 * canonicalised without launching a simulation. */
static uint64_t fast_clean_ddnet_fields(clean_job_t *job) {
  uint64_t changed = 0;
  for (uint32_t row = 0; row < job->context->frame->record_count; ++row) {
    SPlayerInput *input = clean_cell(job->context, (int)row);
    if (field_selected(job, DD_IN_TARGET) && !input->m_Hook && !(input->m_Fire & 1) &&
        !field_equal(input, job->defaults, DD_IN_TARGET)) {
      copy_field(input, job->defaults, DD_IN_TARGET);
      mark_fast_change(job, (int)row, DD_IN_TARGET);
      changed |= UINT64_C(1) << DD_IN_TARGET;
    }
  }
  return changed;
}

static bool try_clean_range(clean_job_t *job, int first, int last, int field) {
  clean_context_t *context = job->context;
  int changes = 0;
  for (int row = first; row <= last; ++row)
    changes += row_change_count(job, row, field);
  if (changes == 0) return true;

  for (int row = first; row <= last; ++row) {
    SPlayerInput *cell = clean_cell(context, row);
    context->saved[row] = *cell;
    if (field >= 0)
      copy_field(cell, job->defaults, field);
    else
      for (int i = 0; i < DD_IN_COUNT; ++i)
        if (field_selected(job, i)) copy_field(cell, job->defaults, i);
  }
  context->first_edit = first;
  context->last_edit = last;
  const int changed_tick = context->frame->start_tick + last;
  context->horizon = context->frame->authored_end_tick;
  /* A weapon choice can remain latent until a shot arbitrarily far in the
   * future. Other DDNet input consequences (including projectiles and hooks)
   * have settled after 250 ticks. */
  if (field != DD_IN_WEAPON && changed_tick < context->horizon - 250) context->horizon = changed_tick + 250;
  const ft_world *source = context->frame->world_at_tick
                               ? context->frame->world_at_tick(context->frame->timeline_user,
                                                               context->frame->start_tick + first)
                               : NULL;
  if (!source || !candidate_preserves_run(context, source, context->frame->start_tick + first)) {
    for (int row = first; row <= last; ++row)
      *clean_cell(context, row) = context->saved[row];
    return false;
  }

  context->runtime->changed_values += (uint32_t)changes;
  for (int row = first; row <= last; ++row) {
    if (saved_change_count(job, row, field) == 0) continue;
    if (!job->changed_rows[row]) {
      job->changed_rows[row] = true;
      ++context->runtime->changed_rows;
    }
  }
  if (field >= 0 && !job->changed_fields[field]) {
    job->changed_fields[field] = true;
    ++context->runtime->changed_fields;
  } else if (field < 0) {
    for (int i = 0; i < DD_IN_COUNT; ++i) {
      bool removed = false;
      for (int row = first; row <= last && !removed; ++row)
        removed = field_selected(job, i) && !field_equal(&context->saved[row], job->defaults, i);
      if (removed && !job->changed_fields[i]) {
        job->changed_fields[i] = true;
        ++context->runtime->changed_fields;
      }
    }
  }
  job->pass_changed = true;
  return true;
}

static void clean_range(clean_job_t *job, int first, int last, int field) {
  while (first <= last && row_change_count(job, first, field) == 0)
    ++first;
  while (first <= last && row_change_count(job, last, field) == 0)
    --last;
  if (first > last || try_clean_range(job, first, last, field) || first == last) return;
  const int middle = first + (last - first) / 2;
  clean_range(job, middle + 1, last, field);
  clean_range(job, first, middle, field);
}

static void clean_field_runs(clean_job_t *job, int field) {
  int row = (int)job->context->frame->record_count - 1;
  while (row >= 0) {
    while (row >= 0 && row_change_count(job, row, field) == 0)
      --row;
    if (row < 0) break;
    const int last = row;
    while (row > 0 && row_change_count(job, row - 1, field) != 0)
      --row;
    const int first = row;
    clean_range(job, first, last, field);
    row = first - 1;
  }
}

static void clean_quiet_weapon_ranges(clean_job_t *job) {
  int row = (int)job->context->frame->record_count - 1;
  while (row >= 0) {
    SPlayerInput *input = clean_cell(job->context, row);
    while (row >= 0 && ((input->m_Fire & 1) || row_change_count(job, row, DD_IN_WEAPON) == 0)) {
      --row;
      if (row >= 0) input = clean_cell(job->context, row);
    }
    if (row < 0) break;
    const int last = row;
    while (row > 0) {
      const SPlayerInput *previous = clean_cell(job->context, row - 1);
      if ((previous->m_Fire & 1) || row_change_count(job, row - 1, DD_IN_WEAPON) == 0) break;
      --row;
    }
    clean_range(job, row, last, DD_IN_WEAPON);
    --row;
  }
}

static bool prepare_expected_path(clean_context_t *context) {
  const ft_input_effect_frame *frame = context->frame;
  context->expected_tick_count = frame->authored_end_tick - frame->start_tick + 1;
  if (context->expected_tick_count <= 0) return false;
  const size_t count = (size_t)context->expected_tick_count * frame->player_count;
  context->expected = calloc(count, sizeof(*context->expected));
  if (!context->expected) return false;
  if (frame->reset_simulation) frame->reset_simulation(frame->timeline_user);
  for (int tick_index = 0; tick_index < context->expected_tick_count; ++tick_index) {
    const ft_world *world = frame->world_at_tick
                                ? frame->world_at_tick(frame->timeline_user, frame->start_tick + tick_index)
                                : NULL;
    if (!world || world->core.m_NumCharacters != (int)frame->player_count) return false;
    expected_player_t *expected = context->expected + (size_t)tick_index * frame->player_count;
    for (uint32_t player = 0; player < frame->player_count; ++player) {
      const SCharacterCore *character = &world->core.m_pCharacters[player];
      expected[player].position = character->m_Pos;
      expected[player].velocity = character->m_Vel;
      expected[player].freeze_time = character->m_FreezeTime;
      expected[player].deep_frozen = character->m_DeepFrozen;
      expected[player].race_time = character->m_RaceTime;
      expected[player].start_tick = character->m_StartTick;
    }
  }
  return true;
}

static bool clean_inputs(const ft_input_effect_frame *frame, const dd_clean_parameters_t *parameters,
                         dd_clean_runtime_t *runtime, void *records) {
  if (!frame || !records || frame->record_count == 0 || frame->player < 0 ||
      (uint32_t)frame->player >= frame->player_count || frame->record_stride < sizeof(SPlayerInput))
    return false;
  uint64_t selected = parameters->fields & DD_CLEANABLE_FIELDS;
  if (selected == 0) return true;

  clean_context_t context = {
      .frame = frame,
      .records = records,
      .candidate_world = wc_empty(),
      .runtime = runtime,
  };
  context.saved = calloc(frame->record_count, sizeof(*context.saved));
  context.candidate_inputs = calloc(frame->player_count, sizeof(*context.candidate_inputs));
  bool *changed_rows = calloc(frame->record_count, sizeof(*changed_rows));
  bool changed_fields[DD_IN_COUNT] = {false};
  if (!context.saved || !context.candidate_inputs || !changed_rows) {
    free(context.saved);
    free(context.candidate_inputs);
    free(changed_rows);
    wc_free(&context.candidate_world);
    return false;
  }

  const SPlayerInput defaults = default_input();
  clean_job_t initial_job = {.context = &context,
                             .defaults = &defaults,
                             .fields = selected,
                             .changed_rows = changed_rows,
                             .changed_fields = changed_fields};
  fast_clean_ddnet_fields(&initial_job);
  if (!prepare_expected_path(&context)) {
    free(context.saved);
    free(context.candidate_inputs);
    free(context.expected);
    free(changed_rows);
    wc_free(&context.candidate_world);
    return false;
  }

  uint64_t run_fields = selected;
  bool pass_changed;
  do {
    ++runtime->passes;
    clean_job_t job = {.context = &context,
                       .defaults = &defaults,
                       .fields = selected,
                       .changed_rows = changed_rows,
                       .changed_fields = changed_fields};
    uint64_t changed_this_pass = fast_clean_ddnet_fields(&job);
    job.fields = run_fields;
    for (int field = DD_IN_COUNT - 1; field >= 0; --field) {
      if (!field_selected(&job, field)) continue;
      bool has_changes = false;
      for (uint32_t row = 0; row < frame->record_count && !has_changes; ++row)
        has_changes = row_change_count(&job, (int)row, field) != 0;
      if (!has_changes) continue;
      /* Fields are cleaned from the end towards the beginning. Rebuild the
       * reference cache once per field so its source worlds include every
       * previously accepted field change; edits accepted within this field
       * are later than the next candidate and cannot affect its source. */
      if (frame->reset_simulation) frame->reset_simulation(frame->timeline_user);
      const uint32_t before_values = runtime->changed_values;
      if (field == DD_IN_WEAPON) clean_quiet_weapon_ranges(&job);
      clean_field_runs(&job, field);
      if (runtime->changed_values != before_values) changed_this_pass |= UINT64_C(1) << field;
    }
    run_fields = 0;
    for (int field = 1; field < DD_IN_COUNT; ++field) {
      const uint64_t lower_fields = (UINT64_C(1) << field) - 1;
      if ((selected & (UINT64_C(1) << field)) && (changed_this_pass & lower_fields))
        run_fields |= UINT64_C(1) << field;
    }
    pass_changed = run_fields != 0;
  } while (pass_changed);

  if (frame->reset_simulation) frame->reset_simulation(frame->timeline_user);
  free(context.saved);
  free(context.candidate_inputs);
  free(context.expected);
  free(changed_rows);
  wc_free(&context.candidate_world);
  return true;
}

static float curve_value(uint32_t curve, float t) {
  if (curve == DD_CURVE_SMOOTHSTEP) return t * t * (3.f - 2.f * t);
  if (curve == DD_CURVE_SMOOTHERSTEP) return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
  return t;
}

static int16_t clamp_target(float value) {
  if (value < -32768.f) value = -32768.f;
  if (value > 32767.f) value = 32767.f;
  return (int16_t)lroundf(value);
}

static bool smooth_target(const ft_input_effect_frame *frame, const dd_smooth_parameters_t *parameters,
                          dd_smooth_runtime_t *runtime, void *records) {
  if (!frame || !records || frame->record_count < 2 || frame->record_stride < sizeof(SPlayerInput)) return true;
  const uint32_t count = frame->record_count;
  bool *anchors = calloc(count, sizeof(*anchors));
  int *attack_ticks = calloc(count, sizeof(*attack_ticks));
  int *hook_states = calloc(count, sizeof(*hook_states));
  if (!anchors || !attack_ticks || !hook_states) {
    free(anchors);
    free(attack_ticks);
    free(hook_states);
    return false;
  }
  anchors[0] = true;
  anchors[count - 1] = true;
  runtime->anchors = count > 1 ? 2 : 1;
  for (uint32_t row = 1; row + 1 < count; ++row) {
    const SPlayerInput *previous = (const SPlayerInput *)((unsigned char *)records + (size_t)(row - 1) * frame->record_stride);
    const SPlayerInput *current = (const SPlayerInput *)((unsigned char *)records + (size_t)row * frame->record_stride);
    const bool fire = (current->m_Fire & 1) &&
                      (!(previous->m_Fire & 1) || current->m_Fire != previous->m_Fire);
    const bool hook = current->m_Hook && !previous->m_Hook;
    if (fire || hook) {
      anchors[row] = true;
      ++runtime->anchors;
    }
  }

  if (frame->world_at_tick) {
    if (frame->reset_simulation) frame->reset_simulation(frame->timeline_user);
    for (uint32_t row = 0; row < count; ++row) {
      const ft_world *world = frame->world_at_tick(frame->timeline_user, frame->start_tick + (int32_t)row);
      if (!world || frame->player < 0 || frame->player >= world->core.m_NumCharacters) continue;
      const SCharacterCore *character = &world->core.m_pCharacters[frame->player];
      attack_ticks[row] = character->m_AttackTick;
      hook_states[row] = character->m_HookState;
    }
    for (uint32_t row = 0; row + 1 < count; ++row) {
      const bool fired = attack_ticks[row + 1] != attack_ticks[row];
      const bool hooked = hook_states[row] == HOOK_IDLE && hook_states[row + 1] != HOOK_IDLE;
      if ((fired || hooked) && !anchors[row]) {
        anchors[row] = true;
        ++runtime->anchors;
      }
    }
  }

  uint32_t left = 0;
  while (left + 1 < count) {
    uint32_t right = left + 1;
    while (right + 1 < count && !anchors[right])
      ++right;
    const int gap = (int)(right - left);
    if (parameters->maximum_gap <= 0 || gap <= parameters->maximum_gap) {
      const SPlayerInput *a = (const SPlayerInput *)((unsigned char *)records + (size_t)left * frame->record_stride);
      const SPlayerInput *b = (const SPlayerInput *)((unsigned char *)records + (size_t)right * frame->record_stride);
      ft_vec2 start = {(float)a->m_TargetX, (float)a->m_TargetY};
      ft_vec2 end = {(float)b->m_TargetX, (float)b->m_TargetY};
      const float start_length = hypotf(start.x, start.y);
      const float end_length = hypotf(end.x, end.y);
      const bool angle_basis = parameters->basis == DD_BASIS_ANGLE &&
                               start_length > FLT_EPSILON && end_length > FLT_EPSILON;
      const float start_angle = angle_basis ? atan2f(start.y, start.x) : 0.f;
      const float angle_delta = angle_basis ? remainderf(atan2f(end.y, end.x) - start_angle,
                                                         6.28318530717958647692f)
                                            : 0.f;
      for (uint32_t row = left + 1; row < right; ++row) {
        const float t = curve_value(parameters->curve, (float)(row - left) / (float)(right - left));
        ft_vec2 value;
        if (angle_basis) {
          const float angle = start_angle + angle_delta * t;
          const float length = start_length + (end_length - start_length) * t;
          value = (ft_vec2){cosf(angle) * length, sinf(angle) * length};
        } else {
          value = (ft_vec2){start.x + (end.x - start.x) * t,
                            start.y + (end.y - start.y) * t};
        }
        SPlayerInput *input = (SPlayerInput *)((unsigned char *)records + (size_t)row * frame->record_stride);
        const int16_t x = clamp_target(value.x);
        const int16_t y = clamp_target(value.y);
        bool changed = false;
        if (input->m_TargetX != x) {
          input->m_TargetX = x;
          changed = true;
        }
        if (input->m_TargetY != y) {
          input->m_TargetY = y;
          changed = true;
        }
        if (changed) ++runtime->changed_rows;
      }
    }
    left = right;
  }
  if (frame->reset_simulation) frame->reset_simulation(frame->timeline_user);
  free(anchors);
  free(attack_ticks);
  free(hook_states);
  return true;
}

uint32_t dd_input_effect_count(ft_game *game) {
  (void)game;
  return DD_EFFECT_COUNT;
}

const ft_input_effect_desc *dd_input_effect_desc(ft_game *game, uint32_t index) {
  (void)game;
  return index < DD_EFFECT_COUNT ? &effect_descs[index] : NULL;
}

void dd_input_effect_default(ft_game *game, uint32_t index, void *parameters, uint32_t parameter_size) {
  (void)game;
  if (!parameters) return;
  memset(parameters, 0, parameter_size);
  if (index == DD_EFFECT_CLEAN && parameter_size == sizeof(dd_clean_parameters_t)) {
    dd_clean_parameters_t *clean = parameters;
    clean->fields = DD_CLEANABLE_FIELDS;
  } else if (index == DD_EFFECT_SMOOTH_TARGET && parameter_size == sizeof(dd_smooth_parameters_t)) {
    dd_smooth_parameters_t *smooth = parameters;
    smooth->axes = DD_SMOOTH_REQUIRED_AXES;
    smooth->actions = DD_SMOOTH_REQUIRED_ACTIONS;
    smooth->curve = DD_CURVE_SMOOTHSTEP;
    smooth->basis = DD_BASIS_VECTOR_POSITION;
    smooth->maximum_gap = 0;
    smooth->reserved = 0.f;
  }
}

bool dd_input_effect_apply(ft_game *game, uint32_t index, const ft_input_effect_frame *frame,
                           const void *parameters, uint32_t parameter_size, void *runtime,
                           uint32_t runtime_size, void *inout_records) {
  (void)game;
  if (index == DD_EFFECT_CLEAN && parameter_size == sizeof(dd_clean_parameters_t) &&
      runtime_size == sizeof(dd_clean_runtime_t)) {
    return clean_inputs(frame, parameters, runtime, inout_records);
  }
  if (index == DD_EFFECT_SMOOTH_TARGET && parameter_size == sizeof(dd_smooth_parameters_t) &&
      runtime_size == sizeof(dd_smooth_runtime_t))
    return smooth_target(frame, parameters, runtime, inout_records);
  return false;
}

static bool clean_effect_ui(dd_clean_parameters_t *parameters, const dd_clean_runtime_t *runtime) {
  bool changed = false;
  if ((parameters->fields & ~DD_CLEANABLE_FIELDS) != 0) {
    parameters->fields &= DD_CLEANABLE_FIELDS;
    changed = true;
  }
  if (igButton("Select all", (ImVec2){110.f, 0.f})) {
    parameters->fields = DD_CLEANABLE_FIELDS;
    changed = true;
  }
  igSameLine(0.f, 6.f);
  if (igButton("Select none", (ImVec2){110.f, 0.f})) {
    parameters->fields = 0;
    changed = true;
  }
  if (igBeginTable("CleanFields", 2, ImGuiTableFlags_SizingStretchSame, (ImVec2){0.f, 0.f}, 0.f)) {
    for (int field = 0; field < DD_IN_COUNT; ++field) {
      if ((DD_CLEANABLE_FIELDS & (UINT64_C(1) << field)) == 0) continue;
      igTableNextColumn();
      bool selected = (parameters->fields & (UINT64_C(1) << field)) != 0;
      igPushID_Int(field);
      if (igCheckbox(field_names[field], &selected)) {
        if (selected)
          parameters->fields |= UINT64_C(1) << field;
        else
          parameters->fields &= ~(UINT64_C(1) << field);
        changed = true;
      }
      igPopID();
    }
    igEndTable();
  }
  if (runtime)
    igTextDisabled("%u values removed in %u rows (%llu simulations).", runtime->changed_values,
                   runtime->changed_rows, (unsigned long long)runtime->simulations);
  return changed;
}

static bool smooth_effect_ui(dd_smooth_parameters_t *parameters, const dd_smooth_runtime_t *runtime) {
  bool changed = false;
  if (parameters->axes != DD_SMOOTH_REQUIRED_AXES) {
    parameters->axes = DD_SMOOTH_REQUIRED_AXES;
    changed = true;
  }
  if (parameters->actions != DD_SMOOTH_REQUIRED_ACTIONS) {
    parameters->actions = DD_SMOOTH_REQUIRED_ACTIONS;
    changed = true;
  }
  if (parameters->basis > DD_BASIS_ANGLE) {
    parameters->basis = DD_BASIS_VECTOR_POSITION;
    changed = true;
  }
  if (parameters->reserved != 0.f) {
    parameters->reserved = 0.f;
    changed = true;
  }

  static const char *curves[] = {"Linear", "Smoothstep", "Smootherstep"};
  int curve = (int)parameters->curve;
  if (igCombo_Str_arr("Curve", &curve, curves, 3, -1)) {
    parameters->curve = (uint32_t)curve;
    changed = true;
  }
  static const char *bases[] = {"Vector position", "Angle"};
  int basis = (int)parameters->basis;
  if (igCombo_Str_arr("Basis", &basis, bases, 2, -1)) {
    parameters->basis = (uint32_t)basis;
    changed = true;
  }
  if (igSliderInt("Maximum gap", &parameters->maximum_gap, 0, 500, parameters->maximum_gap == 0 ? "Unlimited" : "%d ticks", 0))
    changed = true;
  if (runtime) igTextDisabled("%u rows smoothed between %u anchors.", runtime->changed_rows, runtime->anchors);
  return changed;
}

bool dd_input_effect_ui(ft_game *game, uint32_t index, const ft_input_effect_ui_frame *frame,
                        void *parameters, uint32_t parameter_size, const void *runtime,
                        uint32_t runtime_size) {
  (void)frame;
  dd_imgui_attach(game ? game->engine : NULL);
  if (index == DD_EFFECT_CLEAN && parameter_size == sizeof(dd_clean_parameters_t) &&
      runtime_size == sizeof(dd_clean_runtime_t))
    return clean_effect_ui(parameters, runtime);
  if (index == DD_EFFECT_SMOOTH_TARGET && parameter_size == sizeof(dd_smooth_parameters_t) &&
      runtime_size == sizeof(dd_smooth_runtime_t))
    return smooth_effect_ui(parameters, runtime);
  return false;
}
