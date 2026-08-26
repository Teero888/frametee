// The one file in this module that reaches into ForeverValidator's internals.
//
// Everything below the API in tmnf/tmnf_sim.h lives here on purpose: these
// headers are not part of what the submodule publishes and can change under a
// bump, so the blast radius is this translation unit and nothing else. When a
// bump does break something it breaks here, at compile time, with the whole
// mapping in one place to fix.
//
// Nothing in the submodule is modified. This links because
// games/tmnf/CMakeLists.txt assembles the validator's archives into one shared
// library with --whole-archive and the submodule carries no export decoration,
// so its internal symbols land in the .so with default visibility.

#include <tmnf/tmnf_sim.h>
#include <tmnf/tmnf_game.h>

#include "format/assets/replay_asset_repository.h"
#include "format/pack/default_vehicle_pack_archive.h"
#include "format/pack/installed/installed_pack_key_catalog.h"
#include "format/pack/installed/plug_file_pack.h"
#include "format/pack/installed_vehicle_asset_graph.h"
#include "format/pack/replay_vehicle_source_bundle.h"
#include "format/replay/replay_file.h"
#include "format/static_solid/default_vehicle_solid_archive.h"
#include "simulation/control/replay_control_timeline.h"
#include "simulation/runtime/replay_deterministic_execution.h"
#include "simulation/runtime/replay_simulation_definition.h"
#include "simulation/runtime/replay_simulation_session.h"
#include "validation/planning/replay_asset_route.h"
#include "validation/planning/replay_challenge_map_preload.h"

#include <cstdio>
#include <utility>

namespace tmnf {
namespace sim {
namespace {

constexpr std::uint32_t kPrestartTicks = kPrestartMs / kTickMs;

forevervalidator::Vector3 ToVec(const GmVec3 &v) { return {v.x, v.y, v.z}; }
GmVec3 ToGm(const forevervalidator::Vector3 &v) {
  GmVec3 out{};
  out.x = v.x;
  out.y = v.y;
  out.z = v.z;
  return out;
}

bool ReadFile(const std::string &path, std::vector<std::uint8_t> *out) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<std::size_t>(n));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

// What ControlsFromState in the validator's plan builder derives, restricted to
// what an authored input record can say: digital accelerate and brake, analog
// steering. The analog-versus-digital arbitration there only matters to replays
// that mix both, which nothing the editor authors ever does.
ReplayVehicleControlState Controls(const TmnfInput &in) {
  ReplayVehicleControlState c{};
  c.lowSpeedGateA = in.accelerate ? 1.0f : 0.0f;
  c.lowSpeedGateB = in.brake ? 1.0f : 0.0f;
  c.steering = static_cast<float>(in.steer) / static_cast<float>(forevervalidator::kAnalogInputScale);
  return c;
}

// The control tick the session consumes.
//
// The action flags reproduce what BuildReplayControlPlan derives for a canonical
// timeline whose only seeded event is RaceRunning at time zero: the spawn is
// established on the first tick, and the race turns on exactly at the end of the
// countdown, which is also where its one reset fires. Everything after that is
// steady state, which is why this needs no history and can be built per tick.
ReplayControlTick MakeTick(std::uint32_t time_ms, const TmnfInput &in, bool respawn) {
  ReplayControlTick tick;
  tick.periodMs = kTickMs;
  tick.timeMs = time_ms;
  tick.observe = false;
  tick.actions.establishRaceSpawn = time_ms == kTickMs;
  tick.actions.enableRaceSimulation = time_ms >= kPrestartMs;
  tick.actions.resetAtRaceStart = time_ms == kPrestartMs;
  // Counted per event rather than held, which is how the game behaves: holding
  // the key respawns again on every tick.
  tick.actions.respawnAtCheckpointCount = respawn ? 1u : 0u;
  tick.controls = Controls(in);
  return tick;
}

} // namespace

// --- State -------------------------------------------------------------------

struct State::Impl {
  std::shared_ptr<const ReplaySimulationInstanceClone> clone;
  StateView view{};
  std::uint32_t time_ms = 0u;
};

const StateView &State::View() const noexcept {
  static const StateView empty{};
  return impl_ ? impl_->view : empty;
}

bool State::EngineOn() const noexcept {
  return impl_ && impl_->clone ? impl_->clone->runtime.vehicle.car.integration.integrateEngine : true;
}

// --- World -------------------------------------------------------------------

struct World::Impl {
  // Held for as long as the session is: the session reads out of them.
  InstalledPackKeyCatalog keys;
  std::unique_ptr<ReplayAssetRepository> map_assets;
  std::unique_ptr<ReplayAssetRepository> decoration_assets;
  ReplayVehicleSourceBundle vehicle_sources;
  ReplaySimulationDefinition definition;
  std::unique_ptr<ReplaySimulationSession> session;
  CGameCtnReplayChallengeMapPreload preload;

  std::string map_name;
  std::vector<Triangle> collision;
  std::vector<Ellipsoid> car_shape;
  RenderSceneHandle render;
  State start;

  // Where the live simulation is: the wall-clock of the control timeline, and
  // which State it is sitting on so a redundant restore can be skipped.
  std::uint32_t time_ms = 0u;
  StateView view{};
  const State::Impl *resident = nullptr;

  // The one-tick block Advance hands to the session, kept so that stepping
  // never allocates.
  std::vector<ReplayControlTick> block{ReplayControlTick{}};

  void ReadView();
  bool Advance(const ReplayControlTick &tick);
};

// Mirrors PhysicsSandbox::Impl::ReadView, which is the only description of how
// a session's raw state becomes the view the rest of the module reads.
void World::Impl::ReadView() {
  const std::optional<ReplaySimulationStateView> state = session->CurrentState();
  if (!state) return;

  StateView v{};
  v.tick = time_ms >= kPrestartMs ? (time_ms - kPrestartMs) / kTickMs : 0u;
  v.timeMs = v.tick * kTickMs;
  v.mapEnvironment = view.mapEnvironment;
  v.vehicleModel = view.vehicleModel;
  v.playMode = view.playMode;

  const ReplayDynaFrameState &frame = state->frame;
  v.car.rotationX = frame.rotationQuaternion.x;
  v.car.rotationY = frame.rotationQuaternion.y;
  v.car.rotationZ = frame.rotationQuaternion.z;
  v.car.rotationW = frame.rotationQuaternion.w;
  v.car.position = ToVec(frame.position);
  v.car.linearSpeed = ToVec(frame.linearSpeed);
  v.car.angularSpeed = ToVec(frame.angularSpeed);
  v.car.force = ToVec(frame.force);
  v.car.torque = ToVec(frame.torque);
  v.car.signedSpeed = state->signedSpeed;
  v.car.turbo = state->turbo;
  v.car.cameraFlightTransition = state->cameraFlightTransition;
  v.car.burning = state->burning;
  v.car.gearChanged = state->gearChanged;
  v.car.wheelContact = state->wheelContact;
  v.car.wheelHasSurface = state->wheelHasSurface;
  v.car.cameraSupportUp = ToVec(state->cameraSupportUp);
  v.car.localSpeed = ToVec(state->localSpeed);
  v.car.freeWheeling = state->freeWheeling;
  v.car.lateralContact = state->lateralContact;
  v.car.sliding = state->sliding;
  v.car.gear = state->gear;
  v.car.rpm = state->rpm;
  v.car.turningRate = state->turningRate;
  v.car.turboType = state->turboType;
  v.car.turboBoostFactor = state->turboBoostFactor;
  v.car.wheelSliding = state->wheelSliding;
  v.car.wheelSurface = state->wheelSurface;
  v.accelerate = state->controls.lowSpeedGateA;
  v.brake = state->controls.lowSpeedGateB;
  v.steering = state->controls.steering;
  v.checkpointsCollected = state->race.checkpointCount;
  v.checkpointsTotal = state->race.requiredCheckpointCount;
  v.completedLaps = state->race.completedLapCount;
  v.totalLaps = state->race.requiredLapCount;
  v.raceCompleted = state->race.raceCompleted;

  // The countdown is simulated but never reported, so every time the race
  // reports is relative to its end.
  if (state->finishTime) {
    const std::uint64_t prestart_ns = static_cast<std::uint64_t>(kPrestartMs) * 1000000u;
    v.finishTime = *state->finishTime;
    const auto trim = [prestart_ns](std::uint64_t ns) { return ns >= prestart_ns ? ns - prestart_ns : 0u; };
    v.finishTime->lowerBoundNs = trim(v.finishTime->lowerBoundNs);
    v.finishTime->upperBoundNs = trim(v.finishTime->upperBoundNs);
    v.finishTime->estimatedNs = trim(v.finishTime->estimatedNs);
    v.finishTimeMs = static_cast<std::uint32_t>(v.finishTime->estimatedNs / 1000000u);
  } else if (state->finishTimeMs) {
    v.finishTimeMs = *state->finishTimeMs >= kPrestartMs ? *state->finishTimeMs - kPrestartMs : 0u;
  }
  v.respawnCount = state->respawnCount;
  v.stuntsScore = state->stuntsScore;
  view = v;
}

// The simulation only reproduces the game's numbers under a fixed
// floating-point environment, and the scope that establishes one also resets the
// game's random sequence and restores both afterwards.
//
// It is taken per advance rather than held, which is what ForeverValidator's own
// sandbox does around every AdvanceTicks: the scope is a process-wide singleton,
// so a World that kept one would stop every other World from simulating at all,
// and the editor and a searching plugin each own one. Entering and leaving it
// is cheap -- see DeterministicExecutionScope's constructor in the submodule,
// which does no work at all when the environment it wants is already installed,
// which after the first tick it always is.
//
// `block` is a member rather than a local because AdvanceIncremental takes a
// vector: a local would malloc and free once per tick, and this is called once
// per tick for the whole length of every candidate a search evaluates.
bool World::Impl::Advance(const ReplayControlTick &tick) {
  tmnf::simulation::DeterministicExecutionScope deterministic;
  if (!deterministic.Established()) return false;
  block[0] = tick;
  const bool ok = session->AdvanceIncremental(block, 0u, 1u).result == ReplaySimulationRunResult::Success;
  return deterministic.Restore() && ok;
}

World::World() : impl_(new Impl()) {}
World::~World() = default;

std::unique_ptr<World> World::Open(const std::string &packs, const void *challenge_bytes, std::size_t size,
                                   std::string *diagnostic) {
  const auto fail = [diagnostic](const char *why) -> std::unique_ptr<World> {
    if (diagnostic) *diagnostic = why;
    return nullptr;
  };
  if (!challenge_bytes || size == 0) return fail("no track bytes");

  std::unique_ptr<World> world(new World());
  Impl &m = *world->impl_;

  std::vector<std::uint8_t> packlist;
  if (!ReadFile(packs + "/packlist.dat", &packlist)) return fail("could not read packlist.dat");
  if (!m.keys.LoadFromMemory(packlist.data(), packlist.size()))
    return fail("could not authenticate packlist.dat");

  ReplayFile replay;
  if (ReadChallengeBytes(static_cast<const std::uint8_t *>(challenge_bytes), size, &replay) !=
      ReplayFileReadError::Success)
    return fail("could not decode the track");

  ReplayAssetRoute route;
  if (BuildReplayAssetRoute(replay, &route) != ReplayAssetRouteResult::Success)
    return fail("the track's environment or vehicle is unsupported");

  const auto open_repository = [&](std::string_view name) -> std::unique_ptr<ReplayAssetRepository> {
    const std::string pack(name);
    std::vector<std::uint8_t> bytes;
    if (!ReadFile(packs + "/" + pack + ".pak", &bytes)) return nullptr;
    return OpenReplayAssetRepository(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size(), m.keys,
                                     pack.c_str());
  };
  m.map_assets = open_repository(route.mapPackName);
  m.decoration_assets = open_repository(route.decorationPackName);
  if (!m.map_assets || !m.decoration_assets) return fail("could not open the track's packs");

  const std::string vehicle_pack(route.vehiclePackName);
  std::vector<std::uint8_t> vehicle_bytes;
  if (!ReadFile(packs + "/" + vehicle_pack + ".pak", &vehicle_bytes)) return fail("could not read the vehicle pack");
  CPlugFilePack pack;
  if (!pack.OpenFromMemory(reinterpret_cast<const std::byte *>(vehicle_bytes.data()), vehicle_bytes.size(), m.keys,
                           vehicle_pack.c_str()))
    return fail("could not open the vehicle pack");
  std::optional<InstalledVehicleAssetGraph> graph = InstalledVehicleAssetGraph::ResolveFromPack(pack);
  if (!graph) return fail("could not resolve the vehicle asset graph");
  std::optional<DefaultVehiclePackData> vehicle = DefaultVehiclePackArchive::LoadFromPack(pack, *graph);
  std::optional<ReplayVehicleSolidDefinition> solid = DefaultVehicleSolidArchive::LoadFromPack(pack, *graph);
  if (!vehicle || !solid) return fail("could not load the vehicle definitions");
  m.vehicle_sources = ReplayVehicleSourceBundle{std::move(*solid), std::move(vehicle->tuning),
                                                std::move(vehicle->vehicle)};
  if (!m.vehicle_sources.IsComplete()) return fail("the vehicle definitions are incomplete");

  m.session.reset(new ReplaySimulationSession(kSimulationBackend));
  if (m.preload.Preload(replay.MapInput(), *m.map_assets, *m.decoration_assets, *m.session) !=
      ReplayChallengePreloadResult::Success)
    return fail("could not build the track's scene");

  ReplaySimulationDefinitionBuild definition = BuildReplaySimulationDefinition(m.vehicle_sources,
                                                                              m.preload.WaterDefinition());
  if (!definition) return fail("could not build the vehicle simulation");
  m.definition = std::move(definition).Value();
  // Only Stadium's specialised kernels are certified exact; see the field's
  // declaration in the submodule.
  m.definition.optimizedCpuStadiumSpecializationsEnabled = route.vehicleModel == ReplayVehicleModel::StadiumCar;
  m.session->ActivateStaticScene();

  const ReplayChallengeMetadata &meta = replay.ChallengeMetadata();
  m.session->ConfigureReplayRace(meta.playMode.value_or(EChallengePlayMode::Race), meta.isLapRace,
                                 meta.isLapRace ? meta.lapCount : 1u);
  m.map_name = meta.mapName;

  // --- the scene, which never changes once the track is decoded ---
  const std::vector<ReplayStaticCollisionTriangle> &triangles = m.session->StaticCollisionTriangles();
  m.collision.reserve(triangles.size());
  for (const ReplayStaticCollisionTriangle &t : triangles)
    m.collision.push_back({ToVec(t.a), ToVec(t.b), ToVec(t.c)});

  // The car's ellipsoids are authored as a tree of local poses; the renderer
  // wants them in vehicle space, so each is composed with its parent.
  const std::vector<VehicleCollisionShapeEntry> &shapes = m.definition.vehicle.collisionModel.ShapesInArchiveOrder();
  std::vector<GmIso4> absolute;
  absolute.reserve(shapes.size());
  m.car_shape.reserve(shapes.size());
  for (const VehicleCollisionShapeEntry &entry : shapes) {
    GmIso4 pose = entry.shape.localPose;
    if (entry.parentShapeIndex) pose.Mult(absolute[*entry.parentShapeIndex]);
    absolute.push_back(pose);
    GmVec3 center;
    center.SetMult(entry.shape.localBounds.center, pose);
    GmQuat rotation;
    rotation.Set(pose.rotation);
    rotation.Normalize();
    const GmVec3 &half = entry.shape.localBounds.halfExtents;
    m.car_shape.push_back({rotation.x, rotation.y, rotation.z, rotation.w, ToVec(center), ToVec(half)});
  }

  m.render = m.session->StaticRenderScene();
  if (!m.render) return fail("could not build the track's visual scene");

  m.view.mapEnvironment = static_cast<forevervalidator::MapEnvironment>(route.mapEnvironment);
  m.view.vehicleModel = static_cast<forevervalidator::VehicleModel>(route.vehicleModel);

  // --- run the countdown, once, and keep where it left off ---
  const TmnfInput neutral{};
  m.time_ms = kTickMs;
  {
    tmnf::simulation::DeterministicExecutionScope deterministic;
    if (!deterministic.Established()) return fail("deterministic execution mode is unavailable");
    const bool started = m.session->StartIncremental(m.definition, MakeTick(m.time_ms, neutral, false), 0u) ==
                         ReplaySimulationRunResult::Success;
    if (!deterministic.Restore() || !started) return fail("the simulation could not start");
  }
  for (std::uint32_t i = 1; i < kPrestartTicks; ++i) {
    m.time_ms += kTickMs;
    if (!m.Advance(MakeTick(m.time_ms, neutral, false))) return fail("the countdown could not be simulated");
  }
  m.ReadView();

  m.start = world->Capture();
  if (!m.start) return fail("the starting state could not be captured");
  return world;
}

const std::string &World::MapName() const noexcept { return impl_->map_name; }
const std::vector<Triangle> &World::Collision() const noexcept { return impl_->collision; }
const std::vector<Ellipsoid> &World::CarShape() const noexcept { return impl_->car_shape; }
RenderSceneHandle World::Render() const noexcept { return impl_->render; }
const State &World::Start() const noexcept { return impl_->start; }
const StateView &World::View() const noexcept { return impl_->view; }

bool World::Restore(const State &state) {
  if (!state.impl_) return false;
  // Straight-line playback never leaves the state it is on, so the common case
  // costs nothing.
  if (impl_->resident == state.impl_.get()) return true;

  ReplaySimulationInstanceClone clone = *state.impl_->clone;
  if (!impl_->session->PrepareRuntimeCloneRestore(clone)) return false;
  impl_->session->RestoreRuntimeClone(std::move(clone));
  impl_->time_ms = state.impl_->time_ms;
  impl_->view = state.impl_->view;
  impl_->resident = state.impl_.get();
  return true;
}

bool World::Step(const TmnfInput &input) {
  impl_->time_ms += kTickMs;
  if (!impl_->Advance(MakeTick(impl_->time_ms, input, input.respawn != 0))) {
    impl_->time_ms -= kTickMs;
    return false;
  }
  // The simulation has moved off whatever state it was restored to.
  impl_->resident = nullptr;
  impl_->ReadView();
  return true;
}

State World::WithEdit(const State &state, const StateEdit &edit) {
  if (!Restore(state)) return State{};
  if (!edit.position && !edit.linearSpeed && !edit.engineOn) return state;

  ReplaySimulationInstanceClone clone = *state.impl_->clone;

  // The body carries three copies of the same frame (the one being written,
  // the one just finished and the scratch one a substep works in) and the next
  // tick reads whichever it is up to. A start has to look the same in all three
  // or the car snaps back on its first step.
  if (edit.position || edit.linearSpeed) {
    CHmsDyna::CHmsStateDyna *frames[] = {&clone.runtime.body.currentState, &clone.runtime.body.writeState,
                                         &clone.runtime.body.tempState};
    for (CHmsDyna::CHmsStateDyna *frame : frames) {
      if (edit.position) frame->position = ToGm(*edit.position);
      if (edit.linearSpeed) {
        frame->linearSpeed = ToGm(*edit.linearSpeed);
        // A correction speed left over from the old position is a push the car
        // never asked for, and a tweaked speed is last tick's answer to a
        // collision that no longer happened.
        frame->linearCorrectionSpeed = GmVec3{};
        frame->tweakedLinearSpeedValid = false;
        frame->tweakedLinearSpeed = GmVec3{};
      }
    }
  }
  // "Engine off" is the integration step the powertrain runs each tick, which
  // BeginRaceSimulation turns on once and nothing turns off again, so clearing
  // it here holds for the rest of the run, and the car coasts.
  if (edit.engineOn) clone.runtime.vehicle.car.integration.integrateEngine = *edit.engineOn;

  if (!impl_->session->PrepareRuntimeCloneRestore(clone)) return State{};
  impl_->session->RestoreRuntimeClone(std::move(clone));
  impl_->resident = nullptr;
  impl_->ReadView();
  return Capture();
}

State World::Capture() const {
  State out;
  std::shared_ptr<const ReplaySimulationInstanceClone> clone = impl_->session->CaptureRuntimeClone();
  if (!clone) return out;
  auto captured = std::make_shared<State::Impl>();
  captured->clone = std::move(clone);
  captured->view = impl_->view;
  captured->time_ms = impl_->time_ms;
  out.impl_ = captured;
  // Taking a snapshot is also the moment the World is known to be sitting on it,
  // so a Restore straight back to it can be skipped.
  impl_->resident = out.impl_.get();
  return out;
}

} // namespace sim
} // namespace tmnf
