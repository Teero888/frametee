// DDNet's viewport text renderer.
//
// FreeType is owned by the game module. Glyphs are baked into an engine atlas
// once, then every character is submitted as a normal world-space sprite. The
// resulting quads are part of the ordinary DDNet render queue: they use the
// same camera, z sorting, viewport clipping and offscreen target as the map.

#include "dd_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DD_TEXT_BAKED_SIZE 64u
// DDNet bakes a glyph at its on-screen pixel size and outlines it by 1, 2 or 4
// pixels (AdjustOutlineThicknessToFontSize, stepping at 18 and 48), so its ring
// is near constant on screen. One sheet baked at 64 cannot follow that with a
// single ratio, so the ring is baked at several and the draw picks the one
// whose scaled width lands on DDNet's pixel count.
static const uint32_t k_outline_thickness[DD_TEXT_OUTLINE_VARIANTS] = {3u, 4u, 6u, 9u};
#define DD_TEXT_MAX_OUTLINE 9u
#define DD_TEXT_PADDING (DD_TEXT_MAX_OUTLINE + 1u)
#define DD_TEXT_SHEET_SIZE 2048u
#define DD_TEXT_MAX_GLYPHS 256u
// One sprite per glyph for the fill and one per outline variant. Each is its
// own array layer, which Vulkan guarantees 2048 of.
#define DD_TEXT_MAX_SPRITES (DD_TEXT_MAX_GLYPHS * (1u + DD_TEXT_OUTLINE_VARIANTS))
#define DD_ENTITY_CELL_SIZE 64u
#define DD_ENTITY_SHEET_SIZE 1024u
#define DD_ENTITY_NUMBER_COUNT 256u

static uint32_t utf8_next(const char **cursor) {
  const unsigned char *text = (const unsigned char *)*cursor;
  const unsigned char first = *text++;
  if (first < 0x80u) {
    *cursor = (const char *)text;
    return first;
  }

  uint32_t value;
  int continuation;
  if ((first & 0xe0u) == 0xc0u) {
    value = first & 0x1fu;
    continuation = 1;
  } else if ((first & 0xf0u) == 0xe0u) {
    value = first & 0x0fu;
    continuation = 2;
  } else if ((first & 0xf8u) == 0xf0u) {
    value = first & 0x07u;
    continuation = 3;
  } else {
    *cursor = (const char *)text;
    return 0xfffdu;
  }

  for (int i = 0; i < continuation; ++i) {
    if (text[i] == '\0' || (text[i] & 0xc0u) != 0x80u) {
      *cursor = (const char *)text;
      return 0xfffdu;
    }
    value = (value << 6u) | (text[i] & 0x3fu);
  }
  text += continuation;
  *cursor = (const char *)text;

  if ((continuation == 1 && value < 0x80u) || (continuation == 2 && value < 0x800u) ||
      (continuation == 3 && value < 0x10000u) || value > 0x10ffffu ||
      (value >= 0xd800u && value <= 0xdfffu))
    return 0xfffdu;
  return value;
}

static const dd_text_glyph_t *find_exact_glyph(const dd_text_renderer_t *text, uint32_t codepoint) {
  uint32_t first = 0;
  uint32_t count = text->glyph_count;
  while (count > 0) {
    const uint32_t step = count / 2u;
    const uint32_t index = first + step;
    if (text->glyphs[index].codepoint < codepoint) {
      first = index + 1u;
      count -= step + 1u;
    } else {
      count = step;
    }
  }
  if (first < text->glyph_count && text->glyphs[first].codepoint == codepoint)
    return &text->glyphs[first];
  return NULL;
}

static const dd_text_glyph_t *find_glyph(const dd_text_renderer_t *text, uint32_t codepoint) {
  const dd_text_glyph_t *glyph = find_exact_glyph(text, codepoint);
  if (!glyph) glyph = find_exact_glyph(text, 0xfffdu);
  if (!glyph) glyph = find_exact_glyph(text, (uint32_t)'?');
  return glyph;
}

// CTextRender::Grow: the glyph dilated by `thickness`, which is the shape the
// outline pass draws underneath the fill.
static uint8_t outline_sample(const uint8_t *fill, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                              uint32_t thickness) {
  int best = fill[(size_t)y * width + x];
  for (int dy = -(int)thickness; dy <= (int)thickness; ++dy) {
    for (int dx = -(int)thickness; dx <= (int)thickness; ++dx) {
      const int px = (int)x + dx;
      const int py = (int)y + dy;
      if (px < 0 || py < 0 || px >= (int)width || py >= (int)height) continue;
      float mask = 1.f - fminf(fmaxf(sqrtf((float)(dx * dx + dy * dy)) - (float)thickness, 0.f), 1.f);
      const int candidate = (int)((float)fill[(size_t)py * width + (uint32_t)px] * mask);
      if (candidate > best) best = candidate;
    }
  }
  return (uint8_t)best;
}

static bool pack_rect(uint32_t width, uint32_t height, uint32_t *x, uint32_t *y, uint32_t *row_height,
                      ft_sprite_rect *out) {
  if (width > DD_TEXT_SHEET_SIZE - 2u || height > DD_TEXT_SHEET_SIZE - 2u) return false;
  if (*x + width + 1u > DD_TEXT_SHEET_SIZE) {
    *x = 1u;
    *y += *row_height + 1u;
    *row_height = 0u;
  }
  if (*y + height + 1u > DD_TEXT_SHEET_SIZE) return false;
  *out = (ft_sprite_rect){*x, *y, width, height};
  *x += width + 1u;
  if (height > *row_height) *row_height = height;
  return true;
}

// Blits one coverage mask into the sheet as white with that coverage in alpha.
// Colour comes from the draw, exactly as it does for DDNet's two textures.
static void blit_mask(uint8_t *sheet, const ft_sprite_rect *rect, const uint8_t *mask, uint32_t width, uint32_t height) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t target = ((size_t)(rect->y + y) * DD_TEXT_SHEET_SIZE + rect->x + x) * 4u;
      sheet[target + 0u] = 255u;
      sheet[target + 1u] = 255u;
      sheet[target + 2u] = 255u;
      sheet[target + 3u] = mask[(size_t)y * width + x];
    }
  }
}

static bool add_glyph(dd_text_renderer_t *text, FT_Face face, uint32_t codepoint, uint8_t *sheet,
                      ft_sprite_rect *rects, uint32_t *sprite_count, uint32_t *pack_x, uint32_t *pack_y,
                      uint32_t *row_height) {
  const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
  if (glyph_index == 0u) return true;
  if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP) != 0) return true;

  const FT_GlyphSlot slot = face->glyph;
  const FT_Bitmap *bitmap = &slot->bitmap;
  const bool visible = bitmap->width > 0u && bitmap->rows > 0u;
  if (visible && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) return true;

  const uint32_t pad = visible ? DD_TEXT_PADDING : 0u;
  const uint32_t width = visible ? bitmap->width + pad * 2u : 1u;
  const uint32_t height = visible ? bitmap->rows + pad * 2u : 1u;
  // A glyph needs its fill sprite and one sprite per outline variant, and they
  // are only useful together, so they are reserved as a group.
  const uint32_t needed = 1u + DD_TEXT_OUTLINE_VARIANTS;
  if (text->glyph_count >= DD_TEXT_MAX_GLYPHS || *sprite_count + needed > DD_TEXT_MAX_SPRITES) return false;

  dd_text_glyph_t *glyph = &text->glyphs[text->glyph_count];
  *glyph = (dd_text_glyph_t){.codepoint = codepoint,
                            .glyph_index = glyph_index,
                            .width = visible ? width : 0u,
                            .height = visible ? height : 0u,
                            .offset_x = (float)(slot->metrics.horiBearingX >> 6),
                            .offset_y = (float)-((slot->metrics.height >> 6) - (slot->metrics.horiBearingY >> 6)),
                            .advance_x = (float)(slot->advance.x >> 6),
                            .visible = visible};

  glyph->sprite_index = *sprite_count;
  if (!pack_rect(width, height, pack_x, pack_y, row_height, &rects[(*sprite_count)++])) return false;
  for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
    glyph->outline_sprite[v] = *sprite_count;
    if (!pack_rect(width, height, pack_x, pack_y, row_height, &rects[(*sprite_count)++])) return false;
  }

  if (visible) {
    const size_t pixel_count = (size_t)width * height;
    uint8_t *fill = calloc(pixel_count, 1u);
    uint8_t *outline = malloc(pixel_count);
    if (!fill || !outline) {
      free(fill);
      free(outline);
      return false;
    }

    for (uint32_t y = 0; y < bitmap->rows; ++y) {
      const uint8_t *source = bitmap->pitch >= 0
                                  ? bitmap->buffer + (size_t)y * (uint32_t)bitmap->pitch
                                  : bitmap->buffer + (size_t)(bitmap->rows - 1u - y) * (uint32_t)(-bitmap->pitch);
      memcpy(fill + (size_t)(y + pad) * width + pad, source, bitmap->width);
    }

    blit_mask(sheet, &rects[glyph->sprite_index], fill, width, height);
    for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
          outline[(size_t)y * width + x] = outline_sample(fill, width, height, x, y, k_outline_thickness[v]);
      blit_mask(sheet, &rects[glyph->outline_sprite[v]], outline, width, height);
    }

    free(fill);
    free(outline);
  }

  ++text->glyph_count;
  return true;
}

void dd_text_destroy(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  for (int i = 0; i < DD_ENTITY_TEXT_STYLE_COUNT; ++i) {
    if (text->entity_atlases[i]) game->engine->atlas_destroy(text->entity_atlases[i]);
    if (text->entity_source_textures[i]) game->engine->texture_destroy(text->entity_source_textures[i]);
  }
  if (text->atlas) game->engine->atlas_destroy(text->atlas);
  if (text->source_texture) game->engine->texture_destroy(text->source_texture);
  if (text->face) FT_Done_Face((FT_Face)text->face);
  if (text->library) FT_Done_FreeType((FT_Library)text->library);
  if (text->font_data) game->engine->free_file_data(text->font_data);
  free(text->glyphs);
  memset(text, 0, sizeof(*text));
}

static int entity_text_width(FT_Face face, const char *value, int length, int font_size) {
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)font_size) != 0) return 0;
  int width = 0;
  for (int i = 0; i < length && value[i]; ++i) {
    if (FT_Load_Char(face, (unsigned char)value[i], FT_LOAD_RENDER | FT_LOAD_NO_BITMAP) != 0) continue;
    width += (int)(face->glyph->metrics.width >> 6) + 1;
  }
  return width;
}

static int entity_font_size(FT_Face face, const char *sample, int digits, int max_size) {
  const int width = entity_text_width(face, sample, digits, 100);
  if (width <= 0) return 1;
  int size = (int)(100.f / ((float)width / (float)DD_ENTITY_CELL_SIZE));
  if (size > max_size) size = max_size;
  size = (int)((float)size * 0.92f);
  return size > 0 ? size : 1;
}

static void entity_blit_number(FT_Face face, uint8_t *sheet, int value, int digits, int font_size, int base_y) {
  if (!face || !sheet || value <= 0 || value >= (int)DD_ENTITY_NUMBER_COUNT || digits < 1 || digits > 3)
    return;
  char number[4];
  snprintf(number, sizeof(number), "%d", value);
  const int width = entity_text_width(face, number, digits, font_size);
  int x_offset = (DD_ENTITY_CELL_SIZE - width) / 2;
  if (x_offset < 0) x_offset = 0;
  const int y_offset = base_y;
  const int cell_x = (value % 16) * DD_ENTITY_CELL_SIZE;
  const int cell_y = (value / 16) * DD_ENTITY_CELL_SIZE;
  int previous_width = 0;

  for (int i = 0; i < digits && number[i]; ++i) {
    if (FT_Load_Char(face, (unsigned char)number[i], FT_LOAD_RENDER | FT_LOAD_NO_BITMAP) != 0) continue;
    const FT_Bitmap *bitmap = &face->glyph->bitmap;
    const uint32_t pitch = bitmap->pitch < 0 ? (uint32_t)(-(int64_t)bitmap->pitch) : (uint32_t)bitmap->pitch;
    // A decimal glyph at these sizes always fits in one 64x64 entity cell.
    // Treat anything else as invalid FreeType output before using its metrics
    // as loop bounds or offsets into the fixed-size atlas.
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || !bitmap->buffer || bitmap->width > DD_ENTITY_CELL_SIZE ||
        bitmap->rows > DD_ENTITY_CELL_SIZE || pitch < bitmap->width)
      continue;
    for (uint32_t by = 0; by < bitmap->rows; ++by) {
      const uint8_t *source = bitmap->pitch >= 0
                                  ? bitmap->buffer + (size_t)by * pitch
                                  : bitmap->buffer + (size_t)(bitmap->rows - 1u - by) * pitch;
      for (uint32_t bx = 0; bx < bitmap->width; ++bx) {
        int local_x = x_offset + previous_width + (int)bx;
        int local_y = y_offset + (int)by;
        if (local_x < x_offset) local_x = x_offset;
        if (local_x >= (int)DD_ENTITY_CELL_SIZE) local_x = (int)DD_ENTITY_CELL_SIZE - 1;
        if (local_y < y_offset) local_y = y_offset;
        if (local_y >= (int)DD_ENTITY_CELL_SIZE) local_y = (int)DD_ENTITY_CELL_SIZE - 1;
        const int target_x = cell_x + local_x;
        const int target_y = cell_y + local_y;
        if (target_x < 0 || target_y < 0 || target_x >= (int)DD_ENTITY_SHEET_SIZE ||
            target_y >= (int)DD_ENTITY_SHEET_SIZE)
          continue;
        const size_t target = ((size_t)(uint32_t)target_y * DD_ENTITY_SHEET_SIZE + (uint32_t)target_x) * 4u;
        sheet[target + 0u] = 255u;
        sheet[target + 1u] = 255u;
        sheet[target + 2u] = 255u;
        sheet[target + 3u] = source[bx];
      }
    }
    previous_width += (int)bitmap->width + 1;
  }
}

static bool create_entity_atlas(ft_game *game, int style) {
  dd_text_renderer_t *text = &game->gfx.text;
  FT_Face face = (FT_Face)text->face;
  uint8_t *sheet = calloc((size_t)DD_ENTITY_SHEET_SIZE * DD_ENTITY_SHEET_SIZE, 4u);
  ft_sprite_rect *rects = malloc(DD_ENTITY_NUMBER_COUNT * sizeof(*rects));
  if (!sheet || !rects) {
    free(sheet);
    free(rects);
    return false;
  }
  for (uint32_t value = 0; value < DD_ENTITY_NUMBER_COUNT; ++value) {
    rects[value] = (ft_sprite_rect){(value % 16u) * DD_ENTITY_CELL_SIZE,
                                   (value / 16u) * DD_ENTITY_CELL_SIZE,
                                   DD_ENTITY_CELL_SIZE, DD_ENTITY_CELL_SIZE};
  }

  // CMapImages::InitOverlayTextures. cl_text_entities_size scales the glyph box
  // inside its 64px cell; the top and bottom styles halve that again so a tile
  // can hold both, while the centring offset stays derived from the full box.
  int texture_size = 64 * text->entity_scale / 100;
  if (texture_size < 2) texture_size = 2;
  if (texture_size > 64) texture_size = 64;
  const int vertical_center_offset = (64 - texture_size) / 2 + (int)((float)texture_size * 0.1f);
  const int initial_y = style == DD_ENTITY_TEXT_BOTTOM ? 32 + vertical_center_offset / 2
                                                       : style == DD_ENTITY_TEXT_TOP ? vertical_center_offset / 2
                                                                                     : vertical_center_offset;
  if (style != DD_ENTITY_TEXT_CENTER) texture_size /= 2;
  const char *samples[3] = {"1", "10", "100"};
  const int first_values[3] = {1, 10, 100};
  const int last_values[3] = {9, 99, 255};
  for (int group = 0; group < 3; ++group) {
    const int font_size = entity_font_size(face, samples[group], group + 1, texture_size);
    const int y = initial_y + (texture_size - font_size) / 2;
    for (int value = first_values[group]; value <= last_values[group]; ++value)
      entity_blit_number(face, sheet, value, group + 1, font_size, y);
  }

  const ft_texture_desc texture_desc = {.struct_size = sizeof(texture_desc),
                                        .pixels = sheet,
                                        .width = DD_ENTITY_SHEET_SIZE,
                                        .height = DD_ENTITY_SHEET_SIZE,
                                        .layers = 1u,
                                        .format = FT_TEXTURE_RGBA8,
                                        .mipmaps = false,
                                        .linear_filter = true};
  if (text->entity_atlases[style]) game->engine->atlas_destroy(text->entity_atlases[style]);
  if (text->entity_source_textures[style]) game->engine->texture_destroy(text->entity_source_textures[style]);
  text->entity_atlases[style] = NULL;
  text->entity_source_textures[style] = game->engine->texture_create(&texture_desc);
  free(sheet);
  if (!text->entity_source_textures[style]) {
    free(rects);
    return false;
  }
  const ft_atlas_desc atlas_desc = {.struct_size = sizeof(atlas_desc),
                                    .texture = text->entity_source_textures[style],
                                    .sprites = rects,
                                    .sprite_count = DD_ENTITY_NUMBER_COUNT,
                                    .max_instances_per_frame = 65536u};
  text->entity_atlases[style] = game->engine->atlas_create(&atlas_desc);
  free(rects);
  return text->entity_atlases[style] != NULL;
}

bool dd_text_create(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->atlas) return true;

  FT_Library library = NULL;
  if (FT_Init_FreeType(&library) != 0) return false;
  text->library = library;

  char path[1024];
  // resolve_data_path starts in data/games/ddnet. The font stays in the shared
  // fonts directory so shipping DDNet does not replace the editor's UI font.
  game->engine->resolve_data_path("../../fonts/DejaVuSans.ttf", path, sizeof(path));
  size_t font_size = 0;
  if (!game->engine->read_file(path, &text->font_data, &font_size) || font_size == 0u) {
    dd_text_destroy(game);
    return false;
  }

  FT_Face face = NULL;
  if (FT_New_Memory_Face(library, text->font_data, (FT_Long)font_size, 0, &face) != 0) {
    dd_text_destroy(game);
    return false;
  }
  text->face = face;
  text->baked_size = (float)DD_TEXT_BAKED_SIZE;
  if (FT_Set_Pixel_Sizes(face, 0, DD_TEXT_BAKED_SIZE) != 0) {
    dd_text_destroy(game);
    return false;
  }

  text->glyphs = calloc(DD_TEXT_MAX_GLYPHS, sizeof(*text->glyphs));
  ft_sprite_rect *rects = calloc(DD_TEXT_MAX_SPRITES, sizeof(*rects));
  uint8_t *sheet = calloc((size_t)DD_TEXT_SHEET_SIZE * DD_TEXT_SHEET_SIZE, 4u);
  if (!text->glyphs || !rects || !sheet) {
    free(rects);
    free(sheet);
    dd_text_destroy(game);
    return false;
  }

  uint32_t pack_x = 1u, pack_y = 1u, row_height = 0u, sprite_count = 0u;
  bool ok = true;
  // DDNet's common chat/nameplate repertoire while staying under Vulkan's
  // guaranteed 256 texture-array layers. Unsupported Unicode falls back to
  // the replacement glyph instead of escaping into an ImGui overlay.
  for (uint32_t codepoint = 0x20u; ok && codepoint <= 0x7eu; ++codepoint)
    ok = add_glyph(text, face, codepoint, sheet, rects, &sprite_count, &pack_x, &pack_y, &row_height);
  for (uint32_t codepoint = 0xa0u; ok && codepoint <= 0xffu; ++codepoint)
    ok = add_glyph(text, face, codepoint, sheet, rects, &sprite_count, &pack_x, &pack_y, &row_height);
  if (ok) ok = add_glyph(text, face, 0xfffdu, sheet, rects, &sprite_count, &pack_x, &pack_y, &row_height);
  if (!ok || text->glyph_count == 0u) {
    free(rects);
    free(sheet);
    dd_text_destroy(game);
    return false;
  }

  const ft_texture_desc texture_desc = {.struct_size = sizeof(texture_desc),
                                        .pixels = sheet,
                                        .width = DD_TEXT_SHEET_SIZE,
                                        .height = DD_TEXT_SHEET_SIZE,
                                        .layers = 1u,
                                        .format = FT_TEXTURE_RGBA8,
                                        .mipmaps = false,
                                        .linear_filter = true};
  text->source_texture = game->engine->texture_create(&texture_desc);
  free(sheet);
  if (!text->source_texture) {
    free(rects);
    dd_text_destroy(game);
    return false;
  }

  const ft_atlas_desc atlas_desc = {.struct_size = sizeof(atlas_desc),
                                    .texture = text->source_texture,
                                    .sprites = rects,
                                    .sprite_count = sprite_count,
                                    .max_instances_per_frame = 32768u};
  text->atlas = game->engine->atlas_create(&atlas_desc);
  free(rects);
  if (!text->atlas) {
    dd_text_destroy(game);
    return false;
  }
  text->entity_scale = game->settings.entity_text_size;
  for (int style = 0; style < DD_ENTITY_TEXT_STYLE_COUNT; ++style) {
    if (!create_entity_atlas(game, style)) {
      dd_text_destroy(game);
      return false;
    }
  }
  // Entity atlas creation changes FreeType's active pixel size. Kerning for
  // the regular glyph atlas must stay in the same 64px metric space it was
  // baked in.
  FT_Set_Pixel_Sizes(face, 0, DD_TEXT_BAKED_SIZE);
  return true;
}

static float kerning(const dd_text_renderer_t *text, const dd_text_glyph_t *left, const dd_text_glyph_t *right) {
  if (!left || !right || !FT_HAS_KERNING((FT_Face)text->face)) return 0.f;
  FT_Vector value = {0, 0};
  if (FT_Get_Kerning((FT_Face)text->face, left->glyph_index, right->glyph_index, FT_KERNING_DEFAULT, &value) != 0)
    return 0.f;
  return (float)(value.x >> 6);
}

float dd_text_width(ft_game *game, float size, const char *value) {
  const dd_text_renderer_t *text = &game->gfx.text;
  if (!text->atlas || !value || size <= 0.f) return 0.f;
  const float scale = size / text->baked_size;
  float cursor = 0.f;
  float widest = 0.f;
  const dd_text_glyph_t *previous = NULL;
  while (*value) {
    const uint32_t codepoint = utf8_next(&value);
    if (codepoint == '\n') {
      if (cursor > widest) widest = cursor;
      cursor = 0.f;
      previous = NULL;
      continue;
    }
    const dd_text_glyph_t *glyph = find_glyph(text, codepoint == '\t' ? (uint32_t)' ' : codepoint);
    if (!glyph) continue;
    cursor += kerning(text, previous, glyph) * scale;
    cursor += glyph->advance_x * scale * (codepoint == '\t' ? 4.f : 1.f);
    previous = glyph;
  }
  if (cursor > widest) widest = cursor;
  return widest;
}

// AdjustOutlineThicknessToFontSize, then the variant whose baked ring lands
// closest to that many pixels once the glyph is scaled to `reference_px`.
static uint32_t outline_variant_for(float reference_px) {
  if (!(reference_px > 0.f)) reference_px = DD_TEXT_BAKED_SIZE;
  const float wanted_px = reference_px > 48.f ? 4.f : (reference_px >= 18.f ? 2.f : 1.f);
  const float wanted_baked = wanted_px * (float)DD_TEXT_BAKED_SIZE / reference_px;

  uint32_t best = 0u;
  float best_error = -1.f;
  for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
    const float error = fabsf((float)k_outline_thickness[v] - wanted_baked);
    if (best_error < 0.f || error < best_error) {
      best_error = error;
      best = v;
    }
  }
  return best;
}

void dd_text_draw_outlined(ft_game *game, float z, ft_vec2 position, float size, ft_color color, ft_color outline,
                           float outline_reference_px, const char *value) {
  const dd_text_renderer_t *text = &game->gfx.text;
  if (!text->atlas || !value || !value[0] || size <= 0.f) return;
  const bool want_outline = outline.a > 0.f;
  const bool want_fill = color.a > 0.f;
  if (!want_outline && !want_fill) return;

  const size_t capacity = strlen(value);
  // Outline pass first, then fill, in one array: a batch keeps the order it was
  // given, so the fill lands on top without needing a second depth.
  ft_sprite_draw *draws = malloc((capacity ? capacity : 1u) * 2u * sizeof(*draws));
  if (!draws) return;

  const uint32_t variant = outline_variant_for(outline_reference_px);
  const float scale = size / text->baked_size;
  float cursor_x = position.x;
  float line_y = position.y;
  uint32_t outline_count = 0u, fill_count = 0u;
  ft_sprite_draw *fills = draws + capacity;
  const dd_text_glyph_t *previous = NULL;
  while (*value) {
    const uint32_t codepoint = utf8_next(&value);
    if (codepoint == '\n') {
      cursor_x = position.x;
      line_y += size;
      previous = NULL;
      continue;
    }
    const dd_text_glyph_t *glyph = find_glyph(text, codepoint == '\t' ? (uint32_t)' ' : codepoint);
    if (!glyph) continue;
    cursor_x += kerning(text, previous, glyph) * scale;

    if (glyph->visible && fill_count < capacity) {
      const float width = (float)glyph->width * scale;
      const float height = (float)glyph->height * scale;
      const float char_x = cursor_x + glyph->offset_x * scale;
      const float char_bottom = line_y + size - glyph->offset_y * scale;
      const ft_sprite_draw quad = {.pos = {char_x + width * 0.5f, char_bottom - height * 0.5f},
                                   .size = {width, height},
                                   .rotation = 0.f,
                                   .sprite_index = glyph->sprite_index,
                                   .color = color,
                                   .tiling = {1.f, 1.f}};
      if (want_outline) {
        draws[outline_count] = quad;
        draws[outline_count].sprite_index = glyph->outline_sprite[variant];
        draws[outline_count].color = outline;
        ++outline_count;
      }
      if (want_fill) fills[fill_count++] = quad;
    }
    cursor_x += glyph->advance_x * scale * (codepoint == '\t' ? 4.f : 1.f);
    previous = glyph;
  }

  // Close the gap the two halves left between them so both go out as one batch.
  if (outline_count < capacity && fill_count > 0u)
    memmove(draws + outline_count, fills, (size_t)fill_count * sizeof(*draws));
  if (outline_count + fill_count > 0u)
    game->engine->draw_sprites(text->atlas, z, draws, outline_count + fill_count);
  free(draws);
}

void dd_text_draw(ft_game *game, float z, ft_vec2 position, float size, ft_color color, const char *value) {
  // DefaultTextOutlineColor, and the glyph's own size as the reference, which is
  // what DDNet bakes plain screen-space text at.
  dd_text_draw_outlined(game, z, position, size, color, (ft_color){0.f, 0.f, 0.f, 0.3f * color.a}, size, value);
}

// CMapImages::SetTextureScale: the sheets carry the size, so a new one means
// re-uploading them. Before the renderer exists there is nothing to rebuild --
// dd_text_create bakes whatever the setting holds by then.
void dd_text_set_entity_scale(ft_game *game, int scale) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->entity_scale == scale) return;
  text->entity_scale = scale;
  if (!text->face || !text->entity_atlases[0]) return;

  for (int style = 0; style < DD_ENTITY_TEXT_STYLE_COUNT; ++style) create_entity_atlas(game, style);
  // Baking a sheet leaves FreeType on the entity pixel size; the glyph atlas
  // and its kerning live in the 64px metric space they were measured in.
  FT_Set_Pixel_Sizes((FT_Face)text->face, 0, DD_TEXT_BAKED_SIZE);
}

void dd_entity_text_draw(ft_game *game, float z, int style, int x, int y, int value, ft_color color) {
  if (style < 0 || style >= DD_ENTITY_TEXT_STYLE_COUNT || value <= 0 || value >= (int)DD_ENTITY_NUMBER_COUNT ||
      !game->gfx.text.entity_atlases[style])
    return;
  const ft_sprite_draw draw = {.pos = {x + 0.5f, y + 0.5f},
                               .size = {1.f, 1.f},
                               .rotation = 0.f,
                               .sprite_index = (uint32_t)value,
                               .color = color,
                               .tiling = {1.f, 1.f}};
  game->engine->draw_sprites(game->gfx.text.entity_atlases[style], z, &draw, 1u);
}
