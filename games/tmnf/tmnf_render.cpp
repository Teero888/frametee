// Drawing a TrackMania track and car through the engine's 3D primitives.
//
// The engine's 3D path is immediate: every triangle is written into a stream
// once per frame, unlit, with a hard cap a little over sixty thousand and no
// warning when it is passed. There is no retained mesh upload for 3D geometry,
// so the work here is entirely about submitting as few triangles as possible
// and making sure they are the right ones — the track around the camera rather
// than the track around the car, which is what a free or orbiting view needs.

#include "tmnf_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tmnf {
namespace {

// --- track -------------------------------------------------------------------

struct VisibleCell {
  const GridCell *cell = nullptr;
  float distance_sq = 0.f;
};

// Squared distance from a point to a box; zero inside. Cells are ordered by it
// so that when the budget runs out it is the far side of the track that is
// dropped rather than whatever happened to be stored last.
float DistanceSq(const Aabb &box, ft_vec3 p) {
  const float dx = std::max({box.mn.x - p.x, 0.f, p.x - box.mx.x});
  const float dy = std::max({box.mn.y - p.y, 0.f, p.y - box.mx.y});
  const float dz = std::max({box.mn.z - p.z, 0.f, p.z - box.mx.z});
  return dx * dx + dy * dy + dz * dz;
}

std::size_t DrawGrid(const ft_engine_api *api, const std::vector<Triangle> &triangles, const TriangleGrid &grid,
                     const Frustum &frustum, bool have_frustum, ft_vec3 eye, float view_distance, bool backface_cull,
                     float opacity, std::size_t budget) {
  if (triangles.empty() || grid.cells.empty() || budget == 0) return 0;

  static thread_local std::vector<VisibleCell> visible;
  visible.clear();
  visible.reserve(grid.cells.size() / 4u + 8u);

  const float max_distance_sq = view_distance * view_distance;
  for (const GridCell &cell : grid.cells) {
    if (cell.count == 0) continue;
    const float distance_sq = DistanceSq(cell.bounds, eye);
    if (distance_sq > max_distance_sq) continue;
    if (have_frustum && !frustum.Intersects(cell.bounds)) continue;
    visible.push_back(VisibleCell{&cell, distance_sq});
  }

  std::sort(visible.begin(), visible.end(),
            [](const VisibleCell &a, const VisibleCell &b) { return a.distance_sq < b.distance_sq; });

  std::size_t drawn = 0;
  for (const VisibleCell &entry : visible) {
    if (drawn >= budget) break;
    const GridCell &cell = *entry.cell;
    for (std::uint32_t i = 0; i < cell.count && drawn < budget; ++i) {
      const Triangle &tri = triangles[cell.first + i];
      if (backface_cull) {
        // The renderer never culls back faces itself, so half the triangles of
        // a solid track are drawn purely to be overwritten. Winding was made
        // consistent at load, which is what makes this safe.
        const ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
        if (Dot(face, Sub(eye, tri.a)) <= 0.f) continue;
      }
      ft_color color = UnpackColor(tri.color);
      color.a *= opacity;
      api->draw_triangle3(tri.a, tri.b, tri.c, color);
      ++drawn;
    }
  }
  return drawn;
}

void RenderTrack(ft_game *game, const ft_render_frame *frame) {
  const ft_engine_api *api = game->engine;
  const ft_level *level = frame->level;
  if (!level || !api->draw_triangle3) return;

  const ft_camera &camera = frame->state.camera;
  const Frustum frustum = Frustum::FromViewProj(camera.view_proj);
  const bool have_frustum = frustum.Valid();

  // Without a usable camera — the very first frame after a level loads — fall
  // back to the car, which is where the view is about to be.
  ft_vec3 eye = camera.eye;
  if (!have_frustum) {
    eye = frame->world ? ToVec3(frame->world->view.car.position) : ToVec3(level->start.car.position);
  }

  const Settings &settings = game->settings;
  const float opacity = std::clamp(frame->opacity, 0.f, 1.f);

  std::size_t budget = kTriangleBudget;
  budget -= std::min(budget, DrawGrid(api, level->track, level->track_grid, frustum, have_frustum, eye,
                                      settings.view_distance, settings.backface_cull, opacity, budget));

  // The stadium shell is drawn last and only with what the track left over: it
  // is scenery, and losing part of it costs nothing a driver can feel.
  if (settings.draw_background && budget > 0) {
    DrawGrid(api, level->backdrop, level->backdrop_grid, frustum, have_frustum, eye, settings.view_distance * 2.f,
             settings.backface_cull, opacity, budget);
  }
}

// --- the car -----------------------------------------------------------------

// The collision ellipsoids the simulation actually pushes around, drawn over
// the car when asked for. Eight translucent shells is not something to look at
// while driving, but it is exactly what you want when a run is losing time to
// a contact nobody can see.
void DrawCollisionShape(const ft_engine_api *api, const ft_level *level, const CarPose &pose, float opacity) {
  constexpr int kSegments = 10;
  constexpr int kRings = 6;
  const ft_color shell{0.35f, 0.95f, 0.55f, 0.20f * opacity};

  for (const auto &ellipsoid : level->car_shape) {
    const Quat local =
        NormalizeQuat(Quat{ellipsoid.rotationX, ellipsoid.rotationY, ellipsoid.rotationZ, ellipsoid.rotationW});
    const ft_vec3 radii{std::max(0.01f, ellipsoid.radii.x), std::max(0.01f, ellipsoid.radii.y),
                        std::max(0.01f, ellipsoid.radii.z)};
    const ft_vec3 offset = ToVec3(ellipsoid.position);
    const auto place = [&](float phi, float theta) {
      const ft_vec3 unit{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
      return Add(pose.position, Rotate(pose.rotation, Add(offset, Rotate(local, Mul(unit, radii)))));
    };

    for (int r = 0; r < kRings; ++r) {
      const float p0 = kPi * r / kRings;
      const float p1 = kPi * (r + 1) / kRings;
      for (int s = 0; s < kSegments; ++s) {
        const float t0 = 2.f * kPi * s / kSegments;
        const float t1 = 2.f * kPi * (s + 1) / kSegments;
        api->draw_triangle3(place(p0, t0), place(p1, t0), place(p0, t1), shell);
        api->draw_triangle3(place(p0, t1), place(p1, t0), place(p1, t1), shell);
      }
    }
  }
}

void RenderCar(ft_game *game, const ft_render_frame *frame) {
  const ft_engine_api *api = game->engine;
  if (!frame->world || !api->draw_triangle3) return;

  const CarPose pose = InterpolateCar(frame->previous_world, frame->world, frame->alpha);
  const float opacity = std::clamp(frame->opacity, 0.f, 1.f);

  DrawCar(game, frame, pose);

  const ft_level *level = frame->level ? frame->level : game->level;
  if (game->settings.draw_collision && level && !level->car_shape.empty()) {
    DrawCollisionShape(api, level, pose, opacity);
  }

  if (!api->draw_line3) return;

  // Heading and velocity. They only differ while the car is sliding, which is
  // exactly when the difference is worth seeing.
  const ft_vec3 origin = Add(pose.position, Scale(pose.up, 0.2f));
  api->draw_line3(origin, Add(origin, Scale(pose.forward, 4.f)), ft_color{1.f, 0.86f, 0.32f, 0.85f * opacity}, 0.08f);
  if (LengthSq(pose.velocity) > 0.5f) {
    api->draw_line3(origin, Add(origin, Scale(pose.velocity, 0.25f)), ft_color{0.42f, 0.85f, 1.f, 0.85f * opacity},
                    0.07f);
  }
}

} // namespace

void Render(ft_game *game, const ft_render_frame *frame) {
  if (!game || !frame || !game->engine || game->headless) return;

  switch (frame->pass) {
  case FT_PASS_LEVEL_BACKGROUND:
    // Level passes are drawn once for the whole frame, whatever the editor has
    // in its timeline; the track does not belong to any one world.
    RenderTrack(game, frame);
    break;
  case FT_PASS_ENTITIES: RenderCar(game, frame); break;
  default: break;
  }
}

} // namespace tmnf
