#ifndef UI_TIMELINE_RECORDINGS_H
#define UI_TIMELINE_RECORDINGS_H

#include "timeline_types.h"

// The recordings a project holds (imported demos) and the playback snippets that replay them. The
// game opens each recording on a worker thread; until it is ready, its playback snippets replay
// nothing and their players stand wherever the world leaves them.

// Takes a copy of a recording file's bytes and starts opening it. `id` is the project's id for it,
// or -1 for a new one. Returns NULL when out of memory.
timeline_recording_t *recordings_add(timeline_state_t *ts, int id, const char *name, const void *data, size_t size);
timeline_recording_t *recordings_find(const timeline_state_t *ts, int id);
// The opened recording, or NULL while it is loading or when it failed.
const ft_recording *recordings_handle(const timeline_state_t *ts, int id);
recording_status_t recordings_status(timeline_recording_t *recording, float *out_progress, char *out_error, size_t error_size);

// Once per frame on the main thread: acts on recordings that finished opening, so the worlds that
// replay them are simulated again.
void recordings_update(timeline_state_t *ts);
// Blocks until `recording` finished opening, one way or the other. True when it is ready.
bool recordings_wait(timeline_state_t *ts, timeline_recording_t *recording);
// Cancels loaders, waits for them and destroys every recording. Must run while the game that opened
// them is still active.
void recordings_clear(timeline_state_t *ts);
// Drops one recording, which no snippet may refer to (an import that was cancelled).
void recordings_remove(timeline_state_t *ts, int id);
// Takes a recording out of the timeline without closing it, so it survives the timeline being torn
// down and rebuilt (a new project on the recording's level), and puts it back. The game that opened
// it must stay active in between.
timeline_recording_t *recordings_detach(timeline_state_t *ts, int id);
void recordings_attach(timeline_state_t *ts, timeline_recording_t *recording);

// What a player replays at `tick` on `track`: the active playback snippet covering it, as the
// recording and recording tick the world shows after stepping from `tick`. False when the player
// follows its input, which includes every tick an active input snippet (or, while recording, the
// recording from its start on) covers: input takes over from a demo where both overlap.
bool recordings_playback_at_tick(const timeline_state_t *ts, const player_track_t *track, int tick, ft_player_playback *out);
// The recording tick a playback snippet shows at timeline tick `tick`, or false when the recording
// is not open yet.
bool recordings_snippet_tick(const timeline_state_t *ts, const input_snippet_t *snippet, int tick, int *out_recording_tick);

// The input to show for a track at `tick`: inside a demo snippet, the input its player most likely
// held, read from the recording; elsewhere the timeline's own. For display only (cursor, HUD):
// replayed players are never simulated from it.
input_record_t recordings_display_input(const timeline_state_t *ts, int track_index, int tick);

#endif // UI_TIMELINE_RECORDINGS_H
