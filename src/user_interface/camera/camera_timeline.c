#include "camera_timeline.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

// Two keys closer than this share a time.
#define KEY_EPSILON 1e-6
// How far auto handles reach into a segment: a third makes a bezier with
// these handles the same curve as a cubic Hermite spline.
#define AUTO_INFLUENCE (1.0f / 3.0f)
// Samples per segment when measuring a path's length.
#define PATH_SAMPLES 24
// A segment the camera does not move along still takes this much distance, so
// its timing stays defined.
#define MIN_SEGMENT_LENGTH 1e-4

void camera_timeline_default(camera_timeline_t *camera) {
  memset(camera, 0, sizeof(*camera));
  camera->subject_count = 1;
  camera->fit_fill = 0.7f;
}

// Key arrays

typedef struct key_array_t {
  char *base;
  int *count;
  int max;
  size_t size;
} key_array_t;

static key_array_t channel_array(const camera_timeline_t *const_camera, camera_channel_t channel) {
  camera_timeline_t *camera = (camera_timeline_t *)const_camera;
  switch (channel) {
  case CAMERA_CHANNEL_TIME:
    return (key_array_t){(char *)camera->time_keys, &camera->time_count, CAMERA_MAX_TIME_KEYS, sizeof(camera_time_key_t)};
  case CAMERA_CHANNEL_POSE:
    return (key_array_t){(char *)camera->pose_keys, &camera->pose_count, CAMERA_MAX_POSE_KEYS, sizeof(camera_pose_key_t)};
  case CAMERA_CHANNEL_FOLLOW:
    return (key_array_t){(char *)camera->follow_keys, &camera->follow_count, CAMERA_MAX_INFLUENCE_KEYS,
                         sizeof(camera_influence_key_t)};
  case CAMERA_CHANNEL_AIM:
  default:
    return (key_array_t){(char *)camera->aim_keys, &camera->aim_count, CAMERA_MAX_INFLUENCE_KEYS,
                         sizeof(camera_influence_key_t)};
  }
}

static char *slot(key_array_t array, int index) { return array.base + (size_t)index * array.size; }
static double slot_time(key_array_t array, int index) { return *(const double *)slot(array, index); }

// The shared head of every key type.
typedef struct key_head_t {
  double time;
  int interp;
  camera_ease_t ease;
} key_head_t;

static key_head_t *slot_head(key_array_t array, int index) { return (key_head_t *)slot(array, index); }

// Keys live at zero seconds or later, where the timeline can show them.
static int array_insert(key_array_t array, const void *key) {
  const double time = *(const double *)key;
  if (!isfinite(time) || time < 0.0) return -1;
  int at = 0;
  while (at < *array.count && slot_time(array, at) < time - KEY_EPSILON) ++at;
  if (at < *array.count && fabs(slot_time(array, at) - time) <= KEY_EPSILON) {
    memcpy(slot(array, at), key, array.size);
    return at;
  }
  if (*array.count >= array.max) return -1;
  memmove(slot(array, at + 1), slot(array, at), (size_t)(*array.count - at) * array.size);
  memcpy(slot(array, at), key, array.size);
  ++*array.count;
  return at;
}

int camera_channel_count(const camera_timeline_t *camera, camera_channel_t channel) {
  return *channel_array(camera, channel).count;
}

double camera_channel_key_time(const camera_timeline_t *camera, camera_channel_t channel, int index) {
  key_array_t array = channel_array(camera, channel);
  return index >= 0 && index < *array.count ? slot_time(array, index) : 0.0;
}

int *camera_channel_key_interp(camera_timeline_t *camera, camera_channel_t channel, int index) {
  key_array_t array = channel_array(camera, channel);
  return index >= 0 && index < *array.count ? &slot_head(array, index)->interp : NULL;
}

camera_ease_t *camera_channel_key_ease(camera_timeline_t *camera, camera_channel_t channel, int index) {
  key_array_t array = channel_array(camera, channel);
  return index >= 0 && index < *array.count ? &slot_head(array, index)->ease : NULL;
}

int camera_insert_time_key(camera_timeline_t *camera, double time, double game_tick, int interp) {
  if (!isfinite(game_tick)) return -1;
  time = fmax(0.0, time);
  camera_time_key_t key;
  memset(&key, 0, sizeof(key));
  key.time = time;
  key.interp = interp;
  key.game_tick = game_tick;
  return array_insert(channel_array(camera, CAMERA_CHANNEL_TIME), &key);
}

int camera_insert_pose_key(camera_timeline_t *camera, double time, const camera_pose_t *pose, int interp) {
  time = fmax(0.0, time);
  camera_pose_key_t key;
  memset(&key, 0, sizeof(key));
  key.time = time;
  key.interp = interp;
  key.pose = *pose;
  return array_insert(channel_array(camera, CAMERA_CHANNEL_POSE), &key);
}

int camera_insert_influence_key(camera_timeline_t *camera, camera_channel_t channel, double time, float value,
                                int interp) {
  if (channel != CAMERA_CHANNEL_FOLLOW && channel != CAMERA_CHANNEL_AIM) return -1;
  time = fmax(0.0, time);
  camera_influence_key_t key;
  memset(&key, 0, sizeof(key));
  key.time = time;
  key.interp = interp;
  key.value = fminf(1.f, fmaxf(0.f, value));
  return array_insert(channel_array(camera, channel), &key);
}

void camera_delete_key(camera_timeline_t *camera, camera_channel_t channel, int index) {
  key_array_t array = channel_array(camera, channel);
  if (index < 0 || index >= *array.count) return;
  memmove(slot(array, index), slot(array, index + 1), (size_t)(*array.count - index - 1) * array.size);
  --*array.count;
}

int camera_move_key(camera_timeline_t *camera, camera_channel_t channel, int index, double time) {
  key_array_t array = channel_array(camera, channel);
  if (index < 0 || index >= *array.count) return -1;
  if (!isfinite(time)) return index;
  time = fmax(0.0, time);
  for (int i = 0; i < *array.count; ++i)
    if (i != index && fabs(slot_time(array, i) - time) <= KEY_EPSILON) return index;
  union {
    camera_pose_key_t pose;
    camera_time_key_t time;
    camera_influence_key_t influence;
  } moved;
  memcpy(&moved, slot(array, index), array.size);
  moved.pose.time = time; // the common first member
  camera_delete_key(camera, channel, index);
  return array_insert(array, &moved);
}

int camera_find_key(const camera_timeline_t *camera, camera_channel_t channel, double time, double tolerance) {
  key_array_t array = channel_array(camera, channel);
  int best = -1;
  double best_distance = tolerance;
  for (int i = 0; i < *array.count; ++i) {
    const double distance = fabs(slot_time(array, i) - time);
    if (distance <= best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

void camera_ripple(camera_timeline_t *camera, double from, double delta) {
  for (int channel = 0; channel < CAMERA_CHANNEL_COUNT; ++channel) {
    key_array_t array = channel_array(camera, (camera_channel_t)channel);
    for (int i = 0; i < *array.count; ++i)
      if (slot_time(array, i) >= from - KEY_EPSILON) *(double *)slot(array, i) = fmax(0.0, slot_time(array, i) + delta);
  }
  if (camera->range_set && camera->range_end >= from) camera->range_end += delta;
}

double camera_last_key_time(const camera_timeline_t *camera) {
  double last = 0.0;
  for (int channel = 0; channel < CAMERA_CHANNEL_COUNT; ++channel) {
    key_array_t array = channel_array(camera, (camera_channel_t)channel);
    if (*array.count) last = fmax(last, slot_time(array, *array.count - 1));
  }
  return last;
}

// The segment [index, index + 1] that contains `time`, for keys[0] <= time <
// keys[count - 1].
static int find_segment(key_array_t array, double time) {
  int low = 0, high = *array.count - 1;
  while (high - low > 1) {
    const int middle = (low + high) / 2;
    if (slot_time(array, middle) <= time) low = middle;
    else high = middle;
  }
  return low;
}

// Spatial path

static const float *pose_point(const camera_pose_key_t *key, int point) {
  return point ? key->pose.target : key->pose.eye;
}

static float distance3(const float a[3], const float b[3]) {
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return sqrtf(x * x + y * y + z * z);
}

// Handles that round the path through a key without overshooting on uneven
// spacing: along the line between the neighbours, each as long as a third of
// its own segment. A key that repeats a neighbour's position is a corner, so
// the camera stops there instead of swinging past.
static void auto_spatial(const camera_timeline_t *camera, int index, int point, float in[3], float out[3]) {
  const camera_pose_key_t *keys = camera->pose_keys;
  const int count = camera->pose_count;
  memset(in, 0, sizeof(float) * 3);
  memset(out, 0, sizeof(float) * 3);
  const bool has_previous = index > 0 && keys[index - 1].interp != CAMERA_INTERP_HOLD;
  const bool has_next = index + 1 < count && keys[index].interp != CAMERA_INTERP_HOLD;
  const float *p = pose_point(&keys[index], point);
  const float *a = has_previous ? pose_point(&keys[index - 1], point) : NULL;
  const float *b = has_next ? pose_point(&keys[index + 1], point) : NULL;
  if (a && b) {
    const float before = distance3(p, a), after = distance3(p, b);
    const float span = distance3(a, b);
    if (before < 1e-5f || after < 1e-5f || span < 1e-5f) return;
    for (int i = 0; i < 3; ++i) {
      const float direction = (b[i] - a[i]) / span;
      out[i] = direction * after / 3.f;
      in[i] = -direction * before / 3.f;
    }
  } else if (b) {
    for (int i = 0; i < 3; ++i) out[i] = (b[i] - p[i]) / 3.f;
  } else if (a) {
    for (int i = 0; i < 3; ++i) in[i] = (a[i] - p[i]) / 3.f;
  }
}

camera_spatial_t camera_pose_key_spatial(const camera_timeline_t *camera, int index) {
  camera_spatial_t out;
  memset(&out, 0, sizeof(out));
  if (index < 0 || index >= camera->pose_count) return out;
  if (camera->pose_keys[index].spatial.mode != CAMERA_HANDLE_AUTO) return camera->pose_keys[index].spatial;
  auto_spatial(camera, index, 0, out.eye_in, out.eye_out);
  auto_spatial(camera, index, 1, out.target_in, out.target_out);
  return out;
}

void camera_set_spatial_mode(camera_timeline_t *camera, int index, int mode) {
  if (index < 0 || index >= camera->pose_count || mode < 0 || mode >= CAMERA_HANDLE_COUNT) return;
  camera_spatial_t *spatial = &camera->pose_keys[index].spatial;
  if (spatial->mode == CAMERA_HANDLE_AUTO && mode != CAMERA_HANDLE_AUTO) *spatial = camera_pose_key_spatial(camera, index);
  if (mode == CAMERA_HANDLE_ALIGNED && spatial->mode == CAMERA_HANDLE_FREE) {
    // Line the in handle up behind the out handle, keeping both lengths.
    float *pairs[2][2] = {{spatial->eye_in, spatial->eye_out}, {spatial->target_in, spatial->target_out}};
    for (int p = 0; p < 2; ++p) {
      const float zero[3] = {0};
      const float in_length = distance3(pairs[p][0], zero), out_length = distance3(pairs[p][1], zero);
      if (out_length < 1e-6f) continue;
      for (int i = 0; i < 3; ++i) pairs[p][0][i] = -pairs[p][1][i] / out_length * in_length;
    }
  }
  spatial->mode = mode;
}

void camera_set_spatial_handle(camera_timeline_t *camera, int index, int point, int side, const float offset[3]) {
  if (index < 0 || index >= camera->pose_count) return;
  camera_spatial_t *spatial = &camera->pose_keys[index].spatial;
  if (spatial->mode == CAMERA_HANDLE_AUTO) camera_set_spatial_mode(camera, index, CAMERA_HANDLE_ALIGNED);
  float *moved = point ? (side < 0 ? spatial->target_in : spatial->target_out) : (side < 0 ? spatial->eye_in : spatial->eye_out);
  float *other = point ? (side < 0 ? spatial->target_out : spatial->target_in) : (side < 0 ? spatial->eye_out : spatial->eye_in);
  memcpy(moved, offset, sizeof(float) * 3);
  if (spatial->mode == CAMERA_HANDLE_ALIGNED) {
    const float zero[3] = {0};
    const float moved_length = distance3(moved, zero), other_length = distance3(other, zero);
    if (moved_length > 1e-6f)
      for (int i = 0; i < 3; ++i) other[i] = -moved[i] / moved_length * other_length;
  }
}

static void bezier3(const float p0[3], const float p1[3], const float p2[3], const float p3[3], double s, float out[3]) {
  const double r = 1.0 - s;
  const double a = r * r * r, b = 3.0 * r * r * s, c = 3.0 * r * s * s, d = s * s * s;
  for (int i = 0; i < 3; ++i) out[i] = (float)(a * p0[i] + b * p1[i] + c * p2[i] + d * p3[i]);
}

// The four control points of the path from key `index` to the next.
static void segment_controls(const camera_timeline_t *camera, int index, int point, float p[4][3]) {
  const camera_spatial_t a = camera_pose_key_spatial(camera, index);
  const camera_spatial_t b = camera_pose_key_spatial(camera, index + 1);
  const float *start = pose_point(&camera->pose_keys[index], point);
  const float *end = pose_point(&camera->pose_keys[index + 1], point);
  const float *out = point ? a.target_out : a.eye_out;
  const float *in = point ? b.target_in : b.eye_in;
  for (int i = 0; i < 3; ++i) {
    p[0][i] = start[i];
    p[1][i] = start[i] + out[i];
    p[2][i] = end[i] + in[i];
    p[3][i] = end[i];
  }
}

// Arc length along the segment at each sample, for moving along it evenly.
static float segment_table(const float p[4][3], float lengths[PATH_SAMPLES + 1]) {
  float previous[3];
  memcpy(previous, p[0], sizeof(previous));
  lengths[0] = 0.f;
  for (int i = 1; i <= PATH_SAMPLES; ++i) {
    float next[3];
    bezier3(p[0], p[1], p[2], p[3], (double)i / PATH_SAMPLES, next);
    lengths[i] = lengths[i - 1] + distance3(previous, next);
    memcpy(previous, next, sizeof(previous));
  }
  return lengths[PATH_SAMPLES];
}

void camera_path_point(const camera_timeline_t *camera, int index, int point, double u, float out[3]) {
  float p[4][3], lengths[PATH_SAMPLES + 1];
  segment_controls(camera, index, point, p);
  const float total = segment_table(p, lengths);
  u = fmin(1.0, fmax(0.0, u));
  double s = u;
  if (total > 1e-6f) {
    const float wanted = (float)u * total;
    int i = 1;
    while (i < PATH_SAMPLES && lengths[i] < wanted) ++i;
    const float span = lengths[i] - lengths[i - 1];
    const double within = span > 1e-9f ? (wanted - lengths[i - 1]) / span : 0.0;
    s = ((double)(i - 1) + within) / PATH_SAMPLES;
  }
  bezier3(p[0], p[1], p[2], p[3], s, out);
}

// How much the picture changes between key `index` and the next, in world
// units: whichever of the eye and the look-at point moves further, combined
// with how far a zoom, a lens change or a roll sweeps the scene. The timing
// runs over this, so a zoom on the spot is as much a move as a pan.
static double segment_length(const camera_timeline_t *camera, int index) {
  float p[4][3], lengths[PATH_SAMPLES + 1];
  segment_controls(camera, index, 0, p);
  const float eye = segment_table(p, lengths);
  segment_controls(camera, index, 1, p);
  const float target = segment_table(p, lengths);
  const camera_pose_t *a = &camera->pose_keys[index].pose, *b = &camera->pose_keys[index + 1].pose;
  // 3D: the lens and roll sweep the scene at the distance the camera looks.
  const double reach = 0.5 * (distance3(a->eye, a->target) + distance3(b->eye, b->target));
  const double lens = fabs((double)b->fov_y - a->fov_y) * reach;
  const double roll = fabs((double)b->roll - a->roll) * reach * 0.5;
  // 2D: a zoom sweeps its share of the view's width.
  const double za = fmax(a->zoom, 1e-4), zb = fmax(b->zoom, 1e-4);
  const double scale = camera->view_scale > 0.f ? camera->view_scale : 1.0;
  const double zoom = fabs(log(zb / za)) * scale / sqrt(za * zb);
  const double moved = fmaxf(eye, target);
  return fmax(sqrt(moved * moved + lens * lens + roll * roll + zoom * zoom), MIN_SEGMENT_LENGTH);
}

void camera_pose_distances(const camera_timeline_t *camera, double out[CAMERA_MAX_POSE_KEYS]) {
  if (camera->pose_count == 0) return;
  out[0] = 0.0;
  for (int i = 1; i < camera->pose_count; ++i) out[i] = out[i - 1] + segment_length(camera, i - 1);
}

// Temporal curves

// A plain view of one key of any channel as a point on a curve.
typedef struct curve_key_t {
  double time, value;
  int interp;
  camera_ease_t ease;
} curve_key_t;

// A window of keys around a segment, with the values the curve passes through.
// Pose values are distances along the path, measured from `base` so only the
// few segments nearby need measuring.
enum { LENGTH_CACHE = 8 };

typedef struct curve_t {
  const camera_timeline_t *camera;
  camera_channel_t channel;
  key_array_t array;
  int count;
  double tps;
  int base;
  double base_distance;
  const double *distances; // all of them, when the caller has them
  // Segment lengths near `base`, measured once per evaluation.
  double lengths[LENGTH_CACHE];
  unsigned measured;
} curve_t;

static curve_t curve_for(const camera_timeline_t *camera, camera_channel_t channel, double tps, const double *distances) {
  curve_t curve;
  memset(&curve, 0, sizeof(curve));
  curve.camera = camera;
  curve.channel = channel;
  curve.array = channel_array(camera, channel);
  curve.count = *curve.array.count;
  curve.tps = tps;
  curve.distances = distances;
  return curve;
}

static void curve_rebase(curve_t *curve, int base) {
  curve->base = base;
  curve->measured = 0;
}

// Pose segment `index`'s length, from the caller's distances, the cache, or
// by measuring it.
static double curve_length(curve_t *curve, int index) {
  if (curve->distances) return curve->distances[index + 1] - curve->distances[index];
  const int slot = index - curve->base + LENGTH_CACHE / 2;
  if (slot < 0 || slot >= LENGTH_CACHE) return segment_length(curve->camera, index);
  if (!(curve->measured & (1u << slot))) {
    curve->lengths[slot] = segment_length(curve->camera, index);
    curve->measured |= 1u << slot;
  }
  return curve->lengths[slot];
}

static double key_value(curve_t *curve, int index) {
  switch (curve->channel) {
  case CAMERA_CHANNEL_TIME: return curve->camera->time_keys[index].game_tick;
  case CAMERA_CHANNEL_FOLLOW: return curve->camera->follow_keys[index].value;
  case CAMERA_CHANNEL_AIM: return curve->camera->aim_keys[index].value;
  case CAMERA_CHANNEL_POSE:
  default: {
    if (curve->distances) return curve->distances[index];
    double value = curve->base_distance;
    for (int i = curve->base; i < index; ++i) value += curve_length(curve, i);
    for (int i = index; i < curve->base; ++i) value -= curve_length(curve, i);
    return value;
  }
  }
}

static curve_key_t curve_key(curve_t *curve, int index) {
  const key_head_t *head = slot_head(curve->array, index);
  return (curve_key_t){head->time, key_value(curve, index), head->interp, head->ease};
}

// A slope that never overshoots: zero at a turning point or a flat stretch,
// otherwise the gentler of the two sides. Motion stays continuous through the
// key, and the slower side keeps its own pace instead of rushing to meet the
// faster one.
static double monotone_slope(double left, double right) {
  if (left * right <= 0.0) return 0.0;
  return left > 0.0 ? fmin(left, right) : fmax(left, right);
}

// The handles an auto key gets. Inside a run of keys the curve passes through
// with a continuous, non-overshooting slope. Where the curve starts or ends,
// game time eases into the 1x it continues at, and everything else comes to
// rest. A hold cuts the run, and a key after one continues along its segment.
static camera_ease_t auto_ease(curve_t *curve, int index) {
  const curve_key_t key = curve_key(curve, index);
  const bool has_left = index > 0 && slot_head(curve->array, index - 1)->interp != CAMERA_INTERP_HOLD;
  const bool has_right = index + 1 < curve->count && key.interp != CAMERA_INTERP_HOLD;
  double left = 0.0, right = 0.0;
  if (has_left) {
    const curve_key_t previous = curve_key(curve, index - 1);
    left = (key.value - previous.value) / (key.time - previous.time);
  }
  if (has_right) {
    const curve_key_t next = curve_key(curve, index + 1);
    right = (next.value - key.value) / (next.time - key.time);
  }
  double slope = 0.0;
  if (has_left && has_right) slope = monotone_slope(left, right);
  else if (curve->channel == CAMERA_CHANNEL_TIME) {
    const bool open_end = index == 0 || index + 1 == curve->count;
    const double other = has_left ? left : has_right ? right : curve->tps;
    slope = open_end ? monotone_slope(other, curve->tps) : other;
  }
  camera_ease_t ease = {CAMERA_HANDLE_AUTO, (float)slope, (float)slope, AUTO_INFLUENCE, AUTO_INFLUENCE};
  return ease;
}

// The time and distance of the pose segment on one side of a key. False when
// there is none.
static bool pose_side(curve_t *curve, int index, int side, double *span, double *length) {
  const int other = index + side;
  if (other < 0 || other >= curve->count) return false;
  const int first = side < 0 ? other : index;
  *span = slot_time(curve->array, first + 1) - slot_time(curve->array, first);
  *length = curve_length(curve, first);
  return *span > 0.0 && *length > 0.0;
}

// A rate in distance per second as a multiple of the side's average speed, and back.
static float pose_to_relative(curve_t *curve, int index, int side, double slope) {
  double span, length;
  return pose_side(curve, index, side, &span, &length) ? (float)(slope * span / length) : 0.f;
}

static float pose_to_absolute(curve_t *curve, int index, int side, double relative) {
  double span, length;
  return pose_side(curve, index, side, &span, &length) ? (float)(relative * length / span) : 0.f;
}

// A pose key's ease with its rates relative, as stored.
static camera_ease_t pose_relative_ease(curve_t *curve, int index) {
  const camera_ease_t *stored = &slot_head(curve->array, index)->ease;
  if (stored->mode != CAMERA_HANDLE_AUTO) return *stored;
  camera_ease_t ease = auto_ease(curve, index);
  ease.in_slope = pose_to_relative(curve, index, -1, ease.in_slope);
  ease.out_slope = pose_to_relative(curve, index, 1, ease.out_slope);
  return ease;
}

// Any key's ease in value per second.
static camera_ease_t resolved_ease(curve_t *curve, int index) {
  const camera_ease_t *stored = &slot_head(curve->array, index)->ease;
  if (curve->channel != CAMERA_CHANNEL_POSE) return stored->mode == CAMERA_HANDLE_AUTO ? auto_ease(curve, index) : *stored;
  camera_ease_t ease = pose_relative_ease(curve, index);
  ease.in_slope = pose_to_absolute(curve, index, -1, ease.in_slope);
  ease.out_slope = pose_to_absolute(curve, index, 1, ease.out_slope);
  return ease;
}

// A segment of a curve as a cubic bezier in (time, value). Handles reaching
// further than the segment are scaled back so time never runs backwards.
static double ease_segment(const curve_key_t *a, const camera_ease_t *out, const curve_key_t *b, const camera_ease_t *in,
                           double time) {
  const double span = b->time - a->time;
  double out_influence = fmin(1.0, fmax(0.001, out->out_influence));
  double in_influence = fmin(1.0, fmax(0.001, in->in_influence));
  if (out_influence + in_influence > 1.0) {
    const double scale = 1.0 / (out_influence + in_influence);
    out_influence *= scale;
    in_influence *= scale;
  }
  const double x1 = out_influence, x2 = 1.0 - in_influence; // in units of the span
  const double y0 = a->value, y1 = a->value + out->out_slope * out_influence * span;
  const double y2 = b->value - in->in_slope * in_influence * span, y3 = b->value;
  const double target = (time - a->time) / span;
  // x(s) is increasing, so bisection finds the curve parameter for this time.
  double low = 0.0, high = 1.0, s = target;
  for (int iteration = 0; iteration < 40; ++iteration) {
    s = 0.5 * (low + high);
    const double r = 1.0 - s;
    const double x = 3.0 * r * r * s * x1 + 3.0 * r * s * s * x2 + s * s * s;
    if (x < target) low = s;
    else high = s;
  }
  s = 0.5 * (low + high);
  const double r = 1.0 - s;
  return r * r * r * y0 + 3.0 * r * r * s * y1 + 3.0 * r * s * s * y2 + s * s * s * y3;
}

// The value of a curve inside the segment from key `index`.
static double curve_segment_value(curve_t *curve, int index, double time) {
  const curve_key_t a = curve_key(curve, index), b = curve_key(curve, index + 1);
  const double span = b.time - a.time;
  if (a.interp == CAMERA_INTERP_HOLD || span <= 0.0) return a.value;
  if (a.interp == CAMERA_INTERP_LINEAR) return a.value + (b.value - a.value) * (time - a.time) / span;
  if (curve->channel != CAMERA_CHANNEL_POSE) {
    const camera_ease_t out = resolved_ease(curve, index), in = resolved_ease(curve, index + 1);
    return ease_segment(&a, &out, &b, &in, time);
  }
  // The pose's progress through the segment, 0 to 1, from its relative rates:
  // a segment the camera barely moves along still eases as it was shaped.
  camera_ease_t out = pose_relative_ease(curve, index), in = pose_relative_ease(curve, index + 1);
  out.out_slope /= (float)span;
  in.in_slope /= (float)span;
  const curve_key_t start = {a.time, 0.0, a.interp, a.ease}, end = {b.time, 1.0, b.interp, b.ease};
  return a.value + (b.value - a.value) * ease_segment(&start, &out, &end, &in, time);
}

camera_ease_t camera_curve_key_ease(const camera_timeline_t *camera, camera_channel_t channel, int index, double tps) {
  curve_t curve = curve_for(camera, channel, tps, NULL);
  camera_ease_t none;
  memset(&none, 0, sizeof(none));
  if (index < 0 || index >= curve.count) return none;
  curve_rebase(&curve, index);
  return resolved_ease(&curve, index);
}

// Stores a rate given in value per second on one side of a key.
static void store_slope(curve_t *curve, int index, int side, float slope) {
  camera_ease_t *ease = &slot_head(curve->array, index)->ease;
  const float stored = curve->channel == CAMERA_CHANNEL_POSE ? pose_to_relative(curve, index, side, slope) : slope;
  if (side < 0) ease->in_slope = stored;
  else ease->out_slope = stored;
}

void camera_set_ease_mode(camera_timeline_t *camera, camera_channel_t channel, int index, int mode, double tps) {
  camera_ease_t *ease = camera_channel_key_ease(camera, channel, index);
  if (!ease || mode < 0 || mode >= CAMERA_HANDLE_COUNT) return;
  curve_t curve = curve_for(camera, channel, tps, NULL);
  curve_rebase(&curve, index);
  if (ease->mode == CAMERA_HANDLE_AUTO && mode != CAMERA_HANDLE_AUTO)
    *ease = channel == CAMERA_CHANNEL_POSE ? pose_relative_ease(&curve, index) : auto_ease(&curve, index);
  if (mode == CAMERA_HANDLE_ALIGNED && ease->mode == CAMERA_HANDLE_FREE) {
    // One speed through the key: the mean of the two sides.
    const camera_ease_t current = resolved_ease(&curve, index);
    const float mean = 0.5f * (current.in_slope + current.out_slope);
    store_slope(&curve, index, -1, mean);
    store_slope(&curve, index, 1, mean);
  }
  ease->mode = mode;
}

void camera_set_ease_handle(camera_timeline_t *camera, camera_channel_t channel, int index, int side, float slope,
                            float influence, double tps) {
  camera_ease_t *ease = camera_channel_key_ease(camera, channel, index);
  if (!ease || !isfinite(slope) || !isfinite(influence)) return;
  if (ease->mode == CAMERA_HANDLE_AUTO) camera_set_ease_mode(camera, channel, index, CAMERA_HANDLE_ALIGNED, tps);
  curve_t curve = curve_for(camera, channel, tps, NULL);
  curve_rebase(&curve, index);
  influence = fminf(1.f, fmaxf(0.01f, influence));
  if (side < 0) ease->in_influence = influence;
  else ease->out_influence = influence;
  store_slope(&curve, index, side, slope);
  // Aligned keys pass through at one speed.
  if (ease->mode == CAMERA_HANDLE_ALIGNED) store_slope(&curve, index, -side, slope);
}

void camera_pose_eases_from_absolute(camera_timeline_t *camera) {
  curve_t curve = curve_for(camera, CAMERA_CHANNEL_POSE, 0.0, NULL);
  for (int i = 0; i < camera->pose_count; ++i) {
    camera_ease_t *ease = &camera->pose_keys[i].ease;
    if (ease->mode == CAMERA_HANDLE_AUTO) continue;
    curve_rebase(&curve, i);
    ease->in_slope = pose_to_relative(&curve, i, -1, ease->in_slope);
    ease->out_slope = pose_to_relative(&curve, i, 1, ease->out_slope);
  }
}

void camera_easy_ease(camera_timeline_t *camera, camera_channel_t channel, int index) {
  camera_ease_t *ease = camera_channel_key_ease(camera, channel, index);
  if (!ease) return;
  *ease = (camera_ease_t){CAMERA_HANDLE_ALIGNED, 0.f, 0.f, AUTO_INFLUENCE, AUTO_INFLUENCE};
  int *interp = camera_channel_key_interp(camera, channel, index);
  if (*interp == CAMERA_INTERP_LINEAR) *interp = CAMERA_INTERP_SMOOTH;
}

// Game time

double camera_game_tick(const camera_timeline_t *camera, double seconds, double tps) {
  const int count = camera->time_count;
  const camera_time_key_t *keys = camera->time_keys;
  if (count == 0) return seconds * tps;
  if (seconds <= keys[0].time) return keys[0].game_tick + (seconds - keys[0].time) * tps;
  const camera_time_key_t *last = &keys[count - 1];
  if (seconds >= last->time)
    return last->interp == CAMERA_INTERP_HOLD ? last->game_tick : last->game_tick + (seconds - last->time) * tps;
  curve_t curve = curve_for(camera, CAMERA_CHANNEL_TIME, tps, NULL);
  return curve_segment_value(&curve, find_segment(curve.array, seconds), seconds);
}

// Each piece of the remap is searched for the tick by bisection, which finds
// it wherever the piece is monotone; auto handles always are.
static bool solve_piece(const camera_timeline_t *camera, double tick, double tps, double a, double b, double *out) {
  const double ga = camera_game_tick(camera, a, tps);
  // Just before b: a hold jumps exactly at b, and b belongs to the next piece.
  const double gb = camera_game_tick(camera, b - (b - a) * 1e-9, tps);
  if (fabs(ga - tick) < 1e-9) {
    *out = a;
    return true;
  }
  if ((tick - ga) * (tick - gb) > 0.0) return false;
  if (ga == gb) return false;
  const bool rising = gb > ga;
  double low = a, high = b;
  for (int iteration = 0; iteration < 60; ++iteration) {
    const double middle = 0.5 * (low + high);
    const double value = camera_game_tick(camera, middle, tps);
    if ((value < tick) == rising) low = middle;
    else high = middle;
  }
  *out = 0.5 * (low + high);
  return true;
}

bool camera_seconds_for_game_tick(const camera_timeline_t *camera, double tick, double tps, double from,
                                  double *out_seconds) {
  if (!isfinite(tick) || tps <= 0.0) return false;
  const int count = camera->time_count;
  const camera_time_key_t *keys = camera->time_keys;
  double seconds;
  if (count == 0) {
    seconds = tick / tps;
    if (seconds < from) return false;
    *out_seconds = seconds;
    return true;
  }
  // Before the first key: 1x.
  if (from < keys[0].time) {
    seconds = keys[0].time + (tick - keys[0].game_tick) / tps;
    if (seconds >= from && seconds < keys[0].time) {
      *out_seconds = seconds;
      return true;
    }
  }
  for (int i = 0; i + 1 < count; ++i) {
    const double a = fmax(keys[i].time, from), b = keys[i + 1].time;
    if (a < b && solve_piece(camera, tick, tps, a, b, out_seconds)) return true;
  }
  const camera_time_key_t *last = &keys[count - 1];
  if (last->interp == CAMERA_INTERP_HOLD) {
    if (fabs(tick - last->game_tick) > 1e-9) return false;
    *out_seconds = fmax(from, last->time);
    return true;
  }
  seconds = last->time + (tick - last->game_tick) / tps;
  if (seconds < fmax(from, last->time)) return false;
  *out_seconds = seconds;
  return true;
}

// Pose

static double hermite(double a, double b, double tangent_a, double tangent_b, double s) {
  const double s2 = s * s, s3 = s2 * s;
  return (2.0 * s3 - 3.0 * s2 + 1.0) * a + (s3 - 2.0 * s2 + s) * tangent_a + (-2.0 * s3 + 3.0 * s2) * b +
         (s3 - s2) * tangent_b;
}

typedef float (*pose_scalar_fn)(const camera_pose_t *pose);
static float pose_roll(const camera_pose_t *pose) { return pose->roll; }
static float pose_fov(const camera_pose_t *pose) { return pose->fov_y; }
// Zoom is a scale: interpolating its logarithm makes a zoom from 1 to 4 pass
// 2 halfway, as the eye expects.
static float pose_log_zoom(const camera_pose_t *pose) { return logf(fmaxf(pose->zoom, 1e-4f)); }

// Roll, lens and zoom ride along with the camera's progress between keys,
// smoothly through each key and at rest at the ends of a shot.
static double scalar_tangent(const camera_pose_key_t *keys, int count, int index, pose_scalar_fn value) {
  const bool has_left = index > 0 && keys[index - 1].interp != CAMERA_INTERP_HOLD;
  const bool has_right = index + 1 < count && keys[index].interp != CAMERA_INTERP_HOLD;
  if (!has_left || !has_right) return 0.0;
  return monotone_slope(value(&keys[index].pose) - value(&keys[index - 1].pose),
                        value(&keys[index + 1].pose) - value(&keys[index].pose));
}

static float blend_scalar(const camera_pose_key_t *keys, int count, int i, double u, bool smooth, pose_scalar_fn value) {
  const double a = value(&keys[i].pose), b = value(&keys[i + 1].pose);
  if (!smooth) return (float)(a + (b - a) * u);
  return (float)hermite(a, b, scalar_tangent(keys, count, i, value), scalar_tangent(keys, count, i + 1, value), u);
}

camera_pose_t camera_eval_pose(const camera_timeline_t *camera, double seconds) {
  const int count = camera->pose_count;
  const camera_pose_key_t *keys = camera->pose_keys;
  if (count == 0) {
    camera_pose_t pose = {.zoom = 1.f, .fov_y = 1.f};
    return pose;
  }
  if (seconds <= keys[0].time) return keys[0].pose;
  if (seconds >= keys[count - 1].time) return keys[count - 1].pose;

  curve_t curve = curve_for(camera, CAMERA_CHANNEL_POSE, 0.0, NULL);
  const int i = find_segment(curve.array, seconds);
  const camera_pose_key_t *a = &keys[i];
  if (a->interp == CAMERA_INTERP_HOLD) return a->pose;

  // How far along this segment's path the camera is, then where that is.
  curve_rebase(&curve, i);
  const double length = segment_length(camera, i);
  const double u = fmin(1.0, fmax(0.0, curve_segment_value(&curve, i, seconds) / length));
  camera_pose_t out = a->pose;
  camera_path_point(camera, i, 0, u, out.eye);
  camera_path_point(camera, i, 1, u, out.target);
  const bool smooth = a->interp == CAMERA_INTERP_SMOOTH;
  out.roll = blend_scalar(keys, count, i, u, smooth, pose_roll);
  out.fov_y = blend_scalar(keys, count, i, u, smooth, pose_fov);
  out.zoom = expf(blend_scalar(keys, count, i, u, smooth, pose_log_zoom));
  return out;
}

void camera_even_speed(camera_timeline_t *camera) {
  camera_pose_key_t *keys = camera->pose_keys;
  const int count = camera->pose_count;
  double distances[CAMERA_MAX_POSE_KEYS];
  camera_pose_distances(camera, distances);
  int first = 0;
  while (first < count) {
    int last = first;
    while (last + 1 < count && keys[last].interp != CAMERA_INTERP_HOLD) ++last;
    const double travel = distances[last] - distances[first], span = keys[last].time - keys[first].time;
    if (last - first >= 2 && travel > 0.0 && span > 0.0)
      for (int i = first + 1; i < last; ++i) {
        keys[i].time = keys[first].time + (distances[i] - distances[first]) / travel * span;
        keys[i].ease.mode = CAMERA_HANDLE_AUTO;
      }
    first = last + 1;
  }
}

// Curves for the graph editor

double camera_curve_key_value(const camera_timeline_t *camera, camera_channel_t channel, int index,
                              const double *distances) {
  curve_t curve = curve_for(camera, channel, 0.0, distances);
  if (index < 0 || index >= curve.count) return 0.0;
  return key_value(&curve, index);
}

double camera_curve_value(const camera_timeline_t *camera, camera_channel_t channel, double seconds, double tps,
                          const double *distances) {
  if (channel == CAMERA_CHANNEL_TIME) return camera_game_tick(camera, seconds, tps);
  curve_t curve = curve_for(camera, channel, tps, distances);
  if (curve.count == 0) return 0.0;
  if (seconds <= slot_time(curve.array, 0)) return key_value(&curve, 0);
  if (seconds >= slot_time(curve.array, curve.count - 1)) return key_value(&curve, curve.count - 1);
  const int i = find_segment(curve.array, seconds);
  if (channel == CAMERA_CHANNEL_POSE && !distances) {
    // Measure only from the segment's start, then add what came before.
    double before = 0.0;
    for (int k = 0; k < i; ++k) before += segment_length(camera, k);
    curve_rebase(&curve, i);
    curve.base_distance = before;
  }
  const double value = curve_segment_value(&curve, i, seconds);
  return channel == CAMERA_CHANNEL_POSE ? value : fmin(1.0, fmax(0.0, value));
}

// Influence

static float eval_influence(const camera_timeline_t *camera, camera_channel_t channel, double seconds) {
  return (float)camera_curve_value(camera, channel, seconds, 0.0, NULL);
}

float camera_eval_follow(const camera_timeline_t *camera, double seconds) {
  return eval_influence(camera, CAMERA_CHANNEL_FOLLOW, seconds);
}

float camera_eval_aim(const camera_timeline_t *camera, double seconds) {
  return eval_influence(camera, CAMERA_CHANNEL_AIM, seconds);
}
