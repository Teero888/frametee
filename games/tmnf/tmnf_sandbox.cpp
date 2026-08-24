// Driving the simulation from the engine's per-tick worlds.
//
// tmnf::sim::World owns one live simulation; an ft_world is a captured state
// plus the tick it was taken at. Stepping a world therefore restores its state,
// advances a single tick under the input the engine just handed over, and
// captures the result.
//
// This used to be considerably more involved. ForeverValidator's published
// sandbox takes input as an event timeline and compiles it into a control plan
// spanning a fixed horizon, so the module had to build event lists, carry the
// scenario's own seeded events into every one of them, keep an immutable plan
// per world so that rewriting inputs did not cost work on every tick, read the
// authored track ahead to make a forward simulation install a plan once, and
// grow the horizon in coarse steps because growing it invalidated every
// capture's plan. tmnf::sim::World takes one input record per tick, so all of
// that is gone -- see tmnf/tmnf_sim.h for why the module owns that API.

#include "tmnf_internal.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace tmnf {
namespace {

bool DirectoryHasPacks(const std::string &directory) {
  if (directory.empty()) return false;
  std::ifstream list(directory + "/packlist.dat", std::ios::binary);
  return static_cast<bool>(list);
}

// Which editor track a world's single player is recorded on, or -1 for a
// scratch world the engine simulates to answer a question.
std::int32_t ResolveTrack(const ft_game *game, const ft_world *world) {
  if (!game || !game->engine || !game->engine->timeline_player_track || !world || world->index < 0) return -1;
  return game->engine->timeline_player_track(static_cast<std::uint32_t>(world->index), 0u);
}

} // namespace

// --- packs -------------------------------------------------------------------

std::string ResolvePacks(const ft_engine_api *api) {
  if (!api || !api->resolve_data_path) return {};
  char buffer[1024];
  api->resolve_data_path("Packs", buffer, sizeof(buffer));
  if (DirectoryHasPacks(buffer)) return buffer;
  return {};
}

// --- lifecycle ---------------------------------------------------------------

bool OpenSandbox(ft_game *game, const void *bytes, std::size_t size, const char *identity) {
  if (!game || !bytes || size == 0) return false;
  (void)identity;

  std::string diagnostic;
  std::unique_ptr<sim::World> world = sim::World::Open(game->packs, bytes, size, &diagnostic);
  if (!world) {
    Log(game, FT_LOG_ERROR, "Could not open the track: %s", diagnostic.c_str());
    return false;
  }
  game->world = std::move(world);
  return true;
}

void CloseSandbox(ft_game *game) {
  if (!game) return;
  std::lock_guard<std::mutex> lock(game->mutex);
  game->world.reset();
}

// --- worlds ------------------------------------------------------------------

ft_world *WorldCreate(ft_game *game, const ft_world_desc *desc) {
  if (!game) return nullptr;
  auto *world = new ft_world();
  world->index = desc ? desc->world_index : -1;
  // Handed on so a game-specific plugin can open a simulation that matches this
  // one; see tmnf/tmnf_game.h.
  world->packs = game->packs.empty() ? nullptr : game->packs.c_str();
  world->level_path = game->level && !game->level->path.empty() ? game->level->path.c_str() : nullptr;
  world->track = ResolveTrack(game, world);
  if (desc && desc->level && desc->level->initial) {
    world->state = desc->level->initial;
    world->view = world->state.View();
  }
  return world;
}

void WorldDestroy(ft_game *, ft_world *world) { delete world; }

void WorldCopy(ft_game *, ft_world *dst, const ft_world *src) {
  if (!dst || !src) return;
  dst->packs = src->packs;
  dst->level_path = src->level_path;
  // A captured state is a refcounted handle, so the engine's constant
  // snapshotting only touches a counter. The destination keeps its own identity:
  // prediction worlds are index -1 and must stay that way.
  dst->state = src->state;
  dst->view = src->view;
}

void WorldStep(ft_game *game, ft_world *world, const void *inputs, std::uint32_t player_count) {
  if (!game || !world || !world->state) return;

  TmnfInput record{};
  if (inputs && player_count > 0) std::memcpy(&record, inputs, sizeof(record));

  std::lock_guard<std::mutex> lock(game->mutex);
  if (!game->world) return;
  // A world created before the level opened has none of these yet, and
  // reopening a level replaces the ones it does have.
  world->packs = game->packs.empty() ? nullptr : game->packs.c_str();
  world->level_path = game->level && !game->level->path.empty() ? game->level->path.c_str() : nullptr;

  // Restoring is skipped when the simulation is already sitting where this
  // world left it, which straight-line playback always is.
  if (!game->world->Restore(world->state)) return;
  if (!game->world->Step(record)) return;

  world->state = game->world->Capture();
  world->view = game->world->View();
}

// --- shared helpers ----------------------------------------------------------

CarPose InterpolateCar(const ft_world *previous, const ft_world *current, float alpha) {
  CarPose pose;
  if (!current) return pose;
  const ft_world *before = previous ? previous : current;
  alpha = std::clamp(alpha, 0.f, 1.f);

  const auto &c0 = before->view.car;
  const auto &c1 = current->view.car;

  pose.position = Lerp(ToVec3(c0.position), ToVec3(c1.position), alpha);
  pose.rotation = Slerp(CarRotation(c0), CarRotation(c1), alpha);
  pose.velocity = Lerp(ToVec3(c0.linearSpeed), ToVec3(c1.linearSpeed), alpha);
  AxesFromQuat(pose.rotation, &pose.forward, &pose.right, &pose.up);
  return pose;
}

} // namespace tmnf
