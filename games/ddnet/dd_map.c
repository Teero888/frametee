// Level rendering for the DDNet module.
//
// The physics layers become a page table and an atlas of pages, the entities
// sheet (dd_assets.c) becomes a texture array, and one quad drawn with this
// game's own shaders samples them. All of it used to sit in the engine's
// graphics backend, which meant the editor had to know DDNet's layer encoding
// to draw anything at all.

#include "dd_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Must match data/games/ddnet/shaders/map.frag.glsl.
typedef struct {
  float camera[4]; // the tile at the centre of the screen, and the centre within it
  float scale[4];  // tiles from the centre to the edge of the screen, lod bias
  float map[4];    // size in tiles, atlas columns, slots per set
} dd_map_uniforms_t;

// The physics layers are drawn from 32x32-tile pages (map.frag.glsl), so a map
// as wide as DDNet allows (100000 tiles) needs no texture wider than a page
// table, and its empty space costs a table entry a page. The page table and an
// atlas set stay within 4096 texels a side, which every Vulkan device takes.
#define PAGE 32
#define ATLAS_COLUMNS 128

// The two atlas texels of tile i (see map.frag.glsl).
static void tile_texels(const map_data_t *m, size_t i, uint8_t out[8]) {
  const uint8_t game_flags = m->game_layer.flags ? m->game_layer.flags[i] & 15 : 0;
  const uint8_t front_flags = m->front_layer.flags ? m->front_layer.flags[i] & 15 : 0;
  out[0] = m->game_layer.data ? m->game_layer.data[i] : 0;
  out[1] = m->front_layer.data ? m->front_layer.data[i] : 0;
  out[2] = m->tele_layer.type ? m->tele_layer.type[i] : 0;
  out[3] = m->tune_layer.type ? m->tune_layer.type[i] : 0;
  out[4] = m->speedup_layer.type ? m->speedup_layer.type[i] : 0;
  out[5] = m->switch_layer.type ? m->switch_layer.type[i] : 0;
  out[6] = (uint8_t)(game_flags | front_flags << 4);
  out[7] = m->switch_layer.flags ? m->switch_layer.flags[i] : 0;
}

// Whether page (px, py) is a single game tile and nothing else; `entry` gets
// its page table entry if so. An empty tile's flags draw nothing, so they do
// not count.
static bool uniform_page(const map_data_t *m, int px, int py, uint8_t entry[4]) {
  const int x0 = px * PAGE, y0 = py * PAGE;
  const int x1 = x0 + PAGE < m->width ? x0 + PAGE : m->width, y1 = y0 + PAGE < m->height ? y0 + PAGE : m->height;
  const size_t first = (size_t)y0 * m->width + x0;
  const uint8_t game = m->game_layer.data[first];
  const uint8_t flags = game && m->game_layer.flags ? m->game_layer.flags[first] & 15 : 0;
  const uint8_t *const others[] = {m->front_layer.data, m->tele_layer.type, m->tune_layer.type, m->speedup_layer.type,
                                   m->switch_layer.type};
  for (int y = y0; y < y1; ++y) {
    const size_t row = (size_t)y * m->width;
    for (int x = x0; x < x1; ++x) {
      const size_t i = row + x;
      if (m->game_layer.data[i] != game) return false;
      if (game && m->game_layer.flags && (m->game_layer.flags[i] & 15) != flags) return false;
      for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); ++k)
        if (others[k] && others[k][i]) return false;
    }
  }
  memcpy(entry, (const uint8_t[4]){game, flags, 0, 0}, 4);
  return true;
}

// Builds the page table and the atlas (map.frag.glsl) of a level's physics layers.
static bool create_entity_pages(ft_game *game, ft_level *level) {
  const map_data_t *m = &level->map;
  if (!m->game_layer.data || m->width <= 0 || m->height <= 0) return false;
  const uint32_t pages_x = ((uint32_t)m->width + PAGE - 1) / PAGE, pages_y = ((uint32_t)m->height + PAGE - 1) / PAGE;
  if (pages_x > ATLAS_COLUMNS * PAGE || pages_y > ATLAS_COLUMNS * PAGE) {
    dd_log(game, FT_LOG_ERROR, "The map is too large (%dx%d) to draw its entities.", m->width, m->height);
    return false;
  }
  uint8_t *table = calloc((size_t)pages_x * pages_y, 4);
  if (!table) return false;
  uint32_t slots = 0;
  for (uint32_t py = 0; py < pages_y; ++py)
    for (uint32_t px = 0; px < pages_x; ++px) {
      uint8_t *entry = table + ((size_t)py * pages_x + px) * 4;
      if (uniform_page(m, (int)px, (int)py, entry)) continue;
      memcpy(entry, (const uint8_t[4]){slots & 255, slots >> 8 & 255, slots >> 16 & 255, 1}, 4);
      ++slots;
    }

  // The slots fill sets of up to ATLAS_COLUMNS x ATLAS_COLUMNS pages, square
  // while there is only one, so a small map gets a small atlas.
  const uint32_t used = slots ? slots : 1;
  uint32_t columns = ATLAS_COLUMNS, rows = ATLAS_COLUMNS;
  if (used < ATLAS_COLUMNS * ATLAS_COLUMNS) {
    columns = (uint32_t)ceil(sqrt((double)used));
    rows = (used + columns - 1) / columns;
  }
  const uint32_t per_set = columns * rows, sets = (used + per_set - 1) / per_set;
  const uint32_t atlas_w = columns * PAGE, atlas_h = rows * PAGE;
  const size_t layer_bytes = (size_t)atlas_w * atlas_h * 4;
  if (slots >= 1u << 24 || sets * 2 > 2048) {
    dd_log(game, FT_LOG_ERROR, "The map has too many tiles (%u pages) to draw its entities.", slots);
    free(table);
    return false;
  }
  uint8_t *atlas = calloc(layer_bytes, (size_t)sets * 2);
  if (!atlas) {
    free(table);
    return false;
  }
  for (uint32_t py = 0; py < pages_y; ++py)
    for (uint32_t px = 0; px < pages_x; ++px) {
      const uint8_t *entry = table + ((size_t)py * pages_x + px) * 4;
      if (!entry[3]) continue;
      const uint32_t slot = entry[0] | (uint32_t)entry[1] << 8 | (uint32_t)entry[2] << 16;
      const uint32_t index = slot % per_set;
      uint8_t *ids = atlas + layer_bytes * (slot / per_set) * 2, *more = ids + layer_bytes;
      const uint32_t ax = index % columns * PAGE, ay = index / columns * PAGE;
      const int x0 = (int)px * PAGE, y0 = (int)py * PAGE;
      for (int y = y0; y < y0 + PAGE && y < m->height; ++y)
        for (int x = x0; x < x0 + PAGE && x < m->width; ++x) {
          uint8_t texels[8];
          tile_texels(m, (size_t)y * m->width + x, texels);
          const size_t at = ((size_t)(ay + (uint32_t)(y - y0)) * atlas_w + ax + (uint32_t)(x - x0)) * 4;
          memcpy(ids + at, texels, 4);
          memcpy(more + at, texels + 4, 4);
        }
    }

  // Tile indices must not be blended together, so no mips and no filtering.
  const ft_texture_desc table_desc = {.struct_size = sizeof(table_desc),
                                      .pixels = table,
                                      .width = pages_x,
                                      .height = pages_y,
                                      .layers = 1,
                                      .format = FT_TEXTURE_RGBA8};
  const ft_texture_desc atlas_desc = {.struct_size = sizeof(atlas_desc),
                                      .pixels = atlas,
                                      .width = atlas_w,
                                      .height = atlas_h,
                                      .layers = sets * 2,
                                      .format = FT_TEXTURE_RGBA8};
  level->entity_pages = game->engine->texture_create(&table_desc);
  level->entity_atlas = game->engine->texture_create(&atlas_desc);
  free(table);
  free(atlas);
  level->entity_atlas_columns = columns;
  level->entity_atlas_per_set = per_set;
  if (!level->entity_pages || !level->entity_atlas) {
    dd_log(game, FT_LOG_ERROR, "Could not upload the map's entities (%u pages).", slots);
    return false;
  }
  return true;
}

// Slices an entities sheet into one array layer per tile index.
ft_texture *dd_map_entities_texture(ft_game *game, const uint8_t *pixels, int width, int height) {
  const uint32_t tile_w = (uint32_t)width / 16;
  const uint32_t tile_h = (uint32_t)height / 16;
  if (!pixels || tile_w == 0 || tile_h == 0) return NULL;
  const size_t layer_bytes = (size_t)tile_w * tile_h * 4;
  uint8_t *layers = malloc(layer_bytes * 256);
  if (!layers) return NULL;

  for (uint32_t index = 0; index < 256; ++index) {
    const uint32_t tx = (index % 16) * tile_w;
    const uint32_t ty = (index / 16) * tile_h;
    for (uint32_t y = 0; y < tile_h; ++y) {
      memcpy(layers + layer_bytes * index + (size_t)y * tile_w * 4, pixels + (((size_t)(ty + y) * (uint32_t)width) + tx) * 4,
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
  return texture;
}

static bool create_map_pipeline(ft_game *game) {
  if (game->gfx.map_pipeline) return true;

  char vert_path[1024], frag_path[1024];
  dd_data_path(game->engine, "shaders/map.vert.spv", vert_path, sizeof(vert_path));
  dd_data_path(game->engine, "shaders/map.frag.spv", frag_path, sizeof(frag_path));

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
                                 .texture_count = 3, // entities array, page table, atlas
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
      {{-1.f, -1.f}, {1.f, 1.f, 1.f}, {-1.f, 1.f}}, // top left
      {{1.f, -1.f}, {1.f, 1.f, 1.f}, {1.f, 1.f}},   // top right
      {{1.f, 1.f}, {1.f, 1.f, 1.f}, {1.f, -1.f}},   // bottom right
      {{-1.f, 1.f}, {1.f, 1.f, 1.f}, {-1.f, -1.f}}, // bottom left
  };
  const uint32_t indices[6] = {0, 1, 2, 2, 3, 0};
  game->gfx.map_mesh = game->engine->mesh_create(vertices, 4, sizeof(ft_vertex), indices, 6);
  return game->gfx.map_mesh != NULL;
}

void dd_map_create(ft_game *game, ft_level *level) {
  if (game->headless || !level) return;
  dd_map_destroy(game, level);
  dd_map_design_create(game, level);
  dd_map_overlays_create(level);
  if (!create_map_pipeline(game)) return;

  if (!game->gfx.entities) dd_assets_load_type(game, DD_ASSET_ENTITIES);

  create_entity_pages(game, level);
}

void dd_map_destroy(ft_game *game, ft_level *level) {
  if (!level) return;
  dd_map_design_destroy(game, level);
  if (level->entity_pages) game->engine->texture_destroy(level->entity_pages);
  if (level->entity_atlas) game->engine->texture_destroy(level->entity_atlas);
  level->entity_pages = level->entity_atlas = NULL;
  dd_map_overlays_destroy(level);
}

void dd_map_render(ft_game *game, const ft_render_frame *frame) {
  const ft_level *level = frame->level;
  if (!level || !game->gfx.map_pipeline || !game->gfx.map_mesh) return;
  if (!level->entity_pages || !level->entity_atlas) return;

  ft_camera camera;
  game->engine->camera_get(&camera);
  if (!(camera.visible.w > 0 && camera.visible.h > 0)) return;

  // The screen's centre as the map design takes it (dd_map_group_view), so
  // the two line up.
  const float left = camera.visible.x, top = camera.visible.y;
  const float right = left + camera.visible.w, bottom = top + camera.visible.h;
  const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
  const float tile_x = floorf(cx), tile_y = floorf(cy);
  const map_data_t *map = &level->map;
  dd_map_uniforms_t ubo = {
      .camera = {tile_x, tile_y, cx - tile_x, cy - tile_y},
      // The editor's own setting. Tiles are sampled by this game's shader now,
      // so this is the only place left that can honour it.
      .scale = {(right - left) * 0.5f, (bottom - top) * 0.5f, frame->state.lod_bias, 0.f},
      .map = {(float)map->width, (float)map->height, (float)level->entity_atlas_columns, (float)level->entity_atlas_per_set},
  };

  ft_texture *textures[3] = {game->gfx.entities, level->entity_pages, level->entity_atlas};
  game->engine->draw_mesh(game->gfx.map_pipeline, DD_Z_MAP, game->gfx.map_mesh, textures, 3, &ubo, sizeof(ubo));
}
