#include "profiler_common.h"
#include "tracy/Tracy.hpp"
#include <ddnet/ddnet_game.h>
#if defined(_OPENMP)
#include <omp.h>
#endif

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
  pInput->m_Direction = static_cast<int8_t>(fast_rand_range(seed, -1, 1));
  pInput->m_Jump = static_cast<uint8_t>(fast_rand_range(seed, 0, 1));
  pInput->m_Fire = static_cast<uint8_t>(fast_rand_range(seed, 0, 1));
  pInput->m_Hook = static_cast<uint8_t>(fast_rand_range(seed, 0, 1));
  pInput->m_TargetX = static_cast<int16_t>(fast_rand_range(seed, -1000, 1000));
  pInput->m_TargetY = static_cast<int16_t>(fast_rand_range(seed, -1000, 1000));
  pInput->m_WantedWeapon = static_cast<uint8_t>(fast_rand_range(seed, 0, NUM_WEAPONS - 1));
}

void benchmark_ddnet(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress) {
  if (!api || !api->get_initial_world()) return;

  SWorldCore StartWorld = wc_empty();
  wc_copy_world(&StartWorld, const_cast<SWorldCore *>(ddnet_world(api->get_initial_world())));

  unsigned int global_seed = 0;
  if (use_multithreading) {
#pragma omp parallel for
    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Parallel)");

      unsigned int run_seed = global_seed ^ (i * 0x9E3779B9u);
      SWorldCore World = {};
      wc_copy_world(&World, &StartWorld);
      for (int t = 0; t < ticks_per_iteration; ++t) {
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
      progress++;
    }
  } else {
    for (int i = 0; i < iterations; ++i) {
      FrameMarkNamed("Worker Thread Frame");
      ZoneScopedN("Single Iteration (Serial)");

      unsigned int run_seed = global_seed ^ (i * 0x9E3779B9u);
      SWorldCore World = {};
      wc_copy_world(&World, &StartWorld);
      for (int t = 0; t < ticks_per_iteration; ++t) {
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
      progress++;
    }
  }

  wc_free(&StartWorld);
}
