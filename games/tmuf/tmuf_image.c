// The game's pictures, DDS and TGA: as the GPU takes them (tm_image_load:
// BC blocks as they are, with their levels; RGBA8 with a box-filtered mip
// chain otherwise), or decoded to RGBA8 (tm_image_decode, top level only).

#include "tmuf_internal.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// --- DDS ---------------------------------------------------------------------

static void rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
  out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
  out[2] = (uint8_t)((c & 31) * 255 / 31);
}

// A DXT colour block into a 4x4 RGBA tile; `alpha_free` for DXT3/5 (four
// colours regardless of the endpoints' order).
static void color_block(const uint8_t *b, bool alpha_free, uint8_t tile[16][4]) {
  const uint16_t c0 = rd16(b), c1 = rd16(b + 2);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || alpha_free) {
    for (int k = 0; k < 3; k++) {
      pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; k++)
      pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255;
    pal[3][3] = 0; // DXT1 one-bit alpha: transparent black
  }
  const uint32_t bits = rd32(b + 4);
  for (int i = 0; i < 16; i++)
    memcpy(tile[i], pal[(bits >> (2 * i)) & 3], 4);
}

static void alpha_dxt3(const uint8_t *b, uint8_t tile[16][4]) {
  for (int i = 0; i < 16; i++) {
    const uint8_t nib = (uint8_t)((b[i / 2] >> (4 * (i & 1))) & 15);
    tile[i][3] = (uint8_t)(nib * 17);
  }
}

static void alpha_dxt5(const uint8_t *b, uint8_t tile[16][4]) {
  uint8_t a[8];
  a[0] = b[0];
  a[1] = b[1];
  if (a[0] > a[1])
    for (int k = 1; k < 7; k++)
      a[k + 1] = (uint8_t)(((7 - k) * a[0] + k * a[1]) / 7);
  else {
    for (int k = 1; k < 5; k++)
      a[k + 1] = (uint8_t)(((5 - k) * a[0] + k * a[1]) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; k++)
    bits |= (uint64_t)b[2 + k] << (8 * k);
  for (int i = 0; i < 16; i++)
    tile[i][3] = a[(bits >> (3 * i)) & 7];
}

static bool decode_dxt(const uint8_t *src, size_t size, uint32_t w, uint32_t h, int kind, uint8_t *out) {
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t block = kind == 1 ? 8 : 16;
  if ((size_t)bw * bh * block > size) return false;
  for (uint32_t by = 0; by < bh; by++)
    for (uint32_t bx = 0; bx < bw; bx++) {
      const uint8_t *b = src + ((size_t)by * bw + bx) * block;
      uint8_t tile[16][4];
      if (kind == 1) {
        color_block(b, false, tile);
      } else {
        color_block(b + 8, true, tile);
        if (kind == 3) alpha_dxt3(b, tile);
        else alpha_dxt5(b, tile);
      }
      for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
          const uint32_t px = bx * 4 + x, py = by * 4 + y;
          if (px < w && py < h) memcpy(out + ((size_t)py * w + px) * 4, tile[y * 4 + x], 4);
        }
    }
  return true;
}

// the value of a channel under a mask, scaled to 8 bits
static uint8_t channel(uint32_t v, uint32_t mask) {
  if (!mask) return 255;
  uint32_t shift = 0;
  while (!((mask >> shift) & 1u))
    shift++;
  const uint32_t max = mask >> shift;
  return (uint8_t)(((v & mask) >> shift) * 255u / max);
}

static bool decode_dds(const uint8_t *d, size_t size, uint8_t **rgba, uint32_t *width, uint32_t *height) {
  if (size < 128 || memcmp(d, "DDS ", 4) != 0) return false;
  const uint32_t h = rd32(d + 12), w = rd32(d + 16);
  const uint32_t pf_flags = rd32(d + 80), fourcc = rd32(d + 84), bits = rd32(d + 88);
  const uint32_t rmask = rd32(d + 92), gmask = rd32(d + 96), bmask = rd32(d + 100), amask = rd32(d + 104);
  if (!w || !h || w > 16384 || h > 16384) return false;
  const uint8_t *src = d + 128;
  size_t left = size - 128;
  uint8_t *out = malloc((size_t)w * h * 4);
  if (!out) return false;
  bool ok = false;
  if (pf_flags & 4u) { // FOURCC
    const int kind = fourcc == 0x31545844u ? 1 : fourcc == 0x33545844u ? 3 : fourcc == 0x35545844u ? 5 : 0;
    ok = kind && decode_dxt(src, left, w, h, kind, out);
  } else if (bits == 8 || bits == 16 || bits == 24 || bits == 32) {
    const size_t bpp = bits / 8, n = (size_t)w * h;
    if (n * bpp <= left) {
      const bool luminance = (pf_flags & 0x20000u) != 0, alpha_only = (pf_flags & 2u) && !(pf_flags & 0x40u) && !luminance;
      for (size_t i = 0; i < n; i++) {
        uint32_t v = 0;
        for (size_t k = 0; k < bpp; k++)
          v |= (uint32_t)src[i * bpp + k] << (8 * k);
        uint8_t *o = out + i * 4;
        if (alpha_only) {
          o[0] = o[1] = o[2] = 255;
          o[3] = channel(v, amask);
        } else if (luminance) {
          o[0] = o[1] = o[2] = channel(v, rmask);
          o[3] = (pf_flags & 1u) ? channel(v, amask) : 255;
        } else {
          o[0] = channel(v, rmask);
          o[1] = channel(v, gmask);
          o[2] = channel(v, bmask);
          o[3] = (pf_flags & 1u) ? channel(v, amask) : 255;
        }
      }
      ok = true;
    }
  }
  if (!ok) {
    free(out);
    return false;
  }
  *rgba = out;
  *width = w;
  *height = h;
  return true;
}

// --- TGA ---------------------------------------------------------------------

static bool decode_tga(const uint8_t *d, size_t size, uint8_t **rgba, uint32_t *width, uint32_t *height) {
  if (size < 18) return false;
  const uint8_t id_len = d[0], cmap = d[1], type = d[2];
  const uint32_t w = rd16(d + 12), h = rd16(d + 14);
  const uint8_t bpp = d[16], desc = d[17];
  if (cmap || !w || !h || (type != 2 && type != 3 && type != 10 && type != 11)) return false;
  if (bpp != 8 && bpp != 24 && bpp != 32) return false;
  const size_t px = bpp / 8u, n = (size_t)w * h;
  const uint8_t *src = d + 18 + id_len, *end = d + size;
  uint8_t *out = malloc(n * 4);
  if (!out) return false;
  const bool rle = type >= 9;
  size_t i = 0;
  while (i < n) {
    size_t count = 1;
    bool repeat = false;
    if (rle) {
      if (src >= end) break;
      const uint8_t head = *src++;
      count = (head & 127u) + 1u;
      repeat = (head & 128u) != 0;
    } else {
      count = n;
    }
    for (size_t k = 0; k < count && i < n; k++, i++) {
      const uint8_t *p = src;
      if ((size_t)(end - p) < px) {
        free(out);
        return false;
      }
      if (!repeat || k + 1 == count) src += px;
      // rows as the file stores them, whatever its origin bit says: the
      // game's own reader keeps them so (its tables and height maps are
      // upside down against the usual reading)
      const size_t row = i / w, col = i % w;
      const size_t y = row;
      (void)desc;
      uint8_t *o = out + (y * w + col) * 4;
      if (px == 1) o[0] = o[1] = o[2] = p[0], o[3] = 255;
      else {
        o[0] = p[2];
        o[1] = p[1];
        o[2] = p[0];
        o[3] = px == 4 ? p[3] : 255;
      }
    }
  }
  if (i < n) {
    free(out);
    return false;
  }
  *rgba = out;
  *width = w;
  *height = h;
  return true;
}

// --- images for the GPU (tmuf_gpu.h) ---------------------------------------------------

// the levels below `rgba` (w x h), each half the one above, box filtered as
// D3DX makes a texture's mip chain: every level after the first, into out
static size_t mip_chain_size(uint32_t w, uint32_t h, uint32_t *levels) {
  size_t total = 0;
  uint32_t n = 0;
  for (;;) {
    total += (size_t)w * h * 4;
    n++;
    if (w == 1 && h == 1) break;
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  *levels = n;
  return total;
}

static void mip_chain(uint8_t *out, uint32_t w, uint32_t h) {
  const uint8_t *src = out;
  uint8_t *dst = out + (size_t)w * h * 4;
  while (w > 1 || h > 1) {
    const uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    for (uint32_t y = 0; y < nh; y++)
      for (uint32_t x = 0; x < nw; x++) {
        const uint32_t x0 = x * 2, y0 = y * 2, x1 = w > 1 ? x0 + 1 : x0, y1 = h > 1 ? y0 + 1 : y0;
        for (int c = 0; c < 4; c++) {
          const uint32_t sum = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                               src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
          dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
        }
      }
    src = dst;
    dst += (size_t)nw * nh * 4;
    w = nw, h = nh;
  }
}

// RGBA8 with its whole mip chain
static bool rgba_image(uint8_t *rgba, uint32_t w, uint32_t h, tm_image *out) {
  uint32_t levels;
  const size_t total = mip_chain_size(w, h, &levels);
  uint8_t *data = realloc(rgba, total);
  if (!data) {
    free(rgba);
    return false;
  }
  mip_chain(data, w, h);
  out->format = TG_RGBA8;
  out->width = w, out->height = h, out->levels = levels, out->faces = 1;
  out->data = data;
  out->size = total;
  return true;
}

// The game keeps its pictures bottom row first: a TGA as its file stores it
// (bottom-up), a DDS turned upside down at load (the cube maps excepted).
// Mesh texture coordinates are made for that.
static void flip_rows(uint8_t *pixels, uint32_t w, uint32_t h, size_t pixel_size) {
  const size_t row = (size_t)w * pixel_size;
  uint8_t *tmp = malloc(row);
  if (!tmp) return;
  for (uint32_t y = 0; y < h / 2; y++) {
    memcpy(tmp, pixels + y * row, row);
    memcpy(pixels + y * row, pixels + (size_t)(h - 1 - y) * row, row);
    memcpy(pixels + (size_t)(h - 1 - y) * row, tmp, row);
  }
  free(tmp);
}

// a BC block's rows reversed (the first `rows` of them: the rest lie outside
// a level smaller than a block)
static void flip_block(uint8_t *b, tg_format format, uint32_t rows) {
  uint8_t *color = format == TG_BC1 ? b : b + 8;
  for (uint32_t i = 0; i < rows / 2; i++) {
    uint8_t t = color[4 + i];
    color[4 + i] = color[4 + rows - 1 - i];
    color[4 + rows - 1 - i] = t;
  }
  if (format == TG_BC2) {
    for (uint32_t i = 0; i < rows / 2; i++)
      for (int k = 0; k < 2; k++) {
        uint8_t t = b[i * 2 + k];
        b[i * 2 + k] = b[(rows - 1 - i) * 2 + k];
        b[(rows - 1 - i) * 2 + k] = t;
      }
  } else if (format == TG_BC3) {
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++)
      bits |= (uint64_t)b[2 + k] << (8 * k);
    uint64_t out = bits;
    for (uint32_t r = 0; r < rows; r++) {
      const uint64_t line = (bits >> (12 * r)) & 0xfffu;
      out &= ~((uint64_t)0xfffu << (12 * (rows - 1 - r)));
      out |= line << (12 * (rows - 1 - r));
    }
    for (int k = 0; k < 6; k++)
      b[2 + k] = (uint8_t)(out >> (8 * k));
  }
}

static void flip_level(uint8_t *data, tg_format format, uint32_t w, uint32_t h) {
  if (format == TG_RGBA8) {
    flip_rows(data, w, h, 4);
    return;
  }
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t block = format == TG_BC1 ? 8 : 16;
  flip_rows(data, bw, bh, block);
  for (size_t i = 0; i < (size_t)bw * bh; i++)
    flip_block(data + i * block, format, h < 4 ? h : 4);
}

bool tm_image_load_rgba(uint8_t *rgba, uint32_t width, uint32_t height, tm_image *out) {
  memset(out, 0, sizeof *out);
  return rgba_image(rgba, width, height, out);
}

bool tm_image_load(const uint8_t *d, size_t size, bool bc, tm_image *out) {
  memset(out, 0, sizeof *out);
  if (size >= 128 && memcmp(d, "DDS ", 4) == 0) {
    const uint32_t flags = rd32(d + 8), h = rd32(d + 12), w = rd32(d + 16);
    const uint32_t mips = (flags & 0x20000u) && rd32(d + 28) ? rd32(d + 28) : 1;
    const uint32_t pf_flags = rd32(d + 80), fourcc = rd32(d + 84), caps2 = rd32(d + 112);
    const uint32_t faces = (caps2 & 0x200u) ? 6 : 1;
    const tg_format format = !(pf_flags & 4u)            ? TG_RGBA8
                             : fourcc == 0x31545844u ? TG_BC1
                             : fourcc == 0x33545844u ? TG_BC2
                             : fourcc == 0x35545844u ? TG_BC3
                                                     : TG_RGBA8;
    // blocks as they are, when the file has its levels (or is a cube)
    if (bc && format != TG_RGBA8 && (mips > 1 || faces == 6 || (w <= 4 && h <= 4)) && w && h) {
      size_t total = 0;
      for (uint32_t l = 0; l < mips; l++)
        total += tg_level_size(format, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
      total *= faces;
      if (total > size - 128) return false;
      uint8_t *data = malloc(total);
      if (!data) return false;
      memcpy(data, d + 128, total);
      if (faces == 1) {
        size_t at = 0;
        for (uint32_t l = 0; l < mips; l++) {
          const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
          flip_level(data + at, format, lw, lh);
          at += tg_level_size(format, lw, lh);
        }
      }
      out->format = format;
      out->width = w, out->height = h, out->levels = mips, out->faces = faces;
      out->data = data;
      out->size = total;
      return true;
    }
    if (faces == 6) {
      // a cube of uncompressed faces (or without BC): each face's first level
      uint8_t *faces_rgba = NULL;
      size_t per = (size_t)w * h * 4, at = 128;
      faces_rgba = malloc(per * 6);
      if (!faces_rgba) return false;
      for (uint32_t f = 0; f < 6; f++) {
        // decode_dds reads one image from a header: give it this face's data
        size_t face_bytes = 0;
        for (uint32_t l = 0; l < mips; l++) {
          const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
          face_bytes += format != TG_RGBA8 ? tg_level_size(format, lw, lh) : (size_t)lw * lh * (rd32(d + 88) / 8);
        }
        if (at + face_bytes > size) {
          free(faces_rgba);
          return false;
        }
        uint8_t *tmp = malloc(128 + face_bytes);
        if (!tmp) {
          free(faces_rgba);
          return false;
        }
        memcpy(tmp, d, 128);
        memcpy(tmp + 128, d + at, face_bytes);
        uint8_t *rgba = NULL;
        uint32_t fw, fh;
        const bool ok = decode_dds(tmp, 128 + face_bytes, &rgba, &fw, &fh);
        free(tmp);
        if (!ok) {
          free(faces_rgba);
          return false;
        }
        memcpy(faces_rgba + per * f, rgba, per);
        free(rgba);
        at += face_bytes;
      }
      out->format = TG_RGBA8;
      out->width = w, out->height = h, out->levels = 1, out->faces = 6;
      out->data = faces_rgba;
      out->size = per * 6;
      return true;
    }
  }
  uint8_t *rgba = NULL;
  uint32_t w = 0, h = 0;
  if (!tm_image_decode(d, size, &rgba, &w, &h)) return false;
  if (size >= 4 && memcmp(d, "DDS ", 4) == 0) flip_rows(rgba, w, h, 4);
  return rgba_image(rgba, w, h, out);
}

void tm_image_free(tm_image *image) {
  free((void *)image->data);
  memset(image, 0, sizeof *image);
}

bool tm_image_decode(const uint8_t *data, size_t size, uint8_t **rgba, uint32_t *width, uint32_t *height) {
  if (!data || size < 4) return false;
  if (memcmp(data, "DDS ", 4) == 0) return decode_dds(data, size, rgba, width, height);
  return decode_tga(data, size, rgba, width, height);
}
