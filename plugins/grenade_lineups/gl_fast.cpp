#include "gl_fast.hpp"
#include "gl_table.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace gl {

uint32_t LandIndex(int MapW, int MapH, ddnet_vec2_t P) {
  const int x = (int)std::floor(P.x + 0.5f), y = (int)std::floor(P.y + 0.5f);
  if (x < 0 || y < 0 || x >= MapW * TILE_UNITS || y >= MapH * TILE_UNITS) return LAND_OUTSIDE;
  return (uint32_t)y * (uint32_t)(MapW * TILE_UNITS) + (uint32_t)x;
}

static int FloorDiv32(int v) { return v >= 0 ? v / 32 : -((-v + 31) / 32); }

void Fast::Init(const Sim *pSim) {
  m_pSim = pSim;
  const ddnet_collision_t *pCol = pSim->m_pCol;
  m_W = pCol->width;
  m_H = pCol->height;
  m_vFlags.assign((size_t)m_W * m_H, 0);
  m_vNearTune.assign((size_t)m_W * m_H, 0);
  m_HasSpecial = m_HasTune = false;
  auto Mark = [&](std::vector<uint8_t> &v, int x, int y, uint8_t Bit) {
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const int nx = x + dx, ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < m_W && ny < m_H) v[(size_t)ny * m_W + nx] |= Bit;
      }
  };
  for (int y = 0; y < m_H; y++)
    for (int x = 0; x < m_W; x++) {
      const size_t i = (size_t)y * m_W + x;
      if (ddnet_collision_check_point(pCol, x * 32.0f + 16.0f, y * 32.0f + 16.0f)) m_vFlags[i] |= FLAG_SOLID;
      if (pCol->tune && pCol->tune[i].type && pCol->tune[i].number) {
        Mark(m_vNearTune, x, y, 1);
        m_HasTune = true;
      }
#if defined(DDNET_PHYSICS_GAME_DDNET)
      if (pCol->tele) {
        const ddnet_tele_tile_t &T = pCol->tele[i];
        if (T.number && T.type == (pSim->m_OldTeleWeapons ? TILE_TELEIN : TILE_TELEINWEAPON) &&
            pCol->tele_outs[T.number - 1].count) {
          Mark(m_vFlags, x, y, FLAG_SPECIAL);
          m_HasSpecial = true;
        }
      }
#endif
    }
  // how far the floats of a tee's real position can be off the ideal ones
  // (a sample is a few roundings at the largest coordinate away: 6 ulps; aims that go further than 6000 units are flown)
  const double Max = (double)std::max(m_W, m_H) * 32.0 + 6000.0;
  const double Ulp = std::ldexp(1.0, (int)std::floor(std::log2(Max)) - 23);
  m_Eps = (float)(8.0 * Ulp);
  const ddnet_tuning_values_t &T = pSim->m_vTunes[0];
  m_Curvature = T.grenade_curvature;
  m_Speed = T.grenade_speed;
  m_Life = (int)(SERVER_TICK_SPEED * T.grenade_lifetime);
}

// CalcPos relative to the start, from the floats that do not depend on where the tee is
static void RelPos(ddnet_vec2_t Dir, float Curvature, float Speed, int k, double *pX, double *pY) {
  float Time = k / (float)SERVER_TICK_SPEED;
  Time *= Speed;
  const float vx = Dir.x * Time, vy = Dir.y * Time, c = Curvature / 10000 * (Time * Time);
  *pX = (double)vx;
  *pY = (double)vy + (double)c;
}

void Fast::BuildOne(const Aim &A, Template *pT) const {
  pT->m_Fast = false;
  pT->m_vBegin.assign(TILE_POSITIONS + 1, 0);
  pT->m_vEntries.clear();
  const int Ticks = m_Life + 1;
  if (Ticks < 1 || Ticks > 4000) return;
  const double Fx = (double)(A.m_Dir.x * 28.0f * 0.75f), Fy = (double)(A.m_Dir.y * 28.0f * 0.75f);

  struct Sample {
    int nx, ny;
    int altx, alty; // the other rounding when it is too close to call, else the same
    bool amb;
  };
  struct Tick {
    bool exact;
    std::vector<Sample> vSamples;
  };
  std::vector<Tick> vTicks((size_t)Ticks + 1);
  auto Round = [&](double v, int *pN, int *pAlt) {
    const double f = std::floor(v), Frac = v - f;
    *pN = (int)(Frac < 0.5 ? f : f + 1);
    *pAlt = std::fabs(Frac - 0.5) < m_Eps ? (*pN == (int)f ? (int)f + 1 : (int)f) : *pN;
    return *pAlt != *pN;
  };
  double x0, y0;
  RelPos(A.m_Dir, m_Curvature, m_Speed, 0, &x0, &y0);
  for (int k = 1; k <= Ticks; k++) {
    double x1, y1;
    RelPos(A.m_Dir, m_Curvature, m_Speed, k, &x1, &y1);
    const double ax = Fx + x0, ay = Fy + y0, bx = Fx + x1, by = Fy + y1;
    if (std::fabs(bx) > 6000.0 || std::fabs(by) > 6000.0) return; // could be clipped off the map
    const double Dist = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) + 1.0;
    Tick &T = vTicks[(size_t)k];
    T.exact = std::fabs(Dist - std::round(Dist)) < m_Eps;
    const int End = (int)Dist;
    if (End > SAMPLE_MASK) return;
    if (!T.exact) {
      T.vSamples.resize((size_t)End + 1);
      for (int i = 0; i <= End; i++) {
        const float a = i / (float)End;
        Sample &S = T.vSamples[(size_t)i];
        const bool AmbX = Round(ax + (bx - ax) * a, &S.nx, &S.altx);
        const bool AmbY = Round(ay + (by - ay) * a, &S.ny, &S.alty);
        S.amb = AmbX || AmbY;
      }
    }
    x0 = x1, y0 = y1;
  }

  for (int r = 0; r < TILE_POSITIONS; r++) {
    const int rx = r & 31, ry = r >> 5;
    int Ptx = INT32_MIN, Pty = INT32_MIN;
    for (int k = 1; k <= Ticks; k++) {
      const Tick &T = vTicks[(size_t)k];
      if (T.exact) {
        pT->m_vEntries.push_back({0, 0, 0, 0, (uint16_t)k, ENTRY_EXACT});
        Ptx = Pty = INT32_MIN;
        continue;
      }
      for (size_t i = 0; i < T.vSamples.size(); i++) {
        const Sample &S = T.vSamples[i];
        const int tx = FloorDiv32(rx + S.nx), ty = FloorDiv32(ry + S.ny);
        if (S.amb && (tx != FloorDiv32(rx + S.altx) || ty != FloorDiv32(ry + S.alty))) {
          pT->m_vEntries.push_back({0, 0, 0, 0, (uint16_t)k, (uint16_t)(i | ENTRY_SAMPLE_EXACT)});
          Ptx = Pty = INT32_MIN;
          continue;
        }
        if (tx == Ptx && ty == Pty) continue;
        Ptx = tx, Pty = ty;
        pT->m_vEntries.push_back({(int16_t)tx, (int16_t)ty, (int16_t)S.nx, (int16_t)S.ny, (uint16_t)k,
                                  (uint16_t)(i | (S.amb ? ENTRY_LAND_EXACT : 0))});
      }
    }
    pT->m_vBegin[(size_t)r + 1] = (uint32_t)pT->m_vEntries.size();
  }
  pT->m_vEntries.shrink_to_fit();
  pT->m_Fast = true;
}

void Fast::Build(const std::vector<Aim> &vAims, int First, int Count, int Threads) {
  m_vAims = vAims;
  m_First = First;
  m_vTemplates.assign((size_t)Count, Template());
  std::vector<std::thread> vThreads;
  const int N = std::max(1, std::min(Threads, Count));
  for (int t = 0; t < N; t++)
    vThreads.emplace_back([&, t] {
      for (int a = t; a < Count; a += N) BuildOne(vAims[(size_t)(First + a)], &m_vTemplates[(size_t)a]);
    });
  for (std::thread &T : vThreads) T.join();
}

void Fast::TickEnds(int x, int y, ddnet_vec2_t Dir, int k, ddnet_vec2_t *pPos0, ddnet_vec2_t *pPos1) const {
  const ddnet_vec2_t Pos = {(float)x + Dir.x * 28.0f * 0.75f, (float)y + Dir.y * 28.0f * 0.75f};
  *pPos0 = Sim::CalcPos(Pos, Dir, m_Curvature, m_Speed, (k - 1) / (float)SERVER_TICK_SPEED);
  *pPos1 = Sim::CalcPos(Pos, Dir, m_Curvature, m_Speed, k / (float)SERVER_TICK_SPEED);
}

int Fast::ExactTick(int x, int y, ddnet_vec2_t Dir, int k, ddnet_vec2_t *pHit) const {
  ddnet_vec2_t a, b;
  TickEnds(x, y, Dir, k, &a, &b);
  if (!m_HasSpecial) {
    ddnet_vec2_t Before;
    return ddnet_collision_intersect_line(m_pSim->m_pCol, a, b, pHit, &Before) ? 1 : 0;
  }
  const float dx = a.x - b.x, dy = a.y - b.y;
  const float Distance = sqrtf(dx * dx + dy * dy);
  const int End = (int)(Distance + 1);
  for (int i = 0; i <= End; i++) {
    const float f = i / (float)End;
    const ddnet_vec2_t P = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
    const uint8_t Flags = TileFlags(Sim::RoundToInt(P.x) / 32, Sim::RoundToInt(P.y) / 32);
    if (Flags & FLAG_SPECIAL) return -1;
    if (Flags & FLAG_SOLID) {
      *pHit = P;
      return 1;
    }
  }
  return 0;
}

ddnet_vec2_t Fast::ExactSample(int x, int y, ddnet_vec2_t Dir, int k, int i) const {
  ddnet_vec2_t a, b;
  TickEnds(x, y, Dir, k, &a, &b);
  const float dx = a.x - b.x, dy = a.y - b.y;
  const int End = (int)(sqrtf(dx * dx + dy * dy) + 1);
  const float f = i / (float)End;
  return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

uint32_t Fast::FlyLand(int x, int y, ddnet_vec2_t Dir) const {
  ddnet_vec2_t P;
  return m_pSim->Fly({(float)x, (float)y}, Dir, &P) == OUTCOME_RANDOM_TELE ? LAND_RANDOM : LandIndex(m_W, m_H, P);
}

uint32_t Fast::Land(int x, int y, int Angle) const {
  const ddnet_vec2_t Dir = m_vAims[(size_t)Angle].m_Dir;
  const Template &T = m_vTemplates[(size_t)(Angle - m_First)];
  const int Ptx = x >> 5, Pty = y >> 5;
  if (!T.m_Fast || m_vNearTune[(size_t)Pty * m_W + Ptx]) return FlyLand(x, y, Dir);
  const int r = (y & 31) << 5 | (x & 31);
  const Entry *pEnd = T.m_vEntries.data() + T.m_vBegin[(size_t)r + 1];
  for (const Entry *e = T.m_vEntries.data() + T.m_vBegin[(size_t)r]; e < pEnd; e++) {
    if (e->m_Sample & ENTRY_EXACT) {
      ddnet_vec2_t Hit;
      const int Result = ExactTick(x, y, Dir, e->m_Tick, &Hit);
      if (Result < 0) return FlyLand(x, y, Dir);
      if (Result > 0) return LandIndex(m_W, m_H, Hit);
      continue;
    }
    if (e->m_Sample & ENTRY_SAMPLE_EXACT) {
      const ddnet_vec2_t P = ExactSample(x, y, Dir, e->m_Tick, e->m_Sample & SAMPLE_MASK);
      const uint8_t Flags = TileFlags(Sim::RoundToInt(P.x) / 32, Sim::RoundToInt(P.y) / 32);
      if (Flags & FLAG_SPECIAL) return FlyLand(x, y, Dir);
      if (Flags & FLAG_SOLID) return LandIndex(m_W, m_H, P);
      continue;
    }
    const uint8_t Flags = TileFlags(Ptx + e->m_Tx, Pty + e->m_Ty);
    if (Flags & FLAG_SPECIAL) return FlyLand(x, y, Dir);
    if (Flags & FLAG_SOLID) {
      if (e->m_Sample & ENTRY_LAND_EXACT)
        return LandIndex(m_W, m_H, ExactSample(x, y, Dir, e->m_Tick, e->m_Sample & SAMPLE_MASK));
      const int lx = x + e->m_Nx, ly = y + e->m_Ny;
      if (lx < 0 || ly < 0 || lx >= m_W * TILE_UNITS || ly >= m_H * TILE_UNITS) return LAND_OUTSIDE;
      return (uint32_t)ly * (uint32_t)(m_W * TILE_UNITS) + (uint32_t)lx;
    }
  }
  ddnet_vec2_t a, b;
  TickEnds(x, y, Dir, m_Life + 1, &a, &b);
  return LandIndex(m_W, m_H, b);
}

bool Fast::Reach(const std::vector<Aim> &vAims, float *pMinX, float *pMinY, float *pMaxX, float *pMaxY) const {
  if (m_HasSpecial || m_HasTune) return false;
  double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
  for (const Aim &A : vAims) {
    const double Fx = (double)(A.m_Dir.x * 28.0f * 0.75f), Fy = (double)(A.m_Dir.y * 28.0f * 0.75f);
    for (int k = 0; k <= m_Life + 1; k++) {
      double x, y;
      RelPos(A.m_Dir, m_Curvature, m_Speed, k, &x, &y);
      MinX = std::min(MinX, Fx + x), MaxX = std::max(MaxX, Fx + x);
      MinY = std::min(MinY, Fy + y), MaxY = std::max(MaxY, Fy + y);
    }
  }
  *pMinX = (float)MinX - 4, *pMinY = (float)MinY - 4, *pMaxX = (float)MaxX + 4, *pMaxY = (float)MaxY + 4;
  return true;
}

} // namespace gl
