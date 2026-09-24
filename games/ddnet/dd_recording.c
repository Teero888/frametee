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
#include <ddnet_physics/collision.h>
#include <ddnet_physics/vmath.h>

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

// FrameTee's world is the DDNet map with MAP_EXPAND tiles of border on every side, so a position in
// the demo sits MAP_EXPAND32 units further in.
static mvec2 world_pos(float x, float y) { return vec2_init(x + (float)MAP_EXPAND32, y + (float)MAP_EXPAND32); }

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
  const map_data_t *map = &level->collision.m_MapData;
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

// What the player most likely held, as far as the state shows it.
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

static void recorded_input(const dd_state_character *c, int tick, SPlayerInput *out) {
  memset(out, 0, sizeof(*out));
  int target_x, target_y;
  recorded_target(c, &target_x, &target_y);
  out->m_Direction = (int8_t)(c->direction < 0 ? -1 : c->direction > 0 ? 1 : 0);
  out->m_TargetX = (int16_t)target_x;
  out->m_TargetY = (int16_t)target_y;
  out->m_Jump = (c->jumped & 1) != 0;
  out->m_Hook = c->hook_state != HOOK_IDLE && c->hook_state != HOOK_RETRACTED;
  out->m_Fire = c->attack_tick == tick || c->attack_tick == tick - 1;
  out->m_WantedWeapon = (uint8_t)(c->weapon >= 0 && c->weapon < NUM_WEAPONS ? c->weapon : WEAPON_GUN);
  set_flag_eye_state(out, eyes_for_emote(c->emote));
  // DDNet's client sits a tee whose player is AFK or paused, not one that is only in a menu.
  set_flag_sit(out, (c->player_flags & RECORDED_INACTIVE) != 0);
  set_flag_chatbubble(out, (c->player_flags & DD_PLAYERFLAG_CHATTING) != 0);
  set_flag_hookline(out, (c->player_flags & DD_PLAYERFLAG_AIM) != 0);
}

bool dd_recording_input(ft_game *game, const ft_recording *recording, int32_t player, int32_t tick, void *out_record) {
  (void)game;
  if (player < 0 || player >= recording->player_count) return false;
  ft_recording *mutable_recording = (ft_recording *)recording;
  pthread_mutex_lock(&mutable_recording->lock);
  const dd_state_character c = recorded_at(mutable_recording, recording->players[player].cid, tick);
  pthread_mutex_unlock(&mutable_recording->lock);
  if (c.quality == DD_QUALITY_NONE) return false;
  recorded_input(&c, tick, ddnet_input_mut(out_record));
  return true;
}

// --- replaying -----------------------------------------------------------------

enum { REPLAY_NONE = 0, REPLAY_PRESENT, REPLAY_ABSENT, REPLAY_PAUSED };

// What replaying did to one player of a world, so it can be undone once the replay ends.
struct dd_replay_slot {
  uint8_t mode;
  uint8_t jumped; // the recorded m_Jumped of the last replayed tick, for the air jump effect
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

bool dd_replay_muted(const ft_world *world, int player) {
  return world && world->replay_muted && player >= 0 && player < world->replay_slot_count &&
         world->replay_slots[player].mode != REPLAY_NONE;
}

// Which world player replays the recording's client `cid` this step, or -1.
static int world_player_of_cid(const ft_recording *recording, int cid, const ft_player_playback *playback, uint32_t count) {
  if (!playback || cid < 0 || cid >= DD_STATE_MAX_CLIENTS || recording->player_of_cid[cid] < 0) return -1;
  for (uint32_t i = 0; i < count; ++i)
    if (playback[i].recording == recording && playback[i].player == recording->player_of_cid[cid]) return (int)i;
  return -1;
}

// Whether something of the replayed recording belonging to `owner` is shown in this world: only
// what belongs to an imported player, or to the world itself. `world_can_own` says whether the
// world could have made it at all (an explosion can be a map's, a damage star never is), which
// decides for things whose owner the demo leaves open.
static bool owner_shown(const ft_world *world, int owner, bool world_can_own) {
  if (owner < 0 || owner >= DD_STATE_MAX_CLIENTS) return world_can_own || world->replay_clients == UINT64_MAX;
  return (world->replay_clients >> owner) & 1u;
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

// Writes a recorded state into a world character. `offset` turns recording ticks into world ticks;
// `prev_pos` is where it was the tick before, which the physics walks tiles from next tick.
static void apply_state(SCharacterCore *core, const dd_state_character *c, int recording_tick, int offset, int hooked_player,
                        mvec2 prev_pos) {
  const mvec2 pos = world_pos(c->x, c->y);
  core->m_PrevPos = prev_pos;
  core->m_Pos = pos;
  core->m_Vel = vec2_init(c->vel_x, c->vel_y);
  core->m_VelMag = sqrtf(c->vel_x * c->vel_x + c->vel_y * c->vel_y);
  core->m_VelRamp = 1.f;
  core->m_HookPos = world_pos(c->hook_x, c->hook_y);
  core->m_HookDir = vec2_init(c->hook_dx, c->hook_dy);
  core->m_HookState = (int8_t)c->hook_state;
  core->m_HookTick = c->hook_tick;
  core->m_HookedPlayer = hooked_player;
  recorded_input(c, recording_tick, &core->m_Input);

  core->m_Jumped = (uint8_t)c->jumped;
  core->m_Jumps = c->jumps;
  // Servers without DDNet's extras do not send it; a used air jump means all but one are gone.
  core->m_JumpedTotal = c->jumped_total >= 0 ? c->jumped_total : ((c->jumped & 2) ? (c->jumps > 1 ? c->jumps - 1 : 0) : 0);
  core->m_ActiveWeapon = (unsigned char)(c->weapon >= 0 && c->weapon < NUM_WEAPONS ? c->weapon : WEAPON_GUN);
  core->m_AttackTick = c->attack_tick + offset;
  core->m_Health = (int8_t)c->health;
  core->m_Armor = (int8_t)c->armor;
  if (core->m_ActiveWeapon < NUM_WEAPONS) core->m_aWeaponAmmo[core->m_ActiveWeapon] = (int8_t)(c->ammo > 127 ? 127 : c->ammo);

  if (c->has_ddnet_info) {
    const unsigned f = c->flags;
    core->m_DeepFrozen = c->freeze_end == -1;
    core->m_FreezeTime = c->freeze_end > recording_tick ? c->freeze_end - recording_tick : 0;
    core->m_FreezeStart = c->freeze_start + offset;
    core->m_IsInFreeze = (f & DD_CHARACTERFLAG_IN_FREEZE) != 0;
    core->m_Solo = (f & DD_CHARACTERFLAG_SOLO) != 0;
    core->m_Jetpack = (f & DD_CHARACTERFLAG_JETPACK) != 0;
    core->m_CollisionDisabled = (f & DD_CHARACTERFLAG_COLLISION_DISABLED) != 0;
    core->m_EndlessHook = (f & DD_CHARACTERFLAG_ENDLESS_HOOK) != 0;
    core->m_EndlessJump = (f & DD_CHARACTERFLAG_ENDLESS_JUMP) != 0;
    core->m_HammerHitDisabled = (f & DD_CHARACTERFLAG_HAMMER_HIT_DISABLED) != 0;
    core->m_ShotgunHitDisabled = (f & DD_CHARACTERFLAG_SHOTGUN_HIT_DISABLED) != 0;
    core->m_GrenadeHitDisabled = (f & DD_CHARACTERFLAG_GRENADE_HIT_DISABLED) != 0;
    core->m_LaserHitDisabled = (f & DD_CHARACTERFLAG_LASER_HIT_DISABLED) != 0;
    core->m_HookHitDisabled = (f & DD_CHARACTERFLAG_HOOK_HIT_DISABLED) != 0;
    core->m_HasTelegunGun = (f & DD_CHARACTERFLAG_TELEGUN_GUN) != 0;
    core->m_HasTelegunGrenade = (f & DD_CHARACTERFLAG_TELEGUN_GRENADE) != 0;
    core->m_HasTelegunLaser = (f & DD_CHARACTERFLAG_TELEGUN_LASER) != 0;
    core->m_aWeaponGot[WEAPON_HAMMER] = (f & DD_CHARACTERFLAG_WEAPON_HAMMER) != 0;
    core->m_aWeaponGot[WEAPON_GUN] = (f & DD_CHARACTERFLAG_WEAPON_GUN) != 0;
    core->m_aWeaponGot[WEAPON_SHOTGUN] = (f & DD_CHARACTERFLAG_WEAPON_SHOTGUN) != 0;
    core->m_aWeaponGot[WEAPON_GRENADE] = (f & DD_CHARACTERFLAG_WEAPON_GRENADE) != 0;
    core->m_aWeaponGot[WEAPON_LASER] = (f & DD_CHARACTERFLAG_WEAPON_LASER) != 0;
    core->m_aWeaponGot[WEAPON_NINJA] = (f & DD_CHARACTERFLAG_WEAPON_NINJA) != 0;
    core->m_Ninja.m_ActivationTick = c->ninja_activation_tick + offset;
    core->m_TeleCheckpoint = (unsigned char)(c->tele_checkpoint > 0 ? c->tele_checkpoint : 0);
  } else {
    // Vanilla servers have none of these; the physics keeps the freeze and tele checkpoint it saw.
    core->m_Solo = false;
    core->m_CollisionDisabled = false;
    core->m_HookHitDisabled = false;
    // Only the weapon in hand is known to be owned.
    core->m_aWeaponGot[core->m_ActiveWeapon] = true;
  }
  if (c->emote == DD_EMOTE_PAIN) core->m_DamageTick = recording_tick + offset;
  cc_calc_indices(core);
}

// Not in the recording at this tick: waits at `pos`, out of everyone's way. An absent player waits
// where it was last seen, a paused one where the demo shows it.
static void apply_absent(SCharacterCore *core, mvec2 pos) {
  core->m_Pos = pos;
  core->m_PrevPos = pos;
  core->m_Vel = vec2_init(0.f, 0.f);
  core->m_Solo = true;
  core->m_CollisionDisabled = true;
  core->m_HookState = HOOK_IDLE;
  core->m_HookedPlayer = -1;
  core->m_FreezeTime = 0;
  cc_calc_indices(core);
}

// Hands a player whose replay ended back to its input, with the flags the replay overrode.
static void release_replay(SCharacterCore *core, struct dd_replay_slot *slot) {
  core->m_HookHitDisabled = slot->hook_hit_disabled;
  if (slot->mode == REPLAY_ABSENT || slot->mode == REPLAY_PAUSED) {
    core->m_Solo = slot->solo;
    core->m_CollisionDisabled = slot->collision_disabled;
  }
  slot->mode = REPLAY_NONE;
}

static void remember_flags(struct dd_replay_slot *slot, const SCharacterCore *core) {
  slot->hook_hit_disabled = core->m_HookHitDisabled;
  slot->solo = core->m_Solo;
  slot->collision_disabled = core->m_CollisionDisabled;
  slot->x = vgetx(core->m_Pos);
  slot->y = vgety(core->m_Pos);
}

static void projectile_pos(const dd_state_projectile *p, const float *tuning, float time, float *out_x, float *out_y);

// Effects of the recording's own events at this tick, through the same callbacks the physics
// raises them with, so they land in this world's particles and sounds, and in what a demo export
// writes (which steps without drawing: the replayed tees' own physics effects are dropped, so
// these are all it gets). `drawn`: the world's particles are drawn, so trails are worth puffing.
static void emit_events(ft_world *world, ft_recording *recording, int recording_tick, const ft_player_playback *playback,
                        uint32_t count, bool drawn) {
  SWorldCore *core = &world->core;
  if (!core->particle && !core->damage_indicator && !core->sound) return;
  dd_state_tick *tick = malloc(sizeof(*tick));
  if (!tick) return;
  float tuning[64];
  pthread_mutex_lock(&recording->lock);
  const bool ok = dd_demo_state_entities(recording->state, recording_tick, tick);
  // The pointers in `tick` are only valid until the next call, which the lock keeps away.
  for (int i = 0; ok && i < tick->num_events; ++i) {
    const dd_state_event *event = &tick->events[i];
    const bool world_can_own = event->type == DD_STATE_EVENT_EXPLOSION || event->type == DD_STATE_EVENT_SOUND_WORLD ||
                               event->type == DD_STATE_EVENT_SOUND_GLOBAL || event->type == DD_STATE_EVENT_MAP_SOUND_WORLD;
    if (!owner_shown(world, event->owner, world_can_own)) continue;
    const mvec2 pos = world_pos(event->x, event->y);
    const int player = world_player_of_cid(recording, event->client_id, playback, count);
    switch (event->type) {
    case DD_STATE_EVENT_EXPLOSION:
      if (core->particle) core->particle(pos, PARTICLE_TYPE_EXPLOSION, -1, core->user_data);
      break;
    case DD_STATE_EVENT_SPAWN:
      if (core->particle) core->particle(pos, PARTICLE_TYPE_PLAYER_SPAWN, -1, core->user_data);
      break;
    case DD_STATE_EVENT_HAMMERHIT:
      if (core->particle) core->particle(pos, PARTICLE_TYPE_HAMMER_HIT, -1, core->user_data);
      break;
    case DD_STATE_EVENT_DEATH:
      if (core->particle) core->particle(pos, PARTICLE_TYPE_PLAYER_DEATH, player, core->user_data);
      break;
    case DD_STATE_EVENT_BIRTHDAY:
    case DD_STATE_EVENT_FINISH:
      if (core->particle) core->particle(pos, PARTICLE_TYPE_CONFETTI, -1, core->user_data);
      break;
    case DD_STATE_EVENT_DAMAGE_IND:
      // DDNet sends one event per star, already turned to its final direction; the callback spreads
      // `amount` stars around 3pi/2 + angle, which for one star is exactly that centre.
      if (core->damage_indicator)
        core->damage_indicator(pos, (float)event->angle / 256.f - 3.f * (float)M_PI / 2.f, 1, player, core->user_data);
      break;
    case DD_STATE_EVENT_SOUND_WORLD:
      if (core->sound) core->sound(pos, event->sound_id, player, core->user_data);
      break;
    default:
      break;
    }
  }
  // Grenades trail smoke every tick, where the physics would have puffed it for its own; other
  // shots leave bullet trails, as for the world's own (emit_bullet_trails).
  for (int i = 0; ok && drawn && core->particle && i < tick->num_projectiles; ++i) {
    const dd_state_projectile *p = &tick->projectiles[i];
    if (recording_tick - 1 < p->start_tick || !owner_shown(world, p->owner, true)) continue;
    if (!dd_demo_state_tuning(recording->state, recording_tick, p->tune_zone > 0 ? p->tune_zone : 0, tuning)) continue;
    const float time = (float)(recording_tick - 1 - p->start_tick) / (float)GAME_TICK_SPEED;
    float x, y;
    projectile_pos(p, tuning, time, &x, &y);
    if (p->type == WEAPON_GRENADE) {
      core->particle(world_pos(x, y), PARTICLE_TYPE_SMOKE, -1, core->user_data);
      continue;
    }
    core->particle(world_pos(x, y), PARTICLE_TYPE_BULLET_TRAIL, -1, core->user_data);
    projectile_pos(p, tuning, time + 0.5f / (float)GAME_TICK_SPEED, &x, &y);
    core->particle(world_pos(x, y), PARTICLE_TYPE_BULLET_TRAIL, -1, core->user_data);
  }
  pthread_mutex_unlock(&recording->lock);
  free(tick);
}

// DDNet's client leaves a bullet trail behind every shot but a grenade, at 100 Hz: two per tick,
// from where the shot is at the start of the tick the world is about to step. The physics only
// puffs grenade smoke, from the same place.
static void emit_bullet_trails(SWorldCore *core) {
  if (!core->particle) return;
  for (SProjectile *p = (SProjectile *)core->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; p;
       p = (SProjectile *)p->m_Base.m_pNextTypeEntity) {
    if (p->m_Type == WEAPON_GRENADE) continue;
    const float time = (float)(core->m_GameTick - p->m_StartTick) / (float)GAME_TICK_SPEED;
    core->particle(prj_get_pos(p, time), PARTICLE_TYPE_BULLET_TRAIL, p->m_Owner, core->user_data);
    core->particle(prj_get_pos(p, time + 0.5f / (float)GAME_TICK_SPEED), PARTICLE_TYPE_BULLET_TRAIL, p->m_Owner, core->user_data);
  }
}

void dd_recording_world_step(ft_game *game, ft_world *world, const void *inputs, const ft_player_playback *playback,
                             uint32_t player_count) {
  if (!world) return;
  const int tick_before = world->core.m_GameTick;
  const bool effects_bound = dd_particles_bind(game, world);
  const SPlayerInput *records = inputs;
  const int count = world->core.m_NumCharacters;
  // Slots only come into being with the first replay; a world that never replays stays without.
  struct dd_replay_slot *slots = playback ? replay_slots(world, count) : world->replay_slots;
  const int slot_count = slots ? world->replay_slot_count : 0;

  bool any_replayed = false;
  for (int i = 0; i < count; ++i) {
    SCharacterCore *core = &world->core.m_pCharacters[i];
    struct dd_replay_slot *slot = i < slot_count ? &slots[i] : NULL;
    const ft_player_playback *pb = slot ? playback_of(playback, player_count, i) : NULL;
    if (pb) {
      // Stepped with what it most likely held, so the physics keeps up with it, but it never
      // fires and never hooks anyone: nothing it does may reach a simulated tee.
      const dd_state_character c = recorded_character(pb);
      SPlayerInput input;
      if (c.quality != DD_QUALITY_NONE) {
        recorded_input(&c, pb->tick, &input);
        input.m_Fire = 0;
      } else {
        memset(&input, 0, sizeof(input));
        input.m_TargetY = -1;
      }
      if (slot->mode == REPLAY_NONE) remember_flags(slot, core);
      core->m_HookHitDisabled = true;
      cc_on_input(core, &input);
      any_replayed = true;
      continue;
    }
    if (slot && slot->mode != REPLAY_NONE) release_replay(core, slot);
    // Players the engine has no input for keep holding their last one.
    cc_on_input(core, records && (uint32_t)i < player_count ? &records[i] : &core->m_Input);
  }

  world->replay_muted = any_replayed;
  emit_bullet_trails(&world->core);
  wc_tick(&world->core);
  world->replay_muted = false;

  world->replay_recording = NULL;
  world->replay_clients = 0;
  int replayed_players = 0;
  for (int i = 0; any_replayed && i < count; ++i) {
    struct dd_replay_slot *slot = i < slot_count ? &slots[i] : NULL;
    const ft_player_playback *pb = slot ? playback_of(playback, player_count, i) : NULL;
    if (!pb) continue;
    ft_recording *recording = (ft_recording *)pb->recording;
    SCharacterCore *core = &world->core.m_pCharacters[i];
    const int cid = recording->players[pb->player].cid;
    const dd_state_character c = recorded_character(pb);
    float paused_x, paused_y;
    if (c.quality == DD_QUALITY_NONE && paused_of(pb, &paused_x, &paused_y)) {
      apply_absent(core, world_pos(paused_x, paused_y));
      slot->mode = REPLAY_PAUSED;
      slot->x = vgetx(core->m_Pos);
      slot->y = vgety(core->m_Pos);
    } else if (c.quality == DD_QUALITY_NONE) {
      apply_absent(core, vec2_init(slot->x, slot->y));
      slot->mode = REPLAY_ABSENT;
    } else {
      const int hooked = c.hooked_player >= 0 ? world_player_of_cid(recording, c.hooked_player, playback, player_count) : -1;
      const mvec2 pos = world_pos(c.x, c.y);
      const mvec2 prev = slot->mode == REPLAY_PRESENT ? vec2_init(slot->x, slot->y) : pos;
      // DDNet's client shows an air jump when the used-air-jump bit appears.
      const bool air_jump = slot->mode == REPLAY_PRESENT && (c.jumped & 2) && !(slot->jumped & 2);
      apply_state(core, &c, pb->tick, world->core.m_GameTick - pb->tick, hooked, prev);
      slot->mode = REPLAY_PRESENT;
      slot->jumped = (uint8_t)c.jumped;
      remember_flags(slot, core);
      if (air_jump && world->core.particle) world->core.particle(core->m_Pos, PARTICLE_TYPE_AIR_JUMP, i, world->core.user_data);
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
  if (world->replay_recording)
    emit_events(world, (ft_recording *)world->replay_recording, world->replay_tick, playback, player_count, effects_bound);
  dd_particles_finish(game, world, tick_before, effects_bound);
}

// --- drawing the recording's world ---------------------------------------------

// DDNet's CalcPos: where a projectile is `time` seconds after it started.
static void projectile_pos(const dd_state_projectile *p, const float *tuning, float time, float *out_x, float *out_y) {
  float curvature = 0.f, speed = 0.f;
  switch (p->type) {
  case WEAPON_GRENADE:
    curvature = tuning[23];
    speed = tuning[24];
    break;
  case WEAPON_SHOTGUN:
    curvature = tuning[19];
    speed = tuning[20];
    break;
  case WEAPON_GUN:
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

void dd_recording_render_entities(ft_game *game, const ft_world *world, float intra) {
  ft_recording *recording = (ft_recording *)world->replay_recording;
  if (!recording) return;
  const int tick = world->replay_tick;
  dd_state_tick *state = malloc(sizeof(*state));
  if (!state) return;
  float tuning[64];
  pthread_mutex_lock(&recording->lock);
  if (dd_demo_state_entities(recording->state, tick, state) && dd_state_tuning_count() <= 64) {
    for (int i = 0; i < state->num_projectiles; ++i) {
      const dd_state_projectile *p = &state->projectiles[i];
      if (!owner_shown(world, p->owner, true)) continue; // fired by a player that was not imported
      const int zone = p->tune_zone > 0 ? p->tune_zone : 0;
      if (!dd_demo_state_tuning(recording->state, tick, zone, tuning)) continue;
      // The world shows the recording tick `tick`; frames interpolate from the one before.
      float x0, y0, x1, y1;
      projectile_pos(p, tuning, (float)(tick - 1 - p->start_tick) / (float)GAME_TICK_SPEED, &x0, &y0);
      projectile_pos(p, tuning, (float)(tick - p->start_tick) / (float)GAME_TICK_SPEED, &x1, &y1);
      const vec2 from = {(x0 + MAP_EXPAND32) / PX_PER_TILE, (y0 + MAP_EXPAND32) / PX_PER_TILE};
      const vec2 to = {(x1 + MAP_EXPAND32) / PX_PER_TILE, (y1 + MAP_EXPAND32) / PX_PER_TILE};
      dd_render_projectile(game, from, to, intra, p->type, tick, i);
    }
    for (int i = 0; i < state->num_lasers; ++i) {
      const dd_state_laser *l = &state->lasers[i];
      if (!owner_shown(world, l->owner, true)) continue;
      if (!dd_demo_state_tuning(recording->state, tick, 0, tuning)) continue;
      const vec2 from = {(l->from_x + MAP_EXPAND32) / PX_PER_TILE, (l->from_y + MAP_EXPAND32) / PX_PER_TILE};
      const vec2 to = {(l->to_x + MAP_EXPAND32) / PX_PER_TILE, (l->to_y + MAP_EXPAND32) / PX_PER_TILE};
      // DDNet's own laser types (doors, draggers, freeze) are drawn like rifle shots.
      const bool shotgun = l->type == WEAPON_SHOTGUN;
      dd_render_laser(game, from, to, !shotgun, (float)(tick - 1 - l->start_tick) + intra, tuning[27], tick, intra);
    }
  }
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
