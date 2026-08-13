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
