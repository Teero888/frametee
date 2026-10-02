#include "audio_timeline.h"

#include <engine/game_host.h>
#include <renderer/graphics_backend.h>
#include <user_interface/camera/camera_timeline.h>
#include <limits.h>
#include <stdlib.h>
#include <user_interface/camera/camera_window.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline_events.h>
#include <user_interface/user_interface.h>

static void cover(void *user, int first, int last) {
  timeline_state_t *ts = &((gfx_handler_t *)user)->user_interface.timeline;
  model_group_audio_cover(ts, ts->active_group_index, first, last);
}

static bool spatialize(void *user, int world_index, const ft_audio_sound *sound, float gain[2]) {
  return gh_audio_spatialize(&((gfx_handler_t *)user)->game_host, world_index, sound, gain);
}

static double camera_tick(void *user, double seconds) {
  gfx_handler_t *handler = user;
  return camera_game_tick(&handler->user_interface.camera_timeline, seconds, game_ticks_per_second(&handler->game_host));
}

// The sounds of the authored events, asked of the game again whenever the
// events (or the game) change.
static void sync_event_sounds(gfx_handler_t *handler) {
  static uint64_t revision;
  static const void *game;
  static bool synced;
  timeline_state_t *ts = &handler->user_interface.timeline;
  if (synced && revision == ts->event_revision && game == handler->game_host.instance) return;
  synced = true;
  revision = ts->event_revision;
  game = handler->game_host.instance;
  audio_event_sound_t *sounds = ts->event_count > 0 ? malloc((size_t)ts->event_count * sizeof(*sounds)) : NULL;
  int count = 0;
  for (int i = 0; sounds && i < ts->event_count; ++i) {
    ft_timeline_event event;
    timeline_event_to_abi(&ts->events[i], &event);
    ft_audio_sound sound;
    if (!gh_event_audio(&handler->game_host, &event, &sound)) continue;
    sounds[count++] = (audio_event_sound_t){.world_index = ts->events[i].group_index, .tick = ts->events[i].tick, .sound = sound};
  }
  audio_set_event_sounds(sounds, count);
  free(sounds);
}

bool audio_timeline_listener(gfx_handler_t *handler, audio_listener_t *out) {
  timeline_state_t *ts = &handler->user_interface.timeline;
  if (!handler->level || !game_has_audio(&handler->game_host) || ts->active_group_index < 0 ||
      ts->active_group_index >= ts->group_count)
    return false;
  sync_event_sounds(handler);
  timeline_group_t *group = ts->groups[ts->active_group_index];
  *out = (audio_listener_t){
      .track = &group->audio,
      .start_offset = group->start_offset,
      .world_index = ts->active_group_index,
      .ticks_per_second = game_ticks_per_second(&handler->game_host),
      // Recording: the ticks past the playhead wait for their inputs.
      .limited = ts->recording,
      .last_tick = model_group_playhead_tick(ts, ts->active_group_index),
      .cover = cover,
      .spatialize = spatialize,
      .user = handler,
  };
  return true;
}

audio_clock_t audio_timeline_camera_clock(gfx_handler_t *handler) {
  return (audio_clock_t){.rate = 1.0, .game_tick = camera_tick, .user = handler};
}

void audio_timeline_update(gfx_handler_t *handler) {
  ui_handler_t *ui = &handler->user_interface;
  timeline_state_t *ts = &ui->timeline;
  audio_set_volume(ui->audio_volume);
  audio_listener_t listener;
  if (!audio_timeline_listener(handler, &listener)) {
    audio_update(NULL, NULL);
    return;
  }

  audio_clock_t clock = {0};
  if (camera_editor_owns_clock(ui) && !ts->recording) {
    clock = audio_timeline_camera_clock(handler);
    clock.position = ui->camera_editor.playhead;
    clock.playing = ui->camera_editor.playing;
  } else {
    // As drawn: between the tick before the playhead and the playhead, by the
    // fraction of a tick played since it moved.
    clock.playing = (ts->is_playing || ts->is_reversing) && ts->playback_speed > 0;
    clock.position = ts->current_tick;
    if (clock.playing) {
      const double speed = ts->playback_speed * (ts->is_reversing ? 2.0 : 1.0);
      double intra = (igGetTime() - ts->last_update_time) * speed;
      intra = intra < 0.0 ? 0.0 : intra > 1.0 ? 1.0 : intra;
      if (ts->is_reversing) intra = 1.0 - intra;
      clock.position = ts->current_tick - 1 + intra;
      clock.rate = ts->is_reversing ? -speed : speed;
    }
  }
  audio_update(&listener, &clock);
}
