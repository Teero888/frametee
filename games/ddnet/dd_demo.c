#include "dd_internal.h"
#include "dd_profile.h"

#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include <ddnet_physics/vmath.h>
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
    pInts[i] = ((aBuf[0] + 128) << 24) | ((aBuf[1] + 128) << 16) | ((aBuf[2] + 128) << 8) | (aBuf[3] + 128);
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

static void snap_world(dd_snapshot_builder *sb, ft_game *game, int world_index, const int *client_ids, int client_count,
                       const int *client_options, SWorldCore *prev, const ft_world *current_world, bool include_static, int demo_tick,
                       int tick_delta, int *next_item_id) {
  SWorldCore *cur = (SWorldCore *)&current_world->core;
  int first_exported_local = -1;
  for (int i = 0; i < client_count; ++i) {
    if (client_ids[i] >= 0) {
      first_exported_local = i;
      break;
    }
  }

  // do pickups first since they have static ids basically
  for (int i = 0; include_static && i < game->current_level->num_pickups; ++i) {
    const SPickup pickup = game->current_level->pickups[i];
    if (cur->m_UniqueRace &&
        ((pickup.m_Type == POWERUP_WEAPON && pickup.m_Subtype != WEAPON_GRENADE) || pickup.m_Type == POWERUP_NINJA)) {
      continue;
    }
    dd_netobj_ddnet_pickup *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPICKUP, 64 + i, sizeof(dd_netobj_ddnet_pickup));
    if (p) {
      p->m_X = vgetx(game->current_level->pickup_positions[i]) - MAP_EXPAND32;
      p->m_Y = vgety(game->current_level->pickup_positions[i]) - MAP_EXPAND32;
      p->m_Type = pickup.m_Type;
      p->m_Subtype = pickup.m_Subtype;
      p->m_SwitchNumber = pickup.m_Number;
      p->m_Flags = 0;
      // log_info("DemoExport", "Added pickup id %d at (%d, %d), type %d, subtype %d", next_item_id, p->m_X, p->m_Y, p->m_Type, p->m_Subtype);
    }
  }

  // game info
  dd_netobj_game_info *game_info = include_static ? demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEINFO, 0, sizeof(dd_netobj_game_info)) : NULL;
  if (game_info) *game_info = (dd_netobj_game_info){0};
  if (game_info && first_exported_local >= 0) {
    SCharacterCore *c = &cur->m_pCharacters[first_exported_local];
    if (c->m_StartTick != -1) {
      game_info->m_WarmupTimer = -(c->m_StartTime + tick_delta);
      game_info->m_GameStateFlags = DD_GAMESTATEFLAG_RACETIME;
    }
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
    if (cur->m_pConfig->m_SvHealthAndAmmo) {
      game_info_ex->m_Flags &=
          ~(DD_GAMEINFOFLAG_UNLIMITED_AMMO | DD_GAMEINFOFLAG_GAMETYPE_DDNET | DD_GAMEINFOFLAG_GAMETYPE_DDRACE | DD_GAMEINFOFLAG_PREDICT_DDRACE);
      game_info_ex->m_Flags |= DD_GAMEINFOFLAG_GAMETYPE_VANILLA | DD_GAMEINFOFLAG_PREDICT_VANILLA;
      game_info_ex->m_Flags2 = DD_GAMEINFOFLAG2_HUD_HEALTH_ARMOR | DD_GAMEINFOFLAG2_HUD_AMMO;
    }
  }
  if (include_static && game_info_ex && cur->m_pConfig->m_SvFastcap) {
    game_info_ex->m_Flags |= DD_GAMEINFOFLAG_GAMETYPE_FASTCAP | DD_GAMEINFOFLAG_FLAG_STARTS_RACE;

    dd_netobj_game_data *game_data = demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEDATA, 0, sizeof(dd_netobj_game_data));
    if (game_data) {
      *game_data = (dd_netobj_game_data){.m_FlagCarrierRed = -3, .m_FlagCarrierBlue = -3};
      const SCharacterCore *view_character = first_exported_local >= 0 ? &cur->m_pCharacters[first_exported_local] : NULL;
      if (!view_character || view_character->m_FinishTick < 0) {
        for (int team = 0; team < 2; ++team) {
          if (!cur->m_pCollision->m_aFastcapFlagPresent[team]) continue;
          dd_netobj_flag *flag = demo_sb_add_item(sb, DD_NETOBJTYPE_FLAG, team, sizeof(dd_netobj_flag));
          if (!flag) continue;
          flag->m_X = vgetx(cur->m_pCollision->m_aFastcapFlagPositions[team]) - MAP_EXPAND32;
          flag->m_Y = vgety(cur->m_pCollision->m_aFastcapFlagPositions[team]) - MAP_EXPAND32;
          flag->m_Team = team;
          int carrier = view_character && view_character->m_aGotFastcapFlag[team] ? client_ids[first_exported_local] : -2;
          if (team == 0) game_data->m_FlagCarrierRed = carrier;
          else game_data->m_FlagCarrierBlue = carrier;
        }
      }
    }
  }

  for (int p = 0; p < cur->m_NumCharacters; ++p) {
    if (p >= client_count || client_ids[p] < 0) continue;
    const int client_id = client_ids[p];
    const int track_index = game->engine->timeline_player_track((uint32_t)world_index, (uint32_t)p);
    if (track_index < 0) continue;
    dd_player_profile_t profile;
    dd_profile_for_track(game, track_index, &profile);
    SCharacterCore *c_cur = &cur->m_pCharacters[p];
    SCharacterCore *c_prev = &prev->m_pCharacters[p];

    dd_netobj_client_info *ci = demo_sb_add_item(sb, DD_NETOBJTYPE_CLIENTINFO, client_id, sizeof(dd_netobj_client_info));
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

    dd_netobj_player_info *pi = demo_sb_add_item(sb, DD_NETOBJTYPE_PLAYERINFO, client_id, sizeof(dd_netobj_player_info));
    pi->m_Latency = client_options ? client_options[client_id] : 0;
    pi->m_Score = -9999;
    pi->m_Local = 0;
    pi->m_ClientId = client_id;
    pi->m_Team = 0;

    dd_netobj_ddnet_player *dp = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPLAYER, client_id, sizeof(dd_netobj_ddnet_player));
    dp->m_AuthLevel = 0;
    dp->m_Flags = 0;

    dd_netobj_character *ch = demo_sb_add_item(sb, DD_NETOBJTYPE_CHARACTER, client_id, sizeof(dd_netobj_character));

    ch->core.m_X = round_to_int(vgetx(c_cur->m_Pos)) - MAP_EXPAND32;
    ch->core.m_Y = round_to_int(vgety(c_cur->m_Pos)) - MAP_EXPAND32;
    ch->core.m_VelX = round_to_int(vgetx(c_cur->m_Vel) * 256.0f);
    ch->core.m_VelY = round_to_int(vgety(c_cur->m_Vel) * 256.0f);
    ch->core.m_HookState = c_cur->m_HookState;
    ch->core.m_HookTick = c_cur->m_HookTick;
    ch->core.m_HookX = round_to_int(vgetx(c_cur->m_HookPos)) - MAP_EXPAND32;
    ch->core.m_HookY = round_to_int(vgety(c_cur->m_HookPos)) - MAP_EXPAND32;
    ch->core.m_HookDx = round_to_int(vgetx(c_cur->m_HookDir) * 256.0f);
    ch->core.m_HookDy = round_to_int(vgety(c_cur->m_HookDir) * 256.0f);
    ch->core.m_HookedPlayer = remap_client_id(client_ids, client_count, c_cur->m_HookedPlayer);
    ch->core.m_Jumped = c_cur->m_Jumped;
    ch->core.m_Direction = c_cur->m_Input.m_Direction;
    // setup angle
    float tmp_angle = atan2(c_cur->m_Input.m_TargetY, c_cur->m_Input.m_TargetX);
    if (tmp_angle < -(M_PI / 2.0f)) ch->core.m_Angle = (int)((tmp_angle + (2.0f * M_PI)) * 256.0f);
    else ch->core.m_Angle = (int)(tmp_angle * 256.0f);

    // Physics groups run on local clocks, while a demo has one shared clock. Every absolute tick
    // written to the protocol must be translated or the client predicts offset groups far away.
    ch->core.m_Tick = demo_tick;
    const int damage_age = cur->m_GameTick - c_cur->m_DamageTick;
    ch->m_Emote = damage_age >= 0 && damage_age < GAME_TICK_SPEED / 2 ? DD_EMOTE_PAIN : DD_EMOTE_HAPPY;

    ch->m_AttackTick = c_cur->m_AttackTick + tick_delta;
    ch->core.m_Direction = c_cur->m_Input.m_Direction;
    ch->m_Weapon = (c_cur->m_DeepFrozen || c_cur->m_FreezeTime > 0 || c_cur->m_LiveFrozen) ? WEAPON_NINJA : c_cur->m_ActiveWeapon;
    ch->m_AmmoCount = cur->m_pConfig->m_SvHealthAndAmmo ? c_cur->m_aWeaponAmmo[c_cur->m_ActiveWeapon] : 0;
    ch->m_Health = cur->m_pConfig->m_SvHealthAndAmmo ? c_cur->m_Health : 10;
    ch->m_Armor = cur->m_pConfig->m_SvHealthAndAmmo ? c_cur->m_Armor : 10;
    ch->m_PlayerFlags = 0;

    dd_netobj_ddnet_character *dc = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETCHARACTER, client_id, sizeof(dd_netobj_ddnet_character));
    dc->m_Flags = 0;
    if (c_cur->m_Solo) dc->m_Flags |= DD_CHARACTERFLAG_SOLO;
    if (c_cur->m_EndlessHook) dc->m_Flags |= DD_CHARACTERFLAG_ENDLESS_HOOK;
    if (c_cur->m_CollisionDisabled) dc->m_Flags |= DD_CHARACTERFLAG_COLLISION_DISABLED;
    if (c_cur->m_HookHitDisabled) dc->m_Flags |= DD_CHARACTERFLAG_HOOK_HIT_DISABLED;
    if (c_cur->m_EndlessJump) dc->m_Flags |= DD_CHARACTERFLAG_ENDLESS_JUMP;
    if (c_cur->m_Jetpack) dc->m_Flags |= DD_CHARACTERFLAG_JETPACK;
    if (c_cur->m_HammerHitDisabled) dc->m_Flags |= DD_CHARACTERFLAG_HAMMER_HIT_DISABLED;
    if (c_cur->m_ShotgunHitDisabled) dc->m_Flags |= DD_CHARACTERFLAG_SHOTGUN_HIT_DISABLED;
    if (c_cur->m_GrenadeHitDisabled) dc->m_Flags |= DD_CHARACTERFLAG_GRENADE_HIT_DISABLED;
    if (c_cur->m_LaserHitDisabled) dc->m_Flags |= DD_CHARACTERFLAG_LASER_HIT_DISABLED;
    if (c_cur->m_HasTelegunGun) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_GUN;
    if (c_cur->m_HasTelegunGrenade) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_GRENADE;
    if (c_cur->m_HasTelegunLaser) dc->m_Flags |= DD_CHARACTERFLAG_TELEGUN_LASER;
    if (c_cur->m_aWeaponGot[WEAPON_HAMMER]) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_HAMMER;
    if (c_cur->m_aWeaponGot[WEAPON_GUN]) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_GUN;
    if (c_cur->m_aWeaponGot[WEAPON_SHOTGUN]) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_SHOTGUN;
    if (c_cur->m_aWeaponGot[WEAPON_GRENADE]) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_GRENADE;
    if (c_cur->m_aWeaponGot[WEAPON_LASER]) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_LASER;
    if (c_cur->m_ActiveWeapon == WEAPON_NINJA) dc->m_Flags |= DD_CHARACTERFLAG_WEAPON_NINJA;
    if (c_cur->m_LiveFrozen) dc->m_Flags |= DD_CHARACTERFLAG_MOVEMENTS_DISABLED;

    dc->m_Jumps = c_cur->m_Jumps;
    dc->m_TeleCheckpoint = c_cur->m_TeleCheckpoint;
    dc->m_StrongWeakId = client_id;
    dc->m_JumpedTotal = c_cur->m_JumpedTotal;
    dc->m_NinjaActivationTick = c_cur->m_Ninja.m_ActivationTick + tick_delta;

    dc->m_FreezeStart = c_cur->m_FreezeStart == 0 ? 0 : c_cur->m_FreezeStart + tick_delta;
    dc->m_FreezeEnd = c_cur->m_DeepFrozen ? -1 : c_cur->m_FreezeTime == 0 ? 0
                                                                          : demo_tick + c_cur->m_FreezeTime;

    if (c_cur->m_IsInFreeze) {
      dc->m_Flags |= DD_CHARACTERFLAG_IN_FREEZE;
    }
    dc->m_TargetX = c_cur->m_Input.m_TargetX;
    dc->m_TargetY = c_cur->m_Input.m_TargetY;

    if (c_cur->m_StartTick != -1 && c_prev->m_FinishTick == -1 && c_cur->m_FinishTick != -1) {
      dd_netevent_finish *nf = demo_sb_add_item(sb, DD_NETEVENTTYPE_FINISH, (*next_item_id)++, sizeof(dd_netevent_finish));
      if (nf) {
        nf->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
        nf->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
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
    const int x = (int)event->x - MAP_EXPAND32;
    const int y = (int)event->y - MAP_EXPAND32;
    switch (event->type) {
    case PARTICLE_TYPE_PLAYER_SPAWN: {
      dd_netevent_spawn *spawn = demo_sb_add_item(sb, DD_NETEVENTTYPE_SPAWN, (*next_item_id)++, sizeof(*spawn));
      spawn->common.m_X = x;
      spawn->common.m_Y = y;
      break;
    }
    case PARTICLE_TYPE_PLAYER_DEATH: {
      dd_netevent_death *death = demo_sb_add_item(sb, DD_NETEVENTTYPE_DEATH, (*next_item_id)++, sizeof(*death));
      death->common.m_X = x;
      death->common.m_Y = y;
      death->m_ClientId = client_id;
      break;
    }
    case PARTICLE_TYPE_HAMMER_HIT: {
      dd_netevent_hammer_hit *hit = demo_sb_add_item(sb, DD_NETEVENTTYPE_HAMMERHIT, (*next_item_id)++, sizeof(*hit));
      hit->common.m_X = x;
      hit->common.m_Y = y;
      break;
    }
    case PARTICLE_TYPE_EXPLOSION: {
      dd_netevent_explosion *explosion = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, (*next_item_id)++, sizeof(*explosion));
      explosion->common.m_X = x;
      explosion->common.m_Y = y;
      break;
    }
    case PARTICLE_TYPE_AIR_JUMP:
      break;
    default:
      break;
    }
  }
  for (int i = 0; i < current_world->physics_sound_event_count; ++i) {
    const dd_physics_sound_event_t *event = &current_world->physics_sound_events[i];
    if (event->client_id >= 0 && remap_client_id(client_ids, client_count, event->client_id) < 0) continue;
    dd_netevent_sound_world *sound =
        demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(*sound));
    sound->common.m_X = (int)event->x - MAP_EXPAND32;
    sound->common.m_Y = (int)event->y - MAP_EXPAND32;
    sound->m_SoundId = event->sound_id;
  }
  for (int i = 0; i < current_world->physics_damage_event_count; ++i) {
    const dd_physics_damage_event_t *event = &current_world->physics_damage_events[i];
    if (event->client_id >= 0 && remap_client_id(client_ids, client_count, event->client_id) < 0) continue;
    const float center = 3.0f * (float)M_PI / 2.0f + event->angle;
    const float start = center - (float)M_PI / 3.0f;
    const float end = center + (float)M_PI / 3.0f;
    for (int indicator = 0; indicator < event->amount; ++indicator) {
      const float angle = start + (end - start) * (float)(indicator + 1) / (float)(event->amount + 1);
      dd_netevent_damage_ind *damage =
          demo_sb_add_item(sb, DD_NETEVENTTYPE_DAMAGEIND, (*next_item_id)++, sizeof(*damage));
      damage->common.m_X = (int)event->x - MAP_EXPAND32;
      damage->common.m_Y = (int)event->y - MAP_EXPAND32;
      damage->m_Angle = (int)(angle * 256.0f);
    }
  }

  // do entities
  for (SProjectile *proj = (SProjectile *)cur->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; proj;
       proj = (SProjectile *)proj->m_Base.m_pNextTypeEntity) {
    int owner = remap_client_id(client_ids, client_count, proj->m_Owner);
    if (proj->m_Owner >= 0 && owner < 0) continue;
    dd_netobj_ddnet_projectile *p =
        demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPROJECTILE, (*next_item_id)++, sizeof(dd_netobj_ddnet_projectile));
    if (p) {
      int Flags = 0;
      if (proj->m_Bouncing & 1) {
        Flags |= DD_PROJECTILEFLAG_BOUNCE_HORIZONTAL;
      }
      if (proj->m_Bouncing & 2) {
        Flags |= DD_PROJECTILEFLAG_BOUNCE_VERTICAL;
      }
      if (proj->m_Explosive) {
        Flags |= DD_PROJECTILEFLAG_EXPLOSIVE;
      }
      if (proj->m_Freeze) {
        Flags |= DD_PROJECTILEFLAG_FREEZE;
      }
      Flags |= DD_PROJECTILEFLAG_NORMALIZE_VEL;
      p->m_VelX = round_to_int(vgetx(proj->m_Direction) * 1e6f);
      p->m_VelY = round_to_int(vgety(proj->m_Direction) * 1e6f);
      p->m_X = round_to_int((vgetx(proj->m_Base.m_Pos) - MAP_EXPAND32) * 100.0f);
      p->m_Y = round_to_int((vgety(proj->m_Base.m_Pos) - MAP_EXPAND32) * 100.0f);
      p->m_Type = proj->m_Type;
      p->m_StartTick = proj->m_StartTick + tick_delta;
      p->m_Owner = owner;
      p->m_Flags = Flags;
      p->m_SwitchNumber = proj->m_Base.m_Number;
      p->m_TuneZone = 0;
    }
  }

  for (SLaser *laser = (SLaser *)cur->m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; laser; laser = (SLaser *)laser->m_Base.m_pNextTypeEntity) {
    int owner = remap_client_id(client_ids, client_count, laser->m_Owner);
    if (laser->m_Owner >= 0 && owner < 0) continue;
    dd_netobj_ddnet_laser *l = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETLASER, (*next_item_id)++, sizeof(dd_netobj_ddnet_laser));
    // laser
    if (l) {
      l->m_ToX = (int)vgetx(laser->m_Base.m_Pos) - MAP_EXPAND32;
      l->m_ToY = (int)vgety(laser->m_Base.m_Pos) - MAP_EXPAND32;
      l->m_FromX = (int)vgetx(laser->m_From) - MAP_EXPAND32;
      l->m_FromY = (int)vgety(laser->m_From) - MAP_EXPAND32;
      l->m_StartTick = laser->m_EvalTick + tick_delta;
      l->m_Owner = owner;
      l->m_Type = laser->m_Type == DD_WEAPON_LASER ? DD_LASERTYPE_RIFLE : DD_LASERTYPE_SHOTGUN;
      l->m_Subtype = -1;
      l->m_SwitchNumber = laser->m_Base.m_Number;
      l->m_Flags = 0;
    }
  }
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

  const void *map_data = game->current_level->collision.m_MapData._map_file_data;
  const size_t map_size = game->current_level->collision.m_MapData._map_file_size;
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

  FILE *file = fopen(request->path, "wb");
  dd_demo_writer *writer = demo_w_create();
  dd_snapshot_builder *builder = demo_sb_create();
  if (!file || !writer || !builder ||
      !demo_w_begin(writer, file, game->current_level->name[0] ? game->current_level->name : "unnamed_map", map_crc, "Race") ||
      !demo_w_write_map(writer, map_sha256, map_data, map_size)) {
    if (builder) demo_sb_destroy(&builder);
    if (writer) demo_w_destroy(&writer);
    if (file) fclose(file);
    free_client_maps(client_maps, client_counts, world_count);
    free(worlds);
    dd_log(game, FT_LOG_ERROR, "Could not initialize the DDNet demo writer.");
    return false;
  }

  uint8_t snapshot[DD_SNAPSHOT_MAX_SIZE];
  bool ok = true;
  const int tick_span = end_tick - start_tick + 1;
  for (int tick = start_tick; tick <= end_tick; ++tick) {
    demo_sb_clear(builder);
    int next_item_id = 64 + game->current_level->num_pickups;
    bool include_static = true;
    for (uint32_t world_index = 0; world_index < world_count; ++world_index) {
      if (!world_has_exported_client(client_maps[world_index], client_counts[world_index])) continue;

      const ft_world *previous = NULL;
      const ft_world *current = NULL;
      if (!game->engine->timeline_world_pair(world_index, tick, &previous, &current) || !previous || !current) {
        ok = false;
        break;
      }
      snap_world(builder, game, (int)world_index, client_maps[world_index], client_counts[world_index], client_options,
                 (SWorldCore *)&previous->core, current, include_static, tick - start_tick,
                 worlds[world_index].start_offset - start_tick, &next_item_id);
      include_static = false;
    }
    if (!ok) break;

    const int snapshot_size = demo_sb_finish(builder, snapshot);
    if (snapshot_size <= 0 || !demo_w_write_snap(writer, tick - start_tick, snapshot, snapshot_size)) {
      ok = false;
      break;
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

  if (!demo_w_finish(writer)) ok = false;
  demo_sb_destroy(&builder);
  demo_w_destroy(&writer);
  fclose(file);
  free_client_maps(client_maps, client_counts, world_count);
  free(worlds);
  if (!ok) dd_log(game, FT_LOG_ERROR, "DDNet demo export failed while writing snapshots.");
  return ok;
}

bool dd_demo_export(ft_game *game, const ft_export_request *request) { return dd_demo_export_impl(game, request, NULL); }

bool dd_demo_export_with_pings(ft_game *game, const ft_export_request *request, const int32_t *player_pings) {
  return dd_demo_export_impl(game, request, player_pings);
}
