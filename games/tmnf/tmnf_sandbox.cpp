// Driving the ForeverValidator sandbox from the engine's per-tick worlds.
//
// The sandbox is not a per-world object: it owns one live simulation, and a
// world is a captured state plus the input plan it was produced under. Stepping
// a world therefore restores its state, makes sure the plan installed in the
// sandbox holds the input the engine just handed over, advances a single tick
// and captures the result.
//
// The plan matters more than it looks. ForeverValidator takes inputs as an
// event timeline and compiles it into a control plan spanning the whole
// simulation horizon, so rewriting inputs is linear in the horizon and doing it
// once per tick would be hopeless. Instead a plan is rebuilt only when the
// input the engine supplies disagrees with the plan already installed, and a
// rebuild reads the rest of the authored track ahead in one go. Sequential
// playback and scrubbing then cost no rebuilds at all, and a prediction line
// feeding one held record for hundreds of ticks costs exactly one.

#include "tmnf_internal.h"

#include <cstdio>
#include <cstdlib>
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

fve::PhysicsSandboxInputEvent SwitchEvent(std::int32_t time_ms, fve::PhysicsSandboxInputAction action, bool pressed) {
  fve::PhysicsSandboxInputEvent event;
  event.timeMs = time_ms;
  event.action = action;
  event.value.kind = fve::PhysicsSandboxInputValueKind::Switch;
  event.value.switchState =
      pressed ? fve::PhysicsSandboxSwitchState::Pressed : fve::PhysicsSandboxSwitchState::Released;
  return event;
}

fve::PhysicsSandboxInputEvent SteerEvent(std::int32_t time_ms, std::int32_t value) {
  fve::PhysicsSandboxInputEvent event;
  event.timeMs = time_ms;
  event.action = fve::PhysicsSandboxInputAction::Steer;
  event.value.kind = fve::PhysicsSandboxInputValueKind::Analog;
  event.value.analog = static_cast<AnalogInputState>(
      std::clamp<std::int32_t>(value, -kAnalogInputScale, kAnalogInputScale));
  return event;
}

// Reads whatever the editor holds for a track, so a rebuild installs the whole
// authored run rather than discovering it one disagreement at a time.
bool ReadAuthoredInput(const ft_game *game, std::int32_t track, std::size_t tick, TmnfInput *out) {
  if (!game || !game->engine || !game->engine->get_player_input || track < 0) return false;
  if (tick > static_cast<std::size_t>(INT32_MAX)) return false;
  TmnfInput record{};
  if (!game->engine->get_player_input(track, static_cast<std::int32_t>(tick), &record)) return false;
  *out = record;
  return true;
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
  if (const char *env = std::getenv("FRAMETEE_TMNF_PACKS"); env && DirectoryHasPacks(env)) return env;

  if (api && api->resolve_data_path) {
    char buffer[1024];
    api->resolve_data_path("Packs", buffer, sizeof(buffer));
    if (DirectoryHasPacks(buffer)) return buffer;
  }

  for (const char *candidate : {"games/tmnf/Packs", "../games/tmnf/Packs"})
    if (DirectoryHasPacks(candidate)) return candidate;
  return {};
}

// --- input plans -------------------------------------------------------------

std::vector<fve::PhysicsSandboxInputEvent> BuildInputEvents(const std::vector<fve::PhysicsSandboxInputEvent> &base,
                                                            const InputPlan &plan, std::uint32_t horizon_ms) {
  // The seeded events come first and are never dropped. On a canonical
  // timeline the only one is the race-running switch at time zero, and without
  // it the sandbox never counts a lap, a respawn or a finish.
  std::vector<fve::PhysicsSandboxInputEvent> events(base);
  if (horizon_ms < kTickMs) return events;

  // A tick's input is what the step leaving that tick consumes, and the sandbox
  // consumes every event stamped at or before the time it is advancing to. Tick
  // i therefore carries the timestamp of its own end.
  const std::size_t last_index = horizon_ms / kTickMs - 1u;
  const std::size_t authored = std::min(plan.ticks.size(), last_index + 1u);
  events.reserve(authored / 4u + 8u);

  TmnfInput previous{};
  const auto emit = [&](std::size_t index, const TmnfInput &input) {
    const std::int32_t time_ms = static_cast<std::int32_t>((index + 1u) * kTickMs);
    if (input.accelerate != previous.accelerate)
      events.push_back(SwitchEvent(time_ms, fve::PhysicsSandboxInputAction::Accelerate, input.accelerate != 0));
    if (input.brake != previous.brake)
      events.push_back(SwitchEvent(time_ms, fve::PhysicsSandboxInputAction::Brake, input.brake != 0));
    if (input.steer != previous.steer) events.push_back(SteerEvent(time_ms, input.steer));
    // Respawn is counted per event rather than held, which is also how the game
    // behaves: keeping the key down respawns again on every tick.
    if (input.respawn) events.push_back(SwitchEvent(time_ms, fve::PhysicsSandboxInputAction::Respawn, true));
    previous = input;
  };

  for (std::size_t i = 0; i < authored; ++i) emit(i, plan.ticks[i]);
  if (authored <= last_index) emit(authored, plan.tail);
  return events;
}

// --- lifecycle ---------------------------------------------------------------

bool OpenSandbox(ft_game *game, const void *bytes, std::size_t size, const char *identity) {
  if (!game || !bytes || size == 0) return false;

  auto source = OpenInstalledPackDirectory(game->packs);
  if (!source) {
    Log(game, FT_LOG_ERROR, "Could not open the packs directory '%s'.", game->packs.c_str());
    return false;
  }

  const std::uint32_t horizon =
      std::clamp(static_cast<std::uint32_t>(game->settings.horizon_seconds) * 1000u, kMinHorizonMs, kMaxHorizonMs);

  fve::PhysicsSandboxOptions options;
  options.backend = SimulationBackend::Reference;
  options.tickDurationMs = kTickMs;
  options.prestartDurationMs = kPrestartMs;
  // A standalone challenge has no recorded run to replay, so the timeline is
  // the canonical one the editor authors into.
  options.timelineMode = fve::PhysicsSandboxTimelineMode::Canonical;
  options.simulationHorizonMs = horizon;

  auto created = fve::CreatePhysicsSandbox(std::move(source).Value(), options);
  if (!created) {
    Log(game, FT_LOG_ERROR, "Could not create the sandbox: %s", created.Error().diagnostic.c_str());
    return false;
  }

  game->sandbox.emplace(std::move(created).Value());
  game->horizon_ms = horizon;

  auto loaded = game->sandbox->LoadScenario({static_cast<const std::byte *>(bytes), size}, ReplayIdentity{identity});
  if (!loaded) {
    Log(game, FT_LOG_ERROR, "Could not load the challenge: %s", loaded.Error().diagnostic.c_str());
    game->sandbox.reset();
    return false;
  }

  game->base_events.clear();
  if (auto seeded = game->sandbox->ReadInputs()) game->base_events = std::move(seeded).Value();
  return true;
}

void CloseSandbox(ft_game *game) {
  if (!game) return;
  std::lock_guard<std::mutex> lock(game->mutex);
  game->sandbox.reset();
}

// --- worlds ------------------------------------------------------------------

ft_world *WorldCreate(ft_game *game, const ft_world_desc *desc) {
  if (!game) return nullptr;
  auto *world = new ft_world();
  world->index = desc ? desc->world_index : -1;
  world->track = ResolveTrack(game, world);
  if (desc && desc->level && desc->level->initial) {
    world->state = desc->level->initial;
    world->view = world->state->View();
  }
  return world;
}

void WorldDestroy(ft_game *, ft_world *world) { delete world; }

void WorldCopy(ft_game *, ft_world *dst, const ft_world *src) {
  if (!dst || !src) return;
  // A captured state and a plan are both refcounted handles, so the engine's
  // constant snapshotting only touches counters. The destination keeps its own
  // identity: prediction worlds are index -1 and must stay that way.
  dst->state = src->state;
  dst->view = src->view;
  dst->plan = src->plan;
}

namespace {

// The plan that has to be installed so `record` is what the next tick consumes.
// Everything before the current tick is copied verbatim: the sandbox refuses a
// replacement that rewrites inputs it has already simulated.
InputPlanPtr MakePlan(ft_game *game, ft_world *world, std::size_t tick, const TmnfInput &record) {
  auto plan = std::make_shared<InputPlan>();
  const InputPlan *old = world->plan.get();
  const std::size_t horizon_ticks = game->horizon_ms / kTickMs;

  plan->ticks.reserve(std::min(horizon_ticks, tick + 64u));
  for (std::size_t i = 0; i < tick; ++i) plan->ticks.push_back(old ? old->At(i) : TmnfInput{});
  plan->ticks.push_back(record);

  // Refresh the track binding: a world created before its group was registered
  // has none yet, and a group can be renumbered underneath us.
  if (world->track < 0) world->track = ResolveTrack(game, world);

  if (world->track >= 0) {
    // Read the authored run ahead so an ordinary forward simulation installs a
    // plan once and then agrees with the engine on every later tick.
    for (std::size_t i = tick + 1u; i < horizon_ticks; ++i) {
      TmnfInput next{};
      if (!ReadAuthoredInput(game, world->track, i, &next)) break;
      plan->ticks.push_back(next);
    }
    plan->tail = TmnfInput{};
    // Trailing neutral ticks say nothing the tail does not, and dropping them
    // keeps the event list short.
    while (plan->ticks.size() > tick + 1u && plan->ticks.back() == plan->tail) plan->ticks.pop_back();
  } else {
    // A scratch world — a prediction line, a race-start probe — is fed one
    // record over and over, so assume it keeps being held.
    plan->tail = record;
  }
  return plan;
}

bool InstallPlan(ft_game *game, const InputPlan &plan) {
  auto replaced = game->sandbox->ReplaceInputs(BuildInputEvents(game->base_events, plan, game->horizon_ms));
  if (!replaced) {
    Log(game, FT_LOG_ERROR, "Could not install the input plan: %s", replaced.Error().diagnostic.c_str());
    return false;
  }
  return true;
}

// Every state captured so far carries the horizon it was made with, and
// restoring one across a change costs a full plan rebuild. Growing is therefore
// a last resort rather than something to do a tick at a time.
bool GrowHorizon(ft_game *game, std::size_t needed_ticks) {
  const std::uint64_t needed_ms = static_cast<std::uint64_t>(needed_ticks) * kTickMs;
  if (needed_ms <= game->horizon_ms) return true;
  if (needed_ms > kMaxHorizonMs) {
    Log(game, FT_LOG_WARN, "The run is longer than the %u second simulation limit.", kMaxHorizonMs / 1000u);
    return false;
  }

  std::uint32_t horizon = game->horizon_ms;
  while (horizon < needed_ms) horizon += kHorizonGrowthMs;
  horizon = std::min(horizon, kMaxHorizonMs);

  auto resized = game->sandbox->SetSimulationHorizonMs(horizon);
  if (!resized) {
    Log(game, FT_LOG_ERROR, "Could not extend the simulation horizon: %s", resized.Error().diagnostic.c_str());
    return false;
  }
  game->horizon_ms = horizon;
  Log(game, FT_LOG_INFO, "Extended the simulation horizon to %u seconds.", horizon / 1000u);
  return true;
}

} // namespace

void WorldStep(ft_game *game, ft_world *world, const void *inputs, std::uint32_t player_count) {
  if (!game || !world || !world->state) return;

  TmnfInput record{};
  if (inputs && player_count > 0) std::memcpy(&record, inputs, sizeof(record));

  std::lock_guard<std::mutex> lock(game->mutex);
  if (!game->sandbox) return;

  auto restored = game->sandbox->RestoreState(*world->state);
  if (!restored) return;

  const std::size_t tick = static_cast<std::size_t>(restored.Value().tick);
  if (!GrowHorizon(game, tick + 1u)) return;

  if (!world->plan || world->plan->At(tick) != record) {
    InputPlanPtr plan = MakePlan(game, world, tick, record);
    if (!InstallPlan(game, *plan)) return;
    world->plan = std::move(plan);
  }

  auto advanced = game->sandbox->AdvanceTicks(1u);
  if (!advanced) return;

  auto captured = game->sandbox->CaptureState();
  if (!captured) return;

  world->state = std::move(captured).Value();
  world->view = world->state->View();
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
