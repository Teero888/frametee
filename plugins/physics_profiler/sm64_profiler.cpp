#include "profiler_common.h"
#include "tracy/Tracy.hpp"
#include <sm64/sm64_game.h>
#include <chrono>
#include <cstdio>
#include <vector>
#include <algorithm>
#if defined(_OPENMP)
#include <omp.h>
#endif

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

static inline void generate_random_sm64_input(sm64_input *pInput, unsigned int *seed) {
  pInput->stick_x = static_cast<int8_t>(fast_rand_range(seed, -80, 80));
  pInput->stick_y = static_cast<int8_t>(fast_rand_range(seed, -80, 80));
  uint16_t buttons = 0;
  if (fast_rand_range(seed, 0, 3) == 0) buttons |= 0x8000; // A (jump)
  if (fast_rand_range(seed, 0, 5) == 0) buttons |= 0x4000; // B (attack)
  if (fast_rand_range(seed, 0, 5) == 0) buttons |= 0x2000; // Z (crouch)
  pInput->buttons = buttons;
}

double benchmark_sm64(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress) {
  if (!api || iterations <= 0 || ticks_per_iteration <= 0) return -1;
  const ft_world *initial = api->get_initial_world();
  if (!initial || !initial->physics_create) return -1;
  ft_world *source = api->clone_world(initial);
  if (!source) return -1;
  int threads = 1;
#if defined(_OPENMP)
  if (use_multithreading) threads = std::min(iterations, omp_get_max_threads());
#endif
  struct Worker { sm64_physics *sim = nullptr; sm64_checkpoint *start = nullptr; };
  std::vector<Worker> workers(threads);
  std::atomic<bool> failed{false};
#pragma omp parallel for num_threads(threads)
  for (int i=0;i<threads;++i) {
    char error[2048]{};
    auto &worker = workers[i];
    worker.sim = sm64_world_physics(source, error, sizeof(error));
    if(worker.sim) worker.start=worker.sim->capture(worker.sim,error,sizeof(error));
    if(!worker.start) { std::fprintf(stderr,"SM64 physics: %s\n",error); failed=true; }
  }
  api->destroy_world(source);
  // Input generation and native image loading are not physics throughput.
  std::vector<sm64_input> inputs(static_cast<size_t>(ticks_per_iteration));
  unsigned int seed = 0x9e3779b9u;
  for (auto &input : inputs) generate_random_sm64_input(&input, &seed);
  const auto begin=std::chrono::steady_clock::now();
  if(!failed) {
#pragma omp parallel for num_threads(threads)
    for(int i=0;i<iterations;++i) {
      int thread=0;
#if defined(_OPENMP)
      thread=omp_get_thread_num();
#endif
      auto &worker=workers[thread];
      char error[2048]{};
      if(!worker.sim->restore(worker.sim,worker.start,error,sizeof(error))) { failed=true; continue; }
      // This is the runtime's own function pointer, with no engine callbacks,
      // world switching, snapshot capture, or Tracy callstack on each tick.
      auto tick=worker.sim->step;
      for(const auto input : inputs) tick(input);
      ++progress;
    }
  }
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  for(auto &worker:workers) if(worker.sim) {
    worker.sim->free_checkpoint(worker.start);
    worker.sim->destroy(worker.sim);
  }
  return failed ? -1 : seconds;
}
