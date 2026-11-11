#define CIMGUI_INCLUDED
#include "imgui.h"
#include "plugin_api.h"

#include "gl_table.hpp"

#include <ddnet/ddnet_game.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if DD_UNIQUE
#define GL_GAME_ID "unique"
#else
#define GL_GAME_ID "ddnet"
#endif
#define GL_NAME "Grenade Lineups"

namespace {

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// 0 when unknown
uint64_t AvailableMemory() {
#ifdef _WIN32
  MEMORYSTATUSEX Status;
  Status.dwLength = sizeof(Status);
  return GlobalMemoryStatusEx(&Status) ? (uint64_t)Status.ullAvailPhys : 0;
#else
  FILE *pFile = fopen("/proc/meminfo", "r");
  if (!pFile) return 0;
  char aLine[256];
  unsigned long long Kib = 0;
  while (fgets(aLine, sizeof(aLine), pFile))
    if (sscanf(aLine, "MemAvailable: %llu kB", &Kib) == 1) break;
  fclose(pFile);
  return (uint64_t)Kib * 1024;
#endif
}

std::string FormatBytes(uint64_t Bytes) {
  const char *apUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double Value = (double)Bytes;
  int Unit = 0;
  while (Value >= 1024.0 && Unit < 4) Value /= 1024.0, Unit++;
  char aBuf[64];
  snprintf(aBuf, sizeof(aBuf), Unit ? "%.2f %s" : "%.0f %s", Value, apUnits[Unit]);
  return aBuf;
}

std::string FormatDuration(double Seconds) {
  char aBuf[64];
  const long long s = (long long)Seconds;
  if (s >= 3600)
    snprintf(aBuf, sizeof(aBuf), "%lldh %02lldm", s / 3600, s / 60 % 60);
  else
    snprintf(aBuf, sizeof(aBuf), "%lldm %02llds", s / 60, s % 60);
  return aBuf;
}

// The draw calls take plane coordinates (tiles), and colors the renderer loads
// with aligned SIMD loads.
void DrawRect(const tas_api_t *pAPI, float x, float y, float w, float h, const float *pColor) {
  alignas(16) float c[4] = {pColor[0], pColor[1], pColor[2], pColor[3]};
  vec2 Pos = {x / 32.0f, y / 32.0f}, Size = {w / 32.0f, h / 32.0f};
  pAPI->draw_rect_filled_world(Pos, Size, 0.0f, c);
}

void DrawStroke(const tas_api_t *pAPI, std::vector<float> &vPlane, const float *pColor, float WidthPx) {
  if (vPlane.size() < 4) return;
  alignas(16) float c[4] = {pColor[0], pColor[1], pColor[2], pColor[3]};
  pAPI->draw_stroke_world((const vec2 *)vPlane.data(), (uint32_t)(vPlane.size() / 2), c, WidthPx);
}

// a path in units, split where it teleports (NAN)
void DrawPath(const tas_api_t *pAPI, const std::vector<ddnet_vec2_t> &vPath, const float *pColor, float WidthPx,
              std::vector<float> &vPlane) {
  vPlane.clear();
  for (const ddnet_vec2_t &P : vPath) {
    if (std::isnan(P.x)) {
      DrawStroke(pAPI, vPlane, pColor, WidthPx);
      vPlane.clear();
      continue;
    }
    vPlane.push_back(P.x / 32.0f);
    vPlane.push_back(P.y / 32.0f);
  }
  DrawStroke(pAPI, vPlane, pColor, WidthPx);
}

void AngleColor(float Degrees, float Alpha, float *pOut) {
  ImGui::ColorConvertHSVtoRGB(Degrees / 360.0f, 0.75f, 1.0f, pOut[0], pOut[1], pOut[2]);
  pOut[3] = Alpha;
}

// The physics world whose tuning the editor plays with.
const ddnet_world_t *EditorPhysics(const ft_world *pWorld) {
#if DD_UNIQUE
  if (!pWorld) return nullptr;
  for (int i = 0; i < pWorld->client_count; i++)
    if (pWorld->spaces[i]) return pWorld->spaces[i];
  return nullptr;
#else
  return pWorld ? &pWorld->core : nullptr;
#endif
}

struct CollisionDeleter {
  void operator()(ddnet_collision_t *p) const {
    ddnet_collision_free(p);
    free(p);
  }
};

// Something that runs on a thread of its own until it has a result.
class CJob {
public:
  virtual ~CJob() { Wait(); }
  bool Running() const { return m_Thread.joinable() && !m_Done.load(); }
  void Wait() {
    m_Cancel = true;
    if (m_Thread.joinable()) m_Thread.join();
  }
  // true once when it has finished
  bool Poll() {
    if (!m_Thread.joinable() || !m_Done.load()) return false;
    m_Thread.join();
    return true;
  }

protected:
  template <typename F> void Launch(F Fn) {
    Wait();
    m_Cancel = false;
    m_Done = false;
    m_Thread = std::thread([this, Fn] {
      Fn();
      m_Done = true;
    });
  }
  std::atomic<bool> m_Cancel{false};

private:
  std::thread m_Thread;
  std::atomic<bool> m_Done{false};
};

// The size of a table, from a table of a few of the map's tiles.
class CEstimateJob : public CJob {
public:
  ~CEstimateJob() override { Wait(); }
  void Start(const gl::Sim &Sim, const gl::Layout &Layout, int Angles, int Threads) {
    Launch([this, Sim, Layout, Angles, Threads] {
      m_Bytes = 0;
      const size_t Slots = Layout.m_vSlotTile.size(), N = std::min<size_t>(Slots, 48);
      if (!N) return;
      std::vector<uint32_t> vTiles;
      for (size_t i = 0; i < N; i++) vTiles.push_back(Layout.m_vSlotTile[i * Slots / N]);
      gl::Table Table;
      std::string Error;
      if (!Table.Start(Sim, Layout.Subset(vTiles), Angles, Threads, &Error)) return;
      while (!Table.Poll()) {
        if (m_Cancel) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      const uint64_t Index = (uint64_t)N * (uint64_t)Angles * sizeof(void *);
      const double PerTile = (double)(Table.Bytes() - Index) / (double)N;
      m_Bytes = (uint64_t)(PerTile * (double)Slots) + (uint64_t)Slots * (uint64_t)Angles * sizeof(void *);
    });
  }
  uint64_t m_Bytes = 0;
};

// Every shot that hits, ready to draw: the arc of its aim moved to its tee,
// up to where it explodes.
struct Shot {
  int32_t m_X, m_Y;
  uint16_t m_Aim, m_Ticks; // the arc's points before the explosion
  float m_LandX, m_LandY;
  int32_t m_Path; // -1, or its own path (teleported, tune zones)
};

struct ShotSet {
  std::vector<Shot> m_vShots;
  std::vector<std::vector<ddnet_vec2_t>> m_vPaths;
  std::vector<std::vector<ddnet_vec2_t>> m_vArcs; // per aim, relative to the tee: start, then every tick
  std::vector<float> m_vBox;                      // per aim: x0, y0, x1, y1 of the arc
};

class CShotJob : public CJob {
public:
  ~CShotJob() override { Wait(); }
  void Start(const gl::Table *pTable, const gl::QueryResult *pResult, int Threads) {
    Launch([this, pTable, pResult, Threads] { Run(*pTable, *pResult, std::max(1, Threads)); });
  }
  ShotSet Take() { return std::move(m_Out); }

private:
  void Run(const gl::Table &T, const gl::QueryResult &R, int Threads) {
    const gl::Layout &L = T.GetLayout();
    const gl::Sim &S = T.GetSim();
    const int NumAims = T.NumAngles();
    ShotSet Out;
    const ddnet_tuning_values_t &Tune = S.m_vTunes[0];
    const int Life = (int)(gl::SERVER_TICK_SPEED * Tune.grenade_lifetime);
    for (const gl::Aim &A : T.Aims()) {
      std::vector<ddnet_vec2_t> vArc;
      const ddnet_vec2_t Start = {A.m_Dir.x * 28.0f * 0.75f, A.m_Dir.y * 28.0f * 0.75f};
      vArc.push_back(Start);
      for (int k = 1; k <= Life + 1; k++)
        vArc.push_back(gl::Sim::CalcPos(Start, A.m_Dir, Tune.grenade_curvature, Tune.grenade_speed, k / (float)gl::SERVER_TICK_SPEED));
      float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      for (const ddnet_vec2_t &P : vArc) x0 = std::min(x0, P.x), y0 = std::min(y0, P.y), x1 = std::max(x1, P.x), y1 = std::max(y1, P.y);
      Out.m_vBox.insert(Out.m_vBox.end(), {x0, y0, x1, y1});
      Out.m_vArcs.push_back(std::move(vArc));
    }

    struct Part {
      std::vector<Shot> vShots;
      std::vector<std::vector<ddnet_vec2_t>> vPaths;
    };
    std::vector<Part> vParts((size_t)Threads);
    std::vector<std::thread> vThreads;
    const size_t Count = R.m_vPos.size();
    for (int t = 0; t < Threads; t++)
      vThreads.emplace_back([&, t] {
        Part &P = vParts[(size_t)t];
        std::vector<uint32_t> vLand((size_t)NumAims * gl::TILE_POSITIONS);
        std::vector<ddnet_vec2_t> vPath;
        int64_t Decoded = -1;
        for (size_t i = Count * t / Threads; i < Count * (t + 1) / Threads && !m_Cancel; i++) {
          const uint64_t Pos = R.m_vPos[i];
          const uint32_t Slot = (uint32_t)(Pos / gl::TILE_POSITIONS);
          if (Decoded != Slot) {
            for (int a = 0; a < NumAims; a++) T.Decode(Slot, a, &vLand[(size_t)a * gl::TILE_POSITIONS]);
            Decoded = Slot;
          }
          int x, y;
          L.PositionXY(Pos, &x, &y);
          for (int a = 0; a < NumAims; a++) {
            if (!R.m_Target.Hits(L, vLand[(size_t)a * gl::TILE_POSITIONS + Pos % gl::TILE_POSITIONS])) continue;
            vPath.clear();
            ddnet_vec2_t Land;
            const ddnet_vec2_t Tee = {(float)x, (float)y};
            S.Fly(Tee, T.Aims()[(size_t)a].m_Dir, &Land, &vPath);
            Shot Sh = {x, y, (uint16_t)a, 0, Land.x, Land.y, -1};
            const size_t Ticks = vPath.size() - 2;
            bool Own = Ticks + 1 > Out.m_vArcs[(size_t)a].size() || S.TuneZone(vPath[0].x, vPath[0].y) != 0;
            for (const ddnet_vec2_t &Q : vPath) Own = Own || std::isnan(Q.x);
            if (Own) {
              vPath.insert(vPath.begin(), Tee);
              Sh.m_Path = (int32_t)P.vPaths.size();
              P.vPaths.push_back(vPath);
            } else {
              Sh.m_Ticks = (uint16_t)Ticks;
            }
            P.vShots.push_back(Sh);
          }
        }
      });
    for (std::thread &Th : vThreads) Th.join();
    for (Part &P : vParts) {
      const int32_t Base = (int32_t)Out.m_vPaths.size();
      for (Shot &Sh : P.vShots) {
        if (Sh.m_Path >= 0) Sh.m_Path += Base;
        Out.m_vShots.push_back(Sh);
      }
      for (auto &vP : P.vPaths) Out.m_vPaths.push_back(std::move(vP));
    }
    m_Out = std::move(Out);
  }
  ShotSet m_Out;
};

class CGrenadeLineups {
public:
  CGrenadeLineups(tas_context_t *pContext, const tas_api_t *pAPI) : m_pContext(pContext), m_pAPI(pAPI) {
    m_Threads = (int)std::max(1u, std::thread::hardware_concurrency());
  }

  ~CGrenadeLineups() { DropResult(); }

  void ToggleWindow() { m_ShowWindow = !m_ShowWindow; }

  void Update() {
    if (m_pContext->is_headless) return;
    ImGui::SetCurrentContext(m_pContext->imgui_context);
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu(GL_NAME)) {
        ImGui::MenuItem("Show Window", nullptr, &m_ShowWindow);
        ImGui::MenuItem("Show Overlay", nullptr, &m_ShowOverlay);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    FollowMap();
    UpdateEstimate();
    if (m_pTable && m_pTable->Running() && m_pTable->Poll()) {
      m_ComputeSeconds = Now() - m_ComputeStart;
      if (m_HaveTarget) StartQuery();
    }
    if (m_Query.Poll()) {
      m_Result = m_Query.Take();
      m_HaveResult = true;
      m_HoverPos = -1;
      m_Shots = ShotSet();
      m_ShotsGen++;
      m_vStrokes.clear();
      m_ShotJob.Start(m_pTable.get(), &m_Result, m_Threads);
    }
    if (m_ShotJob.Poll()) {
      m_Shots = m_ShotJob.Take();
      m_ShotsGen++;
    }

    UpdatePicking();
    if (m_ShowWindow && m_pContext->ui_visible) DrawWindow();
    if (m_ShowOverlay) DrawOverlay();
  }

private:
  bool TableReady() const { return m_pTable && m_pTable->Complete(); }

  // Everything that reads the table or the result, stopped and gone.
  void DropResult() {
    m_ShotJob.Wait();
    m_Query.Wait();
    m_Shots = ShotSet();
    m_ShotsGen++;
    m_vStrokes.clear();
    m_Result = gl::QueryResult();
    m_HaveResult = false;
    m_HoverPos = -1;
  }

  // A map of our own, so the threads never read what the editor may free.
  void FollowMap() {
    const ft_world *pWorld = m_pAPI->get_initial_world();
    const ddnet_collision_t *pEditorCol = pWorld ? dd_world_collision(pWorld) : nullptr;
    if (pEditorCol == m_pEditorCol) return;
    m_pEditorCol = pEditorCol;

    DropResult();
    m_Estimate.Wait();
    m_EstimateBytes = 0;
    m_EstimatedAngles = -1;
    m_pTable.reset();
    m_pCol.reset();
    m_Layout = gl::Layout();
    m_MapError.clear();
    m_MapName.clear();
    if (!pEditorCol) return;

    const char *pPath = m_pAPI->get_level_path();
    if (!pPath || !*pPath) {
      m_MapError = "The level has no map file.";
      return;
    }
    map_data_t Map = load_map(pPath);
    ddnet_collision_t *pCol = (ddnet_collision_t *)calloc(1, sizeof(ddnet_collision_t));
    const bool Ok = pCol && ddnet_collision_init(pCol, &Map);
    free_map_data(&Map);
    if (!Ok) {
      free(pCol);
      m_MapError = "Could not load the map.";
      return;
    }
    m_pCol.reset(pCol);
    m_Layout.Build(pCol);
    const char *pName = m_pAPI->get_level_name();
    m_MapName = pName ? pName : "";
  }

  bool MakeSim(gl::Sim *pSim) {
    const ddnet_world_t *pEditor = EditorPhysics(m_pAPI->get_initial_world());
    if (pEditor && pSim->Init(pEditor, m_pCol.get())) return true;
    // (Unique Race without players has no world to ask)
#if DD_UNIQUE
    const ddnet_config_t Config = ddnet_config_default(DDNET_MODE_UNIQUE_RACE);
#else
    const ddnet_config_t Config = ddnet_config_default(DDNET_MODE_DDRACE);
#endif
    ddnet_world_t *pWorld = (ddnet_world_t *)calloc(1, sizeof(ddnet_world_t));
    bool Ok = pWorld && ddnet_world_init(pWorld, m_pCol.get(), &Config);
    if (Ok) {
      Ok = pSim->Init(pWorld, m_pCol.get());
      ddnet_world_free(pWorld);
    }
    free(pWorld);
    return Ok;
  }

  // The compressed size, measured on a few tiles once the angles hold still.
  void UpdateEstimate() {
    if (m_Estimate.Poll()) {
      m_EstimateBytes = m_Estimate.m_Bytes;
      m_EstimatedAngles = m_EstimatingAngles;
    }
    if (!m_pCol || m_Estimate.Running() || (m_EstimatingAngles == m_Angles && m_EstimatedAngles == m_Angles)) return;
    if (m_pTable && m_pTable->Running()) return;
    if (m_EstimatingAngles != m_Angles) {
      m_EstimatingAngles = m_Angles;
      m_AnglesChanged = Now();
      return;
    }
    if (Now() - m_AnglesChanged < 0.4) return;
    gl::Sim Sim;
    if (!MakeSim(&Sim)) return;
    m_Estimate.Start(Sim, m_Layout, m_Angles, m_Threads);
  }

  void StartCompute() {
    DropResult();
    m_Estimate.Wait();
    m_TableError.clear();
    gl::Sim Sim;
    if (!MakeSim(&Sim)) {
      m_TableError = "No tuning to fly grenades with.";
      return;
    }
    m_pTable = std::make_unique<gl::Table>();
    if (!m_pTable->Start(Sim, m_Layout, m_Angles, m_Threads, &m_TableError)) {
      m_pTable.reset();
      return;
    }
    m_ComputeStart = Now();
  }

  void StartQuery() {
    if (!TableReady()) return;
    DropResult();
    gl::Target T;
    T.m_X = m_Target[0], T.m_Y = m_Target[1];
    T.m_Radius = std::max(m_Radius, 0.5f);
    m_Query.Start(m_pTable.get(), T, m_Threads);
  }

  void MouseUnit(int *pX, int *pY) const {
    const ImVec2 Mouse = ImGui::GetIO().MousePos;
    float wx = 0.0f, wy = 0.0f;
    m_pAPI->screen_to_world(Mouse.x, Mouse.y, &wx, &wy);
    *pX = (int)std::floor(wx * 32.0f);
    *pY = (int)std::floor(wy * 32.0f);
  }

  void UpdatePicking() {
    if (!m_Picking) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      m_Picking = false;
      return;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_pAPI->viewport_accepts_input(false)) {
      MouseUnit(&m_Target[0], &m_Target[1]);
      m_Picking = false;
      m_HaveTarget = true;
      StartQuery();
    }
  }

  void DrawWindow() {
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(GL_NAME, &m_ShowWindow)) {
      ImGui::End();
      return;
    }
    if (!m_pCol) {
      ImGui::TextDisabled("%s", m_MapError.empty() ? "No map." : m_MapError.c_str());
      ImGui::End();
      return;
    }

    ImGui::Text("%s: %d x %d tiles, %zu free", m_MapName.c_str(), m_Layout.m_W, m_Layout.m_H, m_Layout.m_vSlotTile.size());
    ImGui::Text("Positions: %llu", (unsigned long long)m_Layout.Positions());

    const bool Computing = m_pTable && m_pTable->Running();
    ImGui::BeginDisabled(Computing);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Angles", &m_Angles, 8, 64)) m_Angles = std::clamp(m_Angles, 1, 65535);
    ImGui::SetNextItemWidth(120);
    const int MaxThreads = (int)std::max(1u, std::thread::hardware_concurrency());
    ImGui::SliderInt("Threads", &m_Threads, 1, MaxThreads);
    ImGui::EndDisabled();

    const uint64_t Available = AvailableMemory();
    if (m_EstimatedAngles == m_Angles && m_EstimateBytes) {
      const bool TooBig = Available && m_EstimateBytes > Available;
      if (TooBig) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
      ImGui::Text("Table: about %s", FormatBytes(m_EstimateBytes).c_str());
      if (TooBig) ImGui::PopStyleColor();
    } else {
      ImGui::TextDisabled("Table: measuring...");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s uncompressed%s%s)", FormatBytes(m_Layout.RawBytes(m_Angles)).c_str(), Available ? ", " : "",
                        Available ? (FormatBytes(Available) + " available").c_str() : "");
    if (!m_Layout.Addressable()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "The map is too large for this table.");

    if (Computing) {
      const double p = m_pTable->Progress(), Elapsed = Now() - m_ComputeStart;
      char aOverlay[96];
      snprintf(aOverlay, sizeof(aOverlay), "%.1f%%  %s left", p * 100.0,
               p > 0.0 ? FormatDuration(Elapsed * (1.0 - p) / p).c_str() : "?");
      ImGui::ProgressBar((float)p, ImVec2(-FLT_MIN, 0), aOverlay);
      if (ImGui::Button("Cancel")) m_pTable.reset();
    } else {
      if (ImGui::Button(TableReady() ? "Recompute" : "Compute")) StartCompute();
      if (TableReady()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d angles, %s, took %s", m_pTable->NumAngles(), FormatBytes(m_pTable->Bytes()).c_str(),
                            FormatDuration(m_ComputeSeconds).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Free")) {
          DropResult();
          m_pTable.reset();
        }
      }
    }
    if (!m_TableError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", m_TableError.c_str());

    ImGui::SeparatorText("Target");
    ImGui::SetNextItemWidth(200);
    ImGui::InputInt2("Position", m_Target);
    ImGui::SetNextItemWidth(200);
    ImGui::InputFloat("Radius", &m_Radius, 8.0f, 32.0f, "%.0f");
    if (ImGui::Button(m_Picking ? "Picking... (Esc)" : "Pick in Viewport")) m_Picking = !m_Picking;
    ImGui::SameLine();
    ImGui::BeginDisabled(!TableReady());
    if (ImGui::Button("Find")) {
      m_HaveTarget = true;
      StartQuery();
    }
    ImGui::EndDisabled();

    if (m_Query.Running())
      ImGui::TextDisabled("Searching...");
    else if (m_HaveResult) {
      ImGui::Text("%zu positions, %llu shots", m_Result.m_vPos.size(), (unsigned long long)m_Result.m_Aims);
      if (m_ShotJob.Running())
        ImGui::TextDisabled("Tracing shots...");
      else if (m_StrokeCellPx > 1)
        ImGui::TextDisabled("%zu lines: one per angle and %d px", m_vStrokes.size(), m_StrokeCellPx);
      else
        ImGui::TextDisabled("%zu lines: one per angle and pixel", m_vStrokes.size());
    }
    ImGui::Checkbox("Lines", &m_ShowLines);
    ImGui::SameLine();
    ImGui::Checkbox("Positions", &m_ShowPositions);
    ImGui::End();
  }

  // the units the viewport shows, and how many there are to a pixel
  void View(float *pX0, float *pY0, float *pX1, float *pY1, float *pUnitsPerPixel) const {
    float sx, sy, sw, sh;
    m_pAPI->get_viewport_rect(&sx, &sy, &sw, &sh);
    float ax, ay, bx, by;
    m_pAPI->screen_to_world(sx, sy, &ax, &ay);
    m_pAPI->screen_to_world(sx + sw, sy + sh, &bx, &by);
    *pX0 = std::min(ax, bx) * 32.0f, *pY0 = std::min(ay, by) * 32.0f;
    *pX1 = std::max(ax, bx) * 32.0f, *pY1 = std::max(ay, by) * 32.0f;
    *pUnitsPerPixel = sw > 0 ? (*pX1 - *pX0) / sw : 1.0f;
  }

  void DrawCircle(float cx, float cy, float r, const float *pColor, float WidthPx) {
    m_vPlane.clear();
    for (int i = 0; i <= 96; i++) {
      const float a = (float)i / 96.0f * 2.0f * (float)M_PI;
      m_vPlane.push_back((cx + std::cos(a) * r) / 32.0f);
      m_vPlane.push_back((cy + std::sin(a) * r) / 32.0f);
    }
    DrawStroke(m_pAPI, m_vPlane, pColor, WidthPx);
  }

  void DrawOverlay() {
    static const float s_aTarget[4] = {1.0f, 0.25f, 0.2f, 1.0f};
    if (m_Picking && m_pAPI->viewport_accepts_input(false)) {
      int x, y;
      MouseUnit(&x, &y);
      DrawCircle(x + 0.5f, y + 0.5f, std::max(m_Radius, 0.5f), s_aTarget, 2.0f);
    }
    if (!m_HaveResult || !TableReady()) return;
    const gl::QueryResult &R = m_Result;
    float vx0, vy0, vx1, vy1, UnitsPerPixel;
    View(&vx0, &vy0, &vx1, &vy1, &UnitsPerPixel);

    if (m_ShowLines) {
      PickStrokes(vx0, vy0, vx1, vy1, UnitsPerPixel);
      DrawShots();
    }
    if (m_ShowPositions) DrawPositions(vx0, vy0, vx1, vy1, UnitsPerPixel);

    const gl::Target &T = R.m_Target;
    DrawCircle(T.m_X + 0.5f, T.m_Y + 0.5f, T.m_Radius, s_aTarget, 2.0f);
    DrawRect(m_pAPI, (float)T.m_X, (float)T.m_Y, 1.0f, 1.0f, s_aTarget);
    DrawHover();
  }

  // The shots that look different at this zoom: of shots with the same aim
  // whose tees share a screen cell, one. A pixel at first, larger only past
  // what the renderer can take in a frame. Again when the view holds still.
  void PickStrokes(float vx0, float vy0, float vx1, float vy1, float UnitsPerPixel) {
    const float aView[5] = {vx0, vy0, vx1, vy1, UnitsPerPixel};
    if (memcmp(aView, m_aPickView, sizeof(aView)) != 0) {
      memcpy(m_aPickView, aView, sizeof(aView));
      m_ViewMoved = Now();
    }
    if (m_PickedGen == m_ShotsGen && (Now() - m_ViewMoved < 0.12 || memcmp(aView, m_aPickedView, sizeof(aView)) == 0)) return;
    memcpy(m_aPickedView, aView, sizeof(aView));
    m_PickedGen = m_ShotsGen;

    const ShotSet &S = m_Shots;
    std::unordered_set<uint64_t> Seen;
    for (int CellPx = 1;; CellPx *= 2) {
      const float Cell = UnitsPerPixel * (float)CellPx;
      Seen.clear();
      m_vStrokes.clear();
      size_t Points = 0;
      bool Fits = true;
      for (uint32_t i = 0; i < S.m_vShots.size() && Fits; i++) {
        const Shot &Sh = S.m_vShots[i];
        if (Sh.m_Path >= 0) {
          m_vStrokes.push_back(i);
          Points += S.m_vPaths[(size_t)Sh.m_Path].size();
        } else {
          const float *pBox = &S.m_vBox[(size_t)Sh.m_Aim * 4];
          if (Sh.m_X + pBox[2] < vx0 || Sh.m_X + pBox[0] > vx1 || Sh.m_Y + pBox[3] < vy0 || Sh.m_Y + pBox[1] > vy1) continue;
          const uint64_t Key = (uint64_t)Sh.m_Aim << 42 | ((uint64_t)(int64_t)std::floor(Sh.m_X / Cell) & 0x1FFFFF) << 21 |
                               ((uint64_t)(int64_t)std::floor(Sh.m_Y / Cell) & 0x1FFFFF);
          if (!Seen.insert(Key).second) continue;
          m_vStrokes.push_back(i);
          Points += (size_t)Sh.m_Ticks + 3;
        }
        Fits = Points <= MAX_STROKE_POINTS;
      }
      if (Fits || Cell > 1e6f) {
        m_StrokeCellPx = CellPx;
        return;
      }
    }
  }

  void DrawShots() {
    const ShotSet &S = m_Shots;
    for (uint32_t i : m_vStrokes) {
      const Shot &Sh = S.m_vShots[i];
      float c[4];
      AngleColor(m_pTable->Aims()[Sh.m_Aim].m_Degrees, 0.45f, c);
      if (Sh.m_Path >= 0) {
        DrawPath(m_pAPI, S.m_vPaths[(size_t)Sh.m_Path], c, 1.5f, m_vPlane);
        continue;
      }
      const std::vector<ddnet_vec2_t> &vArc = S.m_vArcs[Sh.m_Aim];
      m_vPlane.clear();
      m_vPlane.push_back(Sh.m_X / 32.0f);
      m_vPlane.push_back(Sh.m_Y / 32.0f);
      for (int k = 0; k <= Sh.m_Ticks; k++) {
        m_vPlane.push_back((Sh.m_X + vArc[(size_t)k].x) / 32.0f);
        m_vPlane.push_back((Sh.m_Y + vArc[(size_t)k].y) / 32.0f);
      }
      m_vPlane.push_back(Sh.m_LandX / 32.0f);
      m_vPlane.push_back(Sh.m_LandY / 32.0f);
      DrawStroke(m_pAPI, m_vPlane, c, 1.5f);
    }
  }

  // Each position as a unit, or zoomed out, cells of a few pixels with the best of their positions.
  void DrawPositions(float vx0, float vy0, float vx1, float vy1, float UnitsPerPixel) {
    const gl::QueryResult &R = m_Result;
    const int Cell = std::max(1, (int)std::ceil(3.0f * UnitsPerPixel));
    const int gx0 = (int)std::floor(vx0 / Cell), gy0 = (int)std::floor(vy0 / Cell);
    const int gw = (int)std::floor(vx1 / Cell) - gx0 + 1, gh = (int)std::floor(vy1 / Cell) - gy0 + 1;
    if (gw <= 0 || gh <= 0 || (int64_t)gw * gh > 4000000) return;
    m_vGrid.assign((size_t)gw * gh, 0);
    const int tx0 = std::max(0, (int)std::floor(vx0 / 32)), ty0 = std::max(0, (int)std::floor(vy0 / 32));
    const int tx1 = std::min(m_Layout.m_W - 1, (int)std::floor(vx1 / 32));
    const int ty1 = std::min(m_Layout.m_H - 1, (int)std::floor(vy1 / 32));
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++) {
        const int32_t Slot = m_Layout.m_vTileSlot[(size_t)ty * m_Layout.m_W + tx];
        if (Slot < 0) continue;
        for (uint32_t i = R.m_vSlotBegin[Slot]; i < R.m_vSlotBegin[Slot + 1]; i++) {
          int x, y;
          m_Layout.PositionXY(R.m_vPos[i], &x, &y);
          const int gx = (int)std::floor((float)x / Cell) - gx0, gy = (int)std::floor((float)y / Cell) - gy0;
          if (gx < 0 || gy < 0 || gx >= gw || gy >= gh) continue;
          uint16_t &G = m_vGrid[(size_t)gy * gw + gx];
          G = std::max(G, R.m_vCount[i]);
        }
      }
    for (int gy = 0; gy < gh; gy++)
      for (int gx = 0; gx < gw; gx++) {
        const uint16_t Count = m_vGrid[(size_t)gy * gw + gx];
        if (!Count) continue;
        const float t = R.m_MaxCount > 1 ? (float)(Count - 1) / (float)(R.m_MaxCount - 1) : 1.0f;
        const float c[4] = {0.15f + 0.85f * t, 0.75f + 0.2f * t, 1.0f - 0.8f * t, 0.85f};
        DrawRect(m_pAPI, (float)((gx0 + gx) * Cell), (float)((gy0 + gy) * Cell), (float)Cell, (float)Cell, c);
      }
  }

  void DrawHover() {
    if (!m_pAPI->viewport_accepts_input(false) || m_Picking) return;
    int x, y;
    MouseUnit(&x, &y);
    const int64_t Pos = m_Layout.PositionAt(x, y);
    const gl::QueryResult &R = m_Result;
    if (Pos < 0 || !std::binary_search(R.m_vPos.begin(), R.m_vPos.end(), (uint64_t)Pos)) {
      m_HoverPos = -1;
      return;
    }
    if (Pos != m_HoverPos) {
      m_HoverPos = Pos;
      m_vHoverAims.clear();
      m_vHoverPaths.clear();
      for (int a = 0; a < m_pTable->NumAngles(); a++) {
        if (!R.m_Target.Hits(m_Layout, m_pTable->Land((uint64_t)Pos, a))) continue;
        m_vHoverAims.push_back(a);
        std::vector<ddnet_vec2_t> vPath = {{(float)x, (float)y}};
        ddnet_vec2_t Land;
        m_pTable->GetSim().Fly({(float)x, (float)y}, m_pTable->Aims()[(size_t)a].m_Dir, &Land, &vPath);
        m_vHoverPaths.push_back(std::move(vPath));
      }
    }

    static const float s_aPath[4] = {1.0f, 1.0f, 1.0f, 0.95f};
    for (const std::vector<ddnet_vec2_t> &vPath : m_vHoverPaths) DrawPath(m_pAPI, vPath, s_aPath, 3.0f, m_vPlane);

    ImGui::BeginTooltip();
    ImGui::Text("%d, %d (tile %.2f, %.2f)", x, y, x / 32.0f, y / 32.0f);
    ImGui::Separator();
    const size_t Shown = std::min<size_t>(m_vHoverAims.size(), 16);
    for (size_t i = 0; i < Shown; i++) {
      const gl::Aim &A = m_pTable->Aims()[(size_t)m_vHoverAims[i]];
      ImGui::Text("%7.2f°   aim %d, %d", A.m_Degrees, A.m_TargetX, A.m_TargetY);
    }
    if (m_vHoverAims.size() > Shown) ImGui::TextDisabled("and %zu more", m_vHoverAims.size() - Shown);
    ImGui::EndTooltip();
  }

  tas_context_t *m_pContext;
  const tas_api_t *m_pAPI;
  bool m_ShowWindow = true, m_ShowOverlay = true, m_ShowLines = true, m_ShowPositions = true;
  int m_Angles = 64, m_Threads = 1;

  const ddnet_collision_t *m_pEditorCol = nullptr;
  std::unique_ptr<ddnet_collision_t, CollisionDeleter> m_pCol;
  gl::Layout m_Layout;
  std::string m_MapName, m_MapError;

  CEstimateJob m_Estimate;
  uint64_t m_EstimateBytes = 0;
  int m_EstimatedAngles = -1, m_EstimatingAngles = -1;
  double m_AnglesChanged = 0.0;

  std::unique_ptr<gl::Table> m_pTable;
  std::string m_TableError;
  double m_ComputeStart = 0.0, m_ComputeSeconds = 0.0;

  int m_Target[2] = {0, 0};
  float m_Radius = gl::EXPLOSION_RADIUS;
  bool m_HaveTarget = false, m_Picking = false;
  gl::Query m_Query;
  gl::QueryResult m_Result;
  bool m_HaveResult = false;
  CShotJob m_ShotJob;
  ShotSet m_Shots;
  // about 24 MiB of the renderer's 128 MiB a frame
  enum { MAX_STROKE_POINTS = 1000000 };
  int m_ShotsGen = 0, m_PickedGen = -1, m_StrokeCellPx = 1;
  float m_aPickView[5] = {}, m_aPickedView[5] = {};
  double m_ViewMoved = 0.0;
  std::vector<uint32_t> m_vStrokes;

  std::vector<float> m_vPlane;
  std::vector<uint16_t> m_vGrid;
  int64_t m_HoverPos = -1;
  std::vector<int> m_vHoverAims;
  std::vector<std::vector<ddnet_vec2_t>> m_vHoverPaths;
};

} // namespace

extern "C" {

FT_PLUGIN_ABI_EXPORT()

FT_API const char *plugin_game_id() { return GL_GAME_ID; }

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) { return new CGrenadeLineups(context, api); }

FT_API void plugin_update(void *plugin_data) { static_cast<CGrenadeLineups *>(plugin_data)->Update(); }

FT_API void plugin_shutdown(void *plugin_data) { delete static_cast<CGrenadeLineups *>(plugin_data); }

FT_API void plugin_show_ui(void *plugin_data) { static_cast<CGrenadeLineups *>(plugin_data)->ToggleWindow(); }
}
