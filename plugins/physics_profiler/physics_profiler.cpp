#include "tracy/Tracy.hpp"

#include <atomic>
#include <chrono>
#include <omp.h>
#include <thread>

#define CIMGUI_INCLUDED
#include "imgui.h"
#include "plugin_api.h"

extern "C" {
#include <gamecore.h>
}

static inline unsigned int fast_rand_u32(unsigned int *state) {
  unsigned int x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static inline int fast_rand_range(unsigned int *state, int min, int max) {
  return min + (fast_rand_u32(state) % (max - min + 1));
}

static inline void generate_random_input(SPlayerInput *pInput, unsigned int *seed) {
  pInput->m_Direction = fast_rand_range(seed, -1, 1);
  pInput->m_Jump = fast_rand_range(seed, 0, 1);
  pInput->m_Fire = fast_rand_range(seed, 0, 1);
  pInput->m_Hook = fast_rand_range(seed, 0, 1);
  pInput->m_TargetX = fast_rand_range(seed, -1000, 1000);
  pInput->m_TargetY = fast_rand_range(seed, -1000, 1000);
  pInput->m_WantedWeapon = fast_rand_range(seed, 0, NUM_WEAPONS - 1);
}

class PhysicsProfilerPlugin {
private:
  const tas_api_t *m_pAPI;
  const tas_context_t *m_pContext;

  // UI State
  bool m_ShowWindow;
  int m_Iterations;
  int m_TicksPerIteration;
  bool m_UseMultiThreading;

  // benchmark state
  std::atomic<bool> m_IsRunning;
  std::atomic<int> m_Progress;
  std::thread m_BenchmarkThread;
  double m_LastElapsedTime;

public:
  PhysicsProfilerPlugin(tas_context_t *pContext, const tas_api_t *pAPI)
      : m_pAPI(pAPI), m_pContext(pContext), m_ShowWindow(true), m_Iterations(200), m_TicksPerIteration(500),
        m_UseMultiThreading(true), m_IsRunning(false), m_Progress(0), m_LastElapsedTime(0.0) {

    m_pAPI->log_info("Physics Profiler", "Plugin initialized.");
  }

  ~PhysicsProfilerPlugin() {
    if (m_BenchmarkThread.joinable()) {
      m_BenchmarkThread.join();
    }
    m_pAPI->log_info("Physics Profiler", "Plugin shutting down.");
  }

  void Benchmark() {
    if (!m_pAPI->get_initial_world())
      return;
    ZoneScopedN("Benchmark Execution"); // Tracy Zone for the whole benchmark

    m_IsRunning = true;
    m_Progress = 0;
    m_LastElapsedTime = 0.0;
    auto startTime = std::chrono::high_resolution_clock::now();

    SWorldCore StartWorld = wc_empty();
    wc_copy_world(&StartWorld, m_pAPI->get_initial_world());

    unsigned int global_seed = 0; // (unsigned)time(NULL);
    if (m_UseMultiThreading) {
#pragma omp parallel for
      for (int i = 0; i < m_Iterations; ++i) {
        FrameMarkNamed("Worker Thread Frame");
        ZoneScopedN("Single Iteration (Parallel)");

        unsigned int run_seed = global_seed ^ (i * 0x9E3779B9u);
        SWorldCore World = {};
        wc_copy_world(&World, &StartWorld);
        for (int t = 0; t < m_TicksPerIteration; ++t) {
          ZoneScopedN("Physics Tick");
          unsigned int local_seed = run_seed ^ i;
          for (int c = 0; c < World.m_NumCharacters; c++) {
            SPlayerInput Input = {};
            generate_random_input(&Input, &local_seed);
            cc_on_input(&World.m_pCharacters[c], &Input);
          }
          wc_tick(&World);
        }
        wc_free(&World);
        m_Progress++;
      }
    } else {
      for (int i = 0; i < m_Iterations; ++i) {
        FrameMarkNamed("Worker Thread Frame");
        ZoneScopedN("Single Iteration (Serial)");

        unsigned int run_seed = global_seed ^ (i * 0x9E3779B9u);
        SWorldCore World = {};
        wc_copy_world(&World, &StartWorld);
        for (int t = 0; t < m_TicksPerIteration; ++t) {
          ZoneScopedN("Physics Tick");
          unsigned int local_seed = run_seed ^ i;
          for (int c = 0; c < World.m_NumCharacters; c++) {
            SPlayerInput Input = {};
            generate_random_input(&Input, &local_seed);
            cc_on_input(&World.m_pCharacters[c], &Input);
          }
          wc_tick(&World);
        }
        wc_free(&World);
        m_Progress++;
      }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    m_LastElapsedTime = std::chrono::duration<double>(endTime - startTime).count();

    wc_free(&StartWorld);
    m_IsRunning = false;
  }

  void StartBenchmarkThread() {
    if (m_IsRunning)
      return;

    if (m_BenchmarkThread.joinable()) {
      m_BenchmarkThread.join();
    }
    m_BenchmarkThread = std::thread(&PhysicsProfilerPlugin::Benchmark, this);
  }

  void Update() {
    ImGui::SetCurrentContext(m_pContext->imgui_context);
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("Physics Profiler")) {
        ImGui::MenuItem("Show Window", nullptr, &m_ShowWindow);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    if (m_ShowWindow) {
      if (ImGui::Begin("Physics Profiler", &m_ShowWindow)) {
        ImGui::Text("Benchmark controls for the ddnet_physics library.");
        ImGui::Text("Uses the current initial world to benchmark, add as many players as you want");
        ImGui::Separator();

        ImGui::InputInt("Iterations", &m_Iterations);
        ImGui::InputInt("Ticks per Iteration", &m_TicksPerIteration);
        ImGui::Checkbox("Use Multi-threading (OpenMP)", &m_UseMultiThreading);

        ImGui::Separator();

        if (m_IsRunning) {
          ImGui::Text("Benchmark in progress...");
          ImGui::ProgressBar((float)m_Progress / m_Iterations);
        } else {
          if (ImGui::Button("Start Benchmark")) {
            StartBenchmarkThread();
          }
          if (m_LastElapsedTime > 0.0) {
            ImGui::Text("Last run took: %.4f seconds", m_LastElapsedTime);
            long long TotalTicks = (long long)m_Iterations * m_TicksPerIteration;
            double TicksPerSecond = TotalTicks / m_LastElapsedTime;
            ImGui::Separator();
            ImGui::Text("Raw Performance Metrics:");
            ImGui::Text("  Total Ticks: %lld", TotalTicks);
            ImGui::Text("  Ticks/Second: %f M", TicksPerSecond / 1e6);
            ImGui::Separator();
            ImGui::Text("In-Game Time Simulated Per Real-World Second:");
            const double TICKS_PER_INGAME_SECOND = 50.0;
            double InGameSeconds = TicksPerSecond / TICKS_PER_INGAME_SECOND;
            double InGameMinutes = InGameSeconds / 60.0;
            double InGameHours = InGameMinutes / 60.0;
            double InGameDays = InGameHours / 24.0;
            ImGui::Text("  %.2f in-game days", InGameDays);
            ImGui::Text("  %.2f in-game hours", InGameHours);
            ImGui::Text("  %.2f in-game minutes", InGameMinutes);
          }
        }
      }
      ImGui::End();
    }
  }
};

extern "C" {

FT_API plugin_info_t get_plugin_info() {
  return {"Physics Profiler", "Teero", "1.0.0", "Integrates Tracy to benchmark the ddnet_physics library."};
}

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  return new PhysicsProfilerPlugin(context, api);
}

FT_API void plugin_update(void *plugin_data) { static_cast<PhysicsProfilerPlugin *>(plugin_data)->Update(); }

FT_API void plugin_shutdown(void *plugin_data) { delete static_cast<PhysicsProfilerPlugin *>(plugin_data); }
}
