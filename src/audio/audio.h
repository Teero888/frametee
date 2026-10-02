#ifndef FRAMETEE_AUDIO_H
#define FRAMETEE_AUDIO_H

// Sound, as a function of the timeline. A game says what each step of a world
// sounded like (ft_audio_step); the engine keeps that per tick for each group
// and plays it back for any stretch of game time: forward, backward, at any
// speed, a short grain at a time while scrubbing, or offline into a video.
//
// Playing is done on the frame thread. Each frame renders the next few dozen
// milliseconds into a ring the device drains, and renders again whatever has
// not been heard yet when something changed (a seek, an edit, a new speed), so
// the device thread never touches game data.

#include <frametee/game_abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE 48000

// Opens the default output device; false leaves the editor silent.
bool audio_init(void);
void audio_shutdown(void);
// Whether sound is heard or recorded: an open device, or a video being
// rendered with sound.
bool audio_enabled(void);
void audio_set_recording(bool recording);
// Whether the steps being taken now are heard (the followed world and its
// lookahead): only those are captured. Returns the previous setting.
bool audio_set_heard(bool heard);
bool audio_heard(void);
void audio_set_volume(float volume);

ft_audio_sample audio_sample_load(const char *path);
ft_audio_sample audio_sample_decode(const void *data, size_t size);
ft_audio_sample audio_sample_create(const float *frames, uint32_t frame_count, uint32_t channels, uint32_t sample_rate);
void audio_sample_destroy(ft_audio_sample sample);

// What a group's steps sounded like, by local tick: tick T holds the step that
// reached it, heard over the game time from T - 1 to T.
typedef struct audio_track audio_track_t;
void audio_track_capture(audio_track_t **track, int tick, const ft_audio_step *step, double ticks_per_second);
// Forgets the ticks from `first` on, which the next steps will capture again.
void audio_track_invalidate(audio_track_t *track, int first);
void audio_track_free(audio_track_t *track);
// The first tick in [first, last] not captured yet, or -1.
int audio_track_first_missing(const audio_track_t *track, int first, int last);

// Sounds of authored timeline events, by world and local tick (one-shots from
// the start of the step reaching it). Replaces the previous set.
typedef struct audio_event_sound {
  int world_index;
  int tick;
  ft_audio_sound sound;
} audio_event_sound_t;
void audio_set_event_sounds(const audio_event_sound_t *sounds, int count);

// A group to hear. Several are heard together, each at its volume.
typedef struct audio_listener {
  audio_track_t **track;
  int start_offset; // the group's tick 0 on the global timeline
  int world_index;
  float volume;
  double ticks_per_second;
  // While recording, the last local tick whose sound is decided: past it the
  // inputs are still to come, so nothing there is simulated or heard yet.
  bool limited;
  int last_tick;
  // Makes the track of world `world_index` hold local ticks [first, last],
  // simulating what it lacks.
  void (*cover)(void *user, int world_index, int first, int last);
  // The gains of a positioned sound; false when the game does not say.
  bool (*spatialize)(void *user, int world_index, const ft_audio_sound *sound, float gain[2]);
  void *user;
} audio_listener_t;

// Where the playhead is. Positions are global ticks, or seconds of the camera's
// time when game_tick is set, which maps them to ticks (the camera's remap).
typedef struct audio_clock {
  bool playing;
  double position; // now, as drawn
  double rate;     // position units per second of wall time, while playing
  double (*game_tick)(void *user, double position);
  void *user;
} audio_clock_t;

// Once a frame, after the playhead moved: the groups heard, the followed one
// first.
void audio_update(const audio_listener_t *listeners, int listener_count, const audio_clock_t *clock);
// Silences whatever is queued: the project or the game is going away.
void audio_stop(void);

// Offline, for video: `frames` stereo frames (interleaved floats) of the
// clock's positions from `from` to `to`, at AUDIO_RATE.
void audio_render(const audio_listener_t *listeners, int listener_count, const audio_clock_t *clock, double from,
                  double to, float *out, uint32_t frames);

#endif
