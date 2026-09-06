#ifndef DD_CHARACTER_STATE_H
#define DD_CHARACTER_STATE_H

#include <ddnet_physics/gamecore.h>
#include <string.h>

// Version 1 projects stored the original 544-byte character layout on 64-bit
// builds. Keep that disk record frozen as the runtime layout evolves. Pointer
// slots are reserved bytes: writes zero them and reads keep the live pointers.
// Access records with memcpy because the 20-byte world header is not SIMD aligned.
typedef struct {
  uint64_t m_pWorld;
  uint64_t m_pCollision;
  int m_Id;
  mvec2 m_PrevPos;
  mvec2 m_Pos;
  mvec2 m_Vel;
  uivec2 m_BlockPos;
  int m_BlockIdx;
  unsigned char m_BlockInfo;
  mvec2 m_HookPos;
  mvec2 m_HookDir;
  mvec2 m_HookTeleBase;
  int m_HookTick;
  int8_t m_HookState;
  unsigned char m_LastWeapon;
  unsigned char m_ActiveWeapon;
  bool m_aWeaponGot[8];
  struct {
    mvec2 m_ActivationDir;
    int m_ActivationTick;
    int m_CurrentMoveTime;
    int m_OldVelAmount;
  } m_Ninja;
  bool m_NewHook;
  bool m_Grounded;
  int m_Jumped;
  int m_JumpedTotal;
  int m_Jumps;
  unsigned char m_PrevFire;
  SPlayerInput m_Input;
  int m_StartTime;
  unsigned char m_Colliding;
  bool m_LeftWall;
  unsigned char m_TeleCheckpoint;
  bool m_LastRefillJumps;
  bool m_LastPenalty;
  bool m_LastBonus;
  bool m_Solo;
  bool m_Jetpack;
  bool m_CollisionDisabled;
  bool m_EndlessHook;
  bool m_EndlessJump;
  bool m_HammerHitDisabled;
  bool m_GrenadeHitDisabled;
  bool m_LaserHitDisabled;
  bool m_ShotgunHitDisabled;
  bool m_HookHitDisabled;
  bool m_HasTelegunGun;
  bool m_HasTelegunGrenade;
  bool m_HasTelegunLaser;
  int m_FreezeTime;
  int m_FreezeStart;
  bool m_DeepFrozen;
  bool m_LiveFrozen;
  bool m_FrozenLastTick;
  int m_TuningBlockIdx;
  uint64_t m_pTuning;
  unsigned char m_MoveRestrictions;
  int m_HookedPlayer;
  mvec2 m_TeleGunPos;
  bool m_TeleGunTeleport;
  bool m_IsBlueTeleGunTeleport;
  int m_ReloadTimer;
  int m_aHitObjects[10];
  uint8_t m_NumObjectsHit;
  int m_StartTick;
  int m_FinishTick;
  float m_StartTickOffset;
  float m_FinishTickOffset;
  float m_RaceTime;
  float m_aTimeCp[NUM_TIME_CHECKPOINTS];
  uint32_t m_TimeCpMask;
  int m_LastTimeCp;
  float m_TileFraction;
  bool m_aGotFastcapFlag[2];
  int m_FastcapStartTeam;
  int m_DamageTick;
  int m_DamageTaken;
  uint8_t m_RespawnDelay;
  int m_HitNum;
  int m_AttackTick;
  bool m_IsInFreeze;
  float m_VelMag;
  float m_VelRamp;
  int8_t m_aWeaponAmmo[NUM_WEAPONS];
  int8_t m_Health;
  int8_t m_Armor;
  uint32_t m_SpawnGeneration;
} dd_character_state_v1;

_Static_assert(sizeof(dd_character_state_v1) == 544, "DDNet version 1 character record changed");

#define DD_CHARACTER_STATE_FIELDS(FIELD, ARRAY) \
  FIELD(m_Id) \
  FIELD(m_PrevPos) \
  FIELD(m_Pos) \
  FIELD(m_Vel) \
  FIELD(m_BlockPos) \
  FIELD(m_BlockIdx) \
  FIELD(m_BlockInfo) \
  FIELD(m_HookPos) \
  FIELD(m_HookDir) \
  FIELD(m_HookTeleBase) \
  FIELD(m_HookTick) \
  FIELD(m_HookState) \
  FIELD(m_LastWeapon) \
  FIELD(m_ActiveWeapon) \
  FIELD(m_Ninja.m_ActivationDir) \
  FIELD(m_Ninja.m_ActivationTick) \
  FIELD(m_Ninja.m_CurrentMoveTime) \
  FIELD(m_Ninja.m_OldVelAmount) \
  FIELD(m_NewHook) \
  FIELD(m_Grounded) \
  FIELD(m_Jumped) \
  FIELD(m_JumpedTotal) \
  FIELD(m_Jumps) \
  FIELD(m_PrevFire) \
  FIELD(m_Input) \
  FIELD(m_StartTime) \
  FIELD(m_Colliding) \
  FIELD(m_LeftWall) \
  FIELD(m_TeleCheckpoint) \
  FIELD(m_LastRefillJumps) \
  FIELD(m_LastPenalty) \
  FIELD(m_LastBonus) \
  FIELD(m_Solo) \
  FIELD(m_Jetpack) \
  FIELD(m_CollisionDisabled) \
  FIELD(m_EndlessHook) \
  FIELD(m_EndlessJump) \
  FIELD(m_HammerHitDisabled) \
  FIELD(m_GrenadeHitDisabled) \
  FIELD(m_LaserHitDisabled) \
  FIELD(m_ShotgunHitDisabled) \
  FIELD(m_HookHitDisabled) \
  FIELD(m_HasTelegunGun) \
  FIELD(m_HasTelegunGrenade) \
  FIELD(m_HasTelegunLaser) \
  FIELD(m_FreezeTime) \
  FIELD(m_FreezeStart) \
  FIELD(m_DeepFrozen) \
  FIELD(m_LiveFrozen) \
  FIELD(m_FrozenLastTick) \
  FIELD(m_TuningBlockIdx) \
  FIELD(m_MoveRestrictions) \
  FIELD(m_HookedPlayer) \
  FIELD(m_TeleGunPos) \
  FIELD(m_TeleGunTeleport) \
  FIELD(m_IsBlueTeleGunTeleport) \
  FIELD(m_ReloadTimer) \
  FIELD(m_NumObjectsHit) \
  FIELD(m_StartTick) \
  FIELD(m_FinishTick) \
  FIELD(m_StartTickOffset) \
  FIELD(m_FinishTickOffset) \
  FIELD(m_RaceTime) \
  FIELD(m_TimeCpMask) \
  FIELD(m_LastTimeCp) \
  FIELD(m_TileFraction) \
  FIELD(m_FastcapStartTeam) \
  FIELD(m_DamageTick) \
  FIELD(m_DamageTaken) \
  FIELD(m_RespawnDelay) \
  FIELD(m_HitNum) \
  FIELD(m_AttackTick) \
  FIELD(m_IsInFreeze) \
  FIELD(m_VelMag) \
  FIELD(m_VelRamp) \
  FIELD(m_Health) \
  FIELD(m_Armor) \
  FIELD(m_SpawnGeneration) \
  ARRAY(m_aWeaponGot) \
  ARRAY(m_aHitObjects) \
  ARRAY(m_aTimeCp) \
  ARRAY(m_aGotFastcapFlag) \
  ARRAY(m_aWeaponAmmo)

static inline void dd_character_state_write(void *out, const SCharacterCore *character) {
  dd_character_state_v1 state;
  memset(&state, 0, sizeof(state));
#define FIELD(name) state.name = character->name;
#define ARRAY(name) memcpy(state.name, character->name, sizeof(state.name));
  DD_CHARACTER_STATE_FIELDS(FIELD, ARRAY)
#undef ARRAY
#undef FIELD
  memcpy(out, &state, sizeof(state));
}

static inline void dd_character_state_read(SCharacterCore *character, const void *data) {
  dd_character_state_v1 state;
  memcpy(&state, data, sizeof(state));
#define FIELD(name) character->name = state.name;
#define ARRAY(name) memcpy(character->name, state.name, sizeof(character->name));
  DD_CHARACTER_STATE_FIELDS(FIELD, ARRAY)
#undef ARRAY
#undef FIELD
}

#undef DD_CHARACTER_STATE_FIELDS

#endif
