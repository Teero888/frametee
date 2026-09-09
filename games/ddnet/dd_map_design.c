/* C99 map design renderer, adapted from DDNet's game/map renderer.
 * Static tiles and quads share one immutable GPU mesh. Spatial batches retain
 * map order and let the CPU cull without rebuilding or uploading geometry.
 * Upstream copyright/license: data/games/ddnet/mapres/LICENSE.txt. */
#include "dd_internal.h"
#include "dd_map_math.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAP_CHUNK 64
#define QUAD_BATCH 64

typedef struct {
  uint32_t first, count;
  float bounds[4];
  int quad, border;
} design_batch_t;

typedef struct {
  size_t first, count;
} design_layer_t;
typedef struct {
  ft_texture *tiles, *quads;
} design_image_t;

struct dd_map_design {
  ft_mesh *mesh;
  design_image_t *images;
  design_layer_t *layers;
  design_batch_t *batches;
  size_t num_batches, capacity;
};

typedef struct {
  ft_vertex *vertices;
  uint32_t *indices;
  size_t quads, capacity;
} design_builder_t;

/* Six std140 vec4s, below the engine's 128-byte queued-uniform limit. */
typedef struct {
  float view[4]; /* center.xy, inverse half size.xy */
  float tint[4];
  float clip[4];      /* NDC left, top, right, bottom */
  float geometry[4];  /* offset.xy, scale.xy (tile borders) */
  float animation[4]; /* offset.xy, unused.zw */
  float params[4];    /* cos, sin, lod bias, tile mode */
} design_uniforms_t;

static bool grow(void **data, size_t *capacity, size_t needed, size_t stride) {
  if (needed <= *capacity) return true;
  size_t n = *capacity ? *capacity : 64;
  while (n < needed) {
    if (n > SIZE_MAX / 2) return false;
    n *= 2;
  }
  if (n > SIZE_MAX / stride) return false;
  void *p = realloc(*data, n * stride);
  if (!p) return false;
  *data = p;
  *capacity = n;
  return true;
}

static ft_vertex *append_quad(design_builder_t *b) {
  if (b->quads >= UINT32_MAX / 6) return NULL;
  if (b->quads == b->capacity) {
    size_t cap = b->capacity;
    if (!grow((void **)&b->vertices, &cap, b->quads + 1, 4 * sizeof(ft_vertex))) return NULL;
    uint32_t *indices = realloc(b->indices, cap * 6 * sizeof(*indices));
    if (!indices) return NULL;
    b->indices = indices;
    b->capacity = cap;
  }
  uint32_t base = (uint32_t)b->quads * 4;
  /* DDNet's freeform diagonal is top-left to bottom-right. */
  const uint32_t order[6] = {0, 1, 3, 0, 3, 2};
  for (int i = 0; i < 6; ++i)
    b->indices[b->quads * 6 + i] = base + order[i];
  ft_vertex *v = b->vertices + b->quads++ * 4;
  memset(v, 0, 4 * sizeof(*v));
  return v;
}

static void bounds_add(float bounds[4], float x, float y) {
  if (x < bounds[0]) bounds[0] = x;
  if (y < bounds[1]) bounds[1] = y;
  if (x > bounds[2]) bounds[2] = x;
  if (y > bounds[3]) bounds[3] = y;
}

static bool finish_batch(struct dd_map_design *d, design_builder_t *b, design_batch_t *batch) {
  batch->count = (uint32_t)b->quads * 6 - batch->first;
  if (!batch->count) return true;
  if (!grow((void **)&d->batches, &d->capacity, d->num_batches + 1, sizeof(*d->batches))) return false;
  d->batches[d->num_batches++] = *batch;
  return true;
}

static bool tile_batch(struct dd_map_design *d, design_builder_t *b, const map_layer_t *l, int x0, int y0, int x1, int y1, int border) {
  design_batch_t batch = {.first = (uint32_t)b->quads * 6, .quad = -1, .border = border, .bounds = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX}};
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const map_tile_t *t = &l->tiles[(size_t)y * l->width + x];
      if (!t->index) continue;
      ft_vertex *v = append_quad(b);
      if (!v) return false;
      float px = border == 1 || border == 2 || border >= 5 ? 0.f : (float)x;
      float py = border >= 3 ? 0.f : (float)y;
      uint32_t white = UINT32_MAX, info = t->index | (uint32_t)t->flags << 8;
      for (int k = 0; k < 4; ++k) {
        v[k].pos = (ft_vec2){px + (k & 1), py + (k >> 1)};
        v[k].uv = (ft_vec2){(float)(k & 1), (float)(k >> 1)};
        memcpy(&v[k].color[0], &white, 4);
        v[k].color[1] = (float)info;
      }
      bounds_add(batch.bounds, px, py);
      bounds_add(batch.bounds, px + 1, py + 1);
    }
  return finish_batch(d, b, &batch);
}

static bool build_tiles(struct dd_map_design *d, design_builder_t *b, const map_layer_t *l) {
  for (int y = 0; y < l->height; y += MAP_CHUNK)
    for (int x = 0; x < l->width; x += MAP_CHUNK) {
      int x1 = x + MAP_CHUNK < l->width ? x + MAP_CHUNK : l->width;
      int y1 = y + MAP_CHUNK < l->height ? y + MAP_CHUNK : l->height;
      if (!tile_batch(d, b, l, x, y, x1, y1, 0)) return false;
    }
  for (int y = 0; y < l->height; y += MAP_CHUNK) {
    int end = y + MAP_CHUNK < l->height ? y + MAP_CHUNK : l->height;
    if (!tile_batch(d, b, l, 0, y, 1, end, 1) || !tile_batch(d, b, l, l->width - 1, y, l->width, end, 2)) return false;
  }
  for (int x = 0; x < l->width; x += MAP_CHUNK) {
    int end = x + MAP_CHUNK < l->width ? x + MAP_CHUNK : l->width;
    if (!tile_batch(d, b, l, x, 0, end, 1, 3) || !tile_batch(d, b, l, x, l->height - 1, end, l->height, 4)) return false;
  }
  for (int i = 0; i < 4; ++i) {
    int x = i & 1 ? l->width - 1 : 0, y = i & 2 ? l->height - 1 : 0;
    if (!tile_batch(d, b, l, x, y, x + 1, y + 1, 5 + i)) return false;
  }
  return true;
}

static bool same_envelopes(const map_quad_t *a, const map_quad_t *b) {
  return a->pos_env == b->pos_env && a->color_env == b->color_env && a->pos_env_offset == b->pos_env_offset &&
         a->color_env_offset == b->color_env_offset;
}

static bool build_quads(struct dd_map_design *d, design_builder_t *b, const map_layer_t *l) {
  for (int q = 0; q < l->num_quads;) {
    design_batch_t batch = {.first = (uint32_t)b->quads * 6, .quad = q, .bounds = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX}};
    int first = q;
    do {
      const map_quad_t *quad = &l->quads[q];
      ft_vertex *v = append_quad(b);
      if (!v) return false;
      float cx = quad->points[4][0] / 32768.f, cy = quad->points[4][1] / 32768.f;
      float radius = 0;
      for (int k = 0; k < 4; ++k) {
        v[k].pos = (ft_vec2){quad->points[k][0] / 32768.f, quad->points[k][1] / 32768.f};
        v[k].uv = (ft_vec2){quad->texcoords[k][0] / 1024.f, quad->texcoords[k][1] / 1024.f};
        uint32_t color = 0;
        for (int c = 0; c < 4; ++c)
          color |= (uint32_t)(uint8_t)quad->colors[k][c] << (c * 8);
        memcpy(&v[k].color[0], &color, 4);
        v[k].color[1] = cx;
        v[k].color[2] = cy;
        if (quad->pos_env < 0) bounds_add(batch.bounds, v[k].pos.x, v[k].pos.y);
        else radius = fmaxf(radius, hypotf(v[k].pos.x - cx, v[k].pos.y - cy));
      }
      if (quad->pos_env >= 0) {
        bounds_add(batch.bounds, cx - radius, cy - radius);
        bounds_add(batch.bounds, cx + radius, cy + radius);
      }
      ++q;
    } while (q < l->num_quads && q - first < QUAD_BATCH && same_envelopes(&l->quads[first], &l->quads[q]));
    if (!finish_batch(d, b, &batch)) return false;
  }
  return true;
}

// DDNet resizes non-divisible tile sheets to the lower power of two (at
// least 16) with its bicubic sampler before splitting the 16x16 atlas.
static float cubic(float a, float b, float c, float d, float t) {
  float a3 = -a / 2.f + 3.f * b / 2.f - 3.f * c / 2.f + d / 2.f;
  float a2 = a - 5.f * b / 2.f + 2.f * c - d / 2.f;
  return a3 * t * t * t + a2 * t * t + (-a / 2.f + c / 2.f) * t + b;
}

static unsigned char *resize_tiles(const unsigned char *pixels, int *width, int *height) {
  int w = *width, h = *height, nw = 16, nh = 16;
  while (nw <= w / 2)
    nw *= 2;
  while (nh <= h / 2)
    nh *= 2;
  unsigned char *out = malloc((size_t)nw * nh * 4);
  if (!out) return NULL;
  for (int y = 0; y < nh; ++y) {
    float sy = (float)y / (nh - 1) * h - 0.5f, fy = sy - floorf(sy);
    for (int x = 0; x < nw; ++x) {
      float sx = (float)x / (nw - 1) * w - 0.5f, fx = sx - floorf(sx);
      for (int c = 0; c < 4; ++c) {
        float rows[4];
        for (int j = 0; j < 4; ++j) {
          int iy = (int)sy + j - 1;
          iy = iy < 0 ? 0 : iy >= h ? h - 1
                                    : iy;
          float samples[4];
          for (int k = 0; k < 4; ++k) {
            int ix = (int)sx + k - 1;
            ix = ix < 0 ? 0 : ix >= w ? w - 1
                                      : ix;
            samples[k] = pixels[((size_t)iy * w + ix) * 4 + c];
          }
          rows[j] = cubic(samples[0], samples[1], samples[2], samples[3], fx);
        }
        float value = cubic(rows[0], rows[1], rows[2], rows[3], fy);
        out[((size_t)y * nw + x) * 4 + c] = (unsigned char)fmaxf(0, fminf(255, value));
      }
    }
  }
  *width = nw;
  *height = nh;
  return out;
}

static ft_texture *image_texture(ft_game *game, const unsigned char *pixels, int w, int h, bool tiles) {
  if (!pixels || w <= 0 || h <= 0) return NULL;
  unsigned char *resized = NULL, *sliced = NULL;
  if (tiles && (w % 16 || h % 16)) {
    resized = resize_tiles(pixels, &w, &h);
    if (!resized) return NULL;
    pixels = resized;
  }
  if (tiles) {
    sliced = malloc((size_t)w * h * 4);
    if (!sliced) {
      free(resized);
      return NULL;
    }
    int tw = w / 16, th = h / 16;
    for (int i = 0; i < 256; ++i)
      for (int y = 0; y < th; ++y)
        memcpy(sliced + ((size_t)i * tw * th + (size_t)y * tw) * 4, pixels + ((size_t)(i / 16 * th + y) * w + i % 16 * tw) * 4,
               (size_t)tw * 4);
    pixels = sliced;
    w = tw;
    h = th;
  }
  ft_texture_desc desc = {.struct_size = sizeof(desc),
                          .pixels = pixels,
                          .width = (uint32_t)w,
                          .height = (uint32_t)h,
                          .layers = tiles ? 256 : 1,
                          .format = FT_TEXTURE_RGBA8,
                          .mipmaps = true,
                          .linear_filter = true,
                          .repeat = !tiles};
  ft_texture *texture = game->engine->texture_create(&desc);
  free(sliced);
  free(resized);
  return texture;
}

static bool design_pipelines(ft_game *game) {
  if (game->gfx.design_tiles && game->gfx.design_quads && game->gfx.design_white && game->gfx.design_white_tiles) return true;
  char path[1024];
  void *vert = NULL, *frag = NULL;
  size_t vert_size = 0, frag_size = 0;
  game->engine->resolve_data_path("shaders/design.vert.spv", path, sizeof(path));
  if (!game->engine->read_file(path, &vert, &vert_size)) return false;
  const ft_vertex_attr attrs[] = {
      {0, offsetof(ft_vertex, pos), FT_VERTEX_FLOAT2},
      {1, offsetof(ft_vertex, color), FT_VERTEX_UINT1},
      {2, offsetof(ft_vertex, color) + sizeof(float), FT_VERTEX_FLOAT2},
      {3, offsetof(ft_vertex, uv), FT_VERTEX_FLOAT2},
  };
  ft_pipeline_desc desc = {.struct_size = sizeof(desc),
                           .vertex_spirv = vert,
                           .vertex_spirv_size = vert_size,
                           .instance_attrs = attrs,
                           .instance_attr_count = 4,
                           .texture_count = 1,
                           .alpha_blend = true};
  for (int i = 0; i < 2; ++i) {
    ft_pipeline **pipeline = i ? &game->gfx.design_quads : &game->gfx.design_tiles;
    if (*pipeline) continue;
    game->engine->resolve_data_path(i ? "shaders/design_quad.frag.spv" : "shaders/design_tile.frag.spv", path, sizeof(path));
    if (!game->engine->read_file(path, &frag, &frag_size)) break;
    desc.fragment_spirv = frag;
    desc.fragment_spirv_size = frag_size;
    *pipeline = game->engine->pipeline_create(&desc);
    game->engine->free_file_data(frag);
    frag = NULL;
  }
  game->engine->free_file_data(vert);
  unsigned char white[16 * 16 * 4];
  memset(white, 255, sizeof(white));
  if (!game->gfx.design_white) game->gfx.design_white = image_texture(game, white, 1, 1, false);
  if (!game->gfx.design_white_tiles) game->gfx.design_white_tiles = image_texture(game, white, 16, 16, true);
  return game->gfx.design_tiles && game->gfx.design_quads && game->gfx.design_white && game->gfx.design_white_tiles;
}

void dd_map_design_destroy(ft_game *game, ft_level *level) {
  struct dd_map_design *d = level->design;
  if (!d) return;
  if (d->mesh) game->engine->mesh_destroy(d->mesh);
  if (d->images)
    for (int i = 0; i < level->collision.m_MapData.num_images; ++i) {
      if (d->images[i].tiles) game->engine->texture_destroy(d->images[i].tiles);
      if (d->images[i].quads) game->engine->texture_destroy(d->images[i].quads);
    }
  free(d->images);
  free(d->layers);
  free(d->batches);
  free(d);
  level->design = NULL;
}

void dd_map_design_create(ft_game *game, ft_level *level) {
  if (!design_pipelines(game)) {
    dd_log(game, FT_LOG_ERROR, "Could not create map design pipelines.");
    return;
  }
  const map_data_t *map = &level->collision.m_MapData;
  struct dd_map_design *d = calloc(1, sizeof(*d));
  if (!d) return;
  level->design = d;
  d->images = calloc((size_t)map->num_images, sizeof(*d->images));
  d->layers = calloc((size_t)map->num_layers, sizeof(*d->layers));
  design_builder_t builder = {0};
  if ((map->num_images && !d->images) || (map->num_layers && !d->layers)) goto fail;
  for (int i = 0; i < map->num_images; ++i) {
    bool tiles = false, quads = false;
    for (int l = 0; l < map->num_layers; ++l)
      if (map->layers[l].image == i && !map->layers[l].tile_flags) {
        tiles |= map->layers[l].type == 2;
        quads |= map->layers[l].type == 3;
      }
    if (!tiles && !quads) continue;
    const map_image_t *im = &map->images[i];
    const unsigned char *pixels = im->pixels;
    unsigned char *decoded = NULL;
    int w = im->width, h = im->height;
    if (im->external && im->name && im->name[0] && !strchr(im->name, '/') && !strchr(im->name, '\\') && !strchr(im->name, ':')) {
      char relative[512], path[1024];
      snprintf(relative, sizeof(relative), "mapres/%s.png", im->name);
      game->engine->resolve_data_path(relative, path, sizeof(path));
      void *file = NULL;
      size_t bytes = 0;
      if (game->engine->read_file(path, &file, &bytes)) {
        int channels;
        decoded = dd_decode_png(file, bytes, &w, &h, &channels);
        game->engine->free_file_data(file);
      }
      pixels = decoded;
    }
    if (tiles) d->images[i].tiles = image_texture(game, pixels, w, h, true);
    if (quads) d->images[i].quads = image_texture(game, pixels, w, h, false);
    if ((tiles && !d->images[i].tiles) || (quads && !d->images[i].quads))
      dd_log(game, FT_LOG_WARN, "Could not load map image '%s'.", im->name ? im->name : "(unnamed)");
    dd_free_png(decoded);
  }
  for (int i = 0; i < map->num_layers; ++i) {
    const map_layer_t *l = &map->layers[i];
    d->layers[i].first = d->num_batches;
    if (l->type == 2 && !l->tile_flags && l->tiles) {
      if (!build_tiles(d, &builder, l)) goto fail;
    } else if (l->type == 3 && !build_quads(d, &builder, l)) goto fail;
    d->layers[i].count = d->num_batches - d->layers[i].first;
  }
  if (builder.quads) {
    d->mesh = game->engine->mesh_create(builder.vertices, (uint32_t)builder.quads * 4, sizeof(ft_vertex), builder.indices,
                                        (uint32_t)builder.quads * 6);
    if (!d->mesh) goto fail;
  }
  free(builder.vertices);
  free(builder.indices);
  return;
fail:
  free(builder.vertices);
  free(builder.indices);
  dd_log(game, FT_LOG_ERROR, "Could not create map design geometry.");
  dd_map_design_destroy(game, level);
}

static bool visible(const float a[4], const float b[4]) { return a[2] > b[0] && a[3] > b[1] && a[0] < b[2] && a[1] < b[3]; }

static bool border_geometry(int border, const map_layer_t *l, const float view[4], float g[4]) {
  g[0] = g[1] = 0;
  g[2] = g[3] = 1;
  if (!border) return true;
  bool left = border == 1 || border == 5 || border == 7;
  bool right = border == 2 || border == 6 || border == 8;
  bool top = border == 3 || border == 5 || border == 6;
  bool bottom = border == 4 || border == 7 || border == 8;
  if (left) {
    g[0] = fminf(0, floorf(view[0]));
    g[2] = -g[0];
  }
  if (right) {
    g[0] = (float)l->width;
    g[2] = fmaxf(0, ceilf(view[2]) - l->width);
  }
  if (top) {
    g[1] = fminf(0, floorf(view[1]));
    g[3] = -g[1];
  }
  if (bottom) {
    g[1] = (float)l->height;
    g[3] = fmaxf(0, ceilf(view[3]) - l->height);
  }
  return g[2] > 0 && g[3] > 0;
}

void dd_map_design_render(ft_game *game, const ft_render_frame *frame) {
  const ft_level *level = frame->level;
  if (!level || !level->design || !level->design->mesh) return;
  const map_data_t *map = &level->collision.m_MapData;
  const struct dd_map_design *d = level->design;
  ft_camera cam;
  game->engine->camera_get(&cam);
  float camera[4] = {cam.visible.x - MAP_EXPAND, cam.visible.y - MAP_EXPAND,
                     cam.visible.x + cam.visible.w - MAP_EXPAND, cam.visible.y + cam.visible.h - MAP_EXPAND};
  if (!(cam.visible.w > 0 && cam.visible.h > 0)) return;
  bool foreground = false, want_foreground = frame->pass == FT_PASS_LEVEL_FOREGROUND;
  // The frame interpolates from tick-1 to tick, like DDNet's envelope state
  // and this module's particles. At the initial snapshot the clock is zero.
  double time = fmax(0.0, (double)frame->tick - 1.0 + frame->alpha) * (1000.0 / GAME_TICK_SPEED);
  for (int g = 0; g < map->num_groups; ++g) {
    const map_group_t *group = &map->groups[g];
    design_uniforms_t u = {
        .tint = {1, 1, 1, 1}, .clip = {-1, -1, 1, 1}, .geometry = {0, 0, 1, 1}, .params = {1, 0, frame->state.lod_bias, 0}};
    float view[4], cull[4];
    dd_map_group_view(group, camera, view);
    memcpy(cull, view, sizeof(cull));
    u.view[0] = (view[0] + view[2]) * 0.5f;
    u.view[1] = (view[1] + view[3]) * 0.5f;
    u.view[2] = 2.f / (view[2] - view[0]);
    u.view[3] = 2.f / (view[3] - view[1]);
    bool clipped_out = false;
    if (group->use_clipping) {
      float clip[4] = {group->clip_x / 32.f, group->clip_y / 32.f, ((float)group->clip_x + group->clip_w) / 32.f,
                       ((float)group->clip_y + group->clip_h) / 32.f};
      clipped_out = group->clip_w <= 0 || group->clip_h <= 0 || !visible(clip, camera);
      for (int k = 0; k < 4; ++k) {
        int axis = k & 1;
        float fraction = (clip[k] - camera[axis]) / (camera[axis + 2] - camera[axis]);
        float pixels = axis ? cam.viewport.y : cam.viewport.x;
        if (pixels > 0) fraction = roundf(fraction * pixels) / pixels;
        u.clip[k] = fmaxf(-1, fminf(1, fraction * 2 - 1));
        cull[k] = view[axis] + (u.clip[k] + 1) * 0.5f * (view[axis + 2] - view[axis]);
      }
    }
    for (int j = 0; j < group->num_layers; ++j) {
      int li = group->start_layer + j;
      const map_layer_t *l = &map->layers[li];
      if (l->type == 2 && (l->tile_flags & 1)) foreground = true;
      if (foreground != want_foreground || clipped_out || (l->flags & 1 && !game->settings.map_detail)) continue;
      bool tiles = l->type == 2;
      if ((!tiles && l->type != 3) || l->tile_flags) continue;
      ft_texture *texture = tiles ? game->gfx.design_white_tiles : game->gfx.design_white;
      if (l->image >= 0 && l->image < map->num_images) {
        ft_texture *image = tiles ? d->images[l->image].tiles : d->images[l->image].quads;
        if (image) texture = image;
      }
      float color[4] = {1, 1, 1, 1};
      if (tiles) {
        dd_map_envelope(map, l->color_env, time + l->color_env_offset, 4, color);
        for (int c = 0; c < 4; ++c)
          color[c] *= l->color[c] / 255.f;
        if (color[3] <= 0) continue;
      }
      const design_layer_t *dl = &d->layers[li];
      for (size_t bi = dl->first; bi < dl->first + dl->count; ++bi) {
        const design_batch_t *b = &d->batches[bi];
        memcpy(u.tint, color, sizeof(color));
        memset(u.animation, 0, sizeof(u.animation));
        u.params[0] = 1;
        u.params[1] = 0;
        u.params[3] = tiles ? 1 : 0;
        if (!border_geometry(b->border, l, view, u.geometry)) continue;
        if (!tiles) {
          const map_quad_t *q = &l->quads[b->quad];
          float pos[4] = {0};
          dd_map_envelope(map, q->color_env, time + q->color_env_offset, 4, u.tint);
          if (u.tint[3] <= 0) continue;
          dd_map_envelope(map, q->pos_env, time + q->pos_env_offset, 3, pos);
          u.animation[0] = pos[0] / 32.f;
          u.animation[1] = pos[1] / 32.f;
          float angle = pos[2] * (3.14159265358979323846f / 180.f);
          u.params[0] = cosf(angle);
          u.params[1] = sinf(angle);
        }
        float bounds[4];
        for (int k = 0; k < 4; ++k)
          bounds[k] = b->bounds[k] * u.geometry[2 + (k & 1)] + u.geometry[k & 1] + u.animation[k & 1];
        if (!visible(bounds, cull)) continue;
        uint32_t count = b->count;
        // Consecutive visible tile chunks have identical state. Submit them as
        // one range (often an entire layer when zoomed out), while retaining
        // chunk-level culling when the view only covers part of the map.
        if (tiles && !b->border) {
          while (bi + 1 < dl->first + dl->count) {
            const design_batch_t *next = &d->batches[bi + 1];
            if (next->border || next->first != b->first + count || !visible(next->bounds, cull)) break;
            count += next->count;
            ++bi;
          }
        }
        game->engine->draw_mesh_range(tiles ? game->gfx.design_tiles : game->gfx.design_quads, want_foreground ? DD_Z_MAP : 0.f, d->mesh,
                                      b->first, count, &texture, 1, &u, sizeof(u));
      }
    }
  }
}
