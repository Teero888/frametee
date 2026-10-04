// The game's sound: the car's (CSceneVehicleCar::VehicleUpdateAsync and the
// audio port behind it), the race's countdown, checkpoints and finish, the
// listener's wind and woosh, the scenery's sources, the environment's
// ambience and music, the horn (tmuf_work/audio_logic_spec.md,
// audio_data_spec.md).
//
// The game writes its sound parameters once per rendered frame from the two
// ticks around it and its audio port turns them into OpenAL gains every 35 ms.
// Here every step is a frame: its sound is made from the states before and
// after it, and the port's smoothing is rescaled to 10 ms ticks. What only the
// physics step knows (the sub-steps' impacts, the checkpoint taken) comes
// from tmuf_world_set_sound, on only while there is sound at all: without
// it a step does no work for sound.

#include "tmuf_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- samples -----------------------------------------------------------------

typedef struct tm_sample {
  ft_audio_sample id;
  float seconds; // its length, 0 when unknown (not a .wav)
} tm_sample;

// Samples by file and channel (-1: all of them), loaded once
typedef struct bank_entry {
  char *path;
  int channel;
  tm_sample sample;
} bank_entry;

struct tm_audio_bank {
  uint32_t count, cap;
  bank_entry *entries;
};

typedef struct wav {
  const uint8_t *data;
  uint32_t frames, channels, rate, bits;
} wav;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

// a PCM .wav's samples (8 or 16 bit, the game's every one)
static bool wav_parse(const uint8_t *d, size_t n, wav *out) {
  if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return false;
  bool fmt = false;
  uint32_t format = 0, block = 0;
  for (size_t at = 12; at + 8 <= n;) {
    const uint32_t size = rd32(d + at + 4);
    const uint8_t *body = d + at + 8;
    if (size > n - at - 8) return false;
    if (memcmp(d + at, "fmt ", 4) == 0 && size >= 16) {
      format = rd16(body);
      out->channels = rd16(body + 2);
      out->rate = rd32(body + 4);
      out->bits = rd16(body + 14);
      fmt = true;
    } else if (memcmp(d + at, "data", 4) == 0 && fmt) {
      if (format != 1 || (out->bits != 8 && out->bits != 16) || out->channels < 1 || out->channels > 2 || !out->rate)
        return false;
      block = out->channels * out->bits / 8u;
      out->data = body;
      out->frames = size / block;
      return out->frames > 0;
    }
    at += 8 + (size_t)size + (size & 1u);
  }
  return false;
}

static tm_sample wav_sample(ft_game *game, const wav *w, int channel) {
  const uint32_t channels = channel < 0 ? w->channels : 1u;
  float *frames = malloc((size_t)w->frames * channels * sizeof *frames);
  if (!frames) return (tm_sample){0};
  for (uint32_t i = 0; i < w->frames; i++)
    for (uint32_t c = 0; c < channels; c++) {
      const uint32_t from = channel < 0 ? c : (uint32_t)channel;
      const uint32_t at = i * w->channels + from;
      frames[i * channels + c] = w->bits == 16 ? (float)(int16_t)rd16(w->data + at * 2u) / 32768.f
                                                : ((float)w->data[at] - 128.f) / 128.f;
    }
  const tm_sample s = {game->engine->audio_sample_create(frames, w->frames, channels, w->rate),
                       (float)w->frames / (float)w->rate};
  free(frames);
  return s;
}

// The sample of a file, or of one of its channels: channel 0 of a mono file
// is the file, channel 1 none.
static tm_sample bank_get(ft_game *game, const char *path, int channel) {
  if (!path) return (tm_sample){0};
  struct tm_audio_bank *b = game->audio_bank;
  if (!b && !(b = game->audio_bank = calloc(1, sizeof *b))) return (tm_sample){0};
  for (uint32_t i = 0; i < b->count; i++)
    if (b->entries[i].channel == channel && strcmp(b->entries[i].path, path) == 0) return b->entries[i].sample;
  tm_sample s = {0};
  void *data = NULL;
  size_t size = 0;
  wav w;
  if (game->engine->read_file(path, &data, &size)) {
    if (wav_parse(data, size, &w)) {
      if (channel < 0 || w.channels > 1) s = wav_sample(game, &w, channel);
      else if (channel == 0) s = wav_sample(game, &w, -1);
    } else if (channel <= 0) {
      s.id = game->engine->audio_sample_decode(data, size);
    }
    game->engine->free_file_data(data);
  }
  if (!s.id) tm_log(game, FT_LOG_WARN, "Cannot load the sound %s", path);
  if (b->count == b->cap) {
    const uint32_t cap = b->cap ? b->cap * 2u : 64u;
    bank_entry *grown = realloc(b->entries, cap * sizeof *grown);
    if (!grown) return s;
    b->entries = grown;
    b->cap = cap;
  }
  const size_t n = strlen(path) + 1;
  char *copy = malloc(n);
  if (!copy) return s;
  memcpy(copy, path, n);
  b->entries[b->count++] = (bank_entry){copy, channel, s};
  return s;
}

void tm_audio_free(ft_game *game) {
  struct tm_audio_bank *b = game->audio_bank;
  if (!b) return;
  for (uint32_t i = 0; i < b->count; i++) {
    if (b->entries[i].sample.id) game->engine->audio_sample_destroy(b->entries[i].sample.id);
    free(b->entries[i].path);
  }
  free(b->entries);
  free(b);
  game->audio_bank = NULL;
}

// --- a level's sounds ----------------------------------------------------------

#define TM_PARTS 8                // an engine's components, a multi's variants
#define TM_LOOPS TM_SURFACE_LOOPS // a surface's distinct rolling and skid samples
#define TM_NO_LOOP 0xFF

typedef struct tm_snd {
  const tmuf_sound *def; // NULL: none
  tm_sample sample;
  // engine: per component its dry and underwater channels (a mono file has
  // the dry one only); multi: the variants in [i][0]
  uint32_t parts;
  tm_sample part[TM_PARTS][2];
  // surface: per material its small and big impacts (with the game's
  // fallbacks), and its rolling and skid loops: indices into loops, each
  // distinct sample one source as the game makes them
  tm_sample impact[TMUF_SOUND_MATERIALS][2];
  uint8_t material_loop[TMUF_SOUND_MATERIALS][2];
  uint32_t loop_count;
  tm_sample loops[TM_LOOPS];
} tm_snd;

enum { MEDAL_NADEO, MEDAL_GOLD, MEDAL_SILVER, MEDAL_BRONZE, MEDAL_NONE, MEDAL_COUNT };

struct tm_level_audio {
  const tmuf_track *track;
  tm_snd car[TMUF_CAR_SOUND_COUNT];
  tmuf_iso4 location[TMUF_CAR_SOUND_COUNT];
  tm_snd countdown[3], go, checkpoint, ambience, wind, woosh, woosh_start, woosh_end, figure;
  tm_snd medal[MEDAL_COUNT];
  // the scene's sounds (tmuf_track_scene_sounds): their sample (a multi's
  // first variant)
  uint32_t scene_count;
  const tmuf_scene_sound *scene;
  tm_sample *scene_sample;
  // the race music (CGameCtnApp::PlayChallengeMusic): the environment's,
  // the one the map's name picks
  tm_sample music;
  // where the scene's water is (the spray's probe only runs near it)
  bool water;
  float water_height;
};

static uint8_t loop_of(ft_game *game, tm_snd *s, const tmuf_sound_component *c) {
  if (!c || !c->file) return TM_NO_LOOP;
  const tm_sample smp = bank_get(game, c->file, -1);
  if (!smp.id) return TM_NO_LOOP;
  for (uint32_t i = 0; i < s->loop_count; i++)
    if (s->loops[i].id == smp.id) return (uint8_t)i;
  if (s->loop_count == TM_LOOPS) return TM_NO_LOOP;
  s->loops[s->loop_count] = smp;
  return (uint8_t)s->loop_count++;
}

static void snd_load(ft_game *game, const tmuf_sound *def, tm_snd *s) {
  memset(s, 0, sizeof *s);
  memset(s->material_loop, TM_NO_LOOP, sizeof s->material_loop);
  s->def = def;
  if (!def) return;
  switch (def->kind) {
  case TMUF_SOUND_ENGINE:
    // with several components the game mixes them in software, a stereo
    // file's two channels apart (dry, underwater); one plays as it is
    s->parts = def->component_count < TM_PARTS ? def->component_count : TM_PARTS;
    for (uint32_t i = 0; i < s->parts; i++) {
      const char *file = def->components[i].file;
      if (s->parts > 1) {
        s->part[i][0] = bank_get(game, file, 0);
        s->part[i][1] = bank_get(game, file, 1);
      } else {
        s->part[i][0] = bank_get(game, file, -1);
      }
    }
    break;
  case TMUF_SOUND_SURFACE:
    // CGenAudioSoundSurface::CreateResources: a missing small impact is
    // Concrete's; a missing big one the material's small, else Concrete's big
    for (uint32_t m = 0; m < TMUF_SOUND_MATERIALS; m++) {
      const tm_sample small = bank_get(game, def->materials[m].small_impact, -1);
      const tm_sample big = bank_get(game, def->materials[m].big_impact, -1);
      s->impact[m][0] = def->materials[m].small_impact ? small : s->impact[0][0];
      s->impact[m][1] = def->materials[m].big_impact ? big : def->materials[m].small_impact ? small : s->impact[0][1];
      s->material_loop[m][0] = loop_of(game, s, def->materials[m].texture);
      s->material_loop[m][1] = loop_of(game, s, def->materials[m].skid);
    }
    break;
  case TMUF_SOUND_MULTI:
    s->parts = def->variant_count < TM_PARTS ? def->variant_count : TM_PARTS;
    for (uint32_t i = 0; i < s->parts; i++)
      s->part[i][0] = bank_get(game, def->variants[i], -1);
    s->sample = s->part[0][0];
    break;
  default: s->sample = bank_get(game, def->file, -1); break;
  }
}

#define SOUND_DIR "Interface\\Media\\Audio\\Sound\\"

static void interface_load(ft_game *game, const tmuf_track *track, const char *name, tm_snd *s) {
  char path[256];
  snprintf(path, sizeof path, SOUND_DIR "%s.Sound.gbx", name);
  snd_load(game, tmuf_track_sound(track, path), s);
}

// The environment's: the decoration's ambience (CGameCtnDecorationAudio
// Sounds[0]) and its race musics (Skins\<dir>\Music\Race)
typedef struct environment_audio {
  const char *environment, *ambience, *music_dir;
  const char *music[3];
} environment_audio;

static const environment_audio *environment_of(const char *environment) {
  static const environment_audio table[] = {
      {"Stadium",
       "Stadium\\Media\\Audio\\Sound\\AmbStadium.Sound.gbx",
       "Stadium",
       {" Trackmania United - Stadium - Pulp remix.ogg", " Trackmania United - Stadium - Start Off remix.ogg",
        " Trackmania United - Stadium - Tictac remix.ogg"}},
      {"Alpine", "Alpine\\Media\\Audio\\Sound\\AmbAlpine.Sound.gbx", "Alpine", {" Trackmania United - Snow.ogg"}},
      {"Snow", "Alpine\\Media\\Audio\\Sound\\AmbAlpine.Sound.gbx", "Alpine", {" Trackmania United - Snow.ogg"}},
      {"Rally", "Rally\\Media\\Audio\\Sound\\AmbiWater.Sound.gbx", "Rally", {" Trackmania United - Rally.ogg"}},
      {"Speed", "Speed\\Media\\Audio\\Sound\\desert.Sound.gbx", "Speed", {" Trackmania United - Desert.ogg"}},
      {"Desert", "Speed\\Media\\Audio\\Sound\\desert.Sound.gbx", "Speed", {" Trackmania United - Desert.ogg"}},
      {"Bay", "Bay\\Media\\Audio\\Sound\\BayAmbiant.Sound.gbx", "Bay", {" Trackmania United - Bay.ogg"}},
      {"Coast", "Coast\\Media\\Audio\\Sound\\CoastAmbiant.Sound.gbx", "Coast", {" Trackmania United - Coast.ogg"}},
      {"Island", "Island\\Media\\Audio\\Sound\\IslandAmbiant.Sound.gbx", "Island", {" Trackmania United - Island.ogg"}},
  };
  for (size_t i = 0; environment && i < sizeof table / sizeof *table; i++)
    if (strcmp(environment, table[i].environment) == 0) return &table[i];
  return NULL;
}

void tm_audio_level_load(ft_game *game, ft_level *level) {
  const ft_engine_api *api = game->engine;
  if (!api->audio_enabled || !api->audio_enabled() || !api->audio_sample_create) return;
  const tmuf_track *track = level->track;
  const tmuf_vehicle_visuals *vehicle = tmuf_track_vehicle_visuals(track);
  if (!vehicle) return;
  struct tm_level_audio *a = calloc(1, sizeof *a);
  if (!a) return;
  a->track = track;
  for (int k = 0; k < TMUF_CAR_SOUND_COUNT; k++) {
    snd_load(game, vehicle->sounds[k].sound, &a->car[k]);
    a->location[k] = vehicle->sounds[k].location;
  }
  static const char *const countdown[3] = {"Race1", "Race2", "Race3"};
  for (int i = 0; i < 3; i++)
    interface_load(game, track, countdown[i], &a->countdown[i]);
  interface_load(game, track, "RaceGo", &a->go);
  interface_load(game, track, "RaceCheckPoint", &a->checkpoint);
  interface_load(game, track, "RaceListenerWind", &a->wind);
  interface_load(game, track, "RaceWoosh", &a->woosh);
  interface_load(game, track, "RaceWooshStart", &a->woosh_start);
  interface_load(game, track, "RaceWooshEnd", &a->woosh_end);
  interface_load(game, track, "RaceFigure", &a->figure);
  static const char *const medals[MEDAL_COUNT] = {"RaceNadeo", "RaceGold", "RaceSilver", "RaceBronze", "RaceFinish"};
  for (int i = 0; i < MEDAL_COUNT; i++)
    interface_load(game, track, medals[i], &a->medal[i]);

  a->scene_count = tmuf_track_scene_sounds(track, &a->scene);
  a->scene_sample = calloc(a->scene_count ? a->scene_count : 1, sizeof *a->scene_sample);
  for (uint32_t i = 0; a->scene_sample && i < a->scene_count; i++) {
    const tmuf_sound *s = a->scene[i].sound;
    if (!s) continue;
    a->scene_sample[i] = bank_get(game, s->kind == TMUF_SOUND_MULTI && s->variant_count ? s->variants[0] : s->file, -1);
  }
  if (!a->scene_sample) a->scene_count = 0;

  const environment_audio *env = environment_of(tmuf_track_environment(track));
  if (env) {
    snd_load(game, tmuf_track_sound(track, env->ambience), &a->ambience);
    uint32_t count = 0, hash = 2166136261u;
    while (count < 3 && env->music[count])
      count++;
    for (const char *c = level->name; *c; c++)
      hash = (hash ^ (uint8_t)*c) * 16777619u;
    char relative[512], path[1024];
    snprintf(relative, sizeof relative, "GameData/Skins/%s/Music/Race/%s", env->music_dir, env->music[hash % count]);
    tm_data_resolve_path(api, relative, path, sizeof path);
    a->music = bank_get(game, path, -1);
  }
  const tmuf_sim *sim = tmuf_track_sim(track);
  a->water = sim->water.enabled != 0;
  a->water_height = sim->water.surface_height;
  level->audio = a;
}

void tm_audio_level_free(ft_game *game, ft_level *level) {
  (void)game;
  if (level->audio) free(level->audio->scene_sample);
  free(level->audio); // (the samples are the game's)
  level->audio = NULL;
}

// --- the step's sound ----------------------------------------------------------
//
// The state that carries from step to step (the surfaces' loops fading, the
// one-shots that do not restart while they play, the last wheel material,
// the woosh) is kept on every step while there is sound at all, so it is the
// same in every world that reaches a tick: the editor's, the one ahead of the
// playhead, one resumed from a snapshot. Only heard steps make their sound.

// The port runs every 35 ms; its per-update factors per 10 ms tick
#define PORT_TICKS (35.0 / TMUF_TICK_MS)
#define WATER 13 // EPlugSurfaceMaterialId

enum {
  VOICE_ENGINE = 0x100,   // + component * 2 + channel
  VOICE_SURFACE = 0x1000, // + slot * 0x100 + loop
  VOICE_BRAKES = 0x2000,
  VOICE_AMBIENCE = 0x3000,
  VOICE_WIND = 0x3001,
  VOICE_WOOSH = 0x3002,
  VOICE_MUSIC = 0x3003,
  VOICE_SCENE = 0x10000, // + scene sound
};

static void frame_of(const tmuf_world *w, tm_audio_frame *f) {
  const tmuf_car *c = &w->sim.car;
  const tmuf_dyna_state *s = &w->sim.body.state;
  f->tick = w->tick;
  f->forward_speed = c->frame.forward_speed;
  f->rpm = c->engine.input_memory;
  f->gas = c->controls.gate_a;
  f->brake = c->controls.gate_b;
  f->local_vz = s->rot.m[0][2] * s->lin.x + s->rot.m[1][2] * s->lin.y + s->rot.m[2][2] * s->lin.z;
  f->speed = sqrtf(s->lin.x * s->lin.x + s->lin.y * s->lin.y + s->lin.z * s->lin.z) * 3.6f;
  f->reverse = c->engine.use_gate_b;
  f->gear = c->engine.gear;
  f->engine_state = c->geared.engine_state;
  f->input_window_exceeded = c->geared.input_window_exceeded;
  f->free_wheeling = c->controls.forced_low_speed_friction;
  f->in_water = c->frame.in_water;
  f->burnout = c->geared.wheel_speed_override;
  f->wheel_count = c->wheel_count < TMUF_CAR_MAX_WHEELS ? c->wheel_count : TMUF_CAR_MAX_WHEELS;
  for (uint32_t i = 0; i < f->wheel_count; i++) {
    f->wheel_contact[i] = c->wheels[i].contact != 0;
    f->wheel_slipping[i] = c->wheels[i].slipping != 0;
    f->wheel_material[i] = c->wheels[i].contact_material;
    f->wheel_front[i] = c->wheels[i].front != 0;
  }
  f->last_wheel_material = c->contacts.last_wheel_material;
  f->last_body_material = c->contacts.last_body_material;
  f->turbo_start = c->turbo.start_tick;
  f->splashes = c->water_splash_events;
  f->checkpoints = w->sim.race.checkpoint_time_count;
  f->completed = w->sim.race.completed;
  f->stunt_score = w->sim.race.stunts.score;
}

void tm_audio_world_init(ft_world *world) {
  world->audio.tick = world->w.tick;
  world->audio.wheel_material = world->w.sim.car.contacts.last_wheel_material;
  world->audio.heard_tick = UINT32_MAX;
}

bool tm_audio_prepare(ft_game *game, ft_world *world, tm_audio_frame *before) {
  const ft_engine_api *api = game->engine;
  const bool on = world->level && world->level->audio && api->audio_enabled && api->audio_enabled();
  if ((world->w.sim.car.sound.enabled != 0) != on) tmuf_world_set_sound(&world->w, on);
  if (!on) {
    world->audio.tick = world->audio.heard_tick = UINT32_MAX;
    return false;
  }
  frame_of(&world->w, before);
  return true;
}

void tm_audio_world_free(ft_world *world) { free(world->audio.sounds); }

void tm_audio_copy(ft_world *dst, const ft_world *src) {
  dst->level = src->level;
  memcpy(&dst->audio, &src->audio, offsetof(tm_audio, heard_tick));
  dst->audio.heard_tick = UINT32_MAX;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// CFuncKeysReal::GetValue; an absent curve is 1
static float curve_at(const tmuf_curve *c, float x) {
  if (!c->present || !c->count) return 1.f;
  if (x <= c->x[0]) return c->y[0];
  for (uint32_t i = 1; i < c->count; i++)
    if (x < c->x[i]) {
      if (c->constant || c->x[i] <= c->x[i - 1]) return c->y[i - 1];
      return lerpf(c->y[i - 1], c->y[i], (x - c->x[i - 1]) / (c->x[i] - c->x[i - 1]));
    }
  return c->y[c->count - 1];
}

// CPlugSoundEngineComponent::ComputeValues: below zero, reverse components
// (authored with negative thresholds) see the magnitude; the others are quiet
static void component_values(const tmuf_sound_component *c, float x, float *volume, float *pitch) {
  float fis = c->fade_in_start, fie = c->fade_in_end, fos = c->fade_out_start, foe = c->fade_out_end;
  float pss = c->pitch_shift_start, pse = c->pitch_shift_end;
  if (x < 0.f) {
    if (fie >= 0.f) {
      *volume = c->min_volume;
      *pitch = c->min_pitch;
      return;
    }
    x = -x, fis = -fis, fie = -fie, fos = -fos, foe = -foe, pss = -pss, pse = -pse;
  }
  *volume = x < fis   ? c->min_volume
            : x < fie ? lerpf(c->min_volume, c->max_volume, (x - fis) / (fie - fis))
            : x < fos ? c->max_volume
            : x < foe ? lerpf(c->max_volume, c->min_volume, (x - fos) / (foe - fos))
                      : c->min_volume;
  *pitch = x < pss ? c->min_pitch : x < pse ? lerpf(c->min_pitch, c->max_pitch, (x - pss) / (pse - pss)) : c->max_pitch;
}

typedef struct step_ctx {
  ft_game *game;
  ft_world *world;
  const struct tm_level_audio *la;
  tm_audio *st;
  const tm_audio_frame *a, *b;
  bool fresh; // the state is not the last step's: nothing to smooth from
  bool heard; // the step's sound is made (else only the state goes on)
} step_ctx;

// where the step's sound is in the world: a point, its falloff
static ft_audio_sound *add(const step_ctx *c, ft_audio_sample sample, uint32_t voice, float volume, float pitch,
                           float offset, const float *position, const tmuf_sound *def) {
  tm_audio *st = c->st;
  if (!c->heard || !sample || volume <= 1e-4f || st->count == TM_AUDIO_SOUNDS) return NULL;
  ft_audio_sound *s = &st->sounds[st->count++];
  // Every sound goes through tm_audio_spatialize, which applies the Sound
  // settings' volumes as it is mixed: a 2D one (distance 0) is not placed.
  *s = (ft_audio_sound){
      .sample = sample, .voice = voice, .flags = FT_AUDIO_POSITIONED, .offset = offset, .volume = volume, .pitch = pitch};
  if (position && def && def->mode >= TMUF_SOUND_3D) {
    // inverse distance from the reference distance, silent from 50 times it
    // (3D omni: clamped at its max distance, CAudioPort::Update)
    memcpy(s->position, position, sizeof s->position);
    const float ref = def->ref_distance > 0.f ? def->ref_distance : 1.f;
    s->distance[0] = ref;
    s->distance[1] =
        def->mode == TMUF_SOUND_3D_OMNI && def->max_distance_omni > ref ? def->max_distance_omni : 50.f * ref;
  }
  return s;
}

// a voice on the world's clock: where its sample is at the step's start
static void clocked(const step_ctx *c, ft_audio_sound *s, float phase) {
  if (!s) return;
  s->flags |= FT_AUDIO_CLOCKED;
  s->offset = (float)fmod((double)c->a->tick * TMUF_TICK_MS / 1000.0 + phase, 86400.0);
}

static void transform(const tmuf_iso4 *iso, const float p[3], float out[3]) {
  const float t[3] = {iso->t.x, iso->t.y, iso->t.z};
  for (int r = 0; r < 3; r++)
    out[r] = t[r] + iso->r.m[r][0] * p[0] + iso->r.m[r][1] * p[1] + iso->r.m[r][2] * p[2];
}

static tmuf_iso4 car_pose(const step_ctx *c) {
  const tmuf_dyna_state *s = &c->world->w.sim.body.state;
  return (tmuf_iso4){s->rot, s->pos};
}

// where a car sound slot is in the world
static void slot_position(const step_ctx *c, int slot, float out[3]) {
  const tmuf_iso4 pose = car_pose(c);
  const tmuf_vec3 l = c->la->location[slot].t;
  transform(&pose, (const float[3]){l.x, l.y, l.z}, out);
}

// whether a sample still plays from the last time a slot played it
static bool busy(const step_ctx *c, int slot, tm_sample sample) {
  return c->st->busy_sample[slot] == sample.id && c->b->tick < c->st->busy_until[slot];
}
static void set_busy(const step_ctx *c, int slot, tm_sample sample) {
  c->st->busy_sample[slot] = sample.id;
  c->st->busy_until[slot] = c->b->tick + (uint32_t)ceilf(sample.seconds * (1000.f / TMUF_TICK_MS));
}

// A one-shot that the port does not restart while it plays (IsContinuous)
static void one_shot(const step_ctx *c, int slot, tm_sample sample, float volume) {
  if (!sample.id) return;
  const tmuf_sound *def = c->la->car[slot].def;
  if (def->continuous && busy(c, slot, sample)) return;
  set_busy(c, slot, sample);
  float pos[3];
  slot_position(c, slot, pos);
  add(c, sample.id, 0, volume, 1.f, 0.f, pos, def);
}

// CPlugSoundEngine through the software mix (CAudioSoundEngine_Mixer: the 6
// loudest of the components' channels), or one component as its own source
static void engine(const step_ctx *c, const tm_snd *s, uint32_t voice, float rpm, float accel, float alpha, float speed,
                   float volume, const float *position) {
  const tmuf_sound *def = s->def;
  if (!c->heard || !def || def->kind != TMUF_SOUND_ENGINE || !s->parts) return;
  bool negative = false;
  for (uint32_t i = 0; i < s->parts; i++)
    negative |= def->components[i].fade_in_end < 0.f;
  if (rpm < 0.f && !negative) rpm = -rpm;
  const float v = volume * def->volume * curve_at(&def->volume_speed, speed) * curve_at(&def->volume_rpm, rpm) *
                  curve_at(&def->volume_accel, accel);
  float w1 = fminf(fmaxf(1.f - alpha, 0.f), 1.f), w0 = fminf(fmaxf(1.f + alpha, 0.f), 1.f);
  const float sum = w0 + w1;
  w1 /= sum, w0 /= sum;
  struct {
    float volume, pitch;
    uint32_t part, channel;
  } kept[6];
  uint32_t n = 0;
  for (uint32_t i = 0; i < s->parts; i++) {
    float cv, cp;
    component_values(&def->components[i], rpm, &cv, &cp);
    if (s->parts == 1) {
      add(c, s->part[i][0].id, voice, v * cv, cp, 0.f, position, def);
      return;
    }
    if (cv <= 0.01f) cv = 0.f;
    for (uint32_t ch = 0; ch < 2; ch++) {
      const float ev = cv * (ch == 0 ? w1 : w0);
      if (ev <= 0.001f || !s->part[i][ch].id) continue;
      uint32_t at = 0;
      while (at < n && kept[at].volume >= ev)
        at++;
      if (at >= 6) continue;
      if (n < 6) n++;
      memmove(&kept[at + 1], &kept[at], (n - 1 - at) * sizeof *kept);
      kept[at].volume = ev, kept[at].pitch = cp, kept[at].part = i, kept[at].channel = ch;
    }
  }
  for (uint32_t k = 0; k < n; k++)
    add(c, s->part[kept[k].part][kept[k].channel].id, voice + kept[k].part * 2u + kept[k].channel, v * kept[k].volume,
        kept[k].pitch, 0.f, position, def);
}

// CGenAudioSoundSurface::Update: the impact, the current material's rolling
// and skid loops following the speed and the sliding, the others fading out
static void surface(const step_ctx *c, int slot, int index, uint32_t material, float speed, float skid, int impact) {
  const tm_snd *s = &c->la->car[slot];
  const tmuf_sound *def = s->def;
  if (!def || def->kind != TMUF_SOUND_SURFACE) return;
  const bool known = material < TMUF_SOUND_MATERIALS;
  float pos[3];
  slot_position(c, slot, pos);
  if (impact && known) {
    const tm_sample smp = s->impact[material][impact >= 2];
    const float att = impact >= 2 ? def->big_impact_attenuation : def->small_impact_attenuation;
    if (smp.id && !busy(c, slot, smp)) {
      set_busy(c, slot, smp);
      add(c, smp.id, 0, def->volume * att, 1.f, 0.f, pos, def);
    }
  }
  const float follow = c->fresh ? 1.f : (float)(1.0 - pow(0.7, 1.0 / PORT_TICKS));
  const float fade = (float)pow(0.8, 1.0 / PORT_TICKS);
  const uint8_t texture = known ? s->material_loop[material][0] : TM_NO_LOOP;
  const uint8_t sliding = known ? s->material_loop[material][1] : TM_NO_LOOP;
  float *gains = c->st->surface[index];
  for (uint32_t j = 0; j < s->loop_count; j++) {
    float pitch = 1.f;
    if (j == texture || j == sliding) {
      const tmuf_sound_component *comp = j == texture ? def->materials[material].texture : def->materials[material].skid;
      float cv;
      component_values(comp, j == texture ? speed : skid, &cv, &pitch);
      gains[j] += (cv - gains[j]) * follow;
    } else {
      gains[j] *= c->fresh ? 0.f : fade;
      if (gains[j] < 0.02f) gains[j] = 0.f;
    }
    add(c, s->loops[j].id, VOICE_SURFACE + (uint32_t)index * 0x100u + j, def->volume * gains[j], pitch, 0.f, pos, def);
  }
}

// CSceneVehicle::VisualUpdateAsync's spray (kind-2 emitter): a probe 50 m
// down from the top of the car's box finding water; its volume (car+0x1f4)
// by how near the water is and how fast the car goes
static float spray_of(const step_ctx *c) {
  const tmuf_world *w = &c->world->w;
  const float kmh = c->b->speed;
  // curve(30, 100 km/h) of every tuning (CSceneVehicleTuning +0x24)
  const float k = kmh <= 30.f ? 0.f : kmh >= 100.f ? 1.f : (kmh - 30.f) / 70.f;
  if (!c->la->water || k <= 1e-5f) return -1.f;
  const tmuf_box *box = &w->sim.def.water_box;
  const tmuf_iso4 pose = car_pose(c);
  float top[3];
  transform(&pose, (const float[3]){box->center.x, box->center.y + fabsf(box->half.y), box->center.z}, top);
  if (top[1] - 50.f > c->la->water_height + 1.f) return -1.f;
  const float seg[3] = {0.f, -50.f, 0.f};
  float t = 1.f;
  uint32_t material = UINT32_MAX;
  if (!tmuf_track_segment_hit(w->track, top, seg, &t, &material) || material != WATER) return -1.f;
  const float near = fminf(fmaxf(1.f - t, 0.f), 1.f);
  const float q = 0.25f * kmh;
  const float k2 = q <= 30.f ? 0.f : q >= 100.f ? 1.f : (q - 30.f) / 70.f;
  return near * near * k2;
}

static bool roaring(const tm_audio_frame *f) {
  return !f->free_wheeling && f->engine_state == 2 && !f->input_window_exceeded && f->gas > 0.9f && !f->in_water;
}
static bool braking(const tm_audio_frame *f) { return f->brake > 0.9f && !f->in_water; }

static void car_sounds(const step_ctx *c) {
  const tm_audio_frame *a = c->a, *b = c->b;
  const struct tm_level_audio *la = c->la;
  tm_audio *st = c->st;
  const tmuf_car *car = &c->world->w.sim.car;

  // the last wheel material (car+0x201): the physics' as wheels touch, Water
  // while spraying or after a splash
  const bool splash = b->splashes != a->splashes;
  bool touching = false;
  for (uint32_t i = 0; i < b->wheel_count; i++)
    touching |= b->wheel_contact[i];
  if (c->fresh || touching || b->last_wheel_material != a->last_wheel_material)
    st->wheel_material = b->last_wheel_material;
  const float spray = spray_of(c);
  if (spray >= 0.f || splash) st->wheel_material = WATER;
  const uint32_t body_material = splash ? WATER : b->last_body_material;

  // wheels: the contacts and sliding of both ticks, the material of the first
  uint32_t front_n = 0, rear_n = 0, front_slide = 0, rear_slide = 0;
  uint32_t front_mat = st->wheel_material, rear_mat = st->wheel_material;
  for (uint32_t i = 0; i < b->wheel_count; i++) {
    if (!(a->wheel_contact[i] && b->wheel_contact[i])) continue;
    const bool front = b->wheel_front[i];
    if (front) front_n++, front_mat = a->wheel_material[i];
    else rear_n++, rear_mat = a->wheel_material[i];
    if ((a->wheel_slipping[i] && b->wheel_slipping[i]) || b->burnout) {
      if (front) front_slide++;
      else rear_slide++;
    }
  }
  const float n = b->wheel_count ? (float)b->wheel_count : 1.f;
  const float kmh = fabsf(a->forward_speed * 3.6f);
  // in the air over water the rear sound's speed is the spray's volume
  const float rear_speed = rear_n ? kmh : rear_mat == WATER && spray > 0.f ? spray : 0.f;
  const bool wheels_impact = !b->in_water || st->wheel_material == WATER;
  surface(c, TMUF_CAR_SOUND_WHEEL_SURFACE, 0, rear_mat, rear_speed, rear_n ? (float)(front_slide + rear_slide) / n : 0.f,
          wheels_impact ? car->sound.rear_impact : 0);
  surface(c, TMUF_CAR_SOUND_WHEEL_SURFACE_FRONT, 1, front_mat, front_n ? kmh : 0.f,
          front_n ? (float)front_slide / n : 0.f, wheels_impact ? car->sound.front_impact : 0);
  const int body_impact = splash ? 2 : car->sound.body_impact;
  surface(c, TMUF_CAR_SOUND_BODY_SURFACE, 2, body_material, 0.f, 0.f,
          !b->in_water || body_material == WATER ? body_impact : 0);

  const tm_snd *roar = &la->car[TMUF_CAR_SOUND_ROAR];
  if (roar->def && roaring(b) && !roaring(a)) one_shot(c, TMUF_CAR_SOUND_ROAR, roar->sample, roar->def->volume);
  const tm_snd *lights = &la->car[TMUF_CAR_SOUND_BRAKE_LIGHTS];
  if (lights->def && braking(b) && !braking(a))
    one_shot(c, TMUF_CAR_SOUND_BRAKE_LIGHTS, lights->sample, lights->def->volume);
  // EnableTurbo: when the turbo's kind changes
  const tm_snd *turbo = &la->car[TMUF_CAR_SOUND_TURBO];
  if (turbo->def && b->turbo_start != a->turbo_start)
    one_shot(c, TMUF_CAR_SOUND_TURBO, turbo->sample, turbo->def->volume);
  if (!c->heard) return;

  // the engine: the transmission's rpm, negative in reverse; the throttle in
  // the direction of travel; the underwater channel while in water
  // (StartVehicleSounds starts it at volume 0: it fades in, ~0.67 s)
  float volume = 1.f - powf(0.985f, (float)b->tick);
  if (1.f - volume < 0.01f) volume = 1.f;
  const float rpm = b->free_wheeling ? 0.f : a->rpm * (b->reverse && b->gear == 0 ? -1.f : 1.f);
  const float accel = b->free_wheeling ? 0.f : b->local_vz >= 0.f ? b->gas : b->brake;
  float pos[3];
  slot_position(c, TMUF_CAR_SOUND_ENGINE, pos);
  engine(c, &la->car[TMUF_CAR_SOUND_ENGINE], VOICE_ENGINE, rpm, accel, b->in_water ? 1.f : -1.f, b->speed, volume, pos);
  // (SoundVibrations: nothing in the game ever starts it)

  const tm_snd *brakes = &la->car[TMUF_CAR_SOUND_BRAKES];
  if (brakes->def && !b->reverse && braking(b)) {
    slot_position(c, TMUF_CAR_SOUND_BRAKES, pos);
    add(c, brakes->sample.id, VOICE_BRAKES, brakes->def->volume, 1.f, 0.f, pos, brakes->def);
  }
  // every shift, by gear: {0, 0, 1, 1, 2, 3, 4, 5, 6}, clamped to the variants
  const tm_snd *gears = &la->car[TMUF_CAR_SOUND_GEAR_CHANGE];
  if (gears->def && gears->parts && b->gear != a->gear && !b->in_water) {
    static const uint32_t variant_of[9] = {0, 0, 1, 1, 2, 3, 4, 5, 6};
    uint32_t v = b->gear >= 0 && b->gear < 9 ? variant_of[b->gear] : 0;
    if (v >= gears->parts) v = gears->parts - 1;
    slot_position(c, TMUF_CAR_SOUND_GEAR_CHANGE, pos);
    add(c, gears->part[v][0].id, 0, gears->def->volume, 1.f, 0.f, pos, gears->def);
  }
}

// CTrackManiaRace::UpdateListener's woosh: probes up and to both sides of
// the listener find walls close around it (a tunnel) while it moves fast.
// The listener is the race camera, which follows the car: the car's place,
// axes and speed stand for it. The game probes every frame; here every 4th
// tick (by the world's clock, so every world probes the same ticks), and the
// sides only under a roof (the sum needs the probe up to hit).
#define WOOSH_PROBE_TICKS 4
static float woosh_target(const step_ctx *c) {
  const tmuf_dyna_state *s = &c->world->w.sim.body.state;
  const float v2 = s->lin.x * s->lin.x + s->lin.y * s->lin.y + s->lin.z * s->lin.z;
  const float over = v2 / 434.0278f - 1.f; // above 75 km/h
  if (over <= 0.f) return 0.f;
  float axis[3][3];
  for (int k = 0; k < 3; k++)
    for (int r = 0; r < 3; r++)
      axis[k][r] = s->rot.m[r][k];
  const float p[3] = {s->pos.x, s->pos.y, s->pos.z};
  float seg[3][3];
  for (int r = 0; r < 3; r++) {
    seg[0][r] = 100.f * (1.2f * axis[1][r] + 0.1f * axis[2][r]);
    seg[1][r] = 100.f * (axis[0][r] + 0.2f * axis[1][r] + 0.1f * axis[2][r]);
    seg[2][r] = -seg[1][r];
  }
  float sum = 0.f;
  for (int i = 0; i < 3; i++) {
    float t = 2.f;
    if (!tmuf_track_segment_cast(c->world->w.track, p, seg[i], &t)) t = 2.f;
    sum += t;
    if (i == 0 && t >= 2.f) return 0.f; // (the sides make at most 2 more)
  }
  return sum < 2.f ? 0.2f / fmaxf(sum, 0.2f) * over : 0.f;
}

static void woosh(const step_ctx *c) {
  const struct tm_level_audio *la = c->la;
  tm_audio *st = c->st;
  const bool was_high = st->woosh_target > 0.25f;
  if (c->fresh || c->b->tick % WOOSH_PROBE_TICKS == 0) st->woosh_target = woosh_target(c);
  const bool high = st->woosh_target > 0.25f;
  const float dt = (float)TMUF_TICK_MS / 1000.f;
  st->woosh = fminf(fmaxf(c->fresh ? st->woosh_target : st->woosh + (st->woosh_target - st->woosh) * dt, 0.f), 1.f);
  if (la->woosh.def && st->woosh > 0.01f)
    add(c, la->woosh.sample.id, VOICE_WOOSH, la->woosh.def->volume * st->woosh, 1.f, 0.f, NULL, NULL);
  if (!c->fresh && high != was_high) {
    const tm_snd *edge = high ? &la->woosh_start : &la->woosh_end;
    if (edge->def) add(c, edge->sample.id, 0, edge->def->volume, 1.f, 0.f, NULL, NULL);
  }
}

// the medal the run's finish earned (the end race dialog's jingle)
static int medal_of(const step_ctx *c) {
  const ft_level *level = c->world->level;
  const tmuf_race *race = &c->world->w.sim.race;
  const tmuf_challenge_info *info = level ? level->info : NULL;
  if (!info || race->stunts_mode) return MEDAL_NONE;
  const uint32_t t = race->finish_time;
  return t <= info->author_time ? MEDAL_NADEO
         : t <= info->gold      ? MEDAL_GOLD
         : t <= info->silver    ? MEDAL_SILVER
         : t <= info->bronze    ? MEDAL_BRONZE
                                : MEDAL_NONE;
}

static void race_sounds(const step_ctx *c) {
  const tm_audio_frame *a = c->a, *b = c->b;
  const struct tm_level_audio *la = c->la;
  // the countdown's thirds from the spawn (Race1.wav first), then RaceGo at
  // the start: only when the step crosses them
  const double from = (double)a->tick * TMUF_TICK_MS, to = (double)b->tick * TMUF_TICK_MS;
  for (int k = 0; k <= 3 && to > from; k++) {
    const double at = (double)TMUF_RACE_START_MS * k / 3.0;
    if (at < from || at >= to) continue;
    const tm_snd *s = k < 3 ? &la->countdown[k] : &la->go;
    if (s->def) add(c, s->sample.id, 0, s->def->volume, 1.f, (float)((at - from) / (to - from)), NULL, NULL);
  }
  // a checkpoint, or the finish line of a lap that is not the last
  // (InternalOnCheckpoint): the block's own sound where it stands, else
  // the interface's RaceCheckPoint (variant 0); the race's end plays the
  // medal's jingle
  const bool finished = b->completed && !a->completed;
  const int32_t corpus = c->world->w.sim.car.sound.checkpoint_corpus;
  if (b->checkpoints > a->checkpoints && !finished) {
    const tmuf_scene_sound *own = tmuf_track_trigger_sound(la->track, corpus);
    const uint32_t i = own ? (uint32_t)(own - la->scene) : UINT32_MAX;
    if (i < la->scene_count && la->scene_sample[i].id) {
      float pos[3];
      transform(&own->location, (const float[3]){0.f, 0.f, 0.f}, pos);
      add(c, la->scene_sample[i].id, 0, own->sound->volume * own->volume, own->pitch, 0.f, pos, own->sound);
    } else if (la->checkpoint.def) {
      add(c, la->checkpoint.sample.id, 0, la->checkpoint.def->volume, 1.f, 0.f, NULL, NULL);
    }
  }
  if (finished) {
    const tm_snd *m = &la->medal[medal_of(c)];
    if (m->def) add(c, m->sample.id, 0, m->def->volume, 1.f, 0.f, NULL, NULL);
  }
  // a stunt figure scored (Stunts mode)
  if (c->world->w.sim.race.stunts_mode && b->stunt_score > a->stunt_score && la->figure.def)
    add(c, la->figure.sample.id, 0, la->figure.def->volume, 1.f, 0.f, NULL, NULL);

  // ListenerWind: the listener's speed in km/h as an engine's rpm, from 70
  // km/h (the race camera's, which is the car's here)
  engine(c, &la->wind, VOICE_WIND, b->speed, 0.f, 0.f, 0.f, 1.f, NULL);
}

// The world's own: the decoration's ambience, the scenery's sources that are
// on (Stadium's turbines, Rally's windmills) and the music
static void ambient_sounds(const step_ctx *c) {
  const struct tm_level_audio *la = c->la;
  if (la->ambience.def)
    clocked(c, add(c, la->ambience.sample.id, VOICE_AMBIENCE, la->ambience.def->volume, 1.f, 0.f, NULL, NULL), 0.f);
  // only the ones within reach of the car (whose camera hears them)
  const tmuf_dyna_state *s = &c->world->w.sim.body.state;
  const float car[3] = {s->pos.x, s->pos.y, s->pos.z};
  for (uint32_t i = 0; i < la->scene_count; i++) {
    const tmuf_scene_sound *ss = &la->scene[i];
    if (!ss->on || !ss->sound || !la->scene_sample[i].id) continue;
    const tmuf_sound *def = ss->sound;
    const float ref = def->ref_distance > 0.f ? def->ref_distance : 1.f;
    const float reach = def->mode == TMUF_SOUND_3D_OMNI && def->max_distance_omni > ref ? def->max_distance_omni : 50.f * ref;
    // the nearest point of its box (VolumicSize)
    const tmuf_iso4 *l = &ss->location;
    float local[3], pos[3];
    for (int k = 0; k < 3; k++) {
      const float d = (car[0] - l->t.x) * l->r.m[0][k] + (car[1] - l->t.y) * l->r.m[1][k] + (car[2] - l->t.z) * l->r.m[2][k];
      local[k] = fminf(fmaxf(d, -ss->volumic_size[k]), ss->volumic_size[k]) * 0.98f;
    }
    transform(l, local, pos);
    const float dx = pos[0] - car[0], dy = pos[1] - car[1], dz = pos[2] - car[2];
    if (def->mode >= TMUF_SOUND_3D && dx * dx + dy * dy + dz * dz >= reach * reach) continue;
    clocked(c, add(c, la->scene_sample[i].id, VOICE_SCENE + i, def->volume * ss->volume, ss->pitch, 0.f, pos, def),
            (float)i * 1.37f);
  }
  // the music: heard as the Music setting says (tm_audio_spatialize)
  if (la->music.id) {
    const float t = (float)c->b->tick * TMUF_TICK_MS / 1000.f;
    ft_audio_sound *m = add(c, la->music.id, VOICE_MUSIC, 0.9f * fminf(t / 0.75f, 1.f), 1.f, 0.f, NULL, NULL);
    if (m) {
      m->distance[0] = m->distance[1] = -1.f;
      clocked(c, m, 0.f);
    }
  }
}

void tm_audio_step(ft_game *game, ft_world *world, const tm_audio_frame *before) {
  tm_audio *st = &world->audio;
  const ft_engine_api *api = game->engine;
  bool heard = api->audio_heard && api->audio_heard();
  if (heard && !st->sounds && !(st->sounds = malloc(TM_AUDIO_SOUNDS * sizeof *st->sounds))) heard = false;
  tm_audio_frame after;
  frame_of(&world->w, &after);
  const step_ctx c = {game, world, world->level->audio, st, before, &after, st->tick != before->tick, heard};
  if (c.fresh) {
    st->woosh = st->woosh_target = 0.f;
    memset(st->surface, 0, sizeof st->surface);
    memset(st->busy_until, 0, sizeof st->busy_until);
    memset(st->busy_sample, 0, sizeof st->busy_sample);
  }
  st->count = 0;
  car_sounds(&c);
  woosh(&c);
  if (heard) {
    race_sounds(&c);
    ambient_sounds(&c);
  }
  st->tick = after.tick;
  st->heard_tick = heard ? after.tick : UINT32_MAX;
}

bool tm_world_audio(ft_game *game, const ft_world *world, ft_audio_step *out) {
  (void)game;
  if (world->audio.heard_tick != world->w.tick || !world->audio.count) return false;
  out->sounds = world->audio.sounds;
  out->sound_count = world->audio.count;
  return true;
}

// --- events ------------------------------------------------------------------

// The horn (CSceneVehicle::VehicleHorn): a replay's presses, kept as the
// timeline's events (tm_recording_events)
bool tm_event_audio(ft_game *game, const ft_timeline_event *event, ft_audio_sound *out) {
  const struct tm_level_audio *la = game->level ? game->level->audio : NULL;
  if (!la || !event->category || strcmp(event->category, TM_EVENT_HORN) != 0) return false;
  const tm_snd *horn = &la->car[TMUF_CAR_SOUND_HORN];
  if (!horn->def || !horn->sample.id) return false;
  *out = (ft_audio_sound){.sample = horn->sample.id, .flags = FT_AUDIO_POSITIONED, .volume = horn->def->volume, .pitch = 1.f};
  return true;
}

// --- hearing -----------------------------------------------------------------

void tm_audio_listen(ft_game *game, const ft_camera *camera) {
  // the view's right: the clip x row of its (column-major) view-projection
  const float *m = camera->view_proj;
  const float r[3] = {m[0], m[4], m[8]};
  const float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (len <= 0.f) return;
  game->listener.valid = true;
  game->listener.eye[0] = camera->eye.x, game->listener.eye[1] = camera->eye.y, game->listener.eye[2] = camera->eye.z;
  for (int i = 0; i < 3; i++)
    game->listener.right[i] = r[i] / len;
}

// The Sound settings' volume, then for a placed sound OpenAL's inverse distance
// (clamped from the reference distance), silent beyond the sound's range,
// panned by how far to the side of the view it is. The music (distance -1)
// and the 2D sounds (0) are not placed.
void tm_audio_spatialize(ft_game *game, int32_t world_index, const ft_audio_sound *sound, float gain[2]) {
  (void)world_index;
  if (sound->distance[0] < 0.f) {
    gain[0] = gain[1] = game->settings.music ? (float)game->settings.music_volume / 100.f : 0.f;
    return;
  }
  const float sfx = (float)game->settings.sfx_volume / 100.f;
  gain[0] = gain[1] = sfx;
  if (sound->distance[0] == 0.f || !game->listener.valid) return;
  float d[3];
  for (int i = 0; i < 3; i++)
    d[i] = sound->position[i] - game->listener.eye[i];
  const float distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  const float ref = sound->distance[0] > 0.f ? sound->distance[0] : 1.f;
  if (sound->distance[1] > 0.f && distance >= sound->distance[1]) {
    gain[0] = gain[1] = 0.f;
    return;
  }
  const float g = distance > ref ? ref / distance : 1.f;
  float pan = 0.f;
  if (distance > 1e-3f) {
    const float r = game->listener.right[0] * d[0] + game->listener.right[1] * d[1] + game->listener.right[2] * d[2];
    pan = r / distance * fminf(distance, 1.f); // (no swing as it passes through the head)
  }
  gain[0] = sfx * g * sqrtf(fmaxf(1.f - pan, 0.f));
  gain[1] = sfx * g * sqrtf(fmaxf(1.f + pan, 0.f));
}
