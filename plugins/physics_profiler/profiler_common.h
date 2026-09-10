#pragma once

#include <atomic>
#include "plugin_api.h"

void benchmark_ddnet(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress);
double benchmark_sm64(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress);
void benchmark_generic(const tas_api_t *api, int iterations, int ticks_per_iteration, bool use_multithreading, std::atomic<int> &progress);
