// Replays as recordings.
//
// A .Replay.Gbx holds the map it was driven on and the ghost's inputs from
// time 0 (tmuf_replay_inputs). The physics is deterministic, so playing a
// replay back is stepping the world with those inputs: a recorded player is
// exactly where the replay's run was, and input snippets placed after it
// carry on from there.

#include "tmuf_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ft_recording {
  char name[256];
  char player_name[64];
  tmuf_replay *replay;
  const tmuf_input *inputs;
  uint32_t count;
  // the replay's own bytes: loading them as a level keeps its car, seed and laps
  void *bytes;
  size_t size;
  const void *map; // inside replay
  size_t map_size;
  tm_profile profile; // the ghost's skin
};

ft_recording *tm_recording_open(ft_game *game, const void *data, size_t size, const char *name,
                                    bool (*progress)(void *user, float fraction), void *progress_user, char *error,
                                    size_t error_size) {
  (void)game, (void)progress, (void)progress_user;
  tmuf_replay *replay = tmuf_replay_load(data, size, error, error_size);
  if (!replay) return NULL;
  ft_recording *r = calloc(1, sizeof *r);
  if (!r || !(r->bytes = malloc(size ? size : 1))) {
    free(r);
    tmuf_replay_free(replay);
    snprintf(error, error_size, "out of memory");
    return NULL;
  }
  memcpy(r->bytes, data, size);
  r->size = size;
  r->replay = replay;
  r->count = tmuf_replay_inputs(replay, &r->inputs);
  r->map = tmuf_replay_map(replay, &r->map_size);
  if (!r->count || !r->map) {
    snprintf(error, error_size, "the replay holds no %s", r->map ? "inputs" : "map");
    tmuf_replay_free(replay);
    free(r->bytes);
    free(r);
    return NULL;
  }
  snprintf(r->name, sizeof r->name, "%s", name ? name : "replay");
  memcpy(r->profile.magic, TM_PROFILE_MAGIC, 4);
  snprintf(r->profile.skin, sizeof r->profile.skin, "%s", tmuf_replay_skin(replay));
  const uint32_t time = tmuf_replay_race_time(replay);
  if (time != UINT32_MAX)
    snprintf(r->player_name, sizeof r->player_name, "Ghost %u:%02u.%02u", time / 60000u, time / 1000u % 60u,
             time / 10u % 100u);
  else
    snprintf(r->player_name, sizeof r->player_name, "Ghost");
  return r;
}

void tm_recording_destroy(ft_game *game, ft_recording *r) {
  (void)game;
  if (!r) return;
  tmuf_replay_free(r->replay);
  free(r->bytes);
  free(r);
}

bool tm_recording_info(ft_game *game, const ft_recording *r, ft_recording_info *out) {
  (void)game;
  memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->name = r->name;
  out->first_tick = 0;
  out->last_tick = (int32_t)r->count; // the world after the last input
  out->player_count = 1;
  out->level_data = r->bytes;
  out->level_size = r->size;
  out->level_name = r->name;
  return true;
}

bool tm_recording_player(ft_game *game, const ft_recording *r, uint32_t index, ft_recording_player *out) {
  (void)game;
  if (index != 0) return false;
  memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->name = r->player_name;
  out->first_tick = 0;
  out->last_tick = (int32_t)r->count;
  out->suggested = true;
  out->profile = &r->profile;
  out->profile_size = sizeof r->profile;
  return true;
}

// The level is the replay's map when the challenge bytes are the same.
bool tm_recording_level_matches(ft_game *game, const ft_recording *r, const ft_level *level) {
  (void)game;
  return level && level->map && level->map_size == r->map_size && memcmp(level->map, r->map, r->map_size) == 0;
}

void tm_recording_tick_flags(ft_game *game, const ft_recording *r, int32_t player, int32_t first_tick,
                                 uint32_t count, uint8_t *out) {
  (void)game, (void)player;
  for (uint32_t i = 0; i < count; i++) {
    const int64_t tick = (int64_t)first_tick + i;
    out[i] = tick >= 0 && tick <= (int64_t)r->count ? FT_RECORDING_TICK_PRESENT : 0;
  }
}

static tmuf_input input_at(const ft_recording *r, int32_t tick) {
  return tick >= 0 && (uint32_t)tick < r->count ? r->inputs[tick] : (tmuf_input){0, 0, 0, 0, 0};
}

// The input that drives the step from `tick`.
bool tm_recording_input(ft_game *game, const ft_recording *r, int32_t player, int32_t tick, void *out_record) {
  (void)game;
  if (player != 0 || tick < 0 || (uint32_t)tick > r->count) return false;
  const tmuf_input in = input_at(r, tick);
  tm_input *o = out_record;
  *o = (tm_input){in.accelerate, in.brake, in.respawn, in.input_event, in.steer};
  return true;
}

// The replay's horn presses (not part of its inputs: the physics never
// reads them), heard as the timeline's events from the step that reads each
void tm_recording_events(ft_game *game, const ft_recording *r, const int32_t *world_players, uint32_t player_count,
                         void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  (void)game;
  if (player_count < 1 || world_players[0] < 0) return;
  const uint32_t count = tmuf_replay_horns(r->replay, NULL, 0);
  uint32_t *ticks = count ? malloc(count * sizeof *ticks) : NULL;
  if (!ticks) return;
  tmuf_replay_horns(r->replay, ticks, count);
  for (uint32_t i = 0; i < count; i++) {
    const ft_timeline_event event = {.struct_size = sizeof event,
                                     .world_index = -1,
                                     .tick = (int32_t)ticks[i] + 1,
                                     .player = world_players[0],
                                     .category = TM_EVENT_HORN,
                                     .text = "Horn",
                                     .color = {0.55f, 0.75f, 1.f, 1.f}};
    emit(user, &event);
  }
  free(ticks);
}

void tm_world_step_playback(ft_game *game, ft_world *world, const void *inputs, const ft_player_playback *playback,
                                uint32_t player_count) {
  // the engine names the recording's tick the step starts from (the world's
  // before it), and inputs[i] drives the step from tick i: the input of that
  // tick (one earlier ran the replay a tick late, apart from the game's run
  // wherever it steered)
  if (player_count > 0 && playback[0].recording) {
    world->w.input = input_at(playback[0].recording, playback[0].tick);
    tm_audio_frame before;
    const bool sound = tm_audio_prepare(game, world, &before);
    tmuf_world_tick(&world->w);
    tm_race_display_step(world); // (the speed and distance shown, as a driven run's)
    if (sound) tm_audio_step(game, world, &before);
    return;
  }
  tm_world_step(game, world, inputs, player_count);
}

