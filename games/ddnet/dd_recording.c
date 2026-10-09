// DDNet demos as FrameTee recordings.
//
// The demo library rebuilds every tick of a demo, which is all a recording needs: a recorded player's
// state at each tick. A replayed tee stays an ordinary character of the world: the physics steps it
// with the input it most likely held, so everything the recording does not carry (race timer, tiles
// it crossed, pickups) keeps up, and afterwards its recorded state is written over it. Simulated tees
// collide with it and hook it like with anyone else. It never fires and cannot hook players, and
// whatever a simulated tee does to it is overwritten, so nothing reaches it; the effects its own
// physics step raises are dropped, since the recording brings the real ones.
#include "dd_internal.h"
#include "dd_profile.h"
#include "dd_threads.h"

#include <ddnet_demo/ddnet_demo.h>
#include <ddnet_demo/ddnet_demo_state.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Beside ft_recording_tick_flags in a player's flags, never handed out: paused with /spec, when the
// character is out of the game and the demo only has where it waits.
#define TICK_PAUSED (1u << 7)

typedef struct {
  int cid;
  int first_tick, last_tick;
  char name[17];
  dd_player_profile_t profile;
  // ft_recording_tick_flags and TICK_PAUSED from first_tick to the recording's last tick
  uint8_t *flags;
} dd_recorded_player;

struct ft_recording {
  dd_demo_state *state;
  // The library's queries share caches, and worlds step on several threads.
  pthread_mutex_t lock;
  char name[128];
  char level_name[64];
  int first_tick, last_tick;
  int local_cid;
  dd_recorded_player players[DD_STATE_MAX_CLIENTS];
  int player_count;
  int player_of_cid[DD_STATE_MAX_CLIENTS]; // -1 for clients never in the recording

  // The last tick's characters, since every replayed player of a step asks for the same tick.
  int cached_tick;
  bool cache_valid;
  dd_state_character cached[DD_STATE_MAX_CLIENTS];
};

// A position in the demo, which is one in the world: both are DDNet's.
static ddnet_vec2_t world_pos(float x, float y) { return (ddnet_vec2_t){x, y}; }

// --- opening -------------------------------------------------------------------

static uint64_t fnv1a(const void *data, size_t size) {
  const uint8_t *bytes = data;
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// The library reads demos from files: the bytes go to a file of their own for the load, which is
// removed again afterwards. The reconstruction it keeps sits beside it, under the demo's hash.
static bool write_demo_file(const char *path, const void *data, size_t size) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  const bool wrote = fwrite(data, 1, size, file) == size;
  if (fclose(file) != 0 || !wrote) {
    remove(path);
    return false;
  }
  return true;
}

typedef struct {
  bool (*progress)(void *user, float fraction);
  void *user;
} progress_scale;

// Loading is most of the work; walking every tick for the player list is the rest.
static bool on_load_progress(void *user, float fraction) {
  const progress_scale *scale = user;
  return !scale->progress || scale->progress(scale->user, fraction * 0.9f);
}

static void profile_from_player(const dd_state_player *player, dd_player_profile_t *out) {
  dd_profile_default(out);
  snprintf(out->name, sizeof(out->name), "%s", player->name);
  snprintf(out->clan, sizeof(out->clan), "%s", player->clan);
  if (player->skin[0]) snprintf(out->skin, sizeof(out->skin), "%s", player->skin);
  out->use_custom_color = player->use_custom_color != 0;
  out->color_body = (uint32_t)player->color_body & 0xFFFFFFu;
  out->color_feet = (uint32_t)player->color_feet & 0xFFFFFFu;
}

// A client paused with /spec at a tick, and where it waits (demo units).
static bool paused_at(dd_demo_state *state, int tick, int cid, float *x, float *y) {
  dd_state_player player;
  if (!dd_demo_state_player(state, tick, cid, &player) || !player.connected || !player.spec_char) return false;
  if (x) *x = (float)player.spec_x;
  if (y) *y = (float)player.spec_y;
  return true;
}

// Who is in the recording and when, and how well each tick was rebuilt. Paused players count as
// in it, so a pause neither ends a player nor keeps one that starts paused out of the list.
static bool scan_players(ft_recording *recording, const progress_scale *scale) {
  dd_demo_state *state = recording->state;
  const int ticks = recording->last_tick - recording->first_tick + 1;
  dd_state_character *chars = malloc(sizeof(*chars) * DD_STATE_MAX_CLIENTS);
  if (!chars) return false;
  for (int tick = recording->first_tick; tick <= recording->last_tick; ++tick) {
    if (scale->progress && (tick - recording->first_tick) % 2048 == 0 &&
        !scale->progress(scale->user, 0.9f + 0.1f * (float)(tick - recording->first_tick) / (float)ticks)) {
      free(chars);
      return false;
    }
    dd_demo_state_characters(state, tick, chars);
    for (int cid = 0; cid < DD_STATE_MAX_CLIENTS; ++cid) {
      const bool present = chars[cid].quality != DD_QUALITY_NONE;
      if (!present && !paused_at(state, tick, cid, NULL, NULL)) continue;
      int index = recording->player_of_cid[cid];
      if (index < 0) {
        index = recording->player_count++;
        recording->player_of_cid[cid] = index;
        dd_recorded_player *player = &recording->players[index];
        player->cid = cid;
        player->first_tick = tick;
        player->flags = calloc((size_t)(recording->last_tick - tick + 1), 1);
        if (!player->flags) {
          free(chars);
          return false;
        }
      }
      dd_recorded_player *player = &recording->players[index];
      player->last_tick = tick;
      player->flags[tick - player->first_tick] =
          !present ? TICK_PAUSED
                   : FT_RECORDING_TICK_PRESENT | (chars[cid].quality == DD_QUALITY_APPROXIMATED ? FT_RECORDING_TICK_APPROXIMATED : 0);
    }
  }
  free(chars);

  // Names and looks as each player first appeared.
  dd_state_tick *tick = malloc(sizeof(*tick));
  if (!tick) return false;
  for (int i = 0; i < recording->player_count; ++i) {
    dd_recorded_player *player = &recording->players[i];
    dd_demo_state_get(state, player->first_tick, tick);
    snprintf(player->name, sizeof(player->name), "%s", tick->players[player->cid].name);
    if (!player->name[0]) snprintf(player->name, sizeof(player->name), "Player %d", player->cid);
    profile_from_player(&tick->players[player->cid], &player->profile);
    if (recording->local_cid < 0) recording->local_cid = tick->local_client_id;
  }
  free(tick);
  return true;
}

ft_recording *dd_recording_open(ft_game *game, const void *data, size_t size, const char *name,
                                bool (*progress)(void *user, float fraction), void *progress_user, char *error,
                                size_t error_size) {
  char demo_path[1100], cache_path[1100];
  const unsigned long long hash = (unsigned long long)fnv1a(data, size);
  // Private to this load: the same demo may be opening twice at once.
  snprintf(demo_path, sizeof(demo_path), "%sdemo-%016llx-%p.demo", game->cache_dir, hash, (const void *)&demo_path);
  snprintf(cache_path, sizeof(cache_path), "%sdemo-%016llx.ddrc", game->cache_dir, hash);
  if (!write_demo_file(demo_path, data, size)) {
    snprintf(error, error_size, "could not write '%s'", demo_path);
    return NULL;
  }

  ft_recording *recording = calloc(1, sizeof(*recording));
  if (!recording) {
    snprintf(error, error_size, "out of memory");
    return NULL;
  }
  progress_scale scale = {progress, progress_user};
  const dd_state_load_options options = {on_load_progress, &scale, cache_path};
  char load_error[256] = "";
  recording->state = dd_demo_state_load_ex(demo_path, &options, load_error, sizeof(load_error));
  remove(demo_path);
  if (!recording->state) {
    snprintf(error, error_size, "%s", load_error[0] ? load_error : "the demo could not be read");
    free(recording);
    return NULL;
  }
  pthread_mutex_init(&recording->lock, NULL);
  snprintf(recording->name, sizeof(recording->name), "%s", name ? name : "demo");
  snprintf(recording->level_name, sizeof(recording->level_name), "%s", dd_demo_state_map_name(recording->state));
  recording->first_tick = dd_demo_state_first_tick(recording->state);
  recording->last_tick = dd_demo_state_last_tick(recording->state);
  recording->local_cid = -1;
  for (int i = 0; i < DD_STATE_MAX_CLIENTS; ++i) recording->player_of_cid[i] = -1;
  if (!scan_players(recording, &scale)) {
    snprintf(error, error_size, progress && !progress(progress_user, 0.9f) ? "cancelled" : "out of memory");
    dd_recording_destroy(game, recording);
    return NULL;
  }
  if (progress) progress(progress_user, 1.f);
  return recording;
}

void dd_recording_destroy(ft_game *game, ft_recording *recording) {
  (void)game;
  if (!recording) return;
  for (int i = 0; i < recording->player_count; ++i) free(recording->players[i].flags);
  dd_demo_state_free(recording->state);
  pthread_mutex_destroy(&recording->lock);
  free(recording);
}

// --- what the editor asks ------------------------------------------------------

bool dd_recording_info(ft_game *game, const ft_recording *recording, ft_recording_info *out) {
  (void)game;
  size_t level_size = 0;
  const uint8_t *level = dd_demo_state_map(recording->state, &level_size);
  out->name = recording->name;
  out->first_tick = recording->first_tick;
  out->last_tick = recording->last_tick;
  out->player_count = (uint32_t)recording->player_count;
  out->level_data = level;
  out->level_size = level_size;
  out->level_name = recording->level_name;
  out->ticks_are_states = true;
  return true;
}

bool dd_recording_player(ft_game *game, const ft_recording *recording, uint32_t index, ft_recording_player *out) {
  (void)game;
  if (index >= (uint32_t)recording->player_count) return false;
  const dd_recorded_player *player = &recording->players[index];
  out->name = player->name;
  out->first_tick = player->first_tick;
  out->last_tick = player->last_tick;
  out->suggested = player->cid == recording->local_cid;
  out->profile = &player->profile;
  out->profile_size = sizeof(player->profile);
  // The body colour, as the tee would be drawn; DDNet's default skin is a warm brown.
  float rgb[3] = {0.69f, 0.49f, 0.33f};
  if (player->profile.use_custom_color) dd_hsl_to_rgb(player->profile.color_body, rgb);
  out->has_color = true;
  out->color = (uint32_t)(rgb[0] * 255.f + 0.5f) << 16 | (uint32_t)(rgb[1] * 255.f + 0.5f) << 8 | (uint32_t)(rgb[2] * 255.f + 0.5f);
  return true;
}

bool dd_recording_level_matches(ft_game *game, const ft_recording *recording, const ft_level *level) {
  (void)game;
  size_t size = 0;
  const uint8_t *demo_map = dd_demo_state_map(recording->state, &size);
  const map_data_t *map = &level->map;
  return demo_map && map->_map_file_data && map->_map_file_size == size && memcmp(map->_map_file_data, demo_map, size) == 0;
}

void dd_recording_tick_flags(ft_game *game, const ft_recording *recording, int32_t player, int32_t first_tick, uint32_t count,
                             uint8_t *out) {
  (void)game;
  if (player < 0 || player >= recording->player_count) return;
  const dd_recorded_player *p = &recording->players[player];
  for (uint32_t i = 0; i < count; ++i) {
    const int tick = first_tick + (int)i;
    out[i] = tick >= p->first_tick && tick <= recording->last_tick ? p->flags[tick - p->first_tick] & ~TICK_PAUSED : 0;
  }
}

// Every character at a tick, into `out`. Callers hold the lock.
static const dd_state_character *characters_at(ft_recording *recording, int tick) {
  if (!recording->cache_valid || recording->cached_tick != tick) {
    dd_demo_state_characters(recording->state, tick, recording->cached);
    recording->cached_tick = tick;
    recording->cache_valid = true;
  }
  return recording->cached;
}

// The aim, in DDNet's target coordinates, as DDNet's demo player draws it: interpolated between
// snapshots, since aim only changes at them. Stepping tick by tick through the recorded values
// instead moves it in bursts.
static void recorded_target(const dd_state_character *c, int *x, int *y) {
  if (c->aim_x != 0.f || c->aim_y != 0.f) {
    *x = (int)lroundf(c->aim_x);
    *y = (int)lroundf(c->aim_y);
  } else if (c->has_ddnet_info && (c->target_x || c->target_y)) {
    *x = c->target_x;
    *y = c->target_y;
  } else {
    const float angle = (float)c->angle / 256.f;
    *x = (int)lroundf(cosf(angle) * 256.f);
    *y = (int)lroundf(sinf(angle) * 256.f);
  }
  if (*x < INT16_MIN) *x = INT16_MIN;
  if (*x > INT16_MAX) *x = INT16_MAX;
  if (*y < INT16_MIN) *y = INT16_MIN;
  if (*y > INT16_MAX) *y = INT16_MAX;
}

static uint8_t eyes_for_emote(int emote) {
  switch (emote) {
  case DD_EMOTE_PAIN: return EYE_PAIN;
  case DD_EMOTE_HAPPY: return EYE_HAPPY;
  case DD_EMOTE_SURPRISE: return EYE_SURPRISE;
  case DD_EMOTE_ANGRY: return EYE_ANGRY;
  case DD_EMOTE_BLINK: return EYE_BLINK;
  default: return EYE_NORMAL;
  }
}

// Beside DDNet's player flags in a recorded character, never sent by a server: the player is AFK
// or paused (/pause), which DDNet's client shows as a tee sitting with its eyes closed.
#define RECORDED_INACTIVE (1 << 30)

// A recorded character at `tick`, with RECORDED_INACTIVE from its player. Callers hold the lock.
static dd_state_character recorded_at(ft_recording *recording, int cid, int tick) {
  dd_state_character c = characters_at(recording, tick)[cid];
  dd_state_player player;
  if (c.quality != DD_QUALITY_NONE && dd_demo_state_player(recording->state, tick, cid, &player) &&
      (player.ddnet_flags & (DD_EXPLAYERFLAG_AFK | DD_EXPLAYERFLAG_PAUSED)))
    c.player_flags |= RECORDED_INACTIVE;
  return c;
}

// The ddrace team a recorded client is in at `tick` (team 0 where the recording does not say).
// Callers hold the lock.
static int recorded_team_at(ft_recording *recording, int cid, int tick) {
  dd_state_player player;
  return dd_demo_state_player(recording->state, tick, cid, &player) ? player.ddrace_team : 0;
}

// What the player most likely held and the team it is in, as far as the state shows it.
static void recorded_input(const dd_state_character *c, int team, int tick, dd_input_t *out) {
  memset(out, 0, sizeof(*out));
  int target_x, target_y;
  recorded_target(c, &target_x, &target_y);
  out->m_Direction = (int8_t)(c->direction < 0 ? -1 : c->direction > 0 ? 1 : 0);
  out->m_TargetX = (int16_t)target_x;
  out->m_TargetY = (int16_t)target_y;
  out->m_Jump = (c->jumped & 1) != 0;
  out->m_Hook = c->hook_state != DDNET_HOOK_IDLE && c->hook_state != DDNET_HOOK_RETRACTED;
  out->m_Fire = c->attack_tick == tick || c->attack_tick == tick - 1;
  out->m_WantedWeapon = (uint8_t)(c->weapon >= 0 && c->weapon < DDNET_NUM_WEAPONS ? c->weapon : DDNET_WEAPON_GUN);
  set_flag_eye_state(out, eyes_for_emote(c->emote));
  // DDNet's client sits a tee whose player is AFK or paused, not one that is only in a menu.
  set_flag_sit(out, (c->player_flags & RECORDED_INACTIVE) != 0);
  // (the protocol's bits, which are the ones of DDNET_PLAYERFLAG_*)
  out->m_PlayerFlags = (uint8_t)(c->player_flags & 0x3f);
  dd_input_set_team(out, team);
}

bool dd_recording_input(ft_game *game, const ft_recording *recording, int32_t player, int32_t tick, void *out_record) {
  (void)game;
  if (player < 0 || player >= recording->player_count) return false;
  ft_recording *mutable_recording = (ft_recording *)recording;
  pthread_mutex_lock(&mutable_recording->lock);
  const int cid = recording->players[player].cid;
  const dd_state_character c = recorded_at(mutable_recording, cid, tick);
  const int team = recorded_team_at(mutable_recording, cid, tick);
  pthread_mutex_unlock(&mutable_recording->lock);
  if (c.quality == DD_QUALITY_NONE) return false;
  recorded_input(&c, team, tick, ddnet_input_mut(out_record));
  return true;
}

// --- replaying -----------------------------------------------------------------

enum { REPLAY_NONE = 0, REPLAY_PRESENT, REPLAY_ABSENT, REPLAY_PAUSED };

// What replaying did to one player of a world, so it can be undone once the replay ends.
struct dd_replay_slot {
  uint8_t mode;
  uint8_t jumped; // the recorded m_Jumped of the last replayed tick, for the air jump effect
  const ft_recording *recording;
  int cid;
  // The recorded flags a replay overrides while it runs, as of the last tick the player was present.
  bool hook_hit_disabled, solo, collision_disabled;
  // The recorded position of the last replayed tick (world units): the previous position of the
  // next one, and where an absent or paused player waits.
  float x, y;
};

// Grows the world's slots to `count` players; new ones are not replaying. NULL when out of memory.
static struct dd_replay_slot *replay_slots(ft_world *world, int count) {
  if (world->replay_slot_count < count) {
    struct dd_replay_slot *grown = realloc(world->replay_slots, sizeof(*grown) * (size_t)count);
    if (!grown) return NULL;
    memset(grown + world->replay_slot_count, 0, sizeof(*grown) * (size_t)(count - world->replay_slot_count));
    world->replay_slots = grown;
    world->replay_slot_count = count;
  }
  return world->replay_slots;
}

void dd_replay_copy(ft_world *dst, const ft_world *src) {
  if (src->replay_slot_count <= 0) {
    dst->replay_slot_count = 0;
    return;
  }
  if (dst->replay_slot_count != src->replay_slot_count) {
    struct dd_replay_slot *grown = realloc(dst->replay_slots, sizeof(*grown) * (size_t)src->replay_slot_count);
    if (!grown) {
      dst->replay_slot_count = 0;
      return;
    }
    dst->replay_slots = grown;
  }
  memcpy(dst->replay_slots, src->replay_slots, sizeof(*dst->replay_slots) * (size_t)src->replay_slot_count);
  dst->replay_slot_count = src->replay_slot_count;
}

void dd_replay_free(ft_world *world) {
  free(world->replay_slots);
  world->replay_slots = NULL;
  world->replay_slot_count = 0;
}

void dd_replay_insert_player(ft_world *world, int index) {
  if (index < 0 || index >= world->replay_slot_count) return; // past the slots: not replaying anyway
  const int count = world->replay_slot_count;
  if (!replay_slots(world, count + 1)) {
    dd_replay_free(world);
    return;
  }
  memmove(&world->replay_slots[index + 1], &world->replay_slots[index], sizeof(*world->replay_slots) * (size_t)(count - index));
  memset(&world->replay_slots[index], 0, sizeof(*world->replay_slots));
}

void dd_replay_remove_player(ft_world *world, int index) {
  if (index < 0 || index >= world->replay_slot_count) return;
  memmove(&world->replay_slots[index], &world->replay_slots[index + 1],
          sizeof(*world->replay_slots) * (size_t)(world->replay_slot_count - index - 1));
  --world->replay_slot_count;
}

bool dd_replay_absent(const ft_world *world, int player) {
  return world && player >= 0 && player < world->replay_slot_count && world->replay_slots[player].mode == REPLAY_ABSENT;
}

bool dd_replay_paused(const ft_world *world, int player) {
  return world && player >= 0 && player < world->replay_slot_count && world->replay_slots[player].mode == REPLAY_PAUSED;
}

bool dd_player_specced(const ft_world *world, int player) {
  if (dd_replay_paused(world, player)) return true;
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  return chr && chr->paused;
}

bool dd_player_inactive(const ft_world *world, int player) {
  const int client_id = ddnet_player_client(world, player);
  return client_id >= 0 && (get_flag_sit(&world->inputs[client_id]) || world->core.players[client_id].paused == DDNET_PAUSE_PAUSED);
}

bool dd_replay_active(const ft_world *world, int player) {
  return world && player >= 0 && player < world->replay_slot_count && world->replay_slots[player].mode != REPLAY_NONE;
}

bool dd_replay_muted(const ft_world *world, int player) {
  return world && world->replay_muted && player >= 0 && player < world->replay_slot_count &&
         world->replay_slots[player].mode != REPLAY_NONE;
}

// Sounds a tee's motion makes, which dd_demo_state_sounds works out.
static bool motion_sound(int sound_id) {
  return sound_id == DDNET_SOUND_PLAYER_JUMP || sound_id == DDNET_SOUND_PLAYER_AIRJUMP ||
         sound_id == DDNET_SOUND_HOOK_ATTACH_GROUND || sound_id == DDNET_SOUND_HOOK_ATTACH_PLAYER ||
         sound_id == DDNET_SOUND_HOOK_NOATTACH;
}

// Which world player replays the recording's client `cid` this step, or -1.
static int world_player_of_cid(const ft_recording *recording, int cid, const ft_player_playback *playback, uint32_t count) {
  if (!playback || cid < 0 || cid >= DD_STATE_MAX_CLIENTS || recording->player_of_cid[cid] < 0) return -1;
  for (uint32_t i = 0; i < count; ++i)
    if (playback[i].recording == recording && playback[i].player == recording->player_of_cid[cid]) return (int)i;
  return -1;
}

// Whether something of the replayed recording belonging to `owner` is shown in this world: what
// belongs to an imported player or to the map (turrets, doors). What belongs to a player the demo
// does not show (or cannot be told) is shown only with the whole recording imported.
static bool owner_shown(const ft_world *world, int owner) {
  if (owner == DD_STATE_OWNER_WORLD) return true;
  if (owner < 0 || owner >= DD_STATE_MAX_CLIENTS) return world->replay_clients == UINT64_MAX;
  return (world->replay_clients >> owner) & 1u;
}

// A hammer's impact belongs to the tee it hits as well as the one that swung.
// The protocol only sends its position, just outside the target's body. Find
// the nearest other tee, allowing for movement before the snapshot arrived.
static bool hammer_target_shown(const ft_world *world, const dd_state_character *characters, const dd_state_event *event) {
  int target = -1;
  float nearest = 4.f * 28.f * 28.f; // two of a tee's physical size
  for (int cid = 0; cid < DD_STATE_MAX_CLIENTS; ++cid) {
    if (cid == event->owner || characters[cid].quality == DD_QUALITY_NONE) continue;
    const float dx = characters[cid].x - event->x;
    const float dy = characters[cid].y - event->y;
    const float distance = dx * dx + dy * dy;
    if (distance < nearest) {
      nearest = distance;
      target = cid;
    }
  }
  return target >= 0 && owner_shown(world, target);
}

// The playback entry of world player `i`, or NULL when it follows its input.
static const ft_player_playback *playback_of(const ft_player_playback *playback, uint32_t count, int i) {
  if (!playback || (uint32_t)i >= count || !playback[i].recording) return NULL;
  const ft_recording *recording = playback[i].recording;
  return playback[i].player >= 0 && playback[i].player < recording->player_count ? &playback[i] : NULL;
}

static dd_state_character recorded_character(const ft_player_playback *pb) {
  ft_recording *recording = (ft_recording *)pb->recording;
  pthread_mutex_lock(&recording->lock);
  const dd_state_character c = recorded_at(recording, recording->players[pb->player].cid, pb->tick);
  pthread_mutex_unlock(&recording->lock);
  return c;
}

static int recorded_team(const ft_player_playback *pb) {
  ft_recording *recording = (ft_recording *)pb->recording;
  pthread_mutex_lock(&recording->lock);
  const int team = recorded_team_at(recording, recording->players[pb->player].cid, pb->tick);
  pthread_mutex_unlock(&recording->lock);
  return team;
}

// Where the player of `pb` waits paused at its tick, if it is paused.
static bool paused_of(const ft_player_playback *pb, float *x, float *y) {
  ft_recording *recording = (ft_recording *)pb->recording;
  const dd_recorded_player *player = &recording->players[pb->player];
  const int tick = pb->tick;
  if (tick < player->first_tick || tick > recording->last_tick || !(player->flags[tick - player->first_tick] & TICK_PAUSED)) return false;
  pthread_mutex_lock(&recording->lock);
  const bool paused = paused_at(recording->state, tick, player->cid, x, y);
  pthread_mutex_unlock(&recording->lock);
  return paused;
}

// Writes a recorded state into the tee of world client `client_id`. `offset` turns recording ticks
// into world ticks; `prev_pos` is where it was the tick before, which the physics walks tiles from
// next tick.
static void apply_state(ft_world *world, int client_id, ddnet_character_t *chr, const dd_state_character *c, int team,
                        int recording_tick, int offset, int hooked_client, ddnet_vec2_t prev_pos) {
  ddnet_character_core_t *core = &chr->core;
  const ddnet_vec2_t pos = world_pos(c->x, c->y);
  chr->prev_pos = prev_pos;
  chr->pos = pos;
  core->pos = pos;
  core->vel = (ddnet_vec2_t){c->vel_x, c->vel_y};
  core->hook_pos = world_pos(c->hook_x, c->hook_y);
  core->hook_dir = (ddnet_vec2_t){c->hook_dx, c->hook_dy};
  core->hook_state = c->hook_state;
  core->hook_tick = c->hook_tick;
  core->hooked_player = hooked_client;
  // What it is drawn with (aim, eyes); the physics takes its input from the player again next tick.
  recorded_input(c, team, recording_tick, &world->inputs[client_id]);
  ddnet_input_from_record(&world->inputs[client_id], &core->input);

  core->jumped = c->jumped;
  core->jumps = c->jumps;
  // Servers without DDNet's extras do not send it; a used air jump means all but one are gone.
  core->jumped_total = c->jumped_total >= 0 ? c->jumped_total : ((c->jumped & 2) ? (c->jumps > 1 ? c->jumps - 1 : 0) : 0);
  core->active_weapon = c->weapon >= 0 && c->weapon < DDNET_NUM_WEAPONS ? c->weapon : DDNET_WEAPON_GUN;
  chr->attack_tick = c->attack_tick + offset;
  chr->health = c->health;
  chr->armor = c->armor;
  core->weapons[core->active_weapon].ammo = c->ammo;

  if (c->has_ddnet_info) {
    const unsigned f = c->flags;
    core->deep_frozen = c->freeze_end == -1;
    chr->freeze_time = c->freeze_end > recording_tick ? c->freeze_end - recording_tick : 0;
    core->freeze_start = c->freeze_start + offset;
    core->freeze_end = c->freeze_end > 0 ? c->freeze_end + offset : c->freeze_end;
    core->is_in_freeze = (f & DD_CHARACTERFLAG_IN_FREEZE) != 0;
    core->solo = (f & DD_CHARACTERFLAG_SOLO) != 0;
    world->core.players[client_id].is_solo = core->solo;
    core->jetpack = (f & DD_CHARACTERFLAG_JETPACK) != 0;
    core->collision_disabled = (f & DD_CHARACTERFLAG_COLLISION_DISABLED) != 0;
    core->endless_hook = (f & DD_CHARACTERFLAG_ENDLESS_HOOK) != 0;
    core->endless_jump = (f & DD_CHARACTERFLAG_ENDLESS_JUMP) != 0;
    core->hammer_hit_disabled = (f & DD_CHARACTERFLAG_HAMMER_HIT_DISABLED) != 0;
    core->shotgun_hit_disabled = (f & DD_CHARACTERFLAG_SHOTGUN_HIT_DISABLED) != 0;
    core->grenade_hit_disabled = (f & DD_CHARACTERFLAG_GRENADE_HIT_DISABLED) != 0;
    core->laser_hit_disabled = (f & DD_CHARACTERFLAG_LASER_HIT_DISABLED) != 0;
    core->hook_hit_disabled = (f & DD_CHARACTERFLAG_HOOK_HIT_DISABLED) != 0;
    core->has_telegun_gun = (f & DD_CHARACTERFLAG_TELEGUN_GUN) != 0;
    core->has_telegun_grenade = (f & DD_CHARACTERFLAG_TELEGUN_GRENADE) != 0;
    core->has_telegun_laser = (f & DD_CHARACTERFLAG_TELEGUN_LASER) != 0;
    core->weapons[DDNET_WEAPON_HAMMER].got = (f & DD_CHARACTERFLAG_WEAPON_HAMMER) != 0;
    core->weapons[DDNET_WEAPON_GUN].got = (f & DD_CHARACTERFLAG_WEAPON_GUN) != 0;
    core->weapons[DDNET_WEAPON_SHOTGUN].got = (f & DD_CHARACTERFLAG_WEAPON_SHOTGUN) != 0;
    core->weapons[DDNET_WEAPON_GRENADE].got = (f & DD_CHARACTERFLAG_WEAPON_GRENADE) != 0;
    core->weapons[DDNET_WEAPON_LASER].got = (f & DD_CHARACTERFLAG_WEAPON_LASER) != 0;
    core->weapons[DDNET_WEAPON_NINJA].got = (f & DD_CHARACTERFLAG_WEAPON_NINJA) != 0;
    core->ninja.activation_tick = c->ninja_activation_tick + offset;
    chr->tele_checkpoint = c->tele_checkpoint > 0 ? c->tele_checkpoint : 0;
  } else {
    // Vanilla servers have none of these; the physics keeps the freeze and tele checkpoint it saw.
    core->solo = false;
    world->core.players[client_id].is_solo = false;
    core->collision_disabled = false;
    core->hook_hit_disabled = false;
    // Only the weapon in hand is known to be owned.
    core->weapons[core->active_weapon].got = true;
  }
  if (c->emote == DD_EMOTE_PAIN) world->pain_ticks[client_id] = world->core.tick;
  ddnet_character_changed(&world->core, client_id);
}

// Not in the recording at this tick: waits at `pos`, out of everyone's way. An absent player waits
// where it was last seen, a paused one where the demo shows it.
static void apply_absent(ft_world *world, int client_id, ddnet_character_t *chr, ddnet_vec2_t pos) {
  chr->pos = pos;
  chr->prev_pos = pos;
  chr->core.pos = pos;
  chr->core.vel = (ddnet_vec2_t){0.f, 0.f};
  chr->core.solo = true;
  world->core.players[client_id].is_solo = true;
  chr->core.collision_disabled = true;
  chr->core.hook_state = DDNET_HOOK_IDLE;
  chr->core.hooked_player = -1;
  chr->freeze_time = 0;
  ddnet_character_changed(&world->core, client_id);
}

// Hands a player whose replay ended back to its input, with the flags the replay overrode.
static void release_replay(ft_world *world, int client_id, ddnet_character_t *chr, struct dd_replay_slot *slot) {
  if (chr) {
    chr->core.hook_hit_disabled = slot->hook_hit_disabled;
    if (slot->mode == REPLAY_ABSENT || slot->mode == REPLAY_PAUSED) {
      chr->core.solo = slot->solo;
      world->core.players[client_id].is_solo = slot->solo;
      chr->core.collision_disabled = slot->collision_disabled;
    }
    ddnet_character_changed(&world->core, client_id);
  }
  slot->mode = REPLAY_NONE;
}

static void remember_flags(struct dd_replay_slot *slot, const ddnet_character_t *chr) {
  slot->hook_hit_disabled = chr->core.hook_hit_disabled;
  slot->solo = chr->core.solo;
  slot->collision_disabled = chr->core.collision_disabled;
  slot->x = chr->pos.x;
  slot->y = chr->pos.y;
}

static void projectile_pos(const dd_state_projectile *p, const float *tuning, float time, float *out_x, float *out_y);

// Effects of the recording's own events at this tick, through the same callbacks the physics
// raises them with, so they land in this world's particles and sounds, and in what a demo export
// writes (which steps without drawing: the replayed tees' own physics effects are dropped, so
// these are all it gets). `drawn`: the world's particles are drawn, so trails are worth puffing.
static void emit_events(ft_world *world, ft_recording *recording, int recording_tick, const ft_player_playback *playback,
                        uint32_t count, bool drawn) {
  ddnet_world_t *core = &world->core;
  if (!core->particle && !core->damage_indicator && !core->sound) return;
  dd_state_tick *tick = malloc(sizeof(*tick));
  if (!tick) return;
  float tuning[64];
  pthread_mutex_lock(&recording->lock);
  const dd_state_character *characters = characters_at(recording, recording_tick);
  const bool ok = dd_demo_state_entities(recording->state, recording_tick, tick);
  // The pointers in `tick` are only valid until the next call, which the lock keeps away.
  for (int i = 0; ok && i < tick->num_events; ++i) {
    const dd_state_event *event = &tick->events[i];
    if (!owner_shown(world, event->owner) &&
        !(event->type == DD_STATE_EVENT_HAMMERHIT && hammer_target_shown(world, characters, event)))
      continue;
    const ddnet_vec2_t pos = world_pos(event->x, event->y);
    const int player = world_player_of_cid(recording, event->client_id, playback, count);
    // (the callbacks take the world's client ids)
    const int client_id = ddnet_player_client(world, player);
    switch (event->type) {
    case DD_STATE_EVENT_EXPLOSION:
      if (core->particle) core->particle(pos, DDNET_PARTICLE_EXPLOSION, -1, core->user_data);
      break;
    case DD_STATE_EVENT_SPAWN:
      if (core->particle) core->particle(pos, DDNET_PARTICLE_PLAYER_SPAWN, -1, core->user_data);
      break;
    case DD_STATE_EVENT_HAMMERHIT:
      if (core->particle) core->particle(pos, DDNET_PARTICLE_HAMMER_HIT, -1, core->user_data);
      break;
    case DD_STATE_EVENT_DEATH:
      if (core->particle) core->particle(pos, DDNET_PARTICLE_PLAYER_DEATH, client_id, core->user_data);
      break;
    case DD_STATE_EVENT_BIRTHDAY:
    case DD_STATE_EVENT_FINISH:
      if (core->particle) core->particle(pos, DDNET_PARTICLE_CONFETTI, -1, core->user_data);
      break;
    case DD_STATE_EVENT_DAMAGE_IND:
      // DDNet sends one event per star, already turned to its final direction.
      if (core->damage_indicator) dd_damage_star(world, pos.x, pos.y, (float)event->angle / 256.f, player);
      break;
    case DD_STATE_EVENT_SOUND_WORLD:
      // The motion's own sounds come from the reconstruction below, on time.
      if (core->sound && !motion_sound(event->sound_id)) core->sound(pos, event->sound_id, client_id, core->user_data);
      break;
    default:
      break;
    }
  }
  // Grenades trail smoke every tick, where the physics would have puffed it for its own; other
  // shots leave bullet trails, as for the world's own (emit_bullet_trails).
  for (int i = 0; ok && drawn && core->particle && i < tick->num_projectiles; ++i) {
    const dd_state_projectile *p = &tick->projectiles[i];
    if (recording_tick - 1 < p->start_tick || !owner_shown(world, p->owner)) continue;
    if (!dd_demo_state_tuning(recording->state, recording_tick, p->tune_zone > 0 ? p->tune_zone : 0, tuning)) continue;
    const float time = (float)(recording_tick - 1 - p->start_tick) / (float)GAME_TICK_SPEED;
    float x, y;
    projectile_pos(p, tuning, time, &x, &y);
    if (p->type == DDNET_WEAPON_GRENADE) {
      core->particle(world_pos(x, y), DDNET_PARTICLE_SMOKE, -1, core->user_data);
      continue;
    }
    core->particle(world_pos(x, y), DDNET_PARTICLE_BULLET_TRAIL, -1, core->user_data);
    projectile_pos(p, tuning, time + 0.5f / (float)GAME_TICK_SPEED, &x, &y);
    core->particle(world_pos(x, y), DDNET_PARTICLE_BULLET_TRAIL, -1, core->user_data);
  }
  // Jumps and hooks, as the reconstructed tees made them: the demo has its own late, and none for
  // the recording player.
  dd_state_sound sounds[DD_STATE_MAX_CLIENTS * 2];
  const int sound_count = core->sound ? dd_demo_state_sounds(recording->state, recording_tick, sounds,
                                                             (int)(sizeof(sounds) / sizeof(sounds[0])))
                                      : 0;
  for (int i = 0; i < sound_count; ++i) {
    if (!owner_shown(world, sounds[i].client_id)) continue;
    core->sound(world_pos(sounds[i].x, sounds[i].y), sounds[i].sound_id,
                ddnet_player_client(world, world_player_of_cid(recording, sounds[i].client_id, playback, count)),
                core->user_data);
  }
  pthread_mutex_unlock(&recording->lock);
  free(tick);
}

// DDNet's client leaves a bullet trail behind every shot but a grenade, at 100 Hz: two per tick,
// from where the shot is at the start of the tick the world is about to step. The physics only
// puffs grenade smoke, from the same place.
static void emit_bullet_trails(ddnet_world_t *core) {
  if (!core->particle) return;
  for (int i = core->first_entity[DDNET_ENTTYPE_PROJECTILE]; i != -1; i = core->entities[i].link.next) {
    const ddnet_entity_t *p = &core->entities[i];
    if (p->u.projectile.type == DDNET_WEAPON_GRENADE) continue;
    const float time = (float)(core->tick - p->u.projectile.start_tick) / (float)GAME_TICK_SPEED;
    core->particle(ddnet_projectile_get_pos(core, p, time), DDNET_PARTICLE_BULLET_TRAIL, p->u.projectile.owner, core->user_data);
    core->particle(ddnet_projectile_get_pos(core, p, time + 0.5f / (float)GAME_TICK_SPEED), DDNET_PARTICLE_BULLET_TRAIL,
                   p->u.projectile.owner, core->user_data);
  }
}

// Puts the replaying players where the recording shows them at their tick, and remembers which
// recording the world around them comes from.
static void show_recorded(ft_world *world, struct dd_replay_slot *slots, int slot_count, const ft_player_playback *playback,
                          uint32_t player_count) {
  const int count = world->player_count;
  world->replay_recording = NULL;
  world->replay_clients = 0;
  int replayed_players = 0;
  for (int i = 0; i < count; ++i) {
    struct dd_replay_slot *slot = i < slot_count ? &slots[i] : NULL;
    const ft_player_playback *pb = slot ? playback_of(playback, player_count, i) : NULL;
    if (!pb) continue;
    ft_recording *recording = (ft_recording *)pb->recording;
    const int client_id = world->client_ids[i];
    // A tee the physics killed comes back when it respawns.
    ddnet_character_t *chr = ddnet_world_character(&world->core, client_id);
    const int cid = recording->players[pb->player].cid;
    slot->recording = recording;
    slot->cid = cid;
    // In the team the recording has it in, also where it is only placed (a step puts it there before
    // the tick, see dd_recording_world_step).
    const int team = recorded_team(pb);
    if (world->core.players[client_id].active && world->core.players[client_id].team != team)
      ddnet_player_set_team(&world->core, client_id, team);
    const dd_state_character c = recorded_character(pb);
    float paused_x, paused_y;
    if (!chr) {
      // nothing to put anywhere
    } else if (c.quality == DD_QUALITY_NONE && paused_of(pb, &paused_x, &paused_y)) {
      apply_absent(world, client_id, chr, world_pos(paused_x, paused_y));
      slot->mode = REPLAY_PAUSED;
      slot->x = chr->pos.x;
      slot->y = chr->pos.y;
    } else if (c.quality == DD_QUALITY_NONE) {
      apply_absent(world, client_id, chr, (ddnet_vec2_t){slot->x, slot->y});
      slot->mode = REPLAY_ABSENT;
    } else {
      const int hooked = c.hooked_player >= 0 ? world_player_of_cid(recording, c.hooked_player, playback, player_count) : -1;
      const ddnet_vec2_t pos = world_pos(c.x, c.y);
      const ddnet_vec2_t prev = slot->mode == REPLAY_PRESENT ? (ddnet_vec2_t){slot->x, slot->y} : pos;
      // DDNet's client shows an air jump when the used-air-jump bit appears.
      const bool air_jump = slot->mode == REPLAY_PRESENT && (c.jumped & 2) && !(slot->jumped & 2);
      apply_state(world, client_id, chr, &c, team, pb->tick, world->core.tick - pb->tick, ddnet_player_client(world, hooked),
                  prev);
      slot->mode = REPLAY_PRESENT;
      slot->jumped = (uint8_t)c.jumped;
      remember_flags(slot, chr);
      if (air_jump && world->core.particle)
        world->core.particle(chr->pos, DDNET_PARTICLE_AIR_JUMP, client_id, world->core.user_data);
    }

    // The world around the recording is the recording player's, or else anyone's.
    if (!world->replay_recording || cid == recording->local_cid) {
      if (world->replay_recording != recording) {
        world->replay_clients = 0;
        replayed_players = 0;
      }
      world->replay_recording = recording;
      world->replay_tick = pb->tick;
    }
    if (world->replay_recording == recording && !((world->replay_clients >> cid) & 1u)) {
      world->replay_clients |= UINT64_C(1) << cid;
      ++replayed_players;
    }
  }
  if (world->replay_recording && replayed_players >= ((const ft_recording *)world->replay_recording)->player_count)
    world->replay_clients = UINT64_MAX;
}

// What a player does on the tick about to be stepped (with the effects of the build the world steps
// with), and what it holds from then on.
static void apply_input(ft_world *world, int client_id, const dd_input_t *record, bool events) {
  ddnet_record_apply(&world->core, client_id, record, events);
  world->inputs[client_id] = *record;
  // (a kill is a trigger: one held on is not one again)
  set_flag_kill(&world->inputs[client_id], 0);
}

void dd_recording_world_step(ft_game *game, ft_world *world, const void *inputs, const ft_player_playback *playback,
                             uint32_t player_count) {
  if (!world) return;
  const int tick_before = ddnet_engine_tick(world);
  const bool effects_bound = dd_particles_bind(game, world);
  const bool events = ddnet_step_events(world);
  const int count = world->player_count;
  // Who skids before the step, so a skid's first tick is heard.
  uint8_t was_skidding[DDNET_MAX_CLIENTS];
  for (int i = 0; i < count; ++i)
    was_skidding[i] = dd_skidding(world, i);
  const dd_input_t *records = inputs;
  // Slots only come into being with the first replay; a world that never replays stays without.
  struct dd_replay_slot *slots = playback ? replay_slots(world, count) : world->replay_slots;
  const int slot_count = slots ? world->replay_slot_count : 0;

  bool any_replayed = false;
  for (int i = 0; i < count; ++i) {
    const int client_id = world->client_ids[i];
    ddnet_character_t *chr = ddnet_world_character(&world->core, client_id);
    struct dd_replay_slot *slot = i < slot_count ? &slots[i] : NULL;
    const ft_player_playback *pb = slot ? playback_of(playback, player_count, i) : NULL;
    if (pb) {
      // Stepped with what it most likely held, so the physics keeps up with it, but it never
      // fires and never hooks anyone: nothing it does may reach a simulated tee. It is in the team
      // the recording has it in, which is who it is there for.
      const dd_state_character c = recorded_character(pb);
      dd_input_t input;
      if (c.quality != DD_QUALITY_NONE) {
        recorded_input(&c, recorded_team(pb), pb->tick, &input);
        input.m_Fire = 0;
      } else {
        memset(&input, 0, sizeof(input));
        input.m_TargetY = -1;
      }
      if (chr) {
        if (slot->mode == REPLAY_NONE) remember_flags(slot, chr);
        chr->core.hook_hit_disabled = true;
        ddnet_character_changed(&world->core, client_id);
      }
      apply_input(world, client_id, &input, events);
      any_replayed = true;
      continue;
    }
    if (slot && slot->mode != REPLAY_NONE) release_replay(world, client_id, chr, slot);
    // Players the engine has no input for keep holding their last one.
    if (records && (uint32_t)i < player_count) apply_input(world, client_id, &records[i], events);
    else apply_input(world, client_id, &world->inputs[client_id], events);
  }

  world->replay_muted = any_replayed;
  emit_bullet_trails(&world->core);
  if (events)
    ddnet_ev_world_tick(&world->core);
  else
    ddnet_world_tick(&world->core);
  world->replay_muted = false;

  if (any_replayed) show_recorded(world, slots, slot_count, playback, player_count);
  else {
    world->replay_recording = NULL;
    world->replay_clients = 0;
  }
  if (world->replay_recording)
    emit_events(world, (ft_recording *)world->replay_recording, world->replay_tick, playback, player_count, effects_bound);
  dd_particles_finish(game, world, tick_before, effects_bound, was_skidding);
}

void dd_recording_world_place(ft_game *game, ft_world *world, const ft_player_playback *playback, uint32_t player_count) {
  (void)game;
  if (!world || !playback) return;
  const int count = world->player_count;
  struct dd_replay_slot *slots = replay_slots(world, count);
  if (!slots) return;
  bool any_replayed = false;
  for (int i = 0; i < count; ++i) {
    if (!playback_of(playback, player_count, i)) continue;
    const int client_id = world->client_ids[i];
    ddnet_character_t *chr = ddnet_world_character(&world->core, client_id);
    if (chr) {
      if (slots[i].mode == REPLAY_NONE) remember_flags(&slots[i], chr);
      chr->core.hook_hit_disabled = true;
      ddnet_character_changed(&world->core, client_id);
    }
    any_replayed = true;
  }
  if (any_replayed) show_recorded(world, slots, world->replay_slot_count, playback, player_count);
}

// --- exporting the recording's world ------------------------------------------

static bool recorded_export_owner(const ft_world *world, int owner, const int *client_ids, int client_count, int *out) {
  if (!owner_shown(world, owner)) return false;
  if (owner < 0) {
    *out = -1;
    return true;
  }
  for (int p = 0; p < world->replay_slot_count && p < client_count; ++p) {
    const struct dd_replay_slot *slot = &world->replay_slots[p];
    if (slot->mode != REPLAY_NONE && slot->recording == world->replay_recording && slot->cid == owner && client_ids[p] >= 0) {
      *out = client_ids[p];
      return true;
    }
  }
  return false;
}

bool dd_recording_snap_entities(const ft_world *world, dd_snapshot_builder *builder, const int *client_ids,
                                int client_count, int demo_tick, int *next_item_id) {
  ft_recording *recording = (ft_recording *)world->replay_recording;
  if (!recording) return true;
  dd_state_tick *state = malloc(sizeof(*state));
  if (!state) return false;
  const int tick_shift = demo_tick - world->replay_tick;
  pthread_mutex_lock(&recording->lock);
  bool ok = dd_demo_state_entities(recording->state, world->replay_tick, state);
  for (int i = 0; ok && i < state->num_projectiles; ++i) {
    const dd_state_projectile *source = &state->projectiles[i];
    int owner;
    if (!recorded_export_owner(world, source->owner, client_ids, client_count, &owner)) continue;
    dd_netobj_ddnet_projectile *p = demo_sb_add_item(builder, DD_NETOBJTYPE_DDNETPROJECTILE, (*next_item_id)++, sizeof(*p));
    if (!p) { ok = false; break; }
    p->m_X = (int)lroundf(source->x * 100.f);
    p->m_Y = (int)lroundf(source->y * 100.f);
    p->m_VelX = (int)lroundf(source->vel_x * 1e6f);
    p->m_VelY = (int)lroundf(source->vel_y * 1e6f);
    p->m_Type = source->type;
    p->m_StartTick = source->start_tick + tick_shift;
    p->m_Owner = owner;
    p->m_SwitchNumber = source->switch_number;
    p->m_TuneZone = source->tune_zone;
    p->m_Flags = DD_PROJECTILEFLAG_NORMALIZE_VEL;
    if (source->bouncing & 1) p->m_Flags |= DD_PROJECTILEFLAG_BOUNCE_HORIZONTAL;
    if (source->bouncing & 2) p->m_Flags |= DD_PROJECTILEFLAG_BOUNCE_VERTICAL;
    if (source->explosive) p->m_Flags |= DD_PROJECTILEFLAG_EXPLOSIVE;
    if (source->freeze) p->m_Flags |= DD_PROJECTILEFLAG_FREEZE;
  }
  for (int i = 0; ok && i < state->num_lasers; ++i) {
    const dd_state_laser *source = &state->lasers[i];
    int owner;
    if (!recorded_export_owner(world, source->owner, client_ids, client_count, &owner)) continue;
    dd_netobj_ddnet_laser *l = demo_sb_add_item(builder, DD_NETOBJTYPE_DDNETLASER, (*next_item_id)++, sizeof(*l));
    if (!l) { ok = false; break; }
    l->m_ToX = (int)lroundf(source->to_x);
    l->m_ToY = (int)lroundf(source->to_y);
    l->m_FromX = (int)lroundf(source->from_x);
    l->m_FromY = (int)lroundf(source->from_y);
    l->m_StartTick = source->start_tick < 0 ? source->start_tick : source->start_tick + tick_shift;
    l->m_Owner = owner;
    l->m_Type = source->type;
    l->m_Subtype = source->subtype;
    l->m_SwitchNumber = source->switch_number;
    l->m_Flags = source->flags;
  }
  pthread_mutex_unlock(&recording->lock);
  free(state);
  return ok;
}

// --- drawing the recording's world ---------------------------------------------

// DDNet's CalcPos: where a projectile is `time` seconds after it started.
static void projectile_pos(const dd_state_projectile *p, const float *tuning, float time, float *out_x, float *out_y) {
  float curvature = 0.f, speed = 0.f;
  switch (p->type) {
  case DDNET_WEAPON_GRENADE:
    curvature = tuning[23];
    speed = tuning[24];
    break;
  case DDNET_WEAPON_SHOTGUN:
    curvature = tuning[19];
    speed = tuning[20];
    break;
  case DDNET_WEAPON_GUN:
    curvature = tuning[16];
    speed = tuning[17];
    break;
  default:
    break;
  }
  const float travelled = speed * time;
  *out_x = p->x + p->vel_x * travelled;
  *out_y = p->y + p->vel_y * travelled + curvature / 10000.f * travelled * travelled;
}

// The world player that replays client `cid` of the recording the world shows, or -1.
static int replayed_player(const ft_world *world, int cid) {
  for (int i = 0; i < world->replay_slot_count; ++i) {
    const struct dd_replay_slot *slot = &world->replay_slots[i];
    if (slot->mode != REPLAY_NONE && slot->recording == world->replay_recording && slot->cid == cid) return i;
  }
  return -1;
}

void dd_recording_render_entities(ft_game *game, const ft_world *world, float intra, int viewer) {
  ft_recording *recording = (ft_recording *)world->replay_recording;
  if (!recording) return;
  // (the world's own opacity, which a shot of a tee in another team draws a part of)
  const float world_alpha = game->gfx.world_alpha;
  const int tick = world->replay_tick;
  dd_state_tick *state = malloc(sizeof(*state));
  if (!state) return;
  float tuning[64];
  pthread_mutex_lock(&recording->lock);
  if (dd_demo_state_entities(recording->state, tick, state) && dd_state_tuning_count() <= 64) {
    for (int i = 0; i < state->num_projectiles; ++i) {
      const dd_state_projectile *p = &state->projectiles[i];
      if (!owner_shown(world, p->owner)) continue; // fired by a player that was not imported
      const int zone = p->tune_zone > 0 ? p->tune_zone : 0;
      if (!dd_demo_state_tuning(recording->state, tick, zone, tuning)) continue;
      // The world shows the recording tick `tick`; frames interpolate from the one before.
      float x0, y0, x1, y1;
      projectile_pos(p, tuning, (float)(tick - 1 - p->start_tick) / (float)GAME_TICK_SPEED, &x0, &y0);
      projectile_pos(p, tuning, (float)(tick - p->start_tick) / (float)GAME_TICK_SPEED, &x1, &y1);
      const vec2 from = {x0 / PX_PER_TILE, y0 / PX_PER_TILE};
      const vec2 to = {x1 / PX_PER_TILE, y1 / PX_PER_TILE};
      game->gfx.world_alpha = world_alpha * dd_team_alpha(game, world, viewer, replayed_player(world, p->owner));
      dd_render_projectile(game, from, to, intra, p->type, tick, p->start_tick);
    }
    for (int i = 0; i < state->num_lasers; ++i) {
      const dd_state_laser *l = &state->lasers[i];
      if (!owner_shown(world, l->owner)) continue;
      if (!dd_demo_state_tuning(recording->state, tick, 0, tuning)) continue;
      const vec2 from = {l->from_x / PX_PER_TILE, l->from_y / PX_PER_TILE};
      const vec2 to = {l->to_x / PX_PER_TILE, l->to_y / PX_PER_TILE};
      // DDNet's own laser types (doors, draggers, freeze) are drawn like rifle shots.
      const bool shotgun = l->type == DDNET_WEAPON_SHOTGUN;
      game->gfx.world_alpha = world_alpha * dd_team_alpha(game, world, viewer, replayed_player(world, l->owner));
      dd_render_laser(game, from, to, !shotgun, (float)(tick - 1 - l->start_tick) + intra, tuning[27], tick, intra);
    }
  }
  game->gfx.world_alpha = world_alpha;
  pthread_mutex_unlock(&recording->lock);
  free(state);
}

// --- the recording's messages as timeline events -----------------------------

// The world player a recorded client became, or -1 when it was not imported.
static int imported_as(const ft_recording *recording, const int32_t *world_players, uint32_t count, int cid) {
  if (cid < 0 || cid >= DD_STATE_MAX_CLIENTS) return -1;
  const int player = recording->player_of_cid[cid];
  return player >= 0 && (uint32_t)player < count ? world_players[player] : -1;
}

void dd_recording_events(ft_game *game, const ft_recording *recording, const int32_t *world_players, uint32_t player_count,
                         void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  (void)game;
  int count = 0;
  const dd_state_message *messages = dd_demo_state_messages(recording->state, &count);
  // Race times and records are the recording player's; the rest of the world rides along with it.
  const bool local_imported = imported_as(recording, world_players, player_count, recording->local_cid) >= 0;
  for (int i = 0; i < count; ++i) {
    const dd_state_message *m = &messages[i];
    if (m->tick < recording->first_tick || m->tick > recording->last_tick) continue;
    dd_event_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.magic = DD_EVENT_PAYLOAD_MAGIC;
    payload.client_id = -1;
    payload.killer = payload.victim = -1;
    bool keep = true;
    switch (m->type) {
    case DD_STATE_MSG_CHAT:
      payload.type = DD_EVENT_CHAT;
      payload.team = m->team;
      payload.client_id = imported_as(recording, world_players, player_count, m->client_id);
      keep = m->client_id < 0 || payload.client_id >= 0; // the server's, or an imported player's
      snprintf(payload.message, sizeof(payload.message), "%s", m->text ? m->text : "");
      break;
    case DD_STATE_MSG_BROADCAST:
      payload.type = DD_EVENT_BROADCAST;
      snprintf(payload.message, sizeof(payload.message), "%s", m->text ? m->text : "");
      break;
    case DD_STATE_MSG_KILLMSG:
      payload.type = DD_EVENT_KILLMSG;
      payload.victim = imported_as(recording, world_players, player_count, m->victim);
      payload.killer = imported_as(recording, world_players, player_count, m->killer);
      if (payload.killer < 0) payload.killer = payload.victim; // a kill by someone left out reads as a suicide
      payload.weapon = m->weapon;
      payload.mode_special = m->mode_special;
      keep = payload.victim >= 0;
      break;
    case DD_STATE_MSG_EMOTICON:
      payload.type = DD_EVENT_EMOTICON;
      payload.client_id = imported_as(recording, world_players, player_count, m->client_id);
      payload.emoticon = m->value;
      keep = payload.client_id >= 0;
      break;
    case DD_STATE_MSG_VOTE_SET:
      payload.type = DD_EVENT_VOTE_SET;
      payload.vote_timeout = m->value;
      snprintf(payload.message, sizeof(payload.message), "%s", m->text ? m->text : "");
      snprintf(payload.reason, sizeof(payload.reason), "%s", m->text2 ? m->text2 : "");
      break;
    case DD_STATE_MSG_VOTE_STATUS:
      payload.type = DD_EVENT_VOTE_STATUS;
      payload.vote_yes = m->yes;
      payload.vote_no = m->no;
      payload.vote_pass = m->pass;
      payload.vote_total = m->total;
      break;
    case DD_STATE_MSG_DDRACE_TIME:
      payload.type = DD_EVENT_DDRACE_TIME;
      payload.time = m->time;
      payload.check = m->check;
      payload.finish = m->finish;
      keep = local_imported;
      break;
    case DD_STATE_MSG_RECORD:
      payload.type = DD_EVENT_RECORD;
      payload.server_time_best = m->time;
      payload.player_time_best = m->time2;
      keep = local_imported;
      break;
    default:
      keep = false; // MOTD, tuning, team states, sounds: nothing the timeline shows
      break;
    }
    if (!keep) continue;
    char summary[256];
    ft_timeline_event event;
    dd_event_make(-1, m->tick, &payload, &event, summary, sizeof(summary));
    emit(user, &event);
  }
}
