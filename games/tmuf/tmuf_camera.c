// The view: the game's race camera behind the car, and an orbit the user
// drives.
//
// The race camera is the vehicle's first camera ("Close",
// CGameControlCameraTrackManiaRace3 in Stadium, Island, Bay and Coast): a
// 75 degree lens a few metres behind the car, looking over it. Its lens is set
// here as the game sets it (75 degrees; near plane 0.2 m), so nothing close to the car is
// clipped on large maps where the engine's own near plane grows.

#include "tmuf_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAMERA_CHASE, CAMERA_ORBIT };

const ft_camera_mode tm_camera_modes[] = {
    {"chase", "Race", "The game's race camera, behind the car", FT_CAMERA_MODE_DIRECTED},
    {"orbit", "Orbit", "Drag to turn around the car, scroll to pull back", FT_CAMERA_MODE_FREE},
};
const uint32_t tm_camera_mode_count = 2;

#define LENS_FOV_DEG 75.f
#define LENS_NEAR 0.2f // CHmsCamera's frustum in the race
#define LENS_FAR 50000.f

typedef struct v3 {
  float x, y, z;
} v3;

static v3 sub(v3 a, v3 b) { return (v3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static v3 norm(v3 a) {
  const float l = sqrtf(dot(a, a));
  return l > 0.f ? (v3){a.x / l, a.y / l, a.z / l} : a;
}

// view_proj as the engine builds it (right-handed look-at, reversed depth,
// Vulkan's y), column-major
static void set_lens(ft_camera *c, float fov_y) {
  const v3 eye = {c->eye.x, c->eye.y, c->eye.z}, target = {c->target.x, c->target.y, c->target.z};
  const v3 f = norm(sub(target, eye));
  const v3 s = norm(cross(f, (v3){c->up.x, c->up.y, c->up.z}));
  const v3 u = cross(s, f);
  const float view[16] = {s.x, u.x, -f.x, 0.f, s.y, u.y, -f.y, 0.f,
                          s.z, u.z, -f.z, 0.f, -dot(s, eye), -dot(u, eye), dot(f, eye), 1.f};
  float aspect = c->viewport.y > 0.f ? c->viewport.x / c->viewport.y : c->aspect > 0.f ? c->aspect : 16.f / 9.f;
  const float t = 1.f / tanf(fov_y * 0.5f);
  // perspective_rh_zo with near and far swapped: depth 1 at the near plane
  const float n = LENS_FAR, fa = LENS_NEAR;
  float proj[16] = {0};
  proj[0] = t / aspect;
  proj[5] = -t; // y down
  proj[10] = fa / (n - fa);
  proj[11] = -1.f;
  proj[14] = n * fa / (n - fa);
  // Direct3D 9's pixel centres are at whole window coordinates, Vulkan's
  // half a pixel further: the picture moved half a pixel right and down, as
  // the game's (its high-frequency textures, the Stadium's gratings, line up)
  if (c->viewport.x > 0.f && c->viewport.y > 0.f) {
    proj[8] = -1.f / c->viewport.x;  // x += w / width
    proj[9] = -1.f / c->viewport.y;  // y (down) += w / height
  }
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++)
        sum += proj[k * 4 + row] * view[col * 4 + k];
      c->view_proj[col * 4 + row] = sum;
    }
  c->forward = (ft_vec3){f.x, f.y, f.z};
  c->fov_y = fov_y;
  c->near_z = LENS_NEAR;
  c->far_z = LENS_FAR;
  c->orthographic = false;
  c->use_view_proj = true;
}

// Every perspective view is drawn with the game's lens near plane: the
// engine's own grows with the map (up to 50 m) and cuts off what is close to
// a free camera.
void tm_camera_lens(ft_camera *c) {
  if (c->orthographic || c->fov_y <= 0.f || c->near_z <= LENS_NEAR) return;
  set_lens(c, c->fov_y);
}

// For comparing pictures with the game's (games/tmuf/tools/refshot):
// TM_TEST_CAMERA="ex ey ez tx ty tz fov" holds the race camera at that eye,
// looking at that target, with that vertical fov in degrees.
static bool test_camera(ft_camera *inout) {
  const char *spec = getenv("TM_TEST_CAMERA");
  float v[7];
  if (!spec || sscanf(spec, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7)
    return false;
  inout->eye = (ft_vec3){v[0], v[1], v[2]};
  inout->target = (ft_vec3){v[3], v[4], v[5]};
  inout->up = (ft_vec3){0.f, 1.f, 0.f};
  set_lens(inout, v[6] * 3.14159265f / 180.f);
  return true;
}

// --- the race camera (tmuf_work/camera_spec.md) --------------------------------
//
// CGameControlCameraTrackManiaRace3 (Stadium, Island, Bay, Coast) and the old
// CGameControlCameraTrackManiaRace (Alpine, Speed, Rally), as the game runs
// them once per rendered frame on the car's interpolated pose, with the
// game's float stores. The game's `now` is the render time in whole
// milliseconds; after a jump on the timeline the camera is rebuilt from the
// three seconds before (its longest ramps), a tick at a time, from the
// timeline's worlds. Not reproduced: the Stadium's ground probe (a segment
// cast the library does not offer yet).

#define PI_F 3.14159274101257324 // (double)(float)pi

typedef struct ramp { // SGmSmoothReal2
  float delta;
  uint32_t up, down; // ms
  float cur_x, value;
  uint32_t t0;
  float v0;
  int state, kind;
} ramp;

static float cur_val(int kind, float x) {
  x = x < 0.f ? 0.f : x > 1.f ? 1.f : x;
  const float c = (float)cos((double)(float)(x * PI_F));
  return kind ? (float)((1.0 - c) * 0.5) : (float)((c + 1.0) * 0.5);
}

static float cur_x_of(int kind, float x) {
  x = x <= 1e-5f ? 1e-5f : x >= 0.99999f ? 0.99999f : x;
  const float a = kind ? (float)acos((double)(float)(1.0 - 2.0 * x)) : (float)acos((double)(float)(2.0 * x - 1.0));
  return (float)(a / PI_F);
}

static float ramp_update(ramp *r, bool on, uint32_t now) {
  if (on) {
    if (r->state == 0) {
      r->cur_x = cur_x_of(r->kind, (float)fabs((double)(float)(r->value / r->delta)));
      r->state = r->kind = 1, r->t0 = now, r->v0 = r->value;
    }
    if (fabsf(r->delta) > fabsf(r->value) && now >= r->t0 && fabsf(r->cur_x) > 1e-5f && r->up) {
      const float f = cur_val(r->kind, (float)((double)(now - r->t0) / ((double)r->up * r->cur_x)));
      r->value = (float)(f * (r->delta - r->v0) + r->v0);
    }
  } else {
    if (r->state == 1) {
      r->cur_x = cur_x_of(r->kind, (float)fabs((double)(float)(r->value / r->delta)));
      r->state = r->kind = 0, r->t0 = now, r->v0 = r->value;
    }
    if (fabsf(r->value) > 0.f && now >= r->t0 && fabsf(r->cur_x) > 1e-5f && r->down)
      r->value = (float)(cur_val(r->kind, (float)((double)(now - r->t0) / ((double)r->down * r->cur_x))) * r->v0);
  }
  return r->value;
}

// UpdateUsingFullTime: no remaining fraction, the whole time
static float ramp_update_full(ramp *r, bool on, uint32_t now) {
  if (on) {
    if (r->state == 0) r->state = r->kind = 1, r->t0 = now, r->v0 = r->value;
    if (fabsf(r->delta) > fabsf(r->value) && now >= r->t0 && r->cur_x != 0.f && r->up) {
      const float f = cur_val(r->kind, (float)((double)(now - r->t0) / (double)r->up));
      r->value = (float)(f * (r->delta - r->v0) + r->v0);
    }
  } else {
    if (r->state == 1) r->state = r->kind = 0, r->t0 = now, r->v0 = r->value;
    if (fabsf(r->value) > 0.f && now >= r->t0 && r->cur_x != 0.f && r->down)
      r->value = (float)(cur_val(r->kind, (float)((double)(now - r->t0) / (double)r->down)) * r->v0);
  }
  return r->value;
}

typedef struct curve { // CFuncKeysReal, linear
  int n;
  float xs[9], ys[9];
} curve;

static float curve_value(const curve *c, float x) {
  if (!c->n) return 0.f;
  if (x <= c->xs[0] - 1e-5f) return c->ys[0];
  if (x >= c->xs[c->n - 1] + 1e-5f) return c->ys[c->n - 1];
  for (int i = 0; i + 1 < c->n; i++)
    if (x >= c->xs[i] - 1e-5f && x <= c->xs[i + 1] + 1e-5f) {
      const float dx = c->xs[i + 1] - c->xs[i];
      const float t = fabsf(dx) <= 1e-5f ? 0.f : (x - c->xs[i]) / dx;
      return (float)(t * c->ys[i + 1] + (1.f - t) * c->ys[i]);
    }
  return c->ys[c->n - 1];
}

static const curve k_slerp_mod = {6, {-100, -50, 0, 250, 500, 700}, {1.5f, 1.2f, 1.f, 1.5f, 2.f, 3.f}};
static const curve k_look_up = {7, {0, 1, 5, 10, 20, 40, 60}, {0, 0, -0.3f, -0.35f, -0.4f, -0.45f, -0.5f}};
static const curve k_look_down = {9, {0, 1, 1.1f, 1.3f, 2, 10, 20, 40, 60},
                                  {0, 0, 0.03f, 0.1f, 0.3f, 0.35f, 0.4f, 0.5f, 0.6f}};

typedef struct cam_params {
  bool race3; // else the old CtrlCamTmRace (CameraRally)
  float up, far, look; // Race3: Up, Far, the target's look height factor (0x120); old: H, min, max, look
  float min, max;
  float probe; // Race3: the ground probe's height (0x574; Stadium 2, else none)
  // the old camera's cone (ConeAperture, ConeMinSpeed, ConeMaxSpeed: the
  // angle limit by speed) and its fov (degrees); 0 aperture 1: no limit
  float aperture, cone_min, cone_max, fov;
} cam_params;

// the vehicles' default cameras (index 1 of their collectors' lists)
static cam_params params_of(const char *environment) {
  if (!environment) environment = "";
  if (!strcmp(environment, "Stadium")) return (cam_params){true, 2.2f, 4.5f, 0.88f, 0, 0, 2.f, 1.f, 0.f, 60.f, 75.f};
  if (!strcmp(environment, "Island")) return (cam_params){true, 1.5f, 4.0f, 1.0f, 0, 0, 0, 1.f, 0.f, 60.f, 75.f};
  if (!strcmp(environment, "Bay")) return (cam_params){true, 2.2f, 4.5f, 0.88f, 0, 0, 0, 1.f, 0.f, 60.f, 75.f};
  if (!strcmp(environment, "Coast")) return (cam_params){true, 1.75f, 3.9f, 0.9f, 0, 0, 0, 1.f, 0.f, 60.f, 75.f};
  return (cam_params){false, 3.0f, 0.f, 0.8f, 1.0f, 6.0f, 0, 1.f, 0.f, 60.f, 75.f}; // Alpine, Speed, Rally: CameraRally
}

// the old environments' other cameras a CameraGame clip may name (camera_spec
// §8: CameraNormale2 "Behind", CameraNormale "Behind 2", CameraRaprochee
// "Near"); false for the others (and in Race3's environments, whose lists
// lack them: the race camera stays)
static bool old_camera(const char *id, cam_params *out) {
  if (!strcmp(id, "Behind")) *out = (cam_params){false, 5.9f, 0.f, 0.7f, 1.0f, 7.5f, 0, 1.f, 0.f, 60.f, 75.f};
  else if (!strcmp(id, "Behind 2")) *out = (cam_params){false, 5.5f, 0.f, 0.8f, 2.5f, 9.0f, 0, 1.f, 0.f, 60.f, 70.f};
  else if (!strcmp(id, "Near")) *out = (cam_params){false, 2.5f, 0.f, 0.8f, 1.0f, 3.0f, 0, 0.f, 0.f, 60.f, 80.f};
  else return false;
  return true;
}

typedef struct cam_in {
  v3 pos;          // interpolated
  float rot[9];    // interpolated, rows (world = rot * local)
  v3 vel;          // m/s, blended
  float speed;     // km/h along the car, the current tick's
  float steer, gas, brake;
  bool turbo, burnout, any_contact, gear_up;
  bool all_off;    // no wheel on the ground at both ticks
  uint32_t wheels, contacts; // the wheels, those on the ground at both ticks
  const tmuf_track *track;   // the ground probe's collision
} cam_in;

// CGameControlCameraFollowAboveWater: shown instead of the race camera
// while that one's eye is in the water (loop_camera_spec.md §6)
typedef struct faw {
  bool active;
  v3 eye;
  float rot[9]; // rows, as race3's
  float level;  // the water level its last test found
} faw;

typedef struct race3 {
  bool valid;
  faw water;       // kept across the race camera's resets
  bool inside;     // a clip's Internal camera shown instead (at the last step)
  cam_params p;
  uint32_t last;   // now of the last update
  float rot[9];    // output, rows: columns left, up, forward
  v3 eye;
  v3 prev_up, prev_car;
  uint32_t probe_contacts; // 0x578: the wheels on the ground at the last probe
  bool gas_on, brake_on, steering, flying;
  uint32_t fly_start, t1, steer_start, steer_stop, steer_acc;
  int state;       // 0 normal, 1 burnout, 2 flying
  v3 dir, target, normal, flyblend, flydir, stuck, up, target_up, car_up, fly_up;
  float look_factor, look_total, fov_delta, ld_v, ld, radius, radius_v;
  ramp nb, fly_t, pos_t, camup_t, slerp_t, slerp_up_t, fly_look, fly_radius, gas_far, brake_far, steer_far,
      turbo_fov, turbo_far, gear_far, burn_look, burn_radius;
} race3;

static v3 add3(v3 a, v3 b) { return (v3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static v3 scale3(v3 a, float k) { return (v3){a.x * k, a.y * k, a.z * k}; }
static v3 normalize(v3 v) {
  const float l2 = (float)(v.y * v.y + v.x * v.x + v.z * v.z);
  if (l2 > 1e-10f) {
    const float inv = (float)(1.0 / (float)sqrt(l2));
    v = (v3){v.x * inv, v.y * inv, v.z * inv};
  }
  return v;
}
static bool colinear(v3 a, v3 b) {
  const v3 c = cross(a, b);
  return dot(c, c) < 1e-5f;
}
static v3 mat_mul(const float m[9], v3 v) {
  return (v3){(float)((m[1] * v.y + v.x * m[0]) + m[2] * v.z), (float)(m[3] * v.x + m[4] * v.y + m[5] * v.z),
              (float)(m[6] * v.x + m[7] * v.y + m[8] * v.z)};
}
// ComputeSlerpVector
static v3 slerp_vec(v3 a, v3 b, float t) {
  if (t == 0.f) return a;
  if (t == 1.f) return b;
  const v3 c = cross(a, b);
  const float s = (float)sqrt(dot(c, c)), co = dot(a, b);
  float wa, wb;
  if (1.f - co > 1e-5f) {
    const float ang = (float)asin(s);
    wa = (float)((float)sin((float)((1.f - t) * ang)) / s);
    wb = (float)((float)sin((float)(ang * t)) / s);
  } else {
    wa = 1.f - t, wb = t;
  }
  return add3(scale3(a, wa), scale3(b, wb));
}
static void ramp_init(ramp *r, float delta, uint32_t up, uint32_t down) {
  *r = (ramp){.delta = delta, .up = up, .down = down, .cur_x = 1.f};
}

static void faw_init(faw *f) {
  memset(f, 0, sizeof *f);
  f->rot[0] = f->rot[4] = f->rot[8] = 1.f; // the game's: at the origin until first used
}

static void race3_reset(race3 *m, const cam_params *p, const cam_in *in, uint32_t now) {
  faw water = m->water;
  if (!m->valid) faw_init(&water);
  const bool inside = m->valid && m->inside;
  memset(m, 0, sizeof *m);
  m->water = water;
  m->inside = inside;
  m->valid = true;
  m->p = *p;
  float rot[9];
  memcpy(rot, in->rot, sizeof rot);
  if (in->all_off && rot[4] < -0.5f) // upside down: turned about Z by pi
    for (int c = 0; c < 3; c++) rot[c] = -rot[c], rot[3 + c] = -rot[3 + c];
  memcpy(m->rot, rot, sizeof rot);
  m->prev_car = in->pos;
  const v3 off = mat_mul(rot, (v3){0.f, p->up, -p->far});
  m->eye = add3(in->pos, off);
  const v3 d = normalize(off);
  m->dir = m->target = m->normal = m->flyblend = m->flydir = d;
  v3 st = mat_mul(rot, (v3){0.f, 2.f * p->up, -p->far});
  st.y = fabsf(st.y);
  m->stuck = normalize(st);
  m->up = m->target_up = m->car_up = (v3){rot[1], rot[4], rot[7]};
  m->fly_up = (v3){0.f, 1.f, 0.f};
  m->prev_up = (v3){0.f, 1.f, 0.f};
  m->last = now;
  m->radius = (float)sqrt((double)(p->up * p->up + p->far * p->far));
  ramp_init(&m->nb, 1.f, 2000, 2000);
  ramp_init(&m->fly_t, 1.f, 2000, 2000);
  ramp_init(&m->pos_t, 1.f, 2000, 1000);
  ramp_init(&m->camup_t, 1.f, 2000, 2000);
  ramp_init(&m->slerp_t, 1.f, 1000, 1000);
  ramp_init(&m->slerp_up_t, 1.f, 1000, 1000);
  ramp_init(&m->fly_look, -0.2f, 3000, 2500);
  ramp_init(&m->fly_radius, 3.f, 3000, 2500);
  ramp_init(&m->gas_far, 1.f, 2000, 1000);
  ramp_init(&m->brake_far, -0.7f, 750, 1000);
  ramp_init(&m->steer_far, -0.8f, 2000, 1500);
  ramp_init(&m->turbo_fov, 10.5f, 1000, 1400);
  ramp_init(&m->turbo_far, -0.6f, 1000, 1400);
  ramp_init(&m->gear_far, -0.35f, 500, 2000);
  ramp_init(&m->burn_look, -0.4f, 2000, 1500);
  ramp_init(&m->burn_radius, 1.f, 2000, 1500);
}

// the slerp of the direction (or the up) towards its target, §5 L
static v3 turn_towards(v3 d, v3 t, float w_slow, float w_fast, float mod, float blend, float dt) {
  float c = (float)((d.y * t.y + d.x * t.x) + d.z * t.z);
  const float cc = c < -1.f ? -1.f : c > 1.f ? 1.f : c;
  const float ang = (float)acos(cc);
  const double D = (double)ang + ang + 1.0;
  float wF = (float)(w_slow * dt / D), wN = (float)(w_fast * dt / D);
  wF = wF < 0.f ? 0.f : wF > 1.f ? 1.f : wF;
  wN = wN < 0.f ? 0.f : wN > 1.f ? 1.f : wN;
  const float w = (float)(mod * wN * (1.f - blend) + wF * blend);
  float wa, wb;
  if (1.f - c > 1e-5f) {
    const float sn = (float)sin(ang);
    wa = (float)((float)sin((float)((1.f - w) * ang)) / sn);
    wb = (float)((float)sin((float)(w * ang)) / sn);
  } else {
    wa = 1.f - w, wb = w;
  }
  return add3(scale3(d, wa), scale3(t, wb));
}

static void set_dov_and_up(float out[9], v3 dir, v3 up) {
  const v3 l = normalize(cross(up, dir)), f = normalize(dir), u = cross(f, l);
  const float m[9] = {l.x, u.x, f.x, l.y, u.y, f.y, l.z, u.z, f.z};
  memcpy(out, m, sizeof m);
}

static void race3_update(race3 *m, const cam_in *in, uint32_t now) {
  const cam_params *p = &m->p;
  m->prev_up = (v3){m->rot[1], m->rot[4], m->rot[7]};
  const uint32_t dt_ms = m->last > now ? 0u : (now - m->last > 200u ? 200u : now - m->last);
  m->last = now;
  const float dt = (float)(dt_ms * 0.0010000000474974513);
  const v3 pos = in->pos, vel = in->vel;
  const float speed = in->speed;
  const bool below_min = fabsf(speed) < 30.f, below_min2 = fabsf(speed) < 10.f;
  const float hs = (float)sqrt((double)(float)(vel.x * vel.x + vel.z * vel.z));
  // B. state and inputs
  if (in->all_off) {
    if (!m->flying) m->fly_start = now;
    m->flying = true;
  } else {
    m->flying = false;
  }
  if (m->flying && now >= m->fly_start && now - m->fly_start > 1u) m->state = 2;
  else if (in->burnout) {
    if (m->state != 1) m->t1 = now;
    m->state = 1;
  } else {
    m->state = 0;
  }
  m->gas_on = in->gas > 0.1f;
  m->brake_on = in->brake > 0.1f;
  if (in->steer > 0.1f || in->steer < -0.1f) {
    if (!m->steering) m->steer_start = now;
    m->steering = true;
    m->steer_acc += dt_ms;
  } else {
    if (m->steering) m->steer_stop = now;
    m->steering = false;
    if (now >= m->steer_stop && now - m->steer_stop > 150u) m->steer_acc = 0;
  }
  const bool s0 = m->state == 0, s1 = m->state == 1, s2 = m->state == 2;
  // C.
  float look_up = 0.f;
  bool burn_show = false;
  if (s1) {
    const uint32_t t = m->steer_start > m->t1 ? m->steer_start : m->t1;
    burn_show = now >= t && now - t > 2000u && m->steering;
  }
  if (s2) look_up = curve_value(&k_look_up, (float)(-vel.y / (hs + 1.0)));
  // D. far and fov
  float far = ramp_update(&m->gas_far, (((m->gas_on && speed > 2.f) || (m->brake_on && speed < -2.f)) && s0) || s1 || s2,
                          now) + 0.f;
  const float fov_d = ramp_update(&m->turbo_fov, in->turbo && (s0 || s2), now) + 0.f;
  far = (float)(ramp_update(&m->turbo_far, in->turbo && (s0 || s2), now) + far);
  const float g = ramp_update(&m->gear_far, !in->turbo && in->gear_up && s0, now);
  const float st = ramp_update(&m->steer_far, m->steer_acc > 0 && s0 && !below_min, now);
  float mn = st < g ? st : g;
  const float b = ramp_update(&m->brake_far, m->brake_on && s0, now);
  mn = (float)(mn < b ? mn : b);
  far = (float)(mn + far);
  const float burn_r = ramp_update(&m->burn_radius, burn_show, now) + 0.f;
  const float burn_l = ramp_update(&m->burn_look, burn_show, now) + 0.f;
  const float rad_x = (float)(ramp_update(&m->fly_radius, s2, now) + burn_r);
  const float look_x = (float)(ramp_update(&m->fly_look, s2, now) + burn_l);
  // E. look-up, rate limited
  const float dl = (float)(look_up - m->look_factor);
  if ((float)fabs(dl) > 0.2 * dt) look_up = (float)(0.2 * (dl < 0.f || (dl == 0.f && signbit(dl)) ? -1.0 : 1.0) * dt + m->look_factor);
  m->look_factor = look_up;
  m->look_total = (float)(look_up + look_x);
  m->fov_delta = fov_d;
  // F. radius spring
  far = (float)(p->far + far);
  if (far < 0.1f) far = 0.1f;
  const float target_r = (float)((float)sqrt((double)(float)(far * far + p->up * p->up)) + rad_x);
  const float a = (float)((target_r - m->radius) * 10.0 - 5.0 * m->radius_v);
  m->radius_v = (float)(a * dt + m->radius_v);
  m->radius = (float)(m->radius_v * dt + m->radius);
  // G. flying look-down damper
  float ldt = curve_value(&k_look_down, (float)(-vel.y / (hs + 1.0)));
  ldt = ldt <= 0.f ? 0.f : ldt >= 0.95f ? 0.95f : ldt;
  const float la = (float)((ldt - m->ld) * 1.5 - 0.7 * m->ld_v);
  m->ld_v = (float)(la * dt + m->ld_v);
  m->ld = (float)(m->ld_v * dt + m->ld);
  // H. reverse flip
  const float nb = ramp_update(&m->nb, speed < -4.f && s0, now);
  // I. targets
  if (s2) {
    const v3 cur = m->dir;
    if (!colinear(cur, (v3){0.f, 1.f, 0.f})) {
      const v3 h = normalize((v3){cur.x, 0.f, cur.z});
      m->stuck = normalize((v3){h.x * far, p->up + p->up, h.z * far});
    }
    float hx = (float)(m->prev_car.x + cur.x - pos.x), hz = (float)(cur.z + m->prev_car.z - pos.z);
    const float hl2 = (float)(hx * hx + hz * hz);
    if (hl2 > 1e-10f) {
      const float inv = (float)(1.0 / (float)sqrt(hl2));
      hx *= inv, hz *= inv;
    }
    float k = (float)(0.3 + m->ld);
    k = k < 0.f ? 0.f : k > 0.95f ? 0.95f : k;
    const float c = (float)sin((float)((1.f - k) * PI_F * 0.5));
    m->flydir = (v3){(float)(hx * c), (float)sin((float)(k * PI_F * 0.5)), (float)(hz * c)};
    m->fly_up = (v3){0.f, 1.f, 0.f};
  } else {
    const float an = (float)(nb * PI_F);
    m->normal = normalize(mat_mul(in->rot, (v3){(float)((float)sin(an) * far), p->up, (float)((float)cos(an) * -far)}));
    m->car_up = (v3){in->rot[1], in->rot[4], in->rot[7]};
  }
  float t = ramp_update(&m->fly_t, below_min2 && in->any_contact && s2, now);
  m->flyblend = slerp_vec(m->flydir, m->stuck, t);
  t = ramp_update_full(&m->pos_t, s2, now);
  m->target = slerp_vec(m->normal, m->flyblend, t);
  t = ramp_update_full(&m->camup_t, s2, now);
  m->target_up = slerp_vec(m->car_up, m->fly_up, t);
  // J. the ground probe (Stadium): a segment 2 h down from the eye's place
  // (this frame's radius along the last frame's direction); a hit raises the
  // target to keep the eye h above it (camera_spec.md 7)
  if (m->p.probe > 1e-4f && in->track) {
    if (2 * in->contacts <= in->wheels && 2 * m->probe_contacts <= in->wheels) {
      const float r = m->radius, h = m->p.probe;
      const float start[3] = {(float)(pos.x + (float)(r * m->dir.x)), (float)(pos.y + (float)(r * m->dir.y)),
                              (float)(pos.z + (float)(r * m->dir.z))};
      const float seg[3] = {-0.f, (float)(-2.f * h), -0.f};
      float t = 1.f;
      if (tmuf_track_segment_cast(in->track, start, seg, &t) && t < 1.f) {
        const float y = (float)(t * seg[1] + start[1]);
        float sy = (float)((y + h - pos.y) / r);
        if (m->target.y < sy) {
          sy = sy < -1.f ? -1.f : sy > 1.f ? 1.f : sy;
          const float tx = m->target.x, tz = m->target.z;
          float k = (float)((1.f - sy * sy) / (tx * tx + tz * tz));
          k = k > 1e-5f ? (float)sqrt((double)k) : 0.f;
          m->target = (v3){(float)(k * tx), sy, (float)(k * tz)};
        }
      }
    }
    m->probe_contacts = in->contacts;
  }
  // K.
  m->prev_car = pos;
  // L. the direction and the up towards their targets
  const float mod = curve_value(&k_slerp_mod, speed);
  const float sl = ramp_update(&m->slerp_t, s2, now);
  m->dir = turn_towards(m->dir, m->target, 3.f, 5.f, mod, sl, dt);
  const float slu = ramp_update(&m->slerp_up_t, s2, now);
  m->up = turn_towards(m->up, m->target_up, 1.f, 4.f, mod, slu, dt);
  // M. output
  const float r = m->radius;
  m->eye = (v3){(float)((float)(r * m->dir.x) + pos.x), (float)(pos.y + (float)(r * m->dir.y)),
                (float)(pos.z + (float)(r * m->dir.z))};
  float lf = (float)(p->look + m->look_total);
  lf = lf < 0.f ? 0.f : lf > 1.f ? 1.f : lf;
  const float h = (float)(lf * p->up);
  const v3 look = {(float)((float)(h * m->up.x) + m->prev_car.x), (float)(m->prev_car.y + (float)(h * m->up.y)),
                   (float)(m->prev_car.z + (float)(h * m->up.z))};
  const v3 view = sub(look, m->eye);
  set_dov_and_up(m->rot, view, colinear(m->up, view) ? m->prev_up : m->up);
}

// The old race camera (CameraRally): a leash from 3 m above the car
static void race1_reset(race3 *m, const cam_params *p, const cam_in *in, uint32_t now) {
  faw water = m->water;
  if (!m->valid) faw_init(&water);
  const bool inside = m->valid && m->inside;
  memset(m, 0, sizeof *m);
  m->water = water;
  m->inside = inside;
  m->valid = true;
  m->p = *p;
  m->last = now;
  memcpy(m->rot, in->rot, sizeof m->rot);
  m->eye = sub(in->pos, scale3((v3){in->rot[2], in->rot[5], in->rot[8]}, 15.f));
}

static void race1_update(race3 *m, const cam_in *in, uint32_t now) {
  const cam_params *p = &m->p;
  m->last = now;
  // the heading frame: exact world up, the car's forward
  float frame[9];
  const v3 fwd = {in->rot[2], in->rot[5], in->rot[8]}, up = {0.f, 1.f, 0.f};
  {
    const v3 f = normalize(fwd), l = normalize(cross(up, f)), u = cross(f, l);
    const float fr[9] = {l.x, u.x, f.x, l.y, u.y, f.y, l.z, u.z, f.z};
    memcpy(frame, fr, sizeof fr);
  }
  const v3 pivot = add3(in->pos, (v3){0.f, p->up, 0.f});
  const v3 e = sub(m->eye, pivot);
  // Fᵀ·e
  v3 d = {frame[0] * e.x + frame[3] * e.y + frame[6] * e.z, frame[1] * e.x + frame[4] * e.y + frame[7] * e.z,
          frame[2] * e.x + frame[5] * e.y + frame[8] * e.z};
  d.y = 0.f;
  float l2 = d.x * d.x + d.z * d.z;
  if (l2 <= 1e-10f) d = (v3){0.f, 0.f, -p->min}, l2 = p->min * p->min;
  if (p->min * p->min > l2) d = scale3(d, p->min / sqrtf(l2));
  else if (p->max * p->max < l2) d = scale3(d, p->max / sqrtf(l2));
  // the cone (LimitAngle 0x45ffa0): behind the car within k pi by speed
  // (CameraRally's: aperture 1, no limit)
  const float sp = in->speed;
  const float k = sp <= p->cone_min ? 1.f
                  : sp > p->cone_max ? p->aperture
                                     : (p->cone_max - sp) * (1.f - p->aperture) / (p->cone_max - p->cone_min) + p->aperture;
  if (k < 0.9999f) {
    const float r = sqrtf(d.x * d.x + d.z * d.z), lim = k * 3.14159265f;
    float a = atan2f(-d.x, -d.z);
    a = a > lim ? lim : a < -lim ? -lim : a;
    d.x = -r * sinf(a), d.z = -r * cosf(a);
  }
  v3 o = scale3(d, -1.f);
  o.y -= (1.f - p->look) * p->up;
  m->eye = add3(mat_mul(frame, d), pivot);
  const v3 ow = mat_mul(frame, o);
  set_dov_and_up(m->rot, ow, up);
}

// --- the water camera (loop_camera_spec.md §6) -----------------------------------

// CamIsInWater: the eye in the water column of its cell (within the margin),
// or below the world's cull height; *level the water's there
static bool cam_in_water(const tmuf_camera_water *w, v3 eye, float margin, float *level) {
  const float y = eye.y;
  *level = w->top;
  const float fx = (eye.x - w->origin[0]) / w->cell_size[0], fz = (eye.z - w->origin[1]) / w->cell_size[1];
  // (u32)(i64)trunc: below 0 is outside
  const bool inside = fx > -1.f && fz > -1.f && fx < (float)w->dims[0] && fz < (float)w->dims[1] &&
                      (uint32_t)(int64_t)fx < w->dims[0] && (uint32_t)(int64_t)fz < w->dims[1];
  if (!inside) {
    const uint8_t v = w->outside;
    if (!v) return false;
    if (w->plane_mode) {
      if (v - 1u >= w->plane_count) return false;
      *level = w->plane_levels[v - 1u];
      return (float)(y - margin) < *level;
    }
    return (float)(y - margin) < w->top;
  }
  const uint8_t v = w->cells ? w->cells[(uint32_t)(int64_t)fx + w->dims[0] * (uint32_t)(int64_t)fz] : 0;
  if (v) {
    if (w->plane_mode) {
      if (v - 1u < w->plane_count) {
        const float l = w->plane_levels[v - 1u];
        *level = l;
        if ((float)(l - (float)(w->top - w->bottom)) < (float)(y + margin) && (float)(y - margin) < l) return true;
      }
    } else if (w->bottom < (float)(y + margin) && (float)(y - margin) < w->top) {
      return true;
    }
  }
  return w->cull > y;
}

// FollowAboveWater::Update: 4 m over the water, 4.5 to 6.5 m from the car
// across its heading, looking at it with half the height between them
static void faw_update(faw *f, const cam_in *in, float cull) {
  const v3 fwd = {in->rot[2], in->rot[5], in->rot[8]}, up = {0.f, 1.f, 0.f};
  const v3 fz = normalize(fwd), fl = normalize(cross(up, fz)), fu = cross(fz, fl);
  const float frame[9] = {fl.x, fu.x, fz.x, fl.y, fu.y, fz.y, fl.z, fu.z, fz.z};
  const float h = f->level > cull ? f->level : cull;
  const v3 pivot = {in->pos.x, (float)((double)h + 4.0), in->pos.z};
  const v3 e = sub(f->eye, pivot);
  v3 d = {frame[0] * e.x + frame[3] * e.y + frame[6] * e.z, 0.f, frame[2] * e.x + frame[5] * e.y + frame[8] * e.z};
  float l2 = (float)(d.x * d.x + 0.f * 0.f + d.z * d.z);
  if (l2 <= 1e-10f) d = (v3){0.f, 0.f, -4.5f}, l2 = 4.5f * 4.5f;
  if (4.5f * 4.5f > l2) d = scale3(d, (float)(4.5f / (float)sqrt(l2)));
  else if (6.5f * 6.5f < l2) d = scale3(d, (float)(6.5f / (float)sqrt(l2)));
  f->eye = add3(mat_mul(frame, d), pivot);
  const v3 o = {in->pos.x - f->eye.x, (float)((in->pos.y - f->eye.y) * 0.5f), in->pos.z - f->eye.z};
  set_dov_and_up(f->rot, o, up);
}

// OverrideCamVal on the race camera's output: in below the water + 0.5 m,
// out from 2 m over it. (The game shows the water camera's previous output in
// the frame it comes on, from the origin the first time; updated here.)
static void water_step(race3 *m, const cam_in *in) {
  tmuf_camera_water w;
  if (!in->track || !tmuf_track_camera_water(in->track, &w)) {
    m->water.active = false;
    return;
  }
  faw *f = &m->water;
  const bool was = f->active;
  if (was) faw_update(f, in, w.cull); // with the level of the last test
  f->active = cam_in_water(&w, m->eye, was ? 2.f : 0.5f, &f->level);
  if (f->active && !was) faw_update(f, in, w.cull);
}

static cam_in inputs_of(const ft_world *previous, const ft_world *world, float alpha) {
  cam_in in;
  memset(&in, 0, sizeof in);
  const tm_pose pose = tm_pose_at(previous, world, alpha);
  in.pos = (v3){pose.position.x, pose.position.y, pose.position.z};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) in.rot[r * 3 + c] = pose.rotation[r][c];
  const tmuf_world *w = &world->w, *wp = previous ? &previous->w : w;
  const tmuf_dyna_state *b = &w->sim.body.state, *a = &wp->sim.body.state;
  in.vel = (v3){(float)(a->lin.x + (float)((float)(b->lin.x - a->lin.x) * alpha)),
                (float)(a->lin.y + (float)((float)(b->lin.y - a->lin.y) * alpha)),
                (float)(a->lin.z + (float)((float)(b->lin.z - a->lin.z) * alpha))};
  // front speed: the current tick's velocity along the car
  const float(*R)[3] = b->rot.m;
  in.speed = (float)((R[0][2] * b->lin.x + R[1][2] * b->lin.y + R[2][2] * b->lin.z) * 3.5999999046325684);
  const tmuf_car *cc = &w->sim.car, *cp = &wp->sim.car;
  in.steer = (float)(cp->controls.steering * (float)(1.f - alpha) + cc->controls.steering * alpha);
  in.gas = (float)(cp->controls.gate_a * (float)(1.f - alpha) + cc->controls.gate_a * alpha);
  in.brake = (float)(cp->controls.gate_b * (float)(1.f - alpha) + cc->controls.gate_b * alpha);
  in.turbo = cc->turbo.type != 0;
  in.burnout = cc->geared.wheel_speed_override != 0;
  in.any_contact = cc->air.refresh_memory != 0;
  in.gear_up = cc->geared.engine_state == 1 && cc->geared.shift_down == 0;
  bool any = false;
  for (uint32_t i = 0; i < cc->wheel_count; i++) {
    const bool both = cc->wheels[i].contact && (i < cp->wheel_count ? cp->wheels[i].contact : 1);
    any = any || both;
    in.contacts += both ? 1u : 0u;
  }
  in.wheels = cc->wheel_count;
  in.all_off = !any;
  return in;
}

static race3 g_race;
static const void *g_race_track;

// the camera as it was at the frames shown last (the newest on top): a step
// back on the timeline takes it from there instead of running the three
// seconds before again
#define HISTORY 4096u
static race3 g_history[HISTORY];
static uint32_t g_history_top, g_history_count; // top: the next slot

static void history_clear(void) { g_history_top = g_history_count = 0; }

static const race3 *history_newest(void) {
  return g_history_count ? &g_history[(g_history_top + HISTORY - 1u) % HISTORY] : NULL;
}

// the frames after `now` dropped
static void history_back_to(uint32_t now) {
  while (g_history_count && history_newest()->last > now)
    g_history_top = (g_history_top + HISTORY - 1u) % HISTORY, g_history_count--;
}

static void history_push(const race3 *m) {
  history_back_to(m->last > 0u ? m->last - 1u : 0u);
  g_history[g_history_top] = *m;
  g_history_top = (g_history_top + 1u) % HISTORY;
  if (g_history_count < HISTORY) g_history_count++;
}

// one frame of the race camera; `inside` while a clip shows the car's
// Internal camera instead: the race camera runs on, is reset when the clip's
// camera goes (UpdateCams), and the water camera is off meanwhile (the
// Internal camera's camval may not be overridden)
static void camera_step(race3 *m, const cam_params *p, const cam_in *in, uint32_t now, bool reset, bool inside) {
  if (m->valid && m->inside && !inside) reset = true;
  if (reset) {
    if (p->race3) race3_reset(m, p, in, now);
    else race1_reset(m, p, in, now);
  }
  m->inside = inside;
  if (p->race3) race3_update(m, in, now);
  else race1_update(m, in, now);
  if (inside) m->water.active = false;
  else water_step(m, in);
}

// --- the MediaTracker in-game clips (loop_camera_spec.md §2-§4) -------------------
//
// The map's clips (Nadeo's loopings: one with the car's "Internal" camera at a
// loop's entry, an empty one at its exit) start when the car is in one of
// their trigger cells, once per physics tick from the race start; a clip
// keeps playing until another starts or the car respawns (keep-playing), else
// until its end. Run through the timeline's ticks, checkpointed.

typedef struct clip_state {
  int32_t tick;      // the last tick checked, -1 none
  int32_t last_clip; // the clip triggered last, -1 none (cleared by a respawn)
  int32_t playing;   // the clip playing, -1 none
  int32_t start_tick;
  uint32_t respawns;
  uint64_t triggered[4]; // the clips triggered so far (ClipTriggerConditionOk)
} clip_state;

#define CLIP_CHECKPOINT_TICKS 100
static struct {
  const void *track;
  uint32_t group;
  clip_state cur;
  clip_state *checkpoints; // the state after tick k * CLIP_CHECKPOINT_TICKS - 1
  uint32_t checkpoint_count, checkpoint_cap;
} g_clips;

static void clip_reset(clip_state *s) {
  memset(s, 0, sizeof *s);
  s->tick = -1;
  s->last_clip = s->playing = -1;
}

static bool clip_triggered(const clip_state *s, int64_t i) {
  return i >= 0 && i < 256 && ((s->triggered[i >> 6] >> (i & 63)) & 1u);
}

static bool clip_condition(const tmuf_ingame_clip *c, const clip_state *s, int32_t tick, const tmuf_world *w) {
  const float t = (float)(tick * (int32_t)TMUF_TICK_MS - (int32_t)TMUF_RACE_START_MS) * 0.001f;
  const float v = c->condition_value;
  switch (c->condition) {
  case 0: return true;
  case 1: return t < v;
  case 2: return t > v;
  case 3: return clip_triggered(s, lrintf(v));
  case 4:
  case 5: {
    const tmuf_dyna_state *b = &w->sim.body.state;
    const float(*R)[3] = b->rot.m;
    const float kmh = (float)((R[0][2] * b->lin.x + R[1][2] * b->lin.y + R[2][2] * b->lin.z) * 3.5999999046325684);
    return c->condition == 4 ? kmh < v : kmh > v;
  }
  case 6: return !clip_triggered(s, lrintf(v));
  default: return false; // MaxPlayCount, Random...: never in TMUF
  }
}

// MediaClipCheckInGameTriggers after tick `tick` (b; a the world before it,
// the car where it was drawn)
static void clip_step(clip_state *s, const tmuf_ingame_clip *clips, uint32_t n, float cell_xz, float cell_y,
                      int32_t tick, const ft_world *a, const ft_world *b) {
  s->tick = tick;
  if (b->w.sim.race.respawns != s->respawns) {
    s->respawns = b->w.sim.race.respawns;
    s->last_clip = s->playing = -1;
  }
  if (s->playing >= 0 && !clips[s->playing].keep_playing &&
      (float)(tick - s->start_tick) * (float)TMUF_TICK_MS * 0.001f >= clips[s->playing].end)
    s->playing = -1;
  if (tick * (int32_t)TMUF_TICK_MS < (int32_t)TMUF_RACE_START_MS || !(cell_xz > 0.f) || !(cell_y > 0.f)) return;
  const tmuf_vec3 p = (a ? a : b)->w.sim.body.state.pos;
  const float f[3] = {p.x / cell_xz, p.y / cell_y, p.z / cell_xz};
  uint32_t c[3];
  for (int k = 0; k < 3; k++) {
    if (!(f[k] > -1.f && f[k] < 4294967296.f)) return; // (u32)(i64)trunc: no cell below 0
    c[k] = (uint32_t)(int64_t)f[k];
  }
  int32_t found = -1;
  for (uint32_t i = 0; i < n && found < 0; i++)
    for (uint32_t k = 0; k < clips[i].cell_count; k++)
      if (clips[i].cells[k][0] == c[0] && clips[i].cells[k][1] == c[1] && clips[i].cells[k][2] == c[2]) {
        found = (int32_t)i;
        break;
      }
  if (found < 0 || found == s->last_clip || !clip_condition(&clips[found], s, tick, &b->w)) return;
  s->last_clip = s->playing = found;
  s->start_tick = tick;
  if (found < 256) s->triggered[found >> 6] |= (uint64_t)1 << (found & 63);
}

// the clips' state after `tick` of the group's run
static const clip_state *clips_at(ft_game *game, const tmuf_track *track, uint32_t group, int32_t tick) {
  const tmuf_ingame_clip *clips = NULL;
  const uint32_t n = tmuf_track_ingame_clips(track, &clips);
  if (g_clips.track != (const void *)track || g_clips.group != group) {
    g_clips.track = track, g_clips.group = group;
    g_clips.checkpoint_count = 0;
    clip_reset(&g_clips.cur);
  }
  if (!n) return NULL;
  clip_state *s = &g_clips.cur;
  if (tick < s->tick) {
    // back: from the checkpoint before it
    uint32_t k = (uint32_t)((tick + 1) / CLIP_CHECKPOINT_TICKS);
    if (k > g_clips.checkpoint_count) k = g_clips.checkpoint_count;
    if (k) *s = g_clips.checkpoints[k - 1];
    else clip_reset(s);
    g_clips.checkpoint_count = k;
  }
  float xz = 0.f, y = 0.f;
  tmuf_track_trigger_cell_size(track, &xz, &y);
  const ft_engine_api *api = game->engine;
  for (int32_t t = s->tick + 1; t <= tick; t++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair && api->timeline_world_pair(group, t, &a, &b) && b)
      clip_step(s, clips, n, xz, y, t, a, b);
    else
      s->tick = t;
    // the state after tick k * CLIP_CHECKPOINT_TICKS - 1
    if ((t + 1) % CLIP_CHECKPOINT_TICKS == 0 && (uint32_t)((t + 1) / CLIP_CHECKPOINT_TICKS) == g_clips.checkpoint_count + 1) {
      if (g_clips.checkpoint_count == g_clips.checkpoint_cap) {
        const uint32_t cap = g_clips.checkpoint_cap ? g_clips.checkpoint_cap * 2 : 64;
        clip_state *grown = realloc(g_clips.checkpoints, sizeof *grown * cap);
        if (!grown) continue;
        g_clips.checkpoints = grown, g_clips.checkpoint_cap = cap;
      }
      g_clips.checkpoints[g_clips.checkpoint_count++] = *s;
    }
  }
  return s;
}

// --- the in-game clips' cameras ---------------------------------------------

enum { CLIP_NONE, CLIP_INTERNAL, CLIP_CUSTOM, CLIP_OLD };

// a CameraCustom camera, as its clip shows it (camera_custom_spec.md §6):
// eye, rotation rows (columns left, up, forward), vertical fov (degrees)
typedef struct clip_view {
  v3 eye;
  float rot[9];
  float fov;
} clip_view;

static bool key_interp(const tmuf_clip_custom_key *k) { return k->interp >= 1 && k->interp <= 3; }
static bool key_same(const tmuf_clip_custom_key *a, const tmuf_clip_custom_key *b) {
  return a->target == b->target && a->anchor == b->anchor;
}

// the keys at clip time x (§3-§5): the pair around it, held (a step) unless
// both interpolate with the same anchor and target; the position by Hermite
// with Catmull-Rom tangents (uniform), the angles, fov and target offset
// linearly
static tmuf_clip_custom_key eval_keys(const tmuf_clip_custom_key *K, uint32_t n, float x) {
  int i0 = 0, i1 = 0;
  if (n == 1 || x < K[0].time - 1e-5f) i0 = i1 = 0;
  else if (x > K[n - 1].time + 1e-5f) i0 = i1 = (int)n - 1;
  else {
    for (i0 = 0; i0 < (int)n - 1; i0++)
      if (K[i0].time - 1e-5f <= x && x <= K[i0 + 1].time + 1e-5f) break;
    if (i0 >= (int)n - 1) i0 = (int)n - 2;
    i1 = i0 + 1;
  }
  float t = 0.f;
  if (i0 != i1) {
    const float d = K[i1].time - K[i0].time;
    if (fabsf(d) >= 1e-5f) t = (float)((x - K[i0].time) / d);
  }
  const tmuf_clip_custom_key *A = &K[i0], *B = &K[i1];
  if (t <= 0.f) return *A;
  if (t >= 1.f) return *B;
  if (!key_interp(A) || !key_interp(B) || !key_same(A, B)) return *A;
  float m0[3], m1[3];
  for (int j = 0; j < 3; j++) {
    m0[j] = A->interp == 3 ? A->right_tangent[j] : B->pos[j] - A->pos[j];
    m1[j] = B->interp == 3 ? B->left_tangent[j] : B->pos[j] - A->pos[j];
  }
  if (A->interp == 1 && i0 > 0 && key_interp(&K[i0 - 1]) && key_same(&K[i0 - 1], B))
    for (int j = 0; j < 3; j++)
      m0[j] = (B->pos[j] - K[i0 - 1].pos[j]) * 0.5f;
  if (B->interp == 1 && i1 < (int)n - 1 && key_interp(&K[i1 + 1]) && key_same(&K[i1 + 1], B))
    for (int j = 0; j < 3; j++)
      m1[j] = (K[i1 + 1].pos[j] - A->pos[j]) * 0.5f;
  const float t2 = t * t, t3 = t2 * t;
  const float h00 = 2.f * t3 - 3.f * t2 + 1.f, h01 = 3.f * t2 - 2.f * t3, h10 = t3 - 2.f * t2 + t, h11 = t3 - t2;
  tmuf_clip_custom_key o = *A;
  for (int j = 0; j < 3; j++)
    o.pos[j] = ((A->pos[j] * h00 + B->pos[j] * h01) + m0[j] * h10) + m1[j] * h11;
  const double u = 1.0 - (double)t;
  o.pitch = (float)(B->pitch * (double)t + A->pitch * u);
  o.yaw = (float)(B->yaw * (double)t + A->yaw * u);
  o.roll = (float)(B->roll * (double)t + A->roll * u);
  o.fov = (float)(B->fov * (double)t + A->fov * u);
  const float uf = 1.f - t;
  for (int j = 0; j < 3; j++)
    o.target_pos[j] = A->target_pos[j] * uf + B->target_pos[j] * t;
  return o;
}

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
  float r[9];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
  memcpy(out, r, sizeof r);
}

// the camera of an evaluated key (§6): R = Ry(yaw) Rx(pitch) Rz(roll), or
// looking at the target entity's pose applied to the target offset with
// exact world up; an anchor moves the eye (anchor_rot 0: by its position,
// 1: into its frame). Entity 0 is the player's car (in), others are none
static void key_view(const tmuf_clip_custom_key *k, const cam_in *in, clip_view *out) {
  out->fov = k->fov;
  const bool anchored = k->anchor == 0, targeted = k->target == 0;
  float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  v3 eye = {k->pos[0], k->pos[1], k->pos[2]};
  if (!targeted) {
    const float cp = cosf(k->pitch), sp = sinf(k->pitch), cy = cosf(k->yaw), sy = sinf(k->yaw), cr = cosf(k->roll),
                sr = sinf(k->roll);
    const float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp}, ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy},
                rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
    float yx[9];
    mat3_mul(ry, rx, yx);
    mat3_mul(yx, rz, R);
  }
  if (anchored) {
    if (!k->anchor_rot) {
      eye = add3(in->pos, eye);
    } else {
      float r[9];
      mat3_mul(in->rot, R, r);
      memcpy(R, r, sizeof r);
      eye = add3(mat_mul(in->rot, eye), in->pos);
    }
  }
  if (targeted) {
    const v3 aim = add3(mat_mul(in->rot, (v3){k->target_pos[0], k->target_pos[1], k->target_pos[2]}), in->pos);
    const v3 d = sub(aim, eye);
    if (dot(d, d) > 1e-10f) set_dov_and_up(R, d, (v3){0.f, 1.f, 0.f});
    else {
      const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      memcpy(R, id, sizeof id);
    }
  }
  out->eye = eye;
  memcpy(out->rot, R, sizeof R);
}

// the camera the clip playing at `now` (ms of the run, after tick `tick`)
// shows (InternalUpdateBlocks: each camera block active at the clip's time,
// or from its start on for a keep-playing track's last block; the last one
// wins): the car's Internal camera for a CameraGame block "Internal" (others
// and "<Default>" keep the race camera), a CameraCustom block's own camera
// (view, needs in), one of the old environments' other cameras (CLIP_OLD,
// its parameters in old); CLIP_NONE for the race camera
static int clip_camera(ft_game *game, const tmuf_track *track, uint32_t group, int32_t tick, uint32_t now,
                       const cam_in *in, clip_view *view, cam_params *old) {
  const clip_state *s = track ? clips_at(game, track, group, tick) : NULL;
  if (!s || s->playing < 0) return CLIP_NONE;
  const tmuf_ingame_clip *clips = NULL;
  tmuf_track_ingame_clips(track, &clips);
  const tmuf_ingame_clip *c = &clips[s->playing];
  // (the frame of the trigger shows the clip at its start)
  float t = (float)((int64_t)now - (int64_t)s->start_tick * TMUF_TICK_MS) * 0.001f;
  if (t < 0.f) t = 0.f;
  if (!c->keep_playing && t >= c->end) return CLIP_NONE;
  int kind = CLIP_NONE;
  const tmuf_clip_camera *custom = NULL;
  for (uint32_t i = 0; i < c->camera_count; i++) {
    const tmuf_clip_camera *cam = &c->cameras[i];
    if (!(cam->start <= t && (t <= cam->end || cam->keep))) continue;
    if (cam->kind == TMUF_CLIP_CAMERA_GAME) {
      if (cam->entity != 0) continue;
      cam_params q;
      kind = strcmp(cam->id, "Internal") == 0 ? CLIP_INTERNAL : old_camera(cam->id, &q) ? CLIP_OLD : CLIP_NONE;
      if (kind == CLIP_OLD && old) *old = q;
    } else if (cam->kind == TMUF_CLIP_CAMERA_CUSTOM && cam->key_count) {
      kind = CLIP_CUSTOM;
      custom = cam;
    }
  }
  if (kind == CLIP_CUSTOM) {
    if (!in || !view) return kind;
    const tmuf_clip_custom_key k = eval_keys(custom->keys, custom->key_count, t);
    key_view(&k, in, view);
  }
  return kind;
}

bool tm_camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  game->camera_inside = false;
  if (!frame || !frame->world || frame->mode != CAMERA_CHASE) return false;
  if (test_camera(inout)) return true;
  const ft_level *level = game->level;
  const cam_params p = params_of(level ? tmuf_track_environment(level->track) : NULL);
  // the render time: the frame's place between its two ticks, in whole ms
  const int32_t tick = (int32_t)frame->world->w.tick;
  const float alpha = frame->previous_world ? (frame->alpha < 0.f ? 0.f : frame->alpha > 1.f ? 1.f : frame->alpha) : 1.f;
  const int32_t now_i = (tick - 1) * (int32_t)TMUF_TICK_MS + (int32_t)(alpha * (float)TMUF_TICK_MS + 0.5f);
  const uint32_t now = now_i < 0 ? 0u : (uint32_t)now_i;
  cam_in in = inputs_of(frame->previous_world, frame->world, alpha);
  in.track = level ? level->track : NULL;
  const ft_engine_api *api = game->engine;
  // the timeline's group this world belongs to (the first when none says so)
  uint32_t group = 0;
  if (api->timeline_world_pair && api->timeline_world_count)
    for (uint32_t i = 0; i < api->timeline_world_count(); i++) {
      const ft_world *a = NULL, *b = NULL;
      if (api->timeline_world_pair(i, tick, &a, &b) && b == frame->world) {
        group = i;
        break;
      }
    }
  race3 *m = &g_race;
  const bool track = m->valid && g_race_track == (level ? (const void *)level->track : NULL) && m->p.race3 == p.race3;
  bool same = track && now >= m->last && now - m->last <= 250u;
  if (track && now < m->last) {
    // back: the newest frame shown up to now, when it is close
    history_back_to(now);
    const race3 *h = history_newest();
    if (h && now - h->last <= 250u) *m = *h, same = true;
  }
  if (!same) {
    history_clear();
    // rebuilt from the timeline's last three seconds (or from the start)
    const int32_t first = tick - 300 > 0 ? tick - 300 : 0;
    bool reset = true;
    if (api->timeline_world_pair)
      for (int32_t t = first; t < tick; t++) {
        const ft_world *a = NULL, *b = NULL;
        if (!api->timeline_world_pair(group, t, &a, &b) || !b) continue;
        cam_in step = inputs_of(a, b, 1.f);
        step.track = in.track;
        const uint32_t t_now = (uint32_t)(t > 0 ? t * (int32_t)TMUF_TICK_MS : 0);
        camera_step(m, &p, &step, t_now, reset, clip_camera(game, in.track, group, t, t_now, NULL, NULL, NULL) != CLIP_NONE);
        reset = false;
      }
    if (reset) camera_step(m, &p, &in, now, true, false);
    g_race_track = level ? (const void *)level->track : NULL;
  }
  clip_view cv;
  cam_params oldp;
  int clip = clip_camera(game, in.track, group, tick, now, &in, &cv, &oldp);
  // (the old environments' lists alone have those cameras)
  if (clip == CLIP_OLD && p.race3) clip = CLIP_NONE;
  camera_step(m, &p, &in, now, false, clip != CLIP_NONE);
  history_push(m);
  if (clip == CLIP_OLD) {
    // that camera (its own leash, from where it was; reset when it is first
    // shown or another takes over), its fov
    static race3 other;
    static float other_up = -1.f;
    if (!other.valid || other_up != oldp.up || now < other.last || now - other.last > 250u)
      race1_reset(&other, &oldp, &in, now);
    other_up = oldp.up;
    race1_update(&other, &in, now);
    const v3 f = {other.rot[2], other.rot[5], other.rot[8]}, u = {other.rot[1], other.rot[4], other.rot[7]};
    inout->eye = (ft_vec3){other.eye.x, other.eye.y, other.eye.z};
    inout->target = (ft_vec3){other.eye.x + f.x * 10.f, other.eye.y + f.y * 10.f, other.eye.z + f.z * 10.f};
    inout->up = (ft_vec3){u.x, u.y, u.z};
    set_lens(inout, oldp.fov * 3.14159265f / 180.f);
    return true;
  }
  if (clip == CLIP_CUSTOM) {
    // a TV camera (CameraCustom): the car drawn, no water camera
    const v3 f = {cv.rot[2], cv.rot[5], cv.rot[8]}, u = {cv.rot[1], cv.rot[4], cv.rot[7]};
    inout->eye = (ft_vec3){cv.eye.x, cv.eye.y, cv.eye.z};
    inout->target = (ft_vec3){cv.eye.x + f.x * 10.f, cv.eye.y + f.y * 10.f, cv.eye.z + f.z * 10.f};
    inout->up = (ft_vec3){u.x, u.y, u.z};
    set_lens(inout, cv.fov * 3.14159265f / 180.f);
    return true;
  }
  if (clip == CLIP_INTERNAL) {
    // the car's Internal camera (Vehicles\72F6FB91...): 1.5 m up and 0.7 m
    // forward in the car, turned with it, 80 degrees; the car not drawn
    const v3 off = mat_mul(in.rot, (v3){0.f, 1.5f, 0.7f});
    const v3 e = add3(in.pos, off), f = {in.rot[2], in.rot[5], in.rot[8]}, u = {in.rot[1], in.rot[4], in.rot[7]};
    inout->eye = (ft_vec3){e.x, e.y, e.z};
    inout->target = (ft_vec3){e.x + f.x * 10.f, e.y + f.y * 10.f, e.z + f.z * 10.f};
    inout->up = (ft_vec3){u.x, u.y, u.z};
    set_lens(inout, 80.f * 3.14159265f / 180.f);
    game->camera_inside = true;
    return true;
  }
  // the race camera's view, or the water camera's while it is on
  const v3 eye = m->water.active ? m->water.eye : m->eye;
  const float *rot = m->water.active ? m->water.rot : m->rot;
  inout->eye = (ft_vec3){eye.x, eye.y, eye.z};
  const v3 f = {rot[2], rot[5], rot[8]}, u = {rot[1], rot[4], rot[7]};
  // the look-at point at the car's depth along the view: the orbit the
  // user may switch to turns around it at the race camera's distance
  float depth = dot(sub(in.pos, eye), f);
  if (depth < 1.f) depth = 1.f;
  inout->target = (ft_vec3){eye.x + f.x * depth, eye.y + f.y * depth, eye.z + f.z * depth};
  inout->up = (ft_vec3){u.x, u.y, u.z};
  // the camera's 75 degrees, vertical, always (GetCamVal)
  set_lens(inout, LENS_FOV_DEG * 3.14159265f / 180.f);
  return true;
}
