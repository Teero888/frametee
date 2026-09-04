#include "save.h"

#include "fs.h"
#include <engine/engine_api.h>
#include <engine/game_host.h>
#include <engine/int_math.h>
#include <limits.h>
#include <logger/logger.h>
#include <math.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/snippet_editor.h>
#include <user_interface/timeline/timeline.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline_events.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

static const char *LOG_SOURCE = "SaveFile";

enum {
  PROJECT_MAX_GROUPS = 1024,
  PROJECT_MAX_TRACKS = 10000,
  PROJECT_MAX_EVENTS = 1000000,
  PROJECT_MAX_SOURCE_TICKS = 10000000,
  PROJECT_MAX_EFFECT_PAYLOAD = 4096,
};

#define PROJECT_MAX_FILE_SIZE ((size_t)1024 * 1024 * 1024)
#define PROJECT_MAX_BLOB_SIZE ((size_t)512 * 1024 * 1024)

typedef struct byte_buffer_t {
  uint8_t *data;
  size_t size;
  size_t capacity;
  bool ok;
} byte_buffer_t;

typedef struct byte_reader_t {
  const uint8_t *data;
  size_t size;
  size_t pos;
  bool ok;
} byte_reader_t;

typedef struct project_group_t {
  char name[MAX_TIMELINE_GROUP_NAME];
  float color[4];
  bool visible;
  bool export_enabled;
  bool prediction_enabled;
  int start_offset;
  uint8_t *world_data;
  size_t world_size;
} project_group_t;

typedef struct project_document_t {
  char game_id[FT_ID_MAX];
  char game_version[FT_NAME_MAX];
  char variant_id[FT_ID_MAX];
  char level_name[128];
  char level_path[1024];
  char camera_mode_id[FT_ID_MAX];
  uint64_t input_schema_hash;
  uint32_t input_record_size;

  float camera_pos[2];
  float camera_zoom;
  int current_tick;
  int active_group_index;
  int selected_track_index;
  bool linked_copy_input;

  uint8_t *level_data;
  size_t level_size;
  uint8_t *project_data;
  size_t project_size;
  project_group_t *groups;
  int group_count;
  player_track_t *tracks;
  int track_count;
  timeline_event_t *events;
  int event_count;
} project_document_t;

static bool checked_multiply(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) return false;
  *out = a * b;
  return true;
}

static bool buffer_reserve(byte_buffer_t *buffer, size_t extra) {
  if (!buffer->ok || extra > PROJECT_MAX_FILE_SIZE || buffer->size > PROJECT_MAX_FILE_SIZE - extra) {
    buffer->ok = false;
    return false;
  }
  const size_t needed = buffer->size + extra;
  if (needed <= buffer->capacity) return true;
  size_t capacity = buffer->capacity ? buffer->capacity : 4096;
  while (capacity < needed) {
    if (capacity > PROJECT_MAX_FILE_SIZE / 2) {
      capacity = PROJECT_MAX_FILE_SIZE;
      break;
    }
    capacity *= 2;
  }
  uint8_t *grown = realloc(buffer->data, capacity);
  if (!grown) {
    buffer->ok = false;
    return false;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return true;
}

static bool buffer_write(byte_buffer_t *buffer, const void *data, size_t size) {
  if (!buffer_reserve(buffer, size)) return false;
  if (size > 0) memcpy(buffer->data + buffer->size, data, size);
  buffer->size += size;
  return true;
}

static bool buffer_u8(byte_buffer_t *buffer, uint8_t value) { return buffer_write(buffer, &value, 1); }

static bool buffer_u32(byte_buffer_t *buffer, uint32_t value) {
  uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  return buffer_write(buffer, bytes, sizeof(bytes));
}

static bool buffer_i32(byte_buffer_t *buffer, int32_t value) { return buffer_u32(buffer, (uint32_t)value); }

static bool buffer_u64(byte_buffer_t *buffer, uint64_t value) {
  uint8_t bytes[8];
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = (uint8_t)(value >> (i * 8));
  return buffer_write(buffer, bytes, sizeof(bytes));
}

static bool buffer_i64(byte_buffer_t *buffer, int64_t value) { return buffer_u64(buffer, (uint64_t)value); }

static bool buffer_f32(byte_buffer_t *buffer, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return buffer_u32(buffer, bits);
}

static bool buffer_f64(byte_buffer_t *buffer, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return buffer_u64(buffer, bits);
}

static bool buffer_string(byte_buffer_t *buffer, const char *text) {
  const size_t length = text ? strlen(text) : 0;
  if (length > UINT32_MAX) {
    buffer->ok = false;
    return false;
  }
  return buffer_u32(buffer, (uint32_t)length) && buffer_write(buffer, text, length);
}

static bool buffer_patch_u64(byte_buffer_t *buffer, size_t offset, uint64_t value) {
  if (!buffer->ok || offset > buffer->size || buffer->size - offset < 8) return false;
  for (unsigned i = 0; i < 8; ++i)
    buffer->data[offset + i] = (uint8_t)(value >> (i * 8));
  return true;
}

static bool reader_bytes(byte_reader_t *reader, void *out, size_t size) {
  if (!reader->ok || reader->pos > reader->size || size > reader->size - reader->pos) {
    reader->ok = false;
    return false;
  }
  if (size > 0 && out) memcpy(out, reader->data + reader->pos, size);
  reader->pos += size;
  return true;
}

static bool reader_u8(byte_reader_t *reader, uint8_t *out) { return reader_bytes(reader, out, 1); }

static bool reader_u32(byte_reader_t *reader, uint32_t *out) {
  uint8_t bytes[4];
  if (!reader_bytes(reader, bytes, sizeof(bytes))) return false;
  *out = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
  return true;
}

static bool reader_i32(byte_reader_t *reader, int32_t *out) {
  uint32_t value;
  if (!reader_u32(reader, &value)) return false;
  *out = (int32_t)value;
  return true;
}

static bool reader_u64(byte_reader_t *reader, uint64_t *out) {
  uint8_t bytes[8];
  if (!reader_bytes(reader, bytes, sizeof(bytes))) return false;
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= (uint64_t)bytes[i] << (i * 8);
  *out = value;
  return true;
}

static bool reader_i64(byte_reader_t *reader, int64_t *out) {
  uint64_t value;
  if (!reader_u64(reader, &value)) return false;
  *out = (int64_t)value;
  return true;
}

static bool reader_f32(byte_reader_t *reader, float *out) {
  uint32_t bits;
  if (!reader_u32(reader, &bits)) return false;
  memcpy(out, &bits, sizeof(bits));
  return isfinite(*out);
}

static bool reader_f64(byte_reader_t *reader, double *out) {
  uint64_t bits;
  if (!reader_u64(reader, &bits)) return false;
  memcpy(out, &bits, sizeof(bits));
  return isfinite(*out);
}

static bool reader_string(byte_reader_t *reader, char *out, size_t capacity) {
  uint32_t length;
  if (!reader_u32(reader, &length) || capacity == 0 || length >= capacity) {
    reader->ok = false;
    return false;
  }
  if (!reader_bytes(reader, out, length)) return false;
  out[length] = '\0';
  return true;
}

static bool reader_blob(byte_reader_t *reader, uint64_t encoded_size, uint8_t **out, size_t *out_size) {
  if (reader->pos > reader->size || encoded_size > PROJECT_MAX_BLOB_SIZE || encoded_size > SIZE_MAX ||
      encoded_size > reader->size - reader->pos) {
    reader->ok = false;
    return false;
  }
  const size_t size = (size_t)encoded_size;
  uint8_t *data = size ? malloc(size) : NULL;
  if (size && !data) {
    reader->ok = false;
    return false;
  }
  if (!reader_bytes(reader, data, size)) {
    free(data);
    return false;
  }
  *out = data;
  *out_size = size;
  return true;
}

static bool reader_skip_blob(byte_reader_t *reader, uint64_t encoded_size) {
  if (reader->pos > reader->size || encoded_size > PROJECT_MAX_BLOB_SIZE || encoded_size > SIZE_MAX ||
      encoded_size > reader->size - reader->pos) {
    reader->ok = false;
    return false;
  }
  return reader_bytes(reader, NULL, (size_t)encoded_size);
}

static void project_document_free(project_document_t *document) {
  if (!document) return;
  if (document->tracks) {
    for (int i = 0; i < document->track_count; ++i) {
      player_track_t *track = &document->tracks[i];
      for (int j = 0; j < track->snippet_count; ++j)
        model_free_snippet_inputs(&track->snippets[j]);
      free(track->snippets);
    }
  }
  if (document->groups)
    for (int i = 0; i < document->group_count; ++i)
      free(document->groups[i].world_data);
  free(document->groups);
  free(document->tracks);
  free(document->events);
  free(document->level_data);
  free(document->project_data);
  memset(document, 0, sizeof(*document));
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = data;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
  uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_string(uint64_t hash, const char *text) {
  if (!text) text = "";
  return hash_bytes(hash, text, strlen(text) + 1);
}

static uint64_t input_schema_hash(const game_host_t *host) {
  const ft_input_schema *schema = game_input_schema(host);
  uint64_t hash = UINT64_C(1469598103934665603);
  if (!schema) return hash;
  hash = hash_u32(hash, schema->record_size);
  hash = hash_u32(hash, schema->record_align);
  hash = hash_u32(hash, schema->field_count);
  for (uint32_t i = 0; i < schema->field_count; ++i) {
    const ft_input_field *field = &schema->fields[i];
    hash = hash_string(hash, field->id);
    hash = hash_u32(hash, (uint32_t)field->kind);
    /* Editor visibility does not alter the stored record or its semantics and
     * must therefore not invalidate an otherwise compatible project. */
    hash = hash_u32(hash, field->flags & ~FT_INPUT_FLAG_EDITOR_HIDDEN);
    hash = hash_u32(hash, (uint32_t)field->min_value);
    hash = hash_u32(hash, (uint32_t)field->max_value);
    hash = hash_u32(hash, (uint32_t)field->default_value);
    uint32_t bits;
    memcpy(&bits, &field->min_float, sizeof(bits));
    hash = hash_u32(hash, bits);
    memcpy(&bits, &field->max_float, sizeof(bits));
    hash = hash_u32(hash, bits);
    memcpy(&bits, &field->default_float, sizeof(bits));
    hash = hash_u32(hash, bits);
    hash = hash_u32(hash, field->enum_count);
    for (uint32_t label = 0; label < field->enum_count; ++label)
      hash = hash_string(hash, field->enum_labels ? field->enum_labels[label] : "");
  }
  return hash;
}

/* Version 17 projects originally included the key-control table in the input
 * schema hash. Controls are editor bindings, not bytes stored in a project;
 * keep accepting those hashes so a binding-only change cannot strand a file. */
static uint64_t legacy_control_schema_hash(const game_host_t *host) {
  const ft_input_schema *schema = game_input_schema(host);
  uint64_t hash = input_schema_hash(host);
  if (!schema) return hash;
  hash = hash_u32(hash, schema->control_count);
  for (uint32_t i = 0; i < schema->control_count; ++i) {
    const ft_input_control *control = &schema->controls[i];
    hash = hash_string(hash, control->id);
    hash = hash_u32(hash, control->field);
    hash = hash_u32(hash, (uint32_t)control->value);
    hash = hash_u32(hash, (uint32_t)((uint64_t)control->value >> 32));
    hash = hash_u32(hash, control->flags);
  }
  return hash;
}

/* The multiple-binding migration collapsed controls named "foo" and
 * "foo_alt" into one "foo" control whose default binding contains '|'.
 * Reconstruct that one historical hash without restoring duplicate actions. */
static uint64_t legacy_split_control_schema_hash(const game_host_t *host) {
  const ft_input_schema *schema = game_input_schema(host);
  uint64_t hash = input_schema_hash(host);
  if (!schema) return hash;

  uint32_t legacy_count = 0;
  for (uint32_t i = 0; i < schema->control_count; ++i) {
    ++legacy_count;
    const char *binding = schema->controls[i].default_binding;
    while (binding && (binding = strchr(binding, '|')) != NULL) {
      ++legacy_count;
      ++binding;
    }
  }
  hash = hash_u32(hash, legacy_count);

  for (uint32_t i = 0; i < schema->control_count; ++i) {
    const ft_input_control *control = &schema->controls[i];
    const char *binding = control->default_binding;
    uint32_t binding_index = 0;
    do {
      char alternate_id[FT_NAME_MAX];
      const char *id = control->id;
      if (binding_index == 1) {
        if (snprintf(alternate_id, sizeof(alternate_id), "%s_alt", control->id) >= (int)sizeof(alternate_id))
          return UINT64_MAX;
        id = alternate_id;
      } else if (binding_index > 1) {
        if (snprintf(alternate_id, sizeof(alternate_id), "%s_alt%u", control->id, binding_index) >=
            (int)sizeof(alternate_id))
          return UINT64_MAX;
        id = alternate_id;
      }
      hash = hash_string(hash, id);
      hash = hash_u32(hash, control->field);
      hash = hash_u32(hash, (uint32_t)control->value);
      hash = hash_u32(hash, (uint32_t)((uint64_t)control->value >> 32));
      hash = hash_u32(hash, control->flags);

      binding = binding ? strchr(binding, '|') : NULL;
      if (binding) ++binding;
      ++binding_index;
    } while (binding);
  }
  return hash;
}

static bool collect_level_data(ui_handler_t *ui, uint8_t **out, size_t *out_size) {
  game_host_t *host = &ui->gfx_handler->game_host;
  const size_t needed = gh_level_serialize(host, ui->gfx_handler->level, NULL, 0);
  *out = NULL;
  *out_size = 0;
  if (needed == 0) return true;
  if (needed > PROJECT_MAX_BLOB_SIZE) return false;
  uint8_t *data = malloc(needed);
  if (!data) return false;
  if (gh_level_serialize(host, ui->gfx_handler->level, data, needed) != needed) {
    free(data);
    return false;
  }
  *out = data;
  *out_size = needed;
  return true;
}

static bool collect_project_data(game_host_t *host, uint8_t **out, size_t *out_size) {
  const size_t needed = gh_project_save(host, NULL, 0);
  *out = NULL;
  *out_size = 0;
  if (needed == 0) return true;
  if (needed > PROJECT_MAX_BLOB_SIZE) return false;
  uint8_t *data = malloc(needed);
  if (!data) return false;
  if (gh_project_save(host, data, needed) != needed) {
    free(data);
    return false;
  }
  *out = data;
  *out_size = needed;
  return true;
}

static bool write_world_blob(byte_buffer_t *buffer, game_host_t *host, const ft_world *world) {
  const size_t needed = gh_world_serialize(host, world, NULL, 0);
  if (needed > PROJECT_MAX_BLOB_SIZE || (game_has_cap(host, FT_CAP_WORLD_SERIALIZE) && needed == 0)) {
    buffer->ok = false;
    return false;
  }
  if (!buffer_u64(buffer, needed) || !buffer_reserve(buffer, needed)) return false;
  if (needed > 0 && gh_world_serialize(host, world, buffer->data + buffer->size, needed) != needed) {
    buffer->ok = false;
    return false;
  }
  buffer->size += needed;
  return true;
}

static bool write_value(byte_buffer_t *buffer, const starting_override_t *override) {
  const ft_value *value = &override->value;
  if (!buffer_u32(buffer, (uint32_t)value->kind)) return false;
  switch (value->kind) {
  case FT_VALUE_BOOL:
    return buffer_u8(buffer, value->as.b ? 1 : 0);
  case FT_VALUE_INT:
    return buffer_i64(buffer, value->as.i);
  case FT_VALUE_FLOAT:
    return buffer_f64(buffer, value->as.f);
  case FT_VALUE_VEC2:
    return buffer_f32(buffer, value->as.v.x) && buffer_f32(buffer, value->as.v.y);
  case FT_VALUE_VEC3:
    return buffer_f32(buffer, value->as.v3.x) && buffer_f32(buffer, value->as.v3.y) && buffer_f32(buffer, value->as.v3.z);
  case FT_VALUE_STRING: {
    const char *text = value->as.s ? value->as.s : override->string_value;
    if (strlen(text) >= MAX_STARTING_STRING) return false;
    return buffer_string(buffer, text);
  }
  default:
    buffer->ok = false;
    return false;
  }
}

static bool write_timeline(byte_buffer_t *buffer, ui_handler_t *ui) {
  timeline_state_t *timeline = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const uint32_t input_size = game_input_size(host);

  if (!buffer_i32(buffer, timeline->current_tick) || !buffer_i32(buffer, timeline->active_group_index) ||
      !buffer_i32(buffer, timeline->selected_player_track_index) || !buffer_u8(buffer, timeline->linked_copy_input ? 1 : 0))
    return false;

  for (int i = 0; i < timeline->group_count; ++i) {
    const timeline_group_t *group = timeline->groups[i];
    if (!buffer_string(buffer, group->name)) return false;
    for (int c = 0; c < 4; ++c)
      if (!buffer_f32(buffer, group->color[c])) return false;
    if (!buffer_u8(buffer, group->visible ? 1 : 0) || !buffer_u8(buffer, group->export_enabled ? 1 : 0) ||
        !buffer_u8(buffer, group->prediction_enabled ? 1 : 0) || !buffer_i32(buffer, group->start_offset) ||
        !write_world_blob(buffer, host, group->initial_world))
      return false;
  }

  for (int i = 0; i < timeline->player_track_count; ++i) {
    const player_track_t *track = &timeline->player_tracks[i];
    // The game's profile bytes travel as a length-delimited blob, exactly as the
    // game wrote them. The editor has no idea what is in there and never needs
    // to: a project opened by a different game simply hands it bytes it will
    // reject, which the game-id check above has already caught.
    if (!buffer_u32(buffer, track->player_profile.size) ||
        !buffer_write(buffer, track->player_profile.data, track->player_profile.size) ||
        !buffer_u8(buffer, track->is_linked ? 1 : 0) || !buffer_i32(buffer, track->linked_source_player) ||
        !buffer_u64(buffer, track->linked_copy_fields) || !buffer_u32(buffer, track->linked_transform_flags) ||
        !buffer_u8(buffer, track->starting_config.enabled ? 1 : 0))
      return false;

    int override_count = track->starting_config.override_count;
    if (override_count < 0 || override_count > MAX_STARTING_OVERRIDES || !buffer_u32(buffer, (uint32_t)override_count)) return false;
    for (int override_index = 0; override_index < override_count; ++override_index) {
      const starting_override_t *override = &track->starting_config.overrides[override_index];
      if (!buffer_string(buffer, override->prop_id) || !write_value(buffer, override)) return false;
    }

    if (!buffer_i32(buffer, track->group_index) || !buffer_string(buffer, track->name) ||
        !buffer_u8(buffer, track->export_enabled ? 1 : 0) || !buffer_u8(buffer, track->prediction_enabled ? 1 : 0) ||
        !buffer_u32(buffer, (uint32_t)track->snippet_count))
      return false;

    if (track->snippet_count < 0 || track->snippet_count > MAX_SNIPPETS_PER_PLAYER) return false;
    for (int j = 0; j < track->snippet_count; ++j) {
      const input_snippet_t *snippet = &track->snippets[j];
      if (snippet->source_count < 0 || snippet->source_count > PROJECT_MAX_SOURCE_TICKS || snippet->source_offset < 0 ||
          snippet->input_count < 0 || snippet->source_offset > snippet->source_count ||
          snippet->input_count > snippet->source_count - snippet->source_offset)
        return false;
      if (!buffer_i32(buffer, snippet->id) || !buffer_i32(buffer, snippet->start_tick) ||
          !buffer_u8(buffer, snippet->is_active ? 1 : 0) || !buffer_i32(buffer, snippet->layer) ||
          !buffer_i32(buffer, snippet->input_count) || !buffer_i32(buffer, snippet->source_offset) ||
          !buffer_i32(buffer, snippet->source_count))
        return false;
      for (int tick = 0; tick < snippet->source_count; ++tick)
        if (!buffer_write(buffer, snippet->inputs[tick].bytes, input_size)) return false;
      if (snippet->effect_count < 0 || snippet->effect_count > MAX_SNIPPET_INPUT_EFFECTS ||
          !buffer_u32(buffer, (uint32_t)snippet->effect_count))
        return false;
      for (int effect_index = 0; effect_index < snippet->effect_count; ++effect_index) {
        const input_effect_t *effect = &snippet->effects[effect_index];
        if (effect->parameter_size > PROJECT_MAX_EFFECT_PAYLOAD || !buffer_string(buffer, effect->type_id) ||
            !buffer_u8(buffer, effect->enabled ? 1 : 0) || !buffer_u32(buffer, effect->parameter_size) ||
            !buffer_write(buffer, effect->parameters, effect->parameter_size))
          return false;
      }
    }
  }

  if (timeline->event_count < 0 || timeline->event_count > PROJECT_MAX_EVENTS ||
      !buffer_u32(buffer, (uint32_t)timeline->event_count))
    return false;
  for (int i = 0; i < timeline->event_count; ++i) {
    const timeline_event_t *event = &timeline->events[i];
    if (!buffer_i32(buffer, event->tick) || !buffer_i32(buffer, event->group_index) || !buffer_i32(buffer, event->player) ||
        !buffer_string(buffer, event->category) || !buffer_string(buffer, event->message))
      return false;
    for (int c = 0; c < 4; ++c)
      if (!buffer_f32(buffer, event->color[c])) return false;
    if (event->data_size > FT_TIMELINE_EVENT_DATA_MAX || !buffer_u32(buffer, event->data_size) ||
        !buffer_write(buffer, event->data, event->data_size))
      return false;
  }
  return buffer->ok;
}

static bool write_project_file(ui_handler_t *ui, const char *path) {
  game_host_t *host = &ui->gfx_handler->game_host;
  uint8_t *level_data = NULL;
  size_t level_size = 0;
  uint8_t *project_data = NULL;
  size_t project_size = 0;
  byte_buffer_t buffer = {.ok = true};
  bool ok = false;

  if (!collect_level_data(ui, &level_data, &level_size) || !collect_project_data(host, &project_data, &project_size)) goto done;
  if (level_size == 0 && ui->loaded_level_path[0] == '\0') {
    log_error(LOG_SOURCE, "Game '%s' cannot serialize this level and no reloadable level path is available.", game_host_active_id(host));
    goto done;
  }

  const camera_t *camera = &ui->gfx_handler->renderer.camera;
  const ft_camera_mode *camera_mode = game_camera_mode(host, camera->mode);
  if (!buffer_write(&buffer, TAS_PROJECT_FILE_MAGIC, 4) || !buffer_u32(&buffer, TAS_PROJECT_FILE_VERSION) ||
      !buffer_u32(&buffer, 0) || !buffer_u64(&buffer, input_schema_hash(host)) ||
      !buffer_u32(&buffer, game_input_size(host)) || !buffer_u32(&buffer, (uint32_t)ui->timeline.group_count) ||
      !buffer_u32(&buffer, (uint32_t)ui->timeline.player_track_count) || !buffer_u64(&buffer, level_size) ||
      !buffer_u64(&buffer, project_size))
    goto done;
  const size_t timeline_size_offset = buffer.size;
  if (!buffer_u64(&buffer, 0) || !buffer_string(&buffer, game_host_active_id(host)) ||
      !buffer_string(&buffer, game_host_active_version(host)) || !buffer_string(&buffer, game_host_variant(host)) ||
      !buffer_string(&buffer, ui->loaded_level_name) || !buffer_string(&buffer, ui->loaded_level_path) ||
      !buffer_string(&buffer, camera_mode && camera_mode->id ? camera_mode->id : "free") ||
      !buffer_f32(&buffer, camera->pos[0]) || !buffer_f32(&buffer, camera->pos[1]) || !buffer_f32(&buffer, camera->zoom) ||
      !buffer_write(&buffer, level_data, level_size) || !buffer_write(&buffer, project_data, project_size))
    goto done;

  const size_t timeline_start = buffer.size;
  if (!write_timeline(&buffer, ui) || !buffer_patch_u64(&buffer, timeline_size_offset, buffer.size - timeline_start)) goto done;

  char temporary_path[1200];
  if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path) >= (int)sizeof(temporary_path)) goto done;
  FILE *file = fs_open(temporary_path, "wb");
  if (!file) {
    log_error(LOG_SOURCE, "Failed to open temporary project for writing: '%s'", temporary_path);
    goto done;
  }
  setvbuf(file, NULL, _IOFBF, 64 * 1024);
  const bool wrote = buffer.size == 0 || fwrite(buffer.data, buffer.size, 1, file) == 1;
  const bool flushed = fflush(file) == 0;
  const bool closed = fclose(file) == 0;
  if (!wrote || !flushed || !closed || !fs_replace(temporary_path, path)) {
    fs_remove(temporary_path);
    log_error(LOG_SOURCE, "Failed to finish writing project: '%s'", path);
    goto done;
  }
  ok = true;

done:
  free(level_data);
  free(project_data);
  free(buffer.data);
  return ok;
}

bool save_project(ui_handler_t *ui, const char *path) {
  if (!ui || !path || !*path || !ui->gfx_handler || !ui->gfx_handler->level || !game_host_ready(&ui->gfx_handler->game_host))
    return false;

  // Ctrl+S passes current_project_path itself.  snprintf has undefined
  // behaviour when its source and destination overlap, and on glibc this was
  // clearing the path after the file had been written.
  char stable_path[sizeof(ui->current_project_path)];
  int path_length = snprintf(stable_path, sizeof(stable_path), "%s", path);
  if (path_length <= 0 || (size_t)path_length >= sizeof(stable_path)) {
    log_error(LOG_SOURCE, "Project path is too long.");
    return false;
  }

  const double start = glfwGetTime();
  if (!write_project_file(ui, stable_path)) return false;

  snprintf(ui->current_project_path, sizeof(ui->current_project_path), "%s", stable_path);
  ui->has_unsaved_changes = false;
  ui_add_recent_project(ui, stable_path);
  log_info(LOG_SOURCE, "Project saved successfully to '%s' (%.2f ms)", stable_path, (glfwGetTime() - start) * 1000.0);
  return true;
}

static bool read_value(byte_reader_t *reader, starting_override_t *override) {
  uint32_t kind;
  if (!reader_u32(reader, &kind) || kind > FT_VALUE_STRING) return false;
  override->value.kind = (ft_value_kind)kind;
  switch (override->value.kind) {
  case FT_VALUE_BOOL: {
    uint8_t value;
    if (!reader_u8(reader, &value) || value > 1) return false;
    override->value.as.b = value != 0;
    return true;
  }
  case FT_VALUE_INT:
    return reader_i64(reader, &override->value.as.i);
  case FT_VALUE_FLOAT:
    return reader_f64(reader, &override->value.as.f);
  case FT_VALUE_VEC2:
    return reader_f32(reader, &override->value.as.v.x) && reader_f32(reader, &override->value.as.v.y);
  case FT_VALUE_VEC3:
    return reader_f32(reader, &override->value.as.v3.x) && reader_f32(reader, &override->value.as.v3.y) &&
           reader_f32(reader, &override->value.as.v3.z);
  case FT_VALUE_STRING:
    if (!reader_string(reader, override->string_value, sizeof(override->string_value))) return false;
    override->value.as.s = override->string_value;
    return true;
  }
  return false;
}

static bool read_timeline(byte_reader_t *reader, project_document_t *document, bool keep_world_data) {
  uint8_t boolean;
  uint32_t count;
  if (!reader_i32(reader, &document->current_tick) || !reader_i32(reader, &document->active_group_index) ||
      !reader_i32(reader, &document->selected_track_index) || !reader_u8(reader, &boolean) || boolean > 1)
    return false;
  document->linked_copy_input = boolean != 0;

  for (int i = 0; i < document->group_count; ++i) {
    project_group_t *group = &document->groups[i];
    if (!reader_string(reader, group->name, sizeof(group->name))) return false;
    for (int c = 0; c < 4; ++c)
      if (!reader_f32(reader, &group->color[c])) return false;
    uint8_t visible, export_enabled, prediction_enabled;
    uint64_t world_size;
    if (!reader_u8(reader, &visible) || visible > 1 || !reader_u8(reader, &export_enabled) || export_enabled > 1 ||
        !reader_u8(reader, &prediction_enabled) || prediction_enabled > 1 || !reader_i32(reader, &group->start_offset) ||
        !reader_u64(reader, &world_size))
      return false;
    if (keep_world_data) {
      if (!reader_blob(reader, world_size, &group->world_data, &group->world_size)) return false;
    } else if (!reader_skip_blob(reader, world_size)) {
      return false;
    }
    group->visible = visible != 0;
    group->export_enabled = export_enabled != 0;
    group->prediction_enabled = prediction_enabled != 0;
  }

  for (int i = 0; i < document->track_count; ++i) {
    player_track_t *track = &document->tracks[i];
    uint8_t is_linked, starting_enabled, export_enabled, prediction_enabled;
    uint32_t profile_size;
    if (!reader_u32(reader, &profile_size) || profile_size > FT_PLAYER_PROFILE_MAX ||
        !reader_bytes(reader, track->player_profile.data, profile_size))
      return false;
    track->player_profile.size = profile_size;
    if (!reader_u8(reader, &is_linked) || is_linked > 1 || !reader_i32(reader, &track->linked_source_player) ||
        !reader_u64(reader, &track->linked_copy_fields) || !reader_u32(reader, &track->linked_transform_flags) ||
        !reader_u8(reader, &starting_enabled) || starting_enabled > 1 || !reader_u32(reader, &count) ||
        count > MAX_STARTING_OVERRIDES)
      return false;
    track->is_linked = is_linked != 0;
    track->starting_config.enabled = starting_enabled != 0;
    track->starting_config.override_count = (int)count;
    for (uint32_t override_index = 0; override_index < count; ++override_index) {
      starting_override_t *override = &track->starting_config.overrides[override_index];
      if (!reader_string(reader, override->prop_id, sizeof(override->prop_id)) || !read_value(reader, override)) return false;
    }
    if (!reader_i32(reader, &track->group_index) || track->group_index < 0 || track->group_index >= document->group_count ||
        !reader_string(reader, track->name, sizeof(track->name)) || !reader_u8(reader, &export_enabled) || export_enabled > 1 ||
        !reader_u8(reader, &prediction_enabled) || prediction_enabled > 1 || !reader_u32(reader, &count) ||
        count > MAX_SNIPPETS_PER_PLAYER)
      return false;
    track->export_enabled = export_enabled != 0;
    track->prediction_enabled = prediction_enabled != 0;
    track->snippet_count = (int)count;
    track->snippet_capacity = (int)count;
    if (count > 0) {
      track->snippets = calloc(count, sizeof(*track->snippets));
      if (!track->snippets) return false;
    }
    for (uint32_t j = 0; j < count; ++j) {
      input_snippet_t *snippet = &track->snippets[j];
      uint8_t active;
      if (!reader_i32(reader, &snippet->id) || !reader_i32(reader, &snippet->start_tick) ||
          !reader_u8(reader, &active) || active > 1 || !reader_i32(reader, &snippet->layer) ||
          !reader_i32(reader, &snippet->input_count) || !reader_i32(reader, &snippet->source_offset) ||
          !reader_i32(reader, &snippet->source_count))
        return false;
      snippet->is_active = active != 0;
      if (snippet->layer < 0 || snippet->layer >= MAX_SNIPPET_LAYERS || snippet->input_count < 0 ||
          snippet->source_count < 0 || snippet->source_count > PROJECT_MAX_SOURCE_TICKS || snippet->source_offset < 0 ||
          snippet->source_offset > snippet->source_count ||
          snippet->input_count > snippet->source_count - snippet->source_offset)
        return false;
      size_t allocation_size;
      if (!checked_multiply((size_t)snippet->source_count, sizeof(*snippet->inputs), &allocation_size)) return false;
      snippet->inputs = snippet->source_count ? calloc(1, allocation_size) : NULL;
      if (snippet->source_count && !snippet->inputs) return false;
      for (int tick = 0; tick < snippet->source_count; ++tick)
        if (!reader_bytes(reader, snippet->inputs[tick].bytes, document->input_record_size)) return false;
      uint32_t effect_count;
      if (!reader_u32(reader, &effect_count) || effect_count > MAX_SNIPPET_INPUT_EFFECTS) return false;
      snippet->effect_count = (int)effect_count;
      snippet->effect_capacity = (int)effect_count;
      snippet->effects = effect_count ? calloc(effect_count, sizeof(*snippet->effects)) : NULL;
      if (effect_count && !snippet->effects) return false;
      for (uint32_t effect_index = 0; effect_index < effect_count; ++effect_index) {
        input_effect_t *effect = &snippet->effects[effect_index];
        uint8_t enabled;
        uint32_t payload_size;
        if (!reader_string(reader, effect->type_id, sizeof(effect->type_id)) || !reader_u8(reader, &enabled) ||
            enabled > 1 || !reader_u32(reader, &payload_size) || payload_size > PROJECT_MAX_EFFECT_PAYLOAD ||
            payload_size > FT_INPUT_EFFECT_PARAMETER_MAX)
          return false;
        effect->enabled = enabled != 0;
        effect->parameter_size = payload_size;
        effect->parameters = payload_size ? malloc(payload_size) : NULL;
        if (payload_size > 0 && (!effect->parameters || !reader_bytes(reader, effect->parameters, payload_size)))
          return false;
      }
      model_snippet_normalize(snippet);
    }
    model_rebind_starting_strings(&track->starting_config);
  }

  if (!reader_u32(reader, &count) || count > PROJECT_MAX_EVENTS) return false;
  document->event_count = (int)count;
  document->events = count ? calloc(count, sizeof(*document->events)) : NULL;
  if (count && !document->events) return false;
  for (uint32_t i = 0; i < count; ++i) {
    timeline_event_t *event = &document->events[i];
    if (!reader_i32(reader, &event->tick) || !reader_i32(reader, &event->group_index) || event->group_index < 0 ||
        event->group_index >= document->group_count || !reader_i32(reader, &event->player) ||
        !reader_string(reader, event->category, sizeof(event->category)) ||
        !reader_string(reader, event->message, sizeof(event->message)))
      return false;
    for (int c = 0; c < 4; ++c)
      if (!reader_f32(reader, &event->color[c])) return false;
    if (!reader_u32(reader, &event->data_size) || event->data_size > FT_TIMELINE_EVENT_DATA_MAX ||
        !reader_bytes(reader, event->data, event->data_size))
      return false;
  }
  return reader->ok && reader->pos == reader->size;
}

static bool validate_document_compatibility(const project_document_t *document, game_host_t *host, const char *path) {
  if (strcmp(document->game_id, game_host_active_id(host)) != 0) {
    log_error(LOG_SOURCE, "Project '%s' belongs to game '%s'; start a project with that game first.", path, document->game_id);
    return false;
  }
  if (strcmp(document->game_version, game_host_active_version(host)) != 0) {
    log_error(LOG_SOURCE, "Project '%s' targets %s v%s, but v%s is active.", path, document->game_id,
              document->game_version, game_host_active_version(host));
    return false;
  }
  const uint64_t schema_hash = input_schema_hash(host);
  const bool compatible_schema = document->input_schema_hash == schema_hash ||
                                 document->input_schema_hash == legacy_control_schema_hash(host) ||
                                 document->input_schema_hash == legacy_split_control_schema_hash(host);
  if (document->input_record_size != game_input_size(host) || !compatible_schema) {
    log_error(LOG_SOURCE, "Project '%s' uses a different input schema for game '%s'.", path, document->game_id);
    return false;
  }
  return true;
}

// The game id sits in the fixed part of the header, before any blob, so the
// game a project needs can be known without parsing (or being able to parse)
// the rest of it. Reads the prefix only.
static bool peek_project_game_id(const char *path, char *out_id, size_t out_size) {
  // Everything ahead of the id is fixed width, and the id itself is a short
  // length-prefixed string, so the whole prefix fits comfortably in this.
  enum { HEADER_PREFIX_MAX = 512 };
  FILE *file = fs_open(path, "rb");
  if (!file) {
    log_error(LOG_SOURCE, "Failed to open project for reading: '%s'", path);
    return false;
  }
  uint8_t header[HEADER_PREFIX_MAX];
  const size_t read = fread(header, 1, sizeof(header), file);
  fclose(file);

  byte_reader_t reader = {.data = header, .size = read, .ok = true};
  char magic[4];
  uint32_t version, flags, record_size, group_count, track_count;
  uint64_t schema_hash, level_size, project_size, timeline_size;
  if (!reader_bytes(&reader, magic, sizeof(magic)) || memcmp(magic, TAS_PROJECT_FILE_MAGIC, 4) != 0 ||
      !reader_u32(&reader, &version)) {
    log_error(LOG_SOURCE, "Not a FrameTee project: '%s'", path);
    return false;
  }
  if (version != TAS_PROJECT_FILE_VERSION) {
    log_error(LOG_SOURCE, "Project '%s' is version %u; this build requires version %u.", path, version,
              TAS_PROJECT_FILE_VERSION);
    return false;
  }
  if (!reader_u32(&reader, &flags) || !reader_u64(&reader, &schema_hash) || !reader_u32(&reader, &record_size) ||
      !reader_u32(&reader, &group_count) || !reader_u32(&reader, &track_count) || !reader_u64(&reader, &level_size) ||
      !reader_u64(&reader, &project_size) || !reader_u64(&reader, &timeline_size) ||
      !reader_string(&reader, out_id, out_size)) {
    log_error(LOG_SOURCE, "Malformed or incomplete v%u project: '%s'", TAS_PROJECT_FILE_VERSION, path);
    return false;
  }
  return true;
}

// A project names the game it was authored under, and only that game can read
// it. Opening one is allowed to switch to that game: the splash screen used to
// be the only way in, which is the only reason this ever had to be the user's
// job. Import does not go through here, because a foreign game's project has
// nothing to contribute to the timeline this one is building.
static bool activate_project_game(ui_handler_t *ui, const char *path) {
  char game_id[FT_ID_MAX];
  if (!peek_project_game_id(path, game_id, sizeof(game_id))) return false;

  game_host_t *host = &ui->gfx_handler->game_host;
  const char *active = game_host_active_id(host);
  if (active && strcmp(active, game_id) == 0) return true;

  const int index = game_host_find_id(host, game_id);
  if (index < 0) {
    log_error(LOG_SOURCE, "Project '%s' needs game '%s', which is not installed.", path, game_id);
    return false;
  }
  // Switching clears the level and timeline, so a load that fails after this
  // leaves an empty project under the new game and the splash comes back up.
  if (!gfx_activate_game(ui->gfx_handler, index)) {
    log_error(LOG_SOURCE, "Could not switch to game '%s' to open '%s'.", game_id, path);
    return false;
  }
  log_info(LOG_SOURCE, "Switched to game '%s' to open '%s'.", game_id, path);
  return true;
}

static bool read_project_file(ui_handler_t *ui, const char *path, project_document_t *document, bool keep_session_data) {
  memset(document, 0, sizeof(*document));
  FILE *file = fs_open(path, "rb");
  if (!file) {
    log_error(LOG_SOURCE, "Failed to open project for reading: '%s'", path);
    return false;
  }
  bool ok = false;
  uint8_t *file_data = NULL;
  if (fseek(file, 0, SEEK_END) != 0) goto done;
  const long file_length = ftell(file);
  if (file_length < 0 || (size_t)file_length > PROJECT_MAX_FILE_SIZE || fseek(file, 0, SEEK_SET) != 0) goto done;
  file_data = file_length ? malloc((size_t)file_length) : NULL;
  if (file_length && (!file_data || fread(file_data, (size_t)file_length, 1, file) != 1)) goto done;

  byte_reader_t reader = {.data = file_data, .size = (size_t)file_length, .ok = true};
  char magic[4];
  uint32_t version, flags, group_count, track_count;
  uint64_t level_size, project_size, timeline_size;
  if (!reader_bytes(&reader, magic, sizeof(magic)) || memcmp(magic, TAS_PROJECT_FILE_MAGIC, 4) != 0 ||
      !reader_u32(&reader, &version)) {
    log_error(LOG_SOURCE, "Not a FrameTee project: '%s'", path);
    goto done;
  }
  if (version != TAS_PROJECT_FILE_VERSION) {
    log_error(LOG_SOURCE, "Project '%s' is version %u; this build requires version %u.", path, version,
              TAS_PROJECT_FILE_VERSION);
    goto done;
  }
  if (!reader_u32(&reader, &flags) || flags != 0 || !reader_u64(&reader, &document->input_schema_hash) ||
      !reader_u32(&reader, &document->input_record_size) || !reader_u32(&reader, &group_count) ||
      group_count == 0 || group_count > PROJECT_MAX_GROUPS || !reader_u32(&reader, &track_count) ||
      track_count > PROJECT_MAX_TRACKS || !reader_u64(&reader, &level_size) || !reader_u64(&reader, &project_size) ||
      !reader_u64(&reader, &timeline_size) || !reader_string(&reader, document->game_id, sizeof(document->game_id)) ||
      !reader_string(&reader, document->game_version, sizeof(document->game_version)) ||
      !reader_string(&reader, document->variant_id, sizeof(document->variant_id)) ||
      !reader_string(&reader, document->level_name, sizeof(document->level_name)) ||
      !reader_string(&reader, document->level_path, sizeof(document->level_path)) ||
      !reader_string(&reader, document->camera_mode_id, sizeof(document->camera_mode_id)) ||
      !reader_f32(&reader, &document->camera_pos[0]) || !reader_f32(&reader, &document->camera_pos[1]) ||
      !reader_f32(&reader, &document->camera_zoom) || document->camera_zoom <= 0.f)
    goto malformed;

  document->group_count = (int)group_count;
  document->track_count = (int)track_count;
  if (!validate_document_compatibility(document, &ui->gfx_handler->game_host, path)) goto done;
  if (level_size == 0 && document->level_path[0] == '\0') goto malformed;
  if (keep_session_data) {
    if (!reader_blob(&reader, level_size, &document->level_data, &document->level_size) ||
        !reader_blob(&reader, project_size, &document->project_data, &document->project_size))
      goto malformed;
  } else if (!reader_skip_blob(&reader, level_size) || !reader_skip_blob(&reader, project_size)) {
    goto malformed;
  }
  if (timeline_size > reader.size - reader.pos) goto malformed;

  document->groups = calloc(group_count, sizeof(*document->groups));
  document->tracks = track_count ? calloc(track_count, sizeof(*document->tracks)) : NULL;
  if (!document->groups || (track_count && !document->tracks)) goto malformed;

  byte_reader_t timeline_reader = {.data = reader.data + reader.pos, .size = (size_t)timeline_size, .ok = true};
  reader.pos += (size_t)timeline_size;
  if (!read_timeline(&timeline_reader, document, keep_session_data) || reader.pos != reader.size)
    goto malformed;

  if (document->active_group_index < 0 || document->active_group_index >= document->group_count ||
      document->selected_track_index < -1 || document->selected_track_index >= document->track_count)
    goto malformed;
  for (int group = 0; group < document->group_count; ++group) {
    int players = 0;
    for (int track = 0; track < document->track_count; ++track)
      if (document->tracks[track].group_index == group) ++players;
    if (players < game_min_players(&ui->gfx_handler->game_host) ||
        (game_max_players(&ui->gfx_handler->game_host) > 0 && players > game_max_players(&ui->gfx_handler->game_host)))
      goto malformed;
  }
  ok = true;
  goto done;

malformed:
  log_error(LOG_SOURCE, "Malformed or incomplete v%u project: '%s'", TAS_PROJECT_FILE_VERSION, path);
done:
  free(file_data);
  fclose(file);
  if (!ok) project_document_free(document);
  return ok;
}

static int document_group_track_count(const project_document_t *document, int group_index) {
  int count = 0;
  for (int i = 0; i < document->track_count; ++i)
    if (document->tracks[i].group_index == group_index) ++count;
  return count;
}

static bool populate_timeline_from_document(timeline_state_t *timeline, project_document_t *document) {
  ui_handler_t *ui = timeline->ui;
  game_host_t *host = &ui->gfx_handler->game_host;

  for (int i = timeline->player_track_count - 1; i >= 0; --i)
    model_remove_track_logic(timeline, i);
  while (timeline->group_count < document->group_count)
    if (!model_add_group(timeline, document->groups[timeline->group_count].name)) return false;

  for (int i = 0; i < document->group_count; ++i) {
    project_group_t *source = &document->groups[i];
    timeline_group_t *destination = timeline->groups[i];
    snprintf(destination->name, sizeof(destination->name), "%s", source->name);
    memcpy(destination->color, source->color, sizeof(destination->color));
    destination->visible = source->visible;
    destination->export_enabled = source->export_enabled;
    destination->prediction_enabled = source->prediction_enabled;
    destination->start_offset = source->start_offset;
    if (source->world_size > 0) {
      if (!gh_world_deserialize(host, destination->initial_world, source->world_data, source->world_size) ||
          gh_world_player_count(host, destination->initial_world) != document_group_track_count(document, i))
        return false;
    }
  }

  int max_snippet_id = 0;
  for (int group = 0; group < document->group_count; ++group) {
    timeline->active_group_index = group;
    for (int i = 0; i < document->track_count; ++i) {
      player_track_t *source = &document->tracks[i];
      if (source->group_index != group) continue;
      player_track_t *destination = model_add_new_track(timeline, 1);
      if (!destination) return false;
      const int destination_group = destination->group_index;
      *destination = *source;
      destination->group_index = destination_group;
      destination->recording_snippets = NULL;
      destination->recording_snippet_count = 0;
      destination->recording_snippet_capacity = 0;
      model_rebind_starting_strings(&destination->starting_config);
      source->snippets = NULL;
      source->snippet_count = 0;
      source->snippet_capacity = 0;
      for (int j = 0; j < destination->snippet_count; ++j)
        if (destination->snippets[j].id > max_snippet_id) max_snippet_id = destination->snippets[j].id;
    }
  }

  for (int i = 0; i < timeline->player_track_count; ++i) {
    const int group = timeline->player_tracks[i].group_index;
    if (document->groups[group].world_size == 0 && timeline->player_tracks[i].starting_config.enabled)
      model_apply_starting_config(timeline, i);
  }

  timeline->event_count = document->event_count;
  timeline->event_capacity = document->event_count;
  timeline->events = document->event_count ? malloc(sizeof(*timeline->events) * (size_t)document->event_count) : NULL;
  if (document->event_count && !timeline->events) return false;
  if (document->event_count)
    memcpy(timeline->events, document->events, sizeof(*timeline->events) * (size_t)document->event_count);

  timeline->next_snippet_id = max_snippet_id + 1;
  timeline->current_tick = document->current_tick;
  timeline->active_group_index = document->active_group_index;
  timeline->selected_player_track_index = document->selected_track_index;
  timeline->linked_copy_input = document->linked_copy_input;
  model_recalc_physics(timeline, 0);
  return true;
}

static ft_level *load_document_level(game_host_t *host, const project_document_t *document) {
  if (document->level_size > 0) return gh_level_load_memory(host, document->level_data, document->level_size);
  return gh_level_load_path(host, document->level_path);
}

static void update_level_metadata(ui_handler_t *ui, const project_document_t *document) {
  gfx_handler_t *handler = ui->gfx_handler;
  ft_level_info info;
  if (gh_level_info(&handler->game_host, handler->level, &info) && info.bounds.w > 0.f && info.bounds.h > 0.f)
    engine_api_set_world_extent(handler, info.bounds.w, info.bounds.h);
  else
    engine_api_set_world_extent(handler, 1.f, 1.f);
  snprintf(ui->loaded_level_name, sizeof(ui->loaded_level_name), "%s", document->level_name);
  snprintf(ui->loaded_level_path, sizeof(ui->loaded_level_path), "%s", document->level_path);
}

static void restore_camera(ui_handler_t *ui, const project_document_t *document) {
  game_host_t *host = &ui->gfx_handler->game_host;
  camera_t *camera = &ui->gfx_handler->renderer.camera;
  camera->mode = 0;
  for (unsigned i = 0; i < game_camera_mode_count(host); ++i) {
    const ft_camera_mode *mode = game_camera_mode(host, i);
    if (mode && mode->id && strcmp(mode->id, document->camera_mode_id) == 0) {
      camera->mode = i;
      break;
    }
  }
  camera->pos[0] = document->camera_pos[0];
  camera->pos[1] = document->camera_pos[1];
  camera->zoom = document->camera_zoom;
  camera->zoom_wanted = document->camera_zoom;
  camera->is_dragging = false;
}

bool load_project(ui_handler_t *ui, const char *path) {
  if (!ui || !path || !ui->gfx_handler) return false;
  if (!activate_project_game(ui, path)) return false;
  project_document_t document;
  if (!read_project_file(ui, path, &document, true)) return false;

  game_host_t *host = &ui->gfx_handler->game_host;
  char old_variant[FT_ID_MAX];
  snprintf(old_variant, sizeof(old_variant), "%s", game_host_variant(host));
  uint8_t *old_project_data = NULL;
  size_t old_project_size = 0;
  if (!collect_project_data(host, &old_project_data, &old_project_size)) {
    project_document_free(&document);
    return false;
  }

  game_host_set_variant(host, document.variant_id[0] ? document.variant_id : NULL);
  if (document.variant_id[0] && strcmp(game_host_variant(host), document.variant_id) != 0) {
    log_error(LOG_SOURCE, "Project '%s' requests unknown variant '%s'.", path, document.variant_id);
    goto failed_before_timeline;
  }
  if (!gh_project_load(host, document.project_data, document.project_size)) {
    log_error(LOG_SOURCE, "Game '%s' rejected its project data in '%s'.", document.game_id, path);
    goto failed_before_timeline;
  }

  ft_level *new_level = load_document_level(host, &document);
  if (!new_level) {
    log_error(LOG_SOURCE, "Game '%s' could not load the level stored by '%s'.", document.game_id, path);
    goto failed_before_timeline;
  }

  gfx_handler_t *handler = ui->gfx_handler;
  ft_level *old_level = handler->level;
  handler->level = new_level;
  timeline_state_t loaded = {0};
  model_init(&loaded, ui);
  if (!populate_timeline_from_document(&loaded, &document)) {
    model_cleanup(&loaded);
    handler->level = old_level;
    gh_level_destroy(host, new_level);
    log_error(LOG_SOURCE, "Game '%s' rejected a stored world or player layout in '%s'.", document.game_id, path);
    goto failed_before_timeline;
  }

  timeline_state_t previous = ui->timeline;
  snippet_editor_reset();
  undo_manager_cleanup(&ui->undo_manager);
  undo_manager_init(&ui->undo_manager);
  ui->timeline = loaded;
  model_cleanup(&previous);
  gh_level_destroy(host, old_level);

  update_level_metadata(ui, &document);
  restore_camera(ui, &document);
  snprintf(ui->current_project_path, sizeof(ui->current_project_path), "%s", path);
  ui->has_unsaved_changes = false;
  ui_add_recent_project(ui, path);
  log_info(LOG_SOURCE, "Project loaded successfully from '%s'", path);

  free(old_project_data);
  project_document_free(&document);
  return true;

failed_before_timeline:
  game_host_set_variant(host, old_variant[0] ? old_variant : NULL);
  if (!gh_project_load(host, old_project_data, old_project_size))
    log_error(LOG_SOURCE, "Game '%s' could not restore its previous project state after a failed load.", game_host_active_id(host));
  free(old_project_data);
  project_document_free(&document);
  return false;
}

static const char *project_file_stem(const char *path, char *buffer, size_t buffer_size) {
  const char *name = path;
  for (const char *cursor = path; *cursor; ++cursor)
    if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
  snprintf(buffer, buffer_size, "%s", *name ? name : "Imported group");
  char *dot = strrchr(buffer, '.');
  if (dot && dot != buffer) *dot = '\0';
  return buffer;
}

static bool prepare_imported_events(const timeline_state_t *timeline, const project_document_t *document,
                                    timeline_event_t **out_events, int *out_count) {
  // Payload-free events are observations from simulating the source map. They
  // must be regenerated on the current map; only game-authored events travel
  // with the imported timeline.
  int authored_count = 0;
  for (int i = 0; i < document->event_count; ++i)
    if (document->events[i].data_size > 0) ++authored_count;

  if (timeline->event_count > PROJECT_MAX_EVENTS - authored_count) return false;
  const int count = timeline->event_count + authored_count;
  size_t allocation_size;
  if (!checked_multiply((size_t)count, sizeof(**out_events), &allocation_size)) return false;

  timeline_event_t *events = count > 0 ? malloc(allocation_size) : NULL;
  if (count > 0 && !events) return false;
  if (timeline->event_count > 0)
    memcpy(events, timeline->events, sizeof(*events) * (size_t)timeline->event_count);

  int destination = timeline->event_count;
  for (int i = 0; i < document->event_count; ++i) {
    if (document->events[i].data_size == 0) continue;
    events[destination] = document->events[i];
    events[destination].group_index += timeline->group_count;
    ++destination;
  }
  *out_events = events;
  *out_count = count;
  return true;
}

static bool validate_import_capacity(const timeline_state_t *timeline, const project_document_t *document,
                                     int *out_snippet_count) {
  if (timeline->group_count > PROJECT_MAX_GROUPS - document->group_count ||
      timeline->player_track_count > PROJECT_MAX_TRACKS - document->track_count)
    return false;

  int snippet_count = 0;
  for (int track = 0; track < document->track_count; ++track) {
    const int count = document->tracks[track].snippet_count;
    if (count > INT_MAX - snippet_count) return false;
    snippet_count += count;
  }
  if (timeline->next_snippet_id <= 0 || snippet_count > INT_MAX - timeline->next_snippet_id) return false;
  *out_snippet_count = snippet_count;
  return true;
}

bool import_project_as_group(ui_handler_t *ui, const char *path) {
  if (!ui || !path || !*path || !ui->gfx_handler || !ui->gfx_handler->level ||
      !game_host_ready(&ui->gfx_handler->game_host) || ui->timeline.recording)
    return false;
  project_document_t document;
  // Import only needs portable timeline data. The source level, game-owned
  // project state and serialized worlds are validated and skipped by the reader
  // rather than allocated, loaded or compared with the active session.
  if (!read_project_file(ui, path, &document, false)) return false;

  timeline_state_t *timeline = &ui->timeline;
  const int original_group_count = timeline->group_count;
  const int original_active_group = timeline->active_group_index;
  const int original_selected_track = timeline->selected_player_track_index;
  const int original_next_snippet_id = timeline->next_snippet_id;
  int imported_snippet_count = 0;
  timeline_event_t *merged_events = NULL;
  int merged_event_count = 0;
  bool ok = false;
  char stem[MAX_TIMELINE_GROUP_NAME];
  project_file_stem(path, stem, sizeof(stem));

  if (!validate_import_capacity(timeline, &document, &imported_snippet_count) ||
      !prepare_imported_events(timeline, &document, &merged_events, &merged_event_count)) {
    log_error(LOG_SOURCE, "Cannot import '%s': the combined project is too large.", path);
    free(merged_events);
    project_document_free(&document);
    return false;
  }

  int next_snippet_id = original_next_snippet_id;

  for (int source_group = 0; source_group < document.group_count; ++source_group) {
    project_group_t *source_group_data = &document.groups[source_group];
    timeline_group_t *group = model_add_group(timeline, source_group_data->name[0] ? source_group_data->name : stem);
    if (!group || !group->initial_world) goto done;
    memcpy(group->color, source_group_data->color, sizeof(group->color));
    group->visible = source_group_data->visible;
    group->export_enabled = source_group_data->export_enabled;
    group->prediction_enabled = source_group_data->prediction_enabled;
    group->start_offset = source_group_data->start_offset;
    const int destination_group = timeline->group_count - 1;

    // model_add_group created every runtime world from the active level and
    // variant. A source-map world snapshot is intentionally never restored.

    timeline->active_group_index = destination_group;
    for (int source_track = 0; source_track < document.track_count; ++source_track) {
      player_track_t *from = &document.tracks[source_track];
      if (from->group_index != source_group) continue;
      player_track_t *to = model_add_new_track(timeline, 1);
      if (!to) goto done;
      const int preserved_group = to->group_index;
      *to = *from;
      to->group_index = preserved_group;
      to->recording_snippets = NULL;
      to->recording_snippet_count = 0;
      to->recording_snippet_capacity = 0;
      model_rebind_starting_strings(&to->starting_config);
      for (int snippet = 0; snippet < to->snippet_count; ++snippet)
        to->snippets[snippet].id = next_snippet_id++;
      from->snippets = NULL;
      from->snippet_count = 0;
      from->snippet_capacity = 0;
      if (to->starting_config.enabled)
        model_apply_starting_config(timeline, (int)(to - timeline->player_tracks));
    }
  }

  free(timeline->events);
  timeline->events = merged_events;
  timeline->event_count = merged_event_count;
  timeline->event_capacity = merged_event_count;
  merged_events = NULL;
  timeline_events_sort(timeline);
  timeline->next_snippet_id = next_snippet_id;
  timeline->active_group_index = original_group_count;
  timeline->selected_player_track_index = model_group_track_index(timeline, original_group_count, 0);
  model_recalc_physics(timeline, 0);
  ui_mark_unsaved(ui);
  log_info(LOG_SOURCE, "Imported %d group(s), %d track(s), and %d snippet(s) from '%s' onto the current level",
           document.group_count, document.track_count, imported_snippet_count, path);
  ok = true;

done:
  if (!ok) {
    while (timeline->group_count > original_group_count)
      model_remove_group(timeline, timeline->group_count - 1);
    timeline->active_group_index = original_active_group;
    timeline->selected_player_track_index = original_selected_track;
    timeline->next_snippet_id = original_next_snippet_id;
    model_recalc_physics(timeline, 0);
    log_error(LOG_SOURCE, "Could not import all project groups from '%s'.", path);
  }
  free(merged_events);
  project_document_free(&document);
  return ok;
}
