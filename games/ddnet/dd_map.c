// Level rendering for the DDNet module.
//
// The tile layers become textures, the entities sheet becomes a texture array,
// and one quad drawn with this game's own shaders samples them. All of it used
// to sit in the engine's graphics backend, which meant the editor had to know
// DDNet's layer encoding to draw anything at all.

#include "dd_internal.h"

#include <stdlib.h>
#include <string.h>

// Must match data/games/ddnet/shaders/map.frag.glsl.
typedef struct {
  float transform[3]; // x, y, zoom
  float aspect;
  float lod_bias;
} dd_map_uniforms_t;

// Interleaves three single-channel tile planes into one RGBA texture, which is
// how the fragment shader reads game/front/tele together in one fetch.
static ft_texture *layer_texture(ft_game *game, uint8_t *const planes[3], uint32_t width, uint32_t height) {
  const size_t count = (size_t)width * height;
  uint8_t *rgba = calloc(count, 4);
  if (!rgba) return NULL;

  for (size_t i = 0; i < count; ++i) {
    rgba[i * 4 + 0] = planes[0] ? planes[0][i] : 0;
    rgba[i * 4 + 1] = planes[1] ? planes[1][i] : 0;
    rgba[i * 4 + 2] = planes[2] ? planes[2][i] : 0;
    rgba[i * 4 + 3] = 255;
  }

  const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                .pixels = rgba,
                                .width = width,
                                .height = height,
                                .layers = 1,
                                .format = FT_TEXTURE_RGBA8,
                                // Tile indices must not be blended together, so
                                // no mips and no filtering.
                                .mipmaps = false,
                                .linear_filter = false};
  ft_texture *texture = game->engine->texture_create(&desc);
  free(rgba);
  return texture;
}

// Slices the entities sheet into one array layer per tile index.
static ft_texture *entities_array(ft_game *game) {
  char path[1024];
  game->engine->resolve_data_path("textures/ddnet.png", path, sizeof(path));

  void *file = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &file, &size)) {
    dd_log(game, FT_LOG_ERROR, "Missing entities sheet '%s'.", path);
    return NULL;
  }

  int w = 0, h = 0, channels = 0;
  unsigned char *pixels = dd_decode_png(file, size, &w, &h, &channels);
  game->engine->free_file_data(file);
  if (!pixels) return NULL;

  const uint32_t tile_w = (uint32_t)w / 16;
  const uint32_t tile_h = (uint32_t)h / 16;
  const size_t layer_bytes = (size_t)tile_w * tile_h * 4;
  uint8_t *layers = malloc(layer_bytes * 256);
  if (!layers) {
    dd_free_png(pixels);
    return NULL;
  }

  for (uint32_t index = 0; index < 256; ++index) {
    const uint32_t tx = (index % 16) * tile_w;
    const uint32_t ty = (index / 16) * tile_h;
    for (uint32_t y = 0; y < tile_h; ++y) {
      memcpy(layers + layer_bytes * index + (size_t)y * tile_w * 4, pixels + (((size_t)(ty + y) * (uint32_t)w) + tx) * 4,
             (size_t)tile_w * 4);
    }
  }

  const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                .pixels = layers,
                                .width = tile_w,
                                .height = tile_h,
                                .layers = 256,
                                .format = FT_TEXTURE_RGBA8,
                                .mipmaps = true,
                                .linear_filter = true};
  ft_texture *texture = game->engine->texture_create(&desc);
  free(layers);
  dd_free_png(pixels);
  return texture;
}

static bool create_map_pipeline(ft_game *game) {
  if (game->gfx.map_pipeline) return true;

  char vert_path[1024], frag_path[1024];
  game->engine->resolve_data_path("shaders/map.vert.spv", vert_path, sizeof(vert_path));
  game->engine->resolve_data_path("shaders/map.frag.spv", frag_path, sizeof(frag_path));

  void *vert = NULL, *frag = NULL;
  size_t vert_size = 0, frag_size = 0;
  if (!game->engine->read_file(vert_path, &vert, &vert_size) || !game->engine->read_file(frag_path, &frag, &frag_size)) {
    dd_log(game, FT_LOG_ERROR, "Missing level shaders (%s / %s).", vert_path, frag_path);
    game->engine->free_file_data(vert);
    game->engine->free_file_data(frag);
    return false;
  }

  // A mesh pipeline: attributes come from the quad's own vertices.
  const ft_vertex_attr attrs[] = {
      {0, offsetof(ft_vertex, pos), FT_VERTEX_FLOAT2},
      {1, offsetof(ft_vertex, color), FT_VERTEX_FLOAT3},
      {2, offsetof(ft_vertex, uv), FT_VERTEX_FLOAT2},
  };
  const ft_pipeline_desc desc = {.struct_size = sizeof(desc),
                                 .vertex_spirv = vert,
                                 .vertex_spirv_size = vert_size,
                                 .fragment_spirv = frag,
                                 .fragment_spirv_size = frag_size,
                                 .instance_attrs = attrs,
                                 .instance_attr_count = 3,
                                 .instance_stride = 0, // mesh, not instanced
                                 .max_instances_per_frame = 0,
                                 .texture_count = 4, // entities array + three layer textures
                                 .alpha_blend = true};
  game->gfx.map_pipeline = game->engine->pipeline_create(&desc);
  game->engine->free_file_data(vert);
  game->engine->free_file_data(frag);

  if (!game->gfx.map_pipeline) {
    dd_log(game, FT_LOG_ERROR, "Could not create the level pipeline.");
    return false;
  }

  // A screen-filling quad. The uvs span -1..1 with Y inverted, which is the
  // convention map.frag.glsl samples in; 0..1 would render the level upside
  // down and at half scale.
  const ft_vertex vertices[4] = {
      {{-1.f, -1.f}, {1.f, 1.f, 1.f}, {-1.f, 1.f}},  // top left
      {{1.f, -1.f}, {1.f, 1.f, 1.f}, {1.f, 1.f}},    // top right
      {{1.f, 1.f}, {1.f, 1.f, 1.f}, {1.f, -1.f}},    // bottom right
      {{-1.f, 1.f}, {1.f, 1.f, 1.f}, {-1.f, -1.f}},  // bottom left
  };
  const uint32_t indices[6] = {0, 1, 2, 2, 3, 0};
  game->gfx.map_mesh = game->engine->mesh_create(vertices, 4, sizeof(ft_vertex), indices, 6);
  return game->gfx.map_mesh != NULL;
}

void dd_map_create(ft_game *game, ft_level *level) {
  if (game->headless || !level) return;
  dd_map_destroy(game, level);
  if (!create_map_pipeline(game)) return;

  if (!game->gfx.entities) game->gfx.entities = entities_array(game);

  const map_data_t *map = &level->collision.m_MapData;
  uint8_t *const planes[3][3] = {
      {map->game_layer.data, map->front_layer.data, map->tele_layer.type},
      {map->tune_layer.type, map->speedup_layer.type, map->switch_layer.type},
      {map->game_layer.flags, map->front_layer.flags, map->switch_layer.flags},
  };
  for (int i = 0; i < 3; ++i) level->layer_textures[i] = layer_texture(game, planes[i], map->width, map->height);
}

void dd_map_destroy(ft_game *game, ft_level *level) {
  if (!level) return;
  for (int i = 0; i < 3; ++i) {
    if (level->layer_textures[i]) game->engine->texture_destroy(level->layer_textures[i]);
    level->layer_textures[i] = NULL;
  }
}

void dd_map_render(ft_game *game, const ft_render_frame *frame) {
  const ft_level *level = frame->level;
  if (!level || !game->gfx.map_pipeline || !game->gfx.map_mesh) return;
  if (!level->layer_textures[0]) return;

  ft_camera camera;
  game->engine->camera_get(&camera);

  const map_data_t *map = &level->collision.m_MapData;
  const float map_ratio = (float)map->width / (float)map->height;
  const float zoom = 1.0f / (camera.zoom * (float)(map->width > map->height ? map->width : map->height) * 0.001f);

  // The camera arrives in world units; this shader wants the normalized
  // position it has always used.
  dd_map_uniforms_t ubo = {
      .transform = {camera.position.x / (float)map->width, camera.position.y / (float)map->height, zoom},
      .aspect = 1.0f / (camera.aspect / map_ratio),
      // The editor's own setting. Tiles are sampled by this game's shader now,
      // so this is the only place left that can honour it.
      .lod_bias = frame->state.lod_bias,
  };

  ft_texture *textures[4] = {game->gfx.entities, level->layer_textures[0], level->layer_textures[1], level->layer_textures[2]};
  game->engine->draw_mesh(game->gfx.map_pipeline, DD_Z_MAP, game->gfx.map_mesh, textures, 4, &ubo, sizeof(ubo));
}
