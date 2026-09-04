// Drawing a TrackMania track and car through the engine's 3D primitives.
//
// The engine's 3D path is immediate: every triangle is written into a stream
// once per frame, unlit, with a hard cap a little over sixty thousand and no
// warning when it is passed. There is no retained mesh upload for 3D geometry,
// so the work here is entirely about submitting as few triangles as possible
// and making sure they are the right ones, the track around the camera rather
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

ft_vec2 AnimatedUv(ft_vec2 uv, const TextureAnimation &animation, std::int32_t tick) {
  if (animation.kind == TextureAnimationKind::SpriteSheet && animation.frame_count > 1u &&
      animation.columns > 0u && animation.rows > 0u) {
    const double position = std::floor(static_cast<double>(tick) * animation.frames_per_tick);
    std::int64_t frame = static_cast<std::int64_t>(position) % animation.frame_count;
    if (frame < 0) frame += animation.frame_count;
    const std::uint32_t column = static_cast<std::uint32_t>(frame) % animation.columns;
    const std::uint32_t row = static_cast<std::uint32_t>(frame) / animation.columns;
    return ft_vec2{(static_cast<float>(column) + std::clamp(uv.x, 0.f, 1.f)) / animation.columns,
                   (static_cast<float>(row) + std::clamp(uv.y, 0.f, 1.f)) / animation.rows};
  }
  return uv;
}

std::uint32_t StartLightFrame(std::int32_t tick) {
  // The hidden physics pre-roll ends at timeline tick zero. Negative ticks,
  // when a caller exposes that pre-roll, show the authored red/amber phases;
  // a normal run starts and remains on green instead of cycling mid-race.
  return tick < -100 ? 2u : tick < 0 ? 1u : 0u;
}

std::uint32_t AnimatedLayer(std::uint32_t layer, const TextureAnimation &animation, std::int32_t tick) {
  if (animation.kind == TextureAnimationKind::StartLights && animation.first_layer != kNoTextureLayer)
    return animation.first_layer + StartLightFrame(tick);
  return layer;
}

// Where the level is being looked at from. A perspective camera is a point, and
// every triangle faces it from a slightly different direction; an orthographic
// one is a direction, shared by every ray in the frame. Using the eye as a
// point under the plan view culls walls at the edges of the view that are
// plainly facing the viewer, because the eye sits above the middle of it.
struct ViewPoint {
  ft_vec3 eye;           // orders the work nearest first, either way
  ft_vec3 toward_viewer; // orthographic only
  bool orthographic = false;

  // The direction a front face at `p` has to point in to be worth drawing.
  ft_vec3 TowardViewerFrom(ft_vec3 p) const { return orthographic ? toward_viewer : Sub(eye, p); }
};

std::size_t DrawGrid(const ft_engine_api *api, const std::vector<Triangle> &triangles, const TriangleGrid &grid,
                     const Frustum &frustum, bool have_frustum, const ViewPoint &view, bool backface_cull,
                     float opacity, std::int32_t tick, std::size_t budget) {
  if (triangles.empty() || grid.cells.empty() || budget == 0) return 0;

  static thread_local std::vector<VisibleCell> visible;
  visible.clear();
  visible.reserve(grid.cells.size() / 4u + 8u);

  // Only what is off screen is dropped. Distance orders the work rather than
  // refusing it: a track is a thing you look across, and cutting it off at a
  // radius takes the far half of a stadium away while it is being looked at.
  // What keeps the frame affordable is the budget below, spent nearest first.
  for (const GridCell &cell : grid.cells) {
    if (cell.count == 0) continue;
    if (have_frustum && !frustum.Intersects(cell.bounds)) continue;
    visible.push_back(VisibleCell{&cell, DistanceSq(cell.bounds, view.eye)});
  }

  std::sort(visible.begin(), visible.end(),
            [](const VisibleCell &a, const VisibleCell &b) { return a.distance_sq < b.distance_sq; });

  std::size_t drawn = 0;
  for (const VisibleCell &entry : visible) {
    if (drawn >= budget) break;
    const GridCell &cell = *entry.cell;
    for (std::uint32_t i = 0; i < cell.count && drawn < budget; ++i) {
      const Triangle &tri = triangles[cell.first + i];
      if (backface_cull && !tri.two_sided) {
        // The renderer never culls back faces itself, so half the triangles of
        // a solid track are drawn purely to be overwritten. Winding was made
        // consistent at load, which is what makes this safe, and the surfaces
        // that are sheets rather than solids say so, because a sheet looked at
        // from behind is still meant to be there.
        const ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
        if (Dot(face, view.TowardViewerFrom(tri.a)) <= 0.f) continue;
      }
      ft_color color = UnpackColor(tri.color);
      color.a *= opacity;
      if (tri.layer != kNoTextureLayer && api->draw_triangle3_textured) {
        const ft_vec2 uv0 = AnimatedUv(tri.uv[0], tri.animation, tick);
        const ft_vec2 uv1 = AnimatedUv(tri.uv[1], tri.animation, tick);
        const ft_vec2 uv2 = AnimatedUv(tri.uv[2], tri.animation, tick);
        api->draw_triangle3_textured(tri.a, tri.b, tri.c, uv0, uv1, uv2,
                                     AnimatedLayer(tri.layer, tri.animation, tick), color);
      } else {
        api->draw_triangle3(tri.a, tri.b, tri.c, color);
      }
      ++drawn;
    }
  }
  return drawn;
}

void RenderTrack(ft_game *game, const ft_render_frame *frame) {
  const ft_engine_api *api = game->engine;
  const ft_level *level = frame->level;
  if (!level || !api->draw_triangle3) return;

  // A driver's chosen livery is decoded the first time it is asked for, which
  // adds a page to an array the engine already holds. Settle that before the
  // frame binds it, because handing over the array and then rebuilding it
  // leaves the frame drawing from a texture that has been destroyed.
  if (api->timeline_world_count != nullptr && api->timeline_world_info != nullptr &&
      api->timeline_player_track != nullptr) {
    const std::uint32_t worlds = api->timeline_world_count();
    for (std::uint32_t world = 0u; world < worlds; ++world) {
      ft_timeline_world_info info{};
      info.struct_size = sizeof(info);
      if (!api->timeline_world_info(world, &info)) continue;
      for (std::uint32_t local = 0u; local < info.player_count; ++local)
        SkinLayerFor(game, api->timeline_player_track(world, local));
    }
  }
  if (game->textures.NeedsUpload()) game->textures.Upload(game);

  // The track's own pictures, for the whole of this frame's 3D. A triangle
  // names a page of it and the renderer does the rest; nothing here has to sort
  // or batch by texture.
  if (api->set_texture3) api->set_texture3(game->textures.Texture());

  const ft_camera &camera = frame->state.camera;
  const Frustum frustum = Frustum::FromViewProj(camera.view_proj);
  const bool have_frustum = frustum.Valid();

  // Without a usable camera, the very first frame after a level loads, fall
  // back to the car, which is where the view is about to be. That stand-in is a
  // point, so it is treated as a perspective one whatever the mode says.
  ViewPoint view;
  view.eye = camera.eye;
  view.orthographic = have_frustum && camera.orthographic;
  view.toward_viewer = Scale(camera.forward, -1.f);
  if (!have_frustum) {
    view.eye = frame->world ? ToVec3(frame->world->view.car.position) : ToVec3(level->start.car.position);
  }

  const Settings &settings = game->settings;
  const float opacity = std::clamp(frame->opacity, 0.f, 1.f);

  std::size_t budget = kTriangleBudget;

  // The sky and the ground plane the environment is painted on come first, and
  // out of a reserve of their own. Drawing them from what the track left over
  // meant they were never drawn at all on any track big enough to spend the
  // budget, which is exactly what a stadium is, and why the sky was missing.
  //
  // They are also never back-face culled. A sky is a shell with the viewer
  // inside it, so every one of its triangles faces away; culling it is what
  // made it invisible from the only place it is ever seen from.
  if (settings.draw_background) {
    const std::size_t reserve = std::min(budget, kBackdropBudget);
    budget -= std::min(budget, DrawGrid(api, level->backdrop, level->backdrop_grid, frustum, have_frustum, view,
                                        /*backface_cull=*/false, opacity, frame->tick, reserve));
  }

  budget -= std::min(budget, DrawGrid(api, level->track, level->track_grid, frustum, have_frustum, view,
                                      settings.backface_cull, opacity, frame->tick, budget));

  // Blended surfaces last. Everything solid is resolved by now, so a cut-out or
  // a glow blends over what actually stands behind it rather than over whatever
  // happened to be drawn before it.
  DrawGrid(api, level->translucent, level->translucent_grid, frustum, have_frustum, view, settings.backface_cull,
           opacity, frame->tick, budget);
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
