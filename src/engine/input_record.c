#include "input_record.h"

#include <logger/logger.h>

// One active game at a time, so the resolved index lives here rather than being
// threaded through every recording call site.
static int g_cursor_field = -1;

void engine_input_bind(game_host_t *host) {
  g_cursor_field = -1;
  const ft_input_schema *schema = game_input_schema(host);
  if (schema) {
    for (uint32_t i = 0; i < schema->field_count; ++i) {
      if (schema->fields[i].flags & FT_INPUT_FLAG_RECORDING_CURSOR) {
        g_cursor_field = (int)i;
        break;
      }
    }
  }
}

int engine_input_cursor_field(void) { return g_cursor_field; }

bool engine_input_record_fits(const game_host_t *host) {
  const unsigned size = game_input_size(host);
  if (size == 0) return false;
  if (size > ENGINE_MAX_INPUT_RECORD) {
    log_error("Engine", "Game '%s' uses %u-byte input records; this build stores at most %d.", game_host_active_id(host), size,
              ENGINE_MAX_INPUT_RECORD);
    return false;
  }
  return true;
}

long long engine_input_get(game_host_t *host, const input_record_t *record, int field) {
  if (field < 0 || !record) return 0;
  return gh_input_get(host, record->bytes, (unsigned)field);
}

void engine_input_set(game_host_t *host, input_record_t *record, int field, long long value) {
  if (field < 0 || !record) return;
  gh_input_set(host, record->bytes, (unsigned)field, value);
}

float engine_input_get_float(game_host_t *host, const input_record_t *record, int field) {
  if (field < 0 || !record) return 0.f;
  return gh_input_get_float(host, record->bytes, (unsigned)field);
}

void engine_input_set_float(game_host_t *host, input_record_t *record, int field, float value) {
  if (field < 0 || !record) return;
  gh_input_set_float(host, record->bytes, (unsigned)field, value);
}

ft_vec2 engine_input_get_vec2(game_host_t *host, const input_record_t *record, int field) {
  if (field < 0 || !record) return (ft_vec2){0.f, 0.f};
  return gh_input_get_vec2(host, record->bytes, (unsigned)field);
}

void engine_input_set_vec2(game_host_t *host, input_record_t *record, int field, ft_vec2 value) {
  if (field < 0 || !record) return;
  gh_input_set_vec2(host, record->bytes, (unsigned)field, value);
}

void engine_input_default(game_host_t *host, input_record_t *record) {
  if (!record) return;
  memset(record, 0, sizeof(*record));
  gh_input_default(host, record->bytes);
}

static void input_set_field_default(game_host_t *host, input_record_t *record, uint32_t field_index, const ft_input_field *field) {
  if (field->kind == FT_INPUT_FLOAT)
    engine_input_set_float(host, record, (int)field_index, field->default_float);
  else if (field->kind == FT_INPUT_VEC2)
    engine_input_set_vec2(host, record, (int)field_index, (ft_vec2){field->default_float, field->default_float});
  else
    engine_input_set(host, record, (int)field_index, field->default_value);
}

void engine_input_reset_triggers(game_host_t *host, input_record_t *record) {
  const ft_input_schema *schema = game_input_schema(host);
  if (!schema || !record) return;
  for (uint32_t field_index = 0; field_index < schema->field_count; ++field_index) {
    const ft_input_field *field = &schema->fields[field_index];
    if (field->flags & FT_INPUT_FLAG_TRIGGER) input_set_field_default(host, record, field_index, field);
  }
}

void engine_input_merge_pending_triggers(game_host_t *host, const input_record_t *pending, input_record_t *record) {
  const ft_input_schema *schema = game_input_schema(host);
  if (!schema || !pending || !record) return;
  for (uint32_t field_index = 0; field_index < schema->field_count; ++field_index) {
    const ft_input_field *field = &schema->fields[field_index];
    if ((field->flags & FT_INPUT_FLAG_TRIGGER) == 0) continue;

    if (field->kind == FT_INPUT_FLOAT) {
      const float value = engine_input_get_float(host, pending, (int)field_index);
      if (value != field->default_float) engine_input_set_float(host, record, (int)field_index, value);
    } else if (field->kind == FT_INPUT_VEC2) {
      const ft_vec2 value = engine_input_get_vec2(host, pending, (int)field_index);
      if (value.x != field->default_float || value.y != field->default_float)
        engine_input_set_vec2(host, record, (int)field_index, value);
    } else {
      const long long value = engine_input_get(host, pending, (int)field_index);
      if (value != field->default_value) engine_input_set(host, record, (int)field_index, value);
    }
  }
}
