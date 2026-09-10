#include "tracy/Tracy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#if defined(_OPENMP)
#include <omp.h>
#endif
#include <thread>
#include <vector>

#define CIMGUI_INCLUDED
#include "imgui.h"
#include "plugin_api.h"
#include "profiler_common.h"
#include <logger/logger.h>

void benchmark_generic(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress) {
  if (!api || !api->get_initial_world()) return;

  const uint32_t record_size = api->input_record_size ? api->input_record_size() : 0;
  const int player_count = api->world_player_count ? api->world_player_count(api->get_initial_world()) : 1;
  if (record_size == 0 || player_count <= 0) return;

  std::vector<uint8_t> input_buffer(record_size * player_count, 0);
  for (int p = 0; p < player_count; ++p) {
    if (api->input_default) api->input_default(input_buffer.data() + p * record_size);
  }

  ft_world *StartWorld = api->clone_world(api->get_initial_world());
  if (!StartWorld) return;

  if (use_multithreading) {
#pragma omp parallel for
    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Parallel)");

      ft_world *thread_world = api->clone_world(StartWorld);
      if (!thread_world) {
        progress++;
        continue;
      }

      for (int t = 0; t < ticks_per_iteration; ++t) {
        ZoneScopedN("Physics Tick");
        api->step_world(thread_world, input_buffer.data(), player_count);
      }
      api->destroy_world(thread_world);
      progress++;
    }
  } else {
    ft_world *world = api->clone_world(StartWorld);
    if (!world) {
      api->destroy_world(StartWorld);
      return;
    }

    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Serial)");

      api->copy_world(world, StartWorld);
      for (int t = 0; t < ticks_per_iteration; ++t) {
        ZoneScopedN("Physics Tick");
        api->step_world(world, input_buffer.data(), player_count);
      }
      progress++;
    }
    api->destroy_world(world);
  }

  api->destroy_world(StartWorld);
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
      : m_pAPI(pAPI), m_pContext(pContext), m_ShowWindow(true), m_Iterations(100), m_TicksPerIteration(200), m_UseMultiThreading(true),
        m_IsRunning(false), m_Progress(0), m_LastElapsedTime(0.0) {

    log_info("Physics Profiler", "Plugin initialized.");
  }

  ~PhysicsProfilerPlugin() {
    if (m_BenchmarkThread.joinable()) {
      m_BenchmarkThread.join();
    }
    log_info("Physics Profiler", "Plugin shutting down.");
  }

  void ToggleWindow() { m_ShowWindow = !m_ShowWindow; }

  void Benchmark() {
    if (!m_pAPI || !m_pAPI->get_initial_world()) return;
    ZoneScopedN("Benchmark Execution"); // Tracy Zone for the whole benchmark

    m_IsRunning = true;
    m_Progress = 0;
    m_LastElapsedTime = 0.0;
    auto startTime = std::chrono::high_resolution_clock::now();

    const char *active_game = m_pContext ? m_pContext->active_game_id : "";
    const bool is_sm64 = (active_game && std::strcmp(active_game, "sm64") == 0);
    const bool is_ddnet = (active_game && std::strcmp(active_game, "ddnet") == 0);

    if (is_sm64) {
      m_LastElapsedTime = benchmark_sm64(m_pAPI, m_Iterations, m_TicksPerIteration, m_UseMultiThreading, m_Progress);
      m_IsRunning = false;
      return;
    } else if (is_ddnet) {
      benchmark_ddnet(m_pAPI, m_Iterations, m_TicksPerIteration, m_UseMultiThreading, m_Progress);
    } else {
      benchmark_generic(m_pAPI, m_Iterations, m_TicksPerIteration, m_UseMultiThreading, m_Progress);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    m_LastElapsedTime = std::chrono::duration<double>(endTime - startTime).count();

    m_IsRunning = false;
  }

  void StartBenchmarkThread() {
    if (m_IsRunning) return;

    if (m_BenchmarkThread.joinable()) {
      m_BenchmarkThread.join();
    }
    m_BenchmarkThread = std::thread(&PhysicsProfilerPlugin::Benchmark, this);
  }

  int RunCli(int argc, const char **argv) {
    for (int i = 0; i < argc; ++i) {
      if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
        m_Iterations = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
        m_TicksPerIteration = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--serial") == 0) {
        m_UseMultiThreading = false;
      }
    }
    if (!m_pAPI || !m_pAPI->get_initial_world()) {
      std::fprintf(stderr, "Physics Profiler: No initial world available to benchmark.\n");
      return 1;
    }
    Benchmark();
    if (m_LastElapsedTime < 0) { std::fprintf(stderr, "Physics Profiler: benchmark failed\n"); return 1; }
    const long long TotalTicks = (long long)m_Progress.load() * m_TicksPerIteration;
    const double TicksPerSecond = m_LastElapsedTime > 0.0 ? TotalTicks / m_LastElapsedTime : 0.0;
    const char *game = m_pContext ? m_pContext->active_game_id : "unknown";
    std::printf("Physics Profiler [%s]: %lld ticks in %.4f seconds (%.2f ticks/sec)\n",
                game, TotalTicks, m_LastElapsedTime, TicksPerSecond);
    return 0;
  }

  void Update() {
    if (!m_pContext || !m_pContext->imgui_context) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(m_pContext->imgui_context));
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("Physics Profiler")) {
        ImGui::MenuItem("Show Window", nullptr, &m_ShowWindow);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    // Tab takes the editor's interface down; this panel goes with it. A
    // profiling run started before it went down carries on regardless.
    if (m_ShowWindow && m_pContext->ui_visible) {
      if (ImGui::Begin("Physics Profiler", &m_ShowWindow)) {
        const char *active_game = m_pContext ? m_pContext->active_game_id : "";
        const bool is_sm64 = (active_game && std::strcmp(active_game, "sm64") == 0);
        const bool is_ddnet = (active_game && std::strcmp(active_game, "ddnet") == 0);

        if (is_sm64) {
          ImGui::Text("Benchmark controls for Super Mario 64 physics.");
          ImGui::Text("Uses the current initial world to benchmark SM64 simulation ticks.");
        } else if (is_ddnet) {
          ImGui::Text("Benchmark controls for the ddnet_physics library.");
          ImGui::Text("Uses the current initial world to benchmark, add as many players as you want");
        } else {
          ImGui::Text("Benchmark controls for %s physics.", (active_game && *active_game) ? active_game : "game");
          ImGui::Text("Uses the current initial world to benchmark simulation ticks.");
        }
        ImGui::Separator();

        ImGui::InputInt("Iterations", &m_Iterations);
        ImGui::InputInt("Ticks per Iteration", &m_TicksPerIteration);
        ImGui::Checkbox("Use Multi-threading (OpenMP)", &m_UseMultiThreading);

        ImGui::Separator();

        if (m_IsRunning) {
          ImGui::Text("Benchmark in progress... (%d / %d)", m_Progress.load(), m_Iterations);
          ImGui::ProgressBar((float)m_Progress / std::max(1, m_Iterations));
        } else {
          const bool has_world = (m_pAPI && m_pAPI->get_initial_world() != nullptr);
          if (!has_world) ImGui::BeginDisabled(true);
          if (ImGui::Button("Start Benchmark")) {
            StartBenchmarkThread();
          }
          if (!has_world) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(load a level first)");
          }

          if (m_LastElapsedTime < 0.0) ImGui::TextUnformatted("SM64 benchmark failed; see the log for the runtime error.");
          if (m_LastElapsedTime > 0.0) {
            ImGui::Text("Last run took: %.4f seconds", m_LastElapsedTime);
            long long TotalTicks = (long long)m_Progress.load() * m_TicksPerIteration;
            double TicksPerSecond = m_LastElapsedTime > 0.0 ? TotalTicks / m_LastElapsedTime : 0.0;
            ImGui::Separator();
            ImGui::Text("Raw Performance Metrics:");
            ImGui::Text("  Total Ticks: %lld", TotalTicks);
            if (TicksPerSecond >= 1e6) {
              ImGui::Text("  Ticks/Second: %.2f M", TicksPerSecond / 1e6);
            } else {
              ImGui::Text("  Ticks/Second: %.0f (%.2f k)", TicksPerSecond, TicksPerSecond / 1e3);
            }
            ImGui::Separator();
            ImGui::Text("In-Game Time Simulated Per Real-World Second:");
            const double TICKS_PER_INGAME_SECOND = is_sm64 ? 30.0 : (is_ddnet ? 50.0 : 60.0);
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

// Tracy's client spawns worker threads when it starts and joins them again when
// it stops. Neither may happen while this library is attaching or detaching: on
// Windows the loader lock is held across DllMain, and a thread can neither
// start nor exit without taking it, so a profiler whose lifetime is the shared
// object's deadlocks the loader. That is not a hypothetical for a plugin -- the
// host loads every plugin it finds and unloads the ones it is not running,
// purely to read their names, so the editor hung on startup for as long as this
// library sat in the plugins directory, enabled or not. TRACY_MANUAL_LIFETIME
// moves both ends into plugin_init/plugin_shutdown below, which the host calls
// with no loader lock held.
static void profiler_startup(void) {
#if defined(TRACY_ENABLE) && defined(TRACY_MANUAL_LIFETIME)
  if (!tracy::IsProfilerStarted()) tracy::StartupProfiler();
#endif
}

static void profiler_shutdown(void) {
#if defined(TRACY_ENABLE) && defined(TRACY_MANUAL_LIFETIME)
  if (tracy::IsProfilerStarted()) tracy::ShutdownProfiler();
#endif
}

extern "C" {

FT_PLUGIN_ABI_EXPORT()

// No plugin_game_id() is exported, making this a multi-game plugin supporting
// DDNet, SM64, and any other supported game engine.

FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  profiler_startup();
  return new PhysicsProfilerPlugin(context, api);
}

FT_API void plugin_update(void *plugin_data) { static_cast<PhysicsProfilerPlugin *>(plugin_data)->Update(); }

FT_API void plugin_shutdown(void *plugin_data) {
  delete static_cast<PhysicsProfilerPlugin *>(plugin_data);
  profiler_shutdown();
}

FT_API void plugin_show_ui(void *plugin_data) { static_cast<PhysicsProfilerPlugin *>(plugin_data)->ToggleWindow(); }

FT_API int plugin_cli(void *plugin_data, int argc, const char **argv) {
  return static_cast<PhysicsProfilerPlugin *>(plugin_data)->RunCli(argc, argv);
}
}
