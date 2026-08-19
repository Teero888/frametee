#ifndef TMNF_SIM_H
#define TMNF_SIM_H

// The module's own simulation API.
//
// ForeverValidator publishes an experimental sandbox (physics_sandbox.h) built
// for its real job, which is validating a recorded replay against its ghost. It
// takes input as an event timeline and compiles that into a per-tick control
// plan spanning a fixed simulation horizon, so an input edit costs work
// proportional to the horizon rather than to the edit, the horizon is a wall you
// have to size before you start, and growing it invalidates every capture's
// plan. None of that is physics: TrackMania consumes exactly one input record
// per tick, the same as any other game this editor drives.
//
// So this is the shape the module actually wants, over the same simulation:
//
//     World   the decoded track and one live simulation over it
//     State   a copyable, refcounted snapshot -- the car plus race bookkeeping
//     Step    one tick, one TmnfInput
//
// Underneath it drives ForeverValidator's own ReplaySimulationSession, which is
// what the sandbox drives too. Measured against the sandbox on four randomised
// input programs of 100 to 3000 ticks, both paths agree bit for bit on position,
// velocity and checkpoint count.
//
// The reason to own this rather than use the published API is not raw speed --
// stepping is the same physics either way -- it is that installing input costs
// nothing here, so cost per tick no longer depends on how big a block of input
// is written at once:
//
//     block ticks     this API      sandbox (30 s horizon)
//         1           12.5 us/tick   90.9 us/tick
//        10           10.2           17.5
//       100           10.3           10.6
//
// A search that changes one input and re-steps pays 7x on the sandbox and
// nothing here, which is what makes per-tick search strategies affordable.
//
// The cost is that ReplaySimulationSession is internal to the submodule and
// carries no stability promise. That risk is confined to tmnf_sim.cpp: this
// header names none of it, so the module's other files and its plugins compile
// against this and are insulated from a submodule bump. Only the data types
// below come from ForeverValidator, and those are from its *public* header.

#include <forevervalidator/experimental/physics_sandbox.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tmnf {

struct TmnfInput;

namespace sim {

namespace fve = forevervalidator::experimental;

// Plain data from ForeverValidator's public header, reused rather than
// re-declared so that nothing has to be converted at this boundary.
using CarState = fve::PhysicsSandboxCarState;
using StateView = fve::PhysicsSandboxStateView;
using Ellipsoid = fve::PhysicsSandboxEllipsoid;
using Triangle = fve::PhysicsSandboxCollisionTriangle;
using RenderSceneHandle = fve::PhysicsSandboxRenderSceneHandle;

// A snapshot of one tick.
//
// Copying is a refcount, which is what makes the engine's constant snapshotting
// affordable: the payload is a few kilobytes of car and race state and it is
// never deep-copied. A default-constructed State is empty and restores nowhere.
//
// Unlike the sandbox's capture this carries no fingerprint, so nothing checks
// that a State is being restored into a World over the same track. Worlds opened
// on the same track file from the same packs are interchangeable; anything else
// is undefined, and it is the caller who has to know.
class State {
public:
  State() = default;

  explicit operator bool() const noexcept { return impl_ != nullptr; }
  // Everything the simulation reports about the tick this was taken at.
  const StateView &View() const noexcept;

private:
  friend class World;
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

// One decoded track and one live simulation over it.
//
// A World is expensive to open -- about a second, nearly all of it decoding the
// track out of the installed packs -- and cheap to drive. It holds a single
// simulation, so the pattern is to keep the states you care about and restore
// the one you want to continue from:
//
//     world->Restore(state);
//     world->Step(input);
//     State next = world->Capture();
//
// Restore is a few microseconds and can be skipped when the World is already
// sitting on the state you want, which straight-line playback always is.
//
// Not thread safe, and deliberately not: a World is one simulation. Two threads
// that want to search at once want two Worlds.
class World {
public:
  ~World();
  World(const World &) = delete;
  World &operator=(const World &) = delete;

  // Opens `challenge_bytes` (a .Challenge.Gbx) against an installed Packs
  // directory. Returns null and fills `diagnostic` on any failure.
  static std::unique_ptr<World> Open(const std::string &packs, const void *challenge_bytes, std::size_t size,
                                     std::string *diagnostic);

  // --- the track ---
  const std::string &MapName() const noexcept;
  // Static collision geometry, in world space.
  const std::vector<Triangle> &Collision() const noexcept;
  // The car's own collision ellipsoids, in vehicle space.
  const std::vector<Ellipsoid> &CarShape() const noexcept;
  // The visual scene, shared by pointer and immutable.
  RenderSceneHandle Render() const noexcept;

  // The state at race tick zero, after the countdown the simulation runs
  // internally and never reports.
  const State &Start() const noexcept;

  // --- the simulation ---
  // Puts the simulation back on `state`. Cheap, and a no-op when the World is
  // already there.
  bool Restore(const State &state);
  // Advances exactly one tick under `input`.
  bool Step(const TmnfInput &input);
  // A snapshot of where the simulation is now.
  State Capture() const;
  // Where the simulation is now, without taking a snapshot.
  const StateView &View() const noexcept;

private:
  World();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sim
} // namespace tmnf

#endif // TMNF_SIM_H
