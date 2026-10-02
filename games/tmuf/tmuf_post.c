// The game's post effects over the finished frame (PostFxEnable): the bloom
// (CSceneFxBloom), as the game draws it at its maximum settings:
//
//   high      the frame's bright parts, full size: copy * pow(mean + 0.001, HighExponent)
//   quarter   a quarter-size copy of those, four bilinear taps a texel apart
//   blur      across then down, four taps each (Bloom_Glow)
//   add       frame + bloom * sat(1 - sqrt(mean(frame))): the dark parts get the most
//
// Tap offsets are the game's shader constants, which are Direct3D 9's
// (a texture coordinate of i / size at pixel i): here they are taken back to
// Vulkan's pixel centres, (i + 0.5) / size.

#include "tmuf_internal.h"

#include "post_vert_spv.h"
#include "post_frag_spv.h"

#include <string.h>

typedef struct post_uniforms {
  float params[4];  // mode, HighExponent, HdrExp, HdrRemapS
  float params2[4]; // HdrRemapT
  float offsets[4][4];
  float weights[4];
} post_uniforms;

enum { MODE_HIGH, MODE_TAPS, MODE_ADD };

// the game's constants (A01's trace; the same for every mood seen so far)
#define HIGH_EXPONENT 3.02115f
#define HDR_EXP 0.5f
#define HDR_REMAP_S 1.f
#define HDR_REMAP_T 0.f
// the blur's taps in texels of the quarter-size picture, and their weights
static const float blur_taps[4] = {-1.904896f - 0.5f, 0.019245f - 0.5f, 1.94249f - 0.5f, 0.f};
static const float blur_weights[4] = {0.232053f, 0.361906f, 0.312082f, 0.0939588f};

bool tm_post_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  const tg_program_desc desc = {.vertex_spirv = k_post_vert_spv,
                                .vertex_spirv_size = sizeof k_post_vert_spv,
                                .fragment_spirv = k_post_frag_spv,
                                .fragment_spirv_size = sizeof k_post_frag_spv,
                                .uniform_size = sizeof(post_uniforms),
                                .texture_count = 2};
  game->post_program = tg_program_create(game->gpu, &desc);
  return game->post_program != NULL;
}

static void targets_release(ft_game *game) {
  for (int i = 0; i < 3; i++) {
    tg_target_destroy(game->gpu, game->bloom[i]);
    game->bloom[i] = NULL;
  }
}

void tm_post_resources_destroy(ft_game *game) {
  targets_release(game);
  if (game->post_program) tg_program_destroy(game->gpu, game->post_program);
  game->post_program = NULL;
}

static bool targets_prepare(ft_game *game, uint32_t width, uint32_t height) {
  const uint32_t qw = width / 4 ? width / 4 : 1, qh = height / 4 ? height / 4 : 1;
  if (game->bloom[0] && tg_target_width(game->bloom[0]) == width && tg_target_height(game->bloom[0]) == height)
    return true;
  targets_release(game);
  game->bloom[0] = tg_target_create(game->gpu, width, height);
  game->bloom[1] = tg_target_create(game->gpu, qw, qh);
  game->bloom[2] = tg_target_create(game->gpu, qw, qh);
  return game->bloom[0] && game->bloom[1] && game->bloom[2];
}

static void pass(ft_game *game, tg_target *into, tg_texture src, tg_texture bloom, const post_uniforms *u) {
  static const float clear[4] = {0.f, 0.f, 0.f, 0.f};
  static const tg_state state = {.cull = TG_CULL_NONE};
  const tg_texture textures[2] = {src, bloom};
  const tg_sampler samplers[2] = {{3, 3, 2, 0}, {3, 3, 2, 0}};
  if (into) tg_target_begin(game->gpu, into, clear);
  tg_draw(game->gpu, game->post_program, &state, NULL, 0, 3, textures, samplers, u);
  if (into) tg_target_end(game->gpu);
}

void tm_post_taps(ft_game *game, tg_target *into, tg_texture src, const float offsets[4][2], const float weights[4]) {
  if (!game->post_program) return;
  post_uniforms u = {.params = {MODE_TAPS}};
  for (int i = 0; i < 4; i++) {
    u.offsets[i][0] = offsets[i][0], u.offsets[i][1] = offsets[i][1];
    u.weights[i] = weights[i];
  }
  pass(game, into, src, 0, &u);
}

void tm_post_render(ft_game *game, uint32_t width, uint32_t height) {
  if (!game->post_program || !game->settings.bloom || !width || !height) return;
  if (!targets_prepare(game, width, height)) return;
  tg *g = game->gpu;
  const float qw = (float)tg_target_width(game->bloom[1]), qh = (float)tg_target_height(game->bloom[1]);
  const float w = (float)width, h = (float)height;
  tg_frame_copy(g);

  post_uniforms u = {.params = {MODE_HIGH, HIGH_EXPONENT, HDR_EXP, HDR_REMAP_S}, .params2 = {HDR_REMAP_T}};
  pass(game, game->bloom[0], TG_TEXTURE_FRAME, 0, &u);

  // a quarter the size: four taps a texel of the big one to each side of the
  // small one's pixel corner (Direct3D's texel), averaged
  u.params[0] = MODE_TAPS;
  for (int i = 0; i < 4; i++) {
    u.offsets[i][0] = -0.5f / qw + (i < 2 ? -1.f : 1.f) / w;
    u.offsets[i][1] = -0.5f / qh + (i % 2 ? -1.f : 1.f) / h;
    u.weights[i] = 0.25f;
  }
  pass(game, game->bloom[1], tg_target_texture(game->bloom[0]), 0, &u);

  // across, then down
  for (int i = 0; i < 4; i++) {
    u.offsets[i][0] = blur_taps[i] / qw;
    u.offsets[i][1] = 0.f;
    u.weights[i] = blur_weights[i];
  }
  pass(game, game->bloom[2], tg_target_texture(game->bloom[1]), 0, &u);
  for (int i = 0; i < 4; i++) {
    u.offsets[i][0] = 0.f;
    u.offsets[i][1] = blur_taps[i] / qh;
  }
  pass(game, game->bloom[1], tg_target_texture(game->bloom[2]), 0, &u);

  // added to the frame (the bloom's coordinates are half its texel off, as
  // the game's are)
  u.params[0] = MODE_ADD;
  memset(u.offsets, 0, sizeof u.offsets);
  u.offsets[0][0] = 0.5f / qw - 0.5f / w;
  u.offsets[0][1] = 0.5f / qh - 0.5f / h;
  pass(game, NULL, TG_TEXTURE_FRAME, tg_target_texture(game->bloom[1]), &u);
}
