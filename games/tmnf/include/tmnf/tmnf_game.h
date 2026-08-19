#ifndef TMNF_GAME_PUBLIC_H
#define TMNF_GAME_PUBLIC_H

// What the TrackMania Nations Forever module promises its own plugins.
//
// The engine treats ft_world and ft_level as opaque, and rightly so. A plugin
// that declares itself TMNF-specific with
//
//   FT_API const char *plugin_game_id(void) { return "tmnf"; }
//
// is however built for this game and may look inside, which is what this header
// is for. Nothing here is part of the engine ABI: it is a contract between one
// game and the plugins written for it, and it changes when this game changes.
//
// Unlike its DDNet counterpart this header is the single definition rather than
// a copy: tmnf_internal.h includes it, so the layout a plugin compiles against
// is by construction the layout the module was built with.

#include <frametee/game_abi.h>
#include <tmnf/tmnf_sim.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace tmnf {

// How the module drives the simulation. A TMNF tick is 10 ms, so the engine
// runs this game at 100 ticks per second; the countdown before the clock starts
// is simulated internally and never reaches the timeline.
inline constexpr std::uint32_t kTickMs = 10u;
inline constexpr std::uint32_t kPrestartMs = 2600u;

// Which backend the whole project simulates on.
//
// Not a local performance knob: a state captured by one tmnf::sim::World is
// restored into another, so the module and every plugin that opens a World of
// its own have to agree on this, which is why it lives in the header they share.
// Nothing checks it at runtime -- see tmnf::sim::State.
//
// OptimizedCpu rather than Reference: it is roughly four times faster
// per tick. Upstream names
// Reference as its parity target and does not claim corpus parity for the other
// backends -- see ForeverValidator/docs/reference-determinism.md, whose one
// documented divergence is a checkpoint-contact rule enabled only on a Reference
// leaf. Measured against that: the 209 runnable stock campaign replays, each run
// to its recorded end, agree bit for bit on position, checkpoints, laps, finish
// flag and finish time. Validation that has to be authoritative rather than fast
// should still ask for Reference explicitly.
inline constexpr forevervalidator::SimulationBackend kSimulationBackend =
    forevervalidator::SimulationBackend::OptimizedCpu;

// --- input record ------------------------------------------------------------

enum InputField { FIELD_ACCELERATE = 0, FIELD_BRAKE, FIELD_STEER, FIELD_RESPAWN, FIELD_COUNT };

// Laid out so the record is eight bytes with natural alignment; the engine
// stores arrays of these in snippets and project files.
struct TmnfInput {
  std::uint8_t accelerate = 0;
  std::uint8_t brake = 0;
  std::uint8_t respawn = 0;
  std::uint8_t reserved = 0;
  // Full lock either way, matching the game's own analog range.
  std::int32_t steer = 0;

  bool operator==(const TmnfInput &o) const {
    return accelerate == o.accelerate && brake == o.brake && respawn == o.respawn && steer == o.steer;
  }
  bool operator!=(const TmnfInput &o) const { return !(*this == o); }
};

static_assert(sizeof(TmnfInput) == 8, "the input record must stay a fixed eight bytes");

// What a wheel is rolling on, as the simulation reports it per wheel in
// tmnf::sim::CarState::wheelSurface. The values are the game's own
// EPlugSurfaceMaterialId; only the few a plugin is likely to reason about are
// named here, and a wheel touching nothing reports kSurfaceNone rather than a
// material.
enum : std::uint16_t {
  kSurfaceConcrete = 0u,
  kSurfacePavement = 1u,
  kSurfaceGrass = 2u,
  kSurfaceIce = 3u,
  kSurfaceDirt = 6u,
  kSurfaceTurbo = 7u,
  kSurfaceWater = 13u,
  kSurfaceAsphalt = 16u,
  kSurfaceWetGrass = 20u,
  kSurfaceNone = 0xFFFFu,
};

// Grass comes in a dry and a wet kind and they are different materials, so
// asking "is this grass" is a question rather than a comparison.
inline bool SurfaceIsGrass(std::uint16_t surface) {
  return surface == kSurfaceGrass || surface == kSurfaceWetGrass;
}

} // namespace tmnf

// The layout the engine hands around as an opaque pointer.
//
// A world is a snapshot, not a simulation: the game owns one tmnf::sim::World
// per level and every ft_world is a state it can be put back on. Copying one is
// a refcount.
struct ft_world {
  tmnf::sim::State state;
  tmnf::sim::StateView view{};
  // Which editor world this belongs to; -1 marks a scratch world the engine
  // simulates to answer a question rather than to show.
  std::int32_t index = -1;
  // The editor track this world's single player is recorded on.
  std::int32_t track = -1;
  // What a plugin needs to open a tmnf::sim::World of its own, because the
  // game's belongs to the editor and is serialised behind its lock. Reading the
  // same installation and the same track file is what makes a state captured
  // here restore there -- and nothing checks it, so it is on the plugin to use
  // these rather than to go looking for a track itself. Both are borrowed and
  // die with the level.
  const char *packs = nullptr;
  const char *level_path = nullptr;
};

// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
// The view is everything the simulation reports about a tick; the state is what
// a tmnf::sim::World can be restored to, which is what a plugin running its own
// simulation needs.
static inline const tmnf::sim::StateView *tmnf_world(const ft_world *world) {
  return world ? &world->view : nullptr;
}
static inline const tmnf::sim::State *tmnf_world_state(const ft_world *world) {
  return world && world->state ? &world->state : nullptr;
}
// The packs directory the game is running against, or null when it found none.
static inline const char *tmnf_world_packs(const ft_world *world) {
  return world ? world->packs : nullptr;
}
// The track file this world is simulating, or null before a level is open.
static inline const char *tmnf_world_level_path(const ft_world *world) {
  return world ? world->level_path : nullptr;
}

// Input records the engine stores are the module's own TmnfInput. The engine
// pads them into a fixed-size slot, so read and write through these rather than
// casting an array.
static inline const tmnf::TmnfInput *tmnf_input(const void *record) { return (const tmnf::TmnfInput *)record; }
static inline tmnf::TmnfInput *tmnf_input_mut(void *record) { return (tmnf::TmnfInput *)record; }

#endif // TMNF_GAME_PUBLIC_H
