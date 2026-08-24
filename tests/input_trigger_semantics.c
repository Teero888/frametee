#include <engine/input_record.h>
#include <logger/logger.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { FIELD_DIRECTION,
       FIELD_KILL,
       FIELD_COUNT };

typedef struct test_input_t {
  int direction;
  int kill;
} test_input_t;

static const ft_input_field s_fields[FIELD_COUNT] = {
    [FIELD_DIRECTION] = {.id = "direction", .kind = FT_INPUT_INT, .min_value = -1, .max_value = 1},
    [FIELD_KILL] = {.id = "kill", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_TRIGGER},
};

static const ft_input_schema s_schema = {
    .struct_size = sizeof(s_schema),
    .record_size = sizeof(test_input_t),
    .record_align = _Alignof(test_input_t),
    .fields = s_fields,
    .field_count = FIELD_COUNT,
};

const ft_input_schema *game_input_schema(const game_host_t *host) {
  (void)host;
  return &s_schema;
}

unsigned game_input_size(const game_host_t *host) {
  (void)host;
  return sizeof(test_input_t);
}

const char *game_host_active_id(const game_host_t *host) {
  (void)host;
  return "trigger-test";
}

void gh_input_default(game_host_t *host, void *record) {
  (void)host;
  memset(record, 0, sizeof(test_input_t));
}

long long gh_input_get(game_host_t *host, const void *record, unsigned field) {
  (void)host;
  const test_input_t *input = record;
  return field == FIELD_DIRECTION ? input->direction : field == FIELD_KILL ? input->kill
                                                                           : 0;
}

void gh_input_set(game_host_t *host, void *record, unsigned field, long long value) {
  (void)host;
  test_input_t *input = record;
  if (field == FIELD_DIRECTION) input->direction = (int)value;
  if (field == FIELD_KILL) input->kill = value != 0;
}

float gh_input_get_float(game_host_t *host, const void *record, unsigned field) {
  (void)host;
  (void)record;
  (void)field;
  return 0.f;
}

void gh_input_set_float(game_host_t *host, void *record, unsigned field, float value) {
  (void)host;
  (void)record;
  (void)field;
  (void)value;
}

ft_vec2 gh_input_get_vec2(game_host_t *host, const void *record, unsigned field) {
  (void)host;
  (void)record;
  (void)field;
  return (ft_vec2){0.f, 0.f};
}

void gh_input_set_vec2(game_host_t *host, void *record, unsigned field, ft_vec2 value) {
  (void)host;
  (void)record;
  (void)field;
  (void)value;
}

void logger_log(log_level_t level, const char *source, const char *format, ...) {
  (void)level;
  (void)source;
  (void)format;
}

static int value(game_host_t *host, const input_record_t *record, int field) {
  return (int)engine_input_get(host, record, field);
}

int main(void) {
  game_host_t host = {0};
  input_record_t pending;
  engine_input_default(&host, &pending);
  engine_input_set(&host, &pending, FIELD_DIRECTION, 1);
  engine_input_set(&host, &pending, FIELD_KILL, 1);

  // Rebuilding render-frame input must not discard a trigger before a game
  // tick has had a chance to record it.
  for (int frame = 0; frame < 5; ++frame) {
    input_record_t rebuilt;
    engine_input_default(&host, &rebuilt);
    engine_input_set(&host, &rebuilt, FIELD_DIRECTION, -1);
    engine_input_merge_pending_triggers(&host, &pending, &rebuilt);
    pending = rebuilt;
    if (value(&host, &pending, FIELD_KILL) != 1 || value(&host, &pending, FIELD_DIRECTION) != -1) {
      fprintf(stderr, "trigger was lost or overwrote state before consumption\n");
      return 1;
    }
  }

  // The recorded sample contains the edge, while subsequent ticks retain
  // ordinary state and see the trigger's default value.
  const input_record_t recorded = pending;
  engine_input_reset_triggers(&host, &pending);
  if (value(&host, &recorded, FIELD_KILL) != 1 || value(&host, &pending, FIELD_KILL) != 0 ||
      value(&host, &pending, FIELD_DIRECTION) != -1) {
    fprintf(stderr, "trigger was not consumed exactly once\n");
    return 1;
  }

  return 0;
}
