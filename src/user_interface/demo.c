#include "demo.h"
#include <system/fs.h>
#include "ddnet_physics/vmath.h"
#include "nfd.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include "undo_redo.h"
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <system/include_cimgui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDNET_DEMO_IMPLEMENTATION
#include <ddnet_demo/ddnet_demo.h>

static const char *LOG_SOURCE = "DemoExport";

typedef struct {
  bool active;
  int track_index;
  int before;
} DemoPingEditUndo;

static DemoPingEditUndo g_demo_ping_edit_undo;

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

static void on_world_particle(mvec2 pos, int type, int cid, void *data) {
  (void)cid;
  demo_exporter_t *exporter = data;
  if (type == PARTICLE_TYPE_HAMMER_HIT) {
    if (exporter->num_hammerhits < MAX_HAMMERHITS_PER_TICK) exporter->hammerhits[exporter->num_hammerhits++] = pos;
  }
}

static void on_damage_indicator(mvec2 pos, float angle, int amount, int cid, void *data) {
  (void)cid;
  demo_exporter_t *exporter = data;
  const float center = 3.0f * M_PI / 2.0f + angle;
  const float start = center - M_PI / 3.0f;
  const float end = center + M_PI / 3.0f;
  for (int i = 0; i < amount && exporter->num_damage_indicators < MAX_DAMAGE_INDICATORS_PER_TICK; ++i) {
    const int index = exporter->num_damage_indicators++;
    exporter->damage_indicator_positions[index] = pos;
    exporter->damage_indicator_angles[index] = start + (end - start) * (float)(i + 1) / (float)(amount + 1);
  }
}

static int remap_client_id(const int *client_ids, int client_count, int local_id) {
  if (local_id < 0) return local_id;
  return local_id < client_count ? client_ids[local_id] : -1;
}

static void snap_world(dd_snapshot_builder *sb, timeline_state_t *ts, int group_index, const int *client_ids, int client_count,
                       SWorldCore *prev, SWorldCore *cur, demo_exporter_t *events, bool include_static, int demo_tick, int tick_delta,
                       int *next_item_id) {
  int first_exported_local = -1;
  for (int i = 0; i < client_count; ++i) {
    if (client_ids[i] >= 0) {
      first_exported_local = i;
      break;
    }
  }

  // do pickups first since they have static ids basically
  for (int i = 0; include_static && i < ts->ui->num_pickups; ++i) {
    const SPickup pickup = ts->ui->pickups[i];
    if (cur->m_UniqueRace &&
        ((pickup.m_Type == POWERUP_WEAPON && pickup.m_Subtype != WEAPON_GRENADE) || pickup.m_Type == POWERUP_NINJA)) {
      continue;
    }
    if (cur->m_pConfig->m_SvHealthAndAmmo && cur->m_NumCharacters > 0 &&
        physics_pickup_on_cooldown(cur, 0, ts->ui->pickup_cooldown_keys[i])) {
      continue;
    }
    dd_netobj_ddnet_pickup *p = demo_sb_add_item(sb, DD_NETOBJTYPE_DDNETPICKUP, 64 + i, sizeof(dd_netobj_ddnet_pickup));
    if (p) {
      p->m_X = vgetx(ts->ui->pickup_positions[i]) - MAP_EXPAND32;
      p->m_Y = vgety(ts->ui->pickup_positions[i]) - MAP_EXPAND32;
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
    const int track_index = model_group_track_index(ts, group_index, p);
    if (track_index < 0) continue;
    SCharacterCore *c_cur = &cur->m_pCharacters[p];
    SCharacterCore *c_prev = &prev->m_pCharacters[p];

    dd_netobj_client_info *ci = demo_sb_add_item(sb, DD_NETOBJTYPE_CLIENTINFO, client_id, sizeof(dd_netobj_client_info));
    const char *name = ts->player_tracks[track_index].player_info.name;
    if (name[0] == '\0') name = "nameless tee";
    str_to_ints(ci->m_aName, 4, name);
    str_to_ints(ci->m_aClan, 3, ts->player_tracks[track_index].player_info.clan);

    // 3 offset to get the correct name
    if (ts->player_tracks[track_index].player_info.skin >= 3)
      str_to_ints(ci->m_aSkin, 6, ts->ui->skin_manager.skins[ts->player_tracks[track_index].player_info.skin - 3].name);
    else *ci->m_aSkin = 0;
    ci->m_Country = 0;
    ci->m_UseCustomColor = ts->player_tracks[track_index].player_info.use_custom_color;
    ci->m_ColorBody = ts->player_tracks[track_index].player_info.color_body;
    ci->m_ColorFeet = ts->player_tracks[track_index].player_info.color_feet;

    dd_netobj_player_info *pi = demo_sb_add_item(sb, DD_NETOBJTYPE_PLAYERINFO, client_id, sizeof(dd_netobj_player_info));
    pi->m_Latency = ts->player_tracks[track_index].demo_ping;
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
    ch->m_PlayerFlags = ts->player_tracks[track_index].demo_player_flags;

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

    if (c_cur->m_RespawnDelay > c_prev->m_RespawnDelay) {
      dd_netevent_sound_world *nss;
      nss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      nss->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      nss->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      nss->m_SoundId = DD_SOUND_PLAYER_SPAWN;
      nss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      nss->common.m_X = vgetx(c_prev->m_Pos) - MAP_EXPAND32;
      nss->common.m_Y = vgety(c_prev->m_Pos) - MAP_EXPAND32;
      nss->m_SoundId = DD_SOUND_PLAYER_DIE;

      dd_netevent_spawn *ns = demo_sb_add_item(sb, DD_NETEVENTTYPE_SPAWN, (*next_item_id)++, sizeof(dd_netevent_spawn));
      ns->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      ns->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;

      dd_netevent_death *nd = demo_sb_add_item(sb, DD_NETEVENTTYPE_DEATH, (*next_item_id)++, sizeof(dd_netevent_death));
      nd->common.m_X = vgetx(c_prev->m_Pos) - MAP_EXPAND32;
      nd->common.m_Y = vgety(c_prev->m_Pos) - MAP_EXPAND32;
      nd->m_ClientId = client_id;
    }
    if (c_cur->m_StartTick != -1 && c_prev->m_FinishTick == -1 && c_cur->m_FinishTick != -1) {
      dd_netevent_finish *nf = demo_sb_add_item(sb, DD_NETEVENTTYPE_FINISH, (*next_item_id)++, sizeof(dd_netevent_finish));
      if (nf) {
        nf->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
        nf->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      }
    }
    if (c_prev->m_HookState != HOOK_GRABBED && c_cur->m_HookState == HOOK_GRABBED) {
      dd_netevent_sound_world *nhs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      nhs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      nhs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      if (c_prev->m_HookedPlayer == -1 && c_cur->m_HookedPlayer != -1) nhs->m_SoundId = DD_SOUND_HOOK_ATTACH_PLAYER;
      else nhs->m_SoundId = DD_SOUND_HOOK_ATTACH_GROUND;
    }
    if (c_cur->m_Jumped && c_cur->m_Grounded) {
      dd_netevent_sound_world *njs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      njs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
      njs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
      njs->m_SoundId = DD_SOUND_PLAYER_JUMP;
    }
    if (c_cur->m_ReloadTimer > c_prev->m_ReloadTimer) {
      if (c_cur->m_ActiveWeapon <= 1) {
        dd_netevent_sound_world *nhs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
        nhs->common.m_X = vgetx(c_cur->m_Pos) - MAP_EXPAND32;
        nhs->common.m_Y = vgety(c_cur->m_Pos) - MAP_EXPAND32;
        nhs->m_SoundId = c_cur->m_ActiveWeapon == WEAPON_HAMMER ? DD_SOUND_HAMMER_FIRE : DD_SOUND_GUN_FIRE;
      }
    }
  }

  for (int i = 0; i < events->num_hammerhits; ++i) {
    dd_netevent_hammer_hit *nhh = demo_sb_add_item(sb, DD_NETEVENTTYPE_HAMMERHIT, (*next_item_id)++, sizeof(dd_netevent_hammer_hit));
    nhh->common.m_X = vgetx(events->hammerhits[i]) - MAP_EXPAND32;
    nhh->common.m_Y = vgety(events->hammerhits[i]) - MAP_EXPAND32;
  }
  for (int i = 0; i < events->num_damage_indicators; ++i) {
    dd_netevent_damage_ind *damage =
        demo_sb_add_item(sb, DD_NETEVENTTYPE_DAMAGEIND, (*next_item_id)++, sizeof(dd_netevent_damage_ind));
    damage->common.m_X = vgetx(events->damage_indicator_positions[i]) - MAP_EXPAND32;
    damage->common.m_Y = vgety(events->damage_indicator_positions[i]) - MAP_EXPAND32;
    damage->m_Angle = (int)(events->damage_indicator_angles[i] * 256.0f);
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

    const mvec2 pos = prj_get_pos(proj, (cur->m_GameTick - proj->m_StartTick) / (float)GAME_TICK_SPEED);
    const mvec2 next_pos = prj_get_pos(proj, (cur->m_GameTick - proj->m_StartTick + 1) / (float)GAME_TICK_SPEED);
    if (proj->m_Owner >= 0 && proj->m_Base.m_Spawned) {
      dd_netevent_sound_world *nf = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      nf->common.m_X = vgetx(pos) - MAP_EXPAND32;
      nf->common.m_Y = vgety(pos) - MAP_EXPAND32;
      nf->m_SoundId = DD_SOUND_GRENADE_FIRE;
    }
    if (proj->m_Explosive) {
      mvec2 out, _out;
      if (intersect_line(proj->m_Base.m_pCollision, pos, next_pos, &out, &_out)) {
        dd_netevent_explosion *ne = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, (*next_item_id)++, sizeof(dd_netevent_explosion));
        ne->common.m_X = vgetx(out) - MAP_EXPAND32;
        ne->common.m_Y = vgety(out) - MAP_EXPAND32;
        dd_netevent_sound_world *nes = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
        nes->common.m_X = vgetx(out) - MAP_EXPAND32;
        nes->common.m_Y = vgety(out) - MAP_EXPAND32;
        nes->m_SoundId = DD_SOUND_GRENADE_EXPLODE;
      }
      if (proj->m_LifeSpan <= 0) {
        dd_netevent_explosion *ne = demo_sb_add_item(sb, DD_NETEVENTTYPE_EXPLOSION, (*next_item_id)++, sizeof(dd_netevent_explosion));
        ne->common.m_X = vgetx(pos) - MAP_EXPAND32;
        ne->common.m_Y = vgety(pos) - MAP_EXPAND32;
        dd_netevent_sound_world *nes = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
        nes->common.m_X = vgetx(pos) - MAP_EXPAND32;
        nes->common.m_Y = vgety(pos) - MAP_EXPAND32;
        nes->m_SoundId = DD_SOUND_GRENADE_EXPLODE;
      }
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
    // sounds
    if (laser->m_Owner >= 0 && laser->m_Base.m_Spawned) {
      dd_netevent_sound_world *nlss = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      if (nlss) {
        nlss->common.m_X = vgetx(laser->m_From) - MAP_EXPAND32;
        nlss->common.m_Y = vgety(laser->m_From) - MAP_EXPAND32;
        nlss->m_SoundId = laser->m_Type == DD_WEAPON_LASER ? DD_SOUND_LASER_FIRE : DD_SOUND_SHOTGUN_FIRE;
      }
    } else if (laser->m_EvalTick >= cur->m_GameTick) {
      dd_netevent_sound_world *nlbs = demo_sb_add_item(sb, DD_NETEVENTTYPE_SOUNDWORLD, (*next_item_id)++, sizeof(dd_netevent_sound_world));
      if (nlbs) {
        nlbs->common.m_X = vgetx(laser->m_From) - MAP_EXPAND32;
        nlbs->common.m_Y = vgety(laser->m_From) - MAP_EXPAND32;
        nlbs->m_SoundId = DD_SOUND_LASER_BOUNCE;
      }
    }
  }
}

static void free_client_maps(int **client_maps, int *client_counts, int group_count) {
  if (client_maps) {
    for (int i = 0; i < group_count; ++i) free(client_maps[i]);
  }
  free(client_maps);
  free(client_counts);
}

static bool write_net_event(dd_demo_writer *writer, const net_event_t *ev, const int *client_ids, int client_count) {
  int client_id = remap_client_id(client_ids, client_count, ev->client_id);
  int killer = remap_client_id(client_ids, client_count, ev->killer);
  int victim = remap_client_id(client_ids, client_count, ev->victim);
  if (ev->type == NET_EVENT_CHAT) {
    if (ev->client_id >= 0 && client_id < 0) return false;
    demo_w_write_msg_sv_chat(writer, ev->team, client_id, ev->message);
  } else if (ev->type == NET_EVENT_BROADCAST) {
    demo_w_write_msg_sv_broadcast(writer, ev->message);
  } else if (ev->type == NET_EVENT_KILLMSG) {
    if ((ev->killer >= 0 && killer < 0) || (ev->victim >= 0 && victim < 0)) return false;
    demo_w_write_msg_sv_killmsg(writer, killer, victim, ev->weapon, ev->mode_special);
  } else if (ev->type == NET_EVENT_SOUND_GLOBAL) {
    demo_w_write_msg_sv_sound_global(writer, ev->sound_id);
  } else if (ev->type == NET_EVENT_EMOTICON) {
    if (ev->client_id >= 0 && client_id < 0) return false;
    demo_w_write_msg_sv_emoticon(writer, client_id, ev->emoticon);
  } else if (ev->type == NET_EVENT_VOTE_SET) {
    demo_w_write_msg_sv_vote_set(writer, ev->vote_timeout, ev->message, ev->reason);
  } else if (ev->type == NET_EVENT_VOTE_STATUS) {
    demo_w_write_msg_sv_vote_status(writer, ev->vote_yes, ev->vote_no, ev->vote_pass, ev->vote_total);
  } else if (ev->type == NET_EVENT_DDRACE_TIME) {
    demo_w_write_msg_sv_ddrace_time_legacy(writer, ev->time, ev->check, ev->finish);
    int race_client = client_count > 0 ? client_ids[0] : -1;
    if (race_client >= 0) demo_w_write_msg_sv_racefinish(writer, race_client, ev->time * 10, 0, 1, 1);
  } else if (ev->type == NET_EVENT_RECORD) {
    demo_w_write_msg_sv_record_legacy(writer, ev->server_time_best, ev->player_time_best);
    demo_w_write_msg_sv_record(writer, ev->server_time_best, ev->player_time_best);
  }
  return true;
}

static int export_demo_range(ui_handler_t *ui, const char *path, const char *map_name, int start_tick, int end_tick) {
  if (!ui || !path || !map_name || end_tick < start_tick) return 1;
  timeline_state_t *ts = &ui->timeline;
  int **client_maps = calloc((size_t)ts->group_count, sizeof(*client_maps));
  int *client_counts = calloc((size_t)ts->group_count, sizeof(*client_counts));
  if (!client_maps || !client_counts) {
    free_client_maps(client_maps, client_counts, ts->group_count);
    return 1;
  }

  int exported_clients = 0;
  for (int group_index = 0; group_index < ts->group_count; ++group_index) {
    int count = model_group_track_count(ts, group_index);
    client_counts[group_index] = count;
    if (count > 0) {
      client_maps[group_index] = malloc(sizeof(int) * (size_t)count);
      if (!client_maps[group_index]) {
        free_client_maps(client_maps, client_counts, ts->group_count);
        return 1;
      }
    }
    for (int local_index = 0; local_index < count; ++local_index) {
      int track_index = model_group_track_index(ts, group_index, local_index);
      bool selected = ts->groups[group_index]->demo_export_enabled && track_index >= 0 && ts->player_tracks[track_index].demo_export_enabled;
      client_maps[group_index][local_index] = selected ? exported_clients++ : -1;
    }
  }
  if (exported_clients == 0 || exported_clients > 64) {
    log_error(LOG_SOURCE, "Demo export requires between 1 and 64 selected tracks (selected: %d)", exported_clients);
    free_client_maps(client_maps, client_counts, ts->group_count);
    return 1;
  }

  // set up demo things
  void *map_data = ui->gfx_handler->physics_handler.collision.m_MapData._map_file_data;
  size_t map_size = ui->gfx_handler->physics_handler.collision.m_MapData._map_file_size;
  uint32_t map_crc = map_crc32(map_data, map_size);
  uint8_t map_sha256[32];
  SHA256_CTX ctx = {0};
  map_sha256_init(&ctx);
  map_sha256_update(&ctx, map_data, map_size);
  map_sha256_final(&ctx, map_sha256);

  dd_demo_writer *writer = demo_w_create();
  FILE *f_demo = fs_open(path, "wb");
  if (!writer || !f_demo) {
    log_error(LOG_SOURCE, "Error: Could not create demo writer or open output file.");
    if (f_demo) fclose(f_demo);
    if (writer) demo_w_destroy(&writer);
    free_client_maps(client_maps, client_counts, ts->group_count);
    return 1;
  }

  if (!demo_w_begin(writer, f_demo, map_name, map_crc, "Race") || !demo_w_write_map(writer, map_sha256, map_data, map_size)) {
    demo_w_destroy(&writer);
    fclose(f_demo);
    free_client_maps(client_maps, client_counts, ts->group_count);
    return 1;
  }

  dd_snapshot_builder *sb = demo_sb_create();
  uint8_t snap_buf[DD_SNAPSHOT_MAX_SIZE];

  SWorldCore *previous_worlds = calloc((size_t)ts->group_count, sizeof(*previous_worlds));
  SWorldCore *current_worlds = calloc((size_t)ts->group_count, sizeof(*current_worlds));
  demo_exporter_t *event_collectors = calloc((size_t)ts->group_count, sizeof(*event_collectors));
  bool *active_groups = calloc((size_t)ts->group_count, sizeof(*active_groups));
  if (!sb || !previous_worlds || !current_worlds || !event_collectors || !active_groups) {
    if (sb) demo_sb_destroy(&sb);
    free(previous_worlds);
    free(current_worlds);
    free(event_collectors);
    free(active_groups);
    demo_w_finish(writer);
    demo_w_destroy(&writer);
    fclose(f_demo);
    free_client_maps(client_maps, client_counts, ts->group_count);
    return 1;
  }

  for (int group_index = 0; group_index < ts->group_count; ++group_index) {
    previous_worlds[group_index] = wc_empty();
    current_worlds[group_index] = wc_empty();
    for (int i = 0; i < client_counts[group_index]; ++i)
      if (client_maps[group_index][i] >= 0) active_groups[group_index] = true;
    if (active_groups[group_index])
      model_get_group_world_state_pair(ts, group_index, start_tick, &previous_worlds[group_index], &current_worlds[group_index], false);
  }

  for (int t = start_tick; t <= end_tick; ++t) {
    demo_sb_clear(sb);
    int next_item_id = 64 + ui->num_pickups;
    bool include_static = true;
    for (int group_index = 0; group_index < ts->group_count; ++group_index) {
      if (!active_groups[group_index]) continue;
      int demo_tick = t - start_tick;
      int tick_delta = ts->groups[group_index]->start_offset - start_tick;
      snap_world(sb, ts, group_index, client_maps[group_index], client_counts[group_index], &previous_worlds[group_index],
                 &current_worlds[group_index], &event_collectors[group_index], include_static, demo_tick, tick_delta, &next_item_id);
      include_static = false;
    }

    int snap_size = demo_sb_finish(sb, snap_buf);
    if (snap_size > 0) demo_w_write_snap(writer, t - start_tick, snap_buf, snap_size);

    // Write Net Events
    for (int i = 0; i < ts->net_event_count; ++i) {
      net_event_t *ev = &ts->net_events[i];
      if (ev->group_index < 0 || ev->group_index >= ts->group_count || !ts->groups[ev->group_index]->demo_export_enabled) continue;
      if (ev->tick + ts->groups[ev->group_index]->start_offset == t)
        write_net_event(writer, ev, client_maps[ev->group_index], client_counts[ev->group_index]);
    }

    if (t == end_tick) break;

    // Advance every isolated physics instance once its offset has elapsed. Keeping these worlds
    // alive across ticks preserves transient hammer/damage events as well as entity ownership.
    for (int group_index = 0; group_index < ts->group_count; ++group_index) {
      if (!active_groups[group_index]) continue;
      demo_exporter_t *collector = &event_collectors[group_index];
      collector->num_hammerhits = 0;
      collector->num_damage_indicators = 0;
      int next_local_tick = imax(0, t + 1 - ts->groups[group_index]->start_offset);
      SWorldCore *cur = &current_worlds[group_index];
      while (cur->m_GameTick < next_local_tick) {
        wc_copy_world(&previous_worlds[group_index], cur);
        cur->user_data = collector;
        cur->particle = on_world_particle;
        cur->damage_indicator = on_damage_indicator;
        for (int local_index = 0; local_index < cur->m_NumCharacters; ++local_index) {
          int track_index = model_group_track_index(ts, group_index, local_index);
          SPlayerInput input = track_index >= 0 ? model_get_input_at_tick(ts, track_index, cur->m_GameTick) : (SPlayerInput){0};
          cc_on_input(&cur->m_pCharacters[local_index], &input);
        }
        wc_tick(cur);
        cur->particle = NULL;
        cur->damage_indicator = NULL;
      }
    }
  }
  demo_w_finish(writer);
  demo_w_destroy(&writer);
  fclose(f_demo);
  demo_sb_destroy(&sb);
  for (int group_index = 0; group_index < ts->group_count; ++group_index) {
    wc_free(&previous_worlds[group_index]);
    wc_free(&current_worlds[group_index]);
  }
  free(previous_worlds);
  free(current_worlds);
  free(event_collectors);
  free(active_groups);
  free_client_maps(client_maps, client_counts, ts->group_count);
  return 0;
}

int export_to_demo(ui_handler_t *ui, const char *path, const char *map_name, int ticks) {
  return ticks > 0 ? export_demo_range(ui, path, map_name, 0, ticks - 1) : 1;
}

void ui_export_demo(ui_handler_t *ui) {
  if (!ui || !ui->gfx_handler || !ui->gfx_handler->physics_handler.loaded) return;
  ui->demo_exporter.range_start_tick = model_get_min_global_tick(&ui->timeline);
  ui->demo_exporter.range_end_tick = model_get_max_timeline_tick(&ui->timeline) + 50;
  ui->demo_exporter.show_export_dialog = true;
}

void demo_render_export_dialog(ui_handler_t *ui) {
  if (!ui) return;
  demo_exporter_t *exporter = &ui->demo_exporter;
  if (exporter->show_export_dialog) {
    igOpenPopup_Str("Export Demo", 0);
    exporter->show_export_dialog = false;
  }

  igSetNextWindowSize((ImVec2){620.0f, 640.0f}, ImGuiCond_FirstUseEver);
  if (!igBeginPopupModal("Export Demo", NULL, ImGuiWindowFlags_None)) return;

  static char error[160] = {0};
  timeline_state_t *ts = &ui->timeline;
  igTextWrapped("Choose the global tick range and the independent groups/tracks that will be combined into the demo.");
  igSetNextItemWidth(150.0f);
  igDragInt("Start tick", &exporter->range_start_tick, 1.0f, -100000000, 100000000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSameLine(0, 12.0f);
  igSetNextItemWidth(150.0f);
  igDragInt("End tick (inclusive)", &exporter->range_end_tick, 1.0f, 0, 100000000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSeparator();

  int selected_count = 0;
  if (igBeginChild_Str("##export_tracks", (ImVec2){0, -72.0f}, true, 0)) {
    for (int group_index = 0; group_index < ts->group_count; ++group_index) {
      timeline_group_t *group = ts->groups[group_index];
      igPushID_Int(group_index);
      bool group_export_before = group->demo_export_enabled;
      if (igCheckbox(group->name, &group->demo_export_enabled)) {
        undo_command_t *command = commands_create_group_demo_export_change(ui, group_index, group_export_before);
        if (command) undo_manager_register_command(&ui->undo_manager, command);
      }
      int local_count = model_group_track_count(ts, group_index);
      if (group->demo_export_enabled) {
        igIndent(18.0f);
        for (int local_index = 0; local_index < local_count; ++local_index) {
          int track_index = model_group_track_index(ts, group_index, local_index);
          if (track_index < 0) continue;
          player_track_t *track = &ts->player_tracks[track_index];
          igPushID_Int(track_index + 10000);
          const char *label = track->name[0] ? track->name : "Track";
          bool track_export_before = track->demo_export_enabled;
          if (igCheckbox(label, &track->demo_export_enabled)) {
            undo_command_t *command = commands_create_track_demo_export_change(ui, track_index, track_export_before);
            if (command) undo_manager_register_command(&ui->undo_manager, command);
          }
          if (track->demo_export_enabled) ++selected_count;
          igSameLine(0, 12.0f);
          int ping_before_frame = track->demo_ping;
          igSetNextItemWidth(82.0f);
          bool ping_changed = igDragInt("Ping", &track->demo_ping, 1.0f, 0, 999, "%d ms", ImGuiSliderFlags_AlwaysClamp);
          if (igIsItemActivated()) {
            g_demo_ping_edit_undo.active = true;
            g_demo_ping_edit_undo.track_index = track_index;
            g_demo_ping_edit_undo.before = ping_before_frame;
          }
          if (ping_changed) ui_mark_unsaved(ui);
          if (igIsItemDeactivatedAfterEdit() && g_demo_ping_edit_undo.active && g_demo_ping_edit_undo.track_index == track_index) {
            undo_command_t *command = commands_create_track_demo_ping_change(ui, track_index, g_demo_ping_edit_undo.before);
            if (command) undo_manager_register_command(&ui->undo_manager, command);
            g_demo_ping_edit_undo.active = false;
          }
          igPopID();
        }
        igUnindent(18.0f);
      }
      igSeparator();
      igPopID();
    }
  }
  igEndChild();

  igText("Selected tracks: %d / 64", selected_count);
  if (error[0]) {
    igSameLine(0, 12.0f);
    igTextColored((ImVec4){1.0f, 0.35f, 0.3f, 1.0f}, "%s", error);
  }

  bool valid = selected_count > 0 && selected_count <= 64 && exporter->range_end_tick >= exporter->range_start_tick;
  if (!valid) igBeginDisabled(true);
  if (igButton("Export...", (ImVec2){120.0f, 0.0f})) {
    const char *map_name = ui->loaded_map_name[0] ? ui->loaded_map_name : "unnamed_map";
    char default_file_name[256];
    snprintf(default_file_name, sizeof(default_file_name), "%s.demo", map_name);
    nfdu8char_t *save_path = NULL;
    nfdu8filteritem_t filters[] = {{"DDNet Demo", "demo"}};
    nfdresult_t result = NFD_SaveDialogU8(&save_path, filters, 1, NULL, default_file_name);
    if (result == NFD_OKAY && save_path) {
      if (export_demo_range(ui, save_path, map_name, exporter->range_start_tick, exporter->range_end_tick) == 0) {
        log_info(LOG_SOURCE, "Demo exported successfully to '%s'", save_path);
        error[0] = '\0';
        igCloseCurrentPopup();
      } else {
        snprintf(error, sizeof(error), "Export failed; see the log for details.");
      }
      NFD_FreePathU8(save_path);
    }
  }
  if (!valid) igEndDisabled();
  igSameLine(0, 10.0f);
  if (igButton("Cancel", (ImVec2){100.0f, 0.0f})) {
    error[0] = '\0';
    igCloseCurrentPopup();
  }
  igEndPopup();
}
