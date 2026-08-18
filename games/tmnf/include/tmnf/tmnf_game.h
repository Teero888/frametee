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

#include <forevervalidator/experimental/physics_sandbox.h>
#include <frametee/game_abi.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace tmnf {

namespace fve = forevervalidator::experimental;

// How the module drives the simulation. A TMNF tick is 10 ms, so the engine
// runs this game at 100 ticks per second; the countdown before the clock starts
// is simulated by the sandbox and never reaches the timeline.
inline constexpr std::uint32_t kTickMs = 10u;
inline constexpr std::uint32_t kPrestartMs = 2600u;

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
// PhysicsSandboxCarState::wheelSurface. The values are the game's own
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

// The module's own input timeline. Only the handle crosses this header: a
// plugin carries a world's plan around with it but never looks inside one.
struct InputPlan;
using InputPlanPtr = std::shared_ptr<const InputPlan>;

} // namespace tmnf

// The layout the engine hands around as an opaque pointer.
struct ft_world {
  std::optional<tmnf::fve::PhysicsSandboxState> state;
  tmnf::fve::PhysicsSandboxStateView view{};
  // The plan this world's state was produced under. Copying a world only
  // touches a refcount.
  tmnf::InputPlanPtr plan;
  // Which editor world this belongs to; -1 marks a scratch world the engine
  // simulates to answer a question rather than to show.
  std::int32_t index = -1;
  // The editor track this world's single player is recorded on, so authored
  // input can be read ahead instead of discovered one tick at a time.
  std::int32_t track = -1;
  // The sandbox this world was captured from.
  //
  // A plugin that wants to simulate on its own cannot share this one -- it is
  // the editor's, and serialised behind the game's lock -- and cannot clone it
  // either, because ClonePhysicsSandbox serves only OptimizedCpu sandboxes and
  // this game runs Reference, which is the backend the validator claims parity
  // for. What a plugin can do is build a second sandbox over the same scenario,
  // which RestoreState will accept: it matches captures by fingerprint and
  // options rather than by identity. This is here so those options can be read
  // off the real thing instead of guessed. Borrowed, and dies with the level.
  tmnf::fve::PhysicsSandbox *sandbox = nullptr;
  // Where the game found the installed packs, so a plugin building a sandbox of
  // its own reads the same installation the editor did rather than repeating the
  // search and possibly landing somewhere else. Borrowed from the game.
  const char *packs = nullptr;
};

// Reads a world handed over by the engine, e.g. from tas_api_t::get_world_state_at.
// The view is everything the simulation reports about a tick; the state is the
// capture a sandbox can be restored to, which is what a plugin running its own
// simulation needs.
static inline const tmnf::fve::PhysicsSandboxStateView *tmnf_world(const ft_world *world) {
  return world ? &world->view : nullptr;
}
static inline const tmnf::fve::PhysicsSandboxState *tmnf_world_state(const ft_world *world) {
  return world && world->state ? &*world->state : nullptr;
}
// The editor's sandbox for this world, to copy simulation options from. Null
// before a level is open.
static inline tmnf::fve::PhysicsSandbox *tmnf_world_sandbox(const ft_world *world) {
  return world ? world->sandbox : nullptr;
}
// The packs directory the game is running against, or null when it found none.
static inline const char *tmnf_world_packs(const ft_world *world) {
  return world ? world->packs : nullptr;
}

// Input records the engine stores are the module's own TmnfInput. The engine
// pads them into a fixed-size slot, so read and write through these rather than
// casting an array.
static inline const tmnf::TmnfInput *tmnf_input(const void *record) { return (const tmnf::TmnfInput *)record; }
static inline tmnf::TmnfInput *tmnf_input_mut(void *record) { return (tmnf::TmnfInput *)record; }

#endif // TMNF_GAME_PUBLIC_H
