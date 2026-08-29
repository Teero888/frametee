#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cimgui.h"
#include "plugin_api.h"

typedef struct {
  const tas_api_t *api;
  const tas_context_t *context;
  bool show_window;
  bool auto_create_track;
  bool advance_seed;
  int snippet_length;
  int start_tick;
  uint32_t seed;
  char status_message[128];
} random_input_state_t;

static uint32_t rng_next(uint32_t *state) {
  uint32_t x = *state;
  if (x == 0)
    x = 0x6d2b79f5u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void set_status(random_input_state_t *state, const char *message) {
  snprintf(state->status_message, sizeof(state->status_message), "%s", message ? message : "");
}

static int random_int_inclusive(uint32_t *rng_state, int minimum, int maximum) {
  if (maximum <= minimum) return minimum;
  const uint64_t range = (uint64_t)((int64_t)maximum - (int64_t)minimum) + 1u;
  return (int)((int64_t)minimum + (int64_t)((uint64_t)rng_next(rng_state) % range));
}

static float random_float(uint32_t *rng_state, float minimum, float maximum) {
  if (maximum <= minimum) return minimum;
  const float unit = (float)rng_next(rng_state) / (float)UINT32_MAX;
  return minimum + (maximum - minimum) * unit;
}

static void randomize_record(random_input_state_t *state, uint32_t *rng_state, void *record) {
  const uint32_t field_count = state->api->input_field_count();
  for (uint32_t index = 0; index < field_count; ++index) {
    const ft_input_field *field = state->api->input_field(index);
    if (!field || (field->flags & (FT_INPUT_FLAG_INTERNAL | FT_INPUT_FLAG_EDITOR_HIDDEN))) continue;

    switch (field->kind) {
    case FT_INPUT_BOOL:
      state->api->input_set(record, (int)index, (long long)(rng_next(rng_state) >> 31));
      break;
    case FT_INPUT_INT:
    case FT_INPUT_ENUM:
      state->api->input_set(record, (int)index, random_int_inclusive(rng_state, field->min_value, field->max_value));
      break;
    case FT_INPUT_FLOAT:
      state->api->input_set_float(record, (int)index, random_float(rng_state, field->min_float, field->max_float));
      break;
    case FT_INPUT_VEC2: {
      const ft_vec2 value = {.x = random_float(rng_state, field->min_float, field->max_float),
                             .y = random_float(rng_state, field->min_float, field->max_float)};
      state->api->input_set_vec2(record, (int)index, value);
      break;
    }
    }
  }
}

static void fill_tracks_with_random_inputs(random_input_state_t *state) {
  int track_count = state->api->get_track_count();
  if (track_count <= 0) {
    if (state->auto_create_track) {
      int created_index = -1;
      undo_command_t *create_cmd = state->api->do_create_track(NULL, &created_index);
      if (create_cmd) {
        state->api->register_undo_command(create_cmd);
        track_count = state->api->get_track_count();
        state->api->log(FT_LOG_INFO, "Random Input Filler", "Created a new track because none existed.");
      }
    }
    if (track_count <= 0) {
      set_status(state, "No tracks available to fill.");
      return;
    }
  }

  uint32_t rng_state = state->seed ? state->seed : 0x6d2b79f5u;
  int created_snippets = 0;
  int updated_snippets = 0;
  int failed_tracks = 0;
  int total_ticks_written = 0;

  for (int track_index = 0; track_index < track_count; ++track_index) {
    int snippet_id = -1;
    int tick_offset = 0;
    int fill_count = state->snippet_length;
    undo_command_t *create_cmd =
        state->api->do_create_snippet(track_index, state->start_tick, state->snippet_length, &snippet_id);

    if (create_cmd) {
      state->api->register_undo_command(create_cmd);
      created_snippets++;
      tick_offset = 0;
      fill_count = state->snippet_length;
    } else {
      int available = 0;
      if (!state->api->find_snippet_at(track_index, state->start_tick, &snippet_id, &tick_offset, &available)) {
        state->api->log(FT_LOG_WARN, "Random Input Filler",
                        "Could not create snippet due to overlap and no suitable snippet exists.");
        failed_tracks++;
        continue;
      }
      if (available <= 0) {
        state->api->log(FT_LOG_WARN, "Random Input Filler", "Target snippet does not extend past the requested start tick.");
        failed_tracks++;
        continue;
      }
      if (fill_count > available) {
        fill_count = available;
        state->api->log(FT_LOG_WARN, "Random Input Filler",
                        "Snippet shorter than requested length; filling available portion only.");
      }
      updated_snippets++;
    }

    if (fill_count <= 0 || snippet_id < 0) {
      failed_tracks++;
      continue;
    }

    const size_t record_size = state->api->input_record_size();
    unsigned char *buffer = (unsigned char *)calloc((size_t)fill_count, record_size);
    if (!buffer) {
      state->api->log(FT_LOG_ERROR, "Random Input Filler", "Failed to allocate input buffer.");
      failed_tracks++;
      continue;
    }

    for (int tick = 0; tick < fill_count; ++tick) {
      void *input = buffer + (size_t)tick * record_size;
      state->api->input_default(input);
      randomize_record(state, &rng_state, input);
    }

    undo_command_t *set_cmd = state->api->do_set_inputs(snippet_id, tick_offset, fill_count, buffer, record_size);
    if (set_cmd) {
      state->api->register_undo_command(set_cmd);
      total_ticks_written += fill_count;
    } else {
      state->api->log(FT_LOG_WARN, "Random Input Filler", "Failed to apply random inputs to a snippet.");
      failed_tracks++;
    }

    free(buffer);
  }

  if (state->advance_seed)
    state->seed = rng_state;

  char summary[128];
  snprintf(summary, sizeof(summary),
           "Tracks: %d | New snippets: %d | Updated snippets: %d | Failures: %d | Ticks written: %d",
           track_count, created_snippets, updated_snippets, failed_tracks, total_ticks_written);
  set_status(state, summary);
}

FT_API plugin_info_t get_plugin_info(void) {
  return (plugin_info_t){.name = "Random Input Filler",
                         .author = "Tater",
                         .version = "1.0.0",
                         .description = "Generates random inputs for every track"};
}

// Now schema-driven, so it works under any game and stays global. Export
// plugin_game_id() here to bind it to one instead.

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  random_input_state_t *state = (random_input_state_t *)calloc(1, sizeof(random_input_state_t));
  if (!state)
    return NULL;

  state->api = api;
  state->context = context;
  state->show_window = true;
  state->auto_create_track = true;
  state->advance_seed = true;
  state->snippet_length = 100;
  state->start_tick = 0;
  state->seed = (uint32_t)time(NULL);
  set_status(state, "Ready.");
  return state;
}

FT_API void plugin_update(void *plugin_data) {
  random_input_state_t *state = (random_input_state_t *)plugin_data;
  if (!state)
    return;

  igSetCurrentContext(state->context->imgui_context);

  if (igBeginMainMenuBar()) {
    if (igBeginMenu("Random Input Filler", true)) {
      igMenuItem_BoolPtr("Show Window", NULL, &state->show_window, true);
      igEndMenu();
    }
    igEndMainMenuBar();
  }

  if (!state->show_window)
    return;

  if (igBegin("Random Input Filler", &state->show_window, ImGuiWindowFlags_None)) {
    igText("Tracks detected: %d", state->api->get_track_count());
    igSeparator();

    igInputInt("Snippet Length", &state->snippet_length, 1, 10, ImGuiInputTextFlags_None);
    if (state->snippet_length < 1)
      state->snippet_length = 1;

    igInputInt("Start Tick", &state->start_tick, 1, 10, ImGuiInputTextFlags_None);
    if (state->start_tick < 0)
      state->start_tick = 0;

    int seed_input = (int)state->seed;
    if (igInputInt("Seed", &seed_input, 1, 100, ImGuiInputTextFlags_None) && seed_input >= 0)
      state->seed = (uint32_t)seed_input;

    igCheckbox("Advance seed after fill", &state->advance_seed);
    igCheckbox("Auto-create track when none exist", &state->auto_create_track);

    if (igButton("Randomize Seed", (ImVec2){0, 0}))
      state->seed = (uint32_t)time(NULL);

    igSeparator();

    if (igButton("Fill Tracks", (ImVec2){0, 0})) {
      fill_tracks_with_random_inputs(state);
    }

    igSpacing();
    igTextWrapped("%s", state->status_message);
  }
  igEnd();
}

FT_API void plugin_shutdown(void *plugin_data) {
  random_input_state_t *state = (random_input_state_t *)plugin_data;
  if (!state)
    return;

  state->api->log(FT_LOG_INFO, "Random Input Filler", "Plugin shutting down.");
  free(state);
}
