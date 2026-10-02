// The mixer, offline: sounds land when their steps say, voices keep their
// phase from tick to tick, a game's own stream plays back smoothly, and an
// edit forgets what it changed.
#include <assert.h>
#include <audio/audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double TPS = 50.0;

static audio_listener_t listener_for(audio_track_t **track, double tps) {
  return (audio_listener_t){.track = track, .ticks_per_second = tps, .volume = 1.f};
}

// Renders ticks `from`..`to` at 1x (or backwards) and returns the frames.
static float *render(audio_track_t **track, double tps, double from, double to, uint32_t *frames) {
  *frames = (uint32_t)llround(fabs(to - from) / tps * AUDIO_RATE);
  float *out = malloc((size_t)*frames * 2 * sizeof(float));
  const audio_listener_t listener = listener_for(track, tps);
  const audio_clock_t clock = {0};
  audio_render(&listener, 1, &clock, from, to, out, *frames);
  return out;
}

static uint32_t loudest(const float *out, uint32_t frames) {
  uint32_t best = 0;
  for (uint32_t i = 0; i < frames; ++i)
    if (fabsf(out[2 * i]) > fabsf(out[2 * best])) best = i;
  return best;
}

static void one_shots(void) {
  const float click[4] = {1.f, 0.f, 0.f, 0.f};
  const ft_audio_sample sample = audio_sample_create(click, 4, 1, AUDIO_RATE);
  assert(sample);
  audio_track_t *track = NULL;
  for (int tick = 1; tick <= 20; ++tick) audio_track_capture(&track, tick, NULL, TPS);
  // Tick 10's step covers game time 9..10; halfway through is 9.5 ticks.
  const ft_audio_sound sound = {.sample = sample, .offset = 0.5f, .volume = 1.f, .pitch = 1.f};
  const ft_audio_step step = {.struct_size = sizeof(step), .sounds = &sound, .sound_count = 1};
  audio_track_capture(&track, 10, &step, TPS);
  assert(audio_track_first_missing(track, 1, 20) == -1);

  uint32_t frames;
  float *out = render(&track, TPS, 0.0, 20.0, &frames);
  const uint32_t at = loudest(out, frames);
  assert(abs((int)at - 9120) <= 1 && out[2 * at] > 0.9f && out[2 * at + 1] > 0.9f);
  free(out);

  // Backwards, it is heard as far from the end.
  out = render(&track, TPS, 20.0, 0.0, &frames);
  assert(abs((int)loudest(out, frames) - (19200 - 9120)) <= 2);
  free(out);

  // An edit at tick 7 changes world 8 on: tick 10 is gone until stepped again.
  audio_track_invalidate(track, 8);
  assert(audio_track_first_missing(track, 1, 20) == 8);
  out = render(&track, TPS, 0.0, 20.0, &frames);
  assert(fabsf(out[2 * loudest(out, frames)]) < 1e-6f);
  free(out);

  audio_track_free(track);
  audio_sample_destroy(sample);
}

static void voices(void) {
  // A 480 Hz tone, a whole number of cycles long so the loop is seamless.
  enum { LENGTH = 4800 };
  static float tone[LENGTH];
  for (int i = 0; i < LENGTH; ++i) tone[i] = sinf(2.f * (float)M_PI * 480.f * i / AUDIO_RATE);
  const ft_audio_sample sample = audio_sample_create(tone, LENGTH, 1, AUDIO_RATE);
  audio_track_t *track = NULL;
  const ft_audio_sound sound = {.sample = sample, .voice = 7, .volume = 1.f, .pitch = 1.f};
  const ft_audio_step step = {.struct_size = sizeof(step), .sounds = &sound, .sound_count = 1};
  for (int tick = 1; tick <= 30; ++tick) audio_track_capture(&track, tick, tick >= 5 && tick <= 24 ? &step : NULL, TPS);

  uint32_t frames;
  float *out = render(&track, TPS, 0.0, 30.0, &frames);
  // It starts at game time 4 and runs on unbroken across the ticks, faded in
  // and out over their first and last few milliseconds.
  const uint32_t start = 4 * 960, end = 24 * 960;
  float error = 0.f;
  for (uint32_t i = start + 480; i < end - 480; ++i) {
    const float expected = sinf(2.f * (float)M_PI * 480.f * (i - start) / AUDIO_RATE);
    error = fmaxf(error, fabsf(out[2 * i] - expected));
  }
  assert(error < 1e-3f);
  for (uint32_t i = 0; i < start; ++i) assert(out[2 * i] == 0.f);
  for (uint32_t i = end; i < frames; ++i) assert(out[2 * i] == 0.f);
  free(out);
  audio_track_free(track);
  audio_sample_destroy(sample);
}

static void stream(void) {
  // A console's blocks at 32 kHz and 30 steps a second, a little long or short
  // in turn; each sample holds its place in the stream.
  const double tps = 30.0;
  audio_track_t *track = NULL;
  int16_t block[2 * 1100];
  int position = 0;
  for (int tick = 1; tick <= 90; ++tick) {
    const uint32_t count = tick % 2 ? 1056 : 1078;
    for (uint32_t i = 0; i < count; ++i) block[2 * i] = block[2 * i + 1] = (int16_t)(position++ % 32768);
    const ft_audio_step step = {.struct_size = sizeof(step), .pcm = block, .pcm_frames = count, .pcm_rate = 32000};
    audio_track_capture(&track, tick, &step, tps);
  }
  uint32_t frames;
  float *out = render(&track, tps, 20.0, 80.0, &frames);
  // The stream plays at its own rate, steadily: no block is sped up or slowed
  // down to fit its step.
  const double per_frame = 32000.0 / AUDIO_RATE;
  for (uint32_t i = 4800; i + 4800 < frames; i += 480) {
    const double a = out[2 * i] * 32768.0, b = out[2 * (i + 480)] * 32768.0;
    if (b < a) continue; // wrapped
    assert(fabs((b - a) / 480.0 - per_frame) < per_frame * 0.004);
  }
  free(out);
  audio_track_free(track);
}

int main(void) {
  one_shots();
  voices();
  stream();
  printf("audio: ok\n");
  return 0;
}
