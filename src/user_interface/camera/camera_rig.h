#ifndef FRAMETEE_CAMERA_RIG_H
#define FRAMETEE_CAMERA_RIG_H

#include "camera_timeline.h"

struct gfx_handler_t;

// Where the camera timeline meets the engine: the subject's position in the
// game, the viewport camera, and the path drawn over the level.

double camera_rig_ticks_per_second(struct gfx_handler_t *h);
// The subject's position plus its offset, at a fractional game tick.
bool camera_rig_subject(struct gfx_handler_t *h, double game_tick, float out[3]);
// The camera at `seconds` with Follow and Aim applied. False without pose keys.
bool camera_rig_evaluate(struct gfx_handler_t *h, double seconds, camera_pose_t *out);
// Points the viewport through `pose` for this frame.
void camera_rig_apply(struct gfx_handler_t *h, const camera_pose_t *pose);
// Puts the user's free camera where `pose` is, so navigation carries on from it.
void camera_rig_seed_free_view(struct gfx_handler_t *h, const camera_pose_t *pose);
// The viewport's current view, as the pose key that reproduces it at `seconds`
// once that moment's Follow and Aim are applied.
bool camera_rig_capture(struct gfx_handler_t *h, double seconds, camera_pose_t *out);
void camera_rig_render_gizmos(struct gfx_handler_t *h, double playhead, int selected_pose_key);

// The world width a 2D view spans at zoom 1.
float camera_rig_view_scale(struct gfx_handler_t *h);
// Whether Follow or Aim ever apply, so the camera's path depends on the game.
bool camera_rig_constrained(const camera_timeline_t *camera);
// Walks the tracked characters' paths over the keyed span a slice at a time,
// for drawing where the camera really goes. Call once a frame while it is shown.
void camera_rig_prepare_paths(struct gfx_handler_t *h);
void camera_rig_update_paths(struct gfx_handler_t *h, int first_tick, int last_tick);
// The same without a time limit, for an export.
void camera_rig_prepare_paths_now(struct gfx_handler_t *h);
// Whether Follow needs the characters' heading, which comes from their paths.
bool camera_rig_needs_heading(struct gfx_handler_t *h);
// Drops the walked paths, for a new project.
void camera_rig_forget(void);

typedef struct camera_path_sample_t {
  double time;
  float point[3]; // the eye in 3D, the view center in 2D
  bool shot_start;
} camera_path_sample_t;
// The camera's path as drawn in the level: where it really goes with Follow
// and Aim, as far as the character paths are known; otherwise the keyed path.
int camera_rig_path(struct gfx_handler_t *h, camera_path_sample_t *out, int max);
// The same without copying; valid until the camera changes.
const camera_path_sample_t *camera_rig_path_samples(struct gfx_handler_t *h, int *count);
// Where pose key `index` puts the camera, Follow and Aim included.
bool camera_rig_key_pose(struct gfx_handler_t *h, int index, camera_pose_t *out);

#endif
