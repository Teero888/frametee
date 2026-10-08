#include "profiler_common.h"
#include "tracy/Tracy.hpp"
#include <ddnet/ddnet_game.h>
#if defined(_OPENMP)
#include <omp.h>
#endif

#include <cstring>

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

static inline void generate_random_input(ddnet_input_t *input, unsigned int *seed) {
  input->direction = fast_rand_range(seed, -1, 1);
  input->jump = fast_rand_range(seed, 0, 1);
  input->fire = fast_rand_range(seed, 0, 1);
  input->hook = fast_rand_range(seed, 0, 1);
  input->target_x = fast_rand_range(seed, -1000, 1000);
  input->target_y = fast_rand_range(seed, -1000, 1000);
  input->wanted_weapon = fast_rand_range(seed, 0, DDNET_NUM_WEAPONS - 1) + 1;
}

// One run: a copy of the start world, stepped with random inputs by the physics without effects.
static void run_once(const ddnet_world_t *start, int ticks_per_iteration, unsigned int run_seed, int i) {
  ddnet_world_t world;
  std::memset(&world, 0, sizeof(world));
  ddnet_world_copy(&world, start);
  for (int t = 0; t < ticks_per_iteration; ++t) {
    ZoneScopedN("Physics Tick");
    unsigned int local_seed = run_seed ^ i;
    for (int c = 0; c < world.num_clients; c++) {
      if (!world.players[c].active) continue;
      generate_random_input(&world.players[c].input, &local_seed);
    }
    ddnet_world_tick(&world);
  }
  ddnet_world_free(&world);
}

void benchmark_ddnet(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress) {
  if (!api || !api->get_initial_world()) return;

  ddnet_world_t start;
  std::memset(&start, 0, sizeof(start));
  ddnet_world_copy(&start, ddnet_world(api->get_initial_world()));

  unsigned int global_seed = 0;
  if (use_multithreading) {
#pragma omp parallel for
    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Parallel)");
      run_once(&start, ticks_per_iteration, global_seed ^ (i * 0x9E3779B9u), i);
      progress++;
    }
  } else {
    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Serial)");
      run_once(&start, ticks_per_iteration, global_seed ^ (i * 0x9E3779B9u), i);
      progress++;
    }
  }

  ddnet_world_free(&start);
}
