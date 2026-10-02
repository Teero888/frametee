// The map's lightmap when no shipped cache has it (lightmap_bake_spec.md):
// the game computes it at load (CHmsPackLightMap::ComputeLighting), here on
// the first frame by day (the AD mode of Day and Sunrise):
//
//   samples   the sky: the 132 points of the game's sphere table, all of
//             them, each 4/132 of the light on the faces it reaches
//             (max(0, N.-d)); the sun: Day the 512-point table's points
//             within 40 degrees of it, Sunrise a 9 x 9 disk grid within 10,
//             each a share of its visibility alone
//   shadow    per sample the casters (the static lightmapped surfaces; the
//             sky's only the zone's, not the decoration) into
//             a float map along the light, orthographic over the zone's box
//             inside a one-texel border, back faces culled; the receivers
//             (the lightmapped surfaces, drawn in the atlas) lit where their
//             depth along the light, less 0.002051 x the box's larger half
//             extent across, is before the map's
//   result    r = sat(sky^0.8), g = sat(sun^1.25) (Sunrise 1.0) where any
//             sample covered the texel, then 8 gutter passes; mipmapped
//
// and over the first frames at Sunset and Night (RGB mode, §9: colours):
//
//   ambient   the mood's ambient cube by the normal, on the receivers
//             without a CubeAmbient layer of their own
//   light     the moon (Night: the 132 pack within 40 degrees) or the sun
//             (Sunset: a 5 x 5 grid within 4), N.-d where lit, as the sky's
//             samples; then its colour x sat(the sum)
//   spots     every spot with flag bit 0 reaching the zone's box: those
//             with an emitter (size) jittered over a 7 x 7 grid, else one
//             sample; each sample's perspective shadow map of the zone's
//             casters in its reach (from the light's own frame, or looking
//             at the lit volume when it is far), DoLightSpot where lit;
//             then its colour x sat(the sum)
//   result    each light's part added in 8 bits (the game's ONE/ONE blend
//             into its 8-bit target), then the gutter as by day
//
// The sums are 32-bit floats (the game's cards summed 16-bit unorms: its
// shipped caches are 3-4 % darker at the top). Single spots go through the
// same sum as the jittered ones (one sample: AddLightSpot's value).

#include "tmuf_internal.h"

#include "bakequad_vert_spv.h"
#include "bakecast_frag_spv.h"
#include "bakecast_vert_spv.h"
#include "bakefinish_frag_spv.h"
#include "bakerecv_frag_spv.h"
#include "bakerecv_vert_spv.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BAKE_SIZE 2048u
#define SKY_WANT 256u   // nAmb by day: the 132 pack
#define SUN_WANT 64u    // nDir by day
#define GUTTER_PASSES 8

typedef struct bake_uniforms {
  float x_axis[4], y_axis[4], z_axis[4], centre[4], extent[4], params[4];
  float persp[4];                           // perspective (a spot's): 1, the frustum's centre, its near depth
  float spot_pos[4], spot_dir[4], spot_rgb[4]; // DoLightSpot's constants
} bake_uniforms;

typedef struct finish_uniforms {
  float params[4];
} finish_uniforms;

struct bake_job;
struct tm_bake {
  tg_program *cast_track, *cast_block, *recv_track, *recv_block, *finish;
  struct bake_job *job;  // an RGB bake under way (tm_bake_continue)
  struct bake_job *done; // the one finished this frame, freed after it
};
static void job_free(ft_game *game, struct bake_job *j);

static tg_program *program(ft_game *game, const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
                           uint32_t stride, const tg_attr *attrs, uint32_t attr_count, uint32_t uniform_size,
                           uint32_t textures, uint32_t cube_mask) {
  const tg_program_desc d = {.cube_mask = cube_mask,
                             .vertex_spirv = vs,
                             .vertex_spirv_size = vs_size,
                             .fragment_spirv = fs,
                             .fragment_spirv_size = fs_size,
                             .vertex_stride = stride,
                             .attrs = attrs,
                             .attr_count = attr_count,
                             .uniform_size = uniform_size,
                             .texture_count = textures};
  return tg_program_create(game->gpu, &d);
}

bool tm_bake_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  tm_bake *b = calloc(1, sizeof *b);
  if (!b) return false;
  const tg_attr cast_t[] = {{0, offsetof(tm_track_vertex, pos), TG_FLOAT3}, {1, offsetof(tm_track_vertex, uv), TG_FLOAT2}};
  const tg_attr cast_b[] = {{0, offsetof(tm_block_vertex, pos), TG_FLOAT3}, {1, offsetof(tm_block_vertex, uv), TG_FLOAT2}};
  const tg_attr recv_t[] = {{0, offsetof(tm_track_vertex, pos), TG_FLOAT3},
                            {1, offsetof(tm_track_vertex, uv_prelight), TG_FLOAT2},
                            {2, offsetof(tm_track_vertex, normal), TG_UINT1}};
  const tg_attr recv_b[] = {{0, offsetof(tm_block_vertex, pos), TG_FLOAT3},
                            {1, offsetof(tm_block_vertex, uv_prelight), TG_FLOAT2},
                            {2, offsetof(tm_block_vertex, normal), TG_UINT1}};
  b->cast_track = program(game, k_bakecast_vert_spv, sizeof k_bakecast_vert_spv, k_bakecast_frag_spv,
                          sizeof k_bakecast_frag_spv, sizeof(tm_track_vertex), cast_t, 2, sizeof(bake_uniforms), 1, 0);
  b->cast_block = program(game, k_bakecast_vert_spv, sizeof k_bakecast_vert_spv, k_bakecast_frag_spv,
                          sizeof k_bakecast_frag_spv, sizeof(tm_block_vertex), cast_b, 2, sizeof(bake_uniforms), 1, 0);
  b->recv_track = program(game, k_bakerecv_vert_spv, sizeof k_bakerecv_vert_spv, k_bakerecv_frag_spv,
                          sizeof k_bakerecv_frag_spv, sizeof(tm_track_vertex), recv_t, 3, sizeof(bake_uniforms), 3, 1u << 2);
  b->recv_block = program(game, k_bakerecv_vert_spv, sizeof k_bakerecv_vert_spv, k_bakerecv_frag_spv,
                          sizeof k_bakerecv_frag_spv, sizeof(tm_block_vertex), recv_b, 3, sizeof(bake_uniforms), 3, 1u << 2);
  b->finish = program(game, k_bakequad_vert_spv, sizeof k_bakequad_vert_spv, k_bakefinish_frag_spv,
                      sizeof k_bakefinish_frag_spv, 0, NULL, 0, sizeof(finish_uniforms), 2, 0);
  game->bake = b;
  return b->cast_track && b->cast_block && b->recv_track && b->recv_block && b->finish;
}

void tm_bake_resources_destroy(ft_game *game) {
  tm_bake *b = game->bake;
  if (!b) return;
  tm_bake_cancel(game);
  if (b->done) job_free(game, b->done);
  tg_program *ps[5] = {b->cast_track, b->cast_block, b->recv_track, b->recv_block, b->finish};
  for (int i = 0; i < 5; i++)
    if (ps[i]) tg_program_destroy(game->gpu, ps[i]);
  free(b);
  game->bake = NULL;
}

// --- the light samples (ComputeSpherePoints_Amb / _Dir) --------------------------------

static void normalize3(float v[3]) {
  const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 0.f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// the sun's samples: the sphere table's points within its angle, else the
// disk grid (lightmap_bake_spec.md §3.2, §3.3); out holds up to cap
static uint32_t sun_samples(const tmuf_track *track, const float sun[3], float angle_deg, uint32_t n_dir, float *out,
                            uint32_t cap) {
  const float c = cosf(angle_deg * 3.14159265f / 180.f), capfrac = (1.f - c) * 0.5f;
  const float want = (float)n_dir / capfrac;
  uint32_t n = 0;
  if (want < 4096.f && angle_deg > 19.5f) {
    const float *pts = NULL;
    const uint32_t count = tmuf_track_sphere_points(track, (uint32_t)want, &pts);
    if (pts && (float)count * capfrac > 4.001f)
      for (uint32_t i = 0; i < count && n < cap; i++) {
        const float *p = pts + 3 * i;
        if ((p[1] * sun[1] + p[0] * sun[0]) + p[2] * sun[2] >= c) memcpy(out + 3 * n++, p, 12);
      }
  }
  if (n) return n;
  // the grid: R = (normalize(up x d), d x that, d)
  const float s = sinf(angle_deg * 3.14159265f / 180.f);
  const uint32_t k = (uint32_t)((double)n_dir * 4.0 / 3.14159265358979);
  const float r = sqrtf((float)k), step = 2.f / r;
  const int m = (int)rint((double)r + 0.5);
  int g = ((m + 1) / 2) * 2 + 3;
  while (g >= 3 && (float)(g - 1) * (step * 0.5f) > 1.f)
    g -= 2;
  const int h = (g - 1) >> 1;
  float rx[3] = {sun[2], 0.f, -sun[0]}; // up x d
  normalize3(rx);
  const float ry[3] = {sun[1] * rx[2] - sun[2] * rx[1], sun[2] * rx[0] - sun[0] * rx[2], sun[0] * rx[1] - sun[1] * rx[0]};
  for (int i = 0; i < g; i++)
    for (int j = -h; j < -h + g; j++) {
      const float x = (float)j * step, y = (float)(i - h) * step, r2 = x * x + y * y;
      if (r2 > 1.f || n == cap) continue;
      const float l[3] = {x * s, y * s, sqrtf(1.f - r2 * s * s)};
      for (int a = 0; a < 3; a++)
        out[3 * n + a] = rx[a] * l[0] + ry[a] * l[1] + sun[a] * l[2];
      n++;
    }
  return n;
}

// --- the passes ----------------------------------------------------------------------

// the light's basis (GmMat3::SetDOV) and the box's extents along it
static void basis(const float d[3], const float c[3], const float h[3], uint32_t size, bake_uniforms *u) {
  float x[3] = {-d[2], 0.f, d[0]}; // d x (0, 1, 0)
  if (x[0] * x[0] + x[2] * x[2] < 1e-12f) x[0] = 1.f, x[2] = 0.f;
  normalize3(x);
  const float y[3] = {x[1] * d[2] - x[2] * d[1], x[2] * d[0] - x[0] * d[2], x[0] * d[1] - x[1] * d[0]}; // x x d
  const float ex = fabsf(x[0]) * h[0] + fabsf(x[1]) * h[1] + fabsf(x[2]) * h[2];
  const float ey = fabsf(y[0]) * h[0] + fabsf(y[1]) * h[1] + fabsf(y[2]) * h[2];
  const float ez = fabsf(d[0]) * h[0] + fabsf(d[1]) * h[1] + fabsf(d[2]) * h[2];
  memset(u, 0, sizeof *u);
  memcpy(u->x_axis, x, 12), memcpy(u->y_axis, y, 12), memcpy(u->z_axis, d, 12), memcpy(u->centre, c, 12);
  u->extent[0] = ex > 1e-3f ? ex : 1e-3f;
  u->extent[1] = ey > 1e-3f ? ey : 1e-3f;
  u->extent[2] = (ez > 1e-3f ? ez : 1e-3f) * 1.05f;
  u->extent[3] = (float)(size - 2u) / (float)size;
  // Shadow_ComputeDepthRangeAndBias: 0.3 x 7 x 4 x the larger half extent / (W + H)
  u->params[1] = 0.3f * 7.f * 4.f * (ex > ey ? ex : ey) / (float)(2u * size);
}

static bool reaches(const tm_bake_draw *d, const float *sphere) {
  if (!sphere) return true;
  const float dx = d->center[0] - sphere[0], dy = d->center[1] - sphere[1], dz = d->center[2] - sphere[2];
  const float r = d->radius + sphere[3];
  return dx * dx + dy * dy + dz * dz < r * r;
}

// one sample: the casters (all, or the zone's) into the shadow map, then the
// receivers into `to`, reading the sums so far in `from`; with a sphere (a
// spot's ball) only the draws that reach it. The draws made.
static uint32_t sample(ft_game *game, const tm_bake_draw *draws, uint32_t n, const bake_uniforms *base,
                       tg_target *shadow, tg_target *from, tg_target *to, float weight, float mode, bool zone_only,
                       const float *sphere) {
  tm_bake *b = game->bake;
  bake_uniforms u = *base;
  uint32_t made = 0;
  static const float far[4] = {1e30f, 0.f, 0.f, 1.f};
  // the shadow map (CW culled, as the casters' normal draws; nearest wins);
  // the game's CW: its view is ours mirrored top to bottom
  tg_target_begin(game->gpu, shadow, far);
  const tg_state cast = {.depth_test = true, .depth_write = true, .cull = TG_CULL_CCW};
  const tg_sampler wrap = TG_SAMPLER_WRAP;
  for (uint32_t i = 0; i < n; i++) {
    const tm_bake_draw *d = &draws[i];
    if (!d->caster || !d->count || (zone_only && !d->sky_caster) || !reaches(d, sphere)) continue;
    u.params[0] = d->cutoff;
    tg_draw(game->gpu, d->block ? b->cast_block : b->cast_track, &cast, d->mesh, d->first, d->count, &d->picture, &wrap,
            &u);
    made++;
  }
  tg_target_end(game->gpu);
  // the receivers: the sums so far plus this sample
  static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  tg_target_begin(game->gpu, to, zero);
  const tg_state recv = {.cull = TG_CULL_NONE};
  const tg_texture textures[3] = {tg_target_texture(shadow), tg_target_texture(from), 0};
  const tg_sampler point[3] = {{3, 3, 1, 0}, {3, 3, 1, 0}, TG_SAMPLER_WRAP};
  u.params[0] = 0.f;
  u.params[2] = weight;
  u.params[3] = mode;
  for (uint32_t i = 0; i < n; i++) {
    const tm_bake_draw *d = &draws[i];
    if (!d->receiver || !d->count || !reaches(d, sphere)) continue;
    tg_draw(game->gpu, d->block ? b->recv_block : b->recv_track, &recv, d->mesh, d->first, d->count, textures, point,
            &u);
    made++;
  }
  tg_target_end(game->gpu);
  return made;
}

// a full-screen pass of the finishing shader into dst
static void finish(ft_game *game, tg_target *dst, tg_target *source, tg_target *prev, const float params[4]) {
  static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  const tg_state plain = {.cull = TG_CULL_NONE};
  const tg_sampler point[2] = {{3, 3, 1, 0}, {3, 3, 1, 0}};
  finish_uniforms f;
  memcpy(f.params, params, sizeof f.params);
  tg_target_begin(game->gpu, dst, zero);
  const tg_texture src[2] = {tg_target_texture(source), prev ? tg_target_texture(prev) : 0};
  tg_draw(game->gpu, game->bake->finish, &plain, NULL, 0, 3, src, point, &f);
  tg_target_end(game->gpu);
}

static void clear(ft_game *game, tg_target *t) {
  static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  tg_target_begin(game->gpu, t, zero);
  tg_target_end(game->gpu);
}

// the 8 gutter passes from pre[0], the last into the mipmapped lightmap
static void gutter(ft_game *game, tg_target *pre[2], tg_target *out) {
  const float params[4] = {1.f, 0.f, 0.f, 0.f};
  for (int pass = 0; pass < GUTTER_PASSES; pass++)
    finish(game, pass == GUTTER_PASSES - 1 ? out : pre[(pass + 1) & 1], pre[pass & 1], NULL, params);
}

// --- RGB mode: the spots (lightmap_bake_spec.md §9.5, §9.6) ----------------------------

#define SPOT_GRID 7          // Compute2dPoints_Spot: (rint(sqrt(25) + 0.5) / 2) * 2 + 1
#define BIAS_FACTOR 0.0020508f // 0.3 x 7 x 4 / (W + H)
#define DRAWS_PER_FRAME 250000u

// GmBoxAligned::SetFromConeAndRadius: the box of a cone of half angle acos(c)
static void cone_box(const float p[3], const float d[3], float c, float r, float mn[3], float mx[3]) {
  const float s = sqrtf(fmaxf(0.f, 1.f - c * c));
  for (int k = 0; k < 3; k++) {
    const float dk = d[k], sk = sqrtf(fmaxf(0.f, 1.f - dk * dk));
    mx[k] = p[k] + (dk > c ? r : r * fmaxf(0.f, dk * c + sk * s));
    mn[k] = p[k] - (-dk > c ? r : r * fmaxf(0.f, -dk * c + sk * s));
  }
}

// ShadowLightSpot / Shadow_ComputeFrustumLocation for a sample at p: the
// camera (cone mode from the light's own frame, box mode looking at the lit
// volume L = the casters' box within the cone's) and its bias; false
// without a shadow (lit everywhere)
static bool spot_camera(const tmuf_light *l, const float p[3], const float tmin[3], const float tmax[3], uint32_t size,
                        bake_uniforms *u) {
  const float *d = l->direction, c = l->cos_outer, r0 = l->radius[0];
  float cmin[3], cmax[3], lc[3], lh[3];
  cone_box(p, d, c, r0, cmin, cmax);
  for (int k = 0; k < 3; k++) {
    const float lo = fmaxf(tmin[k], cmin[k]), hi = fminf(tmax[k], cmax[k]);
    if (hi < lo) return false;
    lc[k] = 0.5f * (lo + hi), lh[k] = 0.5f * (hi - lo);
  }
  const float R = sqrtf(lh[0] * lh[0] + lh[1] * lh[1] + lh[2] * lh[2]);
  const float to[3] = {lc[0] - p[0], lc[1] - p[1], lc[2] - p[2]};
  const float x = R > 0.f ? (sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]) - R) / R : 0.f;
  float ax[3], ay[3], az[3];
  bool cone = x <= 0.05f;
  if (cone) {
    if (c <= 0.499f) return false;
    // (-X, Y, d) of the light's location
    for (int k = 0; k < 3; k++)
      ax[k] = -l->location.r.m[k][0], ay[k] = l->location.r.m[k][1], az[k] = d[k];
  } else {
    // GmIso4::SetLookAt(p, centre(L))
    memcpy(az, to, sizeof az);
    normalize3(az);
    ax[0] = -az[2], ax[1] = 0.f, ax[2] = az[0];
    if (ax[0] * ax[0] + ax[2] * ax[2] < 1e-12f) ax[0] = 1.f, ax[2] = 0.f;
    normalize3(ax);
    ay[0] = ax[1] * az[2] - ax[2] * az[1], ay[1] = ax[2] * az[0] - ax[0] * az[2], ay[2] = ax[0] * az[1] - ax[1] * az[0];
  }
  // L's corners in the camera's frame
  float zmin = FLT_MAX, zmax = -FLT_MAX, xmin = FLT_MAX, xmax = -FLT_MAX, ymin = FLT_MAX, ymax = -FLT_MAX;
  for (int i = 0; i < 8; i++) {
    const float q[3] = {lc[0] + ((i & 1) ? lh[0] : -lh[0]) - p[0], lc[1] + ((i & 2) ? lh[1] : -lh[1]) - p[1],
                        lc[2] + ((i & 4) ? lh[2] : -lh[2]) - p[2]};
    const float qx = ax[0] * q[0] + ax[1] * q[1] + ax[2] * q[2], qy = ay[0] * q[0] + ay[1] * q[1] + ay[2] * q[2];
    const float qz = az[0] * q[0] + az[1] * q[1] + az[2] * q[2];
    zmin = fminf(zmin, qz), zmax = fmaxf(zmax, qz);
    if (qz > 1e-6f) xmin = fminf(xmin, qx / qz), xmax = fmaxf(xmax, qx / qz), ymin = fminf(ymin, qy / qz), ymax = fmaxf(ymax, qy / qz);
  }
  const float far_z = fmaxf(zmax, 1e-5f), near_z = fmaxf(zmin, 1e-4f * far_z);
  if (!(far_z > near_z)) return false;
  memset(u, 0, sizeof *u);
  memcpy(u->x_axis, ax, 12), memcpy(u->y_axis, ay, 12), memcpy(u->z_axis, az, 12), memcpy(u->centre, p, 12);
  u->extent[2] = far_z;
  u->extent[3] = (float)(size - 2u) / (float)size;
  u->persp[0] = 1.f;
  u->persp[3] = near_z;
  float t;
  if (cone) {
    t = sqrtf(fmaxf(0.f, 1.f - c * c)) / c; // tan(acos(cos_outer))
    u->extent[0] = u->extent[1] = t;
  } else {
    if (!(xmax > xmin) || !(ymax > ymin)) return false;
    u->extent[0] = 0.5f * (xmax - xmin), u->extent[1] = 0.5f * (ymax - ymin);
    u->persp[1] = 0.5f * (xmax + xmin), u->persp[2] = 0.5f * (ymax + ymin);
    t = fmaxf(u->extent[0], u->extent[1]);
  }
  u->params[1] = BIAS_FACTOR * t; // a factor of the depth: Shadow_ComputeDepthRangeAndBias
  return true;
}

// the spot's light at a sample (DoLightSpot's constants; colour 1 while summing)
static void spot_light(const tmuf_light *l, const float p[3], bake_uniforms *u) {
  const float range = l->cos_inner - l->cos_outer;
  memcpy(u->spot_pos, p, 12);
  u->spot_pos[3] = range > 1e-6f ? 1.f / range : 1e6f;
  memcpy(u->spot_dir, l->direction, 12);
  u->spot_dir[3] = l->cos_outer;
  u->spot_rgb[0] = u->spot_rgb[1] = u->spot_rgb[2] = 1.f;
  u->spot_rgb[3] = l->radius[0] > 0.f ? 1.f / (l->radius[0] * l->radius[0]) : 0.f;
}

// AddInteractLights: a ball or spot with flag bit 0 whose cube (position +-
// radius[0]) meets the zone's box; the balls none on Stadium (§9.7)
static bool baked(const tmuf_light *l, const float bmin[3], const float bmax[3]) {
  if (l->kind != TMUF_LIGHT_SPOT || !(l->flags & TMUF_LIGHT_FLAG_DIFFUSE) || !(l->radius[0] > 0.f)) return false;
  for (int k = 0; k < 3; k++)
    if (l->position[k] + l->radius[0] < bmin[k] || l->position[k] - l->radius[0] > bmax[k]) return false;
  return true;
}

typedef struct bake_job {
  tm_bake_input in;
  tm_bake_draw *draws, *pieces; // the job's own copies
  tg_target *shadow, *acc[2], *lm[2], *pre[2], *out;
  uint32_t acc_cur, lm_cur;
  float dirs[3 * 128];
  uint32_t dir_count;
  const tmuf_light *lights;
  uint32_t light_count;
  uint32_t phase, step, light; // where it is: 0 the start, 1 the directional light, 2 the spots, 3 done
  uint32_t spots, multi, maps;
} bake_job;

static void job_free(ft_game *game, bake_job *j) {
  tg_target *all[8] = {j->shadow, j->acc[0], j->acc[1], j->lm[0], j->lm[1], j->pre[0], j->pre[1], j->out};
  for (int i = 0; i < 8; i++)
    if (all[i]) tg_target_destroy(game->gpu, all[i]);
  free(j->draws), free(j->pieces), free(j);
}

// AccumNorm in RGB mode: the light's colour x sat(its sum) added to the
// lightmap's sum, where its samples covered; the sums cleared for the next
static void rgb_norm(ft_game *game, bake_job *j, const float rgb[3]) {
  const float params[4] = {3.f, rgb[0], rgb[1], rgb[2]};
  finish(game, j->lm[j->lm_cur ^ 1u], j->acc[j->acc_cur], j->lm[j->lm_cur], params);
  j->lm_cur ^= 1u;
  clear(game, j->acc[1]);
  j->acc_cur = 1;
}

static void rgb_start(ft_game *game, bake_job *j) {
  tm_bake *b = game->bake;
  clear(game, j->lm[1]);
  clear(game, j->acc[1]);
  j->lm_cur = j->acc_cur = 1;
  // SetAmbient: the cube by the normal, on the receivers without their own
  static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  tg_target_begin(game->gpu, j->lm[0], zero);
  const tg_state recv = {.cull = TG_CULL_NONE};
  const tg_texture textures[3] = {tg_target_texture(j->shadow), tg_target_texture(j->lm[1]), j->in.ambient_cube};
  const tg_sampler samplers[3] = {{3, 3, 1, 0}, {3, 3, 1, 0}, {3, 3, 3, 2}};
  bake_uniforms u;
  memset(&u, 0, sizeof u);
  u.params[3] = 6.f;
  for (uint32_t i = 0; i < j->in.draw_count; i++) {
    const tm_bake_draw *d = &j->draws[i];
    if (!d->receiver || !d->count) continue;
    u.params[0] = d->ambient && j->in.ambient_cube ? 1.f : 0.f;
    tg_draw(game->gpu, d->block ? b->recv_block : b->recv_track, &recv, d->mesh, d->first, d->count, textures, samplers,
            &u);
  }
  tg_target_end(game->gpu);
  j->lm_cur = 0;
}

// as many samples as the frame's budget allows; true when done
static bool rgb_continue(ft_game *game, bake_job *j) {
  const uint32_t size = BAKE_SIZE;
  float c[3], h[3];
  for (int k = 0; k < 3; k++)
    c[k] = 0.5f * (j->in.box_min[k] + j->in.box_max[k]), h[k] = 0.5f * (j->in.box_max[k] - j->in.box_min[k]);
  uint32_t made = 0;
  bake_uniforms u;
  while (made < DRAWS_PER_FRAME && j->phase < 3) {
    if (j->phase == 0) {
      rgb_start(game, j);
      j->phase = 1, j->step = 0;
    } else if (j->phase == 1) {
      // the directional light: its samples' N.L where lit (the zone's and
      // the decoration's shadows, as the sun's by day), then its colour
      if (j->step < j->dir_count) {
        basis(j->dirs + 3 * j->step, c, h, size, &u);
        made += sample(game, j->draws, j->in.draw_count, &u, j->shadow, j->acc[j->acc_cur], j->acc[j->acc_cur ^ 1u],
                       1.f / (float)j->dir_count, 1.f, false, NULL);
        j->acc_cur ^= 1u;
        j->maps++;
        j->step++;
      } else {
        rgb_norm(game, j, j->in.dir_rgb);
        j->phase = 2, j->step = 0, j->light = 0;
      }
    } else {
      // the spots, one after another: 49 jittered samples for those with
      // an emitter (size), else one, each with its perspective shadow map
      // of the zone's casters within its reach
      while (j->light < j->light_count && !baked(&j->lights[j->light], j->in.box_min, j->in.box_max))
        j->light++;
      if (j->light == j->light_count) {
        j->phase = 3;
        break;
      }
      const tmuf_light *l = &j->lights[j->light];
      const bool multi = l->size > 0.01f;
      const uint32_t n = multi ? SPOT_GRID * SPOT_GRID : 1u;
      if (j->step < n) {
        float p[3];
        memcpy(p, l->position, sizeof p);
        if (multi) {
          const float st = 2.f / (float)(SPOT_GRID - 1) * l->size;
          const float fi = (float)(j->step % SPOT_GRID) - 3.f, fj = (float)(j->step / SPOT_GRID) - 3.f;
          for (int k = 0; k < 3; k++)
            p[k] += st * fi * l->location.r.m[k][0] + st * fj * l->location.r.m[k][1];
        }
        if (!spot_camera(l, p, j->in.box_min, j->in.box_max, size, &u)) {
          memset(&u, 0, sizeof u);
          u.persp[0] = 1.f, u.persp[3] = 1e30f; // no shadow: every receiver before the near plane
          u.extent[0] = u.extent[1] = u.extent[2] = u.extent[3] = 1.f;
        }
        spot_light(l, p, &u);
        // the reach: the light's ball around its own position (the same
        // receivers for every sample)
        const float sphere[4] = {l->position[0], l->position[1], l->position[2], l->radius[0] + 1.5f * l->size};
        made += sample(game, j->pieces, j->in.piece_count, &u, j->shadow, j->acc[j->acc_cur], j->acc[j->acc_cur ^ 1u],
                       1.f / (float)n, 4.f, true, sphere);
        j->acc_cur ^= 1u;
        j->maps++;
        j->step++;
      } else {
        rgb_norm(game, j, l->diffuse_rgb);
        j->spots++;
        j->multi += multi ? 1u : 0u;
        j->light++, j->step = 0;
      }
    }
  }
  if (j->phase < 3) return false;
  // the sum into the 8-bit lightmap, then the gutter
  const float copy[4] = {4.f, 0.f, 0.f, 0.f};
  finish(game, j->pre[0], j->lm[j->lm_cur], NULL, copy);
  gutter(game, j->pre, j->out);
  tm_log(game, FT_LOG_INFO, "Lightmap baked (colours): the ambient cube, %u directional and %u spot samples (%u spots, %u of them jittered)",
         j->dir_count, j->maps - j->dir_count, j->spots, j->multi);
  return true;
}

// --- the bake ---------------------------------------------------------------------------

tg_target *tm_bake_lightmap(ft_game *game, const tm_bake_input *in) {
  tm_bake *b = game->bake;
  if (!b || !in->draw_count) return NULL;
  const uint32_t size = BAKE_SIZE;
  if (in->rgb) {
    tm_bake_cancel(game);
    bake_job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->in = *in;
    j->draws = malloc(sizeof *j->draws * in->draw_count);
    j->pieces = malloc(sizeof *j->pieces * (in->piece_count ? in->piece_count : 1));
    if (j->draws) memcpy(j->draws, in->draws, sizeof *j->draws * in->draw_count);
    if (j->pieces && in->piece_count) memcpy(j->pieces, in->pieces, sizeof *j->pieces * in->piece_count);
    j->in.draws = j->draws, j->in.pieces = j->pieces;
    // the directional light's samples (nDir 25: Night the 132 pack within
    // 40 degrees, Sunset a 5 x 5 grid within 4)
    j->dir_count = sun_samples(in->track, in->sun_dir, in->dir_angle, 25u, j->dirs, sizeof j->dirs / sizeof j->dirs[0] / 3);
    j->light_count = tmuf_track_lights(in->track, &j->lights);
    j->shadow = tg_target_create_float(game->gpu, size, size);
    for (int k = 0; k < 2; k++) {
      j->acc[k] = tg_target_create_float(game->gpu, size, size);
      j->lm[k] = tg_target_create_float(game->gpu, size, size);
      j->pre[k] = tg_target_create(game->gpu, size, size);
    }
    j->out = tg_target_create_mips(game->gpu, size, size);
    if (!j->draws || !j->pieces || !j->dir_count || !j->shadow || !j->acc[0] || !j->acc[1] || !j->lm[0] || !j->lm[1] ||
        !j->pre[0] || !j->pre[1] || !j->out) {
      job_free(game, j);
      return NULL;
    }
    b->job = j;
    return NULL;
  }
  // the samples
  const float *sky = NULL;
  const uint32_t sky_count = tmuf_track_sphere_points(in->track, SKY_WANT, &sky);
  float sun[3 * 128];
  const uint32_t sun_count =
      sun_samples(in->track, in->sun_dir, in->sunrise ? 10.f : 40.f, SUN_WANT, sun, sizeof sun / sizeof sun[0] / 3);
  if (!sky_count || !sun_count) return NULL;
  tg_target *shadow = tg_target_create_float(game->gpu, size, size);
  tg_target *acc[2] = {tg_target_create_float(game->gpu, size, size), tg_target_create_float(game->gpu, size, size)};
  tg_target *pre[2] = {tg_target_create(game->gpu, size, size), tg_target_create(game->gpu, size, size)};
  tg_target *out = tg_target_create_mips(game->gpu, size, size);
  if (!shadow || !acc[0] || !acc[1] || !pre[0] || !pre[1] || !out) {
    tg_target *all[6] = {shadow, acc[0], acc[1], pre[0], pre[1], out};
    for (int i = 0; i < 6; i++)
      if (all[i]) tg_target_destroy(game->gpu, all[i]);
    return NULL;
  }
  // the zone's box
  float c[3], h[3];
  for (int k = 0; k < 3; k++)
    c[k] = 0.5f * (in->box_min[k] + in->box_max[k]), h[k] = 0.5f * (in->box_max[k] - in->box_min[k]);
  // the sums start at zero: the first sample reads acc[1] cleared
  clear(game, acc[1]);
  uint32_t cur = 1; // the sums so far
  bake_uniforms u;
  // the sky's shadows only the zone's casters (§4.1b), the sun's all of them
  for (uint32_t i = 0; i < sky_count; i++) {
    float d[3] = {sky[3 * i], sky[3 * i + 1], sky[3 * i + 2]};
    basis(d, c, h, size, &u);
    sample(game, in->draws, in->draw_count, &u, shadow, acc[cur], acc[cur ^ 1u], 4.f / (float)sky_count, 1.f, true,
           NULL);
    cur ^= 1u;
  }
  for (uint32_t i = 0; i < sun_count; i++) {
    basis(sun + 3 * i, c, h, size, &u);
    sample(game, in->draws, in->draw_count, &u, shadow, acc[cur], acc[cur ^ 1u], 1.f / (float)sun_count, 2.f, false,
           NULL);
    cur ^= 1u;
  }
  // AccumNorm, then the gutter (the last pass into the mipmapped lightmap)
  const float norm[4] = {0.f, 0.8f, in->sunrise ? 1.f : 1.25f, 0.f};
  finish(game, pre[0], acc[cur], NULL, norm);
  gutter(game, pre, out);
  tm_log(game, FT_LOG_INFO, "Lightmap baked: %u sky and %u sun samples, %u casting and receiving draws", sky_count,
         sun_count, in->draw_count);
  // the work targets go once the frame is done with them (tg_target_destroy waits)
  game->bake_leftover[0] = shadow, game->bake_leftover[1] = acc[0], game->bake_leftover[2] = acc[1];
  game->bake_leftover[3] = pre[0], game->bake_leftover[4] = pre[1];
  return out;
}

tg_target *tm_bake_continue(ft_game *game) {
  tm_bake *b = game->bake;
  if (!b || !b->job) return NULL;
  bake_job *j = b->job;
  if (!rgb_continue(game, j)) return NULL;
  tg_target *out = j->out;
  j->out = NULL;
  b->job = NULL;
  b->done = j; // freed after the frame (its targets are in it)
  return out;
}

bool tm_bake_running(const ft_game *game) { return game->bake && game->bake->job; }

void tm_bake_cancel(ft_game *game) {
  tm_bake *b = game->bake;
  if (!b || !b->job) return;
  job_free(game, b->job);
  b->job = NULL;
}

void tm_bake_after_frame(ft_game *game) {
  for (int i = 0; i < 5; i++)
    if (game->bake_leftover[i]) {
      tg_target_destroy(game->gpu, game->bake_leftover[i]);
      game->bake_leftover[i] = NULL;
    }
  if (game->bake && game->bake->done) {
    job_free(game, game->bake->done);
    game->bake->done = NULL;
  }
}
