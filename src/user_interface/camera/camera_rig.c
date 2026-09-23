#include "camera_rig.h"

#include <GLFW/glfw3.h>
#include <engine/engine_api.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <user_interface/camera/camera_window.h>
#include <user_interface/timeline/timeline_model.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static float lerpf(float a, float b, float weight) { return a + (b - a) * weight; }

double camera_rig_ticks_per_second(gfx_handler_t *h) {
  const int tps = game_ticks_per_second(&h->game_host);
  return tps > 0 ? (double)tps : 50.0;
}

// The shape of the rendered video, width over height. The editor's viewport
// can be any shape; the camera frames what the export will show.
static float output_aspect(gfx_handler_t *h) {
  const video_export_options_t *o = &h->user_interface.video_options;
  if (o->width > 0 && o->height > 0) return (float)o->width / (float)o->height;
  return h->viewport[1] > 0.f ? h->viewport[0] / h->viewport[1] : 16.f / 9.f;
}

// The world width a 2D view spans at zoom 1: the inverse of screen_to_world.
float camera_rig_view_scale(gfx_handler_t *h) {
  if (game_is_3d(&h->game_host) || h->world_width <= 0.f || h->world_height <= 0.f) return 1.f;
  return 2.f * h->world_width / (fmaxf(h->world_width, h->world_height) * 0.001f);
}

// Framing is the same at every aspect: a keyed zoom or lens covers the
// picture's shorter side, exactly as the renderer does at 16:9. A 9:16 video
// then shows its width as wide as a 16:9 one shows its height, and everything
// in it is as large in the picture.
static const float REFERENCE_ASPECT = 16.f / 9.f;

static float viewport_aspect(gfx_handler_t *h) {
  return h->viewport[1] > 0.f ? h->viewport[0] / h->viewport[1] : REFERENCE_ASPECT;
}

// The renderer's 2D zoom fixes the view's width; the height follows the aspect.
static float renderer_zoom(float keyed, float aspect) {
  return aspect >= 1.f ? keyed * REFERENCE_ASPECT / aspect : keyed * REFERENCE_ASPECT;
}

static float keyed_zoom(float renderer, float aspect) {
  return aspect >= 1.f ? renderer * aspect / REFERENCE_ASPECT : renderer / REFERENCE_ASPECT;
}

// The renderer's lens is vertical; in a tall picture the keyed one is across.
static float renderer_fov(float keyed, float aspect) {
  return aspect >= 1.f ? keyed : 2.f * atanf(tanf(keyed * 0.5f) / aspect);
}

static float keyed_fov(float renderer, float aspect) {
  return aspect >= 1.f ? renderer : 2.f * atanf(tanf(renderer * 0.5f) * aspect);
}

// Characters

// One track's position at a fractional tick, from the same pair of worlds the
// frame is drawn from: ceil(tick) and the one before, blended by the fraction.
static bool track_position(gfx_handler_t *h, int track, double game_tick, float out[3]) {
  timeline_state_t *ts = &h->user_interface.timeline;
  if (track < 0 || track >= ts->player_track_count || !isfinite(game_tick)) return false;
  const int group = model_track_group_index(ts, track);
  if (group < 0) return false;
  const int local = model_group_local_track_index(ts, track);
  const int current_tick = (int)ceil(fmax(0.0, game_tick));
  const float alpha = 1.f - (float)(current_tick - fmax(0.0, game_tick));
  const ft_world *previous = NULL, *current = NULL;
  model_group_world_pair(ts, group, current_tick, &previous, &current);
  if (!model_player_position(ts, current, local, out)) return false;
  float before[3];
  if (previous && model_player_position(ts, previous, local, before))
    for (int i = 0; i < 3; ++i) out[i] = lerpf(before[i], out[i], alpha);
  return true;
}

// Every tracked character's position over a span of ticks, simulated a slice
// per frame off to the side, so the camera's real path can be drawn while
// the game sits at the playhead.
typedef struct character_path_t {
  int track, start_offset, first_tick, filled, capacity;
  uint64_t revision;
  int generation;
  bool done;
  float (*positions)[3];
  model_position_sampler_t *sampler;
} character_path_t;

static character_path_t g_paths[CAMERA_MAX_SUBJECTS];
static int g_generation = 1;

static void path_reset(gfx_handler_t *h, character_path_t *path) {
  model_position_sampler_destroy(&h->user_interface.timeline, path->sampler);
  free(path->positions);
  memset(path, 0, sizeof(*path));
}

void camera_rig_forget(void) { ++g_generation; }

enum { MAX_PATH_TICKS = 60000, TICKS_PER_SLICE = 32 };
// How long a frame may spend walking character paths, in seconds. An export
// has no frame to keep smooth and walks them in one go.
static double g_path_budget = 0.002;

void camera_rig_update_paths(gfx_handler_t *h, int first_tick, int last_tick) {
  timeline_state_t *ts = &h->user_interface.timeline;
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  first_tick = first_tick < 0 ? 0 : first_tick;
  if (last_tick - first_tick > MAX_PATH_TICKS) last_tick = first_tick + MAX_PATH_TICKS;
  const int wanted = last_tick - first_tick + 1;
  const double deadline = glfwGetTime() + g_path_budget;
  for (int s = 0; s < CAMERA_MAX_SUBJECTS; ++s) {
    character_path_t *path = &g_paths[s];
    const int track = s < camera->subject_count ? camera->subject_tracks[s] : -1;
    if (track < 0 || track >= ts->player_track_count) {
      if (path->positions || path->sampler) path_reset(h, path);
      continue;
    }
    const int group = model_track_group_index(ts, track);
    if (group < 0) continue;
    const int offset = ts->groups[group]->start_offset;
    const uint64_t revision = ts->groups[group]->physics_revision;
    // Anything that moves the character invalidates what was walked so far.
    if (path->track != track || path->revision != revision || path->generation != g_generation ||
        path->start_offset != offset || path->first_tick != first_tick || !path->positions) {
      path_reset(h, path);
      path->track = track;
      path->revision = revision;
      path->generation = g_generation;
      path->start_offset = offset;
      path->first_tick = first_tick;
    }
    if (path->capacity < wanted) {
      float(*grown)[3] = realloc(path->positions, sizeof(float[3]) * (size_t)wanted);
      if (!grown) continue;
      path->positions = grown;
      path->capacity = wanted;
      path->done = false;
    }
    if (path->done || path->filled >= wanted || glfwGetTime() >= deadline) continue;
    if (!path->sampler) path->sampler = model_position_sampler_create(ts, track, first_tick + path->filled);
    if (!path->sampler) {
      path->done = true;
      continue;
    }
    // A slice at a time until the frame's share is spent. A sampler that stops
    // short has reached a world it cannot read; keep what there is.
    while (path->filled < wanted && glfwGetTime() < deadline) {
      const int asked = wanted - path->filled < TICKS_PER_SLICE ? wanted - path->filled : TICKS_PER_SLICE;
      const int got = model_position_sampler_step(ts, path->sampler, asked, path->positions + path->filled);
      path->filled += got;
      if (got < asked) {
        path->done = true;
        break;
      }
    }
    if (path->filled >= wanted) {
      model_position_sampler_destroy(ts, path->sampler);
      path->sampler = NULL;
    }
  }
}

static bool cached_position(int slot, double tick, float out[3]) {
  const character_path_t *path = &g_paths[slot];
  const double local = tick - path->first_tick;
  if (!path->positions || local < 0.0 || local > path->filled - 1) return false;
  const int below = (int)floor(local), above = below + 1 < path->filled ? below + 1 : below;
  const float t = (float)(local - below);
  for (int i = 0; i < 3; ++i) out[i] = lerpf(path->positions[below][i], path->positions[above][i], t);
  return true;
}

// The middle of the box around every tracked character, plus the offset, and
// the box's half size. From the drawn worlds, or from the walked paths.
typedef struct subject_t {
  float center[3], half[3];
  bool valid;
} subject_t;

static subject_t subject_at(gfx_handler_t *h, double game_tick, bool from_paths) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  subject_t out = {{0}, {0}, false};
  float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (int s = 0; s < camera->subject_count && s < CAMERA_MAX_SUBJECTS; ++s) {
    float position[3];
    const bool known = from_paths ? cached_position(s, game_tick, position)
                                  : track_position(h, camera->subject_tracks[s], game_tick, position);
    if (!known) {
      // A path still being walked has no answer yet; the drawn world always does.
      if (from_paths) return out;
      continue;
    }
    for (int i = 0; i < 3; ++i) {
      low[i] = fminf(low[i], position[i]);
      high[i] = fmaxf(high[i], position[i]);
    }
    out.valid = true;
  }
  if (!out.valid) return out;
  for (int i = 0; i < 3; ++i) {
    out.center[i] = 0.5f * (low[i] + high[i]) + camera->subject_offset[i];
    out.half[i] = 0.5f * (high[i] - low[i]);
  }
  return out;
}

bool camera_rig_subject(gfx_handler_t *h, double game_tick, float out[3]) {
  const subject_t subject = subject_at(h, game_tick, false);
  if (subject.valid) memcpy(out, subject.center, sizeof(subject.center));
  return subject.valid;
}

// Follow and Aim for one moment: the subject, and how strongly each applies.
typedef struct rig_constraints_t {
  float subject[3];
  float follow, aim;
  float fit_zoom; // 2D: the most zoom that keeps every character in view, or 0
  float turn;     // Behind: how far the keyed offset turns with the characters, radians
  bool game;      // Game camera: blend toward `game_eye` and `game_target`
  float game_eye[3], game_target[3];
} rig_constraints_t;

bool camera_rig_constrained(const camera_timeline_t *camera) {
  for (int i = 0; i < camera->follow_count; ++i)
    if (camera->follow_keys[i].value > 0.f) return true;
  for (int i = 0; i < camera->aim_count; ++i)
    if (camera->aim_keys[i].value > 0.f) return true;
  return false;
}

// The keyed zoom at which the box fills `fill` of the video: the shorter side
// has to hold the box along it, and the longer side the box along that.
static float fit_zoom(gfx_handler_t *h, const float half[3], float fill) {
  const float aspect = output_aspect(h);
  fill = fminf(0.98f, fmaxf(0.1f, fill));
  const float across = 2.f * half[0] / fill, down = 2.f * half[1] / fill;
  const float shorter = aspect >= 1.f ? fmaxf(down, across / aspect) : fmaxf(across, down * aspect);
  if (shorter < 1e-3f) return 0.f;
  return camera_rig_view_scale(h) / (REFERENCE_ASPECT * shorter);
}

// Which way the characters are heading on the ground, as a yaw from +Z, from
// where the walked paths put them a moment before and after. A wider window
// is tried while they stand still; false if they never move near here.
static bool subject_heading(gfx_handler_t *h, double tick, float *yaw) {
  const double tps = camera_rig_ticks_per_second(h);
  const float least = fmaxf(0.05f, game_units_per_tile(&h->game_host) * 0.25f);
  const double windows[] = {0.25, 0.5, 1.0, 2.0};
  for (size_t w = 0; w < sizeof(windows) / sizeof(windows[0]); ++w) {
    subject_t before = subject_at(h, tick - windows[w] * tps, true), after = subject_at(h, tick + windows[w] * tps, true);
    // Near the ends of the walked span, one side is where they are now.
    if (!before.valid) before = subject_at(h, tick, true);
    if (!after.valid) after = subject_at(h, tick, true);
    if (!before.valid || !after.valid) return false;
    const float dx = after.center[0] - before.center[0], dz = after.center[2] - before.center[2];
    if (dx * dx + dz * dz >= least * least) {
      *yaw = atan2f(dx, dz);
      return true;
    }
  }
  return false;
}

// The game's own follow camera on the first character: the first mode it
// directs itself. False when it has none or declines.
static bool game_follow_camera(gfx_handler_t *h, double tick, float eye[3], float target[3]) {
  timeline_state_t *ts = &h->user_interface.timeline;
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  game_host_t *host = &h->game_host;
  if (camera->subject_count <= 0) return false;
  int mode = -1;
  for (unsigned i = 0; i < game_camera_mode_count(host) && mode < 0; ++i) {
    const ft_camera_mode *candidate = game_camera_mode(host, i);
    if (candidate && (candidate->flags & FT_CAMERA_MODE_DIRECTED)) mode = (int)i;
  }
  const int track = camera->subject_tracks[0];
  const int group = model_track_group_index(ts, track);
  if (mode < 0 || group < 0) return false;
  const int current_tick = (int)ceil(fmax(0.0, tick));
  const ft_world *previous = NULL, *current = NULL;
  model_group_world_pair(ts, group, current_tick, &previous, &current);
  if (!current) return false;
  ft_camera_frame frame = {0};
  frame.struct_size = sizeof(frame);
  frame.mode = (uint32_t)mode;
  frame.world = current;
  frame.previous_world = previous;
  frame.alpha = 1.f - (float)(current_tick - fmax(0.0, tick));
  frame.player = model_group_local_track_index(ts, track);
  ft_camera view;
  engine_api_camera_get(&view);
  view.use_view_proj = false;
  if (!gh_camera_update(host, &frame, &view)) return false;
  eye[0] = view.eye.x; eye[1] = view.eye.y; eye[2] = view.eye.z;
  target[0] = view.target.x; target[1] = view.target.y; target[2] = view.target.z;
  return true;
}

static rig_constraints_t constraints_at(gfx_handler_t *h, double seconds, bool from_paths, bool *known) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  const bool is_3d = game_is_3d(&h->game_host);
  rig_constraints_t out;
  memset(&out, 0, sizeof(out));
  out.follow = camera_eval_follow(camera, seconds);
  out.aim = is_3d ? camera_eval_aim(camera, seconds) : 0.f;
  if (known) *known = true;
  if (out.follow <= 0.f && out.aim <= 0.f) return out;
  const double tick = camera_game_tick(camera, seconds, camera_rig_ticks_per_second(h));
  const subject_t subject = subject_at(h, tick, from_paths);
  if (!subject.valid) {
    if (known) *known = false;
    out.follow = out.aim = 0.f;
    return out;
  }
  memcpy(out.subject, subject.center, sizeof(out.subject));
  if (!is_3d && camera->fit_subjects) out.fit_zoom = fit_zoom(h, subject.half, camera->fit_fill);
  if (!is_3d || out.follow <= 0.f || camera->follow_style == CAMERA_FOLLOW_FIXED) return out;
  // The game's camera keeps state from frame to frame and can only be asked
  // in order, so the drawn path stands in with the chase it most resembles.
  if (camera->follow_style == CAMERA_FOLLOW_GAME && !from_paths)
    out.game = game_follow_camera(h, tick, out.game_eye, out.game_target);
  float yaw;
  if (!out.game && subject_heading(h, tick, &yaw)) out.turn = yaw;
  return out;
}

// Follow moves the rig's pivot onto the subject and keeps the keyed offset
// from pivot to eye, so the keys frame the subject once Follow is on. Aim then
// turns the view toward the subject without moving the eye. Fitting zooms out,
// never in, as far as the characters need, as strongly as Follow applies.
// Turns a vector about +Y.
static void turn_y(float v[3], float angle) {
  const float c = cosf(angle), s = sinf(angle), x = v[0], z = v[2];
  v[0] = c * x + s * z;
  v[2] = -s * x + c * z;
}

static void apply_constraints(const rig_constraints_t *rig, camera_pose_t *pose) {
  if (rig->game) {
    // Toward the game's own camera, as strongly as Follow applies.
    for (int i = 0; i < 3; ++i) {
      pose->eye[i] = lerpf(pose->eye[i], rig->game_eye[i], rig->follow);
      pose->target[i] = lerpf(lerpf(pose->target[i], rig->game_target[i], rig->follow), rig->subject[i], rig->aim);
    }
    return;
  }
  float offset[3];
  for (int i = 0; i < 3; ++i) offset[i] = pose->eye[i] - pose->target[i];
  // Behind: keys are framed as if the characters head along +Z.
  turn_y(offset, rig->turn * rig->follow);
  for (int i = 0; i < 3; ++i) {
    const float anchor = lerpf(pose->target[i], rig->subject[i], rig->follow);
    pose->eye[i] = anchor + offset[i];
    pose->target[i] = lerpf(anchor, rig->subject[i], rig->aim);
  }
  if (rig->fit_zoom > 0.f && rig->fit_zoom < pose->zoom)
    pose->zoom = expf(lerpf(logf(pose->zoom), logf(rig->fit_zoom), rig->follow));
}

bool camera_rig_evaluate(gfx_handler_t *h, double seconds, camera_pose_t *out) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (camera->pose_count == 0) return false;
  *out = camera_eval_pose(camera, seconds);
  const rig_constraints_t rig = constraints_at(h, seconds, false, NULL);
  apply_constraints(&rig, out);
  return true;
}

// The same, from the walked character paths instead of the drawn worlds, for
// anywhere on the timeline. False where the paths have not got to yet.
static bool evaluate_preview(gfx_handler_t *h, double seconds, camera_pose_t *out) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (camera->pose_count == 0) return false;
  *out = camera_eval_pose(camera, seconds);
  bool known;
  const rig_constraints_t rig = constraints_at(h, seconds, true, &known);
  apply_constraints(&rig, out);
  return known;
}

// A roll of zero keeps the view level with world +Y. Positive roll turns the
// up vector toward the camera's right.
static void view_basis(const float eye[3], const float target[3], vec3 forward, vec3 right, vec3 up) {
  glm_vec3_sub((float *)target, (float *)eye, forward);
  if (glm_vec3_norm(forward) < 1e-5f) glm_vec3_copy((vec3){0.f, 0.f, -1.f}, forward);
  glm_vec3_normalize(forward);
  glm_vec3_cross(forward, (vec3){0.f, 1.f, 0.f}, right);
  if (glm_vec3_norm(right) < 1e-4f) glm_vec3_copy((vec3){1.f, 0.f, 0.f}, right);
  glm_vec3_normalize(right);
  glm_vec3_cross(right, forward, up);
}

static void rolled_up(const camera_pose_t *pose, vec3 out) {
  vec3 forward, right, up;
  view_basis(pose->eye, pose->target, forward, right, up);
  glm_vec3_scale(up, cosf(pose->roll), out);
  glm_vec3_muladds(right, sinf(pose->roll), out);
}

void camera_rig_apply(gfx_handler_t *h, const camera_pose_t *pose) {
  if (!h->level) return;
  if (!game_is_3d(&h->game_host)) {
    if (h->world_width <= 0.f || h->world_height <= 0.f) return;
    h->renderer.camera.pos[0] = pose->target[0] / h->world_width;
    h->renderer.camera.pos[1] = pose->target[1] / h->world_height;
    h->renderer.camera.zoom = fmaxf(0.005f, renderer_zoom(pose->zoom, viewport_aspect(h)));
    h->renderer.camera.zoom_wanted = h->renderer.camera.zoom;
    return;
  }
  camera3_t *c = &h->renderer.camera3;
  vec3 eye = {pose->eye[0], pose->eye[1], pose->eye[2]};
  vec3 forward, right, basis_up, up, target;
  view_basis(pose->eye, pose->target, forward, right, basis_up);
  glm_vec3_add(eye, forward, target);
  float distance = sqrtf((pose->target[0] - eye[0]) * (pose->target[0] - eye[0]) +
                         (pose->target[1] - eye[1]) * (pose->target[1] - eye[1]) +
                         (pose->target[2] - eye[2]) * (pose->target[2] - eye[2]));
  if (distance < 1e-3f) distance = 1.f;
  glm_vec3_scale(forward, distance, target);
  glm_vec3_add(eye, target, target);
  rolled_up(pose, up);

  // Directed cameras are drawn through the orbit mode. The orbit's own angles
  // are kept in step, so switching to a free camera afterwards starts from
  // this view.
  c->mode = CAMERA3_ORBIT;
  glm_vec3_copy(target, c->orbit_target);
  c->orbit_distance = distance;
  c->orbit_pitch = asinf(glm_clamp(-forward[1], -1.f, 1.f));
  c->orbit_yaw = atan2f(-forward[2], -forward[0]);

  ft_camera *view = &c->directed_camera;
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->eye = (ft_vec3){eye[0], eye[1], eye[2]};
  view->target = (ft_vec3){target[0], target[1], target[2]};
  view->up = (ft_vec3){up[0], up[1], up[2]};
  view->fov_y = fminf(3.0f, fmaxf(0.01f, renderer_fov(pose->fov_y, viewport_aspect(h))));
  view->near_z = c->near_z;
  view->far_z = c->far_z;
  view->use_view_proj = true;
  mat4 camera_view, projection, combined;
  glm_lookat(eye, target, up, camera_view);
  const float aspect = h->viewport[1] > 0.f ? h->viewport[0] / h->viewport[1] : 1.f;
  glm_perspective_rh_zo(view->fov_y, aspect, c->far_z, c->near_z, projection);
  projection[1][1] *= -1.f;
  glm_mat4_mul(projection, camera_view, combined);
  memcpy(view->view_proj, combined, sizeof(combined));
  c->directed_camera_valid = true;
}

void camera_rig_seed_free_view(gfx_handler_t *h, const camera_pose_t *pose) {
  if (!h->level) return;
  game_host_t *host = &h->game_host;
  camera_t *camera = &h->renderer.camera;
  if (!game_is_3d(host)) {
    const ft_camera_mode *mode = game_camera_mode(host, camera->mode);
    if (!mode || !(mode->flags & FT_CAMERA_MODE_FREE)) {
      for (unsigned i = 0; i < game_camera_mode_count(host); ++i) {
        const ft_camera_mode *candidate = game_camera_mode(host, i);
        if (candidate && (candidate->flags & FT_CAMERA_MODE_FREE)) {
          camera->mode = i;
          break;
        }
      }
    }
    camera_rig_apply(h, pose);
    return;
  }
  for (unsigned i = 0; i < game_camera_mode_count(host); ++i)
    if (game_camera_mode_is_freecam(host, i)) {
      camera->mode = i;
      break;
    }
  camera3_t *c = &h->renderer.camera3;
  vec3 forward, right, up;
  view_basis(pose->eye, pose->target, forward, right, up);
  c->mode = CAMERA3_FREECAM;
  c->directed_camera_valid = false;
  glm_vec3_copy((float *)pose->eye, c->free_eye);
  c->free_pitch = asinf(glm_clamp(-forward[1], -1.f, 1.f));
  c->free_yaw = atan2f(-forward[2], -forward[0]);
  if (pose->fov_y > 0.f) c->fov_y = renderer_fov(pose->fov_y, viewport_aspect(h));
}

bool camera_rig_capture(gfx_handler_t *h, double seconds, camera_pose_t *out) {
  if (!h->level) return false;
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  // Start from the keyed pose here so whatever the view cannot express (roll
  // in a free camera, the lens in 2D) carries over unchanged.
  camera_pose_t keyed = camera_eval_pose(camera, seconds);
  const float aspect = viewport_aspect(h);
  if (camera->pose_count == 0) {
    keyed.roll = 0.f;
    keyed.fov_y = keyed_fov(h->renderer.camera3.fov_y, aspect);
    keyed.zoom = keyed_zoom(h->renderer.camera.zoom, aspect);
  }
  camera_pose_t pose = keyed;

  if (!game_is_3d(&h->game_host)) {
    pose.target[0] = h->renderer.camera.pos[0] * h->world_width;
    pose.target[1] = h->renderer.camera.pos[1] * h->world_height;
    pose.target[2] = 0.f;
    // Where the zoom is heading, not where its smoothing has got to.
    pose.zoom = keyed_zoom(h->renderer.camera.zoom_wanted, aspect);
    memcpy(pose.eye, pose.target, sizeof(pose.eye));
  } else {
    camera3_t *c = &h->renderer.camera3;
    vec3 eye, forward;
    renderer_camera3_eye(h, eye);
    renderer_camera3_forward(h, forward);
    // A free camera has a heading but no point it looks at. Keep the keyed
    // focus distance, so the look-at point stays roughly where it was.
    float distance = sqrtf((keyed.target[0] - keyed.eye[0]) * (keyed.target[0] - keyed.eye[0]) +
                           (keyed.target[1] - keyed.eye[1]) * (keyed.target[1] - keyed.eye[1]) +
                           (keyed.target[2] - keyed.eye[2]) * (keyed.target[2] - keyed.eye[2]));
    if (camera->pose_count == 0 || distance < 1e-3f) {
      vec3 focus;
      renderer_camera3_target(h, focus);
      distance = glm_vec3_distance(eye, focus);
      if (c->mode == CAMERA3_FREECAM || distance < 1e-3f) distance = fmaxf(1.f, game_units_per_tile(&h->game_host) * 8.f);
    }
    memcpy(pose.eye, eye, sizeof(pose.eye));
    for (int i = 0; i < 3; ++i) pose.target[i] = eye[i] + forward[i] * distance;
    if (c->mode == CAMERA3_ORBIT && c->directed_camera_valid) {
      const ft_vec3 up = c->directed_camera.up;
      vec3 f, right, level;
      view_basis(pose.eye, pose.target, f, right, level);
      vec3 u = {up.x, up.y, up.z};
      if (glm_vec3_norm(u) > 1e-4f) pose.roll = atan2f(glm_vec3_dot(u, right), glm_vec3_dot(u, level));
      pose.fov_y = keyed_fov(c->directed_camera.fov_y, aspect);
    } else {
      pose.fov_y = keyed_fov(c->fov_y, aspect);
    }
  }

  // Undo this moment's constraints, so applying them reproduces the view.
  const rig_constraints_t rig = constraints_at(h, seconds, false, NULL);
  if (rig.game && rig.follow > 0.f) {
    // Only what Follow leaves of the key shows; with it all the way on, the key is kept.
    if (rig.follow < 0.999f) {
      for (int i = 0; i < 3; ++i) {
        pose.eye[i] = (pose.eye[i] - rig.follow * rig.game_eye[i]) / (1.f - rig.follow);
        pose.target[i] = (pose.target[i] - rig.follow * rig.game_target[i]) / (1.f - rig.follow);
      }
    } else {
      memcpy(pose.eye, keyed.eye, sizeof(pose.eye));
      memcpy(pose.target, keyed.target, sizeof(pose.target));
    }
  } else if (rig.follow > 0.f || rig.aim > 0.f) {
    for (int i = 0; i < 3; ++i) {
      const float eye = pose.eye[i], target = pose.target[i], subject = rig.subject[i];
      float anchor;
      if (rig.aim < 0.999f) anchor = (target - rig.aim * subject) / (1.f - rig.aim);
      else anchor = lerpf(keyed.target[i], subject, rig.follow); // aim fully on: the pivot is not visible
      float base_target = rig.follow < 0.999f ? (anchor - rig.follow * subject) / (1.f - rig.follow) : anchor;
      pose.target[i] = base_target;
      pose.eye[i] = eye - anchor + base_target;
    }
    // Behind: store the offset as if the characters headed along +Z.
    float offset[3] = {pose.eye[0] - pose.target[0], pose.eye[1] - pose.target[1], pose.eye[2] - pose.target[2]};
    turn_y(offset, -rig.turn * rig.follow);
    for (int i = 0; i < 3; ++i) pose.eye[i] = pose.target[i] + offset[i];
  }
  *out = pose;
  return true;
}

// Gizmos

// A small pyramid from the eye along the view, with a tick on its top edge
// so roll reads at a glance.
static void draw_frustum(gfx_handler_t *h, const camera_pose_t *pose, vec4 color, float size) {
  vec3 forward, right, level, up;
  view_basis(pose->eye, pose->target, forward, right, level);
  rolled_up(pose, up);
  glm_vec3_cross(forward, up, right);
  glm_vec3_normalize(right);
  const float half_height = tanf(fmaxf(0.05f, renderer_fov(pose->fov_y, output_aspect(h))) * 0.5f) * size;
  const float half_width = half_height * output_aspect(h);
  vec3 eye = {pose->eye[0], pose->eye[1], pose->eye[2]}, center, corners[4];
  glm_vec3_scale(forward, size, center);
  glm_vec3_add(eye, center, center);
  const float sx[4] = {-1.f, 1.f, 1.f, -1.f}, sy[4] = {-1.f, -1.f, 1.f, 1.f};
  for (int i = 0; i < 4; ++i) {
    glm_vec3_copy(center, corners[i]);
    glm_vec3_muladds(right, sx[i] * half_width, corners[i]);
    glm_vec3_muladds(up, sy[i] * half_height, corners[i]);
  }
  const float width = size * 0.02f;
  for (int i = 0; i < 4; ++i) {
    renderer_submit_line3(h, eye, corners[i], color, width);
    renderer_submit_line3(h, corners[i], corners[(i + 1) % 4], color, width);
  }
  vec3 top_middle, tip;
  glm_vec3_add(corners[2], corners[3], top_middle);
  glm_vec3_scale(top_middle, 0.5f, top_middle);
  glm_vec3_copy(top_middle, tip);
  glm_vec3_muladds(up, half_height * 0.4f, tip);
  renderer_submit_line3(h, corners[2], tip, color, width);
  renderer_submit_line3(h, corners[3], tip, color, width);
}

static void draw_marker_2d(gfx_handler_t *h, const float at[2], vec4 color, float size) {
  renderer_submit_line(h, 9.4f, (vec2){at[0] - size, at[1]}, (vec2){at[0] + size, at[1]}, color, size * 0.2f);
  renderer_submit_line(h, 9.4f, (vec2){at[0], at[1] - size}, (vec2){at[0], at[1] + size}, color, size * 0.2f);
}

// The border of what a 2D camera shows. screen_to_world makes the width a
// function of the zoom alone, and the height the width over the video's aspect.
static void draw_frame_2d(gfx_handler_t *h, const camera_pose_t *pose, vec4 color) {
  if (h->world_width <= 0.f || h->world_height <= 0.f) return;
  const float half_width = 0.5f * camera_rig_view_scale(h) / fmaxf(0.005f, renderer_zoom(pose->zoom, output_aspect(h)));
  const float half_height = half_width / output_aspect(h);
  const float x = pose->target[0], y = pose->target[1];
  const float thickness = half_width * 0.006f;
  const vec2 corners[4] = {{x - half_width, y - half_height}, {x + half_width, y - half_height},
                           {x + half_width, y + half_height}, {x - half_width, y + half_height}};
  for (int i = 0; i < 4; ++i)
    renderer_submit_line(h, 9.4f, (vec2){corners[i][0], corners[i][1]},
                         (vec2){corners[(i + 1) % 4][0], corners[(i + 1) % 4][1]}, color, thickness);
  draw_marker_2d(h, pose->target, color, half_width * 0.04f);
}

// The span of camera time the path is drawn over: every key, of any channel.
static bool key_span(const camera_timeline_t *camera, double *start, double *end) {
  *start = INFINITY;
  *end = -INFINITY;
  for (int channel = 0; channel < CAMERA_CHANNEL_COUNT; ++channel) {
    const int count = camera_channel_count(camera, (camera_channel_t)channel);
    if (!count) continue;
    *start = fmin(*start, camera_channel_key_time(camera, (camera_channel_t)channel, 0));
    *end = fmax(*end, camera_channel_key_time(camera, (camera_channel_t)channel, count - 1));
  }
  return camera->pose_count > 0 && isfinite(*start);
}

void camera_rig_prepare_paths(gfx_handler_t *h) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  double start, end;
  if (!camera_rig_constrained(camera) || !key_span(camera, &start, &end)) return;
  // The export range too: a chase needs to know the heading wherever it films.
  double range_start, range_end;
  camera_editor_export_range(&h->user_interface, &range_start, &range_end);
  start = fmin(start, range_start);
  end = fmax(end, range_end);
  const double tps = camera_rig_ticks_per_second(h);
  double low = INFINITY, high = -INFINITY;
  for (int i = 0; i <= 256; ++i) {
    const double tick = camera_game_tick(camera, start + (end - start) * i / 256.0, tps);
    low = fmin(low, tick);
    high = fmax(high, tick);
  }
  camera_rig_update_paths(h, (int)floor(low) - 1, (int)ceil(high) + 1);
}

static const float *shown_point(gfx_handler_t *h, const camera_pose_t *pose) {
  return game_is_3d(&h->game_host) ? pose->eye : pose->target;
}

// Where the camera goes: with Follow or Aim, its real path as far as the
// character paths have been walked; otherwise the keyed path.
static int build_path(gfx_handler_t *h, camera_path_sample_t *out, int max) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (camera->pose_count == 0 || max <= 0) return 0;
  int count = 0;
  camera_pose_t pose;
  if (!camera_rig_constrained(camera)) {
    const camera_pose_key_t *keys = camera->pose_keys;
    for (int i = 0; i < camera->pose_count && count < max; ++i) {
      const bool moves = i + 1 < camera->pose_count && keys[i].interp != CAMERA_INTERP_HOLD;
      const int steps = moves ? 32 : 0;
      const bool continues = i > 0 && keys[i - 1].interp != CAMERA_INTERP_HOLD;
      for (int step = continues ? 1 : 0; step <= steps && count < max; ++step) {
        const double time = moves ? keys[i].time + (keys[i + 1].time - keys[i].time) * step / steps : keys[i].time;
        pose = camera_eval_pose(camera, time);
        out[count] = (camera_path_sample_t){time, {0}, step == 0};
        memcpy(out[count].point, shown_point(h, &pose), sizeof(out[count].point));
        ++count;
      }
    }
    return count;
  }
  double start, end;
  key_span(camera, &start, &end);
  // Thirty samples a second draws a smooth line and keeps rebuilding cheap
  // while a key is dragged or the character paths fill in.
  int samples = (int)((end - start) * 30.0) + 2;
  if (samples > max) samples = max;
  if (samples > 2048) samples = 2048;
  double previous = start;
  for (int k = 0; k < samples; ++k) {
    const double time = samples > 1 ? start + (end - start) * k / (samples - 1) : start;
    if (!evaluate_preview(h, time, &pose)) break;
    // A cut between the samples starts the line afresh.
    bool cut = k == 0;
    for (int i = 0; i + 1 < camera->pose_count && !cut; ++i)
      if (camera->pose_keys[i].interp == CAMERA_INTERP_HOLD && camera->pose_keys[i + 1].time > previous &&
          camera->pose_keys[i + 1].time <= time)
        cut = true;
    out[count] = (camera_path_sample_t){time, {0}, cut};
    memcpy(out[count].point, shown_point(h, &pose), sizeof(out[count].point));
    ++count;
    previous = time;
  }
  return count;
}

static bool build_key_pose(gfx_handler_t *h, int index, camera_pose_t *out) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (!camera_rig_constrained(camera)) {
    *out = camera->pose_keys[index].pose;
    return true;
  }
  return evaluate_preview(h, camera->pose_keys[index].time, out);
}

// The drawn path and key positions, rebuilt only when the camera or the walked
// character paths change: both the level and the overlay ask every frame.
enum { MAX_PATH_SAMPLES = 4096 };

typedef struct path_cache_t {
  uint64_t hash;
  bool valid;
  int count;
  camera_path_sample_t samples[MAX_PATH_SAMPLES];
  camera_pose_t key_poses[CAMERA_MAX_POSE_KEYS];
  bool key_known[CAMERA_MAX_POSE_KEYS];
} path_cache_t;

static path_cache_t g_path_cache;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
  return hash;
}

static void refresh_path_cache(gfx_handler_t *h) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  uint64_t hash = 1469598103934665603ull;
  hash = hash_bytes(hash, camera, offsetof(camera_timeline_t, time_keys));
  hash = hash_bytes(hash, camera->time_keys, sizeof(camera->time_keys[0]) * (size_t)camera->time_count);
  hash = hash_bytes(hash, camera->pose_keys, sizeof(camera->pose_keys[0]) * (size_t)camera->pose_count);
  hash = hash_bytes(hash, camera->follow_keys, sizeof(camera->follow_keys[0]) * (size_t)camera->follow_count);
  hash = hash_bytes(hash, camera->aim_keys, sizeof(camera->aim_keys[0]) * (size_t)camera->aim_count);
  const int counts[4] = {camera->time_count, camera->pose_count, camera->follow_count, camera->aim_count};
  hash = hash_bytes(hash, counts, sizeof(counts));
  if (camera_rig_constrained(camera)) {
    hash = hash_bytes(hash, &g_generation, sizeof(g_generation));
    for (int s = 0; s < CAMERA_MAX_SUBJECTS; ++s) {
      hash = hash_bytes(hash, &g_paths[s].filled, sizeof(g_paths[s].filled));
      hash = hash_bytes(hash, &g_paths[s].revision, sizeof(g_paths[s].revision));
      hash = hash_bytes(hash, &g_paths[s].first_tick, sizeof(g_paths[s].first_tick));
    }
  }
  const bool is_3d = game_is_3d(&h->game_host);
  hash = hash_bytes(hash, &is_3d, sizeof(is_3d));
  if (g_path_cache.valid && g_path_cache.hash == hash) return;
  g_path_cache.hash = hash;
  g_path_cache.valid = true;
  g_path_cache.count = build_path(h, g_path_cache.samples, MAX_PATH_SAMPLES);
  for (int i = 0; i < camera->pose_count; ++i) g_path_cache.key_known[i] = build_key_pose(h, i, &g_path_cache.key_poses[i]);
}

int camera_rig_path(gfx_handler_t *h, camera_path_sample_t *out, int max) {
  refresh_path_cache(h);
  const int count = g_path_cache.count < max ? g_path_cache.count : max;
  memcpy(out, g_path_cache.samples, sizeof(*out) * (size_t)count);
  return count;
}

const camera_path_sample_t *camera_rig_path_samples(gfx_handler_t *h, int *count) {
  refresh_path_cache(h);
  *count = g_path_cache.count;
  return g_path_cache.samples;
}

bool camera_rig_key_pose(gfx_handler_t *h, int index, camera_pose_t *out) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (index < 0 || index >= camera->pose_count) return false;
  refresh_path_cache(h);
  if (!g_path_cache.key_known[index]) return false;
  *out = g_path_cache.key_poses[index];
  return true;
}

void camera_rig_render_gizmos(gfx_handler_t *h, double playhead, int selected_pose_key) {
  if (!h->level) return;
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  if (camera->pose_count == 0) return;
  const bool is_3d = game_is_3d(&h->game_host);
  const float scale = is_3d ? fmaxf(0.25f, game_units_per_tile(&h->game_host)) : 1.f;
  vec4 path_color = {0.22f, 0.76f, 1.f, 0.9f};
  vec4 key_color = {0.92f, 0.94f, 0.97f, 1.f};
  vec4 selected_color = {1.f, 0.82f, 0.24f, 1.f};
  vec4 live_color = {1.f, 0.35f, 0.3f, 1.f};

  int count;
  const camera_path_sample_t *samples = camera_rig_path_samples(h, &count);
  for (int i = 1; i < count; ++i) {
    if (samples[i].shot_start) continue;
    const float *a = samples[i - 1].point, *b = samples[i].point;
    if (is_3d)
      renderer_submit_line3(h, (vec3){a[0], a[1], a[2]}, (vec3){b[0], b[1], b[2]}, path_color, 0.05f * scale);
    else
      renderer_submit_line(h, 9.4f, (vec2){a[0], a[1]}, (vec2){b[0], b[1]}, path_color, 0.09f);
  }
  for (int i = 0; i < camera->pose_count; ++i) {
    camera_pose_t pose;
    if (!camera_rig_key_pose(h, i, &pose)) continue;
    float *color = i == selected_pose_key ? selected_color : key_color;
    if (is_3d) draw_frustum(h, &pose, color, scale * 1.2f);
    else if (i == selected_pose_key) draw_frame_2d(h, &pose, color);
    else draw_marker_2d(h, pose.target, color, 0.4f);
  }
  // What the camera shows at the playhead: its whole frame in 2D.
  camera_pose_t live;
  if (camera_rig_evaluate(h, playhead, &live)) {
    if (is_3d) draw_frustum(h, &live, live_color, scale * 1.6f);
    else draw_frame_2d(h, &live, live_color);
  }
}

void camera_rig_prepare_paths_now(gfx_handler_t *h) {
  const double budget = g_path_budget;
  g_path_budget = 1e9;
  camera_rig_prepare_paths(h);
  g_path_budget = budget;
}

bool camera_rig_needs_heading(gfx_handler_t *h) {
  const camera_timeline_t *camera = &h->user_interface.camera_timeline;
  return game_is_3d(&h->game_host) && camera->follow_style != CAMERA_FOLLOW_FIXED && camera_rig_constrained(camera);
}
