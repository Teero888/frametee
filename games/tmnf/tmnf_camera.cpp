// Placing the viewport camera.
//
// ForeverValidator decodes the original game's race camera out of the
// installed packs and exposes it as a stateful controller, so the chase view
// here is not an approximation of TrackMania's camera; it is TrackMania's
// camera, fed the same vehicle signals the game feeds it. That matters for a
// TAS tool: judging a jump or a wall ride against a hand-rolled camera means
// judging it against something the game never showed.
//
// Two things the engine cannot carry are dropped on the way out. Its view
// matrix is built with a fixed world up, so camera roll is discarded, and it
// derives its own field of view, so the lens is ignored. Everything else (the
// position, the heading, the way the view swings out under acceleration and
// settles again) comes through.

#include "tmnf_internal.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace tmnf {
namespace {

enum CameraModeIndex { CAMERA_RACE = 0, CAMERA_ORBIT, CAMERA_MODE_COUNT };

// How far behind and above the car the view sits when the game's own camera
// could not be decoded. Rough, but it keeps a track drivable rather than
// leaving the view stuck at the origin.
constexpr float kFallbackDistance = 8.5f;
constexpr float kFallbackHeight = 3.0f;
constexpr float kFallbackLookAhead = 5.f;

fv::camera::RaceCameraVehicleState VehicleState(const ft_world *world, const CarPose &pose, std::uint32_t time_ms) {
  fv::camera::RaceCameraVehicleState vehicle;
  vehicle.targetId = 1u;
  vehicle.timeMs = time_ms;
  vehicle.transform.position = fv::camera::Vector3{pose.position.x, pose.position.y, pose.position.z};
  // The camera controller is TrackMania code and expects the original
  // row-vector quaternion, while CarPose is kept in renderer convention.
  const Quat game_rotation = Conjugate(pose.rotation);
  vehicle.transform.rotation =
      fv::camera::Quaternion{game_rotation.w, game_rotation.x, game_rotation.y, game_rotation.z};
  vehicle.linearSpeed = fv::camera::Vector3{pose.velocity.x, pose.velocity.y, pose.velocity.z};

  const auto &view = world->view;
  const auto &car = view.car;
  vehicle.signedSpeed = car.signedSpeed;
  vehicle.steering = view.steering;
  vehicle.accelerate = view.accelerate;
  vehicle.brake = view.brake;
  vehicle.turbo = car.turbo;
  vehicle.cameraFlightTransition = car.cameraFlightTransition;
  vehicle.burning = car.burning;
  vehicle.gearChanged = car.gearChanged;
  vehicle.isVehicleCar = true;
  vehicle.wheelContact = car.wheelContact;
  vehicle.wheelHasSurface = car.wheelHasSurface;
  vehicle.cameraSupportUp =
      fv::camera::Vector3{car.cameraSupportUp.x, car.cameraSupportUp.y, car.cameraSupportUp.z};
  return vehicle;
}

// The camera's own +Z, which is where it looks: the controllers seed themselves
// with the car's rotation and place the eye at the car's -Z, and the car looks
// down +Z.
ft_vec3 ForwardOf(const fv::camera::Quaternion &q) {
  const Quat renderer_rotation = Conjugate(Quat{q.x, q.y, q.z, q.w});
  return Rotate(renderer_rotation, ft_vec3{0.f, 0.f, 1.f});
}

bool EnsureSession(ft_game *game) {
  if (!game->race_cameras) return false;
  constexpr fv::camera::RaceCameraProfile kProfile = fv::camera::RaceCameraProfile::Race;
  if (!game->race_cameras->HasProfile(kProfile)) return false;

  if (game->race_session) return true;
  try {
    game->race_session = std::make_unique<fv::camera::RaceCameraSession>(*game->race_cameras, kProfile);
  } catch (const std::exception &error) {
    Log(game, FT_LOG_WARN, "Could not start the %s camera: %s", kCameraModes[CAMERA_RACE].display_name,
        error.what());
    game->race_session.reset();
    return false;
  }
  game->race_session_started = false;
  return true;
}

// A plain chase view, used only when the packs did not yield the real camera.
// It is deliberately built from a heading rather than the car's own forward:
// the engine rebuilds its view with a fixed world up, so a rolled basis would
// only produce a camera that swings for no visible reason.
bool FallbackCamera(const CarPose &pose, ft_camera *inout) {
  const ft_vec3 heading = FlattenY(LengthSq(pose.velocity) > 9.f ? pose.velocity : pose.forward, pose.forward);
  const ft_vec3 focus = Add(pose.position, ft_vec3{0.f, 1.f, 0.f});

  inout->target = Add(focus, Scale(heading, kFallbackLookAhead));
  inout->eye = Add(Sub(focus, Scale(heading, kFallbackDistance)), ft_vec3{0.f, kFallbackHeight, 0.f});
  return true;
}

} // namespace

const ft_camera_mode kCameraModes[] = {
    {"chase", "Chase", "The game's own race camera", FT_CAMERA_MODE_DIRECTED},
    {"orbit", "Orbit", "Drag to turn around the car, scroll to pull back", FT_CAMERA_MODE_FREE},
};
const std::uint32_t kCameraModeCount = CAMERA_MODE_COUNT;

void CameraReset(ft_game *game) {
  if (!game) return;
  game->race_session.reset();
  game->race_session_time_ms = 0u;
  game->race_session_started = false;

  game->race_cameras.reset();
  if (game->packs.empty() || !game->level) return;

  const char *environment = fv::MapEnvironmentName(game->level->start.mapEnvironment);
  if (!environment || !*environment) return;

  auto loaded = fv::LoadInstalledRaceCameraEnvironment(game->packs, environment);
  if (!loaded) {
    Log(game, FT_LOG_WARN, "No race camera for the %s environment; using a plain chase view.", environment);
    return;
  }
  game->race_cameras.emplace(std::move(loaded).Value());
  Log(game, FT_LOG_INFO, "Loaded the %s race camera.", environment);
}

bool CameraUpdate(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  if (!game || !frame || !inout || !frame->world) return false;
  // The orbit view is the user's. Returning false leaves it alone, and the
  // engine keeps it pointed at the car through the position property.
  if (frame->mode >= CAMERA_ORBIT) return false;

  const CarPose pose = InterpolateCar(frame->previous_world, frame->world, frame->alpha);

  if (!EnsureSession(game)) return FallbackCamera(pose, inout);

  // The controllers integrate, so they want time moving forward in small steps.
  // Scrubbing the timeline does neither, and a seek is answered by restarting
  // the camera where the car now is rather than by sweeping it across the map.
  const float alpha = std::clamp(frame->alpha, 0.f, 1.f);
  std::uint64_t time_ms = frame->world->view.timeMs + static_cast<std::uint64_t>(alpha * kTickMs);
  if (game->race_session_started && time_ms < game->race_session_time_ms) {
    game->race_session_started = false;
  } else if (game->race_session_started && time_ms - game->race_session_time_ms > 200u) {
    game->race_session_started = false;
  }
  if (time_ms > 0xFFFFFFFFull) time_ms = 0xFFFFFFFFull;

  const fv::camera::RaceCameraVehicleState vehicle =
      VehicleState(frame->world, pose, static_cast<std::uint32_t>(time_ms));

  if (!game->race_session_started) {
    game->race_session->Reset(vehicle);
    game->race_session_started = true;
  }
  game->race_session_time_ms = time_ms;

  fv::camera::RaceCameraQuery query;
  query.vehicle = vehicle;
  // Keeping the view out of the scenery is part of what the original camera
  // does, and it needs the track to do it.
  const ft_level *level = game->level;
  if (level) {
    query.segmentCollision =
        [level](const fv::camera::SegmentQuery &segment) -> std::optional<fv::camera::SegmentHit> {
      float fraction = 1.f;
      if (!SegmentHit(level, ft_vec3{segment.start.x, segment.start.y, segment.start.z},
                      ft_vec3{segment.end.x, segment.end.y, segment.end.z}, &fraction)) {
        return std::nullopt;
      }
      return fv::camera::SegmentHit{fraction};
    };
  }

  const fv::camera::RaceCameraOutput output = game->race_session->Evaluate(query);
  const ft_vec3 eye{output.transform.position.x, output.transform.position.y, output.transform.position.z};
  const ft_vec3 forward = ForwardOf(output.transform.rotation);

  inout->eye = eye;
  // The engine only reads an eye and a target and rebuilds the rest, so the
  // heading is handed over as a point far enough along it that the direction
  // survives being turned back into yaw and pitch.
  inout->target = Add(eye, Scale(forward, 20.f));
  return true;
}

} // namespace tmnf
