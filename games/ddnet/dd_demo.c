#include "dd_internal.h"
#include "dd_profile.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDNET_DEMO_IMPLEMENTATION
#include <ddnet_demo/ddnet_demo.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// SHA-256 Implementation
// Necessary for creating a valid demo header
typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

#define DBL_INT_ADD(a, b, c)     \
  if (a > 0xffffffff - (c)) ++b; \
  a += c;
#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
                               0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                               0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                               0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                               0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
                               0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                               0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void map_sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
  uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
  for (i = 0, j = 0; i < 16; ++i, j += 4)
    m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
  for (; i < 64; ++i)
    m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];
  for (i = 0; i < 64; ++i) {
    t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
    t2 = EP0(a) + MAJ(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void map_sha256_init(SHA256_CTX *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
}

void map_sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
  for (size_t i = 0; i < len; ++i) {
    ctx->data[ctx->datalen] = data[i];
    ctx->datalen++;
    if (ctx->datalen == 64) {
      map_sha256_transform(ctx, ctx->data);
      DBL_INT_ADD(ctx->bitlen, ctx->bitlen, 512);
      ctx->datalen = 0;
    }
  }
}

void map_sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
  uint32_t i = ctx->datalen;
  if (ctx->datalen < 56) {
    ctx->data[i++] = 0x80;
    while (i < 56)
      ctx->data[i++] = 0x00;
  } else {
    ctx->data[i++] = 0x80;
    while (i < 64)
      ctx->data[i++] = 0x00;
    map_sha256_transform(ctx, ctx->data);
    memset(ctx->data, 0, 56);
  }
  DBL_INT_ADD(ctx->bitlen, ctx->bitlen, ctx->datalen * 8);
  ctx->data[63] = ctx->bitlen;
  ctx->data[62] = ctx->bitlen >> 8;
  ctx->data[61] = ctx->bitlen >> 16;
  ctx->data[60] = ctx->bitlen >> 24;
  ctx->data[59] = ctx->bitlen >> 32;
  ctx->data[58] = ctx->bitlen >> 40;
  ctx->data[57] = ctx->bitlen >> 48;
  ctx->data[56] = ctx->bitlen >> 56;
  map_sha256_transform(ctx, ctx->data);
  for (i = 0; i < 4; ++i) {
    hash[i] = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 4] = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 8] = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
    hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
  }
}

// CRC32 Implementation
uint32_t map_crc32_for_byte(uint32_t r) {
  for (int j = 0; j < 8; ++j)
    r = (r & 1 ? 0 : (uint32_t)0xEDB88320L) ^ r >> 1;
  return r ^ (uint32_t)0xFF000000L;
}

uint32_t map_crc32(const void *data, size_t n_bytes) {
  static uint32_t table[0x100];
  if (!*table)
    for (size_t i = 0; i < 0x100; ++i)
      table[i] = map_crc32_for_byte(i);
  uint32_t crc = 0;
  for (size_t i = 0; i < n_bytes; ++i)
    crc = table[(uint8_t)crc ^ ((uint8_t *)data)[i]] ^ crc >> 8;
  return crc;
}

void str_to_ints(int *pInts, size_t NumInts, const char *pStr) {
  const size_t StrSize = strlen(pStr) + 1;
  for (size_t i = 0; i < NumInts; i++) {
    char aBuf[sizeof(int)] = {0, 0, 0, 0};
    for (size_t c = 0; c < sizeof(int) && i * sizeof(int) + c < StrSize; c++)
      aBuf[c] = pStr[i * sizeof(int) + c];
    pInts[i] = (int)(((uint32_t)(uint8_t)(aBuf[0] + 128) << 24) |
                     ((uint32_t)(uint8_t)(aBuf[1] + 128) << 16) |
                     ((uint32_t)(uint8_t)(aBuf[2] + 128) << 8) |
                     (uint32_t)(uint8_t)(aBuf[3] + 128));
  }
  pInts[NumInts - 1] &= 0xFFFFFF00;
}

int round_to_int(float f) {
  if (f >= 0.0f) return (int)(f + 0.5f);
  else return (int)(f - 0.5f);
}

static int remap_client_id(const int *client_ids, int client_count, int local_id) {
  if (local_id < 0) return local_id;
  return local_id < client_count ? client_ids[local_id] : -1;
}

static int demo_character_emote(const ft_world *world, int player, const ddnet_character_t *character) {
  static const int emotes_by_eye[NUM_EYES] = {
      [EYE_NORMAL] = DD_EMOTE_NORMAL,
      [EYE_ANGRY] = DD_EMOTE_ANGRY,
      [EYE_PAIN] = DD_EMOTE_PAIN,
      [EYE_HAPPY] = DD_EMOTE_HAPPY,
      [EYE_BLINK] = DD_EMOTE_BLINK,
      [EYE_SURPRISE] = DD_EMOTE_SURPRISE,
  };
  const int client_id = ddnet_player_client(world, player);
  int eye = get_flag_eye_state(&world->inputs[client_id]);
  if (eye < EYE_NORMAL || eye >= NUM_EYES) eye = EYE_NORMAL;
  if (character->freeze_time > 0 && eye == EYE_NORMAL) eye = EYE_BLINK;
  const int pain_age = world->core.tick - world->pain_ticks[client_id];
  if (pain_age >= 0 && pain_age < GAME_TICK_SPEED / 2) eye = EYE_PAIN;
  return emotes_by_eye[eye];
}

#define DD_DEMO_VIEW_CLIP_X 4000.0f
#define DD_DEMO_VIEW_CLIP_Y 3000.0f
#define DD_DEMO_MAX_SNAPPED_DOORS 500
#define DD_DEMO_MAX_SNAPPED_PICKUPS 500

static bool is_point_in_view(float px, float py, const ddnet_vec2_t *positions, int count, float clip_x, float clip_y) {
  for (int i = 0; i < count; ++i) {
    float dx = fabsf(px - positions[i].x);
    float dy = fabsf(py - positions[i].y);
    if (dx <= clip_x && dy <= clip_y) {
      return true;
    }
  }
  return false;
}

// The pickups of a level: every world of it has these in its pickup list, in this order.
static int level_pickup_count(const ft_level *level) {
  int count = 0;
  for (int i = level->prototype.first_entity[DDNET_ENTTYPE_PICKUP]; i != -1; i = level->prototype.entities[i].link.next)
    count += level->prototype.entities[i].kind == DDNET_ENTITY_PICKUP;
  return count;
}

// The demo client of a world client (a player of the world), or -1.
static int remap_world_client(const ft_world *world, const int *client_ids, int client_count, int world_client) {
  if (world_client < 0) return world_client;
  return remap_client_id(client_ids, client_count, ddnet_client_player(world, world_client));
}

static void snap_laser(dd_netobj_ddnet_laser *l, ddnet_vec2_t to, ddnet_vec2_t from, int start_tick, int owner, int type, int subtype,
                       int number) {
  l->m_ToX = (int)to.x;
  l->m_ToY = (int)to.y;
  l->m_FromX = (int)from.x;
  l->m_FromY = (int)from.y;
  l->m_StartTick = start_tick;
  l->m_Owner = owner;
  l->m_Type = type;
  l->m_Subtype = subtype;
  l->m_SwitchNumber = number;
  l->m_Flags = 0;
}

static bool snap_world(dd_snapshot_builder *sb, ft_game *game, int world_index, const int *client_ids, int client_count,
                       const int *client_options, const ft_world *prev_world, const ft_world *current_world, bool include_static,
                       const ddnet_vec2_t *active_positions, int active_pos_count, int demo_tick, int tick_delta, int *next_item_id) {
  (void)client_options;
  const ddnet_world_t *cur = &current_world->core;
  const ft_level *level = game->current_level;
  int first_exported_local = -1;
  for (int i = 0; i < client_count; ++i) {
    if (client_ids[i] >= 0) {
      first_exported_local = i;
      break;
    }
  }
  const ddnet_character_t *view_character = ddnet_player_character(current_world, first_exported_local);
  // The switch states a demo shows are those of the team of the first player in it.
  const int view_client = ddnet_player_client(current_world, first_exported_local);
  const int view_team = view_client >= 0 ? cur->players[view_client].team : 0;

  ddnet_vec2_t fallback_pos[1];
  if (active_pos_count <= 0 && view_character) {
    fallback_pos[0] = view_character->pos;
    active_positions = fallback_pos;
    active_pos_count = 1;
  }

  // do pickups first since they have static ids basically
  const int num_pickups = level ? level_pickup_count(level) : 0;
  int pickups_snapped = 0, pickup_index = 0;
  for (int i = include_static ? cur->first_entity[DDNET_ENTTYPE_PICKUP] : -1; i != -1; i = cur->entities[i].link.next) {
    const ddnet_entity_t *pickup = &cur->entities[i];
    if (pickup->kind != DDNET_ENTITY_PICKUP) continue;
    const int id = 64 + pickup_index++;
    if (pickups_snapped >= DD_DEMO_MAX_SNAPPED_PICKUPS) break;
    if (num_pickups > 128) {
      if (active_pos_count > 0) {
        if (!is_point_in_view(pickup->pos.x, pickup->pos.y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X,
                              DD_DEMO_VIEW_CLIP_Y)) {
          continue;
        }
      } else if (pickups_snapped >= 64) {
        break;
      }
    }
    dd_netobj_ddnet_pickup *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPICKUP, id, sizeof(dd_netobj_ddnet_pickup));
    if (!p) break;
    pickups_snapped++;
    p->m_X = (int)pickup->pos.x;
    p->m_Y = (int)pickup->pos.y;
    p->m_Type = pickup->u.pickup.type;
    p->m_Subtype = pickup->u.pickup.subtype;
    p->m_SwitchNumber = pickup->number;
    p->m_Flags = 0;
  }

  // do doors and switch state
  if (include_static && level) {
    const ddnet_collision_t *collision = &level->collision;

    int doors_snapped = 0;
    for (int i = 0; i < collision->num_doors; ++i) {
      if (doors_snapped >= DD_DEMO_MAX_SNAPPED_DOORS) break;
      const ddnet_map_door_t *door = &collision->doors[i];
      if (collision->num_doors > 64) {
        if (active_pos_count > 0) {
          const float mid_x = 0.5f * (door->pos.x + door->to.x);
          const float mid_y = 0.5f * (door->pos.y + door->to.y);
          if (!is_point_in_view(door->pos.x, door->pos.y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X, DD_DEMO_VIEW_CLIP_Y) &&
              !is_point_in_view(door->to.x, door->to.y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X, DD_DEMO_VIEW_CLIP_Y) &&
              !is_point_in_view(mid_x, mid_y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X, DD_DEMO_VIEW_CLIP_Y)) {
            continue;
          }
        } else if (doors_snapped >= 64) {
          break;
        }
      }
      const int door_id = 64 + num_pickups + i;
      dd_netobj_ddnet_laser *l = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETLASER, door_id, sizeof(dd_netobj_ddnet_laser));
      if (!l) break;
      doors_snapped++;
      // CDoor::Snap for a client with DDNet's entity objects: from where it ends, whatever the switch;
      // the client shows it by the switch states.
      snap_laser(l, (ddnet_vec2_t){(float)round_to_int(door->pos.x), (float)round_to_int(door->pos.y)},
                 (ddnet_vec2_t){(float)round_to_int(door->to.x), (float)round_to_int(door->to.y)}, -1, -1, DD_LASERTYPE_DOOR, 0,
                 door->number);
    }
    if (cur->num_switchers > 0) {
      dd_netobj_switch_state *ss = demo_sb_add_item(sb, DD_NETOBJTYPE_SWITCHSTATE, 0, sizeof(dd_netobj_switch_state));
      if (ss) {
        memset(ss, 0, sizeof(*ss));
        ss->m_HighestSwitchNumber = cur->num_switchers - 1;
        if (ss->m_HighestSwitchNumber > 255) ss->m_HighestSwitchNumber = 255;
        for (int i = 0; i <= ss->m_HighestSwitchNumber; ++i) {
          if (ddnet_world_switch(cur, i, view_team).status) {
            ss->m_aStatus[i / 32] |= (1 << (i % 32));
          }
        }
        int num_timed = 0;
        for (int i = 0; i <= ss->m_HighestSwitchNumber && num_timed < 4; ++i) {
          const int end_tick = ddnet_world_switch(cur, i, view_team).end_tick;
          if (end_tick > 0 && end_tick < cur->tick + 3 * 50) {
            ss->m_aSwitchNumbers[num_timed] = i;
            ss->m_aEndTicks[num_timed] = end_tick + tick_delta;
            num_timed++;
          }
        }
      }
    }
  }

  // game info
  dd_netobj_game_info *game_info = include_static ? demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEINFO, 0, sizeof(dd_netobj_game_info)) : NULL;
  if (game_info) *game_info = (dd_netobj_game_info){0};
  if (game_info && view_character && view_character->race_state != DDNET_RACE_NONE) {
    game_info->m_WarmupTimer = -(view_character->start_time + tick_delta);
    game_info->m_GameStateFlags = DD_GAMESTATEFLAG_RACETIME;
  }
  dd_netobj_game_info_ex *game_info_ex = include_static ? demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEINFOEX, 0, sizeof(dd_netobj_game_info_ex)) : NULL;
  if (game_info_ex) {
    game_info_ex->m_Version = 10;
    game_info_ex->m_Flags = DD_GAMEINFOFLAG_TIMESCORE | DD_GAMEINFOFLAG_GAMETYPE_RACE | DD_GAMEINFOFLAG_GAMETYPE_DDRACE |
                            DD_GAMEINFOFLAG_GAMETYPE_DDNET | DD_GAMEINFOFLAG_UNLIMITED_AMMO | DD_GAMEINFOFLAG_RACE_RECORD_MESSAGE |
                            DD_GAMEINFOFLAG_ALLOW_EYE_WHEEL | DD_GAMEINFOFLAG_ALLOW_HOOK_COLL | DD_GAMEINFOFLAG_ALLOW_ZOOM |
                            DD_GAMEINFOFLAG_BUG_DDRACE_GHOST | DD_GAMEINFOFLAG_BUG_DDRACE_INPUT | DD_GAMEINFOFLAG_PREDICT_DDRACE |
                            DD_GAMEINFOFLAG_PREDICT_DDRACE_TILES | DD_GAMEINFOFLAG_ENTITIES_DDNET | DD_GAMEINFOFLAG_ENTITIES_DDRACE |
                            DD_GAMEINFOFLAG_ENTITIES_RACE | DD_GAMEINFOFLAG_RACE;
    game_info_ex->m_Flags2 = DD_GAMEINFOFLAG2_HUD_DDRACE;
  }

  for (int p = 0; p < current_world->player_count; ++p) {
    if (p >= client_count || client_ids[p] < 0) continue;
    const int client_id = client_ids[p];
    const int track_index = game->engine->timeline_player_track((uint32_t)world_index, (uint32_t)p);
    if (track_index < 0) continue;
    dd_player_profile_t profile;
    dd_profile_for_track(game, track_index, &profile);
    const ddnet_character_t *c_cur = ddnet_player_character(current_world, p);
    const int world_client = ddnet_player_client(current_world, p);
    const bool paused = dd_replay_paused(current_world, p);

    dd_netobj_client_info *ci = demo_sb_add_item(sb, DD_NETOBJTYPE_CLIENTINFO, client_id, sizeof(dd_netobj_client_info));
    if (ci) {
      char name[sizeof(profile.name)];
      if (profile.name[0]) snprintf(name, sizeof(name), "%s", profile.name);
      else dd_profile_display_name(game, track_index, name, sizeof(name));
      str_to_ints(ci->m_aName, 4, name);
      str_to_ints(ci->m_aClan, 3, profile.clan);
      str_to_ints(ci->m_aSkin, 6, profile.skin[0] ? profile.skin : "default");
      ci->m_Country = 0;
      // The profile already holds what the protocol wants: DDNet's own packed
      // hue/saturation/lightness, never converted to anything else on the way.
      ci->m_UseCustomColor = profile.use_custom_color ? 1 : 0;
      ci->m_ColorBody = profile.use_custom_color ? profile.color_body : 0;
      ci->m_ColorFeet = profile.use_custom_color ? profile.color_feet : 0;
    }

    dd_netobj_player_info *pi = demo_sb_add_item(sb, DD_NETOBJTYPE_PLAYERINFO, client_id, sizeof(dd_netobj_player_info));
    if (pi) {
      pi->m_Latency = client_options ? client_options[client_id] : 0;
      pi->m_Score = -9999;
      pi->m_Local = 0;
      pi->m_ClientId = client_id;
      pi->m_Team = 0;
    }

    dd_netobj_ddnet_player *dp = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPLAYER, client_id, sizeof(dd_netobj_ddnet_player));
    if (dp) {
      dp->m_AuthLevel = 0;
      dp->m_Flags = paused ? DD_EXPLAYERFLAG_SPEC : 0;
    }

    // Replayed /spec players only have a waiting position. Export it as DDNet
    // does, rather than turning the placeholder physics character into a tee.
    if (paused && c_cur) {
      dd_netobj_spec_char *spec = demo_sb_add_item(sb, DD_NETOBJTYPE_SPECCHAR, client_id, sizeof(*spec));
      if (spec) {
        spec->m_X = round_to_int(c_cur->pos.x);
        spec->m_Y = round_to_int(c_cur->pos.y);
      }
      continue;
    }
    if (!c_cur || dd_replay_absent(current_world, p)) continue;
    const ddnet_character_core_t *core = &c_cur->core;

    dd_netobj_character *ch = demo_sb_add_item(sb, DD_NETOBJTYPE_CHARACTER, client_id, sizeof(dd_netobj_character));
    if (ch) {
      memset(ch, 0, sizeof(*ch));
      ch->core.m_X = round_to_int(c_cur->pos.x);
      ch->core.m_Y = round_to_int(c_cur->pos.y);
      ch->core.m_VelX = round_to_int(core->vel.x * 256.0f);
      ch->core.m_VelY = round_to_int(core->vel.y * 256.0f);
      ch->core.m_HookState = core->hook_state;
      ch->core.m_HookTick = core->hook_tick;
      ch->core.m_HookX = round_to_int(core->hook_pos.x);
      ch->core.m_HookY = round_to_int(core->hook_pos.y);
      ch->core.m_HookDx = round_to_int(core->hook_dir.x * 256.0f);
      ch->core.m_HookDy = round_to_int(core->hook_dir.y * 256.0f);
      ch->core.m_HookedPlayer = remap_world_client(current_world, client_ids, client_count, core->hooked_player);
      ch->core.m_Jumped = core->jumped;
      ch->core.m_Direction = core->direction;
      ch->core.m_Angle = ddnet_character_angle(c_cur);

      // Physics groups run on local clocks, while a demo has one shared clock. Every absolute tick
      // written to the protocol must be translated or the client predicts offset groups far away.
      ch->core.m_Tick = demo_tick;
      ch->m_Emote = demo_character_emote(current_world, p, c_cur);

      ch->m_AttackTick = c_cur->attack_tick + tick_delta;
      ch->m_Weapon = (core->deep_frozen || c_cur->freeze_time > 0) ? DDNET_WEAPON_NINJA : core->active_weapon;
      ch->m_AmmoCount = 0;
      ch->m_Health = 10;
      ch->m_Armor = 10;
      ch->m_PlayerFlags = 0;
    }

    dd_netobj_ddnet_character *dc = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETCHARACTER, client_id, sizeof(dd_netobj_ddnet_character));
    if (dc) {
      memset(dc, 0, sizeof(*dc));
      dc->m_TuneZoneOverride = -1;
      dc->m_Flags = 0;
      if (core->solo) dc->m_Flags |= DD_CHARACTERFLAG_SOLO;
      if (core->endless_hook) dc->m_Flags |= DD_CHARACTERFLAG_ENDLESS_HOOK;
      if (core->collision_disabled) dc->m_Flags |= DD_CHARACTERFLAG_COLLISION_DISABLED;
      if (core->hook_hit_disabled) dc->m_Flags |= DD_CHARACTERFLAG_HOOK_HIT_DISABLED;
      if (core->endless_jump) dc->m_Flags |= DD_CHARACTERFLAG_ENDLESS_JUMP;
      if (core->jetpack) dc->m_Flags |= DD_CHARACTERFLAG_JETPACK;
      if (core->hammer_hit_disabled) dc->m_Flags |= DD_CHARACTERFLAG_HAMMER_HIT_DISABLED;
      if (core->shotgun_hit_disabled) dc->m_Flags |= DD_CHARACTERFLAG_SHOTGUN_HIT_DISABLED;
      if (core->grenade_hit_disabled) dc->m_Flags |= DD_CHARACTERFLAG_GRENADE_HIT_DISABLED;
      if (core->laser_hit_disabled) dc->m_Flags |= DD_CHARACTERFLAG_LASER_HIT_DISABLED;
      if (core->has_telegun_gun) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_GUN;
      if (core->has_telegun_grenade) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_GRENADE;
      if (core->has_telegun_laser) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_LASER;
      if (core->weapons[DDNET_WEAPON_HAMMER].got) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_HAMMER;
      if (core->weapons[DDNET_WEAPON_GUN].got) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_GUN;
      if (core->weapons[DDNET_WEAPON_SHOTGUN].got) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_SHOTGUN;
      if (core->weapons[DDNET_WEAPON_GRENADE].got) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_GRENADE;
      if (core->weapons[DDNET_WEAPON_LASER].got) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_LASER;
      if (core->active_weapon == DDNET_WEAPON_NINJA) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_NINJA;
      if (core->live_frozen) dc->m_Flags |= DD_CHARACTERFLAG_MOVEMENTS_DISABLED;

      dc->m_Jumps = core->jumps;
      dc->m_TeleCheckpoint = c_cur->tele_checkpoint;
      dc->m_StrongWeakId = c_cur->strong_weak_id;
      dc->m_JumpedTotal = core->jumped_total;
      dc->m_NinjaActivationTick = core->ninja.activation_tick + tick_delta;

      dc->m_FreezeStart = core->freeze_start == 0 ? 0 : core->freeze_start + tick_delta;
      dc->m_FreezeEnd = core->deep_frozen ? -1 : c_cur->freeze_time == 0 ? 0 : demo_tick + c_cur->freeze_time;

      if (core->is_in_freeze) {
        dc->m_Flags |= DD_CHARACTERFLAG_IN_FREEZE;
      }
      dc->m_TargetX = core->input.target_x;
      dc->m_TargetY = core->input.target_y;
    }

    // CreateFinishEffect, which is when DDNet's server sends it
    if (prev_world && world_client >= 0 && ddnet_player_client(prev_world, p) == world_client &&
        prev_world->core.players[world_client].finish_tick < 0 && cur->players[world_client].finish_tick >= 0) {
      dd_netevent_finish *nf = demo_sb_add_item(sb, DD_NETEVENTTYPE_FINISH, (*next_item_id)++, sizeof(dd_netevent_finish));
      if (nf) {
        nf->common.m_X = (int)c_cur->pos.x;
        nf->common.m_Y = (int)c_cur->pos.y;
      }
    }
  }

  // Snapshot events come from the callbacks raised by the physics step. They
  // must not be reconstructed from the surviving entities below: an explosive
  // projectile can be spawned, hit a wall and be removed in the same tick.
  for (int i = 0; i < current_world->physics_particle_event_count; ++i) {
    const dd_physics_particle_event_t *event = &current_world->physics_particle_events[i];
    const int client_id = remap_client_id(client_ids, client_count, event->client_id);
    if (event->client_id >= 0 && client_id < 0) continue;
    const int x = (int)event->x;
    const int y = (int)event->y;
    switch (event->type) {
    case DDNET_PARTICLE_PLAYER_SPAWN: {
      dd_netevent_spawn *spawn = demo_sb_add_item(sb, DD_NETEVENTTYPE_SPAWN, (*next_item_id)++, sizeof(*spawn));
      if (spawn) {
        spawn->common.m_X = x;
        spawn->common.m_Y = y;
      }
      break;
    }
    case DDNET_PARTICLE_PLAYER_DEATH: {
      dd_netevent_death *death = demo_sb_add_item(sb, DD_NETEVENTTYPE_DEATH, (*next_item_id)++, sizeof(*death));
      if (death) {
        death->common.m_X = x;
        death->common.m_Y = y;
        death->m_ClientId = client_id;
      }
      break;
    }
    case DDNET_PARTICLE_HAMMER_HIT: {
      dd_netevent_hammer_hit *hit = demo_sb_add_item(sb, DD_NETEVENTTYPE_HAMMERHIT, (*next_item_id)++, sizeof(*hit));
      if (hit) {
        hit->common.m_X = x;
        hit->common.m_Y = y;
      }
      break;
    }
    case DDNET_PARTICLE_EXPLOSION: {
      dd_netevent_explosion *explosion = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, (*next_item_id)++, sizeof(*explosion));
      if (explosion) {
        explosion->common.m_X = x;
        explosion->common.m_Y = y;
      }
      break;
    }
    default:
      break;
    }
  }
  for (int i = 0; i < current_world->physics_sound_event_count; ++i) {
    const dd_physics_sound_event_t *event = &current_world->physics_sound_events[i];
    if (event->client_side) continue;
    if (event->client_id >= 0 && remap_client_id(client_ids, client_count, event->client_id) < 0) continue;
    dd_netevent_sound_world *sound =
        demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(*sound));
    if (sound) {
      sound->common.m_X = (int)event->x;
      sound->common.m_Y = (int)event->y;
      sound->m_SoundId = event->sound_id;
    }
  }
  // one event per star, at its final angle (CGameContext::CreateDamageInd)
  for (int i = 0; i < current_world->physics_damage_event_count; ++i) {
    const dd_physics_damage_event_t *event = &current_world->physics_damage_events[i];
    if (event->client_id >= 0 && remap_client_id(client_ids, client_count, event->client_id) < 0) continue;
    dd_netevent_damage_ind *damage = demo_sb_add_item(sb, DD_NETEVENTTYPE_DAMAGEIND, (*next_item_id)++, sizeof(*damage));
    if (damage) {
      damage->common.m_X = (int)event->x;
      damage->common.m_Y = (int)event->y;
      damage->m_Angle = (int)(event->angle * 256.0f);
    }
  }

  // do entities
  for (int i = cur->first_entity[DDNET_ENTTYPE_PROJECTILE]; i != -1; i = cur->entities[i].link.next) {
    const ddnet_entity_t *ent = &cur->entities[i];
    const ddnet_projectile_t *proj = &ent->u.projectile;
    const int owner = remap_world_client(current_world, client_ids, client_count, proj->owner);
    if (proj->owner >= 0 && owner < 0) continue;
    dd_netobj_ddnet_projectile *p =
        demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPROJECTILE, (*next_item_id)++, sizeof(dd_netobj_ddnet_projectile));
    if (p) {
      int Flags = 0;
      if (proj->bouncing & 1) Flags |= DD_PROJECTILEFLAG_BOUNCE_HORIZONTAL;
      if (proj->bouncing & 2) Flags |= DD_PROJECTILEFLAG_BOUNCE_VERTICAL;
      if (proj->explosive) Flags |= DD_PROJECTILEFLAG_EXPLOSIVE;
      if (proj->freeze) Flags |= DD_PROJECTILEFLAG_FREEZE;
      Flags |= DD_PROJECTILEFLAG_NORMALIZE_VEL;
      p->m_VelX = round_to_int(proj->direction.x * 1e6f);
      p->m_VelY = round_to_int(proj->direction.y * 1e6f);
      p->m_X = round_to_int(ent->pos.x * 100.0f);
      p->m_Y = round_to_int(ent->pos.y * 100.0f);
      p->m_Type = proj->type;
      p->m_StartTick = proj->start_tick + tick_delta;
      p->m_Owner = owner;
      p->m_Flags = Flags;
      p->m_SwitchNumber = ent->number;
      p->m_TuneZone = 0;
    }
  }

  // The laser list as DDNet's server snaps it for a demo (CLaser, CLight, CGun, CPlasma, CDragger and
  // CDraggerBeam::Snap for SERVER_DEMO_CLIENT, with DDNet's entity objects).
  for (int i = cur->first_entity[DDNET_ENTTYPE_LASER]; i != -1; i = cur->entities[i].link.next) {
    const ddnet_entity_t *ent = &cur->entities[i];
    int owner = -1, type = -1, subtype = 0, start_tick = -1;
    ddnet_vec2_t to = ent->pos, from = ent->pos;
    switch (ent->kind) {
    case DDNET_ENTITY_LASER:
      owner = remap_world_client(current_world, client_ids, client_count, ent->u.laser.owner);
      if (ent->u.laser.owner >= 0 && owner < 0) continue;
      from = ent->u.laser.from;
      start_tick = ent->u.laser.eval_tick + tick_delta;
      type = ent->u.laser.type == DDNET_WEAPON_LASER ? DD_LASERTYPE_RIFLE : DD_LASERTYPE_SHOTGUN;
      subtype = -1;
      break;
    case DDNET_ENTITY_LIGHT:
      // light on game and switch layer with a number 0 is always on; the demo client has no team
      if (ent->number == 0) from = ent->u.light.to;
      type = DD_LASERTYPE_FREEZE;
      break;
    case DDNET_ENTITY_GUN:
      type = DD_LASERTYPE_GUN;
      subtype = (ent->u.gun.explosive ? 1 : 0) | (ent->u.gun.freeze ? 2 : 0);
      break;
    case DDNET_ENTITY_PLASMA:
      owner = remap_world_client(current_world, client_ids, client_count, ent->u.plasma.for_client_id);
      if (owner < 0) continue;
      type = DD_LASERTYPE_PLASMA;
      subtype = (ent->u.plasma.explosive ? 1 : 0) | (ent->u.plasma.freeze ? 2 : 0);
      start_tick = ent->u.plasma.eval_tick + tick_delta;
      break;
    case DDNET_ENTITY_DRAGGER: {
      const int strength = (int)lroundf(ent->u.dragger.strength - 1.f);
      type = DD_LASERTYPE_DRAGGER;
      subtype = (ent->u.dragger.ignore_walls ? 1 : 0) | ((strength < 0 ? 0 : strength > 2 ? 2 : strength) << 1);
      break;
    }
    case DDNET_ENTITY_DRAGGER_BEAM: {
      const ddnet_dragger_beam_t *beam = &ent->u.dragger_beam;
      const ddnet_character_t *target = beam->active ? ddnet_world_character((ddnet_world_t *)cur, beam->for_client_id) : NULL;
      owner = remap_world_client(current_world, client_ids, client_count, beam->for_client_id);
      if (!target || owner < 0) continue;
      const float dx = target->pos.x - ent->pos.x, dy = target->pos.y - ent->pos.y;
      if (sqrtf(dx * dx + dy * dy) >= (float)cur->config.sv_dragger_range) continue;
      to = target->pos;
      from = ent->pos;
      const int strength = (int)lroundf(beam->strength - 1.f);
      type = DD_LASERTYPE_DRAGGER;
      subtype = (beam->ignore_walls ? 1 : 0) | ((strength < 0 ? 0 : strength > 2 ? 2 : strength) << 1);
      break;
    }
    default:
      continue;
    }
    if (active_pos_count > 0 && ent->kind != DDNET_ENTITY_LASER &&
        !is_point_in_view(to.x, to.y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X, DD_DEMO_VIEW_CLIP_Y) &&
        !is_point_in_view(from.x, from.y, active_positions, active_pos_count, DD_DEMO_VIEW_CLIP_X, DD_DEMO_VIEW_CLIP_Y))
      continue;
    dd_netobj_ddnet_laser *l = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETLASER, (*next_item_id)++, sizeof(dd_netobj_ddnet_laser));
    if (l) snap_laser(l, to, from, start_tick, owner, type, subtype, ent->number);
  }
  return dd_recording_snap_entities(current_world, sb, client_ids, client_count, demo_tick, next_item_id);
}

static bool request_includes_track(const ft_export_request *request, int32_t track) {
  if (!request->players || request->player_count == 0) return true;
  for (uint32_t i = 0; i < request->player_count; ++i)
    if (request->players[i] == track) return true;
  return false;
}

static int request_player_option(const ft_export_request *request, const int32_t *player_options, int32_t track) {
  if (!request->players || !player_options) return 0;
  for (uint32_t i = 0; i < request->player_count; ++i)
    if (request->players[i] == track) return player_options[i];
  return 0;
}

static bool world_has_exported_client(const int *client_ids, int client_count) {
  for (int i = 0; i < client_count; ++i)
    if (client_ids[i] >= 0) return true;
  return false;
}

static bool write_timeline_event(dd_demo_writer *writer, const dd_event_payload_t *event, const int *client_ids, int client_count) {
  const int client_id = remap_client_id(client_ids, client_count, event->client_id);
  const int killer = remap_client_id(client_ids, client_count, event->killer);
  const int victim = remap_client_id(client_ids, client_count, event->victim);
  switch ((dd_event_type_t)event->type) {
  case DD_EVENT_CHAT:
    if (event->client_id >= 0 && client_id < 0) return false;
    demo_w_write_msg_sv_chat(writer, event->team, client_id, event->message);
    break;
  case DD_EVENT_BROADCAST:
    demo_w_write_msg_sv_broadcast(writer, event->message);
    break;
  case DD_EVENT_KILLMSG:
    if ((event->killer >= 0 && killer < 0) || (event->victim >= 0 && victim < 0)) return false;
    demo_w_write_msg_sv_killmsg(writer, killer, victim, event->weapon, event->mode_special);
    break;
  case DD_EVENT_SOUND_GLOBAL:
    demo_w_write_msg_sv_sound_global(writer, event->sound_id);
    break;
  case DD_EVENT_EMOTICON:
    if (event->client_id >= 0 && client_id < 0) return false;
    demo_w_write_msg_sv_emoticon(writer, client_id, event->emoticon);
    break;
  case DD_EVENT_VOTE_SET:
    demo_w_write_msg_sv_vote_set(writer, event->vote_timeout, event->message, event->reason);
    break;
  case DD_EVENT_VOTE_STATUS:
    demo_w_write_msg_sv_vote_status(writer, event->vote_yes, event->vote_no, event->vote_pass, event->vote_total);
    break;
  case DD_EVENT_DDRACE_TIME: {
    demo_w_write_msg_sv_ddrace_time_legacy(writer, event->time, event->check, event->finish);
    int race_client = -1;
    for (int i = 0; i < client_count; ++i) {
      if (client_ids[i] >= 0) {
        race_client = client_ids[i];
        break;
      }
    }
    if (race_client >= 0) demo_w_write_msg_sv_racefinish(writer, race_client, event->time * 10, 0, 1, 1);
    break;
  }
  case DD_EVENT_RECORD:
    demo_w_write_msg_sv_record_legacy(writer, event->server_time_best, event->player_time_best);
    demo_w_write_msg_sv_record(writer, event->server_time_best, event->player_time_best);
    break;
  default:
    return false;
  }
  return true;
}

// CGameContext::SendTuningParams: every parameter, in hundredths, in DDNet's order.
static void write_sv_tuneparams(dd_demo_writer *writer, const ddnet_tuning_t *tuning) {
  char buffer[DD_MAX_MESSAGE_SIZE];
  dd_msg_packer packer;
  demo_msg_init(&packer, buffer, sizeof(buffer));
  demo_msg_add_int(&packer, DD_NETMSGTYPE_SV_TUNEPARAMS << 1);
  const ddnet_tune_param_t *params = (const ddnet_tune_param_t *)tuning;
  for (int i = 0; i < DDNET_NUM_TUNING_PARAMS; ++i)
    demo_msg_add_int(&packer, params[i]);

  int size = demo_msg_finish(&packer);
  if (size >= 0) {
    demo_w_write_msg(writer, buffer, size);
  }
}

static void free_client_maps(int **maps, int *counts, uint32_t world_count) {
  if (maps)
    for (uint32_t i = 0; i < world_count; ++i)
      free(maps[i]);
  free(maps);
  free(counts);
}

static bool dd_demo_export_impl(ft_game *game, const ft_export_request *request, const int32_t *player_options) {
  if (!game || !request || !request->path || !game->current_level || !game->engine->timeline_world_count ||
      !game->engine->timeline_world_info || !game->engine->timeline_world_pair || !game->engine->timeline_player_track)
    return false;

  const int start_tick = request->start_tick;
  const int end_tick = request->end_tick;
  if (end_tick < start_tick) return false;

  const uint32_t world_count = game->engine->timeline_world_count();
  int **client_maps = calloc(world_count ? world_count : 1, sizeof(*client_maps));
  int *client_counts = calloc(world_count ? world_count : 1, sizeof(*client_counts));
  ft_timeline_world_info *worlds = calloc(world_count ? world_count : 1, sizeof(*worlds));
  if (!client_maps || !client_counts || !worlds) {
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    return false;
  }

  int exported_clients = 0;
  int selected_clients = 0;
  int client_options[64] = {0};
  for (uint32_t world_index = 0; world_index < world_count; ++world_index) {
    worlds[world_index].struct_size = sizeof(worlds[world_index]);
    if (!game->engine->timeline_world_info(world_index, &worlds[world_index])) continue;
    client_counts[world_index] = (int)worlds[world_index].player_count;
    if (client_counts[world_index] > 0) {
      client_maps[world_index] = malloc((size_t)client_counts[world_index] * sizeof(**client_maps));
      if (!client_maps[world_index]) {
        free_client_maps(client_maps, client_counts, world_count);
        free(worlds);
        return false;
      }
    }
    for (int local = 0; local < client_counts[world_index]; ++local) {
      const int32_t track = game->engine->timeline_player_track(world_index, (uint32_t)local);
      const bool selected = track >= 0 && request_includes_track(request, track);
      if (selected) ++selected_clients;
      if (selected && exported_clients < 64) {
        client_options[exported_clients] = request_player_option(request, player_options, track);
        client_maps[world_index][local] = exported_clients++;
      } else {
        client_maps[world_index][local] = -1;
      }
    }
  }

  if (exported_clients == 0) {
    dd_log(game, FT_LOG_ERROR, "Demo export has no selected players.");
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    return false;
  }
  if (selected_clients > 64) {
    dd_log(game, FT_LOG_ERROR, "DDNet demos support at most 64 selected players.");
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    return false;
  }

  const void *map_data = game->current_level->map._map_file_data;
  const size_t map_size = game->current_level->map._map_file_size;
  if (!map_data || map_size == 0) {
    dd_log(game, FT_LOG_ERROR, "The loaded map has no source bytes to embed in the demo.");
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    return false;
  }

  const uint32_t map_crc = map_crc32(map_data, map_size);
  uint8_t map_sha256[32];
  SHA256_CTX hash = {0};
  map_sha256_init(&hash);
  map_sha256_update(&hash, map_data, map_size);
  map_sha256_final(&hash, map_sha256);

  const char *map_name = (game->current_level && game->current_level->name[0] && strcmp(game->current_level->name, "map") != 0)
                             ? game->current_level->name
                             : (game->engine && game->engine->get_level_name ? game->engine->get_level_name() : NULL);
  if (!map_name || !*map_name || strcmp(map_name, "unnamed_level") == 0) map_name = "unnamed_map";
  if (game->current_level && (!game->current_level->name[0] || strcmp(game->current_level->name, "map") == 0) &&
      strcmp(map_name, "unnamed_map") != 0) {
    snprintf(game->current_level->name, sizeof(game->current_level->name), "%s", map_name);
  }

  FILE *file = fopen(request->path, "wb");
  dd_demo_writer *writer = demo_w_create();
  dd_snapshot_builder *builder = demo_sb_create();
  if (!file || !writer || !builder ||
      !demo_w_begin(writer, file, map_name, map_crc, "Race") ||
      !demo_w_write_map(writer, map_sha256, map_data, map_size)) {
    if (builder) demo_sb_destroy(&builder);
    if (writer) demo_w_destroy(&writer);
    if (file) fclose(file);
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    dd_log(game, FT_LOG_ERROR, "Could not initialize the DDNet demo writer.");
    return false;
  }

  const ft_world **curr_worlds = (const ft_world **)calloc(world_count, sizeof(const ft_world *));
  const ft_world **prev_worlds = (const ft_world **)calloc(world_count, sizeof(const ft_world *));
  uint8_t snapshot[DD_SNAPSHOT_MAX_SIZE];
  bool ok = (curr_worlds && prev_worlds);
  const int tick_span = end_tick - start_tick + 1;
  // The worlds asked for below are stepped with their effects, which the snapshots carry.
  game->physics_events_forced = true;
  for (int tick = start_tick; ok && tick <= end_tick; ++tick) {
    demo_sb_clear(builder);
    int num_pickups = game->current_level ? level_pickup_count(game->current_level) : 0;
    int num_doors = game->current_level ? game->current_level->collision.num_doors : 0;
    int next_item_id = 64 + num_pickups + num_doors;

    ddnet_vec2_t active_positions[64];
    int active_pos_count = 0;
    for (uint32_t wi = 0; wi < world_count; ++wi) {
      curr_worlds[wi] = NULL;
      prev_worlds[wi] = NULL;
      if (!world_has_exported_client(client_maps[wi], client_counts[wi])) continue;
      if (!game->engine->timeline_world_pair(wi, tick, &prev_worlds[wi], &curr_worlds[wi]) || !prev_worlds[wi] || !curr_worlds[wi]) {
        ok = false;
        break;
      }
      for (int p = 0; p < curr_worlds[wi]->player_count && active_pos_count < 64; ++p) {
        const ddnet_character_t *chr = ddnet_player_character(curr_worlds[wi], p);
        if (chr && p < client_counts[wi] && client_maps[wi][p] >= 0) {
          active_positions[active_pos_count++] = chr->pos;
        }
      }
    }
    if (!ok) break;

    bool include_static = true;
    for (uint32_t world_index = 0; world_index < world_count; ++world_index) {
      if (!curr_worlds[world_index]) continue;

      if (!snap_world(builder, game, (int)world_index, client_maps[world_index], client_counts[world_index], client_options,
                 prev_worlds[world_index], curr_worlds[world_index], include_static, active_positions,
                 active_pos_count, tick - start_tick, worlds[world_index].start_offset - start_tick, &next_item_id)) {
        ok = false;
        break;
      }
      include_static = false;
    }
    if (!ok) break;

    const int snapshot_size = demo_sb_finish(builder, snapshot);
    if (snapshot_size <= 0 || !demo_w_write_snap(writer, tick - start_tick, snapshot, snapshot_size)) {
      ok = false;
      break;
    }

    if (game->current_level && tick == start_tick) {
      write_sv_tuneparams(writer, &game->current_level->prototype.tuning[0]);
    }

    // Authored protocol messages are stored by the engine as opaque DDNet
    // payloads. Translate their local group clocks to the demo's global clock
    // and write them immediately after the matching snapshot, as DDNet does.
    if (game->engine->timeline_event_count && game->engine->timeline_event_get) {
      const uint32_t event_count = game->engine->timeline_event_count();
      for (uint32_t event_index = 0; event_index < event_count; ++event_index) {
        ft_timeline_event timeline_event = {.struct_size = sizeof(timeline_event)};
        dd_event_payload_t payload;
        if (!game->engine->timeline_event_get(event_index, &timeline_event) || !dd_event_decode(&timeline_event, &payload) ||
            timeline_event.world_index < 0 || (uint32_t)timeline_event.world_index >= world_count)
          continue;
        const int world_index = timeline_event.world_index;
        if (!world_has_exported_client(client_maps[world_index], client_counts[world_index])) continue;
        if (timeline_event.tick + worlds[world_index].start_offset != tick) continue;
        write_timeline_event(writer, &payload, client_maps[world_index], client_counts[world_index]);
      }
    }

    if (request->progress && ((tick - start_tick) % 50 == 0 || tick == end_tick))
      request->progress(request->progress_user, (float)(tick - start_tick + 1) / (float)tick_span, "Writing DDNet demo");
  }

  game->physics_events_forced = false;
  if (!demo_w_finish(writer)) ok = false;
  demo_sb_destroy(&builder);
  demo_w_destroy(&writer);
  fclose(file);
  if (curr_worlds) free(curr_worlds);
  if (prev_worlds) free(prev_worlds);
  free_client_maps(client_maps, client_counts, world_count);
  free(worlds);
  if (!ok) dd_log(game, FT_LOG_ERROR, "DDNet demo export failed while writing snapshots.");
  return ok;
}

bool dd_demo_export(ft_game *game, const ft_export_request *request) { return dd_demo_export_impl(game, request, NULL); }

bool dd_demo_export_with_pings(ft_game *game, const ft_export_request *request, const int32_t *player_pings) {
  return dd_demo_export_impl(game, request, player_pings);
}
