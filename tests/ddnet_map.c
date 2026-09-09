/* Map-format and rendering regression tests. Fixtures are constructed here so
 * these tests do not depend on an untracked DDNet checkout or a GPU. */
#include "dd_internal.h"
#include "dd_map_math.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
  unsigned char data[32768];
  size_t size;
} bytes_t;
static void put(bytes_t *b, uint32_t v) {
  assert(b->size + 4 <= sizeof(b->data));
  for (int i = 0; i < 4; ++i)
    b->data[b->size++] = (unsigned char)(v >> (i * 8));
}
static void ints(bytes_t *b, const int *p, int n) {
  for (int i = 0; i < n; ++i)
    put(b, (uint32_t)p[i]);
}
static void item_add(bytes_t *items, bytes_t *offsets, int type, int id, const int *p, int n) {
  put(offsets, (uint32_t)items->size);
  put(items, (uint32_t)type << 16 | (uint32_t)id);
  put(items, (uint32_t)n * 4);
  ints(items, p, n);
}
static void raw_add(bytes_t *raw, bytes_t *offsets, bytes_t *sizes, const void *data, size_t size, int version) {
  put(offsets, (uint32_t)raw->size);
  put(sizes, (uint32_t)size);
  if (version == 4) {
    uLongf n = sizeof(raw->data) - raw->size;
    assert(compress(raw->data + raw->size, &n, data, (uLong)size) == Z_OK);
    raw->size += n;
  } else {
    memcpy(raw->data + raw->size, data, size);
    raw->size += size;
  }
}
static unsigned char *fixture(int version, int packed, int legacy, int upstream, size_t *size) {
  bytes_t items = {0}, offsets = {0}, raw = {0}, roff = {0}, sizes = {0};
  unsigned char tiles[16] = {1, 0, 0, 0, 2, 1, 0, 0, 3, 2, 0, 0, 4, 8, 0, 0};
  unsigned char run[4] = {7, 9, 3, 0}, pixels[4] = {10, 20, 30, 255};
  raw_add(&raw, &roff, &sizes, "test", 5, version);
  raw_add(&raw, &roff, &sizes, pixels, 4, version);
  raw_add(&raw, &roff, &sizes, packed ? run : tiles, packed ? 4 : 16, version);
  raw_add(&raw, &roff, &sizes, tiles, 16, version);
  bytes_t quad = {0};
  int q[38] = {0, 0, 32768, 0, 0, 32768, 32768, 32768, 16384, 16384, 255, 0, 0, 255, 0, 255, 0, 128, 0,
               0, 255, 64, 255, 255, 255, 0, 0, 0, 2048, 0, 0, 1024, 2048, 1024, 0, 75, 0, -25};
  ints(&quad, q, 38);
  raw_add(&raw, &roff, &sizes, quad.data, quad.size, version);
  int info[6] = {1, -1, -1, -1, -1, 5};
  raw_add(&raw, &roff, &sizes, "tune gravity 0.5\0sv_test 1", 27, version);
  item_add(&items, &offsets, 1, 0, info, 6);
  int im[6] = {1, 1, 1, 0, 0, 1};
  item_add(&items, &offsets, 2, 0, im, 6);
  int env[13] = {upstream ? 3 : 2, 4, 0, 2};
  env[12] = 1;
  item_add(&items, &offsets, 3, 0, env, 13);
  int group[15] = {3, 10, 20, 25, 50, 0, 4, 1, 32, 64, 96, 128};
  item_add(&items, &offsets, 4, 0, group, 15);
  int layer[23] = {0, 2, 1, packed ? 4 : legacy ? 2
                                                : 3,
                   2, 2, 0, 100, 150, 200, 128, 0, 25, 0, 2};
  item_add(&items, &offsets, 5, 0, layer, legacy ? 15 : 23);
  layer[2] = 0;
  layer[6] = 1;
  item_add(&items, &offsets, 5, 1, layer, legacy ? 15 : 23);
  layer[3] = legacy ? 2 : 3;
  layer[6] = 8;
  layer[legacy ? 17 : 20] = 3;
  item_add(&items, &offsets, 5, 2, layer, legacy ? 20 : 23);
  int qlayer[7] = {0, 3, 0, 1, 1, 4, -1};
  item_add(&items, &offsets, 5, 3, qlayer, 7);
  int points[44] = {0};
  int stride = upstream ? 22 : 6;
  points[1] = 5;
  points[2] = 1024;
  points[3] = 512;
  points[4] = 256;
  points[5] = 1024;
  points[stride] = 1000;
  points[stride + 1] = 1;
  points[stride + 2] = 2048;
  points[stride + 3] = 1024;
  points[stride + 4] = 512;
  points[stride + 5] = 0;
  item_add(&items, &offsets, 6, 0, points, stride * 2);
  if (!upstream) {
    int tangents[32] = {0};
    tangents[8] = 250;
    tangents[16] = -250;
    item_add(&items, &offsets, 0x8001, 0, tangents, 32);
    int uuid[4] = {(int)0x3ab4f84d, (int)0xd9cc3c78, (int)0xb5850d6c, (int)0xcdb2e25c};
    item_add(&items, &offsets, 0xffff, 0x8001, uuid, 4);
  }
  int nt = upstream ? 6 : 8, ni = (int)(offsets.size / 4), nr = (int)(roff.size / 4);
  bytes_t file = {0};
  put(&file, 0x41544144);
  put(&file, (uint32_t)version);
  put(&file, 0);
  put(&file, 0);
  put(&file, (uint32_t)nt);
  put(&file, (uint32_t)ni);
  put(&file, (uint32_t)nr);
  put(&file, (uint32_t)items.size);
  put(&file, (uint32_t)raw.size);
  int types[24] = {1, 0, 1, 2, 1, 1, 3, 2, 1, 4, 3, 1, 5, 4, 4, 6, 8, 1, 0x8001, 9, 1, 0xffff, 10, 1};
  ints(&file, types, nt * 3);
#define ADD(b)                                         \
  do {                                                 \
    memcpy(file.data + file.size, (b).data, (b).size); \
    file.size += (b).size;                             \
  } while (0)
  ADD(offsets);
  ADD(roff);
  if (version == 4) ADD(sizes);
  ADD(items);
  ADD(raw);
#undef ADD
  unsigned char *result = malloc(file.size);
  assert(result);
  memcpy(result, file.data, file.size);
  *size = file.size;
  return result;
}

static void test_loader(void) {
  for (int version = 3; version <= 4; ++version)
    for (int packed = 0; packed < 2; ++packed)
      for (int legacy = 0; legacy < 2; ++legacy)
        for (int upstream = 0; upstream < 2; ++upstream) {
          if (packed && legacy) continue;
          size_t size;
          unsigned char *data = fixture(version, packed, legacy, upstream, &size);
          map_data_t m = load_map_from_memory(data, size);
          assert(m.game_layer.data && m.width == 2 && m.height == 2);
          assert(m.game_layer.data[0] == (packed ? 7 : 1));
          assert(m.game_layer.data[3] == (packed ? 7 : 4));
          assert(m.layers[0].tiles[3].index == (packed ? 7 : 4));
          assert(m.front_layer.flags[3] == 8);
          assert(m.num_images == 1 && m.images[0].pixels[2] == 30 && !strcmp(m.images[0].name, "test"));
          assert(m.num_groups == 1 && m.groups[0].parallax_x == 100 && m.groups[0].offset_x == 0 && !m.groups[0].use_clipping);
          assert(m.layers[3].quads[0].colors[2][3] == 64 && m.layers[3].quads[0].texcoords[1][0] == 2048);
          assert(m.num_envelopes == 1 && m.num_env_points == 2 && m.env_bezier);
          assert(m.env_points[1].values[0] == 2048);
          assert(m.num_settings == 2 && !strcmp(m.settings[0], "tune gravity 0.5"));
          free_map_data(&m);
          assert(!m.layers && !m._map_file_data);
        }
  size_t size;
  unsigned char *data = fixture(4, 0, 0, 0, &size);
  /* Every truncated prefix must fail without taking ownership or leaking. */
  for (size_t n = 0; n < size; ++n) {
    map_data_t m = load_map_from_memory(data, n);
    assert(!m.game_layer.data);
    free_map_data(&m);
  }
  /* Corrupt metadata offsets and compressed streams. */
  for (int i = 4; i < 36; i += 4) {
    unsigned char saved[4];
    memcpy(saved, data + i, 4);
    memset(data + i, 255, 4);
    map_data_t m = load_map_from_memory(data, size);
    if (m.game_layer.data) {
      m._map_file_data = NULL;
      free_map_data(&m);
    }
    memcpy(data + i, saved, 4);
  }
  data[size - 1] ^= 0xff;
  map_data_t m = load_map_from_memory(data, size);
  assert(!m.game_layer.data);
  free(data);
}

static void near(float a, float b) { assert(fabsf(a - b) < 0.0001f); }
static void test_envelopes(void) {
  map_env_point_t p[2] = {{.time = 0, .values = {0, 1024, 2048, 4096}}, {.time = 1000, .values = {1024, 2048, 4096, 0}}};
  map_envelope_t e = {4, 0, 2, 1};
  map_data_t m = {.num_envelopes = 1, .envelopes = &e, .num_env_points = 2, .env_points = p};
  float out[4] = {7, 7, 7, 7};
  dd_map_envelope(&m, -1, 500, 4, out);
  near(out[0], 7);
  const float expected[5] = {0, 0.25f, 0.015625f, 0.578125f, 0.15625f};
  for (int curve = 0; curve < 5; ++curve) {
    p[0].curve = curve;
    dd_map_envelope(&m, 0, 250, 4, out);
    near(out[0], expected[curve]);
  }
  p[0].curve = 1;
  dd_map_envelope(&m, 0, 1250, 4, out);
  near(out[0], 0.25f);
  dd_map_envelope(&m, 0, -250, 4, out);
  near(out[0], 1); // upstream negative remainder falls back to last
  dd_map_envelope(&m, 0, 1000, 4, out);
  near(out[0], 0);
  p[0].curve = 5;
  dd_map_envelope(&m, 0, 500, 4, out);
  near(out[0], 0.5f); // no tangents: linear fallback
  m.env_bezier = 1;
  p[0].out_dx[0] = 250;
  p[1].in_dx[0] = -250;
  p[0].out_dy[0] = 1024;
  p[1].in_dy[0] = 0;
  dd_map_envelope(&m, 0, 500, 4, out);
  near(out[0], 0.875f);
  e.channels = 1;
  out[1] = 42;
  dd_map_envelope(&m, 0, 500, 4, out);
  near(out[1], 42);
  e.num_points = 1;
  dd_map_envelope(&m, 0, 500, 4, out);
  near(out[0], 0);
  float camera[4] = {10, 20, 110, 70}, view[4];
  map_group_t g = {.parallax_x = 100, .parallax_y = 100};
  dd_map_group_view(&g, camera, view);
  for (int i = 0; i < 4; ++i)
    near(view[i], camera[i]);
  g.parallax_x = g.parallax_y = 0;
  dd_map_group_view(&g, camera, view);
  near(view[0], -1500.f / 64);
  near(view[2], 1500.f / 64);
  g.offset_x = 32;
  g.offset_y = 64;
  dd_map_group_view(&g, camera, view);
  near((view[0] + view[2]) / 2, 1);
  near((view[1] + view[3]) / 2, 2);
}

/* Mock engine verifies resource ownership, culling and submitted transform
 * data. Shader behavior is additionally exercised by the viewport captures. */
static size_t textures, meshes, pipelines, draws, created, vertices, indices;
static ft_camera camera;
static float last_uniforms[24], last_z;
static ft_texture *texture_create(const ft_texture_desc *d) {
  assert(d->pixels && d->width && d->height);
  ++textures;
  ++created;
  return (ft_texture *)malloc(1);
}
static void texture_destroy(ft_texture *p) {
  assert(textures);
  --textures;
  free(p);
}
static ft_pipeline *pipeline_create(const ft_pipeline_desc *d) {
  assert(d->instance_stride == 0);
  ++pipelines;
  return (ft_pipeline *)malloc(1);
}
static ft_mesh *mesh_create(const void *v, uint32_t n, uint32_t stride, const uint32_t *idx, uint32_t count) {
  assert(v && stride == sizeof(ft_vertex) && n && count % 6 == 0);
  for (uint32_t i = 0; i < count; ++i)
    assert(idx[i] < n);
  vertices = n;
  indices = count;
  ++meshes;
  ++created;
  return (ft_mesh *)malloc(1);
}
static void mesh_destroy(ft_mesh *m) {
  assert(meshes);
  --meshes;
  free(m);
}
static void camera_get(ft_camera *out) { *out = camera; }
static size_t resolve(const char *rel, char *out, size_t size) {
  snprintf(out, size, "%s", rel);
  return strlen(out);
}
static bool read_file(const char *p, void **out, size_t *size) {
  *out = calloc(1, 4);
  *size = 4;
  return true;
}
static void draw_range(ft_pipeline *p, float z, ft_mesh *m, uint32_t first, uint32_t count, ft_texture *const *t, uint32_t n, const void *u,
                       size_t size) {
  assert(p && m && n == 1 && t[0] && first + count <= indices && size == sizeof(last_uniforms));
  memcpy(last_uniforms, u, size);
  last_z = z;
  ++draws;
}
void dd_log(ft_game *g, ft_log_level level, const char *fmt, ...) {
  (void)g;
  (void)level;
  (void)fmt;
}
unsigned char *dd_decode_png(const void *p, size_t n, int *w, int *h, int *channels) { return NULL; }
void dd_free_png(unsigned char *p) { free(p); }

static void test_render(void) {
  ft_engine_api api = {.texture_create = texture_create,
                       .texture_destroy = texture_destroy,
                       .pipeline_create = pipeline_create,
                       .mesh_create = mesh_create,
                       .mesh_destroy = mesh_destroy,
                       .camera_get = camera_get,
                       .resolve_data_path = resolve,
                       .read_file = read_file,
                       .free_file_data = free,
                       .draw_mesh_range = draw_range};
  ft_game game = {.engine = &api};
  game.settings.map_detail = true;
  ft_level level = {0};
  size_t size;
  unsigned char *data = fixture(4, 0, 0, 0, &size);
  level.collision.m_MapData = load_map_from_memory(data, size);
  assert(level.collision.m_MapData.game_layer.data);
  camera.visible = (ft_rect){-1 + MAP_EXPAND, -1 + MAP_EXPAND, 4, 4};
  camera.viewport = (ft_vec2){400, 400};
  ft_render_frame frame = {.level = &level, .pass = FT_PASS_LEVEL_BACKGROUND, .tick = 25};
  dd_map_design_create(&game, &level);
  assert(level.design && meshes == 1 && vertices > 0);
  size_t before = created;
  dd_map_design_render(&game, &frame);
  assert(draws > 0 && last_z == 0 && created == before);
  near(last_uniforms[0], 1);
  near(last_uniforms[1], 1);
  near(last_uniforms[2], 0.5f);
  draws = 0;
  game.settings.map_detail = false;
  dd_map_design_render(&game, &frame);
  assert(!draws);
  frame.pass = FT_PASS_LEVEL_FOREGROUND;
  dd_map_design_render(&game, &frame);
  assert(draws == 1 && last_z == DD_Z_MAP);
  map_group_t *group = &level.collision.m_MapData.groups[0];
  group->use_clipping = 1;
  group->clip_x = group->clip_y = 32;
  group->clip_w = group->clip_h = 32;
  draws = 0;
  dd_map_design_render(&game, &frame);
  assert(draws == 1);
  near(last_uniforms[8], 0);
  near(last_uniforms[9], 0);
  near(last_uniforms[10], 0.5f);
  near(last_uniforms[11], 0.5f);
  group->clip_w = 0;
  draws = 0;
  dd_map_design_render(&game, &frame);
  assert(!draws);
  group->use_clipping = 0;
  camera.visible = (ft_rect){100 + MAP_EXPAND, 100 + MAP_EXPAND, 4, 4};
  draws = 0;
  dd_map_design_render(&game, &frame);
  assert(!draws);
  dd_map_design_destroy(&game, &level);
  assert(!level.design && !meshes);
  // A zoomed-out layer spans three chunks, but shares one contiguous draw.
  map_layer_t *tiles = &level.collision.m_MapData.layers[0];
  free(tiles->tiles);
  tiles->width = 130;
  tiles->height = 2;
  tiles->flags = 0;
  tiles->tiles = calloc(260, sizeof(*tiles->tiles));
  assert(tiles->tiles);
  for (int i = 0; i < 260; ++i)
    tiles->tiles[i].index = 1;
  camera.visible = (ft_rect){0 + MAP_EXPAND, 0 + MAP_EXPAND, 130, 2};
  frame.pass = FT_PASS_LEVEL_BACKGROUND;
  dd_map_design_create(&game, &level);
  draws = 0;
  dd_map_design_render(&game, &frame);
  assert(draws == 1);
  dd_map_design_destroy(&game, &level);
  // Level reloads must release all per-level resources, retaining only the
  // shared pipelines and white fallback textures.
  for (int i = 0; i < 80; ++i) {
    dd_map_design_create(&game, &level);
    assert(level.design && meshes == 1);
    dd_map_design_destroy(&game, &level);
    assert(meshes == 0 && textures == 2);
  }
  free_map_data(&level.collision.m_MapData);
  texture_destroy(game.gfx.design_white);
  texture_destroy(game.gfx.design_white_tiles);
  free(game.gfx.design_tiles);
  free(game.gfx.design_quads);
  pipelines -= 2;
  assert(!textures && !meshes && !pipelines);
}
int main(void) {
  test_loader();
  test_envelopes();
  test_render();
  puts("DDNet map tests passed");
  return 0;
}
