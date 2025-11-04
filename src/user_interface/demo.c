#include "demo.h"
#include "../logger/logger.h"
#include "../renderer/graphics_backend.h"
#include "cimgui.h"
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include "nfd.h"
#include "timeline/timeline_model.h"
#include "undo_redo.h"
#include "user_interface.h"
#include <string.h>

#define DDNET_DEMO_IMPLEMENTATION
#include "../../libs/ddnet_demo/ddnet_demo.h"

static const char *LOG_SOURCE = "DemoExport";

// SHA-256 Implementation
// Necessary for creating a valid demo header
typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

#define DBL_INT_ADD(a, b, c)                                                                                                                         \
  if (a > 0xffffffff - (c)) ++b;                                                                                                                     \
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

int snap_world(dd_snapshot_builder *sb, timeline_state_t *ts, SWorldCore *prev, SWorldCore *cur) {
  int width = cur->m_pCollision->m_MapData.width;
  int height = cur->m_pCollision->m_MapData.height;
  int next_item_id = cur->m_NumCharacters; // start after reserved player ids

  // do pickups first since they have static ids basically
  for (int y = 0; y < height; ++y) {
    const int yidx = y * width;
    for (int x = 0; x < width; ++x) {
      const SPickup pickup = cur->m_pCollision->m_pPickups[yidx + x];
      if (pickup.m_Type >= 0) {
        dd_netobj_ddnet_pickup *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPICKUP, next_item_id++, sizeof(dd_netobj_ddnet_pickup));
        if (p) {
          p->m_X = x * 32 + 16 - MAP_EXPAND32;
          p->m_Y = y * 32 + 16 - MAP_EXPAND32;
          p->m_Type = pickup.m_Type;
          p->m_Subtype = pickup.m_Subtype;
          p->m_SwitchNumber = pickup.m_Number;
          p->m_Flags = 0;
          // log_info("DemoExport", "Added pickup id %d at (%d, %d), type %d, subtype %d", next_item_id, p->m_X, p->m_Y, p->m_Type, p->m_Subtype);
        }
      }
      const SPickup fpickup = cur->m_pCollision->m_pFrontPickups[yidx + x];
      if (fpickup.m_Type >= 0) {
        dd_netobj_ddnet_pickup *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPICKUP, next_item_id++, sizeof(dd_netobj_ddnet_pickup));
        if (p) {
          p->m_X = x * 32 + 16 - MAP_EXPAND32;
          p->m_Y = y * 32 + 16 - MAP_EXPAND32;
          p->m_Type = fpickup.m_Type;
          p->m_Subtype = fpickup.m_Subtype;
          p->m_SwitchNumber = fpickup.m_Number;
          p->m_Flags = 0;
        }
      }
    }
  }

  // game info
  dd_netobj_game_info *game_info = demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEINFO, 0, sizeof(dd_netobj_game_info));
  *game_info = (dd_netobj_game_info){};
  if (cur->m_NumCharacters > 0) {
    SCharacterCore *c = &cur->m_pCharacters[0];
    if (c->m_StartTick != -1) {
      game_info->m_WarmupTimer = -c->m_StartTick;
      game_info->m_GameStateFlags = DD_GAMESTATEFLAG_RACETIME;
    }
  }
  dd_netobj_game_info_ex *game_info_ex = demo_sb_add_item(sb, DD_NETOBJTYPE_GAMEINFOEX, 0, sizeof(dd_netobj_game_info_ex));
  game_info_ex->m_Version = 10;
  game_info_ex->m_Flags = DD_GAMEINFOFLAG_TIMESCORE | DD_GAMEINFOFLAG_GAMETYPE_RACE | DD_GAMEINFOFLAG_GAMETYPE_DDRACE |
                          DD_GAMEINFOFLAG_GAMETYPE_DDNET | DD_GAMEINFOFLAG_UNLIMITED_AMMO | DD_GAMEINFOFLAG_RACE_RECORD_MESSAGE |
                          DD_GAMEINFOFLAG_ALLOW_EYE_WHEEL | DD_GAMEINFOFLAG_ALLOW_HOOK_COLL | DD_GAMEINFOFLAG_ALLOW_ZOOM |
                          DD_GAMEINFOFLAG_BUG_DDRACE_GHOST | DD_GAMEINFOFLAG_BUG_DDRACE_INPUT | DD_GAMEINFOFLAG_PREDICT_DDRACE |
                          DD_GAMEINFOFLAG_PREDICT_DDRACE_TILES | DD_GAMEINFOFLAG_ENTITIES_DDNET | DD_GAMEINFOFLAG_ENTITIES_DDRACE |
                          DD_GAMEINFOFLAG_ENTITIES_RACE | DD_GAMEINFOFLAG_RACE;
  game_info_ex->m_Flags2 = DD_GAMEINFOFLAG2_HUD_DDRACE;

  for (int p = 0; p < cur->m_NumCharacters; ++p) {
    SCharacterCore *c_cur = &cur->m_pCharacters[p];
    SCharacterCore *c_prev = &prev->m_pCharacters[p];

    dd_netobj_client_info *ci = demo_sb_add_item(sb, DD_NETOBJTYPE_CLIENTINFO, p, sizeof(dd_netobj_client_info));
    str_to_ints(&ci->m_aName, 4, ts->player_tracks[p].player_info.name);
    str_to_ints(&ci->m_aClan, 3, ts->player_tracks[p].player_info.clan);

    // 3 offset to get the correct name
    if (ts->player_tracks[p].player_info.skin >= 3)
      str_to_ints(&ci->m_aSkin, 6, ts->ui->skin_manager.skins[ts->player_tracks[p].player_info.skin - 3].name);
    else *ci->m_aSkin = 0;
    ci->m_Country = 0;
    ci->m_UseCustomColor = ts->player_tracks[p].player_info.use_custom_color;
    ci->m_ColorBody = ts->player_tracks[p].player_info.color_body;
    ci->m_ColorFeet = ts->player_tracks[p].player_info.color_feet;

    dd_netobj_player_info *pi = demo_sb_add_item(sb, DD_NETOBJTYPE_PLAYERINFO, p, sizeof(dd_netobj_player_info));
    pi->m_Latency = 307;
    pi->m_Score = -9999;
    pi->m_Local = 0;
    pi->m_ClientId = p;
    pi->m_Team = 0;

    dd_netobj_ddnet_player *dp = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPLAYER, p, sizeof(dd_netobj_ddnet_player));
    dp->m_AuthLevel = 0;
    dp->m_Flags = 0;

    dd_netobj_character *ch = demo_sb_add_item(sb, DD_NETOBJTYPE_CHARACTER, p, sizeof(dd_netobj_character));

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
    ch->core.m_HookedPlayer = c_cur->m_HookedPlayer;
    ch->core.m_Jumped = c_cur->m_Jumped;
    ch->core.m_Direction = c_cur->m_Input.m_Direction;
    // setup angle
    float tmp_angle = atan2(c_cur->m_Input.m_TargetY, c_cur->m_Input.m_TargetX);
    if (tmp_angle < -(M_PI / 2.0f)) ch->core.m_Angle = (int)((tmp_angle + (2.0f * M_PI)) * 256.0f);
    else ch->core.m_Angle = (int)(tmp_angle * 256.0f);

    ch->core.m_Tick = cur->m_GameTick;
    ch->m_Emote = 2;

    ch->m_AttackTick = c_cur->m_AttackTick;
    ch->core.m_Direction = c_cur->m_Input.m_Direction;
    ch->m_Weapon = (c_cur->m_DeepFrozen || c_cur->m_FreezeTime > 0 || c_cur->m_LiveFrozen) ? WEAPON_NINJA : c_cur->m_ActiveWeapon;
    ch->m_AmmoCount = 0;
    ch->m_Health = 10;
    ch->m_Armor = 10;
    ch->m_PlayerFlags = 0;

    dd_netobj_ddnet_character *dc = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETCHARACTER, p, sizeof(dd_netobj_ddnet_character));
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

    dc->m_FreezeEnd = c_cur->m_DeepFrozen ? -1 : c_cur->m_FreezeTime == 0 ? 0 : cur->m_GameTick + c_cur->m_FreezeTime;

    dc->m_Jumps = c_cur->m_Jumps;
    dc->m_TeleCheckpoint = c_cur->m_TeleCheckpoint;
    dc->m_StrongWeakId = 0; // pCharacter->m_StrongWeakId;
    dc->m_JumpedTotal = c_cur->m_JumpedTotal;
    dc->m_NinjaActivationTick = c_cur->m_Ninja.m_ActivationTick;
    dc->m_FreezeStart = c_cur->m_FreezeStart;
    if (c_cur->m_FreezeTime > 0) {
      dc->m_Flags |= DD_CHARACTERFLAG_IN_FREEZE;
    }
    dc->m_TargetX = c_cur->m_Input.m_TargetX;
    dc->m_TargetY = c_cur->m_Input.m_TargetY;

    if (c_cur->m_RespawnDelay > c_prev->m_RespawnDelay) {
      dd_netevent_sound_world *nss;
      nss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      nss->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      nss->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      nss->m_SoundId = DD_SOUND_PLAYER_SPAWN;
      nss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      nss->common.m_X = vgetx(c_prev->m_Pos) - MAP_EXPAND32;
      nss->common.m_Y = vgety(c_prev->m_Pos) - MAP_EXPAND32;
      nss->m_SoundId = DD_SOUND_PLAYER_DIE;

      dd_netevent_spawn *ns = demo_sb_add_item(sb, DD_NETEVENTTYPE_SPAWN, next_item_id++, sizeof(dd_netevent_spawn));
      ns->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      ns->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;

      dd_netevent_death *nd = demo_sb_add_item(sb, DD_NETEVENTTYPE_DEATH, next_item_id++, sizeof(dd_netevent_death));
      nd->common.m_X = vgetx(c_prev->m_Pos) - MAP_EXPAND32;
      nd->common.m_Y = vgety(c_prev->m_Pos) - MAP_EXPAND32;
      nd->m_ClientId = c_cur->m_Id;
    }
    if (c_prev->m_HookState != HOOK_GRABBED && c_cur->m_HookState == HOOK_GRABBED) {
      dd_netevent_sound_world *nhs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      nhs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      nhs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      if (c_prev->m_HookedPlayer == -1 && c_cur->m_HookedPlayer != -1) nhs->m_SoundId = DD_SOUND_HOOK_ATTACH_PLAYER;
      else nhs->m_SoundId = DD_SOUND_HOOK_ATTACH_GROUND;
    }
    if (c_cur->m_Jumped && c_cur->m_Grounded) {
      dd_netevent_sound_world *njs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      njs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      njs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      njs->m_SoundId = DD_SOUND_PLAYER_JUMP;
    }
    if (c_cur->m_ReloadTimer > c_prev->m_ReloadTimer) {
      if (c_cur->m_ActiveWeapon <= 1) {
        dd_netevent_sound_world *nhs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
        nhs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
        nhs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
        nhs->m_SoundId = c_cur->m_ActiveWeapon == WEAPON_HAMMER ? DD_SOUND_HAMMER_FIRE : DD_SOUND_GUN_FIRE;
      }
    }
    // TODO: Getting hit by hammer
    // if (c_cur->m_DamageTick == pWorld->m_GameTick && pCharacter->m_DamageWeapon == WEAPON_HAMMER) {
    //   CNetEvent_HammerHit *pNetHammerHit = SnapNewItem<CNetEvent_HammerHit>(((uint64_t)pCharacter + 5) % 0xffff);
    //   pNetHammerHit->m_X = vgetx(pCharacter->m_Pos);
    //   pNetHammerHit->m_Y = vgety(pCharacter->m_Pos);
    //   pCharacter->m_DamageTick = -1;
    // }
  }

  // do entities
  for (SProjectile *proj = cur->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; proj; proj = proj->m_Base.m_pNextTypeEntity) {
    dd_netobj_ddnet_projectile *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPROJECTILE, next_item_id++, sizeof(dd_netobj_ddnet_projectile));
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
      p->m_StartTick = proj->m_StartTick;
      p->m_Owner = proj->m_Owner;
      p->m_Flags = Flags;
      p->m_SwitchNumber = proj->m_Base.m_Number;
      p->m_TuneZone = 0;
    }

    const mvec2 pos = prj_get_pos(proj, (cur->m_GameTick - proj->m_StartTick) / (float)GAME_TICK_SPEED);
    const mvec2 next_pos = prj_get_pos(proj, (cur->m_GameTick - proj->m_StartTick + 1) / (float)GAME_TICK_SPEED);
    if (proj->m_Owner >= 0 && proj->m_Base.m_Spawned) {
      dd_netevent_sound_world *nf = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      nf->common.m_X = vgetx(pos) - MAP_EXPAND32;
      nf->common.m_Y = vgety(pos) - MAP_EXPAND32;
      nf->m_SoundId = DD_SOUND_GRENADE_FIRE;
    }
    if (proj->m_Explosive) {
      mvec2 out, _out;
      if (intersect_line(proj->m_Base.m_pCollision, pos, next_pos, &out, &_out)) {
        dd_netevent_explosion *ne = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, next_item_id++, sizeof(dd_netevent_explosion));
        ne->common.m_X = vgetx(out) - MAP_EXPAND32;
        ne->common.m_Y = vgety(out) - MAP_EXPAND32;
        dd_netevent_sound_world *nes = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
        nes->common.m_X = vgetx(out) - MAP_EXPAND32;
        nes->common.m_Y = vgety(out) - MAP_EXPAND32;
        nes->m_SoundId = DD_SOUND_GRENADE_EXPLODE;
      }
      if (proj->m_LifeSpan <= 0) {
        dd_netevent_explosion *ne = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, next_item_id++, sizeof(dd_netevent_explosion));
        ne->common.m_X = vgetx(pos) - MAP_EXPAND32;
        ne->common.m_Y = vgety(pos) - MAP_EXPAND32;
        dd_netevent_sound_world *nes = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
        nes->common.m_X = vgetx(pos) - MAP_EXPAND32;
        nes->common.m_Y = vgety(pos) - MAP_EXPAND32;
        nes->m_SoundId = DD_SOUND_GRENADE_EXPLODE;
      }
    }
  }

  for (SLaser *laser = cur->m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; laser; laser = laser->m_Base.m_pNextTypeEntity) {
    dd_netobj_ddnet_laser *l = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETLASER, next_item_id++, sizeof(dd_netobj_ddnet_laser));
    // laser
    if (l) {
      l->m_ToX = (int)vgetx(laser->m_Base.m_Pos) - MAP_EXPAND32;
      l->m_ToY = (int)vgety(laser->m_Base.m_Pos) - MAP_EXPAND32;
      l->m_FromX = (int)vgetx(laser->m_From) - MAP_EXPAND32;
      l->m_FromY = (int)vgety(laser->m_From) - MAP_EXPAND32;
      l->m_StartTick = laser->m_EvalTick;
      l->m_Owner = laser->m_Owner;
      l->m_Type = laser->m_Type == DD_WEAPON_LASER ? DD_LASERTYPE_RIFLE : DD_LASERTYPE_SHOTGUN;
      l->m_Subtype = -1;
      l->m_SwitchNumber = laser->m_Base.m_Number;
      l->m_Flags = 0;
    }
    // sounds
    if (laser->m_Owner >= 0 && laser->m_Base.m_Spawned) {
      dd_netevent_sound_world *nlss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      if (nlss) {
        nlss->common.m_X = vgetx(laser->m_From) - MAP_EXPAND32;
        nlss->common.m_Y = vgety(laser->m_From) - MAP_EXPAND32;
        nlss->m_SoundId = laser->m_Type == DD_WEAPON_LASER ? DD_SOUND_LASER_FIRE : DD_SOUND_SHOTGUN_FIRE;
      }
    } else if (laser->m_EvalTick >= cur->m_GameTick) {
      dd_netevent_sound_world *nlbs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, next_item_id++, sizeof(dd_netevent_sound_world));
      if (nlbs) {
        nlbs->common.m_X = vgetx(laser->m_From) - MAP_EXPAND32;
        nlbs->common.m_Y = vgety(laser->m_From) - MAP_EXPAND32;
        nlbs->m_SoundId = DD_SOUND_LASER_BOUNCE;
      }
    }
  }
}

int export_to_demo(ui_handler_t *ui, const char *path, const char *map_name, int ticks) {
  // set up demo things
  void *map_data = ui->gfx_handler->physics_handler.collision.m_MapData._map_file_data;
  size_t map_size = ui->gfx_handler->physics_handler.collision.m_MapData._map_file_size;
  uint32_t map_crc = map_crc32(map_data, map_size);
  uint8_t map_sha256[32];
  SHA256_CTX ctx;
  map_sha256_init(&ctx);
  map_sha256_update(&ctx, map_data, map_size);
  map_sha256_final(&ctx, map_sha256);

  dd_demo_writer *writer = demo_w_create();
  FILE *f_demo = fopen(path, "wb");
  if (!writer || !f_demo) {
    log_error(LOG_SOURCE, "Error: Could not create demo writer or open output file.\n");
    return 1;
  }

  demo_w_begin(writer, f_demo, map_name, map_crc, "Race");
  demo_w_write_map(writer, map_sha256, map_data, map_size);

  dd_snapshot_builder *sb = demo_sb_create();
  uint8_t snap_buf[DD_MAX_SNAPSHOT_SIZE];

  // empty previous and current worlds
  SWorldCore prev = wc_empty();
  SWorldCore cur = wc_empty();

  // get initial worlds
  model_get_world_state_at_tick(&ui->timeline, 0, &cur);
  wc_copy_world(&prev, &cur);

  for (int t = 0; t < ticks; ++t) {
    demo_sb_clear(sb);
    for (int i = 0; i < cur.m_NumCharacters; ++i) {
      SPlayerInput input = model_get_input_at_tick(&ui->timeline, i, cur.m_GameTick);
      cc_on_input(&cur.m_pCharacters[i], &input);
    }
    snap_world(sb, &ui->timeline, &prev, &cur);
    wc_copy_world(&prev, &cur);
    wc_tick(&cur);
    int snap_size = demo_sb_finish(sb, snap_buf);
    if (snap_size > 0) demo_w_write_snap(writer, t, snap_buf, snap_size);
  }
  demo_w_finish(writer);
  demo_w_destroy(&writer);
  demo_sb_destroy(&sb);
  wc_free(&cur);
  return 0;
}

void render_demo_window(ui_handler_t *ui) {
  demo_exporter_t *dx = &ui->demo_exporter;

  // Center the popup on first appearance
  ImGuiViewport *viewport = igGetMainViewport();
  ImVec2 center;
  ImGuiViewport_GetCenter(&center, viewport);
  igSetNextWindowPos(center, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f});

  if (igBeginPopupModal("Demo Export", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
    // Path
    igText("Export Path");
    igInputText("##Path", dx->export_path, sizeof(dx->export_path), 0, NULL, NULL);
    igSameLine(0, 5.0f);
    if (igButton("Browse...", (ImVec2){0, 0})) {
      nfdu8char_t *save_path;
      nfdu8filteritem_t filters[] = {{"DDNet Demo", "demo"}};
      nfdresult_t result = NFD_SaveDialogU8(&save_path, filters, 1, NULL, "unnamed.demo");
      if (result == NFD_OKAY) {
        strncpy(dx->export_path, save_path, sizeof(dx->export_path) - 1);
        dx->export_path[sizeof(dx->export_path) - 1] = '\0'; // Ensure null termination
        NFD_FreePathU8(save_path);
      }
    }

    // Map Name
    igText("Map Name (in demo)");
    igInputText("##MapName", dx->map_name, sizeof(dx->map_name), 0, NULL, NULL);

    // Ticks
    igText("Number of Ticks");
    igInputInt("##Ticks", &dx->num_ticks, 1, 100, 0);
    igSameLine(0, 5.0f);
    if (igButton("Max Ticks", (ImVec2){0, 0})) {
      dx->num_ticks = model_get_max_timeline_tick(&ui->timeline);
    }

    igSeparator();
    igSpacing();

    if (igButton("Export", (ImVec2){120, 0})) {
      if (strlen(dx->export_path) > 0) {
        const char *map_name_to_use = (strlen(dx->map_name) > 0) ? dx->map_name : "unnamed_map";
        int result = export_to_demo(ui, dx->export_path, map_name_to_use, dx->num_ticks);
        if (result == 0) {
          log_info(LOG_SOURCE, "Demo exported successfully to '%s'", dx->export_path);
        } else {
          log_error(LOG_SOURCE, "Failed to export demo to '%s'", dx->export_path);
        }
      } else {
        log_warn(LOG_SOURCE, "Export path is empty. Cannot export demo.");
      }
      igCloseCurrentPopup();
    }

    igSetItemDefaultFocus();
    igSameLine(0, 10.0f);

    if (igButton("Cancel", (ImVec2){120, 0})) {
      igCloseCurrentPopup();
    }

    igEndPopup();
  }
}
