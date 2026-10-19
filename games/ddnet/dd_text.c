// DDNet's viewport text renderer.
//
// FreeType is owned by the game module. Glyphs are baked into array textures
// once, then every character is submitted as one world-space quad of the text
// pipeline. The quads are part of the ordinary DDNet render queue: they use the
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
#define DD_TEXT_MAX_GLYPHS 256u
// One sprite per glyph and outline variant. Each is its own array layer, which
// Vulkan guarantees 2048 of: 256 glyphs make 1024.
// Pages baked on demand hold fewer, only what text asked for. Every layer of a
// page is as big as its biggest sprite, so a page costs what it holds.
#define DD_TEXT_EXTRA_GLYPHS 96u
// Glyph quads one frame can draw, across all text.
#define DD_TEXT_MAX_QUADS 32768u
// What DDNet draws for a character no font has: a white square (CGlyphMap's REPLACEMENT_CHARACTER).
#define DD_TEXT_REPLACEMENT 0x25a1u
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
  if (!glyph) glyph = find_exact_glyph(text, DD_TEXT_REPLACEMENT);
  if (!glyph) glyph = find_exact_glyph(text, 0xfffdu);
  if (!glyph) glyph = find_exact_glyph(text, (uint32_t)'?');
  return glyph;
}

// CTextRender::Grow: the glyph dilated by `thickness`, which is the shape of the
// outline under the fill.
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

// How far past the bitmap a glyph's sprite for outline variant `v` reaches:
// its ring reaches as far as it is thick, and one more texel of nothing is left
// for the filter at its edge. Every sprite padded for the thickest outline
// instead made the quads up to twice the size, each extra pixel shaded and
// blended for nothing.
static uint32_t sprite_margin(uint32_t v) { return k_outline_thickness[v] + 1u; }

static bool page_init(dd_text_page_t *page, uint32_t glyph_capacity) {
  memset(page, 0, sizeof(*page));
  page->sprites = calloc((size_t)glyph_capacity * DD_TEXT_OUTLINE_VARIANTS, sizeof(*page->sprites));
  if (!page->sprites) return false;
  page->glyph_capacity = glyph_capacity;
  page->sprite_capacity = glyph_capacity * DD_TEXT_OUTLINE_VARIANTS;
  return true;
}

// The coverage of a page's sprites is only needed to rebuild its texture, which a page that no
// longer grows never is.
static void page_release_coverage(dd_text_page_t *page) {
  for (uint32_t s = 0; s < page->sprite_count; ++s) {
    free(page->sprites[s].coverage);
    page->sprites[s].coverage = NULL;
  }
}

// Puts a glyph into the table, which stays sorted by codepoint. Pointers into it do not survive this.
static bool insert_glyph(dd_text_renderer_t *text, const dd_text_glyph_t *glyph) {
  if (text->glyph_count == text->glyph_capacity) {
    const uint32_t capacity = text->glyph_capacity ? text->glyph_capacity * 2u : 512u;
    dd_text_glyph_t *grown = realloc(text->glyphs, (size_t)capacity * sizeof(*grown));
    if (!grown) return false;
    text->glyphs = grown;
    text->glyph_capacity = capacity;
  }
  uint32_t at = text->glyph_count;
  while (at > 0 && text->glyphs[at - 1].codepoint > glyph->codepoint) --at;
  memmove(&text->glyphs[at + 1], &text->glyphs[at], (size_t)(text->glyph_count - at) * sizeof(*text->glyphs));
  text->glyphs[at] = *glyph;
  ++text->glyph_count;
  return true;
}

typedef enum { GLYPH_ADDED, GLYPH_NOT_IN_FONT, GLYPH_PAGE_FULL, GLYPH_FAILED } glyph_result_t;

// Bakes `codepoint` from font `face_index` into page `page_index`: its fill with every outline variant.
static glyph_result_t add_glyph(dd_text_renderer_t *text, int page_index, int face_index, uint32_t codepoint) {
  FT_Face face = (FT_Face)text->faces[face_index];
  dd_text_page_t *page = &text->pages[page_index];
  const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
  if (glyph_index == 0u) return GLYPH_NOT_IN_FONT;
  if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP) != 0) return GLYPH_NOT_IN_FONT;

  const FT_GlyphSlot slot = face->glyph;
  const FT_Bitmap *bitmap = &slot->bitmap;
  const bool visible = bitmap->width > 0u && bitmap->rows > 0u;
  if (visible && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) return GLYPH_NOT_IN_FONT;
  // A glyph needs a sprite per outline variant, and they are only useful together.
  if (page->glyph_count >= page->glyph_capacity) return GLYPH_PAGE_FULL;

  dd_text_glyph_t glyph = {.codepoint = codepoint,
                           .glyph_index = glyph_index,
                           .layer = page->sprite_count,
                           .width = visible ? bitmap->width : 0u,
                           .height = visible ? bitmap->rows : 0u,
                           .offset_x = (float)(slot->metrics.horiBearingX >> 6),
                           .offset_y = (float)-((slot->metrics.height >> 6) - (slot->metrics.horiBearingY >> 6)),
                           .advance_x = (float)(slot->advance.x >> 6),
                           .visible = visible,
                           .face = (uint8_t)face_index,
                           .page = (uint8_t)page_index};

  dd_text_sprite_t sprites[DD_TEXT_OUTLINE_VARIANTS];
  for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
    const uint32_t margin = visible ? sprite_margin(v) : 0u;
    sprites[v] = (dd_text_sprite_t){visible ? bitmap->width + margin * 2u : 1u, visible ? bitmap->rows + margin * 2u : 1u, NULL};
  }

  if (visible) {
    // The masks are made with room for the thickest outline, and each sprite takes its own margin of them.
    const uint32_t pad = DD_TEXT_PADDING;
    const uint32_t width = bitmap->width + pad * 2u;
    const uint32_t height = bitmap->rows + pad * 2u;
    const size_t pixel_count = (size_t)width * height;
    uint8_t *fill = calloc(pixel_count, 1u);
    uint8_t *outline = malloc(pixel_count);
    bool ok = fill && outline;
    for (uint32_t v = 0; ok && v < DD_TEXT_OUTLINE_VARIANTS; ++v)
      ok = (sprites[v].coverage = malloc((size_t)sprites[v].width * sprites[v].height * 2u)) != NULL;
    if (!ok) {
      free(fill);
      free(outline);
      for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) free(sprites[v].coverage);
      return GLYPH_FAILED;
    }

    for (uint32_t y = 0; y < bitmap->rows; ++y) {
      const uint8_t *source = bitmap->pitch >= 0
                                  ? bitmap->buffer + (size_t)y * (uint32_t)bitmap->pitch
                                  : bitmap->buffer + (size_t)(bitmap->rows - 1u - y) * (uint32_t)(-bitmap->pitch);
      memcpy(fill + (size_t)(y + pad) * width + pad, source, bitmap->width);
    }

    for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
          outline[(size_t)y * width + x] = outline_sample(fill, width, height, x, y, k_outline_thickness[v]);
      const uint32_t inset = pad - sprite_margin(v);
      uint8_t *coverage = sprites[v].coverage;
      for (uint32_t y = 0; y < sprites[v].height; ++y) {
        const size_t row = (size_t)(y + inset) * width + inset;
        for (uint32_t x = 0; x < sprites[v].width; ++x) {
          *coverage++ = fill[row + x];
          *coverage++ = outline[row + x];
        }
      }
    }

    free(fill);
    free(outline);
  }

  if (!insert_glyph(text, &glyph)) {
    for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) free(sprites[v].coverage);
    return GLYPH_FAILED;
  }
  memcpy(&page->sprites[page->sprite_count], sprites, sizeof(sprites));
  page->sprite_count += DD_TEXT_OUTLINE_VARIANTS;
  page->glyph_count++;
  return GLYPH_ADDED;
}

// (Re)creates a page's texture from its sprites, each in the top-left corner of a layer of its own,
// so that each mip level of it is that sprite's alone. The texture it replaces may still have draws
// queued this frame, so it is kept until the next. False when the page cannot be shown (out of
// memory, or too many textures replaced in one frame).
static bool page_upload(ft_game *game, int page_index) {
  dd_text_renderer_t *text = &game->gfx.text;
  dd_text_page_t *page = &text->pages[page_index];
  if (page->sprite_count == 0u) return false;
  if (page->texture && text->retired_count >= DD_TEXT_MAX_RETIRED) return false;

  uint32_t layer_width = 1u, layer_height = 1u;
  for (uint32_t s = 0; s < page->sprite_count; ++s) {
    if (page->sprites[s].width > layer_width) layer_width = page->sprites[s].width;
    if (page->sprites[s].height > layer_height) layer_height = page->sprites[s].height;
  }
  const size_t layer_bytes = (size_t)layer_width * layer_height * 2u;
  uint8_t *pixels = calloc(page->sprite_count, layer_bytes);
  if (!pixels) return false;
  for (uint32_t s = 0; s < page->sprite_count; ++s) {
    const dd_text_sprite_t *sprite = &page->sprites[s];
    if (!sprite->coverage) continue;
    for (uint32_t y = 0; y < sprite->height; ++y)
      memcpy(pixels + s * layer_bytes + (size_t)y * layer_width * 2u, sprite->coverage + (size_t)y * sprite->width * 2u,
             (size_t)sprite->width * 2u);
  }

  const ft_texture_desc texture_desc = {.struct_size = sizeof(texture_desc),
                                        .pixels = pixels,
                                        .width = layer_width,
                                        .height = layer_height,
                                        .layers = page->sprite_count,
                                        .format = FT_TEXTURE_RG8,
                                        .mipmaps = true,
                                        .linear_filter = true};
  ft_texture *texture = game->engine->texture_create(&texture_desc);
  free(pixels);
  if (!texture) return false;
  if (page->texture) text->retired_textures[text->retired_count++] = page->texture;
  page->texture = texture;
  page->layer_width = layer_width;
  page->layer_height = layer_height;
  page->uploaded_sprites = page->sprite_count;
  return true;
}

void dd_text_frame_begin(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  for (int i = 0; i < text->retired_count; ++i) game->engine->texture_destroy(text->retired_textures[i]);
  text->retired_count = 0;
}

// A codepoint no font has: it shows as the replacement, which it now simply points at.
static void alias_replacement(dd_text_renderer_t *text, uint32_t codepoint) {
  const dd_text_glyph_t *replacement = find_glyph(text, codepoint);
  if (!replacement) return;
  dd_text_glyph_t glyph = *replacement;
  glyph.codepoint = codepoint;
  insert_glyph(text, &glyph);
}

// Makes sure every character of `value` has a glyph, baking the ones it lacks from the first font
// of the stack that has them, as DDNet's CGlyphMap::GetCharGlyph picks them.
static void prepare_text(ft_game *game, const char *value) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->page_count == 0 || !value) return;
  uint32_t missing[32];
  int missing_count = 0;
  for (const char *cursor = value; *cursor && missing_count < 32;) {
    const uint32_t codepoint = utf8_next(&cursor);
    if (codepoint < 0x20u || find_exact_glyph(text, codepoint)) continue;
    bool seen = false;
    for (int i = 0; i < missing_count && !seen; ++i) seen = missing[i] == codepoint;
    if (!seen) missing[missing_count++] = codepoint;
  }
  if (missing_count == 0) return;

  bool touched[DD_TEXT_MAX_PAGES] = {false};
  for (int m = 0; m < missing_count; ++m) {
    glyph_result_t result = GLYPH_NOT_IN_FONT;
    for (int f = 0; f < text->face_count && result == GLYPH_NOT_IN_FONT; ++f) {
      if (FT_Get_Char_Index((FT_Face)text->faces[f], missing[m]) == 0u) continue;
      // The newest page that still grows, or a new one.
      int page = text->page_count - 1;
      if (text->pages[page].full) {
        if (text->page_count >= DD_TEXT_MAX_PAGES ||
            !page_init(&text->pages[text->page_count], DD_TEXT_EXTRA_GLYPHS)) {
          result = GLYPH_FAILED;
          break;
        }
        page = text->page_count++;
      }
      result = add_glyph(text, page, f, missing[m]);
      if (result == GLYPH_PAGE_FULL) {
        text->pages[page].full = true;
        --m; // the next page takes it
        result = GLYPH_ADDED;
        break;
      }
      if (result == GLYPH_ADDED) touched[page] = true;
    }
    if (result != GLYPH_ADDED) alias_replacement(text, missing[m]);
  }
  // A page whose upload failed before is tried again with this one; until it succeeds, its newest
  // glyphs draw as the replacement (see drawable_glyph).
  for (int page = 0; page < text->page_count; ++page)
    if (touched[page] || text->pages[page].uploaded_sprites < text->pages[page].sprite_count) page_upload(game, page);
}

// The glyph as it can be drawn now: one whose page's texture does not have it yet is the replacement.
static const dd_text_glyph_t *drawable_glyph(const dd_text_renderer_t *text, const dd_text_glyph_t *glyph) {
  if (!glyph || glyph->page >= text->page_count) return NULL;
  const dd_text_page_t *page = &text->pages[glyph->page];
  if (page->texture && glyph->layer + DD_TEXT_OUTLINE_VARIANTS <= page->uploaded_sprites) return glyph;
  const dd_text_glyph_t *replacement = find_exact_glyph(text, DD_TEXT_REPLACEMENT);
  return replacement && replacement != glyph ? drawable_glyph(text, replacement) : NULL;
}

void dd_text_destroy(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  for (int i = 0; i < DD_ENTITY_TEXT_STYLE_COUNT; ++i) {
    if (text->entity_atlases[i]) game->engine->atlas_destroy(text->entity_atlases[i]);
    if (text->entity_source_textures[i]) game->engine->texture_destroy(text->entity_source_textures[i]);
  }
  dd_text_frame_begin(game);
  for (int i = 0; i < text->page_count; ++i) {
    dd_text_page_t *page = &text->pages[i];
    if (page->texture) game->engine->texture_destroy(page->texture);
    page_release_coverage(page);
    free(page->sprites);
  }
  if (text->pipeline) game->engine->pipeline_destroy(text->pipeline);
  for (int i = 0; i < text->face_count; ++i) FT_Done_Face((FT_Face)text->faces[i]);
  if (text->library) FT_Done_FreeType((FT_Library)text->library);
  for (int i = 0; i < text->font_data_count; ++i) game->engine->free_file_data(text->font_data[i]);
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
                        : style == DD_ENTITY_TEXT_TOP  ? vertical_center_offset / 2
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

// Loads every face of a font file (a .ttc holds several) onto the end of the stack. False when the
// file is missing or unreadable.
static bool load_font_file(ft_game *game, const char *relative_path) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->font_data_count >= DD_TEXT_MAX_FACES || text->face_count >= DD_TEXT_MAX_FACES) return false;
  char path[1024];
  game->engine->resolve_data_path(relative_path, path, sizeof(path));
  void *data = NULL;
  size_t size = 0;
  if (!game->engine->read_file(path, &data, &size) || size == 0u) {
    if (data) game->engine->free_file_data(data);
    return false;
  }
  FT_Face probe = NULL;
  if (FT_New_Memory_Face((FT_Library)text->library, data, (FT_Long)size, -1, &probe) != 0) {
    game->engine->free_file_data(data);
    return false;
  }
  const FT_Long face_count = probe->num_faces;
  FT_Done_Face(probe);
  int loaded = 0;
  for (FT_Long index = 0; index < face_count && text->face_count < DD_TEXT_MAX_FACES; ++index) {
    FT_Face face = NULL;
    if (FT_New_Memory_Face((FT_Library)text->library, data, (FT_Long)size, index, &face) != 0) continue;
    if (!face->charmap || FT_Set_Pixel_Sizes(face, 0, DD_TEXT_BAKED_SIZE) != 0) {
      FT_Done_Face(face);
      continue;
    }
    text->faces[text->face_count++] = face;
    ++loaded;
  }
  if (loaded == 0) {
    game->engine->free_file_data(data);
    return false;
  }
  text->font_data[text->font_data_count++] = data;
  return true;
}

// One glyph quad. Attribute locations mirror data/games/ddnet/shaders/text.vert;
// location 0 is the engine's quad corner.
typedef struct {
  ft_vec2 pos;  // world-space centre
  ft_vec2 size; // world-space size
  ft_vec2 uv;   // how much of its layer the sprite covers
  int32_t layer;
  ft_color fill;
  ft_color outline;
} text_instance_t;

static bool create_text_pipeline(ft_game *game) {
  char vert_path[1024], frag_path[1024];
  game->engine->resolve_data_path("shaders/text.vert.spv", vert_path, sizeof(vert_path));
  game->engine->resolve_data_path("shaders/text.frag.spv", frag_path, sizeof(frag_path));
  void *vert = NULL, *frag = NULL;
  size_t vert_size = 0, frag_size = 0;
  if (!game->engine->read_file(vert_path, &vert, &vert_size) || !game->engine->read_file(frag_path, &frag, &frag_size)) {
    dd_log(game, FT_LOG_ERROR, "Missing text shaders (%s / %s).", vert_path, frag_path);
    game->engine->free_file_data(vert);
    game->engine->free_file_data(frag);
    return false;
  }
  const ft_vertex_attr attrs[] = {
      {1, offsetof(text_instance_t, pos), FT_VERTEX_FLOAT2},
      {2, offsetof(text_instance_t, size), FT_VERTEX_FLOAT2},
      {3, offsetof(text_instance_t, uv), FT_VERTEX_FLOAT2},
      {4, offsetof(text_instance_t, layer), FT_VERTEX_INT1},
      {5, offsetof(text_instance_t, fill), FT_VERTEX_FLOAT4},
      {6, offsetof(text_instance_t, outline), FT_VERTEX_FLOAT4},
  };
  const ft_pipeline_desc desc = {.struct_size = sizeof(desc),
                                 .vertex_spirv = vert,
                                 .vertex_spirv_size = vert_size,
                                 .fragment_spirv = frag,
                                 .fragment_spirv_size = frag_size,
                                 .instance_attrs = attrs,
                                 .instance_attr_count = (uint32_t)(sizeof(attrs) / sizeof(attrs[0])),
                                 .instance_stride = sizeof(text_instance_t),
                                 .max_instances_per_frame = DD_TEXT_MAX_QUADS,
                                 .texture_count = 1,
                                 .alpha_blend = true};
  game->gfx.text.pipeline = game->engine->pipeline_create(&desc);
  game->engine->free_file_data(vert);
  game->engine->free_file_data(frag);
  return game->gfx.text.pipeline != NULL;
}

bool dd_text_create(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->page_count > 0) return true;
  if (!create_text_pipeline(game)) return false;

  FT_Library library = NULL;
  if (FT_Init_FreeType(&library) != 0) {
    dd_text_destroy(game);
    return false;
  }
  text->library = library;

  // DDNet's stack (data/fonts/index.json): DejaVu Sans, then its fallbacks for what DejaVu lacks,
  // CJK above all. The fonts live in the shared fonts directory so shipping DDNet does not replace
  // the editor's UI font; the fallbacks are big and optional, used when someone puts them there.
  static const char *const fallbacks[] = {"../../fonts/SourceHanSans.ttc", "../../fonts/GlowSansJ-Compressed-Book.otf",
                                          "../../fonts/NotoSansCJK-Regular.ttc"};
  if (!load_font_file(game, "../../fonts/DejaVuSans.ttf")) {
    dd_text_destroy(game);
    return false;
  }
  text->face = text->faces[0];
  for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i)
    if (load_font_file(game, fallbacks[i])) dd_log(game, FT_LOG_INFO, "Text falls back to '%s'.", fallbacks[i] + 6);
  text->baked_size = (float)DD_TEXT_BAKED_SIZE;

  // The common chat and nameplate repertoire up front, in one page; everything else as it shows up.
  if (!page_init(&text->pages[0], DD_TEXT_MAX_GLYPHS)) {
    dd_text_destroy(game);
    return false;
  }
  text->page_count = 1;
  bool ok = true;
  for (uint32_t codepoint = 0x20u; ok && codepoint <= 0x7eu; ++codepoint) ok = add_glyph(text, 0, 0, codepoint) != GLYPH_FAILED;
  for (uint32_t codepoint = 0xa0u; ok && codepoint <= 0xffu; ++codepoint) ok = add_glyph(text, 0, 0, codepoint) != GLYPH_FAILED;
  if (ok) ok = add_glyph(text, 0, 0, DD_TEXT_REPLACEMENT) != GLYPH_FAILED;
  if (ok) ok = add_glyph(text, 0, 0, 0xfffdu) != GLYPH_FAILED;
  if (!ok || text->glyph_count == 0u || !page_upload(game, 0)) {
    dd_text_destroy(game);
    return false;
  }
  // The first page is done growing.
  text->pages[0].full = true;
  page_release_coverage(&text->pages[0]);

  text->entity_scale = game->settings.entity_text_size;
  for (int style = 0; style < DD_ENTITY_TEXT_STYLE_COUNT; ++style) {
    if (!create_entity_atlas(game, style)) {
      dd_text_destroy(game);
      return false;
    }
  }
  // Entity atlas creation changes FreeType's active pixel size. Kerning for
  // the regular glyphs must stay in the same 64px metric space they were
  // baked in.
  FT_Set_Pixel_Sizes((FT_Face)text->face, 0, DD_TEXT_BAKED_SIZE);
  return true;
}

static float kerning(const dd_text_renderer_t *text, const dd_text_glyph_t *left, const dd_text_glyph_t *right) {
  if (!left || !right || left->face != right->face || left->face >= text->face_count) return 0.f;
  FT_Face face = (FT_Face)text->faces[left->face];
  if (!FT_HAS_KERNING(face)) return 0.f;
  FT_Vector value = {0, 0};
  if (FT_Get_Kerning(face, left->glyph_index, right->glyph_index, FT_KERNING_DEFAULT, &value) != 0) return 0.f;
  return (float)(value.x >> 6);
}

float dd_text_width(ft_game *game, float size, const char *value) {
  const dd_text_renderer_t *text = &game->gfx.text;
  if (text->page_count == 0 || !value || size <= 0.f) return 0.f;
  prepare_text(game, value);
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
    const dd_text_glyph_t *glyph = drawable_glyph(text, find_glyph(text, codepoint == '\t' ? (uint32_t)' ' : codepoint));
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
  if (text->page_count == 0 || !text->pipeline || !value || !value[0] || size <= 0.f) return;
  const bool want_outline = outline.a > 0.f;
  if (!want_outline && !(color.a > 0.f)) return;
  prepare_text(game, value);

  // One quad per glyph, with the page it is drawn from.
  const size_t capacity = strlen(value);
  text_instance_t stack_quads[64];
  uint8_t stack_pages[64];
  text_instance_t *quads = capacity <= 64 ? stack_quads : malloc(capacity * sizeof(*quads));
  uint8_t *pages = capacity <= 64 ? stack_pages : malloc(capacity);
  if (!quads || !pages) {
    if (quads != stack_quads) free(quads);
    if (pages != stack_pages) free(pages);
    return;
  }

  // Without an outline, the thinnest variant: its quads are the smallest.
  const uint32_t variant = want_outline ? outline_variant_for(outline_reference_px) : 0u;
  const ft_color ring = want_outline ? outline : (ft_color){0.f, 0.f, 0.f, 0.f};
  const float margin = (float)sprite_margin(variant) * 2.f;
  const float scale = size / text->baked_size;
  float cursor_x = position.x;
  float line_y = position.y;
  uint32_t count = 0u;
  const dd_text_glyph_t *previous = NULL;
  while (*value) {
    const uint32_t codepoint = utf8_next(&value);
    if (codepoint == '\n') {
      cursor_x = position.x;
      line_y += size;
      previous = NULL;
      continue;
    }
    const dd_text_glyph_t *glyph = drawable_glyph(text, find_glyph(text, codepoint == '\t' ? (uint32_t)' ' : codepoint));
    if (!glyph) continue;
    cursor_x += kerning(text, previous, glyph) * scale;

    if (glyph->visible && count < capacity) {
      // Where the glyph's sprites were laid out when each had the room of the thickest outline: the
      // bitmap that far in from the corner. Every sprite is centred on it, as large as its margin makes it.
      const float char_x = cursor_x + glyph->offset_x * scale;
      const float char_bottom = line_y + size - glyph->offset_y * scale;
      const dd_text_page_t *page = &text->pages[glyph->page];
      const float sprite_w = (float)glyph->width + margin, sprite_h = (float)glyph->height + margin;
      quads[count] = (text_instance_t){
          .pos = {char_x + ((float)glyph->width * 0.5f + DD_TEXT_PADDING) * scale,
                  char_bottom - ((float)glyph->height * 0.5f + DD_TEXT_PADDING) * scale},
          .size = {sprite_w * scale, sprite_h * scale},
          .uv = {sprite_w / (float)page->layer_width, sprite_h / (float)page->layer_height},
          .layer = (int32_t)(glyph->layer + variant),
          .fill = color,
          .outline = ring};
      pages[count++] = glyph->page;
    }
    cursor_x += glyph->advance_x * scale * (codepoint == '\t' ? 4.f : 1.f);
    previous = glyph;
  }

  // One draw per page.
  text_instance_t stack_batch[64];
  text_instance_t *batch = count <= 64 ? stack_batch : malloc((size_t)count * sizeof(*batch));
  for (int page = 0; batch && page < text->page_count; ++page) {
    uint32_t n = 0;
    for (uint32_t q = 0; q < count; ++q)
      if (pages[q] == page) batch[n++] = quads[q];
    ft_texture *texture = text->pages[page].texture;
    if (n > 0 && texture) game->engine->draw_instances(text->pipeline, z, &texture, 1u, batch, n);
  }
  if (batch != stack_batch) free(batch);
  if (quads != stack_quads) free(quads);
  if (pages != stack_pages) free(pages);
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

  for (int style = 0; style < DD_ENTITY_TEXT_STYLE_COUNT; ++style)
    create_entity_atlas(game, style);
  // Baking a sheet leaves FreeType on the entity pixel size; the glyphs
  // and their kerning live in the 64px metric space they were measured in.
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
