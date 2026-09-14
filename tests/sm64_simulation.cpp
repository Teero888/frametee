#include "sm64_bridge.h"
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cglm/cglm.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <filesystem>
#include <vector>

static char error[2048];
static void require(bool ok, const char *what) {
  if (!ok) { std::fprintf(stderr, "%s: %s\n", what, error); std::exit(1); }
}
static sm64_view view(sm64_world *world) {
  sm64_view result{};
  sm64_ft_view(world, &result);
  return result;
}
static void equal(sm64_world *a, sm64_world *b, const char *what) {
  const auto x = view(a), y = view(b);
  require(x.valid == y.valid && x.frame == y.frame && x.action == y.action && x.health == y.health &&
          std::memcmp(x.pos, y.pos, sizeof(x.pos)) == 0 && std::memcmp(x.vel, y.vel, sizeof(x.vel)) == 0, what);
  sm64_camera a_camera{}, b_camera{};
  const bool a_valid = sm64_ft_camera(a, 4.f/3.f, &a_camera);
  const bool b_valid = sm64_ft_camera(b, 4.f/3.f, &b_camera);
  require(a_valid == b_valid && (!a_valid || std::memcmp(&a_camera, &b_camera, sizeof(a_camera)) == 0), what);
}
static sm64_input input(int frame) {
  return {uint16_t(frame % 37 == 0 ? 0x8000 : 0), int8_t(frame % 81 - 40), 65};
}
static void step(sm64_world *world, int frame) {
  require(sm64_ft_step(world, input(frame), error, sizeof(error)), "step");
}
static std::vector<uint8_t> render(sm64_world *world, uint32_t width, uint32_t height) {
  sm64_render_config config{};
  config.width = width; config.height = height;
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  require(sm64_ft_render(world, &config, pixels.data(), pixels.size(), error, sizeof(error)), "render");
  return pixels;
}

static void editor_cameras(sm64_world *branch, sm64_world *reference) {
  sm64_camera camera{};
  if (!sm64_ft_camera(branch, 4.f/3.f, &camera)) {
    std::puts("Editor camera tests skipped: legacy runtime has no scene hooks");
    return;
  }
  require(camera.near_z > 0 && camera.far_z > camera.near_z && camera.fov_y > 0, "native camera lens");
  // Compare the same scene without HUD/dialog overlays. A dialog can cover
  // much more than 10% of the frame without any camera projection error.
  sm64_render_config native_scene{};
  native_scene.width=128; native_scene.height=96; native_scene.scene_only=true;
  std::vector<uint8_t> original(128*96*4);
  require(sm64_ft_render(branch,&native_scene,original.data(),original.size(),error,sizeof(error)), "native scene");
  const auto mario = view(branch);
  std::vector<uint8_t> first(128*96*4), repeated(first.size());
  // Regenerating the exact native pose/lens must put scene geometry at the
  // same pixels. HUD omission and tiny floating-point round trips are allowed.
  sm64_render_config matched{};
  matched.width = 128; matched.height = 96; matched.mode = 1; matched.scene_only = true;
  std::memcpy(matched.eye, camera.eye, sizeof(matched.eye));
  std::memcpy(matched.target, camera.target, sizeof(matched.target));
  std::memcpy(matched.up, camera.up, sizeof(matched.up));
  std::memcpy(matched.view_proj, camera.view_proj, sizeof(matched.view_proj));
  require(sm64_ft_render(branch, &matched, first.data(), first.size(), error, sizeof(error)), "native camera round trip");
  size_t matching = 0;
  for (size_t i = 0; i < first.size(); i += 4) {
    const int delta = std::abs(int(first[i])-original[i]) + std::abs(int(first[i+1])-original[i+1]) +
                      std::abs(int(first[i+2])-original[i+2]);
    matching += delta < 12;
  }
  if (matching <= 128*96*9/10) {
    std::fprintf(stderr, "Camera round-trip matching pixels: %zu/%u\n", matching, 128*96);
    if (const char *directory = std::getenv("SM64_TEST_ARTIFACT_DIR")) {
      auto dump = [&](const char *name, const std::vector<uint8_t> &pixels) {
        std::ofstream file(std::filesystem::path(directory) / name, std::ios::binary);
        file << "P6\n128 96\n255\n";
        for (size_t i = 0; i < pixels.size(); i += 4) file.write(reinterpret_cast<const char *>(pixels.data()+i), 3);
      };
      dump("sm64-camera-original.ppm", original);
      dump("sm64-camera-matched.ppm", first);
    }
  }
  require(matching > 128*96*9/10, "native and editor camera projections align");
  first.resize(128*72*4); repeated.resize(first.size());
  const auto wide_original = render(branch, 128, 72);
  for (int mode = 1; mode <= 2; ++mode) {
    sm64_render_config config{};
    config.width = 128; config.height = 72; config.mode = mode;
    vec3 target = {mario.pos[0], mario.pos[1], mario.pos[2]};
    vec3 eye = {target[0]+700, target[1]+600, target[2]+900};
    vec3 up = {0, 1, 0};
    mat4 projection, view_matrix, vp;
    if (mode == 2) {
      eye[0] = target[0]; eye[1] = target[1]+3000; eye[2] = target[2];
      up[1] = 0; up[2] = -1;
      glm_ortho_rh_zo(-2400, 2400, -1350, 1350, 6000, 0, projection);
    } else glm_perspective_rh_zo(.8f, 16.f/9.f, 20000, 10, projection);
    projection[1][1] *= -1;
    glm_lookat(eye, target, up, view_matrix);
    glm_mat4_mul(projection, view_matrix, vp);
    std::memcpy(config.eye, eye, sizeof(eye));
    std::memcpy(config.target, target, sizeof(target));
    std::memcpy(config.up, up, sizeof(up));
    std::memcpy(config.view_proj, vp, sizeof(vp));
    require(sm64_ft_render(branch, &config, first.data(), first.size(), error, sizeof(error)), "external camera render");
    require(sm64_ft_render(branch, &config, repeated.data(), repeated.size(), error, sizeof(error)), "repeat external camera");
    if (first != repeated || first == wide_original)
      std::fprintf(stderr, "Editor camera failure: mode %d, frame %u, Mario %.1f %.1f %.1f, action %08x\n",
                   mode, mario.frame, mario.pos[0], mario.pos[1], mario.pos[2], mario.action);
    require(first == repeated, "editor camera is repeatable");
    require(first != wide_original, "editor camera differs from native view");
    size_t colored = 0;
    for (size_t i = 0; i < first.size(); i += 4) colored += first[i] + first[i+1] + first[i+2] > 40;
    require(wide_original == render(branch, 128, 72), "editor rendering preserves game display list");
    sm64_camera after{};
    require(sm64_ft_camera(branch, 4.f/3.f, &after) && std::memcmp(&camera, &after, sizeof(camera)) == 0,
            "editor rendering preserves cached game camera");
    config.view_proj[0] = std::numeric_limits<float>::quiet_NaN();
    require(!sm64_ft_render(branch, &config, first.data(), first.size(), error, sizeof(error)), "reject invalid editor camera");
    require(wide_original == render(branch, 128, 72), "failed editor rendering preserves game display list");
    equal(branch, reference, "editor rendering preserves world state");
  }
  step(branch, 81); step(reference, 81);
  equal(branch, reference, "editor rendering preserves simulation continuation");
}

// Uses a locally supplied setup/ROM; no copyrighted fixture is checked in.
int main(int argc, char **argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: sm64_simulation_test setup.sm64\n"); return 64; }
  auto *level = sm64_ft_open(argv[1], "/tmp", error, sizeof(error));
  require(level, "open");
  auto *reference = sm64_ft_world_new(level, error, sizeof(error));
  auto *branch = sm64_ft_world_new(level, error, sizeof(error));
  require(reference && branch, "worlds");
  require(view(reference).valid, "Mario is present");
  editor_cameras(branch, reference);

  // A lazy (not yet captured) world must survive opening a second session.
  for (int i = 0; i < 60; ++i) step(reference, i);
  for (int i = 0; i < 60; ++i) step(branch, i);
  auto *second_level = sm64_ft_open(argv[1], "/tmp", error, sizeof(error));
  require(second_level, "second session");
  step(branch, 60); step(reference, 60);
  equal(reference, branch, "opening a session preserves the active branch");
  sm64_ft_level_free(second_level);

  const auto start = std::chrono::steady_clock::now();
  // Repeat and resize renders, then compare continuation with a headless run.
  // This also catches stale Fast3D texture/shader pointers between renderers.
  for (int i = 61; i < 81; ++i) {
    const auto first = render(branch, 96, 72);
    require(first == render(branch, 96, 72), "repeated rendering is deterministic");
    render(branch, 128, 72);
    step(branch, i); step(reference, i);
    equal(reference, branch, "rendering does not change simulation");
  }
  require(render(branch, 96, 72) == render(reference, 96, 72), "headless and rendered worlds look identical");
  editor_cameras(branch, reference);

  const size_t size = sm64_ft_save(branch, nullptr, 0, error, sizeof(error));
  std::vector<uint8_t> saved(size);
  require(size && sm64_ft_save(branch, saved.data(), size, error, sizeof(error)) == size, "save");
  require(sm64_ft_load(branch, saved.data(), size, error, sizeof(error)), "load");
  equal(reference, branch, "save reload restores simulation");
  for (int i = 81; i < 101; ++i) {
    step(branch, i); step(reference, i);
    equal(reference, branch, "save reload continuation");
  }

  // Destroy and overwrite worlds that own the restore comparison baseline.
  for (int i = 101; i < 141; ++i) {
    auto *temporary = sm64_ft_world_new(level, error, sizeof(error));
    require(temporary, "temporary world");
    sm64_ft_copy(temporary, reference);
    step(temporary, i);
    sm64_ft_copy(branch, temporary);
    sm64_ft_world_free(temporary);
    step(reference, i);
    equal(reference, branch, "copy and destroy preserve snapshots");
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  // Starting overrides and later edits must be replayed at their own frames,
  // and a copied world's journal must remain independent of further edits.
  auto *edited = sm64_ft_world_new(level, error, sizeof(error));
  auto *loaded = sm64_ft_world_new(level, error, sizeof(error));
  require(edited && loaded, "override worlds");
  const auto before_override = render(edited, 128, 96);
  auto override = view(edited);
  override.pos[0] += 150.f; override.pos[1] += 300.f;
  override.health = 0x580;
  require(sm64_ft_set(edited, 0, &override, error, sizeof(error)), "starting position");
  const auto after_override = render(edited, 128, 96);
  if (after_override == before_override) {
    std::fprintf(stderr, "Invisible position edit: action %08x, before/after position %.1f %.1f %.1f\n",
                 override.action, override.pos[0], override.pos[1], override.pos[2]);
    if (const char *directory = std::getenv("SM64_TEST_ARTIFACT_DIR")) {
      std::ofstream file(std::filesystem::path(directory) / "sm64-invisible-edit.ppm", std::ios::binary);
      file << "P6\n128 96\n255\n";
      for (size_t i = 0; i < after_override.size(); i += 4)
        file.write(reinterpret_cast<const char *>(after_override.data()+i), 3);
    }
  }
  require(after_override != before_override, "starting position is visible without stepping (requires current native edit hooks)");
  require(after_override == render(edited, 128, 96), "paused override rendering is repeatable");
  require(view(edited).frame == 0, "override rendering does not take a simulation tick");
  require(sm64_ft_set(edited, 3, &override, error, sizeof(error)), "starting health");
  sm64_ft_copy(loaded, edited);
  for (int i = 0; i < 12; ++i) { step(edited, i); step(loaded, i); equal(edited, loaded, "override copy continuation"); }
  override = view(edited); override.vel[1] = 42.f;
  require(sm64_ft_set(edited, 1, &override, error, sizeof(error)), "later velocity edit");
  const size_t edited_size = sm64_ft_save(edited, nullptr, 0, error, sizeof(error));
  std::vector<uint8_t> edited_save(edited_size);
  require(edited_size && sm64_ft_save(edited, edited_save.data(), edited_size, error, sizeof(error)) == edited_size, "override save");
  require(sm64_ft_load(loaded, edited_save.data(), edited_size, error, sizeof(error)), "override load");
  equal(edited, loaded, "override replay result");
  for (int i = 12; i < 32; ++i) { step(edited, i); step(loaded, i); equal(edited, loaded, "override save continuation"); }
  override.pos[0] = std::numeric_limits<float>::quiet_NaN();
  require(!sm64_ft_set(loaded, 0, &override, error, sizeof(error)), "reject nonfinite override");
  require(!sm64_ft_load(loaded, edited_save.data(), edited_size - 1, error, sizeof(error)), "reject truncated state");
  equal(edited, loaded, "invalid changes leave world intact");
  step(edited, 32); step(loaded, 32); equal(edited, loaded, "invalid changes preserve continuation");
  require(!sm64_ft_export(edited, "/tmp/frametee-sm64-invalid-override.m64", error, sizeof(error)), "M64 cannot silently discard edits");
  // Legacy input-only saves remain loadable.
  auto legacy = saved;
  legacy[7] = '2'; legacy.erase(legacy.begin() + 20, legacy.begin() + 24);
  require(sm64_ft_load(loaded, legacy.data(), legacy.size(), error, sizeof(error)), "legacy save load");
  require(sm64_ft_load(edited, saved.data(), saved.size(), error, sizeof(error)), "current save load");
  equal(edited, loaded, "legacy and current saves agree");
  sm64_ft_world_free(edited); sm64_ft_world_free(loaded);
  std::printf("SM64 simulation isolation, branching, render repeat/resize, and save replay passed (%.3fs)\n", elapsed);
  sm64_ft_world_free(branch); sm64_ft_world_free(reference); sm64_ft_level_free(level);
}
