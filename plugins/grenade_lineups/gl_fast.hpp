// Landings without flying every grenade. For one aim, the shots of all tees
// are one arc shifted by whole units, so which tiles it passes depends only on
// where in its tile the tee is: per aim and position within a tile, the tiles
// the arc enters, in order. A landing is the first solid one of them. Where
// float rounding at the tee's real position could change a sample, that tick
// is computed exactly; near teleporters and tune zones it is a whole flight.
#ifndef GL_FAST_HPP
#define GL_FAST_HPP

#include "gl_table.hpp"

#include <cstdint>
#include <vector>

namespace gl {

// The unit a grenade explodes on, as Table stores it.
uint32_t LandIndex(int MapW, int MapH, ddnet_vec2_t P);

class Fast {
public:
  void Init(const Sim *pSim);
  // the templates of aims [First, First + Count)
  void Build(const std::vector<Aim> &vAims, int First, int Count, int Threads);
  uint32_t Land(int x, int y, int Angle) const;
  // the box every flight of these aims stays in, relative to the tee; false
  // where a grenade can go anywhere (teleporters, tune zones)
  bool Reach(const std::vector<Aim> &vAims, float *pMinX, float *pMinY, float *pMaxX, float *pMaxY) const;

  struct Entry {
    int16_t m_Tx, m_Ty; // the tile, relative to the tee's
    int16_t m_Nx, m_Ny; // where the sample rounds to, relative to the tee
    uint16_t m_Tick;
    uint16_t m_Sample; // and flags
  };
  // EXACT: the whole tick at the real position. SAMPLE_EXACT: that sample, which may round
  // into either of two tiles. LAND_EXACT: where it lands, if this tile is solid.
  enum : uint16_t { ENTRY_EXACT = 0x8000, ENTRY_LAND_EXACT = 0x4000, ENTRY_SAMPLE_EXACT = 0x2000, SAMPLE_MASK = 0x1FFF };

private:
  enum { FLAG_SOLID = 1, FLAG_SPECIAL = 2 };
  struct Template {
    bool m_Fast = false;
    std::vector<uint32_t> m_vBegin; // [1025], by position within the tile
    std::vector<Entry> m_vEntries;
  };
  void BuildOne(const Aim &A, Template *pT) const;
  uint8_t TileFlags(int tx, int ty) const {
    tx = tx < 0 ? 0 : (tx >= m_W ? m_W - 1 : tx);
    ty = ty < 0 ? 0 : (ty >= m_H ? m_H - 1 : ty);
    return m_vFlags[(size_t)ty * m_W + tx];
  }
  void TickEnds(int x, int y, ddnet_vec2_t Dir, int k, ddnet_vec2_t *pPos0, ddnet_vec2_t *pPos1) const;
  // the tick exactly as collision_intersect_line samples it: 1 hit, 0 none, -1 near something special
  int ExactTick(int x, int y, ddnet_vec2_t Dir, int k, ddnet_vec2_t *pHit) const;
  ddnet_vec2_t ExactSample(int x, int y, ddnet_vec2_t Dir, int k, int i) const;
  uint32_t FlyLand(int x, int y, ddnet_vec2_t Dir) const;

  const Sim *m_pSim = nullptr;
  int m_W = 0, m_H = 0;
  std::vector<uint8_t> m_vFlags;    // per tile
  std::vector<uint8_t> m_vNearTune; // per tile: a tune zone in its 3x3
  bool m_HasSpecial = false, m_HasTune = false;
  float m_Eps = 0.0f;
  float m_Curvature = 0.0f, m_Speed = 0.0f;
  int m_Life = 0;
  int m_First = 0;
  std::vector<Aim> m_vAims;
  std::vector<Template> m_vTemplates;
};

} // namespace gl

#endif
