// The ambient occlusion of the vertex-lit environments, as the game makes
// its light mask's alpha (env_light_spec.md §4; Shadows = Complex):
//
//   depth      the receivers' view depth into a picture of the view's size
//   ssao       per pixel, 16 points of a small sphere (3 % of the view's
//              half height at its depth) turned by the game's random
//              normals, each occluded as far as the scene is in front of it
//   blur       across then down, 15 taps (21 for some moods), the taps
//              across a depth step left out, then raised to the mood's power
//   receivers  their ambient light times it, MidGray at one half (track.frag)
//
// The game also draws the face normals first; here they come from the depth
// of the neighbouring pixels.

#include "tmuf_internal.h"

#include "aoblur_frag_spv.h"
#include "aodepth_frag_spv.h"
#include "aodepth_vert_spv.h"
#include "aoquad_vert_spv.h"
#include "ssao_frag_spv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define Z_MIN 0.2f     // the game's near and far, the depth picture's range
#define Z_MAX 50000.f
#define DEPTH_TEST_MAX_DIST 0.1f // BlurDepthTestMaxDist

typedef struct depth_uniforms {
  float view_proj[16];
  float eye[4], forward[4], range[4];
} depth_uniforms;

typedef struct ssao_uniforms {
  float proj[4], size[4];
} ssao_uniforms;

typedef struct blur_uniforms {
  float axis[4], weights[12], depth[4];
} blur_uniforms;

struct tm_ssao {
  tg_program *depth, *ssao, *blur;
  tg_target *depth_map, *ao, *temp;
  uint32_t width, height;
  tg_texture rand_normal;
  bool drawn; // this frame
};

// CPlugFileGen::GenRandNormal: MSVC's rand() from 0, a direction a texel
// (x from the third number, z from the first), packed as the game stores it
static tg_texture rand_normals(ft_game *game) {
  static uint8_t px[64 * 64 * 4];
  uint32_t state = 0;
  for (int i = 0; i < 64 * 64; i++) {
    float x, y, z, l2;
    do {
      float r[3];
      for (int k = 0; k < 3; k++) {
        state = state * 214013u + 2531011u;
        r[k] = (float)((state >> 16) & 0x7fffu) / 32767.f;
      }
      x = 2.f * r[2] - 1.f, y = 2.f * r[1] - 1.f, z = 2.f * r[0] - 1.f;
      l2 = x * x + y * y + z * z;
    } while (l2 <= 1e-10f);
    const float inv = 1.f / sqrtf(l2), n[3] = {x * inv, y * inv, z * inv};
    for (int k = 0; k < 3; k++) {
      const float v = floorf((n[k] + 1.f) * 127.f);
      px[4 * i + k] = (uint8_t)(v < 0.f ? 0.f : v > 255.f ? 255.f : v);
    }
    px[4 * i + 3] = 255;
  }
  const tg_image image = {TG_RGBA8, 64, 64, 1, 1, px, sizeof px};
  return tg_texture_create(game->gpu, &image);
}

bool tm_ssao_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  tm_ssao *s = calloc(1, sizeof *s);
  if (!s) return false;
  const tg_attr attrs[] = {{0, offsetof(tm_track_vertex, pos), TG_FLOAT3}, {1, offsetof(tm_track_vertex, uv), TG_FLOAT2}};
  const tg_program_desc depth = {.vertex_spirv = k_aodepth_vert_spv,
                                 .vertex_spirv_size = sizeof k_aodepth_vert_spv,
                                 .fragment_spirv = k_aodepth_frag_spv,
                                 .fragment_spirv_size = sizeof k_aodepth_frag_spv,
                                 .vertex_stride = sizeof(tm_track_vertex),
                                 .attrs = attrs,
                                 .attr_count = 2,
                                 .uniform_size = sizeof(depth_uniforms),
                                 .texture_count = 1};
  const tg_program_desc ssao = {.vertex_spirv = k_aoquad_vert_spv,
                                .vertex_spirv_size = sizeof k_aoquad_vert_spv,
                                .fragment_spirv = k_ssao_frag_spv,
                                .fragment_spirv_size = sizeof k_ssao_frag_spv,
                                .uniform_size = sizeof(ssao_uniforms),
                                .texture_count = 2};
  const tg_program_desc blur = {.vertex_spirv = k_aoquad_vert_spv,
                                .vertex_spirv_size = sizeof k_aoquad_vert_spv,
                                .fragment_spirv = k_aoblur_frag_spv,
                                .fragment_spirv_size = sizeof k_aoblur_frag_spv,
                                .uniform_size = sizeof(blur_uniforms),
                                .texture_count = 1};
  s->depth = tg_program_create(game->gpu, &depth);
  s->ssao = tg_program_create(game->gpu, &ssao);
  s->blur = tg_program_create(game->gpu, &blur);
  s->rand_normal = rand_normals(game);
  game->ssao = s;
  return s->depth && s->ssao && s->blur && s->rand_normal;
}

static void release_targets(ft_game *game, tm_ssao *s) {
  if (s->depth_map) tg_target_destroy(game->gpu, s->depth_map);
  if (s->ao) tg_target_destroy(game->gpu, s->ao);
  if (s->temp) tg_target_destroy(game->gpu, s->temp);
  s->depth_map = s->ao = s->temp = NULL;
  s->width = s->height = 0;
}

void tm_ssao_resources_destroy(ft_game *game) {
  tm_ssao *s = game->ssao;
  if (!s) return;
  release_targets(game, s);
  if (s->depth) tg_program_destroy(game->gpu, s->depth);
  if (s->ssao) tg_program_destroy(game->gpu, s->ssao);
  if (s->blur) tg_program_destroy(game->gpu, s->blur);
  if (s->rand_normal) tg_texture_destroy(game->gpu, s->rand_normal);
  free(s);
  game->ssao = NULL;
}

bool tm_ssao_begin(ft_game *game, const ft_camera *cam) {
  tm_ssao *s = game->ssao;
  if (!s) return false;
  s->drawn = false;
  const uint32_t w = cam->viewport.x < 1.f ? 1u : (uint32_t)cam->viewport.x;
  const uint32_t h = cam->viewport.y < 1.f ? 1u : (uint32_t)cam->viewport.y;
  if (w != s->width || h != s->height) {
    release_targets(game, s);
    s->depth_map = tg_target_create(game->gpu, w, h);
    s->ao = tg_target_create(game->gpu, w, h);
    s->temp = tg_target_create(game->gpu, w, h);
    if (!s->depth_map || !s->ao || !s->temp) {
      release_targets(game, s);
      return false;
    }
    s->width = w, s->height = h;
  }
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  tg_target_begin(game->gpu, s->depth_map, white);
  return true;
}

void tm_ssao_depth(ft_game *game, const ft_camera *cam, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                   tg_texture picture, float alpha_test, bool two_sided) {
  tm_ssao *s = game->ssao;
  depth_uniforms u;
  memset(&u, 0, sizeof u);
  memcpy(u.view_proj, cam->view_proj, sizeof u.view_proj);
  u.eye[0] = cam->eye.x, u.eye[1] = cam->eye.y, u.eye[2] = cam->eye.z;
  u.forward[0] = cam->forward.x, u.forward[1] = cam->forward.y, u.forward[2] = cam->forward.z;
  u.forward[3] = alpha_test;
  u.range[0] = Z_MIN;
  u.range[1] = 1.f / (Z_MAX - Z_MIN);
  const tg_state state = {.depth_test = true, .depth_write = true, .cull = two_sided ? TG_CULL_NONE : TG_CULL_CW};
  const tg_texture textures[1] = {picture};
  const tg_sampler samplers[1] = {TG_SAMPLER_WRAP};
  tg_draw(game->gpu, s->depth, &state, mesh, first_index, index_count, textures, samplers, &u);
}

// the gaussian of n taps (made odd), a side: exp(-(i - c)^2 ln 2 / c^2) normalised
static void weights_of(uint32_t n, float out[12], uint32_t *side) {
  if (!(n & 1u)) n++;
  if (n > 23u) n = 23u; // (the shader's 12 a side)
  const uint32_t h = (n + 1u) / 2u;
  const float c = (float)(n - 1u) / 2.f;
  float sum = 0.f;
  memset(out, 0, sizeof(float) * 12);
  for (uint32_t i = 0; i < h; i++) {
    out[i] = c > 0.f ? expf(-(float)(i * i) * logf(2.f) / (c * c)) : 1.f;
    sum += i ? 2.f * out[i] : out[i];
  }
  for (uint32_t i = 0; i < h; i++)
    out[i] /= sum;
  *side = h;
}

void tm_ssao_end(ft_game *game, const ft_camera *cam, const tmuf_ambient_occlusion *params) {
  tm_ssao *s = game->ssao;
  tg_target_end(game->gpu);
  // the occlusion
  const float ty = tanf(0.5f * cam->fov_y), aspect = cam->aspect > 0.f ? cam->aspect : 1.f;
  ssao_uniforms su = {{1.f / (ty * aspect), 1.f / ty, Z_MIN, Z_MAX - Z_MIN},
                      {(float)s->width, (float)s->height, params->radius * ty, 0.f}};
  const tg_state state = {.blend_src = 0};
  const tg_sampler point[2] = {{3, 3, 1, 0}, {1, 1, 1, 0}};
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  tm_prof(game, "ssao_ao");
  tg_target_begin(game->gpu, s->ao, white);
  const tg_texture ssao_textures[2] = {tg_target_texture(s->depth_map), s->rand_normal};
  tg_draw(game->gpu, s->ssao, &state, NULL, 0, 3, ssao_textures, point, &su);
  tg_target_end(game->gpu);
  // the blur, across then down (with the power)
  blur_uniforms bu;
  memset(&bu, 0, sizeof bu);
  uint32_t side = 0;
  weights_of(params->blur_texels, bu.weights, &side);
  bu.axis[3] = (float)side;
  bu.depth[0] = Z_MAX / DEPTH_TEST_MAX_DIST;
  const tg_sampler blur_samplers[1] = {{3, 3, 1, 0}};
  bu.axis[0] = 1.f, bu.axis[1] = 0.f, bu.axis[2] = 1.f;
  tm_prof(game, "ssao_blur");
  tg_target_begin(game->gpu, s->temp, white);
  const tg_texture h_textures[1] = {tg_target_texture(s->ao)};
  tg_draw(game->gpu, s->blur, &state, NULL, 0, 3, h_textures, blur_samplers, &bu);
  tg_target_end(game->gpu);
  bu.axis[0] = 0.f, bu.axis[1] = 1.f, bu.axis[2] = params->power;
  tg_target_begin(game->gpu, s->ao, white);
  const tg_texture v_textures[1] = {tg_target_texture(s->temp)};
  tg_draw(game->gpu, s->blur, &state, NULL, 0, 3, v_textures, blur_samplers, &bu);
  tg_target_end(game->gpu);
  s->drawn = true;
}

tg_texture tm_ssao_texture(const ft_game *game) {
  return game->ssao && game->ssao->drawn ? tg_target_texture(game->ssao->ao) : 0;
}
