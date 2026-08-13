// Graphics resources owned by the DDNet game module.
//
// Everything a tee needs to appear on screen is created here through
// ft_engine_api: the sprite sheets become engine atlases, and the tee itself
// gets its own pipeline built from this game's SPIR-V. The engine supplies
// batching and the graphics API; it has no idea what a tee is.

#include "dd_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define DD_GFX_PATH_SEP '\\'
#else
#define DD_GFX_PATH_SEP '/'
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

// Sprite tables, in source-sheet pixel coordinates.
static const ft_sprite_rect gameskin_rects[GAMESKIN_SPRITE_COUNT] = {
    [GAMESKIN_HAMMER_BODY] = {64, 32, 128, 96},
    [GAMESKIN_GUN_BODY] = {64, 128, 128, 64},
    [GAMESKIN_GUN_PROJ] = {192, 128, 64, 64},
    [GAMESKIN_GUN_MUZZLE1] = {256, 128, 128, 64},
    [GAMESKIN_GUN_MUZZLE2] = {384, 128, 128, 64},
    [GAMESKIN_GUN_MUZZLE3] = {512, 128, 128, 64},
    [GAMESKIN_SHOTGUN_BODY] = {64, 192, 256, 64},
    [GAMESKIN_SHOTGUN_PROJ] = {320, 192, 64, 64},
    [GAMESKIN_SHOTGUN_MUZZLE1] = {384, 192, 128, 64},
    [GAMESKIN_SHOTGUN_MUZZLE2] = {512, 192, 128, 64},
    [GAMESKIN_SHOTGUN_MUZZLE3] = {640, 192, 128, 64},
    [GAMESKIN_GRENADE_BODY] = {64, 256, 224, 64},
    [GAMESKIN_GRENADE_PROJ] = {320, 256, 64, 64},
    [GAMESKIN_LASER_BODY] = {64, 384, 224, 96},
    [GAMESKIN_LASER_PROJ] = {320, 384, 64, 64},
    [GAMESKIN_NINJA_BODY] = {64, 320, 256, 64},
    [GAMESKIN_NINJA_MUZZLE1] = {800, 0, 224, 128},
    [GAMESKIN_NINJA_MUZZLE2] = {800, 128, 224, 128},
    [GAMESKIN_NINJA_MUZZLE3] = {800, 256, 224, 128},
    [GAMESKIN_HEALTH_FULL] = {672, 0, 64, 64},
    [GAMESKIN_HEALTH_EMPTY] = {736, 0, 64, 64},
    [GAMESKIN_ARMOR_FULL] = {672, 64, 64, 64},
    [GAMESKIN_ARMOR_EMPTY] = {736, 64, 64, 64},
    [GAMESKIN_HOOK_CHAIN] = {64, 0, 32, 32},
    [GAMESKIN_HOOK_HEAD] = {96, 0, 64, 32},
    [GAMESKIN_PARTICLE_0] = {192, 0, 32, 32},
    [GAMESKIN_PARTICLE_1] = {192, 32, 32, 32},
    [GAMESKIN_PARTICLE_2] = {224, 0, 32, 32},
    [GAMESKIN_PARTICLE_3] = {224, 32, 32, 32},
    [GAMESKIN_PARTICLE_4] = {256, 0, 32, 32},
    [GAMESKIN_PARTICLE_5] = {256, 32, 32, 32},
    [GAMESKIN_PARTICLE_6] = {288, 0, 64, 64},
    [GAMESKIN_PARTICLE_7] = {352, 0, 64, 64},
    [GAMESKIN_PARTICLE_8] = {416, 0, 64, 64},
    [GAMESKIN_STAR_0] = {480, 0, 64, 64},
    [GAMESKIN_STAR_1] = {544, 0, 64, 64},
    [GAMESKIN_STAR_2] = {608, 0, 64, 64},
    [GAMESKIN_PICKUP_HEALTH] = {320, 64, 64, 64},
    [GAMESKIN_PICKUP_ARMOR] = {384, 64, 64, 64},
    [GAMESKIN_PICKUP_HAMMER] = {64, 32, 128, 96},
    [GAMESKIN_PICKUP_GUN] = {64, 128, 128, 64},
    [GAMESKIN_PICKUP_SHOTGUN] = {64, 192, 256, 64},
    [GAMESKIN_PICKUP_GRENADE] = {64, 256, 224, 64},
    [GAMESKIN_PICKUP_LASER] = {64, 384, 224, 96},
    [GAMESKIN_PICKUP_NINJA] = {64, 320, 256, 64},
    [GAMESKIN_PICKUP_ARMOR_SHOTGUN] = {480, 64, 64, 64},
    [GAMESKIN_PICKUP_ARMOR_GRENADE] = {544, 64, 64, 64},
    [GAMESKIN_PICKUP_ARMOR_NINJA] = {320, 320, 64, 64},
    [GAMESKIN_PICKUP_ARMOR_LASER] = {608, 64, 64, 64},
    [GAMESKIN_FLAG_BLUE] = {384, 256, 128, 256},
    [GAMESKIN_FLAG_RED] = {512, 256, 128, 256}};

static const ft_sprite_rect particle_rects[PARTICLE_SPRITE_COUNT] = {
    [PARTICLE_SLICE] = {0, 0, 64, 64},         // 0,0
    [PARTICLE_BALL] = {64, 0, 64, 64},         // 1,0
    [PARTICLE_SPLAT01] = {128, 0, 64, 64},     // 2,0
    [PARTICLE_SPLAT02] = {192, 0, 64, 64},     // 3,0
    [PARTICLE_SPLAT03] = {256, 0, 64, 64},     // 4,0
    [PARTICLE_SMOKE] = {0, 64, 64, 64},        // 0,1
    [PARTICLE_SHELL] = {0, 128, 128, 128},     // 0,2 2x2
    [PARTICLE_EXPL01] = {0, 256, 256, 256},    // 0,4 4x4
    [PARTICLE_AIRJUMP] = {128, 128, 128, 128}, // 2,2 2x2
    [PARTICLE_HIT01] = {256, 64, 128, 128}     // 4,1 2x2
};

static const ft_sprite_rect extra_rects[EXTRA_SPRITE_COUNT] = {
    [EXTRA_SNOWFLAKE] = {0, 0, 64, 64}, // 0,0 2x2
    [EXTRA_SPARKLE] = {64, 0, 64, 64},  // 2,0 2x2
    [EXTRA_PULLEY] = {128, 0, 32, 32},  // 4,0 1x1
    [EXTRA_HECTAGON] = {192, 0, 64, 64} // 6,0 2x2
};

static const ft_sprite_rect cursor_rects[CURSOR_SPRITE_COUNT] = {
    [CURSOR_HAMMER] = {0, 0, 64, 64}, [CURSOR_GUN] = {0, 128, 64, 64}, [CURSOR_SHOTGUN] = {0, 192, 64, 64}, [CURSOR_GRENADE] = {0, 256, 64, 64}, [CURSOR_LASER] = {0, 384, 64, 64}, [CURSOR_NINJA] = {0, 320, 64, 64}};

void dd_log(ft_game *game, ft_log_level level, const char *fmt, ...) {
  if (!game || !game->engine || !game->engine->log) return;
  char message[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  game->engine->log(level, "DDNet", message);
}

static ft_color to_color(const vec4 c) { return (ft_color){c[0], c[1], c[2], c[3]}; }

unsigned char *dd_decode_png(const void *data, size_t size, int *out_w, int *out_h, int *out_channels) {
  return stbi_load_from_memory(data, (int)size, out_w, out_h, out_channels, STBI_rgb_alpha);
}

void dd_free_png(unsigned char *pixels) { stbi_image_free(pixels); }

// --- resource loading --------------------------------------------------------

static ft_texture *load_sheet(ft_game *game, const char *relative, uint32_t *out_w, uint32_t *out_h) {
  char path[1024];
  game->engine->resolve_data_path(relative, path, sizeof(path));

  void *file = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &file, &size)) {
    dd_log(game, FT_LOG_ERROR, "Missing asset '%s'.", path);
    return NULL;
  }

  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(file, (int)size, &w, &h, &channels, STBI_rgb_alpha);
  game->engine->free_file_data(file);
  if (!pixels) {
    dd_log(game, FT_LOG_ERROR, "Could not decode '%s'.", path);
    return NULL;
  }

  ft_texture_desc desc = {.struct_size = sizeof(desc),
                          .pixels = pixels,
                          .width = (uint32_t)w,
                          .height = (uint32_t)h,
                          .layers = 1,
                          .format = FT_TEXTURE_RGBA8,
                          .mipmaps = true,
                          .linear_filter = true};
  ft_texture *texture = game->engine->texture_create(&desc);
  stbi_image_free(pixels);
  if (out_w) *out_w = (uint32_t)w;
  if (out_h) *out_h = (uint32_t)h;
  return texture;
}

static ft_atlas *make_atlas(ft_game *game, ft_texture *texture, const ft_sprite_rect *rects, uint32_t count, uint32_t max_instances) {
  if (!texture) return NULL;
  ft_atlas_desc desc = {.struct_size = sizeof(desc),
                        .texture = texture,
                        .sprites = rects,
                        .sprite_count = count,
                        .max_instances_per_frame = max_instances};
  return game->engine->atlas_create(&desc);
}

// The tee pipeline. Attribute locations mirror data/games/ddnet/shaders/skin.vert;
// location 0 is the engine's quad corner, so this game's data starts at 1.
static bool create_skin_pipeline(ft_game *game) {
  char vert_path[1024], frag_path[1024];
  game->engine->resolve_data_path("shaders/skin.vert.spv", vert_path, sizeof(vert_path));
  game->engine->resolve_data_path("shaders/skin.frag.spv", frag_path, sizeof(frag_path));

  void *vert = NULL, *frag = NULL;
  size_t vert_size = 0, frag_size = 0;
  if (!game->engine->read_file(vert_path, &vert, &vert_size) || !game->engine->read_file(frag_path, &frag, &frag_size)) {
    dd_log(game, FT_LOG_ERROR, "Missing tee shaders (%s / %s).", vert_path, frag_path);
    game->engine->free_file_data(vert);
    game->engine->free_file_data(frag);
    return false;
  }

  const ft_vertex_attr attrs[] = {
      {1, offsetof(dd_skin_instance_t, pos), FT_VERTEX_FLOAT2},
      {2, offsetof(dd_skin_instance_t, scale), FT_VERTEX_FLOAT1},
      {3, offsetof(dd_skin_instance_t, skin_index), FT_VERTEX_INT1},
      {4, offsetof(dd_skin_instance_t, eye_state), FT_VERTEX_INT1},
      {5, offsetof(dd_skin_instance_t, body), FT_VERTEX_FLOAT3},
      {6, offsetof(dd_skin_instance_t, back_foot), FT_VERTEX_FLOAT3},
      {7, offsetof(dd_skin_instance_t, front_foot), FT_VERTEX_FLOAT3},
      {8, offsetof(dd_skin_instance_t, attach), FT_VERTEX_FLOAT3},
      {9, offsetof(dd_skin_instance_t, dir), FT_VERTEX_FLOAT2},
      {10, offsetof(dd_skin_instance_t, col_body), FT_VERTEX_FLOAT3},
      {11, offsetof(dd_skin_instance_t, col_feet), FT_VERTEX_FLOAT3},
      {12, offsetof(dd_skin_instance_t, col_custom), FT_VERTEX_INT1},
      {13, offsetof(dd_skin_instance_t, mode), FT_VERTEX_INT1},
  };

  ft_pipeline_desc desc = {.struct_size = sizeof(desc),
                           .vertex_spirv = vert,
                           .vertex_spirv_size = vert_size,
                           .fragment_spirv = frag,
                           .fragment_spirv_size = frag_size,
                           .instance_attrs = attrs,
                           .instance_attr_count = (uint32_t)(sizeof(attrs) / sizeof(attrs[0])),
                           .instance_stride = sizeof(dd_skin_instance_t),
                           .max_instances_per_frame = 4096,
                           .texture_count = 2, // colour sheet + weight sheet
                           .alpha_blend = true};
  game->gfx.skin_pipeline = game->engine->pipeline_create(&desc);
  game->engine->free_file_data(vert);
  game->engine->free_file_data(frag);

  if (!game->gfx.skin_pipeline) {
    dd_log(game, FT_LOG_ERROR, "Could not create the tee pipeline.");
    return false;
  }
  return true;
}

bool dd_gfx_create(ft_game *game) {
  if (game->headless) return true;
  dd_gfx_t *gfx = &game->gfx;
  if (gfx->ready) return true;

  memcpy(gfx->gameskin_rects, gameskin_rects, sizeof(gameskin_rects));
  memcpy(gfx->cursor_rects, cursor_rects, sizeof(cursor_rects));
  memcpy(gfx->particle_rects, particle_rects, sizeof(particle_rects));
  memcpy(gfx->extra_rects, extra_rects, sizeof(extra_rects));

  gfx->gameskin_texture = load_sheet(game, "textures/game.png", NULL, NULL);
  gfx->particles_texture = load_sheet(game, "textures/particles.png", NULL, NULL);
  gfx->extras_texture = load_sheet(game, "textures/extras.png", NULL, NULL);

  gfx->gameskin = make_atlas(game, gfx->gameskin_texture, gfx->gameskin_rects, GAMESKIN_SPRITE_COUNT, 200000);
  gfx->particles = make_atlas(game, gfx->particles_texture, gfx->particle_rects, PARTICLE_SPRITE_COUNT, 200000);
  gfx->extras = make_atlas(game, gfx->extras_texture, gfx->extra_rects, EXTRA_SPRITE_COUNT, 200000);
  // One crosshair at a time, so a tiny instance ring is plenty.
  gfx->cursor = make_atlas(game, gfx->gameskin_texture, gfx->cursor_rects, CURSOR_SPRITE_COUNT, 8);
  if (!gfx->gameskin) {
    dd_log(game, FT_LOG_ERROR, "Sprite sheets unavailable; tees will not be drawn.");
    return false;
  }

  // Two arrays with the same layout: the tee sheets themselves, and the
  // grayscale weights the shader tints with.
  ft_texture_desc skin_desc = {.struct_size = sizeof(skin_desc),
                               .pixels = NULL,
                               .width = DD_SKIN_ATLAS_W,
                               .height = DD_SKIN_ATLAS_H,
                               .layers = DD_MAX_SKINS,
                               .format = FT_TEXTURE_RGBA8,
                               .mipmaps = true,
                               .linear_filter = true};
  gfx->skin_array = game->engine->texture_create(&skin_desc);
  skin_desc.format = FT_TEXTURE_RG8;
  gfx->skin_color_array = game->engine->texture_create(&skin_desc);

  if (!gfx->skin_array || !gfx->skin_color_array) {
    dd_log(game, FT_LOG_ERROR, "Could not create the DDNet skin texture arrays.");
    dd_gfx_destroy(game);
    return false;
  }

  if (!create_skin_pipeline(game)) return false;

  gfx->skin_batch_capacity = 256;
  gfx->skin_batch = calloc(gfx->skin_batch_capacity, sizeof(dd_skin_instance_t));
  gfx->default_skin = dd_gfx_skin_index(game, "default");
  gfx->ninja_skin = dd_gfx_skin_index(game, "x_ninja");
  gfx->ready = true;
  return true;
}

void dd_gfx_destroy(ft_game *game) {
  dd_gfx_t *gfx = &game->gfx;
  if (!game->engine) return;

  if (gfx->skin_pipeline) game->engine->pipeline_destroy(gfx->skin_pipeline);
  if (gfx->map_pipeline) game->engine->pipeline_destroy(gfx->map_pipeline);
  if (gfx->entities) game->engine->texture_destroy(gfx->entities);
  if (gfx->gameskin) game->engine->atlas_destroy(gfx->gameskin);
  if (gfx->particles) game->engine->atlas_destroy(gfx->particles);
  if (gfx->extras) game->engine->atlas_destroy(gfx->extras);
  if (gfx->cursor) game->engine->atlas_destroy(gfx->cursor);
  if (gfx->skin_array) game->engine->texture_destroy(gfx->skin_array);
  if (gfx->skin_color_array) game->engine->texture_destroy(gfx->skin_color_array);
  free(gfx->skin_batch);
  free(gfx->hand_batch);
  free(gfx->hook_hand_batch);
  memset(gfx, 0, sizeof(*gfx));
}

// --- skins -------------------------------------------------------------------

// Repacks a 2:1 DDNet skin sheet into the layout the tee shader samples, and
// derives the grayscale weight sheet DDNet tints with. Ported from the engine's
// renderer, where it never belonged: nothing about this is general to games.
static bool build_skin_layers(const stbi_uc *source, int src_w, int src_h, uint8_t *out_rgba, uint8_t *out_weights) {
  const int final_width = DD_SKIN_ATLAS_W;
  const int final_height = DD_SKIN_ATLAS_H;
  const float scale = (float)src_w / 256.0f;
  const size_t src_pixel_count = (size_t)src_w * (size_t)src_h;

  stbi_uc *pixels = malloc(src_pixel_count * 4);
  uint8_t *gray = malloc(src_pixel_count * 2);
  if (!pixels || !gray) {
    free(pixels);
    free(gray);
    return false;
  }
  memcpy(pixels, source, src_pixel_count * 4);

  for (size_t i = 0; i < src_pixel_count; ++i) {
    gray[i * 2 + 0] = (uint8_t)(0.2126f * pixels[i * 4 + 0] + 0.7152f * pixels[i * 4 + 1] + 0.0722f * pixels[i * 4 + 2]);
    gray[i * 2 + 1] = pixels[i * 4 + 3];
  }

  // Renormalise the body sprite only, so the outline and feet keep their weights.
  const int body_w = (int)(scale * 96.0f);
  const int body_h = (int)(scale * 96.0f);
  uint32_t freq[256] = {0};
  for (int y = 0; y < body_h; ++y) {
    for (int x = 0; x < body_w; ++x) {
      size_t idx = ((size_t)y * src_w + (size_t)x) * 2u;
      if (gray[idx + 1] > 128) freq[gray[idx]]++;
    }
  }
  uint8_t org_weight = 1;
  for (int i = 1; i < 256; ++i) {
    if (freq[org_weight] < freq[i]) org_weight = (uint8_t)i;
  }

  const float new_weight = 192.0f;
  const float inv_org = 1.0f / (float)org_weight;
  const float inv_rest = org_weight < 255 ? 1.0f / (float)(255 - org_weight) : 0.0f;
  for (int y = 0; y < body_h; ++y) {
    for (int x = 0; x < body_w; ++x) {
      size_t idx = ((size_t)y * src_w + (size_t)x) * 2u;
      const uint8_t v = gray[idx];
      gray[idx] = v <= org_weight ? (uint8_t)((float)v * inv_org * new_weight)
                                  : (uint8_t)(((float)(v - org_weight) * inv_rest) * (255.0f - new_weight) + new_weight);
    }
  }

  // Premultiply before resizing, so bilinear filtering does not bleed colour
  // out of transparent texels.
  for (size_t i = 0; i < src_pixel_count; i++) {
    const size_t idx = i * 4;
    const uint8_t a = pixels[idx + 3];
    pixels[idx + 0] = (uint8_t)((int)pixels[idx + 0] * a / 255);
    pixels[idx + 1] = (uint8_t)((int)pixels[idx + 1] * a / 255);
    pixels[idx + 2] = (uint8_t)((int)pixels[idx + 2] * a / 255);
    gray[i * 2] = (uint8_t)((int)gray[i * 2] * a / 255);
  }

  memset(out_rgba, 0, (size_t)final_width * final_height * 4);
  memset(out_weights, 0, (size_t)final_width * final_height * 2);

  // Both sheets go through the same layout so one set of uvs addresses either.
#define COPY_PART(src_x, src_y, w, h, dst_x, dst_y)                                                                                        \
  do {                                                                                                                                     \
    const int sx_ = (int)((src_x) * scale), sy_ = (int)((src_y) * scale);                                                                   \
    const int sw_ = (int)((w) * scale), sh_ = (int)((h) * scale);                                                                           \
    stbir_resize_uint8_linear(pixels + ((size_t)sy_ * src_w + sx_) * 4, sw_, sh_, src_w * 4,                                                \
                              out_rgba + ((size_t)(dst_y) * final_width + (dst_x)) * 4, (w) * 2, (h) * 2, final_width * 4, STBIR_RGBA_PM);  \
    stbir_resize_uint8_linear(gray + ((size_t)sy_ * src_w + sx_) * 2, sw_, sh_, src_w * 2,                                                  \
                              out_weights + ((size_t)(dst_y) * final_width + (dst_x)) * 2, (w) * 2, (h) * 2, final_width * 2,               \
                              STBIR_2CHANNEL);                                                                                              \
  } while (0)

  COPY_PART(0, 0, 96, 96, 8, 8);        // body
  COPY_PART(96, 0, 96, 96, 208, 8);     // body shadow
  COPY_PART(192, 32, 64, 32, 8, 208);   // foot
  COPY_PART(192, 64, 64, 32, 144, 208); // foot shadow
  COPY_PART(192, 0, 32, 32, 280, 208);  // hand
  COPY_PART(224, 0, 32, 32, 352, 208);  // hand shadow
  for (int i = 0; i < 6; ++i) {
    COPY_PART(64 + i * 32, 96, 32, 32, 8 + i * 72, 280); // eyes
  }
#undef COPY_PART

  for (int i = 0; i < final_width * final_height; i++) {
    if (out_rgba[i * 4 + 3] == 0) {
      out_rgba[i * 4 + 0] = 0;
      out_rgba[i * 4 + 1] = 0;
      out_rgba[i * 4 + 2] = 0;
    }
    if (out_weights[i * 2 + 1] == 0) out_weights[i * 2 + 0] = 0;
  }

  free(pixels);
  free(gray);
  return true;
}

static bool load_skin_path_layers(ft_game *game, const char *name, const char *path, uint8_t **out_rgba, uint8_t **out_weights) {
  *out_rgba = NULL;
  *out_weights = NULL;
  void *file = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &file, &size)) return false;

  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(file, (int)size, &w, &h, &channels, STBI_rgb_alpha);
  game->engine->free_file_data(file);
  if (!pixels) return false;
  if (w <= 0 || h <= 0 || w != h * 2) {
    dd_log(game, FT_LOG_WARN, "Skin '%s' is %dx%d; a skin sheet has to be 2:1.", name, w, h);
    stbi_image_free(pixels);
    return false;
  }

  uint8_t *rgba = malloc((size_t)DD_SKIN_ATLAS_W * DD_SKIN_ATLAS_H * 4);
  uint8_t *weights = malloc((size_t)DD_SKIN_ATLAS_W * DD_SKIN_ATLAS_H * 2);
  bool ok = rgba && weights && build_skin_layers(pixels, w, h, rgba, weights);
  stbi_image_free(pixels);
  if (!ok) {
    free(rgba);
    free(weights);
    return false;
  }

  *out_rgba = rgba;
  *out_weights = weights;
  return true;
}

static int load_skin_path_into_layer(ft_game *game, const char *name, const char *path, int layer) {
  uint8_t *rgba = NULL, *weights = NULL;
  if (!load_skin_path_layers(game, name, path, &rgba, &weights)) return -1;
  const bool ok = game->engine->texture_update_layer(game->gfx.skin_array, (uint32_t)layer, rgba, DD_SKIN_ATLAS_W, DD_SKIN_ATLAS_H) &&
                  game->engine->texture_update_layer(game->gfx.skin_color_array, (uint32_t)layer, weights, DD_SKIN_ATLAS_W,
                                                     DD_SKIN_ATLAS_H);
  free(rgba);
  free(weights);
  return ok ? layer : -1;
}

static int load_skin_into_layer(ft_game *game, const char *name, int layer) {
  if (!name || !*name || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return -1;

  char relative[256];
  snprintf(relative, sizeof(relative), "skins/%s.png", name);
  char path[1024];
  game->engine->resolve_cache_path(relative, path, sizeof(path));
  int loaded = load_skin_path_into_layer(game, name, path, layer);
  if (loaded >= 0) return loaded;

  game->engine->resolve_data_path(relative, path, sizeof(path));
  loaded = load_skin_path_into_layer(game, name, path, layer);
  if (loaded >= 0) return loaded;

#ifdef _WIN32
  const char *base = getenv("APPDATA");
  if (base) {
    snprintf(path, sizeof(path), "%s\\frametee\\skins\\%s.png", base, name);
    loaded = load_skin_path_into_layer(game, name, path, layer);
    if (loaded >= 0) return loaded;
  }
  static const char *subdirs[] = {"Teeworlds\\skins", "Teeworlds\\downloadedskins", "DDNet\\skins", "DDNet\\downloadedskins"};
#else
  const char *base = getenv("HOME");
  const char *config_home = getenv("XDG_CONFIG_HOME");
  if (config_home) {
    snprintf(path, sizeof(path), "%s/frametee/skins/%s.png", config_home, name);
    loaded = load_skin_path_into_layer(game, name, path, layer);
    if (loaded >= 0) return loaded;
  } else if (base) {
    snprintf(path, sizeof(path), "%s/.config/frametee/skins/%s.png", base, name);
    loaded = load_skin_path_into_layer(game, name, path, layer);
    if (loaded >= 0) return loaded;
  }
  static const char *subdirs[] = {".teeworlds/skins", ".teeworlds/downloadedskins", ".local/share/ddnet/skins",
                                  ".local/share/ddnet/downloadedskins"};
#endif
  if (base) {
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
      snprintf(path, sizeof(path), "%s%c%s%c%s.png", base, DD_GFX_PATH_SEP, subdirs[i], DD_GFX_PATH_SEP, name);
      loaded = load_skin_path_into_layer(game, name, path, layer);
      if (loaded >= 0) return loaded;
    }
  }
  return -1;
}

int dd_gfx_skin_index(ft_game *game, const char *name) {
  dd_gfx_t *gfx = &game->gfx;
  if (!name || !*name) return gfx->default_skin;

  for (int i = 0; i < DD_MAX_PLAYER_SKINS; ++i) {
    if (gfx->skins[i].used && strcmp(gfx->skins[i].name, name) == 0) return gfx->skins[i].loaded ? i : gfx->default_skin;
  }

  int layer = -1;
  for (int i = 0; i < DD_MAX_PLAYER_SKINS; ++i) {
    if (!gfx->skins[i].used) {
      layer = i;
      break;
    }
  }
  if (layer < 0) {
    dd_log(game, FT_LOG_WARN, "Out of skin layers (%d); '%s' falls back to the default.", DD_MAX_PLAYER_SKINS, name);
    return gfx->default_skin;
  }

  // Remember the attempt either way, so a missing skin is not re-read every frame.
  gfx->skins[layer].used = true;
  snprintf(gfx->skins[layer].name, sizeof(gfx->skins[layer].name), "%s", name);
  gfx->skins[layer].loaded = load_skin_into_layer(game, name, layer) >= 0;
  if (!gfx->skins[layer].loaded) {
    dd_log(game, FT_LOG_WARN, "Skin '%s' could not be loaded.", name);
    return gfx->default_skin;
  }
  return layer;
}

int dd_gfx_load_skin_path(ft_game *game, const char *name, const char *path) {
  if (!game || !name || !*name || !path || !*path) return -1;
  dd_gfx_t *gfx = &game->gfx;

  for (int i = 0; i < DD_MAX_PLAYER_SKINS; ++i) {
    if (!gfx->skins[i].used || strcmp(gfx->skins[i].name, name) != 0) continue;
    if (!gfx->skins[i].loaded) gfx->skins[i].loaded = load_skin_path_into_layer(game, name, path, i) >= 0;
    return gfx->skins[i].loaded ? i : gfx->default_skin;
  }

  for (int i = 0; i < DD_MAX_PLAYER_SKINS; ++i) {
    if (gfx->skins[i].used) continue;
    gfx->skins[i].used = true;
    snprintf(gfx->skins[i].name, sizeof(gfx->skins[i].name), "%s", name);
    gfx->skins[i].loaded = load_skin_path_into_layer(game, name, path, i) >= 0;
    return gfx->skins[i].loaded ? i : gfx->default_skin;
  }
  dd_log(game, FT_LOG_WARN, "Out of skin layers (%d); '%s' falls back to the default.", DD_MAX_PLAYER_SKINS, name);
  return gfx->default_skin;
}

bool dd_gfx_render_skin_preview(ft_game *game, const char *name, const char *path, ft_texture *destination, uint32_t destination_x,
                                uint32_t destination_y) {
  if (!game || !game->gfx.ready || !destination || !game->engine->render_instances_preview) return false;
  uint8_t *rgba = NULL, *weights = NULL;
  if (!load_skin_path_layers(game, name, path, &rgba, &weights)) return false;

  dd_anim_state_t anim;
  dd_anim_state_set(&anim, &anim_base, 0.f);
  dd_anim_state_add(&anim, &anim_idle, 0.f, 1.f);

  dd_skin_instance_t instance = {0};
  instance.scale = 0.75f * 1.25f;
  instance.skin_index = DD_SKIN_PREVIEW_LAYER;
  instance.eye_state = 6;
  instance.dir[0] = 1.f;
  instance.col_body[0] = instance.col_body[1] = instance.col_body[2] = 1.f;
  instance.col_feet[0] = instance.col_feet[1] = instance.col_feet[2] = 1.f;
  instance.body[0] = anim.body.x;
  instance.body[1] = anim.body.y;
  instance.body[2] = anim.body.angle;
  instance.back_foot[0] = anim.back_foot.x;
  instance.back_foot[1] = anim.back_foot.y;
  instance.back_foot[2] = anim.back_foot.angle;
  instance.front_foot[0] = anim.front_foot.x;
  instance.front_foot[1] = anim.front_foot.y;
  instance.front_foot[2] = anim.front_foot.angle;
  instance.attach[0] = anim.attach.x;
  instance.attach[1] = anim.attach.y;
  instance.attach[2] = anim.attach.angle;
  instance.mode = DD_SKIN_MODE_TEE;

  ft_texture *textures[2] = {game->gfx.skin_array, game->gfx.skin_color_array};
  const ft_texture_layer_update updates[2] = {
      {.texture = game->gfx.skin_array,
       .layer = DD_SKIN_PREVIEW_LAYER,
       .pixels = rgba,
       .width = DD_SKIN_ATLAS_W,
       .height = DD_SKIN_ATLAS_H},
      {.texture = game->gfx.skin_color_array,
       .layer = DD_SKIN_PREVIEW_LAYER,
       .pixels = weights,
       .width = DD_SKIN_ATLAS_W,
       .height = DD_SKIN_ATLAS_H},
  };
  const ft_instance_preview_desc desc = {.struct_size = sizeof(desc),
                                         .pipeline = game->gfx.skin_pipeline,
                                         .textures = textures,
                                         .texture_count = 2,
                                         .instances = &instance,
                                         .instance_count = 1,
                                         .width = 128,
                                         .height = 128,
                                         .clear_color = {0.f, 0.f, 0.f, 0.f},
                                         .updates = updates,
                                         .update_count = 2,
                                         .destination = destination,
                                         .destination_x = destination_x,
                                         .destination_y = destination_y};
  ft_texture *preview = game->engine->render_instances_preview(&desc);
  free(rgba);
  free(weights);
  return preview == destination;
}

// --- draw helpers ------------------------------------------------------------

const ft_sprite_rect *dd_sprite_rect(ft_game *game, ft_atlas *atlas, uint32_t index) {
  const dd_gfx_t *gfx = &game->gfx;
  if (atlas == gfx->gameskin && index < GAMESKIN_SPRITE_COUNT) return &gfx->gameskin_rects[index];
  if (atlas == gfx->particles && index < PARTICLE_SPRITE_COUNT) return &gfx->particle_rects[index];
  if (atlas == gfx->extras && index < EXTRA_SPRITE_COUNT) return &gfx->extra_rects[index];
  if (atlas == gfx->cursor && index < CURSOR_SPRITE_COUNT) return &gfx->cursor_rects[index];
  return NULL;
}

void dd_draw_sprite(ft_game *game, ft_atlas *atlas, float z, vec2 pos, vec2 size, float rotation, uint32_t sprite, vec4 color) {
  if (!atlas) return;
  const ft_sprite_draw draw = {.pos = {pos[0], pos[1]},
                               .size = {size[0], size[1]},
                               .rotation = rotation,
                               .sprite_index = sprite,
                               .color = to_color(color),
                               .tiling = {1.f, 1.f}};
  game->engine->draw_sprites(atlas, z, &draw, 1);
}

void dd_draw_sprites(ft_game *game, ft_atlas *atlas, float z, const ft_sprite_draw *draws, uint32_t count) {
  if (!atlas || count == 0) return;
  game->engine->draw_sprites(atlas, z, draws, count);
}

void dd_draw_line(ft_game *game, float z, vec2 a, vec2 b, vec4 color, float thickness) {
  game->engine->draw_line(z, (ft_vec2){a[0], a[1]}, (ft_vec2){b[0], b[1]}, to_color(color), thickness);
}

void dd_draw_circle(ft_game *game, float z, vec2 center, float radius, vec4 color, uint32_t segments) {
  game->engine->draw_circle(z, (ft_vec2){center[0], center[1]}, radius, to_color(color), segments);
}

void dd_draw_triangle(ft_game *game, float z, vec2 a, vec2 b, vec2 c, vec4 color) {
  game->engine->draw_triangle(z, (ft_vec2){a[0], a[1]}, (ft_vec2){b[0], b[1]}, (ft_vec2){c[0], c[1]}, to_color(color));
}

// --- tee batching ------------------------------------------------------------

void dd_skins_begin(ft_game *game) {
  game->gfx.skin_batch_count = 0;
  game->gfx.hand_batch_count = 0;
  game->gfx.hook_hand_batch_count = 0;
}

static dd_skin_instance_t *hand_batch_push(dd_skin_instance_t **batch, uint32_t *count, uint32_t *batch_capacity) {
  if (*count >= *batch_capacity) {
    const uint32_t capacity = *batch_capacity ? *batch_capacity * 2 : 256;
    dd_skin_instance_t *grown = realloc(*batch, (size_t)capacity * sizeof(*grown));
    if (!grown) return NULL;
    *batch = grown;
    *batch_capacity = capacity;
  }
  dd_skin_instance_t *inst = &(*batch)[(*count)++];
  memset(inst, 0, sizeof(*inst));
  return inst;
}

static dd_skin_instance_t *skin_batch_push(ft_game *game) {
  dd_gfx_t *gfx = &game->gfx;
  if (gfx->skin_batch_count >= gfx->skin_batch_capacity) {
    const uint32_t capacity = gfx->skin_batch_capacity ? gfx->skin_batch_capacity * 2 : 256;
    dd_skin_instance_t *grown = realloc(gfx->skin_batch, (size_t)capacity * sizeof(*grown));
    if (!grown) return NULL;
    gfx->skin_batch = grown;
    gfx->skin_batch_capacity = capacity;
  }
  dd_skin_instance_t *inst = &gfx->skin_batch[gfx->skin_batch_count++];
  memset(inst, 0, sizeof(*inst));
  return inst;
}

void dd_skin_push(ft_game *game, vec2 pos, float scale, int skin, int eye, vec2 dir, const dd_anim_state_t *anim, vec3 col_body, vec3 col_feet,
                  bool custom) {
  if (!game->gfx.ready) return;
  dd_skin_instance_t *inst = skin_batch_push(game);
  if (!inst) return;

  inst->pos[0] = pos[0];
  inst->pos[1] = pos[1];
  // The shader maps uvs as in_pos * 0.625 + 0.5, i.e. it expects a quad 1.25x
  // oversized so the animation has room to move within it.
  inst->scale = scale * 1.25f;
  inst->skin_index = skin;
  // Eye sprites start at index 6 in the packed sheet; the first six are the
  // body and feet parts. Passing the raw eye state drew no eyes at all.
  inst->eye_state = eye + 6;
  inst->body[0] = anim->body.x;
  inst->body[1] = anim->body.y;
  inst->body[2] = anim->body.angle;
  inst->back_foot[0] = anim->back_foot.x;
  inst->back_foot[1] = anim->back_foot.y;
  inst->back_foot[2] = anim->back_foot.angle;
  inst->front_foot[0] = anim->front_foot.x;
  inst->front_foot[1] = anim->front_foot.y;
  inst->front_foot[2] = anim->front_foot.angle;
  inst->attach[0] = anim->attach.x;
  inst->attach[1] = anim->attach.y;
  inst->attach[2] = anim->attach.angle;
  inst->dir[0] = dir[0];
  inst->dir[1] = dir[1];
  glm_vec3_copy(col_body, inst->col_body);
  glm_vec3_copy(col_feet, inst->col_feet);
  inst->col_custom = custom ? 1 : 0;
  inst->mode = DD_SKIN_MODE_TEE;
}

void dd_hand_push(ft_game *game, vec2 pos, float scale, int skin, float angle, vec3 col_body, bool custom, bool hook_hand) {
  if (!game->gfx.ready) return;
  dd_gfx_t *gfx = &game->gfx;
  dd_skin_instance_t *inst = hook_hand ? hand_batch_push(&gfx->hook_hand_batch, &gfx->hook_hand_batch_count, &gfx->hook_hand_batch_capacity)
                                       : hand_batch_push(&gfx->hand_batch, &gfx->hand_batch_count, &gfx->hand_batch_capacity);
  if (!inst) return;

  inst->pos[0] = pos[0];
  inst->pos[1] = pos[1];
  // A hand quad spans its sprite exactly, so no 1.25 headroom here.
  inst->scale = scale;
  inst->skin_index = skin;
  inst->eye_state = 6;
  // The hand rides the weapon, which can put it outside the tee's own quad, so
  // it is drawn as its own instance rotated by attach.angle.
  inst->attach[2] = angle;
  glm_vec3_copy(col_body, inst->col_body);
  glm_vec3_copy(col_body, inst->col_feet);
  inst->col_custom = custom ? 1 : 0;
  inst->mode = DD_SKIN_MODE_HAND;
}

// Hands and bodies go in as two draws at their own depths, so a hand gripping
// a weapon stays behind the tee holding it.
void dd_skins_flush(ft_game *game) {
  dd_gfx_t *gfx = &game->gfx;
  if (!gfx->ready || !gfx->skin_pipeline || !gfx->skin_array || !gfx->skin_color_array) return;
  ft_texture *textures[2] = {gfx->skin_array, gfx->skin_color_array};

  if (gfx->hook_hand_batch_count > 0) {
    game->engine->draw_instances(gfx->skin_pipeline, DD_Z_HOOK_HAND, textures, 2, gfx->hook_hand_batch, gfx->hook_hand_batch_count);
    gfx->hook_hand_batch_count = 0;
  }
  if (gfx->hand_batch_count > 0) {
    game->engine->draw_instances(gfx->skin_pipeline, DD_Z_WEAPON_HAND, textures, 2, gfx->hand_batch, gfx->hand_batch_count);
    gfx->hand_batch_count = 0;
  }
  if (gfx->skin_batch_count > 0) {
    game->engine->draw_instances(gfx->skin_pipeline, DD_Z_SKINS, textures, 2, gfx->skin_batch, gfx->skin_batch_count);
    gfx->skin_batch_count = 0;
  }
}

// --- level derived data ------------------------------------------------------

// Walks the map's pickup layers once so rendering does not have to. This lived
// in the engine's ui_post_map_load, which meant the editor had to understand
// DDNet's pickup encoding.
void dd_level_build_pickups(ft_level *level) {
  const int width = level->collision.m_MapData.width;
  const int height = level->collision.m_MapData.height;

  free(level->pickups);
  free(level->pickup_positions);
  free(level->pickup_cooldown_keys);
  free(level->ninja_pickup_indices);
  level->pickups = NULL;
  level->pickup_positions = NULL;
  level->pickup_cooldown_keys = NULL;
  level->ninja_pickup_indices = NULL;
  level->num_pickups = 0;
  level->num_ninja_pickups = 0;

  int num = 0;
  for (int i = 0; i < width * height; ++i) {
    if (level->collision.m_pPickups[i].m_Type >= 0) ++num;
    if (level->collision.m_pFrontPickups[i].m_Type >= 0) ++num;
  }
  if (num <= 0) return;

  level->pickups = malloc(sizeof(SPickup) * num);
  level->pickup_positions = malloc(sizeof(mvec2) * num);
  level->pickup_cooldown_keys = malloc(sizeof(int) * num);
  level->ninja_pickup_indices = malloc(sizeof(int) * num);
  if (!level->pickups || !level->pickup_positions || !level->pickup_cooldown_keys || !level->ninja_pickup_indices) return;

  int count = 0;
  for (int i = 0; i < width * height; ++i) {
    // Two pickup layers share one tile index, so the cooldown key encodes which
    // layer a pickup came from.
    const SPickup front_and_game[2] = {level->collision.m_pPickups[i], level->collision.m_pFrontPickups[i]};
    for (int layer = 0; layer < 2; ++layer) {
      const SPickup pickup = front_and_game[layer];
      if (pickup.m_Type < 0) continue;
      level->pickup_positions[count] = vec2_init((i % width) * 32.f + 16.f, (int)(i / width) * 32.f + 16.f);
      level->pickups[count] = pickup;
      level->pickup_cooldown_keys[count] = i * 2 + layer;
      if (pickup.m_Type == POWERUP_NINJA) level->ninja_pickup_indices[level->num_ninja_pickups++] = count;
      count++;
    }
  }
  level->num_pickups = count;
}
