#include "timeline_recordings.h"

#include "timeline_model.h"

#include <engine/game_host.h>
#include <engine/input_record.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>

static const char *LOG_SOURCE = "Recordings";

static game_host_t *recordings_host(const timeline_state_t *ts) {
  return ts && ts->ui && ts->ui->gfx_handler ? &ts->ui->gfx_handler->game_host : NULL;
}

typedef struct {
  timeline_recording_t *recording;
  game_host_t *host;
} loader_args_t;

static bool loader_progress(void *user, float fraction) {
  timeline_recording_t *recording = user;
  pthread_mutex_lock(&recording->lock);
  recording->progress = fraction;
  const bool keep_going = !recording->cancel;
  pthread_mutex_unlock(&recording->lock);
  return keep_going;
}

static void *loader_main(void *arg) {
  loader_args_t args = *(loader_args_t *)arg;
  free(arg);
  timeline_recording_t *recording = args.recording;
  char error[256] = "";
  ft_recording *handle = gh_recording_open(args.host, recording->data, recording->size, recording->name, loader_progress, recording,
                                           error, sizeof(error));
  pthread_mutex_lock(&recording->lock);
  recording->handle = handle;
  recording->status = handle ? RECORDING_READY : RECORDING_FAILED;
  recording->progress = handle ? 1.f : recording->progress;
  snprintf(recording->error, sizeof(recording->error), "%s", handle ? "" : (error[0] ? error : "could not be opened"));
  pthread_mutex_unlock(&recording->lock);
  return NULL;
}

timeline_recording_t *recordings_add(timeline_state_t *ts, int id, const char *name, const void *data, size_t size) {
  if (!ts || !data || !size) return NULL;
  timeline_recording_t **grown = realloc(ts->recordings, sizeof(*grown) * (size_t)(ts->recording_count + 1));
  if (!grown) return NULL;
  ts->recordings = grown;
  timeline_recording_t *recording = calloc(1, sizeof(*recording));
  if (!recording) return NULL;
  recording->data = malloc(size);
  if (!recording->data) {
    free(recording);
    return NULL;
  }
  memcpy(recording->data, data, size);
  recording->size = size;
  snprintf(recording->name, sizeof(recording->name), "%s", name ? name : "recording");
  recording->id = id >= 0 ? id : ts->next_recording_id;
  if (recording->id >= ts->next_recording_id) ts->next_recording_id = recording->id + 1;
  pthread_mutex_init(&recording->lock, NULL);
  recording->status = RECORDING_LOADING;
  ts->recordings[ts->recording_count++] = recording;

  loader_args_t *args = malloc(sizeof(*args));
  game_host_t *host = recordings_host(ts);
  if (args && host) {
    args->recording = recording;
    args->host = host;
    recording->thread_running = pthread_create(&recording->thread, NULL, loader_main, args) == 0;
    if (!recording->thread_running) free(args);
  } else {
    free(args);
  }
  if (!recording->thread_running) {
    recording->status = RECORDING_FAILED;
    snprintf(recording->error, sizeof(recording->error), "could not start loading");
  }
  return recording;
}

timeline_recording_t *recordings_find(const timeline_state_t *ts, int id) {
  if (!ts) return NULL;
  for (int i = 0; i < ts->recording_count; ++i)
    if (ts->recordings[i]->id == id) return ts->recordings[i];
  return NULL;
}

// Only recordings the main thread has announced count as open: until then the timeline has not
// invalidated the worlds that replay them, and half the frame would see the recording and half not.
const ft_recording *recordings_handle(const timeline_state_t *ts, int id) {
  const timeline_recording_t *recording = recordings_find(ts, id);
  return recording && recording->announced ? recording->handle : NULL;
}

recording_status_t recordings_status(timeline_recording_t *recording, float *out_progress, char *out_error, size_t error_size) {
  if (!recording) return RECORDING_FAILED;
  pthread_mutex_lock(&recording->lock);
  const recording_status_t status = recording->status;
  if (out_progress) *out_progress = recording->progress;
  if (out_error && error_size) snprintf(out_error, error_size, "%s", recording->error);
  pthread_mutex_unlock(&recording->lock);
  return status;
}

static void join_loader(timeline_recording_t *recording) {
  if (!recording->thread_running) return;
  pthread_join(recording->thread, NULL);
  recording->thread_running = false;
}

static void announce(timeline_state_t *ts, timeline_recording_t *recording) {
  join_loader(recording);
  recording->announced = true;
  game_host_t *host = recordings_host(ts);
  if (!recording->handle || !gh_recording_info(host, recording->handle, &recording->info)) {
    log_warn(LOG_SOURCE, "'%s' could not be opened: %s", recording->name, recording->error);
    return;
  }
  recording->first_tick = recording->info.first_tick;
  recording->last_tick = recording->info.last_tick;
  log_info(LOG_SOURCE, "'%s' is ready (ticks %d..%d, %u players)", recording->name, recording->first_tick, recording->last_tick,
           recording->info.player_count);

  // Every world replaying it was simulated without it until now.
  for (int t = 0; t < ts->player_track_count; ++t) {
    const player_track_t *track = &ts->player_tracks[t];
    int earliest = -1;
    for (int s = 0; s < track->snippet_count; ++s) {
      const input_snippet_t *snippet = &track->snippets[s];
      if (snippet_is_playback(snippet) && snippet->recording_id == recording->id && (earliest < 0 || snippet->start_tick < earliest))
        earliest = snippet->start_tick;
    }
    if (earliest >= 0) model_recalc_group_physics(ts, track->group_index, earliest);
  }
}

void recordings_update(timeline_state_t *ts) {
  if (!ts) return;
  for (int i = 0; i < ts->recording_count; ++i) {
    timeline_recording_t *recording = ts->recordings[i];
    if (recording->announced) continue;
    if (recordings_status(recording, NULL, NULL, 0) != RECORDING_LOADING) announce(ts, recording);
  }
}

bool recordings_wait(timeline_state_t *ts, timeline_recording_t *recording) {
  if (!recording) return false;
  if (!recording->announced) announce(ts, recording);
  return recording->handle != NULL;
}

static void destroy_recording(timeline_state_t *ts, timeline_recording_t *recording) {
  pthread_mutex_lock(&recording->lock);
  recording->cancel = true;
  pthread_mutex_unlock(&recording->lock);
  join_loader(recording);
  if (recording->handle) gh_recording_destroy(recordings_host(ts), recording->handle);
  pthread_mutex_destroy(&recording->lock);
  free(recording->data);
  free(recording);
}

void recordings_clear(timeline_state_t *ts) {
  if (!ts) return;
  for (int i = 0; i < ts->recording_count; ++i) destroy_recording(ts, ts->recordings[i]);
  free(ts->recordings);
  ts->recordings = NULL;
  ts->recording_count = 0;
}

void recordings_remove(timeline_state_t *ts, int id) {
  timeline_recording_t *recording = recordings_detach(ts, id);
  if (recording) destroy_recording(ts, recording);
}

timeline_recording_t *recordings_detach(timeline_state_t *ts, int id) {
  if (!ts) return NULL;
  for (int i = 0; i < ts->recording_count; ++i) {
    if (ts->recordings[i]->id != id) continue;
    timeline_recording_t *recording = ts->recordings[i];
    memmove(&ts->recordings[i], &ts->recordings[i + 1], sizeof(*ts->recordings) * (size_t)(ts->recording_count - i - 1));
    --ts->recording_count;
    return recording;
  }
  return NULL;
}

void recordings_attach(timeline_state_t *ts, timeline_recording_t *recording) {
  if (!ts || !recording) return;
  timeline_recording_t **grown = realloc(ts->recordings, sizeof(*grown) * (size_t)(ts->recording_count + 1));
  if (!grown) {
    destroy_recording(ts, recording);
    return;
  }
  ts->recordings = grown;
  if (recordings_find(ts, recording->id)) recording->id = ts->next_recording_id;
  if (recording->id >= ts->next_recording_id) ts->next_recording_id = recording->id + 1;
  ts->recordings[ts->recording_count++] = recording;
}

bool recordings_snippet_tick(const timeline_state_t *ts, const input_snippet_t *snippet, int tick, int *out_recording_tick) {
  if (!snippet_is_playback(snippet)) return false;
  const timeline_recording_t *recording = recordings_find(ts, snippet->recording_id);
  if (!recording || !recording->announced || !recording->handle) return false;
  if (out_recording_tick) *out_recording_tick = recording->first_tick + snippet->source_offset + (tick - snippet->start_tick);
  return true;
}

// Input takes over from a demo wherever both cover a tick, which is how recording over a demo
// continues from it. While recording, the take-over lasts from where the recording started on.
static bool input_covers(const timeline_state_t *ts, const player_track_t *track, int tick) {
  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (snippet->is_active && !snippet_is_playback(snippet) && tick >= snippet->start_tick && tick < snippet->end_tick) return true;
  }
  if (ts->recording)
    for (int i = 0; i < track->recording_snippet_count; ++i)
      if (track->recording_snippets[i].is_active && tick >= track->recording_snippets[i].start_tick) return true;
  return false;
}

bool recordings_playback_at_tick(const timeline_state_t *ts, const player_track_t *track, int tick, ft_player_playback *out) {
  if (!ts || !track) return false;
  if (input_covers(ts, track, tick)) return false;
  for (int i = 0; i < track->snippet_count; ++i) {
    const input_snippet_t *snippet = &track->snippets[i];
    if (!snippet->is_active || !snippet_is_playback(snippet)) continue;
    if (tick < snippet->start_tick || tick >= snippet->end_tick) continue;
    int recording_tick;
    if (!recordings_snippet_tick(ts, snippet, tick, &recording_tick)) return false;
    if (out) {
      out->recording = recordings_handle(ts, snippet->recording_id);
      out->player = snippet->recording_player;
      out->tick = recording_tick;
    }
    return true;
  }
  return false;
}

input_record_t recordings_display_input(const timeline_state_t *ts, int track_index, int tick) {
  input_record_t record = model_get_input_at_tick(ts, track_index, tick);
  ft_player_playback playback;
  if (track_index < 0 || track_index >= ts->player_track_count ||
      !recordings_playback_at_tick(ts, &ts->player_tracks[track_index], tick, &playback) || !playback.recording)
    return record;
  game_host_t *host = recordings_host(ts);
  input_record_t recorded;
  engine_input_default(host, &recorded);
  // A player absent from the demo at that tick holds nothing.
  gh_recording_input(host, playback.recording, playback.player, playback.tick, recorded.bytes);
  return recorded;
}
