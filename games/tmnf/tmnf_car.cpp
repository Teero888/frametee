// The car.
//
// The authored model comes out of the installed packs (see
// tmnf_vehicle_model.cpp) and is what is drawn whenever it could be read. The
// modelled car below stands in when it could not — no packs, a pack the decoder
// does not recognise — so the editor still shows something car-shaped rather
// than nothing at all. It is built to the dimensions the simulation actually
// uses, which come out of the collision ellipsoids.
//
// Either mesh lives in vehicle space and is transformed per frame. Wheels are
// separate so the front pair can be turned with the steering, and each of the
// four is tinted by whether it is on the ground and whether it is gripping,
// which is the thing worth seeing while authoring a run.

#include "tmnf_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tmnf {
namespace {

// Vehicle space: +X right, +Y up, +Z forward, origin on the ground between the
// wheels. These match the collision ellipsoids the sandbox reports.
constexpr float kWheelRadius = 0.364f;
constexpr float kWheelHalfWidth = 0.182f;
constexpr float kFrontAxleZ = 1.782f;
constexpr float kRearAxleZ = -1.2055f;
constexpr float kFrontTrackX = 0.863f;
constexpr float kRearTrackX = 0.885f;
constexpr float kAxleY = 0.3525f;

// Full lock at the wheel. The simulation's steering is a normalised axis, and
// this is what the visible wheel does with it.
constexpr float kMaxSteerAngle = 0.38f;

struct Face {
  ft_vec3 a, b, c;
  ft_vec3 normal;
  ft_color color;
};

// Adds a quad wound so its normal points out of the solid.
void Quad(std::vector<Face> &out, ft_vec3 a, ft_vec3 b, ft_vec3 c, ft_vec3 d, ft_color color) {
  const ft_vec3 normal = Normalize(Cross(Sub(b, a), Sub(c, a)));
  out.push_back(Face{a, b, c, normal, color});
  out.push_back(Face{a, c, d, normal, color});
}

// A box given by two opposite corners.
void Box(std::vector<Face> &out, ft_vec3 lo, ft_vec3 hi, ft_color color) {
  const ft_vec3 p000{lo.x, lo.y, lo.z}, p100{hi.x, lo.y, lo.z}, p110{hi.x, hi.y, lo.z}, p010{lo.x, hi.y, lo.z};
  const ft_vec3 p001{lo.x, lo.y, hi.z}, p101{hi.x, lo.y, hi.z}, p111{hi.x, hi.y, hi.z}, p011{lo.x, hi.y, hi.z};
  Quad(out, p001, p101, p111, p011, color); // +Z
  Quad(out, p100, p000, p010, p110, color); // -Z
  Quad(out, p101, p100, p110, p111, color); // +X
  Quad(out, p000, p001, p011, p010, color); // -X
  Quad(out, p010, p011, p111, p110, color); // +Y
  Quad(out, p000, p100, p101, p001, color); // -Y
}

// A box whose two ends have different widths and heights: the nose, the tub and
// the sidepods are all tapers rather than boxes, and a car built only out of
// boxes reads as a brick.
void Taper(std::vector<Face> &out, float z0, float half_x0, float y0_lo, float y0_hi, float z1, float half_x1,
           float y1_lo, float y1_hi, ft_color color) {
  const ft_vec3 a0{-half_x0, y0_lo, z0}, a1{half_x0, y0_lo, z0}, a2{half_x0, y0_hi, z0}, a3{-half_x0, y0_hi, z0};
  const ft_vec3 b0{-half_x1, y1_lo, z1}, b1{half_x1, y1_lo, z1}, b2{half_x1, y1_hi, z1}, b3{-half_x1, y1_hi, z1};
  Quad(out, b0, b1, b2, b3, color); // the z1 end
  Quad(out, a1, a0, a3, a2, color); // the z0 end
  Quad(out, a1, b1, b2, a2, color); // +X
  Quad(out, b0, a0, a3, b3, color); // -X
  Quad(out, a3, b3, b2, a2, color); // top
  Quad(out, a0, b0, b1, a1, color); // bottom
}

// One wheel, in its own space with the axle along X and the origin at the hub.
std::vector<Face> BuildWheel() {
  std::vector<Face> faces;
  constexpr int kSegments = 16;
  const ft_color tread{0.10f, 0.10f, 0.11f, 1.f};
  const ft_color sidewall{0.14f, 0.14f, 0.15f, 1.f};
  const ft_color rim{0.62f, 0.64f, 0.68f, 1.f};

  for (int i = 0; i < kSegments; ++i) {
    const float t0 = 2.f * kPi * static_cast<float>(i) / kSegments;
    const float t1 = 2.f * kPi * static_cast<float>(i + 1) / kSegments;
    const float y0 = std::cos(t0) * kWheelRadius, z0 = std::sin(t0) * kWheelRadius;
    const float y1 = std::cos(t1) * kWheelRadius, z1 = std::sin(t1) * kWheelRadius;

    // Tread.
    Quad(faces, ft_vec3{-kWheelHalfWidth, y0, z0}, ft_vec3{kWheelHalfWidth, y0, z0}, ft_vec3{kWheelHalfWidth, y1, z1},
         ft_vec3{-kWheelHalfWidth, y1, z1}, tread);

    // Sidewalls, with a lighter rim in the middle so a turning wheel reads as
    // turning rather than as a black disc.
    const float hub = kWheelRadius * 0.45f;
    const float hy0 = std::cos(t0) * hub, hz0 = std::sin(t0) * hub;
    const float hy1 = std::cos(t1) * hub, hz1 = std::sin(t1) * hub;
    for (int side = 0; side < 2; ++side) {
      const float x = side ? kWheelHalfWidth : -kWheelHalfWidth;
      const ft_vec3 normal{side ? 1.f : -1.f, 0.f, 0.f};
      const ft_vec3 outer0{x, y0, z0}, outer1{x, y1, z1};
      const ft_vec3 inner0{x, hy0, hz0}, inner1{x, hy1, hz1};
      const ft_vec3 centre{x, 0.f, 0.f};
      if (side) {
        faces.push_back(Face{inner0, outer0, outer1, normal, sidewall});
        faces.push_back(Face{inner0, outer1, inner1, normal, sidewall});
        faces.push_back(Face{centre, inner0, inner1, normal, rim});
      } else {
        faces.push_back(Face{outer0, inner0, outer1, normal, sidewall});
        faces.push_back(Face{outer1, inner0, inner1, normal, sidewall});
        faces.push_back(Face{inner0, centre, inner1, normal, rim});
      }
    }
  }
  return faces;
}

// The body, in vehicle space. An open-wheeler: front wing, tapered nose,
// cockpit tub with a driver in it, sidepods, engine cover and a rear wing.
std::vector<Face> BuildBody(ft_color livery) {
  std::vector<Face> f;
  const ft_color dark{0.13f, 0.13f, 0.15f, 1.f};
  const ft_color trim{0.22f, 0.22f, 0.25f, 1.f};
  const ft_color glass{0.10f, 0.13f, 0.18f, 1.f};
  const ft_color helmet{0.92f, 0.92f, 0.94f, 1.f};
  const ft_color visor{0.10f, 0.12f, 0.16f, 1.f};

  // Front wing and its endplates.
  Box(f, ft_vec3{-0.95f, 0.10f, 1.86f}, ft_vec3{0.95f, 0.17f, 2.22f}, dark);
  Box(f, ft_vec3{-1.00f, 0.08f, 1.84f}, ft_vec3{-0.90f, 0.42f, 2.24f}, trim);
  Box(f, ft_vec3{0.90f, 0.08f, 1.84f}, ft_vec3{1.00f, 0.42f, 2.24f}, trim);

  // Nose, from the wing back to the tub.
  Taper(f, 2.00f, 0.13f, 0.20f, 0.34f, 1.20f, 0.30f, 0.24f, 0.52f, livery);
  Taper(f, 1.20f, 0.30f, 0.24f, 0.52f, 0.60f, 0.42f, 0.22f, 0.60f, livery);

  // Cockpit tub.
  Taper(f, 0.60f, 0.42f, 0.22f, 0.60f, -0.40f, 0.44f, 0.20f, 0.62f, livery);
  // The opening the driver sits in.
  Box(f, ft_vec3{-0.32f, 0.58f, -0.30f}, ft_vec3{0.32f, 0.63f, 0.44f}, glass);

  // Sidepods, wide at the back and drawn in towards the rear axle.
  Taper(f, 0.55f, 0.46f, 0.20f, 0.48f, -0.20f, 0.92f, 0.16f, 0.56f, livery);
  Taper(f, -0.20f, 0.92f, 0.16f, 0.56f, -1.15f, 0.72f, 0.16f, 0.52f, livery);

  // Engine cover and the roll hoop over the driver's head.
  Taper(f, -0.30f, 0.36f, 0.20f, 0.78f, -1.30f, 0.26f, 0.18f, 0.62f, livery);
  Box(f, ft_vec3{-0.16f, 0.62f, -0.46f}, ft_vec3{0.16f, 0.98f, -0.24f}, trim);

  // Driver: shoulders, helmet and visor.
  Box(f, ft_vec3{-0.26f, 0.50f, -0.28f}, ft_vec3{0.26f, 0.68f, 0.06f}, dark);
  Box(f, ft_vec3{-0.15f, 0.66f, -0.24f}, ft_vec3{0.15f, 0.92f, 0.06f}, helmet);
  Box(f, ft_vec3{-0.13f, 0.72f, 0.05f}, ft_vec3{0.13f, 0.85f, 0.09f}, visor);

  // Diffuser under the gearbox.
  Taper(f, -1.30f, 0.62f, 0.12f, 0.40f, -1.70f, 0.70f, 0.16f, 0.34f, trim);

  // Rear wing, its pylons and its endplates.
  Box(f, ft_vec3{-0.10f, 0.42f, -1.62f}, ft_vec3{0.10f, 0.92f, -1.48f}, trim);
  Box(f, ft_vec3{-1.00f, 0.92f, -1.68f}, ft_vec3{1.00f, 1.00f, -1.34f}, dark);
  Box(f, ft_vec3{-1.04f, 0.62f, -1.70f}, ft_vec3{-0.96f, 1.04f, -1.32f}, trim);
  Box(f, ft_vec3{0.96f, 0.62f, -1.70f}, ft_vec3{1.04f, 1.04f, -1.32f}, trim);

  return f;
}

struct CarMesh {
  std::vector<Face> body;
  std::vector<Face> wheel;
  ft_color livery{};
  bool built = false;
};

// The livery is the editor's accent colour for the world, so the body is
// rebuilt only when that changes — which is to say almost never.
const CarMesh &Mesh(ft_color livery) {
  static CarMesh mesh;
  if (!mesh.built || mesh.livery.r != livery.r || mesh.livery.g != livery.g || mesh.livery.b != livery.b) {
    mesh.body = BuildBody(livery);
    if (mesh.wheel.empty()) mesh.wheel = BuildWheel();
    mesh.livery = livery;
    mesh.built = true;
  }
  return mesh;
}

ft_color Lit(ft_color base, ft_vec3 normal, float opacity) {
  const float sun = std::max(0.f, Dot(normal, kSunDirection));
  const float sky = 0.5f + 0.5f * normal.y;
  const float light = 0.32f + 0.24f * sky + 0.52f * sun;
  return ft_color{base.r * light, base.g * light, base.b * light, base.a * opacity};
}

void Submit(const ft_engine_api *api, const std::vector<Face> &faces, const CarPose &pose, ft_vec3 offset,
            const Quat *local_rotation, const ft_color *override_color, float opacity) {
  // Vehicle space to world: the car's own basis, then its position.
  const auto place = [&](ft_vec3 p) {
    if (local_rotation) p = Rotate(*local_rotation, p);
    p = Add(p, offset);
    return Add(pose.position, Rotate(pose.rotation, p));
  };
  const auto direction = [&](ft_vec3 n) {
    if (local_rotation) n = Rotate(*local_rotation, n);
    return Rotate(pose.rotation, n);
  };

  for (const Face &face : faces) {
    const ft_color base = override_color ? *override_color : face.color;
    api->draw_triangle3(place(face.a), place(face.b), place(face.c), Lit(base, direction(face.normal), opacity));
  }
}

// How a wheel's state shows on it. The authored model already has its own
// colours, so its wheels are tinted rather than repainted; the modelled car has
// nothing worth keeping and is painted outright.
ft_color WheelStateColor(const fve::PhysicsSandboxCarState &car, std::size_t wheel, bool *replace) {
  constexpr ft_color kTyre{0.12f, 0.12f, 0.13f, 1.f};
  constexpr ft_color kAirborne{0.42f, 0.46f, 0.55f, 1.f};
  constexpr ft_color kSliding{0.95f, 0.42f, 0.12f, 1.f};
  if (!car.wheelContact[wheel]) {
    *replace = true;
    return kAirborne;
  }
  if (car.wheelSliding[wheel]) {
    *replace = true;
    return kSliding;
  }
  *replace = false;
  return kTyre;
}

// The authored model, drawn part by part so the front wheels can be turned.
void DrawAuthored(const ft_engine_api *api, const VehicleModel &model, const ft_render_frame *frame,
                  const CarPose &pose, const Quat &steered, ft_color livery, float opacity) {
  const auto &car = frame->world->view.car;

  // The four wheels are turned about their own hubs and tinted by their state;
  // everything else is the body and is drawn where it was authored.
  ft_color tint[VEHICLE_PART_COUNT];
  bool replace[VEHICLE_PART_COUNT] = {};
  for (std::size_t i = 0; i < 4; ++i) tint[i] = WheelStateColor(car, i, &replace[i]);

  const Quat straight{};
  for (const VehicleFace &face : model.faces) {
    const bool wheel = IsWheelPart(face.part);
    const bool front = face.part == VEHICLE_PART_WHEEL_FL || face.part == VEHICLE_PART_WHEEL_FR;
    const Quat &local = wheel && front ? steered : straight;

    const auto place = [&](ft_vec3 p) {
      if (wheel) p = Add(Rotate(local, p), model.hub[face.part]);
      return Add(pose.position, Rotate(pose.rotation, p));
    };
    ft_vec3 normal = face.normal;
    if (wheel) normal = Rotate(local, normal);
    normal = Rotate(pose.rotation, normal);

    // Body panels come out of the pack unpainted, and the editor's colour for
    // this world is what they are painted with; the wheels keep the black they
    // were decoded with and only carry their state on top.
    // A panel that carries its own picture is already painted; only the parts
    // that came back bare take the editor's colour for this world.
    const bool painted = face.layer != kNoTextureLayer;
    ft_color color = wheel || painted
                         ? face.color
                         : ft_color{face.color.r * livery.r, face.color.g * livery.g, face.color.b * livery.b,
                                    face.color.a};
    if (wheel) {
      const ft_color state = tint[face.part];
      color = replace[face.part] ? state : MixColor(color, state, 0.35f);
    }
    const ft_color lit = Lit(color, normal, opacity);
    if (painted && api->draw_triangle3_textured) {
      api->draw_triangle3_textured(place(face.a), place(face.b), place(face.c), face.uv[0], face.uv[1], face.uv[2],
                                   face.layer, lit);
    } else {
      api->draw_triangle3(place(face.a), place(face.b), place(face.c), lit);
    }
  }
}

} // namespace

void DrawCar(ft_game *game, const ft_render_frame *frame, const CarPose &pose) {
  const ft_engine_api *api = game->engine;
  if (!api->draw_triangle3 || !frame->world) return;

  const float opacity = std::clamp(frame->opacity, 0.f, 1.f);
  const auto &view = frame->world->view;

  // Front wheels turn with the steering axis; the rears stay put.
  //
  // The axis runs negative to the left, and steering left turns the car through
  // a positive angle about its own up axis, so the wheel's angle is the
  // negation of the axis. Getting this backwards points the wheels out of the
  // corner, which looks fine standing still and wrong the moment it matters.
  const float steer = -std::clamp(view.steering, -1.f, 1.f) * kMaxSteerAngle;
  const Quat steered{0.f, std::sin(steer * 0.5f), 0.f, std::cos(steer * 0.5f)};

  const ft_color livery = frame->accent.a > 0.01f ? frame->accent : ft_color{0.20f, 0.58f, 1.f, 1.f};

  if (game->vehicle.loaded) {
    DrawAuthored(api, game->vehicle, frame, pose, steered, livery, opacity);
    return;
  }

  const CarMesh &mesh = Mesh(livery);
  Submit(api, mesh.body, pose, ft_vec3{0.f, 0.f, 0.f}, nullptr, nullptr, opacity);

  const Quat straight{};
  struct Wheel {
    ft_vec3 hub;
    bool front;
  };
  // In the order the simulation reports contact and sliding in.
  const Wheel wheels[4] = {
      {ft_vec3{-kFrontTrackX, kAxleY, kFrontAxleZ}, true}, // front left
      {ft_vec3{kFrontTrackX, kAxleY, kFrontAxleZ}, true},  // front right
      {ft_vec3{kRearTrackX, kAxleY, kRearAxleZ}, false},   // rear right
      {ft_vec3{-kRearTrackX, kAxleY, kRearAxleZ}, false},  // rear left
  };

  for (std::size_t i = 0; i < 4; ++i) {
    bool replace = false;
    const ft_color color = WheelStateColor(view.car, i, &replace);
    Submit(api, mesh.wheel, pose, wheels[i].hub, wheels[i].front ? &steered : &straight, &color, opacity);
  }
}

} // namespace tmnf
