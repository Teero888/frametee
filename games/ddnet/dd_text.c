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
// own array layer, which Vulkan guarantees 2048 of: 256 glyphs make 1280.
// Pages baked on demand: smaller, as they hold only what text asked for. Every
// layer of an atlas is as big as its biggest sprite, so a page costs what it holds.
#define DD_TEXT_EXTRA_SHEET_SIZE 1024u
#define DD_TEXT_EXTRA_GLYPHS 96u
#define DD_TEXT_SPRITES_PER_GLYPH (1u + DD_TEXT_OUTLINE_VARIANTS)
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

static bool pack_rect(uint32_t sheet_size, uint32_t width, uint32_t height, uint32_t *x, uint32_t *y, uint32_t *row_height,
                      ft_sprite_rect *out) {
  if (width > sheet_size - 2u || height > sheet_size - 2u) return false;
  if (*x + width + 1u > sheet_size) {
    *x = 1u;
    *y += *row_height + 1u;
    *row_height = 0u;
  }
  if (*y + height + 1u > sheet_size) return false;
  *out = (ft_sprite_rect){*x, *y, width, height};
  *x += width + 1u;
  if (height > *row_height) *row_height = height;
  return true;
}

// Blits one coverage mask into the sheet as white with that coverage in alpha.
// Colour comes from the draw, exactly as it does for DDNet's two textures.
static void blit_mask(uint8_t *sheet, uint32_t sheet_size, const ft_sprite_rect *rect, const uint8_t *mask, uint32_t width,
                      uint32_t height) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t target = ((size_t)(rect->y + y) * sheet_size + rect->x + x) * 4u;
      sheet[target + 0u] = 255u;
      sheet[target + 1u] = 255u;
      sheet[target + 2u] = 255u;
      sheet[target + 3u] = mask[(size_t)y * width + x];
    }
  }
}

static bool page_init(dd_text_page_t *page, uint32_t sheet_size, uint32_t glyph_capacity) {
  memset(page, 0, sizeof(*page));
  page->sheet = calloc((size_t)sheet_size * sheet_size, 4u);
  page->rects = calloc((size_t)glyph_capacity * DD_TEXT_SPRITES_PER_GLYPH, sizeof(*page->rects));
  if (!page->sheet || !page->rects) {
    free(page->sheet);
    free(page->rects);
    memset(page, 0, sizeof(*page));
    return false;
  }
  page->sheet_size = sheet_size;
  page->glyph_capacity = glyph_capacity;
  page->sprite_capacity = glyph_capacity * DD_TEXT_SPRITES_PER_GLYPH;
  page->pack_x = page->pack_y = 1u;
  return true;
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

// Bakes `codepoint` from font `face_index` into page `page_index`: its fill and every outline variant.
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

  const uint32_t pad = visible ? DD_TEXT_PADDING : 0u;
  const uint32_t width = visible ? bitmap->width + pad * 2u : 1u;
  const uint32_t height = visible ? bitmap->rows + pad * 2u : 1u;
  // A glyph needs its fill sprite and one sprite per outline variant, and they
  // are only useful together, so they are reserved as a group.
  if (page->glyph_count >= page->glyph_capacity || page->sprite_count + DD_TEXT_SPRITES_PER_GLYPH > page->sprite_capacity)
    return GLYPH_PAGE_FULL;
  uint32_t pack_x = page->pack_x, pack_y = page->pack_y, row_height = page->row_height;
  ft_sprite_rect rects[DD_TEXT_SPRITES_PER_GLYPH];
  for (uint32_t r = 0; r < DD_TEXT_SPRITES_PER_GLYPH; ++r)
    if (!pack_rect(page->sheet_size, width, height, &pack_x, &pack_y, &row_height, &rects[r])) return GLYPH_PAGE_FULL;

  dd_text_glyph_t glyph = {.codepoint = codepoint,
                           .glyph_index = glyph_index,
                           .width = visible ? width : 0u,
                           .height = visible ? height : 0u,
                           .offset_x = (float)(slot->metrics.horiBearingX >> 6),
                           .offset_y = (float)-((slot->metrics.height >> 6) - (slot->metrics.horiBearingY >> 6)),
                           .advance_x = (float)(slot->advance.x >> 6),
                           .visible = visible,
                           .face = (uint8_t)face_index,
                           .page = (uint8_t)page_index};
  glyph.sprite_index = page->sprite_count;
  for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) glyph.outline_sprite[v] = page->sprite_count + 1u + v;

  if (visible) {
    const size_t pixel_count = (size_t)width * height;
    uint8_t *fill = calloc(pixel_count, 1u);
    uint8_t *outline = malloc(pixel_count);
    if (!fill || !outline) {
      free(fill);
      free(outline);
      return GLYPH_FAILED;
    }

    for (uint32_t y = 0; y < bitmap->rows; ++y) {
      const uint8_t *source = bitmap->pitch >= 0
                                  ? bitmap->buffer + (size_t)y * (uint32_t)bitmap->pitch
                                  : bitmap->buffer + (size_t)(bitmap->rows - 1u - y) * (uint32_t)(-bitmap->pitch);
      memcpy(fill + (size_t)(y + pad) * width + pad, source, bitmap->width);
    }

    blit_mask(page->sheet, page->sheet_size, &rects[0], fill, width, height);
    for (uint32_t v = 0; v < DD_TEXT_OUTLINE_VARIANTS; ++v) {
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
          outline[(size_t)y * width + x] = outline_sample(fill, width, height, x, y, k_outline_thickness[v]);
      blit_mask(page->sheet, page->sheet_size, &rects[1u + v], outline, width, height);
    }

    free(fill);
    free(outline);
  }

  if (!insert_glyph(text, &glyph)) return GLYPH_FAILED;
  memcpy(&page->rects[page->sprite_count], rects, sizeof(rects));
  page->sprite_count += DD_TEXT_SPRITES_PER_GLYPH;
  page->glyph_count++;
  page->pack_x = pack_x;
  page->pack_y = pack_y;
  page->row_height = row_height;
  return GLYPH_ADDED;
}

// (Re)creates a page's atlas from its sheet. The one it replaces may still have draws queued this
// frame, so it is kept until the next. False when the page cannot be shown (out of memory, or too
// many atlases replaced in one frame).
static bool page_upload(ft_game *game, int page_index) {
  dd_text_renderer_t *text = &game->gfx.text;
  dd_text_page_t *page = &text->pages[page_index];
  if (page->sprite_count == 0u) return false;
  if (page->atlas && text->retired_count >= DD_TEXT_MAX_RETIRED) return false;
  const ft_texture_desc texture_desc = {.struct_size = sizeof(texture_desc),
                                        .pixels = page->sheet,
                                        .width = page->sheet_size,
                                        .height = page->sheet_size,
                                        .layers = 1u,
                                        .format = FT_TEXTURE_RGBA8,
                                        .mipmaps = false,
                                        .linear_filter = true};
  ft_texture *texture = game->engine->texture_create(&texture_desc);
  if (!texture) return false;
  const ft_atlas_desc atlas_desc = {.struct_size = sizeof(atlas_desc),
                                    .texture = texture,
                                    .sprites = page->rects,
                                    .sprite_count = page->sprite_count,
                                    .max_instances_per_frame = 32768u};
  ft_atlas *atlas = game->engine->atlas_create(&atlas_desc);
  if (!atlas) {
    game->engine->texture_destroy(texture);
    return false;
  }
  if (page->atlas) {
    text->retired_atlases[text->retired_count] = page->atlas;
    text->retired_textures[text->retired_count] = page->texture;
    text->retired_count++;
  }
  page->atlas = atlas;
  page->texture = texture;
  page->uploaded_sprites = page->sprite_count;
  return true;
}

void dd_text_frame_begin(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  for (int i = 0; i < text->retired_count; ++i) {
    game->engine->atlas_destroy(text->retired_atlases[i]);
    game->engine->texture_destroy(text->retired_textures[i]);
  }
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
            !page_init(&text->pages[text->page_count], DD_TEXT_EXTRA_SHEET_SIZE, DD_TEXT_EXTRA_GLYPHS)) {
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

// The glyph as it can be drawn now: one whose page's atlas does not have it yet is the replacement.
static const dd_text_glyph_t *drawable_glyph(const dd_text_renderer_t *text, const dd_text_glyph_t *glyph) {
  if (!glyph || glyph->page >= text->page_count) return NULL;
  const dd_text_page_t *page = &text->pages[glyph->page];
  if (page->atlas && glyph->sprite_index + DD_TEXT_SPRITES_PER_GLYPH <= page->uploaded_sprites) return glyph;
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
    if (page->atlas) game->engine->atlas_destroy(page->atlas);
    if (page->texture) game->engine->texture_destroy(page->texture);
    free(page->sheet);
    free(page->rects);
  }
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

bool dd_text_create(ft_game *game) {
  dd_text_renderer_t *text = &game->gfx.text;
  if (text->page_count > 0) return true;

  FT_Library library = NULL;
  if (FT_Init_FreeType(&library) != 0) return false;
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
  if (!page_init(&text->pages[0], DD_TEXT_SHEET_SIZE, DD_TEXT_MAX_GLYPHS)) {
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
  // The first page is done growing; its sheet is only needed to rebuild it.
  text->pages[0].full = true;
  free(text->pages[0].sheet);
  text->pages[0].sheet = NULL;

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
  if (text->page_count == 0 || !value || !value[0] || size <= 0.f) return;
  const bool want_outline = outline.a > 0.f;
  const bool want_fill = color.a > 0.f;
  if (!want_outline && !want_fill) return;
  prepare_text(game, value);

  const size_t capacity = strlen(value);
  // Every quad twice, outline and fill, with the page each is drawn from.
  const size_t total_quads = (capacity ? capacity : 1u) * 2u;
  ft_sprite_draw stack_draws[128];
  uint8_t stack_pages[128];
  ft_sprite_draw *draws = total_quads <= 128 ? stack_draws : malloc(total_quads * sizeof(*draws));
  uint8_t *pages = total_quads <= 128 ? stack_pages : malloc(total_quads);
  if (!draws || !pages) {
    if (draws != stack_draws) free(draws);
    if (pages != stack_pages) free(pages);
    return;
  }

  const uint32_t variant = outline_variant_for(outline_reference_px);
  const float scale = size / text->baked_size;
  float cursor_x = position.x;
  float line_y = position.y;
  uint32_t outline_count = 0u, fill_count = 0u;
  ft_sprite_draw *fills = draws + capacity;
  uint8_t *fill_pages = pages + capacity;
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
        pages[outline_count] = glyph->page;
        ++outline_count;
      }
      if (want_fill) {
        fill_pages[fill_count] = glyph->page;
        fills[fill_count++] = quad;
      }
    }
    cursor_x += glyph->advance_x * scale * (codepoint == '\t' ? 4.f : 1.f);
    previous = glyph;
  }

  // One batch per page. Batches at one depth are ordered by atlas, not by when they were drawn, so
  // the fill goes a hair above the outline to stay on top of it whichever pages they come from.
  ft_sprite_draw stack_batch[128];
  const uint32_t most = outline_count > fill_count ? outline_count : fill_count;
  ft_sprite_draw *batch = most <= 128 ? stack_batch : malloc((size_t)most * sizeof(*batch));
  for (int pass = 0; batch && pass < 2; ++pass) {
    const ft_sprite_draw *source = pass == 0 ? draws : fills;
    const uint8_t *source_pages = pass == 0 ? pages : fill_pages;
    const uint32_t count = pass == 0 ? outline_count : fill_count;
    for (int page = 0; page < text->page_count; ++page) {
      uint32_t n = 0;
      for (uint32_t q = 0; q < count; ++q)
        if (source_pages[q] == page) batch[n++] = source[q];
      if (n > 0 && text->pages[page].atlas) game->engine->draw_sprites(text->pages[page].atlas, pass == 0 ? z : z + 0.001f, batch, n);
    }
  }
  if (batch != stack_batch) free(batch);
  if (draws != stack_draws) free(draws);
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
