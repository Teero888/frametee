// Exporting a run as a replay the game plays and validates: the track's
// inputs through tmuf_replay_write, which simulates the run to record what the
// game checks (the car's samples, race time, respawns, stunt score). Or as
// a TMInterface input script (.txt), which TMInterface plays in the game.

#include "tmuf_internal.h"

#include <stdio.h>
#include <stdlib.h>

enum { EXPORT_REPLAY = TM_EXPORT_REPLAY, EXPORT_TMINTERFACE = TM_EXPORT_TMINTERFACE, EXPORT_COUNT };

static const ft_exporter_desc exporters[EXPORT_COUNT] = {
    {"replay", "TrackMania replay", "Replay.Gbx", "TrackMania replay"},
    {"tminterface", "TMInterface inputs", "txt", "TMInterface input script"},
};

uint32_t tm_exporter_count(ft_game *game) { return EXPORT_COUNT; }

const ft_exporter_desc *tm_exporter_desc(ft_game *game, uint32_t index) {
  return index < EXPORT_COUNT ? &exporters[index] : NULL;
}

// The run as TMInterface's input script: a line per change, at the race time
// the game reads it ("0.00 press up", "1.23 steer -12345", "4.56 press
// enter"). The input of tick n is read at race time (n + 1) * 10 - 2600 ms
// (as tmuf_replay_inputs places a replay's events); before the race starts
// the car is held, so the script starts from the input at 0.00. Steering is
// TMInterface's analog axis, negative to the left, like the game's keys at
// +-65536. A key event that leaves the input as it was (a second steering
// key, which cancels a Stunts master jump) is the steering written again.
static bool export_tminterface(ft_game *game, const ft_export_request *request) {
  const ft_engine_api *api = game->engine;
  const int32_t track = request->players && request->player_count ? request->players[0] : 0;
  FILE *f = fopen(request->path, "w");
  if (!f) {
    tm_log(game, FT_LOG_ERROR, "Cannot write %s", request->path);
    return false;
  }
  const int32_t first = (int32_t)(TMUF_RACE_START_MS / TMUF_TICK_MS) - 1; // read at 0.00
  tm_input held = {0};
  bool ok = true, respawn_held = false;
  for (int32_t tick = first; tick <= request->end_tick && ok; tick++) {
    tm_input in = {0};
    if (!api->get_player_input(track, tick, &in)) in = (tm_input){0};
    const int32_t ms = (tick + 1) * (int32_t)TMUF_TICK_MS - (int32_t)TMUF_RACE_START_MS;
    char at[32];
    snprintf(at, sizeof at, "%d.%02d", ms / 1000, ms % 1000 / 10);
    if (respawn_held) ok &= fprintf(f, "%s rel enter\n", at) > 0, respawn_held = false;
    if (in.accelerate != held.accelerate) ok &= fprintf(f, "%s %s up\n", at, in.accelerate ? "press" : "rel") > 0;
    if (in.brake != held.brake) ok &= fprintf(f, "%s %s down\n", at, in.brake ? "press" : "rel") > 0;
    if (in.steer != held.steer || (in.input_event && in.accelerate == held.accelerate && in.brake == held.brake))
      ok &= fprintf(f, "%s steer %d\n", at, (int)in.steer) > 0;
    if (in.respawn) ok &= fprintf(f, "%s press enter\n", at) > 0, respawn_held = true;
    held = in;
    if (request->progress && (tick & 1023) == 0)
      request->progress(request->progress_user, (float)(tick - first) / (float)(request->end_tick - first + 1), "Writing inputs");
  }
  ok &= fclose(f) == 0;
  if (!ok) tm_log(game, FT_LOG_ERROR, "Cannot write %s", request->path);
  else tm_log(game, FT_LOG_INFO, "Wrote %s", request->path);
  if (request->progress) request->progress(request->progress_user, 1.f, "Done");
  return ok;
}

bool tm_export_run(ft_game *game, uint32_t index, const ft_export_request *request) {
  const ft_engine_api *api = game->engine;
  if (index >= EXPORT_COUNT || !request || !request->path || !game->level || !api->get_player_input) return false;
  if (request->end_tick < 0) return false;
  if (index == EXPORT_TMINTERFACE) return export_tminterface(game, request);
  const int32_t track = request->players && request->player_count ? request->players[0] : 0;
  const int32_t end = request->end_tick;
  if (end < 0) return false;
  const uint32_t count = (uint32_t)end + 1u;
  tmuf_input *inputs = calloc(count, sizeof *inputs);
  if (!inputs) return false;
  for (uint32_t i = 0; i < count; i++) {
    tm_input in = {0};
    if (api->get_player_input(track, (int32_t)i, &in))
      inputs[i] = (tmuf_input){in.accelerate, in.brake, in.respawn, in.input_event, in.steer};
    if (request->progress && (i & 1023u) == 0)
      request->progress(request->progress_user, 0.5f * (float)i / (float)count, "Collecting inputs");
  }
  char err[512];
  size_t size = 0;
  // the player's name from the Player Info tab, as the ghost's nickname
  tmuf_replay_write_options options = {0};
  ft_player_setup setup = {.struct_size = sizeof setup};
  const char *name = api->get_player_setup && api->get_player_setup(track, &setup) ? tm_profile_name(&setup, 1, 0) : "";
  if (name[0]) options.nickname = name;
  void *gbx = tmuf_replay_write(game->level->track, inputs, count, &options, &size, err, sizeof err);
  free(inputs);
  if (!gbx) {
    tm_log(game, FT_LOG_ERROR, "Cannot write the replay: %s", err);
    return false;
  }
  FILE *f = fopen(request->path, "wb");
  const bool ok = f && fwrite(gbx, 1, size, f) == size;
  if (f) fclose(f);
  tmuf_free(gbx);
  if (!ok) tm_log(game, FT_LOG_ERROR, "Cannot write %s", request->path);
  else tm_log(game, FT_LOG_INFO, "Wrote %s", request->path);
  if (request->progress) request->progress(request->progress_user, 1.f, "Done");
  return ok;
}

// The run of an editor track as a replay or a TMInterface script
// (TM_EXPORT_*): its inputs from time 0 up to the finish (found by driving
// them), else up to the playhead.
bool tm_export_track(ft_game *game, uint32_t format, const char *path, int32_t track) {
  const ft_engine_api *api = game->engine;
  if (!game->level || !api->get_player_input) return false;
  ft_engine_state state = {.struct_size = sizeof state};
  if (api->get_state) api->get_state(&state);
  const int32_t limit = 360000; // an hour of racing
  tmuf_world w = tmuf_world_empty();
  if (!tmuf_world_init(&w, game->level->track)) return false;
  int32_t end = state.current_tick > 0 ? state.current_tick : 0;
  for (int32_t tick = 0; tick < limit; tick++) {
    tm_input in = {0};
    if (!api->get_player_input(track, tick, &in)) break;
    w.input = (tmuf_input){in.accelerate, in.brake, in.respawn, in.input_event, in.steer};
    tmuf_world_tick(&w);
    if (w.sim.race.completed) {
      end = tick + 1;
      break;
    }
  }
  tmuf_world_free(&w);
  const int32_t players[1] = {track};
  const ft_export_request request = {.struct_size = sizeof request, .path = path, .start_tick = 0, .end_tick = end,
                                     .players = players, .player_count = 1};
  return tm_export_run(game, format, &request);
}
