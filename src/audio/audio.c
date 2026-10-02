#include "audio.h"

#include <logger/logger.h>
#include <limits.h>
#include <math.h>
#include <miniaudio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// --- samples ---------------------------------------------------------------------

// A handle is a slot index plus one, and the slot's generation above it, so a
// destroyed sample's handle stays dead when the slot is reused.
#define SAMPLE_INDEX_BITS 20u
#define SAMPLE_INDEX_MASK ((1u << SAMPLE_INDEX_BITS) - 1u)

typedef struct sample {
  float *data;
  uint32_t frames, channels, rate;
  uint32_t generation;
  bool used;
} sample_t;

static sample_t *g_samples;
static uint32_t g_sample_count, g_sample_capacity;

static const sample_t *sample_get(ft_audio_sample handle) {
  const uint32_t index = (handle & SAMPLE_INDEX_MASK) - 1u;
  if (!handle || index >= g_sample_count) return NULL;
  const sample_t *s = &g_samples[index];
  return s->used && s->generation == handle >> SAMPLE_INDEX_BITS ? s : NULL;
}

static ft_audio_sample sample_add(float *data, uint32_t frames, uint32_t channels, uint32_t rate) {
  if (!data || !frames || channels < 1 || channels > 2 || !rate) {
    free(data);
    return 0;
  }
  uint32_t index = 0;
  while (index < g_sample_count && g_samples[index].used)
    ++index;
  if (index == g_sample_count) {
    if (g_sample_count == SAMPLE_INDEX_MASK) {
      free(data);
      return 0;
    }
    if (g_sample_count == g_sample_capacity) {
      const uint32_t capacity = g_sample_capacity ? g_sample_capacity * 2 : 64;
      sample_t *grown = realloc(g_samples, capacity * sizeof(*grown));
      if (!grown) {
        free(data);
        return 0;
      }
      g_samples = grown;
      g_sample_capacity = capacity;
    }
    g_samples[g_sample_count++] = (sample_t){0};
  }
  sample_t *s = &g_samples[index];
  const uint32_t generation = (s->generation + 1u) & (0xFFFFFFFFu >> SAMPLE_INDEX_BITS);
  *s = (sample_t){data, frames, channels, rate, generation ? generation : 1u, true};
  return (s->generation << SAMPLE_INDEX_BITS) | (index + 1u);
}

static ft_audio_sample sample_from_decoder(ma_decoder *decoder) {
  const uint32_t channels = decoder->outputChannels;
  const uint32_t rate = decoder->outputSampleRate;
  if (channels < 1 || channels > 2) {
    ma_decoder_uninit(decoder);
    return 0;
  }
  // Not every format knows its length up front: read in chunks.
  size_t capacity = 0, frames = 0;
  float *data = NULL;
  for (;;) {
    if (frames == capacity) {
      capacity = capacity ? capacity * 2 : 16384;
      float *grown = realloc(data, capacity * channels * sizeof(float));
      if (!grown) {
        free(data);
        ma_decoder_uninit(decoder);
        return 0;
      }
      data = grown;
    }
    ma_uint64 read = 0;
    const ma_result result =
        ma_decoder_read_pcm_frames(decoder, data + frames * channels, capacity - frames, &read);
    frames += (size_t)read;
    if (result != MA_SUCCESS || read == 0) break;
  }
  ma_decoder_uninit(decoder);
  if (frames == 0 || frames > UINT32_MAX) {
    free(data);
    return 0;
  }
  return sample_add(data, (uint32_t)frames, channels, rate);
}

ft_audio_sample audio_sample_load(const char *path) {
  if (!path) return 0;
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
  ma_decoder decoder;
  if (ma_decoder_init_file(path, &config, &decoder) != MA_SUCCESS) return 0;
  return sample_from_decoder(&decoder);
}

ft_audio_sample audio_sample_decode(const void *data, size_t size) {
  if (!data || !size) return 0;
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
  ma_decoder decoder;
  if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS) return 0;
  return sample_from_decoder(&decoder);
}

ft_audio_sample audio_sample_create(const float *frames, uint32_t frame_count, uint32_t channels, uint32_t sample_rate) {
  if (!frames || !frame_count || channels < 1 || channels > 2) return 0;
  float *data = malloc((size_t)frame_count * channels * sizeof(float));
  if (!data) return 0;
  memcpy(data, frames, (size_t)frame_count * channels * sizeof(float));
  return sample_add(data, frame_count, channels, sample_rate);
}

void audio_sample_destroy(ft_audio_sample handle) {
  sample_t *s = (sample_t *)sample_get(handle);
  if (!s) return;
  free(s->data);
  s->data = NULL;
  s->used = false;
}

// Linear interpolation between a sample's frames; `pos` in frames.
static inline void sample_read(const sample_t *s, double pos, float *left, float *right) {
  const uint32_t i = (uint32_t)pos;
  if (i >= s->frames) {
    *left = *right = 0.f;
    return;
  }
  const float t = (float)(pos - (double)i);
  const uint32_t j = i + 1 < s->frames ? i + 1 : i;
  if (s->channels == 1) {
    *left = *right = s->data[i] + (s->data[j] - s->data[i]) * t;
  } else {
    *left = s->data[2 * i] + (s->data[2 * j] - s->data[2 * i]) * t;
    *right = s->data[2 * i + 1] + (s->data[2 * j + 1] - s->data[2 * i + 1]) * t;
  }
}

// --- tracks ---------------------------------------------------------------------

typedef struct captured_sound {
  ft_audio_sound sound;
  // One-shots: how long they sound, in ticks. Voices: where in the sample
  // (frames) the tick starts, carried over from the tick before.
  double duration;
  double phase;
} captured_sound_t;

typedef struct audio_tick {
  captured_sound_t *sounds;
  uint32_t sound_count;
  int16_t *pcm;
  uint32_t pcm_frames, pcm_rate;
  // The stream position (frames) where this block ends, and how far that is
  // from where an even stream would be, smoothed: games fill an audio
  // interface by its level, so blocks run a little long or short.
  double pcm_end;
  double pcm_drift;
  bool captured;
} audio_tick_t;

struct audio_track {
  audio_tick_t *ticks;
  int tick_count;
  // The longest a one-shot sounds, in ticks: how far back one may still be
  // heard from.
  double tail;
  uint64_t revision;
  // The earliest tick captured since the mixer last looked that it had not
  // had before (INT_MAX: none): sound that may land in what it rendered.
  int fresh;
  struct audio_track *next;
};

static audio_track_t *g_tracks;
static size_t g_pcm_bytes;

// Captured console sound kept around the playhead; the rest is simulated again
// when it is heard.
#define PCM_BUDGET ((size_t)512 << 20)

static void tick_clear(audio_tick_t *t) {
  free(t->sounds);
  if (t->pcm) g_pcm_bytes -= (size_t)t->pcm_frames * 4;
  free(t->pcm);
  memset(t, 0, sizeof(*t));
}

static bool track_reserve(audio_track_t *track, int count) {
  if (count <= track->tick_count) return true;
  int capacity = track->tick_count ? track->tick_count : 1024;
  while (capacity < count)
    capacity *= 2;
  audio_tick_t *grown = realloc(track->ticks, (size_t)capacity * sizeof(*grown));
  if (!grown) return false;
  memset(grown + track->tick_count, 0, (size_t)(capacity - track->tick_count) * sizeof(*grown));
  track->ticks = grown;
  track->tick_count = capacity;
  return true;
}

static const audio_tick_t *track_tick(const audio_track_t *track, int tick) {
  if (!track || tick < 0 || tick >= track->tick_count) return NULL;
  const audio_tick_t *t = &track->ticks[tick];
  return t->captured ? t : NULL;
}

static const captured_sound_t *find_voice(const audio_tick_t *t, const ft_audio_sound *sound) {
  if (!t) return NULL;
  for (uint32_t i = 0; i < t->sound_count; ++i)
    if (t->sounds[i].sound.voice == sound->voice && t->sounds[i].sound.sample == sound->sample) return &t->sounds[i];
  return NULL;
}

void audio_track_capture(audio_track_t **slot, int tick, const ft_audio_step *step, double ticks_per_second) {
  if (!slot || tick < 0 || ticks_per_second <= 0.0) return;
  audio_track_t *track = *slot;
  if (!track) {
    if (!(track = calloc(1, sizeof(*track)))) return;
    track->fresh = INT_MAX;
    track->next = g_tracks;
    g_tracks = track;
    *slot = track;
  }
  if (!track_reserve(track, tick + 1)) return;
  audio_tick_t *t = &track->ticks[tick];
  // New sound for the tick (it had none, or other ones) may land in what the
  // mixer rendered already.
  bool same = t->captured && (step ? step->sound_count : 0) == t->sound_count && !t->pcm == !(step && step->pcm_frames);
  for (uint32_t i = 0; same && i < t->sound_count; ++i) same = t->sounds[i].sound.sample == step->sounds[i].sample;
  if (!same && tick < track->fresh) track->fresh = tick;
  tick_clear(t);
  t->captured = true;
  if (!step) return;
  const audio_tick_t *previous = track_tick(track, tick - 1);

  if (step->sound_count && step->sounds) {
    t->sounds = malloc(step->sound_count * sizeof(*t->sounds));
    for (uint32_t i = 0; t->sounds && i < step->sound_count; ++i) {
      const ft_audio_sound *sound = &step->sounds[i];
      const sample_t *s = sample_get(sound->sample);
      if (!s || !(sound->pitch > 0.f) || !(sound->volume > 0.f)) continue;
      captured_sound_t *c = &t->sounds[t->sound_count++];
      c->sound = *sound;
      if (!sound->voice) {
        c->sound.offset = fminf(fmaxf(sound->offset, 0.f), 1.f);
        c->duration = (double)s->frames / s->rate / sound->pitch * ticks_per_second;
        if (c->duration + 1.0 > track->tail) track->tail = c->duration + 1.0;
      } else {
        // A voice that sounded the tick before carries on where it was.
        const captured_sound_t *before = find_voice(previous, sound);
        if (sound->flags & FT_AUDIO_CLOCKED)
          c->phase = fmod(fmax((double)sound->offset, 0.0) * s->rate, (double)s->frames);
        else
          c->phase = before ? fmod(before->phase + before->sound.pitch * s->rate / ticks_per_second, (double)s->frames) : 0.0;
      }
    }
  }

  if (step->pcm && step->pcm_frames && step->pcm_rate) {
    t->pcm = malloc((size_t)step->pcm_frames * 4);
    if (t->pcm) {
      memcpy(t->pcm, step->pcm, (size_t)step->pcm_frames * 4);
      t->pcm_frames = step->pcm_frames;
      t->pcm_rate = step->pcm_rate;
      g_pcm_bytes += (size_t)step->pcm_frames * 4;
      const double per_tick = step->pcm_rate / ticks_per_second;
      const bool continues = previous && previous->pcm && previous->pcm_rate == step->pcm_rate;
      t->pcm_end = (continues ? previous->pcm_end : tick * per_tick) + step->pcm_frames;
      // Jitter is smoothed away; a jump (the game refilling its buffers after a
      // reset) is taken at once rather than played out fast.
      const double drift = t->pcm_end - tick * per_tick;
      const bool small = continues && fabs(drift - previous->pcm_drift) < per_tick * 0.25;
      t->pcm_drift = small ? previous->pcm_drift + (drift - previous->pcm_drift) / 16.0 : drift;
    }
  }
}

void audio_track_invalidate(audio_track_t *track, int first) {
  if (!track) return;
  if (first < 0) first = 0;
  for (int tick = first; tick < track->tick_count; ++tick)
    if (track->ticks[tick].captured) tick_clear(&track->ticks[tick]);
  ++track->revision;
}

void audio_track_free(audio_track_t *track) {
  if (!track) return;
  for (audio_track_t **link = &g_tracks; *link; link = &(*link)->next)
    if (*link == track) {
      *link = track->next;
      break;
    }
  for (int tick = 0; tick < track->tick_count; ++tick)
    tick_clear(&track->ticks[tick]);
  free(track->ticks);
  free(track);
}

int audio_track_first_missing(const audio_track_t *track, int first, int last) {
  if (first < 0) first = 0;
  for (int tick = first; tick <= last; ++tick)
    if (!track_tick(track, tick)) return tick;
  return -1;
}

// Keeps console sound near the playhead when it outgrows its budget.
static void tracks_trim(const audio_track_t *heard, double local_tick, double ticks_per_second) {
  if (g_pcm_bytes <= PCM_BUDGET) return;
  const double keep = 60.0 * ticks_per_second;
  for (audio_track_t *track = g_tracks; track; track = track->next) {
    for (int tick = 0; tick < track->tick_count; ++tick) {
      if (!track->ticks[tick].pcm) continue;
      if (track == heard && fabs(tick - local_tick) < keep) continue;
      tick_clear(&track->ticks[tick]);
    }
    ++track->revision;
  }
}

// --- authored events --------------------------------------------------------------

// Sorted by world, then tick.
static audio_event_sound_t *g_event_sounds;
static int g_event_sound_count;
static double g_event_tail_seconds; // the longest of their samples
static uint64_t g_event_revision;

static int compare_event_sounds(const void *a, const void *b) {
  const audio_event_sound_t *x = a, *y = b;
  if (x->world_index != y->world_index) return x->world_index < y->world_index ? -1 : 1;
  return x->tick < y->tick ? -1 : x->tick > y->tick;
}

void audio_set_event_sounds(const audio_event_sound_t *sounds, int count) {
  free(g_event_sounds);
  g_event_sounds = NULL;
  g_event_sound_count = 0;
  g_event_tail_seconds = 0.0;
  ++g_event_revision;
  if (count <= 0 || !(g_event_sounds = malloc((size_t)count * sizeof(*g_event_sounds)))) return;
  for (int i = 0; i < count; ++i) {
    const sample_t *sample = sample_get(sounds[i].sound.sample);
    if (!sample || !(sounds[i].sound.pitch > 0.f) || !(sounds[i].sound.volume > 0.f)) continue;
    g_event_sounds[g_event_sound_count++] = sounds[i];
    const double seconds = (double)sample->frames / sample->rate / sounds[i].sound.pitch;
    if (seconds > g_event_tail_seconds) g_event_tail_seconds = seconds;
  }
  qsort(g_event_sounds, (size_t)g_event_sound_count, sizeof(*g_event_sounds), compare_event_sounds);
}

// The first of world's event sounds at `tick` or later.
static int event_sounds_from(int world_index, int tick) {
  int lo = 0, hi = g_event_sound_count;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const audio_event_sound_t *e = &g_event_sounds[mid];
    if (e->world_index < world_index || (e->world_index == world_index && e->tick < tick)) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// --- device ----------------------------------------------------------------------

// Rendered sound waits here for the device. Positions count frames since the
// device opened; the frame thread writes ahead of the device thread's reading,
// and may take back what was not read yet, all under the lock.
#define RING_FRAMES 16384u
static float g_ring[RING_FRAMES * 2];
static uint64_t g_read, g_write;
static ma_spinlock g_lock;
static ma_device g_device;
static bool g_device_open;
static bool g_recording;
static float g_volume = 1.f;
// When the device last took frames, and how many: where it is between takes.
static double g_take_time;
static uint64_t g_take_frame;
static uint32_t g_take_count;

// A monotonic clock both threads can read, whatever else is up.
static double seconds_now(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void device_data(ma_device *device, void *output, const void *input, ma_uint32 frame_count) {
  (void)device;
  (void)input;
  float *out = output;
  ma_spinlock_lock(&g_lock);
  uint32_t available = (uint32_t)(g_write - g_read);
  if (available > frame_count) available = frame_count;
  for (uint32_t i = 0; i < available; ++i) {
    const uint32_t at = (uint32_t)((g_read + i) % RING_FRAMES);
    out[2 * i] = g_ring[2 * at];
    out[2 * i + 1] = g_ring[2 * at + 1];
  }
  g_take_time = seconds_now();
  g_take_frame = g_read;
  g_take_count = frame_count;
  g_read += available;
  ma_spinlock_unlock(&g_lock);
  memset(out + 2 * available, 0, (size_t)(frame_count - available) * 2 * sizeof(float));
}

bool audio_init(void) {
  if (g_device_open) return true;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = AUDIO_RATE;
  config.periodSizeInMilliseconds = 10;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.dataCallback = device_data;
  if (ma_device_init(NULL, &config, &g_device) != MA_SUCCESS) {
    log_warn("Audio", "No sound: the output device did not open");
    return false;
  }
  if (ma_device_start(&g_device) != MA_SUCCESS) {
    log_warn("Audio", "No sound: the output device did not start");
    ma_device_uninit(&g_device);
    return false;
  }
  g_device_open = true;
  return true;
}

void audio_shutdown(void) {
  if (g_device_open) ma_device_uninit(&g_device);
  g_device_open = false;
  audio_set_event_sounds(NULL, 0);
  for (uint32_t i = 0; i < g_sample_count; ++i)
    free(g_samples[i].data);
  free(g_samples);
  g_samples = NULL;
  g_sample_count = g_sample_capacity = 0;
}

bool audio_enabled(void) { return g_device_open || g_recording; }

static bool g_heard;
bool audio_set_heard(bool heard) {
  const bool previous = g_heard;
  g_heard = heard;
  return previous;
}
bool audio_heard(void) { return g_heard && audio_enabled(); }
void audio_set_recording(bool recording) { g_recording = recording; }
void audio_set_volume(float volume) { g_volume = fminf(fmaxf(volume, 0.f), 1.f); }

// How long after the device takes a frame it is heard.
static double device_latency(void) {
  if (!g_device_open || !g_device.playback.internalSampleRate) return 0.0;
  return (double)g_device.playback.internalPeriodSizeInFrames * g_device.playback.internalPeriods /
         g_device.playback.internalSampleRate;
}

// --- mixing ----------------------------------------------------------------------

// A tape head: plays the timeline from `position` at `start` (an output frame),
// moving `rate` position units a frame, until `end`, faded in and out.
typedef struct head {
  uint64_t start, end;
  double position, rate;
  double nominal; // the playhead's own rate the head follows, before any steering
  int steering;   // -1, 0 or 1: slowed, even or hurried, to meet the playhead
  uint32_t fade_in, fade_out;
  bool grain; // shaped as a whole by a Hann window instead
} head_t;

#define MAX_HEADS 32
#define BLOCK 64
#define FADE_IN (AUDIO_RATE / 200)    // 5 ms
#define FADE_OUT (AUDIO_RATE / 100)   // 10 ms
#define GRAIN (AUDIO_RATE * 7 / 100)  // 70 ms
#define GRAIN_SPACING (GRAIN / 2)
// What stays as rendered: only a little, as the device takes its frames under
// the same lock and whatever it took is never rewritten anyway.
#define KEEP (AUDIO_RATE / 200)       // 5 ms
#define FILL (AUDIO_RATE * 8 / 100)   // how far ahead is rendered: 80 ms

typedef struct mixer {
  head_t heads[MAX_HEADS];
  int head_count;
  int main_head; // the one following the playhead, or -1
  bool was_playing;
  double last_position;
  double grain_position;
  uint64_t last_grain;
  audio_track_t *track;
  uint64_t revision, event_revision;
  bool scrub_ready;
} mixer_t;

static mixer_t g_mixer = {.main_head = -1};

static double head_position(const head_t *h, uint64_t frame) { return h->position + (double)(frame - h->start) * h->rate; }

// The last tick that may be heard.
static int decided(const audio_listener_t *listener) { return listener->limited ? listener->last_tick : INT_MAX; }

static double map_tick(const audio_clock_t *clock, double position) {
  return clock->game_tick ? clock->game_tick(clock->user, position) : position;
}

static void gains_for(const audio_listener_t *listener, const ft_audio_sound *sound, float gain[2]) {
  gain[0] = gain[1] = sound->volume;
  float g[2];
  if ((sound->flags & FT_AUDIO_POSITIONED) && listener->spatialize &&
      listener->spatialize(listener->user, listener->world_index, sound, g)) {
    gain[0] *= g[0];
    gain[1] *= g[1];
  }
}

static void pcm_read(const audio_track_t *track, double local, double ticks_per_second, float *left, float *right) {
  *left = *right = 0.f;
  const int tick = (int)floor(local) + 1;
  const audio_tick_t *t = track_tick(track, tick);
  if (!t || !t->pcm) return;
  const double per_tick = t->pcm_rate / ticks_per_second;
  const audio_tick_t *before = track_tick(track, tick - 1);
  const double f = local - (tick - 1);
  const bool smooth = before && before->pcm && fabs(t->pcm_drift - before->pcm_drift) < per_tick * 0.25;
  const double drift = smooth ? before->pcm_drift + (t->pcm_drift - before->pcm_drift) * f : t->pcm_drift;
  const double at = local * per_tick + drift;
  // The block holding that stream position is this tick's or a neighbour's.
  static const int order[] = {0, 1, -1, 2, -2, 3, -3};
  for (int k = 0; k < (int)(sizeof(order) / sizeof(order[0])); ++k) {
    const audio_tick_t *b = track_tick(track, tick + order[k]);
    if (!b || !b->pcm || b->pcm_rate != t->pcm_rate) continue;
    const double begin = b->pcm_end - b->pcm_frames;
    if (at < begin || at >= b->pcm_end) continue;
    const double pos = at - begin;
    const uint32_t i = (uint32_t)pos;
    const uint32_t j = i + 1 < b->pcm_frames ? i + 1 : i;
    const float w = (float)(pos - i);
    *left = (b->pcm[2 * i] + (b->pcm[2 * j] - b->pcm[2 * i]) * w) * (1.f / 32768.f);
    *right = (b->pcm[2 * i + 1] + (b->pcm[2 * j + 1] - b->pcm[2 * i + 1]) * w) * (1.f / 32768.f);
    return;
  }
}

// Adds a one-shot starting at local tick `begin` and lasting `duration` ticks
// to the frames of a span (frame i at local tick from + step * i).
static void mix_one_shot(const audio_listener_t *listener, const ft_audio_sound *sound, double begin, double duration,
                         double from, double step, int n, const float *envelope, float *out) {
  const sample_t *s = sample_get(sound->sample);
  if (!s) return;
  float gain[2];
  gains_for(listener, sound, gain);
  const double frames_per_tick = s->rate * sound->pitch / listener->ticks_per_second;
  for (int i = 0; i < n; ++i) {
    const double since = from + step * i - begin;
    if (since < 0.0 || since >= duration) continue;
    float l, r;
    sample_read(s, since * frames_per_tick, &l, &r);
    out[2 * i] += l * gain[0] * envelope[i];
    out[2 * i + 1] += r * gain[1] * envelope[i];
  }
}

// The one-shots the playhead's own head has started. Rendering runs a little
// ahead of what is heard, and an edit (or recording) can change the ticks it
// rendered: a sound already started then plays out as it began, and one that
// turns up after its start was rendered starts from its beginning where the
// rendering still can, instead of in its middle. Either way nothing clicks.
typedef struct live_sound {
  int tick, ordinal; // which one it is: the tick's nth sound of its sample
  ft_audio_sound sound;
  double begin, duration; // local ticks, as it plays
  double origin;          // where the track puts its start
  bool seen;
} live_sound_t;

#define MAX_LIVE 512
static live_sound_t g_live[MAX_LIVE];
static int g_live_count;
static bool g_live_seed; // a new head: what is found playing plays on from where it is

static void live_reset(void) {
  g_live_count = 0;
  g_live_seed = true;
}

// Brings the live sounds up to date with the track for a render from local tick
// `committed` (the first one still to be rendered) to `until`; `heard` is where
// the device is.
static void live_reconcile(const audio_track_t *track, int last_tick, double ticks_per_second, double heard, double committed,
                           double until) {
  // A sound turning up this late was still to come when its tick was decided
  // (recording); one older than that is an edit behind the playhead. A sound
  // that played is remembered until then, so it is not taken for a late one.
  const double late = 0.15 * ticks_per_second;
  int kept = 0;
  for (int i = 0; i < g_live_count; ++i)
    if (g_live[i].begin + g_live[i].duration > heard || g_live[i].origin >= heard - late - 1.0) {
      g_live[kept] = g_live[i];
      g_live[kept++].seen = false;
    }
  g_live_count = kept;
  const int first = track ? (int)floor(fmin(committed - track->tail, heard - late)) : 0;
  const int last = (int)fmin(ceil(until) + 1, (double)last_tick);
  for (int tick = first < 1 ? 1 : first; track && tick <= last; ++tick) {
    const audio_tick_t *t = track_tick(track, tick);
    if (!t) continue;
    for (uint32_t k = 0; k < t->sound_count; ++k) {
      const captured_sound_t *c = &t->sounds[k];
      if (c->sound.voice) continue;
      const double begin = tick - 1 + c->sound.offset;
      if (begin > until) continue;
      int ordinal = 0;
      for (uint32_t j = 0; j < k; ++j) ordinal += t->sounds[j].sound.sample == c->sound.sample && !t->sounds[j].sound.voice;
      live_sound_t *live = NULL;
      for (int i = 0; i < g_live_count && !live; ++i)
        if (g_live[i].tick == tick && g_live[i].ordinal == ordinal && g_live[i].sound.sample == c->sound.sample) live = &g_live[i];
      if (live) {
        live->seen = true;
        continue;
      }
      // Its start is still to be rendered; or a new head finds it playing; or
      // it turned up late, and starts from its beginning now.
      double at;
      if (begin >= committed) at = begin;
      else if (g_live_seed) {
        if (begin + c->duration <= committed) continue;
        at = begin;
      } else if (begin >= heard - late) at = committed;
      else continue;
      if (g_live_count == MAX_LIVE) continue;
      live = &g_live[g_live_count++];
      *live = (live_sound_t){
          .tick = tick, .ordinal = ordinal, .sound = c->sound, .begin = at, .duration = c->duration, .origin = begin, .seen = true};
    }
  }
  // Gone before a frame of it was rendered: never heard, so let it go.
  kept = 0;
  for (int i = 0; i < g_live_count; ++i)
    if (g_live[i].seen || g_live[i].begin < committed) g_live[kept++] = g_live[i];
  g_live_count = kept;
  g_live_seed = false;
}

// Adds the sound of game time going from local tick `from` to `to` over `n`
// frames, weighted per frame by `envelope`. With `live`, the one-shots are the
// live ones rather than the track's.
static void mix_span(const audio_listener_t *listener, double from, double to, int n, const float *envelope, float *out,
                     bool live) {
  const audio_track_t *track = *listener->track;
  if (!track) return;
  const double tps = listener->ticks_per_second;
  const double step = n > 0 ? (to - from) / n : 0.0;
  const double lo = fmin(from, to), hi = fmax(from, to);

  // One-shots that started in the tail before and sound into the span.
  const int first = (int)floor(lo - track->tail);
  const int last = (int)fmin(ceil(hi) + 1, (double)decided(listener));
  for (int i = 0; live && i < g_live_count; ++i) {
    const live_sound_t *l = &g_live[i];
    if (l->begin <= hi && l->begin + l->duration >= lo)
      mix_one_shot(listener, &l->sound, l->begin, l->duration, from, step, n, envelope, out);
  }
  for (int tick = first < 1 ? 1 : first; !live && tick <= last; ++tick) {
    const audio_tick_t *t = track_tick(track, tick);
    if (!t) continue;
    for (uint32_t k = 0; k < t->sound_count; ++k) {
      const captured_sound_t *c = &t->sounds[k];
      if (c->sound.voice) continue;
      const double begin = tick - 1 + c->sound.offset;
      if (begin > hi || begin + c->duration < lo) continue;
      mix_one_shot(listener, &c->sound, begin, c->duration, from, step, n, envelope, out);
    }
  }

  // Authored events' sounds, the same way.
  const double event_tail = g_event_tail_seconds * tps + 1.0;
  for (int k = event_sounds_from(listener->world_index, (int)floor(lo - event_tail));
       k < g_event_sound_count && g_event_sounds[k].world_index == listener->world_index && g_event_sounds[k].tick <= last; ++k) {
    const audio_event_sound_t *e = &g_event_sounds[k];
    const sample_t *s = sample_get(e->sound.sample);
    if (!s) continue;
    const double begin = e->tick - 1;
    const double duration = (double)s->frames / s->rate / e->sound.pitch * tps;
    if (begin > hi || begin + duration < lo) continue;
    mix_one_shot(listener, &e->sound, begin, duration, from, step, n, envelope, out);
  }

  // Continuous voices and the game's own stream, tick by tick. While
  // recording, the frames being rendered are heard a device latency after the
  // playhead, past the last tick decided: the voices of that tick hold on
  // until the next is decided and rendered over them.
  const double fade = 0.005 * tps; // 5 ms, in ticks
  const int last_decided = decided(listener);
  for (int i = 0; i < n; ++i) {
    const double local = from + step * i;
    if (local < 0.0) continue;
    const int tick = (int)floor(local) + 1;
    const int source = tick <= last_decided ? tick : last_decided;
    const audio_tick_t *t = track_tick(track, source);
    if (!t) continue;
    const double f = local - (tick - 1);
    const double held = tick - source; // ticks past the one captured
    for (uint32_t k = 0; k < t->sound_count; ++k) {
      const captured_sound_t *c = &t->sounds[k];
      if (!c->sound.voice) continue;
      const sample_t *s = sample_get(c->sound.sample);
      if (!s) continue;
      const bool holding = tick >= last_decided;
      const captured_sound_t *next = holding ? c : find_voice(track_tick(track, tick + 1), &c->sound);
      const captured_sound_t *before = held > 0.0 ? c : find_voice(track_tick(track, tick - 1), &c->sound);
      float volume = c->sound.volume;
      if (next) volume += (next->sound.volume - volume) * (float)f;
      else volume *= (float)fmin(1.0, (1.0 - f) / fade);
      if (!before) volume *= (float)fmin(1.0, f / fade);
      double pos = fmod(c->phase + (held + f) / tps * s->rate * c->sound.pitch, (double)s->frames);
      float l, r, gain[2];
      sample_read(s, pos, &l, &r);
      gains_for(listener, &c->sound, gain);
      const float w = volume / (c->sound.volume > 0.f ? c->sound.volume : 1.f) * envelope[i];
      out[2 * i] += l * gain[0] * w;
      out[2 * i + 1] += r * gain[1] * w;
    }
    if (t->pcm && held == 0.0) {
      float l, r;
      pcm_read(track, local, tps, &l, &r);
      out[2 * i] += l * envelope[i];
      out[2 * i + 1] += r * envelope[i];
    }
  }
}

static void cover_span(const audio_listener_t *listener, double from, double to) {
  if (!listener->cover) return;
  const audio_track_t *track = *listener->track;
  const double tail = track ? track->tail : 0.0;
  const int first = (int)floor(fmin(from, to) - tail);
  const int last = (int)fmin(ceil(fmax(from, to)) + 1, (double)decided(listener));
  if (last < 1 || last < first) return;
  if (audio_track_first_missing(track, first < 1 ? 1 : first, last) >= 0)
    listener->cover(listener->user, first < 1 ? 1 : first, last);
}

static float head_envelope(const head_t *h, uint64_t frame) {
  const double since = (double)(frame - h->start);
  if (h->grain) {
    const double length = (double)(h->end - h->start);
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * since / length));
  }
  float e = 1.f;
  if (h->fade_in && since < h->fade_in) e *= (float)(since / h->fade_in);
  if (h->end != UINT64_MAX && h->fade_out) {
    const double left = (double)(h->end - frame);
    if (left < h->fade_out) e *= (float)(left / h->fade_out);
  }
  return e;
}

// Renders output frames [first, first + count) from every head.
static void render_heads(const audio_listener_t *listener, const audio_clock_t *clock, const head_t *heads, int head_count,
                         int live_head, uint64_t first, uint32_t count, float *out) {
  memset(out, 0, (size_t)count * 2 * sizeof(float));
  float envelope[BLOCK];
  for (int h = 0; h < head_count; ++h) {
    const head_t *head = &heads[h];
    const uint64_t begin = head->start > first ? head->start : first;
    const uint64_t end = head->end < first + count ? head->end : first + count;
    for (uint64_t at = begin; at < end; at += BLOCK) {
      const int n = (int)(end - at < BLOCK ? end - at : BLOCK);
      const double from = map_tick(clock, head_position(head, at)) - listener->start_offset;
      const double to = map_tick(clock, head_position(head, at + n)) - listener->start_offset;
      cover_span(listener, from, to);
      for (int i = 0; i < n; ++i)
        envelope[i] = head_envelope(head, at + i);
      mix_span(listener, from, to, n, envelope, out + 2 * (at - first), h == live_head);
    }
  }
  for (uint32_t i = 0; i < count * 2; ++i)
    out[i] = fminf(fmaxf(out[i] * g_volume, -1.f), 1.f);
}

static void heads_drop_before(mixer_t *m, uint64_t frame) {
  int kept = 0;
  for (int h = 0; h < m->head_count; ++h) {
    if (m->heads[h].end <= frame) {
      if (h == m->main_head) m->main_head = -1;
      continue;
    }
    if (h == m->main_head) m->main_head = kept;
    m->heads[kept++] = m->heads[h];
  }
  m->head_count = kept;
}

static head_t *heads_add(mixer_t *m) {
  if (m->head_count == MAX_HEADS) {
    // The oldest grain or tail goes first; the main head stays.
    int victim = m->main_head == 0 ? 1 : 0;
    memmove(&m->heads[victim], &m->heads[victim + 1], (size_t)(m->head_count - victim - 1) * sizeof(head_t));
    if (m->main_head > victim) --m->main_head;
    --m->head_count;
  }
  head_t *h = &m->heads[m->head_count++];
  memset(h, 0, sizeof(*h));
  h->end = UINT64_MAX;
  return h;
}

// Ends the main head at `frame` with a short fade, so stopping or jumping
// never clicks.
static void main_head_release(mixer_t *m, uint64_t frame) {
  if (m->main_head < 0) return;
  head_t *h = &m->heads[m->main_head];
  if (frame < h->start) frame = h->start;
  h->end = frame + FADE_OUT;
  h->fade_out = FADE_OUT;
  m->main_head = -1;
  live_reset();
}

void audio_stop(void) {
  ma_spinlock_lock(&g_lock);
  g_write = g_read;
  ma_spinlock_unlock(&g_lock);
  g_mixer.head_count = 0;
  g_mixer.main_head = -1;
  g_mixer.track = NULL;
  g_mixer.was_playing = false;
  g_mixer.scrub_ready = false;
  live_reset();
}

void audio_update(const audio_listener_t *listener, const audio_clock_t *clock) {
  if (!g_device_open) return;
  mixer_t *m = &g_mixer;
  if (!listener || !listener->track || listener->ticks_per_second <= 0.0) {
    audio_stop();
    return;
  }

  ma_spinlock_lock(&g_lock);
  const uint64_t read = g_read, written = g_write;
  const double take_time = g_take_time;
  const uint64_t take_frame = g_take_frame;
  const uint32_t take_count = g_take_count;
  ma_spinlock_unlock(&g_lock);
  heads_drop_before(m, read);
  // The frame the device is at now. It takes frames at its own steady rate, but
  // its calls come late now and then (never early), so the line through its
  // takes follows the earliest of them, easing down for a clock that runs slow.
  (void)take_count;
  const double now_frames = seconds_now() * AUDIO_RATE;
  static double offset;
  static bool offset_known;
  if (take_time > 0.0) {
    const double take = (double)take_frame - take_time * AUDIO_RATE;
    if (!offset_known || take > offset) offset = take;
    else offset += (take - offset) * 0.01;
    offset_known = true;
  }
  const double playing_frame = offset_known ? now_frames + offset : (double)read;

  const uint64_t keep = written < read + KEEP ? written : read + KEEP;
  const uint64_t end = read + FILL;
  bool rerender = false;

  audio_track_t *track = *listener->track;
  const uint64_t revision = track ? track->revision : 0;
  if (track != m->track || revision != m->revision || g_event_revision != m->event_revision) {
    rerender = true;
    m->track = track;
    m->revision = revision;
    m->event_revision = g_event_revision;
  }

  // Where the playhead will be when a frame is heard.
  const double latency = device_latency();
  const double per_frame = clock->rate / AUDIO_RATE;
  const double base = clock->position + clock->rate * latency;
#define TARGET(frame) (base + ((double)(frame) - playing_frame) * per_frame)

  if (clock->playing && clock->rate != 0.0) {
    head_t *h = m->main_head >= 0 ? &m->heads[m->main_head] : NULL;
    if (h) {
      // How far ahead of the playhead the head is, in seconds.
      const double error = (head_position(h, keep) - TARGET(keep)) / (per_frame * AUDIO_RATE);
      const bool reversed = (h->rate > 0.0) != (per_frame > 0.0);
      if (reversed || fabs(error) > 0.1) {
        // A jump: what was playing fades out under the new place.
        main_head_release(m, keep);
        h = NULL;
      } else if (keep >= h->start + h->fade_in) {
        // Within a few milliseconds nobody hears a difference; beyond, the head
        // eases back three tenths of a percent slower or faster, which nobody
        // hears either. A new speed is taken at once.
        int steering = h->steering;
        if (fabs(error) > 0.010) steering = error > 0.0 ? -1 : 1;
        else if (fabs(error) < 0.002) steering = 0;
        if (steering != h->steering || h->nominal != per_frame) {
          const double position = head_position(h, keep);
          h->nominal = per_frame;
          h->steering = steering;
          h->rate = per_frame * (1.0 + 0.003 * steering);
          h->position = position;
          h->start = keep;
          h->fade_in = 0;
          rerender = true;
        }
      }
    }
    if (!h) {
      h = heads_add(m);
      h->start = keep;
      h->position = TARGET(keep);
      h->rate = h->nominal = per_frame;
      h->fade_in = FADE_IN;
      m->main_head = (int)(h - m->heads);
      live_reset();
      rerender = true;
    }
    m->scrub_ready = false;
  } else {
    if (m->main_head >= 0) {
      main_head_release(m, keep);
      rerender = true;
    }
    // Scrubbing: each new place the playhead stops at is heard for a moment,
    // a grain at a time.
    if (!m->was_playing && m->scrub_ready && clock->position != m->grain_position &&
        (m->last_grain == 0 || keep >= m->last_grain + GRAIN_SPACING)) {
      head_t *g = heads_add(m);
      g->start = keep;
      g->end = keep + GRAIN;
      g->position = clock->position;
      // Game time at its own pace: a tick a tick, or a second of camera time.
      g->rate = (clock->game_tick ? 1.0 : listener->ticks_per_second) / AUDIO_RATE;
      g->grain = true;
      m->grain_position = clock->position;
      m->last_grain = keep;
      rerender = true;
    }
    if (!m->scrub_ready) m->grain_position = clock->position;
    m->scrub_ready = true;
  }
  m->was_playing = clock->playing;
  m->last_position = clock->position;
#undef TARGET

  // Sound captured since the last look for ticks already rendered (a tick
  // decided only now while recording, or simulated late) is rendered again.
  if (track && track->fresh != INT_MAX) {
    const double fresh = track->fresh - 1 + listener->start_offset;
    const head_t *h = m->main_head >= 0 ? &m->heads[m->main_head] : NULL;
    if (h && !clock->game_tick && h->rate > 0.0 && fresh < head_position(h, written)) rerender = true;
    track->fresh = INT_MAX;
  }
  const uint64_t from = rerender ? keep : written;
  if (end <= from) return;
  const uint32_t count = (uint32_t)(end - from);
  float *buffer = malloc((size_t)count * 2 * sizeof(float));
  if (!buffer) return;
  // The playhead's head keeps its one-shots steady while it plays forward on
  // the timeline's own clock.
  int live_head = -1;
  if (m->main_head >= 0 && !clock->game_tick && m->heads[m->main_head].rate > 0.0) {
    const head_t *h = &m->heads[m->main_head];
    const double origin = listener->start_offset;
    cover_span(listener, head_position(h, from) - origin, head_position(h, end) - origin);
    live_reconcile(*listener->track, decided(listener), listener->ticks_per_second,
                   head_position(h, read > h->start ? read : h->start) - origin,
                   head_position(h, from) - origin, head_position(h, end) - origin);
    live_head = m->main_head;
  }
  render_heads(listener, clock, m->heads, m->head_count, live_head, from, count, buffer);
  // What the lookahead captured while rendering is in this render already.
  if (*listener->track) (*listener->track)->fresh = INT_MAX;

  ma_spinlock_lock(&g_lock);
  // Whatever the device took meanwhile is heard as it was.
  const uint64_t taken = g_read;
  const uint64_t start = taken > from ? taken : from;
  for (uint64_t f = start; f < end; ++f) {
    const uint32_t at = (uint32_t)(f % RING_FRAMES);
    g_ring[2 * at] = buffer[2 * (f - from)];
    g_ring[2 * at + 1] = buffer[2 * (f - from) + 1];
  }
  if (end > start) g_write = end;
  ma_spinlock_unlock(&g_lock);
  free(buffer);

  const double now = map_tick(clock, clock->position) - listener->start_offset;
  tracks_trim(*listener->track, now, listener->ticks_per_second);
}

void audio_render(const audio_listener_t *listener, const audio_clock_t *clock, double from, double to, float *out,
                  uint32_t frames) {
  if (!frames) return;
  if (!listener || !listener->track || listener->ticks_per_second <= 0.0) {
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    return;
  }
  const head_t head = {.start = 0, .end = frames, .position = from, .rate = (to - from) / frames};
  render_heads(listener, clock, &head, 1, -1, 0, frames, out);
}
