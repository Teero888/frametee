// The sun's shadows on the vertex-lit environments' scenery and on the
// Stadium's by day (its blocks, lawn and dirt: GbxShadow0, the B4 and A06
// traces), as the game makes its light mask's shadow (env_light_spec.md §3;
// Shadows = Complex):
//
//   static     one map of the zone's blocks and terrain along the sun, its
//              frame aligned with the world, drawn once
//   cascades   five maps of the view's depth slices (ends at 140, 60, 27,
//              10 and 4 m, each overlapping the next by 5 %), their frame
//              along the camera's right (or up) axis, fitted to the slice's
//              corners; the far ones left out where the static map is as
//              fine as they would be
//   receivers  the static map's 2x2 filtered test, then each cascade's
//              blended in far to near by its weight (track.frag)
//
// The maps are depth targets (the biased depth, reversed); the cascades
// share one, three cells across and two down.

#include "tmuf_internal.h"

#include "pssm_frag_spv.h"
#include "pssmsolid_frag_spv.h"
#include "pssm_vert_spv.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CELL 1280u         // a cascade's size
#define STATIC_SIZE 2048u  // the static map's
#define CAM_NEAR 0.2f      // the game's near plane the slices start from
#define DEPTH_BIAS 1.0f    // DepthBiasConst
static const float split_far[TM_PSSM_CASCADES] = {140.f, 60.f, 27.f, 10.f, 4.f}; // far to near

typedef struct pssm_uniforms {
  float rows[3][4];
  float cell[4];
  float params[4];
} pssm_uniforms;

// a map: its frame (rows u, v, L), box and depth range, its rows for the
// casters (unbiased depth) and for the receivers
typedef struct pssm_map {
  float axes[3][3];
  float centre[2], width[2], near, far;
  float caster[3][4], receiver[3][4];
  bool active;
} pssm_map;

struct tm_pssm {
  tg_program *caster, *caster_block; // the pictures', the blocks' (their vertices), alpha tested
  tg_program *solid, *solid_block;   // the same, opaque: no discard (the depth tested early)
  tg_target *atlas, *static_map;
  bool static_drawn;
  float static_light[3]; // the light the static map was drawn for
  pssm_map statics, cascades[TM_PSSM_CASCADES];
  uint32_t map; // the map being cast into (tm_pssm_cast)
};

bool tm_pssm_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  tm_pssm *p = calloc(1, sizeof *p);
  if (!p) return false;
  const tg_attr attrs[] = {{0, offsetof(tm_track_vertex, pos), TG_FLOAT3}, {1, offsetof(tm_track_vertex, uv), TG_FLOAT2}};
  const tg_program_desc desc = {.vertex_spirv = k_pssm_vert_spv,
                                .vertex_spirv_size = sizeof k_pssm_vert_spv,
                                .fragment_spirv = k_pssm_frag_spv,
                                .fragment_spirv_size = sizeof k_pssm_frag_spv,
                                .vertex_stride = sizeof(tm_track_vertex),
                                .attrs = attrs,
                                .attr_count = 2,
                                .uniform_size = sizeof(pssm_uniforms),
                                .texture_count = 1};
  p->caster = tg_program_create(game->gpu, &desc);
  const tg_attr block_attrs[] = {{0, offsetof(tm_block_vertex, pos), TG_FLOAT3},
                                 {1, offsetof(tm_block_vertex, uv), TG_FLOAT2}};
  tg_program_desc block = desc;
  block.vertex_stride = sizeof(tm_block_vertex);
  block.attrs = block_attrs;
  p->caster_block = tg_program_create(game->gpu, &block);
  tg_program_desc solid = desc, solid_block = block;
  solid.fragment_spirv = solid_block.fragment_spirv = k_pssmsolid_frag_spv;
  solid.fragment_spirv_size = solid_block.fragment_spirv_size = sizeof k_pssmsolid_frag_spv;
  p->solid = tg_program_create(game->gpu, &solid);
  p->solid_block = tg_program_create(game->gpu, &solid_block);
  if (!p->caster || !p->caster_block || !p->solid || !p->solid_block) {
    tg_program *all[4] = {p->caster, p->caster_block, p->solid, p->solid_block};
    for (int i = 0; i < 4; i++)
      if (all[i]) tg_program_destroy(game->gpu, all[i]);
    free(p);
    return false;
  }
  game->pssm = p;
  return true;
}

void tm_pssm_resources_destroy(ft_game *game) {
  tm_pssm *p = game->pssm;
  if (!p) return;
  if (p->atlas) tg_target_destroy(game->gpu, p->atlas);
  if (p->static_map) tg_target_destroy(game->gpu, p->static_map);
  if (p->caster) tg_program_destroy(game->gpu, p->caster);
  if (p->caster_block) tg_program_destroy(game->gpu, p->caster_block);
  if (p->solid) tg_program_destroy(game->gpu, p->solid);
  if (p->solid_block) tg_program_destroy(game->gpu, p->solid_block);
  free(p);
  game->pssm = NULL;
}

void tm_pssm_forget(ft_game *game) {
  if (game->pssm) game->pssm->static_drawn = false;
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
static bool normalize3(float v[3]) {
  const float n = sqrtf(dot3(v, v));
  if (!(n > 1e-12f)) return false;
  for (int k = 0; k < 3; k++)
    v[k] /= n;
  return true;
}

// the light frame from its u axis: rows (u, v = L x u, L)
static void frame_from_u(pssm_map *m, const float l[3], const float u[3]) {
  memcpy(m->axes[0], u, sizeof m->axes[0]);
  cross3(l, u, m->axes[1]);
  memcpy(m->axes[2], l, sizeof m->axes[2]);
}

// the map's rows: Shadow_ComputeDepthRangeAndBias (a size x size map, a
// texel of border)
static void rows_of(pssm_map *m, float size) {
  for (int k = 0; k < 2; k++) {
    // (widened by |x| 1e-4, as every bound)
    const float w = m->width[k] + fabsf(m->centre[k]) * 2e-4f;
    const float scale = (size - 2.f) / (size * w);
    for (int c = 0; c < 3; c++)
      m->caster[k][c] = m->receiver[k][c] = m->axes[k][c] * scale;
    m->caster[k][3] = m->receiver[k][3] = 0.5f + 0.5f / size - scale * m->centre[k];
  }
  const float range = m->far - m->near > 1e-3f ? m->far - m->near : 1e-3f;
  const float bias = DEPTH_BIAS * 7.f * 2.f * fmaxf(m->width[0], m->width[1]) / (2.f * size);
  for (int c = 0; c < 3; c++)
    m->caster[2][c] = m->receiver[2][c] = m->axes[2][c] / range;
  m->caster[2][3] = -m->near / range;
  m->receiver[2][3] = -(m->near + bias) / range;
}

// the light-frame box of points
static void box_of(pssm_map *m, const float (*points)[3], uint32_t n, float out_min[3], float out_max[3]) {
  for (int k = 0; k < 3; k++)
    out_min[k] = FLT_MAX, out_max[k] = -FLT_MAX;
  for (uint32_t i = 0; i < n; i++)
    for (int k = 0; k < 3; k++) {
      const float d = dot3(m->axes[k], points[i]);
      if (d < out_min[k]) out_min[k] = d;
      if (d > out_max[k]) out_max[k] = d;
    }
}

bool tm_pssm_setup(ft_game *game, const float light[3], const float zone_min[3], const float zone_max[3],
                   const ft_camera *cam, tm_track_uniforms *u) {
  tm_pssm *p = game->pssm;
  if (!p || cam->orthographic) return false;
  float l[3] = {light[0], light[1], light[2]};
  if (!normalize3(l)) return false;

  // the static map: world aligned (GmMat3::SetDOV(L, 1))
  pssm_map *s = &p->statics;
  static const float X[3] = {1.f, 0.f, 0.f}, Y[3] = {0.f, 1.f, 0.f};
  float lx[3], ly[3], ax[3];
  cross3(l, X, lx);
  cross3(l, Y, ly);
  if (dot3(ly, ly) > dot3(lx, lx)) {
    memcpy(ax, ly, sizeof ax);
  } else {
    const float d = dot3(X, l);
    for (int k = 0; k < 3; k++)
      ax[k] = -(X[k] - d * l[k]);
  }
  if (!normalize3(ax)) return false;
  frame_from_u(s, l, ax);
  // the zone's box: its 8 corners in the light frame
  float corners[8][3];
  for (int i = 0; i < 8; i++)
    for (int k = 0; k < 3; k++)
      corners[i][k] = (i >> k) & 1 ? zone_max[k] : zone_min[k];
  float lo[3], hi[3];
  box_of(s, (const float(*)[3])corners, 8, lo, hi);
  for (int k = 0; k < 2; k++) {
    s->centre[k] = 0.5f * (lo[k] + hi[k]);
    s->width[k] = hi[k] - lo[k];
  }
  s->near = lo[2];
  s->far = hi[2] + 50.f;
  s->active = true;
  rows_of(s, (float)STATIC_SIZE);
  const bool redraw = !p->static_drawn || dot3(p->static_light, l) < 0.99999f;
  if (redraw) memcpy(p->static_light, l, sizeof l);

  // the cascades: the view's slices, along the camera axis most across the light
  const float fwd[3] = {cam->forward.x, cam->forward.y, cam->forward.z}, eye[3] = {cam->eye.x, cam->eye.y, cam->eye.z};
  float up[3] = {cam->up.x, cam->up.y, cam->up.z}, right[3];
  cross3(fwd, up, right);
  if (!normalize3(right)) return false;
  cross3(right, fwd, up);
  const float *pick = fabsf(dot3(right, l)) <= fabsf(dot3(up, l)) ? right : up;
  const float dp = dot3(pick, l);
  float cu[3] = {pick[0] - dp * l[0], pick[1] - dp * l[1], pick[2] - dp * l[2]};
  if (!normalize3(cu)) return false;
  const float ty = tanf(0.5f * cam->fov_y), aspect = cam->aspect > 0.f ? cam->aspect : 1.f, tx = ty * aspect;
  // splits the static map is as fine as: left out (UpdateViewDependantShadows)
  const float static_tpm = sqrtf((float)STATIC_SIZE / fmaxf(s->width[0], 1e-3f) * (float)STATIC_SIZE /
                                 fmaxf(s->width[1], 1e-3f));
  for (int i = 0; i < TM_PSSM_CASCADES; i++) {
    pssm_map *m = &p->cascades[i];
    const float a = split_far[i], b = i + 1 < TM_PSSM_CASCADES ? split_far[i + 1] : 0.f;
    const float near = fmaxf(0.f, b - 0.05f * (a - b));
    m->active = (float)CELL / (2.f * a) >= 1.75f * static_tpm;
    frame_from_u(m, l, cu);
    float pts[8][3];
    for (int c = 0; c < 8; c++) {
      const float d = CAM_NEAR + ((c & 4) ? a : near);
      const float sx = (c & 1) ? 1.f : -1.f, sy = (c & 2) ? 1.f : -1.f;
      for (int k = 0; k < 3; k++)
        pts[c][k] = eye[k] + fwd[k] * d + right[k] * sx * d * tx + up[k] * sy * d * ty;
    }
    box_of(m, (const float(*)[3])pts, 8, lo, hi);
    for (int k = 0; k < 2; k++) {
      m->centre[k] = 0.5f * (lo[k] + hi[k]);
      m->width[k] = hi[k] - lo[k];
    }
    m->near = s->near;
    m->far = hi[2] + 5.f;
    rows_of(m, (float)CELL);
    // the receivers: its rows, cell, and weight sat((a - z) / (0.05 (a - b)))
    memcpy(u->pssm_cascade[i], m->receiver, sizeof m->receiver);
    u->pssm_cell[i][0] = (float)(i % 3) / 3.f;
    u->pssm_cell[i][1] = (float)(i / 3) / 2.f;
    u->pssm_cell[i][2] = 1.f / 3.f;
    u->pssm_cell[i][3] = 1.f / 2.f;
    u->pssm_split[i][0] = CAM_NEAR + a;
    u->pssm_split[i][1] = 1.f / (0.05f * (a - b));
    u->pssm_split[i][2] = m->active ? 1.f : 0.f;
  }
  memcpy(u->pssm_static, s->receiver, sizeof s->receiver);
  u->pssm_view[0] = fwd[0], u->pssm_view[1] = fwd[1], u->pssm_view[2] = fwd[2];
  return redraw;
}

// whether a batch's sphere can cast into a map (its box across the light,
// anything toward the sun)
static bool reaches(const pssm_map *m, const float centre[3], float radius) {
  if (!m->active) return false;
  for (int k = 0; k < 2; k++)
    if (fabsf(dot3(m->axes[k], centre) - m->centre[k]) > 0.5f * m->width[k] + radius) return false;
  return dot3(m->axes[2], centre) - radius <= m->far;
}

bool tm_pssm_begin(ft_game *game, bool statics) {
  tm_pssm *p = game->pssm;
  if (!p) return false;
  tg_target **t = statics ? &p->static_map : &p->atlas;
  if (!*t) *t = statics ? tg_target_create_depth(game->gpu, STATIC_SIZE, STATIC_SIZE)
                        : tg_target_create_depth(game->gpu, 3u * CELL, 2u * CELL);
  if (!*t) return false;
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  tg_target_begin(game->gpu, *t, white);
  p->map = UINT32_MAX - 1u; // (no scissor set yet)
  if (statics) p->static_drawn = true;
  return true;
}

void tm_pssm_end(ft_game *game) { tg_target_end(game->gpu); }

bool tm_pssm_reaches(ft_game *game, int map, const float centre[3], float radius) {
  const tm_pssm *p = game->pssm;
  if (!p) return false;
  return reaches(map < 0 ? &p->statics : &p->cascades[map], centre, radius);
}

void tm_pssm_cast(ft_game *game, int map, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                  tg_texture picture, float alpha_test, bool two_sided, bool block) {
  tm_pssm *p = game->pssm;
  const pssm_map *m = map < 0 ? &p->statics : &p->cascades[map];
  pssm_uniforms u;
  memcpy(u.rows, m->caster, sizeof u.rows);
  if (map < 0) {
    u.cell[0] = u.cell[1] = 0.f, u.cell[2] = u.cell[3] = 1.f;
    u.params[1] = 1.f / (float)STATIC_SIZE;
  } else {
    u.cell[0] = (float)(map % 3) / 3.f, u.cell[1] = (float)(map / 3) / 2.f;
    u.cell[2] = 1.f / 3.f, u.cell[3] = 1.f / 2.f;
    u.params[1] = 1.f / (float)CELL;
  }
  u.params[0] = alpha_test;
  u.params[2] = u.params[3] = 0.f;
  // (the light's own depth: nearest kept, reversed)
  // (the slope's bias 0.75: reversed, toward the far end)
  const tg_state state = {.depth_test = true, .depth_write = true, .cull = two_sided ? TG_CULL_NONE : TG_CULL_NONE,
                          .slope_bias = -3};
  const tg_texture textures[1] = {picture};
  const tg_sampler samplers[1] = {TG_SAMPLER_WRAP};
  // the map's cell, its outer texels left clear (the border the receivers
  // clamp into): texels 1 .. size - 2
  if ((uint32_t)map != p->map) {
    p->map = (uint32_t)map;
    if (map < 0) tg_scissor(game->gpu, 1, 1, STATIC_SIZE - 2u, STATIC_SIZE - 2u);
    else tg_scissor(game->gpu, (int32_t)((uint32_t)map % 3u * CELL + 1u), (int32_t)((uint32_t)map / 3u * CELL + 1u),
                    CELL - 2u, CELL - 2u);
  }
  tg_program *program = alpha_test > 0.f ? (block ? p->caster_block : p->caster) : (block ? p->solid_block : p->solid);
  tg_draw(game->gpu, program, &state, mesh, first_index, index_count, textures, samplers, &u);
}

void tm_pssm_cast_part(ft_game *game, int map, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                       const float r[3][3], const float t[3]) {
  tm_pssm *p = game->pssm;
  pssm_map *m = map < 0 ? &p->statics : &p->cascades[map];
  // the map's rows times the part's place: its own vertices straight in
  const float saved[3][4] = {{m->caster[0][0], m->caster[0][1], m->caster[0][2], m->caster[0][3]},
                             {m->caster[1][0], m->caster[1][1], m->caster[1][2], m->caster[1][3]},
                             {m->caster[2][0], m->caster[2][1], m->caster[2][2], m->caster[2][3]}};
  for (int i = 0; i < 3; i++) {
    for (int k = 0; k < 3; k++)
      m->caster[i][k] = saved[i][0] * r[0][k] + saved[i][1] * r[1][k] + saved[i][2] * r[2][k];
    m->caster[i][3] = saved[i][3] + saved[i][0] * t[0] + saved[i][1] * t[1] + saved[i][2] * t[2];
  }
  tm_pssm_cast(game, map, mesh, first_index, index_count, 0, 0.f, false, false);
  memcpy(m->caster, saved, sizeof saved);
}

tg_texture tm_pssm_static_texture(const ft_game *game) {
  return game->pssm && game->pssm->static_map ? tg_target_texture(game->pssm->static_map) : 0;
}
tg_texture tm_pssm_atlas_texture(const ft_game *game) {
  return game->pssm && game->pssm->atlas ? tg_target_texture(game->pssm->atlas) : 0;
}

