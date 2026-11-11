// The flight of one grenade, as the physics fly it (CProjectile::Tick), with
// no tees in the way. Built with -ffp-contract=off like the physics, so the
// floats come out the same.
#ifndef GL_SIM_HPP
#define GL_SIM_HPP

#include <ddnet_physics/ddnet_physics.h>
extern "C" {
#include <ddnet_map_loader.h>
}

#include <cmath>
#include <vector>

namespace gl {

enum { SERVER_TICK_SPEED = 50 };

enum Outcome {
  OUTCOME_EXPLODED,
  // a weapon teleporter with several exits: where it lands depends on the tee
  OUTCOME_RANDOM_TELE,
};

struct Sim {
  const ddnet_collision_t *m_pCol = nullptr;
  std::vector<ddnet_tuning_values_t> m_vTunes; // by tune zone
  bool m_OldTeleWeapons = false;

  // Takes the tuning and settings of `pWorld`, on `pCol` (its collision when NULL).
  bool Init(const ddnet_world_t *pWorld, const ddnet_collision_t *pCol = nullptr) {
    if (!pWorld || !pWorld->tuning_values || pWorld->num_tune_zones <= 0) return false;
    m_pCol = pCol ? pCol : pWorld->collision;
    if (!m_pCol) return false;
    m_vTunes.assign(pWorld->tuning_values, pWorld->tuning_values + pWorld->num_tune_zones);
#if defined(DDNET_PHYSICS_GAME_DDNET)
    m_OldTeleWeapons = pWorld->config.sv_old_teleport_weapons != 0;
#endif
    return true;
  }

  // collision_get_map_index + collision_is_tune
  int TuneZone(float x, float y) const {
    if (!m_pCol->tune) return 0;
    const int Index = TileIndex(x, y);
    const int Zone = m_pCol->tune[Index].type ? m_pCol->tune[Index].number : 0;
    return Zone < (int)m_vTunes.size() ? Zone : 0;
  }

  int TileIndex(float x, float y) const {
    int nx = (int)x / 32, ny = (int)y / 32;
    nx = nx < 0 ? 0 : (nx > m_pCol->width - 1 ? m_pCol->width - 1 : nx);
    ny = ny < 0 ? 0 : (ny > m_pCol->height - 1 ? m_pCol->height - 1 : ny);
    return ny * m_pCol->width + nx;
  }

  static int RoundToInt(float f) { return (int)(f + (f > 0 ? 0.5f : -0.5f)); }

  bool Clipped(ddnet_vec2_t p) const {
    return RoundToInt(p.x) / 32 < -200 || RoundToInt(p.x) / 32 > m_pCol->width + 200 || RoundToInt(p.y) / 32 < -200 ||
           RoundToInt(p.y) / 32 > m_pCol->height + 200;
  }

  // vnormalize of the aim, as CCharacter::FireWeapon does it
  static ddnet_vec2_t Direction(int TargetX, int TargetY) {
    const float x = (float)TargetX, y = (float)TargetY;
    const float Divisor = sqrtf(x * x + y * y);
    if (Divisor == 0.0f) return {0.0f, 0.0f};
    const float l = 1.0f / Divisor;
    return {x * l, y * l};
  }

  // A tee at `Tee` fires a grenade in `Dir`. `pPath`, if given, gets the
  // position at every tick and where it explodes last.
  Outcome Fly(ddnet_vec2_t Tee, ddnet_vec2_t Dir, ddnet_vec2_t *pOut, std::vector<ddnet_vec2_t> *pPath = nullptr) const {
    ddnet_vec2_t Pos = {Tee.x + Dir.x * 28.0f * 0.75f, Tee.y + Dir.y * 28.0f * 0.75f};
    const ddnet_tuning_values_t &Tune = m_vTunes[TuneZone(Pos.x, Pos.y)];
    const float Curvature = Tune.grenade_curvature, Speed = Tune.grenade_speed;
    int Life = (int)(SERVER_TICK_SPEED * m_vTunes[TuneZone(Tee.x, Tee.y)].grenade_lifetime);

    if (pPath) pPath->push_back(Pos);
    // k: ticks since the start tick, which the first tick after firing is 1 of
    for (int k = 1;; k++) {
      const ddnet_vec2_t Prev = CalcPos(Pos, Dir, Curvature, Speed, (k - 1) / (float)SERVER_TICK_SPEED);
      const ddnet_vec2_t Cur = CalcPos(Pos, Dir, Curvature, Speed, k / (float)SERVER_TICK_SPEED);
      ddnet_vec2_t ColPos, NewPos;
      const int Collide = ddnet_collision_intersect_line(m_pCol, Prev, Cur, &ColPos, &NewPos);
      if (Life > -1) Life--;
      if (Collide || Clipped(Cur) || Life == -1) {
        if (pPath) pPath->push_back(ColPos);
        *pOut = ColPos;
        return OUTCOME_EXPLODED;
      }
      if (pPath) pPath->push_back(Cur);
#if defined(DDNET_PHYSICS_GAME_DDNET)
      // collision_get_index: with a tele layer, the tile of Prev
      if (m_pCol->tele) {
        const ddnet_tele_tile_t &T = m_pCol->tele[TileIndex(Prev.x, Prev.y)];
        const int z = T.type == (m_OldTeleWeapons ? TILE_TELEIN : TILE_TELEINWEAPON) ? T.number : 0;
        if (z && m_pCol->tele_outs[z - 1].count != 0) {
          if (m_pCol->tele_outs[z - 1].count > 1) return OUTCOME_RANDOM_TELE;
          Pos = m_pCol->tele_outs[z - 1].positions[0];
          k = 0;
          if (pPath) {
            pPath->push_back({NAN, NAN}); // a break in the line
            pPath->push_back(Pos);
          }
        }
      }
#endif
    }
  }

  static ddnet_vec2_t CalcPos(ddnet_vec2_t Pos, ddnet_vec2_t Vel, float Curvature, float Speed, float Time) {
    Time *= Speed;
    return {Pos.x + Vel.x * Time, Pos.y + Vel.y * Time + Curvature / 10000 * (Time * Time)};
  }
};

} // namespace gl

#endif
