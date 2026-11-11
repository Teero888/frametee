// For every unit position of a map's free tiles and every one of N aims:
// where the grenade fired from there explodes.
#ifndef GL_TABLE_HPP
#define GL_TABLE_HPP

#include "gl_sim.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gl {

enum { TILE_UNITS = 32, TILE_POSITIONS = TILE_UNITS * TILE_UNITS };

// A landing: the unit it explodes on as y * map width in units + x, or one of these.
enum : uint32_t {
  LAND_OUTSIDE = 0xFFFFFFFFu, // off the map
  LAND_RANDOM = 0xFFFFFFFEu,  // through a teleporter with several exits
};

struct Aim {
  int m_TargetX, m_TargetY; // the input's aim
  ddnet_vec2_t m_Dir;
  float m_Degrees; // clockwise from the right, as the screen shows it
};
std::vector<Aim> MakeAims(int Count);

// The positions of a map: every unit of every tile that is not solid. A
// position's index is its tile's slot * TILE_POSITIONS + y * 32 + x within it.
struct Layout {
  int m_W = 0, m_H = 0;
  std::vector<int32_t> m_vTileSlot;  // [W * H], -1 for solid
  std::vector<uint32_t> m_vSlotTile; // the free tiles
  void Build(const ddnet_collision_t *pCol);
  // the same map with only these free tiles
  Layout Subset(const std::vector<uint32_t> &vTiles) const;
  uint64_t Positions() const { return (uint64_t)m_vSlotTile.size() * TILE_POSITIONS; }
  // uncompressed: 4 bytes a landing
  uint64_t RawBytes(int Angles) const { return Positions() * (uint64_t)Angles * sizeof(uint32_t); }
  // whether every unit of the map has a landing index
  bool Addressable() const { return (uint64_t)m_W * m_H * TILE_POSITIONS < LAND_RANDOM; }
  uint32_t WidthUnits() const { return (uint32_t)m_W * TILE_UNITS; }
  void PositionXY(uint64_t Pos, int *pX, int *pY) const {
    const uint32_t Tile = m_vSlotTile[Pos / TILE_POSITIONS];
    const int u = (int)(Pos % TILE_POSITIONS);
    *pX = (int)(Tile % (uint32_t)m_W) * TILE_UNITS + (u & 31);
    *pY = (int)(Tile / (uint32_t)m_W) * TILE_UNITS + (u >> 5);
  }
  // -1 for a solid or off-map unit
  int64_t PositionAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_W * TILE_UNITS || y >= m_H * TILE_UNITS) return -1;
    const int32_t Slot = m_vTileSlot[(size_t)(y / TILE_UNITS) * m_W + x / TILE_UNITS];
    if (Slot < 0) return -1;
    return (int64_t)Slot * TILE_POSITIONS + (y % TILE_UNITS) * TILE_UNITS + x % TILE_UNITS;
  }
};

// The landings of a tile's positions for one aim, each predicted from its
// neighbors (left + up - up-left): most are exactly that, and a run of them
// takes a byte.
void EncodeLandings(const uint32_t *pLand, uint32_t WidthUnits, std::vector<uint8_t> &Out);
void DecodeLandings(const uint8_t *pIn, uint32_t WidthUnits, uint32_t *pLand);

// Runs on threads of its own. Everything it reads is its own copy.
class Table {
public:
  ~Table() { Stop(); }

  bool Start(const Sim &Sim, const Layout &Layout, int Angles, int Threads, std::string *pError);
  void Stop();
  // true once it is finished (and its threads are joined)
  bool Poll();
  bool Running() const { return m_Driver.joinable(); }
  bool Complete() const { return m_Complete; }
  double Progress() const { return m_Total ? (double)m_Done.load() / (double)m_Total : 1.0; }

  const Layout &GetLayout() const { return m_Layout; }
  const Sim &GetSim() const { return m_Sim; }
  const std::vector<Aim> &Aims() const { return m_vAims; }
  int NumAngles() const { return (int)m_vAims.size(); }
  void Decode(uint32_t Slot, int Angle, uint32_t *pLand) const {
    DecodeLandings(m_vStreams[(size_t)Angle * m_Layout.m_vSlotTile.size() + Slot], m_Layout.WidthUnits(), pLand);
  }
  uint32_t Land(uint64_t Pos, int Angle) const;
  uint64_t Bytes() const;
  // the box, relative to a tee, that its grenades land in; false if anywhere
  bool Reach(float *pBox) const {
    for (int i = 0; i < 4; i++) pBox[i] = m_aReach[i];
    return m_HasReach;
  }

private:
  void Drive(int Threads);
  struct Arena {
    std::vector<std::unique_ptr<uint8_t[]>> m_vBlocks;
    size_t m_Used = 0, m_Cap = 0;
    uint64_t m_Total = 0;
    uint8_t *Alloc(size_t Size);
  };

  Sim m_Sim;
  Layout m_Layout;
  std::vector<Aim> m_vAims;
  std::vector<const uint8_t *> m_vStreams; // [angle * slots + slot]
  std::vector<Arena> m_vArenas;            // one per thread
  std::thread m_Driver;
  std::atomic<uint64_t> m_Done{0};
  uint64_t m_Total = 0;
  std::atomic<bool> m_Cancel{false}, m_Finished{false};
  bool m_Complete = false;
  bool m_HasReach = false;
  float m_aReach[4] = {0, 0, 0, 0};
};

// What an explosion pushes: a tee closer than this (CreateExplosion).
constexpr float EXPLOSION_RADIUS = 135.0f;

// The positions with an aim that lands closer than a radius to a unit.
struct Target {
  int m_X = 0, m_Y = 0;
  float m_Radius = EXPLOSION_RADIUS;
  bool Hits(const Layout &L, uint32_t Land) const {
    if (Land >= LAND_RANDOM) return false;
    const uint32_t W = L.WidthUnits();
    const float dx = (float)((int)(Land % W) - m_X), dy = (float)((int)(Land / W) - m_Y);
    return dx * dx + dy * dy < m_Radius * m_Radius;
  }
};

struct QueryResult {
  Target m_Target;
  std::vector<uint64_t> m_vPos;       // ascending
  std::vector<uint16_t> m_vCount;     // aims that land there, per position
  std::vector<uint32_t> m_vSlotBegin; // [slots + 1]: m_vPos of a tile's slot
  uint32_t m_MaxCount = 0;
  uint64_t m_Aims = 0; // all of them
};

class Query {
public:
  ~Query() { Wait(); }
  void Start(const Table *pTable, Target T, int Threads);
  bool Running() const { return m_Thread.joinable() && !m_Done.load(); }
  // true when a result is ready to be taken
  bool Poll();
  void Wait() {
    if (m_Thread.joinable()) m_Thread.join();
  }
  QueryResult Take() { return std::move(m_Result); }

private:
  std::thread m_Thread;
  std::atomic<bool> m_Done{false};
  QueryResult m_Result;
};

} // namespace gl

#endif
