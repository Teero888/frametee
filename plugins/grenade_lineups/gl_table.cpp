#include "gl_table.hpp"
#include "gl_fast.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace gl {

std::vector<Aim> MakeAims(int Count) {
  // as far out as the input's aim reaches, for the finest directions
  const double Radius = 30000.0;
  std::vector<Aim> vAims((size_t)std::max(Count, 1));
  for (size_t i = 0; i < vAims.size(); i++) {
    const double Angle = 2.0 * M_PI * (double)i / (double)vAims.size();
    Aim &A = vAims[i];
    A.m_TargetX = (int)std::lround(std::cos(Angle) * Radius);
    A.m_TargetY = (int)std::lround(std::sin(Angle) * Radius);
    A.m_Dir = Sim::Direction(A.m_TargetX, A.m_TargetY);
    A.m_Degrees = (float)(360.0 * (double)i / (double)vAims.size());
  }
  return vAims;
}

void Layout::Build(const ddnet_collision_t *pCol) {
  m_W = pCol->width;
  m_H = pCol->height;
  m_vTileSlot.assign((size_t)m_W * m_H, -1);
  m_vSlotTile.clear();
  for (int y = 0; y < m_H; y++)
    for (int x = 0; x < m_W; x++) {
      if (ddnet_collision_check_point(pCol, x * 32.0f + 16.0f, y * 32.0f + 16.0f)) continue;
      m_vTileSlot[(size_t)y * m_W + x] = (int32_t)m_vSlotTile.size();
      m_vSlotTile.push_back((uint32_t)(y * m_W + x));
    }
}

Layout Layout::Subset(const std::vector<uint32_t> &vTiles) const {
  Layout L;
  L.m_W = m_W, L.m_H = m_H;
  L.m_vTileSlot.assign(m_vTileSlot.size(), -1);
  for (uint32_t Tile : vTiles) {
    if (m_vTileSlot[Tile] < 0 || L.m_vTileSlot[Tile] >= 0) continue;
    L.m_vTileSlot[Tile] = (int32_t)L.m_vSlotTile.size();
    L.m_vSlotTile.push_back(Tile);
  }
  return L;
}

// --- the landings of a tile, compressed --------------------------------------

namespace {

struct Unit {
  int32_t x, y; // x < 0: y is 0 off the map, 1 random
  bool Valid() const { return x >= 0; }
  bool operator==(const Unit &o) const { return x == o.x && y == o.y; }
};

Unit ToUnit(uint32_t Land, uint32_t W) {
  if (Land >= LAND_RANDOM) return {-1, Land == LAND_RANDOM ? 1 : 0};
  return {(int32_t)(Land % W), (int32_t)(Land / W)};
}

uint32_t FromUnit(Unit u, uint32_t W) {
  if (!u.Valid()) return u.y ? LAND_RANDOM : LAND_OUTSIDE;
  return (uint32_t)u.y * W + (uint32_t)u.x;
}

// false when there is nothing to predict from
bool Predict(const Unit *pU, int i, Unit *pOut) {
  const int x = i & 31, y = i >> 5;
  const Unit *l = x ? &pU[i - 1] : nullptr, *up = y ? &pU[i - 32] : nullptr, *ul = x && y ? &pU[i - 33] : nullptr;
  if (l && up && ul && l->Valid() && up->Valid() && ul->Valid())
    *pOut = {l->x + up->x - ul->x, l->y + up->y - ul->y};
  else if (l && l->Valid())
    *pOut = {l->x + 1, l->y};
  else if (up && up->Valid())
    *pOut = {up->x, up->y + 1};
  else if (l)
    *pOut = *l;
  else if (up)
    *pOut = *up;
  else
    return false;
  return true;
}

enum {
  TOKEN_RUN = 0x00,    // 0x00-0x3f: 1-64 as predicted
  TOKEN_SMALL = 0x40,  // 0x40-0x7f: off by -4..3 in x and y
  TOKEN_MEDIUM = 0x80, // 0x80-0xbf + 2 bytes: off by -1024..1023
  TOKEN_ABSOLUTE = 0xc0,
  TOKEN_OUTSIDE = 0xc1,
  TOKEN_RANDOM = 0xc2,
};

} // namespace

void EncodeLandings(const uint32_t *pLand, uint32_t W, std::vector<uint8_t> &Out) {
  Unit aU[TILE_POSITIONS];
  for (int i = 0; i < TILE_POSITIONS; i++) aU[i] = ToUnit(pLand[i], W);
  int Run = 0;
  auto Flush = [&] {
    if (Run) Out.push_back((uint8_t)(TOKEN_RUN | (Run - 1)));
    Run = 0;
  };
  for (int i = 0; i < TILE_POSITIONS; i++) {
    Unit P;
    const bool Have = Predict(aU, i, &P);
    const Unit &U = aU[i];
    if (Have && P == U) {
      if (++Run == 64) Flush();
      continue;
    }
    Flush();
    if (!U.Valid()) {
      Out.push_back(U.y ? TOKEN_RANDOM : TOKEN_OUTSIDE);
      continue;
    }
    if (Have && P.Valid()) {
      const int ex = U.x - P.x, ey = U.y - P.y;
      if (ex >= -4 && ex <= 3 && ey >= -4 && ey <= 3) {
        Out.push_back((uint8_t)(TOKEN_SMALL | (ex + 4) << 3 | (ey + 4)));
        continue;
      }
      if (ex >= -1024 && ex <= 1023 && ey >= -1024 && ey <= 1023) {
        const uint32_t v = (uint32_t)(ex + 1024) << 11 | (uint32_t)(ey + 1024);
        Out.push_back((uint8_t)(TOKEN_MEDIUM | v >> 16));
        Out.push_back((uint8_t)(v >> 8));
        Out.push_back((uint8_t)v);
        continue;
      }
    }
    const uint32_t L = FromUnit(U, W);
    Out.push_back(TOKEN_ABSOLUTE);
    for (int b = 0; b < 4; b++) Out.push_back((uint8_t)(L >> (8 * b)));
  }
  Flush();
}

void DecodeLandings(const uint8_t *p, uint32_t W, uint32_t *pLand) {
  Unit aU[TILE_POSITIONS];
  int i = 0;
  while (i < TILE_POSITIONS) {
    const uint8_t t = *p++;
    if (t < TOKEN_SMALL) {
      for (int n = 0; n <= t; n++, i++) Predict(aU, i, &aU[i]);
    } else if (t < TOKEN_MEDIUM) {
      Predict(aU, i, &aU[i]);
      aU[i].x += ((t >> 3) & 7) - 4;
      aU[i].y += (t & 7) - 4;
      i++;
    } else if (t < TOKEN_ABSOLUTE) {
      const uint32_t v = (uint32_t)(t & 0x3f) << 16 | (uint32_t)p[0] << 8 | p[1];
      p += 2;
      Predict(aU, i, &aU[i]);
      aU[i].x += (int)(v >> 11) - 1024;
      aU[i].y += (int)(v & 0x7ff) - 1024;
      i++;
    } else if (t == TOKEN_ABSOLUTE) {
      const uint32_t L = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
      p += 4;
      aU[i++] = ToUnit(L, W);
    } else {
      aU[i++] = {-1, t == TOKEN_RANDOM ? 1 : 0};
    }
  }
  for (int j = 0; j < TILE_POSITIONS; j++) pLand[j] = FromUnit(aU[j], W);
}

// --- the table -----------------------------------------------------------------

uint8_t *Table::Arena::Alloc(size_t Size) {
  if (m_Used + Size > m_Cap) {
    m_Cap = std::max<size_t>(Size, 4 << 20);
    m_vBlocks.emplace_back(new uint8_t[m_Cap]);
    m_Used = 0;
  }
  uint8_t *p = m_vBlocks.back().get() + m_Used;
  m_Used += Size;
  m_Total += Size;
  return p;
}

bool Table::Start(const Sim &S, const Layout &L, int Angles, int Threads, std::string *pError) {
  Stop();
  m_Complete = false;
  if (!L.Addressable()) {
    *pError = "The map is too large for this table.";
    return false;
  }
  m_Sim = S;
  m_Layout = L;
  m_vAims = MakeAims(Angles);
  m_vArenas.clear();
  m_vStreams.assign(m_vAims.size() * m_Layout.m_vSlotTile.size(), nullptr);
  m_Total = (uint64_t)m_vAims.size() * m_Layout.m_vSlotTile.size();
  m_Done = 0;
  m_Cancel = false;
  m_Finished = false;
  m_Driver = std::thread([this, Threads] {
    Drive(std::max(1, Threads));
    m_Finished = true;
  });
  return true;
}

void Table::Drive(int Threads) {
  Fast F;
  F.Init(&m_Sim);
  m_HasReach = F.Reach(m_vAims, &m_aReach[0], &m_aReach[1], &m_aReach[2], &m_aReach[3]);
  m_vArenas.resize((size_t)Threads);
  const int Angles = (int)m_vAims.size();
  const uint32_t Slots = (uint32_t)m_Layout.m_vSlotTile.size();
  const uint32_t W = m_Layout.WidthUnits();
  // aims at a time, for what their templates take
  const int Chunk = 16;
  for (int First = 0; First < Angles && !m_Cancel; First += Chunk) {
    const int Count = std::min(Chunk, Angles - First);
    F.Build(m_vAims, First, Count, Threads);
    std::atomic<uint32_t> Next{0};
    std::vector<std::thread> vWorkers;
    for (int t = 0; t < Threads; t++)
      vWorkers.emplace_back([&, t] {
        uint32_t aLand[TILE_POSITIONS];
        std::vector<uint8_t> vBytes;
        for (;;) {
          const uint32_t Slot = Next.fetch_add(1);
          if (Slot >= Slots || m_Cancel.load(std::memory_order_relaxed)) break;
          const uint32_t Tile = m_Layout.m_vSlotTile[Slot];
          const int x0 = (int)(Tile % (uint32_t)m_Layout.m_W) * TILE_UNITS, y0 = (int)(Tile / (uint32_t)m_Layout.m_W) * TILE_UNITS;
          for (int a = First; a < First + Count; a++) {
            for (int u = 0; u < TILE_POSITIONS; u++) aLand[u] = F.Land(x0 + (u & 31), y0 + (u >> 5), a);
            vBytes.clear();
            EncodeLandings(aLand, W, vBytes);
            uint8_t *p = m_vArenas[(size_t)t].Alloc(vBytes.size());
            memcpy(p, vBytes.data(), vBytes.size());
            m_vStreams[(size_t)a * Slots + Slot] = p;
          }
          m_Done.fetch_add((uint64_t)Count);
        }
      });
    for (std::thread &T : vWorkers) T.join();
  }
}

void Table::Stop() {
  m_Cancel = true;
  if (m_Driver.joinable()) m_Driver.join();
}

bool Table::Poll() {
  if (!m_Driver.joinable()) return m_Complete;
  if (!m_Finished.load()) return false;
  m_Driver.join();
  m_Complete = !m_Cancel.load() && m_Done.load() == m_Total;
  if (!m_Complete) {
    m_vArenas.clear();
    m_vStreams.clear();
  }
  return m_Complete;
}

uint32_t Table::Land(uint64_t Pos, int Angle) const {
  uint32_t aLand[TILE_POSITIONS];
  Decode((uint32_t)(Pos / TILE_POSITIONS), Angle, aLand);
  return aLand[Pos % TILE_POSITIONS];
}

uint64_t Table::Bytes() const {
  uint64_t Bytes = m_vStreams.size() * sizeof(const uint8_t *);
  for (const Arena &A : m_vArenas) Bytes += A.m_Total;
  return Bytes;
}

// --- finding what lands in a rectangle -----------------------------------------

void Query::Start(const Table *pTable, Target Tgt, int Threads) {
  Wait();
  m_Done = false;
  m_Thread = std::thread([this, pTable, Tgt, Threads] {
    // the box around the circle
    const int Reach = (int)std::ceil(Tgt.m_Radius);
    const int X0 = Tgt.m_X - Reach, Y0 = Tgt.m_Y - Reach, X1 = Tgt.m_X + Reach, Y1 = Tgt.m_Y + Reach;
    const Layout &L = pTable->GetLayout();
    const int NumAims = pTable->NumAngles();
    const uint32_t Slots = (uint32_t)L.m_vSlotTile.size();
    const uint32_t W = L.WidthUnits();
    const uint64_t Lo = (uint64_t)std::max(Y0, 0) * W + (uint64_t)std::max(X0, 0);
    const uint64_t Hi = (uint64_t)std::max(Y1, 0) * W + (uint64_t)std::max(X1, 0);
    float aReach[4];
    const bool HasReach = pTable->Reach(aReach);

    const int N = std::max(1, Threads);
    struct Part {
      std::vector<uint64_t> vPos;
      std::vector<uint16_t> vCount;
    };
    std::vector<Part> vParts((size_t)N);
    std::vector<std::thread> vThreads;
    for (int t = 0; t < N; t++)
      vThreads.emplace_back([&, t] {
        const uint32_t Begin = (uint32_t)((uint64_t)Slots * t / N), End = (uint32_t)((uint64_t)Slots * (t + 1) / N);
        Part &P = vParts[(size_t)t];
        uint32_t aLand[TILE_POSITIONS];
        uint16_t aCount[TILE_POSITIONS];
        for (uint32_t Slot = Begin; Slot < End; Slot++) {
          const uint32_t Tile = L.m_vSlotTile[Slot];
          const float tx = (float)(Tile % (uint32_t)L.m_W) * TILE_UNITS, ty = (float)(Tile / (uint32_t)L.m_W) * TILE_UNITS;
          // too far to land there
          if (HasReach && (tx + 31 + aReach[2] < X0 || tx + aReach[0] > X1 || ty + 31 + aReach[3] < Y0 || ty + aReach[1] > Y1))
            continue;
          memset(aCount, 0, sizeof(aCount));
          bool Any = false;
          for (int a = 0; a < NumAims; a++) {
            pTable->Decode(Slot, a, aLand);
            for (int u = 0; u < TILE_POSITIONS; u++) {
              const uint32_t Land = aLand[u];
              if (Land < Lo || Land > Hi) continue;
              if (Tgt.Hits(L, Land)) {
                aCount[u]++;
                Any = true;
              }
            }
          }
          if (!Any) continue;
          for (int u = 0; u < TILE_POSITIONS; u++)
            if (aCount[u]) {
              P.vPos.push_back((uint64_t)Slot * TILE_POSITIONS + (uint64_t)u);
              P.vCount.push_back(aCount[u]);
            }
        }
      });
    for (std::thread &T : vThreads) T.join();

    QueryResult R;
    R.m_Target = Tgt;
    for (Part &P : vParts) {
      R.m_vPos.insert(R.m_vPos.end(), P.vPos.begin(), P.vPos.end());
      R.m_vCount.insert(R.m_vCount.end(), P.vCount.begin(), P.vCount.end());
    }
    R.m_vSlotBegin.assign((size_t)Slots + 1, 0);
    size_t i = 0;
    for (uint32_t s = 0; s <= Slots; s++) {
      while (i < R.m_vPos.size() && R.m_vPos[i] / TILE_POSITIONS < s) i++;
      R.m_vSlotBegin[s] = (uint32_t)i;
    }
    for (uint16_t c : R.m_vCount) {
      R.m_MaxCount = std::max<uint32_t>(R.m_MaxCount, c);
      R.m_Aims += c;
    }
    m_Result = std::move(R);
    m_Done = true;
  });
}

bool Query::Poll() {
  if (!m_Thread.joinable() || !m_Done.load()) return false;
  m_Thread.join();
  return true;
}

} // namespace gl
