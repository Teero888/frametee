// Rally's falling leaves (leaves_spec.md): CSceneMobilLeaves, the tree
// blocks' CMotionEmitterLeaves (tmuf_track_leaves).
//
//   slots     max_count leaves (150); each rendered frame
//             (CSceneMobilLeaves::OnRenderBefore) a slot whose leaf is dead
//             or whose spawn point left the view's box respawns at once in
//             an emitter whose centre is in the box, at most
//             emitter_max_count (5) per emitter (§2.4, §2.5); no emitter in
//             the box: nothing changes
//   box       FarZ (128 m) deep in front of the eye, a fixed 37.5° half
//             angle up and down, times the aspect across (§2.2)
//   life      a random pre-age: it dies at the end of one respawn period
//             (6 s) of a closed-form fall, born anywhere along it, so a tree
//             coming into view shows every stage at once (§2.8, §3): a
//             Lissajous swing, a fall, two spins; through the ground
//   drawing   a quad folded along its v1-v2 diagonal by Curvature, unlit:
//             leafD x TreeClouds (a constant grey) x (1, 1, 1, fade), fading
//             from 90.5 m to FarZ in the squared distance (§4), blended
//             after the scenery
//
// The game's leaves run on the CRT's rand shared with everything else; here
// on its own copy, seeded when the run starts over (a jump back in time).

#include "tmuf_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct leaf_vertex { // tm_particle_program_create's
  float pos[3];
  float uv[2];
  uint32_t color; // RGBA8
} leaf_vertex;

typedef struct leaf_uniforms {
  float view_proj[16];
  float mode[4];
  float uv0[8], uv1[8];
} leaf_uniforms;

typedef struct leaf { // the game's 0x34 bytes record
  int32_t emitter;    // -1 none
  float phase;        // β's
  float p0[3];        // its spawn point
  uint32_t death;     // ms
  float size, alpha_rate, beta_rate, fall, swing_rate, swing_radius;
} leaf;

struct tm_leaves {
  const tmuf_leaves *def;
  tg_program *program;
  tm_picture_table pictures;
  uint32_t picture[2];  // leafD, TreeClouds (UINT32_MAX none)
  tg_sampler sampler[2];
  leaf *slots;
  uint32_t *counts;     // per emitter, its leaves
  uint32_t *visible;    // this frame's emitters in the box
  leaf_vertex *verts;
  uint32_t rng;         // the MSVC CRT's holdrand
  uint32_t last_ms;
  bool started;
};

static int crt_rand(tm_leaves *s) {
  s->rng = s->rng * 214013u + 2531011u;
  return (int)((s->rng >> 16) & 0x7fffu);
}

// rand() / 32767.0, stored as a float
static float r01(tm_leaves *s) { return (float)((double)crt_rand(s) / 32767.0); }

// uniform on [m - h, m + h], the game's float stores
static float spread(tm_leaves *s, float m, float h) {
  const float r = r01(s);
  return m + (-h + r * (h - -h));
}

static void reset(tm_leaves *s) {
  for (uint32_t i = 0; i < s->def->max_count; i++) {
    s->slots[i].emitter = -1;
    s->slots[i].death = 0;
  }
  memset(s->counts, 0, sizeof *s->counts * s->def->emitter_count);
  s->rng = 0;
}

tm_leaves *tm_leaves_create(ft_game *game, ft_level *level) {
  const tmuf_leaves *def = tmuf_track_leaves(level->track);
  const tmuf_visuals *vis = tmuf_track_visuals(level->track);
  if (!def || !vis || !def->emitter_count || !def->max_count || !game->gpu) return NULL;
  if (def->material >= vis->material_count) return NULL;
  const tmuf_visual_material *m = &vis->materials[def->material];
  if (!m->texture_count) return NULL;
  tm_leaves *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->def = def;
  s->slots = calloc(def->max_count, sizeof *s->slots);
  s->counts = calloc(def->emitter_count, sizeof *s->counts);
  s->visible = calloc(def->emitter_count, sizeof *s->visible);
  s->verts = calloc((size_t)def->max_count * 6u, sizeof *s->verts);
  tm_picture_table_init(game, &s->pictures, level->track);
  // the shader's two stages, MODULATE: the leaf, then RallyEnvmapClouds
  // (TreeClouds.dds, every texel (127, 125, 127): its generated uvs don't
  // matter)
  for (uint32_t k = 0; k < 2; k++) {
    s->picture[k] = k < m->texture_count ? tm_map_index(game, &s->pictures, &m->textures[k]) : UINT32_MAX;
    s->sampler[k] = TG_SAMPLER_WRAP;
    if (k < m->texture_count && m->textures[k].address[0]) {
      s->sampler[k].address_u = (uint8_t)m->textures[k].address[0];
      s->sampler[k].address_v = (uint8_t)m->textures[k].address[1];
    }
  }
  s->program = tm_particle_program_create(game);
  if (!s->slots || !s->counts || !s->visible || !s->verts || !s->program || s->picture[0] == UINT32_MAX) {
    tm_leaves_destroy(game, s);
    return NULL;
  }
  reset(s);
  return s;
}

void tm_leaves_destroy(ft_game *game, tm_leaves *s) {
  if (!s) return;
  if (s->program) tg_program_destroy(game->gpu, s->program);
  tm_picture_table_free(game, &s->pictures);
  free(s->slots);
  free(s->counts);
  free(s->visible);
  free(s->verts);
  free(s);
}

// --- the view's box (GmBoxAligned's test, <= passes) -----------------------------

typedef struct view_box {
  float eye[3], x[3], y[3], z[3]; // the view's axes: across, up, forward
  float hx, hy, depth;
} view_box;

static bool inbox(const view_box *b, const float p[3]) {
  const float d[3] = {p[0] - b->eye[0], p[1] - b->eye[1], p[2] - b->eye[2]};
  const float vx = d[0] * b->x[0] + d[1] * b->x[1] + d[2] * b->x[2];
  const float vy = d[0] * b->y[0] + d[1] * b->y[1] + d[2] * b->y[2];
  const float vz = d[0] * b->z[0] + d[1] * b->z[1] + d[2] * b->z[2];
  return fabsf(vx) <= b->hx && fabsf(vy) <= b->hy && fabsf(vz - 0.5f * b->depth) <= 0.5f * b->depth;
}

// --- SpawnLeaf (§2.5): the draws in the game's order ------------------------------

static bool spawn(tm_leaves *s, leaf *l, uint32_t nvis, uint32_t now) {
  const tmuf_leaves *d = s->def;
  const uint32_t k = (uint32_t)((double)crt_rand(s) * (1.0 / 32768.0) * (double)nvis);
  const uint32_t ei = s->visible[k < nvis ? k : nvis - 1];
  // (the multiplier: 1, 200 the frame its tree is hit; not modelled)
  if ((float)d->emitter_max_count * 1.f <= (float)s->counts[ei]) return false;
  l->emitter = (int32_t)ei;
  s->counts[ei]++;
  const tmuf_leaf_emitter *e = &d->emitters[ei];
  const float hx = e->half[0], hy = e->half[1], hz = e->half[2];
  float x = 0.f, y = 0.f, z = 0.f;
  for (int tries = 0; tries < 1000; tries++) { // inside the ellipsoid
    z = -hz + r01(s) * (hz - -hz);
    y = -hy + r01(s) * (hy - -hy);
    x = -hx + r01(s) * (hx - -hx);
    const float qx = hx > 0.f ? x / hx : 0.f, qy = hy > 0.f ? y / hy : 0.f, qz = hz > 0.f ? z / hz : 0.f;
    if (qx * qx + qy * qy + qz * qz < 1.f) break;
  }
  l->p0[0] = e->center[0] + x;
  l->p0[1] = e->center[1] + y;
  l->p0[2] = e->center[2] + z;
  l->swing_radius = spread(s, d->swing_radius, d->swing_radius_random);
  l->swing_rate = spread(s, d->swing_rate, d->swing_rate_random);
  l->phase = (float)((double)r01(s) * 6.283185307179586 - 3.141592653589793);
  l->alpha_rate = spread(s, 0.f, d->alpha_speed_max);
  l->beta_rate = spread(s, 0.f, d->beta_speed_max);
  l->size = spread(s, d->size, d->size_random);
  const float life = r01(s) * d->respawn_period;
  l->death = now + (uint32_t)(int64_t)life;
  l->fall = spread(s, d->fall, d->fall_random);
  return true;
}

static uint32_t rgba(float a) {
  const float c = a < 0.f ? 0.f : a > 1.f ? 1.f : a;
  return 0x00ffffffu | (uint32_t)lrintf(c * 255.f) << 24;
}

// --- LeafAnimation (§3) and the quad (§4.1) ---------------------------------------

static void leaf_quad(const tm_leaves *s, const leaf *l, uint32_t now, const float eye[3], leaf_vertex out[6]) {
  const tmuf_leaves *d = s->def;
  const float t = (float)((double)(int32_t)((uint32_t)(int32_t)truncf(d->respawn_period) - l->death + now) * 0.001);
  const float p[3] = {l->p0[0] + l->swing_radius * cosf(l->swing_rate * t), l->p0[1] - l->fall * t,
                      // (sic: the radius as the z swing's rate)
                      l->p0[2] + l->swing_radius * sinf(l->swing_radius * t)};
  const float t1 = l->alpha_rate * t, t2 = l->beta_rate * t + l->phase;
  const float c1 = cosf(t1), s1 = sinf(t1), c2 = cosf(t2), s2 = sinf(t2);
  const float L0[3] = {s1 * c2, -c1, s1 * s2};
  const float L2[3] = {c1 * c2, s1, c1 * s2};
  const float L1[3] = {L0[2] * L2[1] - L0[1] * L2[2], L0[0] * L2[2] - L2[0] * L0[2], L0[1] * L2[0] - L0[0] * L2[1]};
  float ls[3], ms[3], k[3];
  for (int i = 0; i < 3; i++) {
    ls[i] = L0[i] * l->size;
    ms[i] = L1[i] * l->size;
    k[i] = L2[i] * d->curvature;
  }
  // the fade: linear in the squared distance from half FarZ² to FarZ²
  const float dd[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
  const float d2 = dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2], r2 = d->far_z * d->far_z;
  const float alpha = d2 > 0.5f * r2 ? (r2 - d2) / (0.5f * r2) : 1.f;
  const uint32_t color = rgba(alpha);
  leaf_vertex v[4];
  for (int i = 0; i < 3; i++) {
    v[0].pos[i] = p[i] + ls[i] - ms[i];
    v[1].pos[i] = p[i] - ls[i] - ms[i] + k[i];
    v[2].pos[i] = p[i] + ls[i] + ms[i] + k[i];
    v[3].pos[i] = p[i] - ls[i] + ms[i];
  }
  static const float uv[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {1.f, 1.f}};
  for (int j = 0; j < 4; j++) {
    v[j].uv[0] = uv[j][0];
    v[j].uv[1] = uv[j][1];
    v[j].color = color;
  }
  // CPlugVisualQuads: (0, 1, 2), (1, 3, 2)
  out[0] = v[0], out[1] = v[1], out[2] = v[2];
  out[3] = v[1], out[4] = v[3], out[5] = v[2];
}

void tm_leaves_render(ft_game *game, tm_leaves *s, const ft_render_frame *frame) {
  if (!s || !frame->world) return;
  const tmuf_leaves *d = s->def;
  const ft_camera *cam = &frame->state.camera;
  if (cam->orthographic) return;
  // the engine's clock (the flags' and the shader functions'), whole ms
  const float a = frame->previous_world ? (frame->alpha < 0.f ? 0.f : frame->alpha > 1.f ? 1.f : frame->alpha) : 1.f;
  const float tick = (float)frame->world->w.tick - (frame->previous_world ? 1.f - a : 0.f);
  const uint32_t now = tick > 0.f ? (uint32_t)(tick * (float)TMUF_TICK_MS) : 0u;
  // back in time: the run starts over (ResetLeaves)
  if (!s->started || now < s->last_ms) reset(s);
  s->started = true;
  s->last_ms = now;

  view_box b;
  b.eye[0] = cam->eye.x, b.eye[1] = cam->eye.y, b.eye[2] = cam->eye.z;
  b.z[0] = cam->forward.x, b.z[1] = cam->forward.y, b.z[2] = cam->forward.z;
  const float cu[3] = {cam->up.x, cam->up.y, cam->up.z};
  b.x[0] = b.z[1] * cu[2] - b.z[2] * cu[1];
  b.x[1] = b.z[2] * cu[0] - b.z[0] * cu[2];
  b.x[2] = b.z[0] * cu[1] - b.z[1] * cu[0];
  const float xl = sqrtf(b.x[0] * b.x[0] + b.x[1] * b.x[1] + b.x[2] * b.x[2]);
  if (!(xl > 0.f)) return;
  for (int i = 0; i < 3; i++)
    b.x[i] /= xl;
  b.y[0] = b.x[1] * b.z[2] - b.x[2] * b.z[1];
  b.y[1] = b.x[2] * b.z[0] - b.x[0] * b.z[2];
  b.y[2] = b.x[0] * b.z[1] - b.x[1] * b.z[0];
  const float aspect = cam->viewport.y > 0.f ? cam->viewport.x / cam->viewport.y : 4.f / 3.f;
  const float ty = (float)tan(0.6544984579086304); // 37.5°: fixed, not the camera's
  b.depth = d->far_z;
  b.hy = d->far_z * ty;
  b.hx = b.hy * aspect;

  // the emitters seen (their centres)
  uint32_t nvis = 0;
  for (uint32_t i = 0; i < d->emitter_count; i++)
    if (inbox(&b, d->emitters[i].center)) s->visible[nvis++] = i;
  if (!nvis) return; // the leaves keep their state

  uint32_t n = 0;
  for (uint32_t i = 0; i < d->max_count; i++) {
    leaf *l = &s->slots[i];
    if (l->death < now || !inbox(&b, l->p0)) {
      if (l->emitter >= 0 && (uint32_t)l->emitter < d->emitter_count && s->counts[l->emitter] > 0)
        s->counts[l->emitter]--;
      l->emitter = -1;
      if (!spawn(s, l, nvis, now)) continue;
    }
    leaf_quad(s, l, now, b.eye, s->verts + n);
    n += 6;
  }
  if (!n) return;

  leaf_uniforms u;
  memset(&u, 0, sizeof u);
  memcpy(u.view_proj, cam->view_proj, sizeof u.view_proj);
  u.mode[0] = s->picture[1] != UINT32_MAX ? 3.f : 0.f; // stage 0 x stage 1 x the vertex colour
  static const float ident[8] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
  memcpy(u.uv0, ident, sizeof ident);
  memcpy(u.uv1, ident, sizeof ident);
  const tg_texture textures[2] = {
      s->pictures.textures[s->picture[0]],
      s->pictures.textures[s->picture[1] != UINT32_MAX ? s->picture[1] : s->picture[0]]};
  const tg_sampler samplers[2] = {s->sampler[0], s->sampler[1]};
  // blended SRC_ALPHA / INV_SRC_ALPHA, alpha tested != 0, no z write, double sided
  const tg_state state = {.blend_src = TG_BLEND_SRCALPHA, .blend_dst = TG_BLEND_INVSRCALPHA, .depth_test = true,
                          .cull = TG_CULL_NONE};
  tg_draw_dynamic(game->gpu, s->program, &state, s->verts, n, textures, samplers, &u);
}
