// Exercise the live queue and device callback with a deterministic clock,
// without opening an audio device. Include the mixer to access those private
// services; offline audio_render cannot reproduce a queued-audio splice.
#include "../src/audio/audio.c"

#include <assert.h>
#include <stdio.h>

static void slow_recording(void) {
  // A seamless stereo engine loop. No sample seam or clipping can explain a
  // pop here: new recording ticks replace the extrapolated pitch and gain.
  enum { LENGTH = 4800, DEVICE_FRAMES = 480 };
  float tone[LENGTH * 2];
  for (int i = 0; i < LENGTH; ++i) {
    const float phase = 2.f * (float)M_PI * 430.f * i / AUDIO_RATE;
    tone[2 * i] = 0.3f * sinf(phase);
    tone[2 * i + 1] = 0.3f * cosf(phase);
  }
  const ft_audio_sample sample = audio_sample_create(tone, LENGTH, 2, AUDIO_RATE);
  assert(sample);
  audio_track_t *track = NULL;
  ft_audio_sound sound = {.sample = sample, .voice = 1, .volume = 1.f, .pitch = 1.f};
  const ft_audio_step step = {.sounds = &sound, .sound_count = 1};
  for (int tick = 1; tick <= 100; ++tick) audio_track_capture(&track, tick, &step, 100.0);
  int captured = 100;
  audio_listener_t listener = {.track = &track, .volume = 1.f, .ticks_per_second = 100.0,
                               .limited = true, .last_tick = captured};
  audio_clock_t clock = {.playing = true, .position = 99.0, .rate = 20.0};

  // A 10 ms device period with 20 ms latency, as a typical output device.
  g_device_open = true;
  g_device.playback.internalSampleRate = AUDIO_RATE;
  g_device.playback.internalPeriodSizeInFrames = DEVICE_FRAMES;
  g_device.playback.internalPeriods = 2;
  float previous[2] = {0}, max_jump = 0.f;
  double energy = 0.0;
  for (int frame = 0; frame < 1000; ++frame) {
    const double now = 1.0 + frame * 0.01;
    // Vary main-thread cadence while the audio device runs steadily. At
    // 20 ticks/sec a newly recorded tick arrives every 50 ms; the 80 ms
    // lookahead will already contain a prediction of it.
    if (frame % 5 != 4) {
      const int tick = 100 + (int)floor(frame * 0.2 + 1e-9);
      for (int t = captured + 1; t <= tick; ++t) {
        sound.pitch = 1.f + 0.3f * sinf((t - 100) * 0.2f);
        sound.volume = 0.7f + 0.2f * sinf((t - 100) * 0.37f);
        audio_track_capture(&track, t, &step, 100.0);
      }
      captured = tick;
      listener.last_tick = tick;
      clock.position = 99.0 + frame * 0.2;
      mixer_update(&listener, 1, &clock, now);
    }
    float out[DEVICE_FRAMES * 2];
    device_data(NULL, out, NULL, DEVICE_FRAMES);
    // Use the simulated callback time rather than the wall clock recorded
    // by device_data, so the next update follows exactly this device take.
    g_take_time = now;
    for (int i = 0; i < DEVICE_FRAMES; ++i) {
      for (int channel = 0; channel < 2; ++channel) {
        const float value = out[2 * i + channel];
        assert(isfinite(value));
        if (frame > 10) {
          max_jump = fmaxf(max_jump, fabsf(value - previous[channel]));
          energy += value * value;
        }
        previous[channel] = value;
      }
    }
  }
  // The oscillation itself changes smoothly, by a few thousandths per
  // output frame. An unblended replacement produces jumps above 0.2.
  assert(energy > 1000.0);
  assert(max_jump < 0.008f);
  printf("audio realtime: largest recording splice step %.6f\n", max_jump);
  audio_stop();
  g_device_open = false; // the test never opened a real device
  audio_track_free(track);
  audio_sample_destroy(sample);
  audio_shutdown();
}

int main(void) {
  slow_recording();
  return 0;
}
