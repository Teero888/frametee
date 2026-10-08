// Sound: DDNet's sound sets, played where the physics raised them and heard
// from the camera as DDNet's own mixer hears them.

#include "dd_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Each set is a few takes of one sound (data/games/ddnet/audio/<name>-NN.flac,
// or <name>.flac for a set of one), by the protocol's sound id. A set may draw
// on two names: the jump's footsteps are left and right feet.
typedef struct sound_set {
  const char *name;
  int takes;
  const char *name2;
  int takes2;
} sound_set_t;

static const sound_set_t kSets[DD_SOUND_SETS] = {
    {"wp_gun_fire", 3, NULL, 0},
    {"wp_shotty_fire", 3, NULL, 0},
    {"wp_flump_launch", 3, NULL, 0},
    {"wp_hammer_swing", 3, NULL, 0},
    {"wp_hammer_hit", 3, NULL, 0},
    {"wp_ninja_attack", 3, NULL, 0},
    {"wp_flump_explo", 3, NULL, 0},
    {"wp_ninja_hit", 3, NULL, 0},
    {"wp_laser_fire", 3, NULL, 0},
    {"wp_laser_bnce", 3, NULL, 0},
    {"wp_switch", 3, NULL, 0},
    {"vo_teefault_pain_short", 12, NULL, 0},
    {"vo_teefault_pain_long", 2, NULL, 0},
    {"foley_land", 4, NULL, 0},
    {"foley_dbljump", 3, NULL, 0},
    {"foley_foot_left", 4, "foley_foot_right", 4},
    {"foley_body_splat", 3, NULL, 0},
    {"vo_teefault_spawn", 7, NULL, 0},
    {"sfx_skid", 4, NULL, 0},
    {"vo_teefault_cry", 2, NULL, 0},
    {"hook_loop", 2, NULL, 0},
    {"hook_attach", 3, NULL, 0},
    {"foley_body_impact", 3, NULL, 0},
    {"hook_noattach", 2, NULL, 0},
    {"sfx_pickup_hrt", 2, NULL, 0},
    {"sfx_pickup_arm", 4, NULL, 0},
    {"sfx_pickup_launcher", 0, NULL, 0},
    {"sfx_pickup_sg", 0, NULL, 0},
    {"sfx_pickup_ninja", 0, NULL, 0},
    {"sfx_spawn_wpn", 3, NULL, 0},
    {"wp_noammo", 5, NULL, 0},
    {"sfx_hit_weak", 2, NULL, 0},
    {"sfx_msg-server", 0, NULL, 0},
    {"sfx_msg-client", 0, NULL, 0},
    {"sfx_msg-highlight", 0, NULL, 0},
    {"sfx_ctf_drop", 0, NULL, 0},
    {"sfx_ctf_rtn", 0, NULL, 0},
    {"sfx_ctf_grab_pl", 0, NULL, 0},
    {"sfx_ctf_grab_en", 0, NULL, 0},
    {"sfx_ctf_cap_pl", 0, NULL, 0},
};

// DDNet's world channel plays at 0.9, and a positioned sound fades out over
// 1500 units from the listener.
#define WORLD_VOLUME 0.9f
#define SOUND_RANGE 1500.f

static void load_takes(ft_game *game, int set, const char *name, int takes) {
  const ft_engine_api *engine = game->engine;
  for (int take = 0; take < (takes ? takes : 1) && game->sound_take_count[set] < DD_SOUND_TAKES; ++take) {
    char relative[96], path[1024];
    if (takes) snprintf(relative, sizeof(relative), "audio/%s-%02d.flac", name, take + 1);
    else snprintf(relative, sizeof(relative), "audio/%s.flac", name);
    engine->resolve_data_path(relative, path, sizeof(path));
    const ft_audio_sample sample = engine->audio_sample_load(path);
    if (sample) game->sound_samples[set][game->sound_take_count[set]++] = sample;
    else dd_log(game, FT_LOG_WARN, "Missing sound %s", relative);
  }
}

void dd_audio_load(ft_game *game) {
  for (int set = 0; set < DD_SOUND_SETS; ++set) {
    load_takes(game, set, kSets[set].name, kSets[set].takes);
    if (kSets[set].name2) load_takes(game, set, kSets[set].name2, kSets[set].takes2);
  }
}

void dd_audio_unload(ft_game *game) {
  for (int set = 0; set < DD_SOUND_SETS; ++set) {
    for (int take = 0; take < game->sound_take_count[set]; ++take)
      game->engine->audio_sample_destroy(game->sound_samples[set][take]);
    game->sound_take_count[set] = 0;
  }
}

// DDNet picks a take at random. Here the pick is a hash of the moment, so a
// tick sounds the same every time it is heard.
static ft_audio_sample pick_take(const ft_game *game, int set, int tick, int client_id, int index) {
  if (set < 0 || set >= DD_SOUND_SETS || game->sound_take_count[set] == 0) return 0;
  uint32_t h = (uint32_t)tick * 0x9E3779B1u ^ (uint32_t)(client_id + 1) * 0x85EBCA77u ^ (uint32_t)set * 0xC2B2AE3Du ^
               (uint32_t)index * 0x27D4EB2Fu;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return game->sound_samples[set][h % (uint32_t)game->sound_take_count[set]];
}

static ft_audio_sound positioned(ft_audio_sample sample, float x, float y) {
  return (ft_audio_sound){.sample = sample,
                          .flags = FT_AUDIO_POSITIONED,
                          .volume = WORLD_VOLUME,
                          .pitch = 1.f,
                          .position = {x, y, 0.f}};
}

// The sound DDNet's client plays with an effect (CEffects::HammerHit, PlayerSpawn, AirJump), or -1. A
// replayed tee's air jumps are heard from the recording's own reconstruction.
static int effect_sound(const ft_world *world, const dd_physics_particle_event_t *event) {
  switch (event->type) {
  case DDNET_PARTICLE_HAMMER_HIT: return DDNET_SOUND_HAMMER_HIT;
  case DDNET_PARTICLE_PLAYER_SPAWN: return DDNET_SOUND_PLAYER_SPAWN;
  case DDNET_PARTICLE_AIR_JUMP: return dd_replay_active(world, event->client_id) ? -1 : DDNET_SOUND_PLAYER_AIRJUMP;
  default: return -1;
  }
}

bool dd_world_audio(ft_game *game, const ft_world *world, ft_audio_step *out) {
  if (!world) return false;
  // The step's sounds, and those DDNet's client plays with an effect, for
  // which the physics raises no sound: hammer hits, spawns and air jumps.
  int count = world->physics_sound_event_count;
  for (int i = 0; i < world->physics_particle_event_count; ++i)
    if (effect_sound(world, &world->physics_particle_events[i]) >= 0) ++count;
  if (count == 0) return false;

  // The list lives with the world, which only its own thread steps.
  ft_world *mutable_world = (ft_world *)world;
  if (count > mutable_world->audio_sound_capacity) {
    ft_audio_sound *grown = realloc(mutable_world->audio_sounds, (size_t)count * sizeof(*grown));
    if (!grown) return false;
    mutable_world->audio_sounds = grown;
    mutable_world->audio_sound_capacity = count;
  }
  ft_audio_sound *sounds = mutable_world->audio_sounds;
  const int tick = ddnet_engine_tick(world);
  uint32_t n = 0;
  for (int i = 0; i < world->physics_sound_event_count; ++i) {
    const dd_physics_sound_event_t *event = &world->physics_sound_events[i];
    const ft_audio_sample sample = pick_take(game, event->sound_id, tick, event->client_id, i);
    if (sample) sounds[n++] = positioned(sample, event->x, event->y);
  }
  for (int i = 0; i < world->physics_particle_event_count; ++i) {
    const dd_physics_particle_event_t *event = &world->physics_particle_events[i];
    const int sound = effect_sound(world, event);
    if (sound < 0) continue;
    const ft_audio_sample sample = pick_take(game, sound, tick, event->client_id, i + 64);
    if (sample) sounds[n++] = positioned(sample, event->x, event->y);
  }
  out->sounds = sounds;
  out->sound_count = n;
  return n > 0;
}

// DDNet's client plays a global sound message as it is, and a chat line with
// the server's or the players' chat sound (a whisper sent makes none).
#define SOUND_CHAT_SERVER 32
#define SOUND_CHAT_CLIENT 33
#define CHAT_TEAM_WHISPER_SEND 2

bool dd_event_audio(ft_game *game, const ft_timeline_event *event, ft_audio_sound *out) {
  dd_event_payload_t payload;
  if (!dd_event_decode(event, &payload)) return false;
  int set;
  switch ((dd_event_type_t)payload.type) {
  case DD_EVENT_SOUND_GLOBAL:
    set = payload.sound_id;
    break;
  case DD_EVENT_CHAT:
    if (payload.team == CHAT_TEAM_WHISPER_SEND) return false;
    set = payload.client_id < 0 ? SOUND_CHAT_SERVER : SOUND_CHAT_CLIENT;
    break;
  default:
    return false;
  }
  const ft_audio_sample sample = pick_take(game, set, event->tick, payload.client_id, 0);
  if (!sample) return false;
  *out = (ft_audio_sound){.sample = sample, .volume = 1.f, .pitch = 1.f};
  return true;
}

void dd_audio_listen(ft_game *game, const ft_rect *visible) {
  game->listener[0] = (visible->x + visible->w * 0.5f) * 32.f;
  game->listener[1] = (visible->y + visible->h * 0.5f) * 32.f;
  game->listener_set = true;
}

// DDNet's mixer: a sound fades out linearly to nothing at the range, and the
// ear away from it hears less by how far it is to the side.
void dd_audio_spatialize(ft_game *game, int32_t world_index, const ft_audio_sound *sound, float gain[2]) {
  (void)world_index;
  gain[0] = gain[1] = 1.f;
  if (!game->listener_set) return;
  const float dx = sound->position[0] - game->listener[0];
  const float dy = sound->position[1] - game->listener[1];
  const float distance = sqrtf(dx * dx + dy * dy);
  if (distance >= SOUND_RANGE) {
    gain[0] = gain[1] = 0.f;
    return;
  }
  const float falloff = (SOUND_RANGE - distance) / SOUND_RANGE;
  const float side = (SOUND_RANGE - fabsf(dx)) / SOUND_RANGE;
  gain[0] = falloff * (dx > 0.f ? side : 1.f);
  gain[1] = falloff * (dx > 0.f ? 1.f : side);
}
