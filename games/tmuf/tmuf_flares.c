// The lights' flares (the game's lens flares, CVisionViewportDx9: after the
// transparent draws, before the post effects): one quad per light, added,
// as bright as the light is visible:
//
//   the sun   centred where the sun is at 0.95 of the far distance, half
//             tan(FlareAngularSizeSun) of that across, in the sun's colour,
//             hidden by the scenery in front and by the clouds (their light
//             occlusion at its middle)
//   lamps     the map's lights with a flare: FlareSize (m) half across, their
//             colour by FlareIntensity; within their flare range, spots
//             only seen from inside AngleFlare; faded over the last fifth of
//             the far distance
//   the car   its tail lights: red, half on at night, full while braking
//
// Visibility is the game's occlusion query: the light's point, moved
// FlareBiasZ toward the camera, against the frame's depth, read back after
// the frame (a one-pixel draw per light into a row, tm_flares_after_frame)
// and so a frame late, as the game's queries are; each flare then eases in
// at 3 T per second and out at 3 per second (LensFlaresFindVisible), so a
// light at the edge of what hides it does not flicker. Pictures of the module
// for comparisons (TM_TEST_CAMERA) take the settled state at once.

#include "tmuf_internal.h"

#include "flare_frag_spv.h"
#include "flare_vert_spv.h"
#include "groundrefl_frag_spv.h"
#include "groundrefl_vert_spv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct flare_vertex {
  float clip[4];
  float uv[2];
  float color[4];
  float test[4];
  float clouds;
} flare_vertex;

typedef struct ground_vertex {
  float clip[4];
  float uv[2];
  float color[4];
} ground_vertex;

typedef struct flare_picture {
  const char *file, *pack_file;
  tg_texture texture;
} flare_picture;

// a light's flare across frames: its eased intensity and last test
typedef struct flare_state {
  uint32_t key;  // which light (flare_key)
  float intensity;
  bool visible;  // the last test read back
  uint32_t frame; // the frame it was last shown in
} flare_state;

#define VIS_WIDTH 1024u // lights tested a frame, at most

typedef struct vis_frame {
  uint32_t serial, count;
  uint32_t keys[VIS_WIDTH];
  uint32_t query[VIS_WIDTH]; // its query, else VIS_SEEN or VIS_HIDDEN (not tested: seen; off screen: hidden)
} vis_frame;
enum { VIS_SEEN = 0xffffffffu, VIS_HIDDEN = 0xfffffffeu };

struct tm_flares {
  tg_program *program;
  tg_program *ground; // the lamps' fake ground reflection (tm_flares_ground_reflection)
  ground_vertex *ground_vertices;
  uint32_t ground_cap; // in lights (12 vertices each)
  flare_picture pictures[16];
  uint32_t picture_count;
  flare_vertex *vertices;
  uint32_t *picture_of; // per quad
  float (*tests)[4];    // per quad: its light's point, clip space
  uint32_t *key_of;     // per quad: its light
  uint32_t quad_count, quad_cap;
  flare_state *states;
  uint32_t state_count, state_cap, frame;
  double last_seconds;
  // the lights each frame tested, by frame (an occlusion query a quad: the
  // counts come back a frame or two later, tg_query_results)
  vis_frame vis_frames[4];
  uint32_t vis_taken; // the frame whose counts were taken last
  bool built;         // this frame's quads made (tm_flares_test)
};

// the renderer's flare picture for lights that name none (LensFlareAdd's
// default; the fake ground reflection's glow too)
static const char *const default_flare = "Media\\Texture\\Image\\WhiteFlare1.tga";

// the lights' keys: the sun, the map's lamps by index, the car's by index
enum { KEY_SUN = 1u, KEY_LAMP = 1u << 20, KEY_CAR = 2u << 20 };

static flare_state *state_of(tm_flares *f, uint32_t key) {
  for (uint32_t i = 0; i < f->state_count; i++)
    if (f->states[i].key == key) return &f->states[i];
  if (f->state_count == f->state_cap) {
    const uint32_t cap = f->state_cap ? f->state_cap * 2 : 64;
    flare_state *grown = realloc(f->states, sizeof *grown * cap);
    if (!grown) return NULL;
    f->states = grown, f->state_cap = cap;
  }
  flare_state *st = &f->states[f->state_count++];
  *st = (flare_state){.key = key};
  return st;
}

static const flare_state *state_of_const(const tm_flares *f, uint32_t key) {
  for (uint32_t i = 0; i < f->state_count; i++)
    if (f->states[i].key == key) return &f->states[i];
  return NULL;
}

bool tm_flares_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  tm_flares *f = calloc(1, sizeof *f);
  if (!f) return false;
  game->flares = f;
  const tg_attr attrs[] = {{0, offsetof(flare_vertex, clip), TG_FLOAT4},
                           {1, offsetof(flare_vertex, uv), TG_FLOAT2},
                           {2, offsetof(flare_vertex, color), TG_FLOAT4},
                           {3, offsetof(flare_vertex, test), TG_FLOAT4},
                           {4, offsetof(flare_vertex, clouds), TG_FLOAT1}};
  tg_program_desc d = {.vertex_spirv = k_flare_vert_spv,
                       .vertex_spirv_size = sizeof k_flare_vert_spv,
                       .fragment_spirv = k_flare_frag_spv,
                       .fragment_spirv_size = sizeof k_flare_frag_spv,
                       .vertex_stride = sizeof(flare_vertex),
                       .attrs = attrs,
                       .attr_count = 5,
                       .uniform_size = 4 * sizeof(float),
                       .texture_count = 3};
  f->program = tg_program_create(game->gpu, &d);
  const tg_attr ground_attrs[] = {{0, offsetof(ground_vertex, clip), TG_FLOAT4},
                                  {1, offsetof(ground_vertex, uv), TG_FLOAT2},
                                  {2, offsetof(ground_vertex, color), TG_FLOAT4}};
  const tg_program_desc gd = {.vertex_spirv = k_groundrefl_vert_spv,
                              .vertex_spirv_size = sizeof k_groundrefl_vert_spv,
                              .fragment_spirv = k_groundrefl_frag_spv,
                              .fragment_spirv_size = sizeof k_groundrefl_frag_spv,
                              .vertex_stride = sizeof(ground_vertex),
                              .attrs = ground_attrs,
                              .attr_count = 3,
                              .uniform_size = 4 * sizeof(float),
                              .texture_count = 1};
  f->ground = tg_program_create(game->gpu, &gd);
  return f->program && f->ground;
}

void tm_flares_resources_destroy(ft_game *game) {
  tm_flares *f = game->flares;
  if (!f) return;
  if (f->program) tg_program_destroy(game->gpu, f->program);
  if (f->ground) tg_program_destroy(game->gpu, f->ground);
  free(f->ground_vertices);
  free(f->states);
  free(f->tests);
  free(f->key_of);
  for (uint32_t i = 0; i < f->picture_count; i++)
    if (f->pictures[i].texture) tg_texture_destroy(game->gpu, f->pictures[i].texture);
  free(f->vertices);
  free(f->picture_of);
  free(f);
  game->flares = NULL;
}

// a flare picture, loaded once
static uint32_t picture(ft_game *game, tm_flares *f, const char *file, const char *pack_file) {
  if (!file && !pack_file) return UINT32_MAX;
  for (uint32_t i = 0; i < f->picture_count; i++)
    if ((file && f->pictures[i].file && strcmp(file, f->pictures[i].file) == 0) ||
        (!file && pack_file && f->pictures[i].pack_file && strcmp(pack_file, f->pictures[i].pack_file) == 0))
      return f->pictures[i].texture ? i : UINT32_MAX;
  if (f->picture_count == 16) return UINT32_MAX;
  flare_picture *p = &f->pictures[f->picture_count++];
  p->file = file, p->pack_file = pack_file;
  const ft_engine_api *api = game->engine;
  void *engine_data = NULL;
  uint8_t *data = NULL;
  size_t size = 0;
  if (file) {
    if (api->read_file(file, &engine_data, &size)) data = engine_data;
  } else {
    data = tmuf_packs_read(game->packs, pack_file, &size);
  }
  if (data) {
    tm_image image;
    if (tm_image_load(data, size, tg_supports_bc(game->gpu), &image)) {
      p->texture = tg_texture_create(game->gpu, &image);
      tm_image_free(&image);
    }
    if (engine_data) api->free_file_data(engine_data);
    else tmuf_free(data);
  }
  return p->texture ? f->picture_count - 1 : UINT32_MAX;
}

static void clip_of(const float vp[16], const float p[3], float out[4]) {
  for (int r = 0; r < 4; r++)
    out[r] = vp[r] * p[0] + vp[4 + r] * p[1] + vp[8 + r] * p[2] + vp[12 + r];
}

typedef struct view {
  const float *view_proj;
  float eye[3], right[3], up[3], forward[3];
  float far_z;
  float dt;     // seconds since the last frame, 0: settled at once
} view;

// a quad facing the camera, half `half` across, its test point `test`
static void add(tm_flares *f, const view *v, uint32_t key, uint32_t pic, const float centre[3], float half,
                const float rgb[3], float target, const float test[3], bool clouds) {
  if (pic == UINT32_MAX || target <= 0.01f) return;
  // the eased intensity, from the last test (LensFlaresFindVisible)
  flare_state *st = state_of(f, key);
  if (!st) return;
  st->frame = f->frame;
  if (v->dt <= 0.f) st->intensity = st->visible ? target : 0.f;
  else if (st->visible) st->intensity = fminf(st->intensity + target * 3.f * v->dt, target);
  else st->intensity = fmaxf(st->intensity - 3.f * v->dt, 0.f);
  const float intensity = st->intensity;
  if (f->quad_count == f->quad_cap) {
    const uint32_t cap = f->quad_cap ? f->quad_cap * 2 : 64;
    flare_vertex *vg = realloc(f->vertices, sizeof *vg * cap * 6);
    uint32_t *pg = realloc(f->picture_of, sizeof *pg * cap);
    float(*tg_)[4] = realloc(f->tests, sizeof *tg_ * cap);
    uint32_t *kg = realloc(f->key_of, sizeof *kg * cap);
    if (vg) f->vertices = vg;
    if (pg) f->picture_of = pg;
    if (tg_) f->tests = tg_;
    if (kg) f->key_of = kg;
    if (!vg || !pg || !tg_ || !kg) return;
    f->quad_cap = cap;
  }
  float t[4];
  clip_of(v->view_proj, test, t);
  static const float corner[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
  flare_vertex q[4];
  for (int i = 0; i < 4; i++) {
    float p[3];
    for (int k = 0; k < 3; k++)
      p[k] = centre[k] + (corner[i][0] * v->right[k] + corner[i][1] * v->up[k]) * half;
    clip_of(v->view_proj, p, q[i].clip);
    if (q[i].clip[3] <= 0.f) return; // behind the eye
    q[i].clip[2] = 0.5f * q[i].clip[3]; // no depth test: always inside the depth range
    q[i].uv[0] = 0.5f + 0.5f * corner[i][0];
    q[i].uv[1] = 0.5f + 0.5f * corner[i][1];
    for (int k = 0; k < 3; k++)
      q[i].color[k] = rgb[k] * intensity;
    q[i].color[3] = 1.f;
    memset(q[i].test, 0, sizeof q[i].test); // shown as eased: no test of its own
    q[i].clouds = clouds ? 1.f : 0.f;
  }
  static const int tri[6] = {0, 1, 2, 2, 1, 3};
  for (int i = 0; i < 6; i++)
    f->vertices[f->quad_count * 6 + (uint32_t)i] = q[tri[i]];
  memcpy(f->tests[f->quad_count], t, sizeof t);
  f->key_of[f->quad_count] = key;
  f->picture_of[f->quad_count++] = pic;
}

static float len3(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// a light at (pos, dir): seen, and how faded, from the eye
static float light_seen(const view *v, const tmuf_light *l, const float pos[3], const float dir[3]) {
  const float to_eye[3] = {v->eye[0] - pos[0], v->eye[1] - pos[1], v->eye[2] - pos[2]};
  const float dist = len3(to_eye);
  if (l->radius[3] > 0.f && dist > l->radius[3]) return 0.f;
  if (l->kind == TMUF_LIGHT_SPOT && dist > 0.f) {
    const float c = (to_eye[0] * dir[0] + to_eye[1] * dir[1] + to_eye[2] * dir[2]) / dist;
    if (c < l->cos_flare) return 0.f;
  }
  const float fade = (v->far_z - dist) / (0.2f * v->far_z);
  return fade < 0.f ? 0.f : fade > 1.f ? 1.f : fade;
}

static void add_light(ft_game *game, tm_flares *f, const view *v, uint32_t key, const tmuf_light *l,
                      const float pos[3], const float dir[3], float intensity) {
  const float seen = light_seen(v, l, pos, dir);
  if (seen <= 0.f) return;
  // the test point, moved toward the camera
  float test[3], d[3] = {v->eye[0] - pos[0], v->eye[1] - pos[1], v->eye[2] - pos[2]};
  const float dl = len3(d);
  for (int k = 0; k < 3; k++)
    test[k] = pos[k] + (dl > 0.f ? d[k] / dl * l->flare_bias_z : 0.f);
  // a light without a flare picture: the renderer's default
  // (CVisionViewport::LensFlareAdd), WhiteFlare1 (IA4's trace, draw 455)
  const uint32_t pic = l->flare_file || l->flare_pack_file ? picture(game, f, l->flare_file, l->flare_pack_file)
                                                           : picture(game, f, NULL, default_flare);
  add(f, v, key, pic, pos, l->flare_size, l->rgb, l->flare_intensity * intensity * seen, test, false);
}

void tm_flares_test(ft_game *game, const ft_level *level, const ft_render_frame *frame) {
  tm_flares *f = game->flares;
  if (!f) return;
  f->built = false;
  if (!game->settings.flares || !level->track) return;
  f->quad_count = 0;
  f->frame++;
  const ft_camera *cam = &frame->state.camera;
  if (cam->orthographic) return;
  view v = {.view_proj = cam->view_proj, .far_z = cam->far_z > 0.f ? cam->far_z : 50000.f};
  // the frame's period, for easing (none for the module's test pictures)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  v.dt = f->last_seconds > 0.0 ? (float)fmin(now - f->last_seconds, 0.2) : 0.f;
  f->last_seconds = now;
  if (getenv("TM_TEST_CAMERA")) v.dt = 0.f;
  v.eye[0] = cam->eye.x, v.eye[1] = cam->eye.y, v.eye[2] = cam->eye.z;
  v.forward[0] = cam->forward.x, v.forward[1] = cam->forward.y, v.forward[2] = cam->forward.z;
  const float cu[3] = {cam->up.x, cam->up.y, cam->up.z};
  v.right[0] = v.forward[1] * cu[2] - v.forward[2] * cu[1];
  v.right[1] = v.forward[2] * cu[0] - v.forward[0] * cu[2];
  v.right[2] = v.forward[0] * cu[1] - v.forward[1] * cu[0];
  const float rl = len3(v.right);
  for (int k = 0; k < 3; k++)
    v.right[k] = rl > 0.f ? v.right[k] / rl : 0.f;
  v.up[0] = v.right[1] * v.forward[2] - v.right[2] * v.forward[1];
  v.up[1] = v.right[2] * v.forward[0] - v.right[0] * v.forward[2];
  v.up[2] = v.right[0] * v.forward[1] - v.right[1] * v.forward[0];

  // the sun
  const tmuf_weather *w = tmuf_track_weather(level->track);
  const tm_light *light = &level->light;
  if (w && w->sun_flare && !light->moon) {
    const float d = 0.95f * v.far_z;
    const float centre[3] = {v.eye[0] - light->sun_dir[0] * d, v.eye[1] - light->sun_dir[1] * d,
                             v.eye[2] - light->sun_dir[2] * d};
    const float half = tanf(w->flare_size_sun * 3.14159265f / 180.f) * d;
    add(f, &v, KEY_SUN, picture(game, f, w->flare_sun.file, w->flare_sun.pack_file), centre, half, light->sun_rgb,
        1.f, centre, true);
  }
  // the map's lamps
  const tmuf_light *lights = NULL;
  const uint32_t n = tmuf_track_lights(level->track, &lights);
  for (uint32_t i = 0; i < n; i++)
    if (lights[i].flags & TMUF_LIGHT_FLAG_LENS_FLARE)
      add_light(game, f, &v, KEY_LAMP + i, &lights[i], lights[i].position, lights[i].direction, lights[i].intensity);
  // the car's lights
  if (level->car && frame->world && game->settings.draw_car && !game->camera_inside) {
    tm_car_light l[8];
    const uint32_t cn = tm_car_lights(level->car, l, 8);
    const float brake = frame->world->w.input.brake ? 1.f : 0.f;
    const float intensity = w && w->is_night ? 0.5f + 0.5f * brake : 0.5f * brake;
    for (uint32_t i = 0; i < cn; i++)
      if (l[i].light->flags & TMUF_LIGHT_FLAG_LENS_FLARE)
        add_light(game, f, &v, KEY_CAR + i, l[i].light, l[i].position, l[i].direction, intensity);
  }
  if (!f->quad_count) return;

  f->built = true;
  // lights no longer shown are forgotten (the game removes them at once)
  for (uint32_t i = 0; i < f->state_count;)
    if (f->states[i].frame != f->frame) f->states[i] = f->states[--f->state_count];
    else i++;
  // their tests (the flare program's own): the pixel the light's point is
  // in, against the frame's depth (any sample nearer than the point hides
  // nothing: visible when one sample passes), one occlusion query each
  const uint32_t tested = f->quad_count < VIS_WIDTH ? f->quad_count : VIS_WIDTH;
  const uint32_t serial = tg_frame_serial(game->gpu);
  vis_frame *vf = &f->vis_frames[serial % 4u];
  vf->serial = serial;
  vf->count = tested;
  const float vw = cam->viewport.x, vh = cam->viewport.y;
  const tg_state test = {.depth_test = true, .no_color_write = true, .cull = TG_CULL_NONE};
  const tg_sampler samplers[3] = {TG_SAMPLER_CLAMP, {3, 3, 1, 0}, {3, 3, 1, 0}};
  const float size[4] = {vw, vh, 0.f, 0.f};
  for (uint32_t q = 0; q < tested; q++) {
    vf->keys[q] = f->key_of[q];
    const float *t = f->tests[q];
    if (t[3] <= 0.f) {
      vf->query[q] = VIS_SEEN;
      continue;
    }
    const float nx = t[0] / t[3], ny = t[1] / t[3], nz = t[2] / t[3];
    const float px = floorf((nx * 0.5f + 0.5f) * vw), py = floorf((ny * 0.5f + 0.5f) * vh);
    if (px < 0.f || py < 0.f || px >= vw || py >= vh) {
      vf->query[q] = VIS_HIDDEN;
      continue;
    }
    // the pixel's square, at the point's depth
    const float x0 = px / vw * 2.f - 1.f, x1 = (px + 1.f) / vw * 2.f - 1.f;
    const float y0 = py / vh * 2.f - 1.f, y1 = (py + 1.f) / vh * 2.f - 1.f;
    static const int tri[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    flare_vertex quad[6];
    memset(quad, 0, sizeof quad);
    for (int c = 0; c < 6; c++) {
      quad[c].clip[0] = tri[c][0] ? x1 : x0;
      quad[c].clip[1] = tri[c][1] ? y1 : y0;
      quad[c].clip[2] = nz, quad[c].clip[3] = 1.f;
    }
    vf->query[q] = tg_query_begin(game->gpu);
    if (vf->query[q] == UINT32_MAX) {
      vf->query[q] = VIS_SEEN;
      continue;
    }
    const tg_texture textures[3] = {0, 0, 0};
    tg_draw_dynamic(game->gpu, f->program, &test, quad, 6, textures, samplers, size);
    tg_query_end(game->gpu);
  }
}

void tm_flares_render(ft_game *game, const ft_level *level, const ft_render_frame *frame, tg_texture clouds_occlusion) {
  tm_flares *f = game->flares;
  (void)level;
  if (!f || !f->built || !f->quad_count) return;
  const ft_camera *cam = &frame->state.camera;
  // added, where they were seen (the tests' counts: a frame or two before)
  const float size[4] = {cam->viewport.x, cam->viewport.y, 0.f, 0.f};
  const tg_state state = {.blend_src = TG_BLEND_ONE, .blend_dst = TG_BLEND_ONE, .cull = TG_CULL_NONE};
  const tg_sampler samplers[3] = {TG_SAMPLER_CLAMP, {3, 3, 1, 0}, {3, 3, 1, 0}};
  flare_vertex *batch = malloc(sizeof *batch * f->quad_count * 6);
  if (!batch) return;
  for (uint32_t p = 0; p < f->picture_count; p++) {
    // this picture's quads, together
    uint32_t count = 0;
    for (uint32_t q = 0; q < f->quad_count; q++) {
      if (f->picture_of[q] != p) continue;
      memcpy(&batch[6 * count], &f->vertices[6 * q], sizeof(flare_vertex) * 6);
      const flare_state *st = state_of_const(f, f->key_of[q]);
      for (int c = 0; c < 6; c++)
        batch[6 * count + (uint32_t)c].test[0] = st && st->visible ? 1.f : 0.f;
      count++;
    }
    if (!count) continue;
    const tg_texture textures[3] = {f->pictures[p].texture, 0, clouds_occlusion};
    tg_draw_dynamic(game->gpu, f->program, &state, batch, count * 6, textures, samplers, size);
  }
  free(batch);
}

void tm_flares_after_frame(ft_game *game) {
  tm_flares *f = game->flares;
  if (!f) return;
  uint32_t serial = 0;
  const uint64_t *counts = NULL;
  const uint32_t n = tg_query_results(game->gpu, &serial, &counts);
  if (!n || serial == f->vis_taken) return;
  const vis_frame *vf = &f->vis_frames[serial % 4u];
  if (vf->serial != serial) return;
  f->vis_taken = serial;
  for (uint32_t q = 0; q < vf->count; q++) {
    flare_state *st = state_of(f, vf->keys[q]);
    if (!st) continue;
    const uint32_t k = vf->query[q];
    st->visible = k == VIS_SEEN ? true : k == VIS_HIDDEN ? false : k < n && counts[k] > 0;
  }
}

// --- the lamps' fake ground reflection -------------------------------------
// (CVisionViewportDx9::LightFakeGroundReflecGlossInDstAlpha, the main view's,
// with the lens flares: after the opaque scene and the sky, before the
// transparent draws). Each spot with CPlugLight flag bit 1 seen from within
// its flare range (radius [3]) and AngleFlare + 30 degrees, mirrored about
// its reflect plane (tmuf_light.reflect_plane), shows a glow streak: a
// billboard at the mirror image, FlareSize either side and above, FlareSize
// (4 |camera up . Y| + 1) below, each corner pulled along its camera ray onto
// the plane 0.25 m up (its place on screen kept, its depth the ground's),
// WhiteFlare1 (bottom row first) at v 0 below, 0.5 at the image, 1 above,
// u 0 on the screen's right; its colour the light's specular one by
// min((c - cos) / (c - cos_flare), 1) (1 - d^2 / R^2), c the cosine of the
// widened half angle; added by the frame's alpha (DESTALPHA / ONE), where
// the CSpecL ground wrote its gloss (track.frag, spec_cube.z). StarBayA1's
// Sunset trace (draw 536): 64 quads, within 1 of the game on 95 % of the
// pixels they touch.

static bool reflects(const tmuf_light *l) {
  return l->kind == TMUF_LIGHT_SPOT && (l->plug_flags & 2u) && l->radius[3] > 0.f && l->flare_size > 0.f;
}

bool tm_flares_ground_reflects(ft_game *game, const ft_level *level) {
  if (!game->flares || !game->flares->ground || !game->settings.flares || !level || !level->track) return false;
  const tmuf_light *lights = NULL;
  const uint32_t n = tmuf_track_lights(level->track, &lights);
  for (uint32_t i = 0; i < n; i++)
    if (reflects(&lights[i])) return true;
  return false;
}

static float dot3f(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void tm_flares_ground_reflection(ft_game *game, const ft_level *level, const ft_camera *cam) {
  tm_flares *f = game->flares;
  if (!tm_flares_ground_reflects(game, level) || cam->orthographic) return;
  const uint32_t pic = picture(game, f, NULL, default_flare);
  if (pic == UINT32_MAX) return;
  const float eye[3] = {cam->eye.x, cam->eye.y, cam->eye.z};
  const float fw[3] = {cam->forward.x, cam->forward.y, cam->forward.z}, cu[3] = {cam->up.x, cam->up.y, cam->up.z};
  float right[3] = {fw[1] * cu[2] - fw[2] * cu[1], fw[2] * cu[0] - fw[0] * cu[2], fw[0] * cu[1] - fw[1] * cu[0]};
  const float rl = len3(right);
  if (rl <= 0.f) return;
  for (int k = 0; k < 3; k++)
    right[k] /= rl;
  const float up[3] = {right[1] * fw[2] - right[2] * fw[1], right[2] * fw[0] - right[0] * fw[2],
                       right[0] * fw[1] - right[1] * fw[0]};
  const float k_down = fabsf(up[1]) * 4.f + 1.f;
  const tmuf_light *lights = NULL;
  const uint32_t n = tmuf_track_lights(level->track, &lights);
  uint32_t count = 0;
  for (uint32_t i = 0; i < n; i++) {
    const tmuf_light *l = &lights[i];
    if (!reflects(l)) continue;
    const float *pl = l->reflect_plane;
    // the light and its axis mirrored about the plane
    const float s = dot3f(pl, l->position) + pl[3], a = dot3f(pl, l->direction);
    float p[3], axis[3], to_eye[3];
    for (int k = 0; k < 3; k++) {
      p[k] = l->position[k] - 2.f * s * pl[k];
      axis[k] = l->direction[k] - 2.f * a * pl[k];
      to_eye[k] = eye[k] - p[k];
    }
    const float d2 = dot3f(to_eye, to_eye), r2 = l->radius[3] * l->radius[3];
    if (d2 < 1e-10f || d2 > r2) continue;
    const float c = cosf(fminf(l->angle_flare + 30.f, 360.f) * 3.14159265f / 360.f);
    const float ca = dot3f(axis, to_eye) / sqrtf(d2);
    if (c > ca) continue;
    const float fa = c != l->cos_flare ? fminf((c - ca) / (c - l->cos_flare), 1.f) : 1.f;
    const float fade = fa * (1.f - d2 / r2);
    if (count == f->ground_cap) {
      const uint32_t cap = f->ground_cap ? f->ground_cap * 2 : 64;
      ground_vertex *g = realloc(f->ground_vertices, sizeof *g * cap * 12);
      if (!g) break;
      f->ground_vertices = g, f->ground_cap = cap;
    }
    // the six points, bottom to top, the screen's right first
    const float size = l->flare_size, ys[3] = {-size * k_down, 0.f, size};
    const float plane_d = pl[3] - 0.25f, eye_side = dot3f(pl, eye) + plane_d;
    float clip[6][4];
    for (int q = 0; q < 6; q++) {
      const float x = q & 1 ? -size : size, y = ys[q / 2];
      float pt[3], dir[3];
      for (int k = 0; k < 3; k++)
        pt[k] = p[k] + right[k] * x + up[k] * y;
      for (int k = 0; k < 3; k++)
        dir[k] = pt[k] - eye[k];
      const float dl2 = dot3f(dir, dir);
      if (dl2 > 1e-10f) {
        const float dl = sqrtf(dl2);
        for (int k = 0; k < 3; k++)
          dir[k] /= dl;
        const float den = dot3f(pl, dir);
        if (fabsf(den) > 1e-5f) {
          const float t = -eye_side / den;
          if (t >= 1e-5f)
            for (int k = 0; k < 3; k++)
              pt[k] = eye[k] + t * dir[k];
        }
      }
      clip_of(cam->view_proj, pt, clip[q]);
    }
    float rgb[3];
    for (int k = 0; k < 3; k++)
      rgb[k] = fminf(fmaxf(l->specular_rgb[k] * fade, 0.f), 1.f);
    // two quads: rows 0-1 (v 0 to 0.5) and 1-2 (0.5 to 1)
    static const int tri[6] = {0, 1, 2, 2, 1, 3};
    ground_vertex *o = &f->ground_vertices[12 * count++];
    for (int j = 0; j < 2; j++)
      for (int t = 0; t < 6; t++, o++) {
        const int q = 2 * j + tri[t];
        memcpy(o->clip, clip[q], sizeof o->clip);
        o->uv[0] = (float)(q & 1);
        o->uv[1] = 0.5f * (float)(q / 2);
        o->color[0] = rgb[0], o->color[1] = rgb[1], o->color[2] = rgb[2], o->color[3] = 0.f;
      }
  }
  if (!count) return;
  const tg_state state = {.blend_src = TG_BLEND_DESTALPHA,
                          .blend_dst = TG_BLEND_ONE,
                          .depth_test = true,
                          .depth_write = false,
                          .cull = TG_CULL_NONE};
  const tg_sampler samplers[1] = {TG_SAMPLER_CLAMP};
  const tg_texture textures[1] = {f->pictures[pic].texture};
  const float unused[4] = {0.f, 0.f, 0.f, 0.f};
  tg_draw_dynamic(game->gpu, f->ground, &state, f->ground_vertices, count * 12, textures, samplers, unused);
}
