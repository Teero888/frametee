#include "profiler_common.h"

#ifdef PHYSICS_PROFILER_NO_SM64
// Built without the SM64 game: there is no library to benchmark.
double benchmark_sm64(const tas_api_t *, int, int, bool, std::atomic<int> &) { return -1; }
#else
#include "tracy/Tracy.hpp"
#include <sm64/sm64_game.h>
#include <chrono>
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

// One controller read in the .m64 layout sm64_step takes.
static inline uint32_t generate_random_sm64_input(unsigned int *seed) {
  uint32_t input = 0;
  input |= static_cast<uint32_t>(static_cast<uint8_t>(fast_rand_range(seed, -80, 80))) << 16; // stick x
  input |= static_cast<uint32_t>(static_cast<uint8_t>(fast_rand_range(seed, -80, 80))) << 24; // stick y
  if (fast_rand_range(seed, 0, 3) == 0) input |= 0x80; // A (jump)
  if (fast_rand_range(seed, 0, 5) == 0) input |= 0x40; // B (attack)
  if (fast_rand_range(seed, 0, 5) == 0) input |= 0x20; // Z (crouch)
  return input;
}

double benchmark_sm64(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress) {
  if (!api || iterations <= 0 || ticks_per_iteration <= 0) return -1;
  const sm64_world *start = sm64_game_world(api->get_initial_world());
  if (!start) return -1;
  int threads = 1;
#if defined(_OPENMP)
  if (use_multithreading) threads = std::min(iterations, omp_get_max_threads());
#endif
  // One world per thread, reset from the start before every iteration.
  std::vector<sm64_world *> worlds(threads);
  for (auto &world : worlds) world = sm64_world_clone(start);
  // Input generation is not physics throughput.
  std::vector<uint32_t> inputs(static_cast<size_t>(ticks_per_iteration));
  unsigned int seed = 0x9e3779b9u;
  for (auto &input : inputs) input = generate_random_sm64_input(&seed);
  const auto begin = std::chrono::steady_clock::now();
#pragma omp parallel for num_threads(threads)
  for (int i = 0; i < iterations; ++i) {
    FrameMarkNamed("Worker Thread Frame");
    int thread = 0;
#if defined(_OPENMP)
    thread = omp_get_thread_num();
#endif
    sm64_world *world = worlds[thread];
    sm64_world_copy(world, start);
    for (const auto input : inputs) sm64_step(world, input);
    ++progress;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  for (auto *world : worlds) sm64_world_destroy(world);
  return seconds;
}
#endif
