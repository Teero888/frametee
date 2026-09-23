#ifndef FRAMETEE_CAMERA_TIMELINE_H
#define FRAMETEE_CAMERA_TIMELINE_H

#include <stdbool.h>

// The camera is edited like a video: its clock is the output time in seconds,
// and the game is the footage. game_tick = g(seconds) is the time remap, so a
// flat stretch freezes the game while the camera keeps moving and a falling one
// plays it backwards. Nothing in here knows about the renderer or the game, so
// all of it can be evaluated and tested on its own.
//
// Interpolation follows After Effects. Where the camera goes is a spatial
// path: a bezier through the keys whose handles shape the route. When it gets
// there is a temporal curve: the distance travelled along that path over time,
// with ease handles at each key. Game time, Follow and Aim are plain curves of
// value over time with the same ease handles.

enum {
  CAMERA_MAX_POSE_KEYS = 256,
  CAMERA_MAX_TIME_KEYS = 128,
  CAMERA_MAX_INFLUENCE_KEYS = 64,
  CAMERA_MAX_SUBJECTS = 16,
};

// How a key blends into the next one in time. Hold on a pose key is a cut; on
// a time key it jumps the game to the next key's tick.
typedef enum camera_interp_t {
  CAMERA_INTERP_SMOOTH = 0, // bezier, shaped by the ease handles
  CAMERA_INTERP_LINEAR = 1, // constant rate
  CAMERA_INTERP_HOLD = 2,
  CAMERA_INTERP_COUNT
} camera_interp_t;

// Who owns a key's handles. Auto keys compute theirs from the neighbours;
// dragging a handle bakes the auto shape and hands it to the user.
typedef enum camera_handle_mode_t {
  CAMERA_HANDLE_AUTO = 0,
  CAMERA_HANDLE_ALIGNED = 1, // user-shaped, in and out stay in line so motion is continuous
  CAMERA_HANDLE_FREE = 2,    // in and out independent: a corner
  CAMERA_HANDLE_COUNT
} camera_handle_mode_t;

// Temporal handles, in After Effects' terms: the rate of change arriving at
// and leaving the key, and how far into the neighbouring segment each handle
// reaches as a fraction of it. Auto keys reach a third of the way. Pose keys
// store their rates as multiples of the neighbouring segment's average speed,
// so a shape survives the path being moved; the curve functions below always
// speak in value per second.
typedef struct camera_ease_t {
  int mode;
  float in_slope, out_slope;         // value per second
  float in_influence, out_influence; // 0..1
} camera_ease_t;

// Spatial handles: offsets from the key's point to the bezier controls on
// either side, for the eye's path and the look-at point's.
typedef struct camera_spatial_t {
  int mode;
  float eye_in[3], eye_out[3];
  float target_in[3], target_out[3];
} camera_spatial_t;

typedef struct camera_pose_t {
  float eye[3];
  // 3D: the look-at point. 2D: the view center in world units (x, y).
  float target[3];
  float roll;  // radians around the view axis, relative to world +Y
  float fov_y; // radians, 3D only
  float zoom;  // 2D only
} camera_pose_t;

// Every key type starts with its time, interpolation and ease, so the key
// arrays share one set of editing helpers.
typedef struct camera_pose_key_t {
  double time;
  int interp;
  camera_ease_t ease; // on the distance travelled along the path
  camera_pose_t pose;
  camera_spatial_t spatial;
} camera_pose_key_t;

typedef struct camera_time_key_t {
  double time;
  int interp;
  camera_ease_t ease;
  double game_tick;
} camera_time_key_t;

typedef struct camera_influence_key_t {
  double time;
  int interp;
  camera_ease_t ease;
  float value; // 0..1
} camera_influence_key_t;

// How Follow places the camera around the characters in 3D.
typedef enum camera_follow_style_t {
  CAMERA_FOLLOW_FIXED = 0,  // the keyed offset, fixed in the world: an orbit around them
  CAMERA_FOLLOW_BEHIND = 1, // the keyed offset turns with the way they are heading: a chase
  CAMERA_FOLLOW_GAME = 2,   // the game's own follow camera
  CAMERA_FOLLOW_COUNT
} camera_follow_style_t;

typedef enum camera_channel_t {
  CAMERA_CHANNEL_TIME = 0,
  CAMERA_CHANNEL_POSE,
  CAMERA_CHANNEL_FOLLOW,
  CAMERA_CHANNEL_AIM,
  CAMERA_CHANNEL_COUNT
} camera_channel_t;

typedef struct camera_timeline_t {
  // The export range; unset means from zero to the end of the game timeline.
  bool range_set;
  double range_start, range_end;

  // The characters Follow and Aim track, as player track indices. Several
  // are tracked through the middle of the box around them.
  int subject_tracks[CAMERA_MAX_SUBJECTS];
  int subject_count;
  float subject_offset[3];
  // 2D: while following, zoom out as far as needed to keep every tracked
  // character in view, filling at most `fit_fill` of it.
  bool fit_subjects;
  float fit_fill;
  int follow_style; // camera_follow_style_t

  // Not saved: the world width a 2D view spans at zoom 1, set by the editor
  // from the level, so the timing can weigh a zoom against a pan.
  float view_scale;

  camera_time_key_t time_keys[CAMERA_MAX_TIME_KEYS];
  int time_count;
  camera_pose_key_t pose_keys[CAMERA_MAX_POSE_KEYS];
  int pose_count;
  // Follow moves the rig's pivot onto the subject, keeping the keyed eye
  // offset; Aim turns the camera toward the subject.
  camera_influence_key_t follow_keys[CAMERA_MAX_INFLUENCE_KEYS];
  int follow_count;
  camera_influence_key_t aim_keys[CAMERA_MAX_INFLUENCE_KEYS];
  int aim_count;
} camera_timeline_t;

void camera_timeline_default(camera_timeline_t *camera);

// Game time. Without keys the game runs at 1x; outside the keyed span it
// continues at 1x from the nearest key, unless the last key holds.
double camera_game_tick(const camera_timeline_t *camera, double seconds, double ticks_per_second);
// The earliest time at or after `from` that shows `game_tick`. False if the
// remap never reaches it there.
bool camera_seconds_for_game_tick(const camera_timeline_t *camera, double game_tick, double ticks_per_second,
                                  double from, double *out_seconds);

// Pose and influence channels. The pose needs at least one key.
camera_pose_t camera_eval_pose(const camera_timeline_t *camera, double seconds);
float camera_eval_follow(const camera_timeline_t *camera, double seconds);
float camera_eval_aim(const camera_timeline_t *camera, double seconds);

// Every channel as a curve of one value over time, for the graph editor. For
// the pose that value is the distance travelled along the path, measured from
// the first key; `distances` is camera_pose_distances' output, or NULL to
// measure it on the spot.
void camera_pose_distances(const camera_timeline_t *camera, double out[CAMERA_MAX_POSE_KEYS]);
double camera_curve_value(const camera_timeline_t *camera, camera_channel_t channel, double seconds, double tps,
                          const double *distances);
double camera_curve_key_value(const camera_timeline_t *camera, camera_channel_t channel, int index,
                              const double *distances);
// The handles a key uses, whether computed or the user's.
camera_ease_t camera_curve_key_ease(const camera_timeline_t *camera, camera_channel_t channel, int index, double tps);
// Shapes one side of a key's ease (side < 0: in, > 0: out). An auto key is
// baked first; an aligned key keeps both sides on one slope.
void camera_set_ease_handle(camera_timeline_t *camera, camera_channel_t channel, int index, int side, float slope,
                            float influence, double tps);
void camera_set_ease_mode(camera_timeline_t *camera, camera_channel_t channel, int index, int mode, double tps);
// Pose keys once stored their shaped rates in distance per second; this turns
// such rates into the relative ones stored now. For loading older projects.
void camera_pose_eases_from_absolute(camera_timeline_t *camera);
// After Effects' Easy Ease: arrive and leave at rest, a third of the way out.
void camera_easy_ease(camera_timeline_t *camera, camera_channel_t channel, int index);

// Spatial handles of a pose key, whether computed or the user's.
camera_spatial_t camera_pose_key_spatial(const camera_timeline_t *camera, int index);
// Moves one handle (point 0: eye, 1: target; side < 0: in, > 0: out). An auto
// key is baked first; an aligned key turns its other handle to stay in line.
void camera_set_spatial_handle(camera_timeline_t *camera, int index, int point, int side, const float offset[3]);
void camera_set_spatial_mode(camera_timeline_t *camera, int index, int mode);
// The position on the path between key `index` and the next, at fraction `u`
// of that segment's length. point 0: eye, 1: target.
void camera_path_point(const camera_timeline_t *camera, int index, int point, double u, float out[3]);
// Retimes the keys inside each shot so the camera travels its whole path at
// one speed, keeping the shot's first and last key where they are.
void camera_even_speed(camera_timeline_t *camera);

// Key arrays by channel, for editing code that does not care which it holds.
int camera_channel_count(const camera_timeline_t *camera, camera_channel_t channel);
double camera_channel_key_time(const camera_timeline_t *camera, camera_channel_t channel, int index);
int *camera_channel_key_interp(camera_timeline_t *camera, camera_channel_t channel, int index);
camera_ease_t *camera_channel_key_ease(camera_timeline_t *camera, camera_channel_t channel, int index);

// Inserts in time order and returns the key's index, replacing a key at the
// same time. -1 when the channel is full or the time is not finite.
int camera_insert_time_key(camera_timeline_t *camera, double time, double game_tick, int interp);
int camera_insert_pose_key(camera_timeline_t *camera, double time, const camera_pose_t *pose, int interp);
int camera_insert_influence_key(camera_timeline_t *camera, camera_channel_t channel, double time, float value,
                                int interp);
void camera_delete_key(camera_timeline_t *camera, camera_channel_t channel, int index);
// Retimes a key, keeping the channel sorted. Returns its new index. A key
// cannot land on another key's time.
int camera_move_key(camera_timeline_t *camera, camera_channel_t channel, int index, double time);
// The key whose time is within `tolerance` of `time`, or -1.
int camera_find_key(const camera_timeline_t *camera, camera_channel_t channel, double time, double tolerance);

// Shifts every key at or after `from` by `delta` seconds.
void camera_ripple(camera_timeline_t *camera, double from, double delta);
// The last keyed time over every channel, or zero.
double camera_last_key_time(const camera_timeline_t *camera);

#endif
