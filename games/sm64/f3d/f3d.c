// The Fast3D interpreter (f3d.h). The display list is walked as the RSP
// would: matrices, lights and the viewport are kept, vertices are transformed
// and lit when loaded, triangles are culled; the RDP's modes, colors, tiles
// and 4 KiB of texture memory are kept as the commands set them. Textures are
// decoded from texture memory when a triangle uses them, and cached by what
// they hold.
#include "f3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <PR/gbi.h>
#include <sm64_physics.h>

#define N64_WIDTH 320.0f
#define N64_HEIGHT 240.0f
#define VERTEX_SLOTS 32
#define STACK_DEPTH 10
#define DL_DEPTH 32
#define BATCH_VERTICES (3 * 1024)
#define TMEM_SIZE 4096
#define CACHE_KEEP_FRAMES 120

// The opcodes, as bytes (F3D defines some as negative numbers).
#define OP(x) ((uint8_t) (x))

struct vertex {
    float clip[4];
    float s, t; // texture coordinates in 1/32 texels, scaled by G_TEXTURE
    float color[4];
    float fog;
};

struct light {
    float color[3];
    float dir[3];
};

struct tile {
    uint8_t fmt, siz, palette;
    uint8_t cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t line, tmem;
    uint16_t uls, ult, lrs, lrt; // 10.2
};

struct cached {
    uint64_t key;
    void *texture;
    uint32_t used;
};

struct resolved {
    void *texture;
    float width, height;
    struct f3d_sampler sampler;
};

struct f3d {
    struct f3d_backend backend;
    float width, height;
    uint32_t frame;

    // RSP
    float modelview[STACK_DEPTH][4][4];
    int depth;
    float projection[4][4];
    float mvp[4][4];
    bool mvp_dirty;
    uint32_t geometry;
    struct light lights[8];
    int num_lights; // directional ones; lights[num_lights] is the ambient light
    float lookat[2][3];
    float light_dirs[8][3], lookat_dirs[2][3]; // in the model's space
    bool lights_dirty;
    int16_t fog_mul, fog_offset;
    float tex_scale_s, tex_scale_t;
    uint8_t tex_tile;
    bool tex_on;
    struct vertex vertices[VERTEX_SLOTS];
    float viewport[4]; // in the target's pixels
    float clip_x_scale;
    bool camera;
    float camera_view[4][4];

    // RDP
    uint32_t other_h, other_l, combine[2];
    float prim[4], env[4], fog[4], blend[4], prim_lod_frac;
    uint32_t fill_color;
    const uint8_t *timg;
    uint8_t timg_siz;
    uint32_t timg_width;
    uintptr_t cimg, zimg;
    struct tile tiles[8];
    uint8_t tmem[TMEM_SIZE];
    bool textures_dirty;
    struct resolved resolved[2];
    float scissor[4];

    // The batch being gathered.
    struct f3d_state state;
    bool have_state;
    struct f3d_vertex batch[BATCH_VERTICES];
    uint32_t batch_count;

    // Textures by content.
    struct cached *cache;
    uint32_t cache_capacity, cache_count;
    uint8_t rgba[1024 * 1024 * 4];
};

// --- Matrices -----------------------------------------------------------------------

// Row vectors: a point times a matrix. out = a then b.
static void mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    float r[4][4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void identity(float m[4][4]) {
    memset(m, 0, 16 * sizeof(float));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static bool invert(float out[4][4], const float m[4][4]) {
    // Gauss-Jordan with partial pivoting.
    float a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i][j];
            a[i][j + 4] = i == j ? 1.0f : 0.0f;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) {
            if (fabsf(a[r][c]) > fabsf(a[p][c])) {
                p = r;
            }
        }
        if (a[p][c] == 0.0f) {
            return false;
        }
        if (p != c) {
            for (int j = 0; j < 8; ++j) {
                float t = a[c][j];
                a[c][j] = a[p][j];
                a[p][j] = t;
            }
        }
        const float d = a[c][c];
        for (int j = 0; j < 8; ++j) {
            a[c][j] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c && a[r][c] != 0.0f) {
                const float f = a[r][c];
                for (int j = 0; j < 8; ++j) {
                    a[r][j] -= f * a[c][j];
                }
            }
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i][j] = a[i][j + 4];
        }
    }
    return true;
}

// Mtx: sixteen s15.16 numbers, the integer halves of all of them first, two
// to a 32-bit word, then the fractions the same way.
static void read_matrix(float out[4][4], const void *address) {
    const uint32_t *w = address;
    for (int i = 0; i < 16; ++i) {
        const uint32_t whole = w[i / 2], fraction = w[8 + i / 2];
        const uint32_t hi = i % 2 == 0 ? whole >> 16 : whole & 0xffff;
        const uint32_t lo = i % 2 == 0 ? fraction >> 16 : fraction & 0xffff;
        out[i / 4][i % 4] = (float) (int32_t) (hi << 16 | lo) / 65536.0f;
    }
}

static void update_mvp(f3d *f) {
    if (f->mvp_dirty) {
        mul(f->mvp, f->modelview[f->depth], f->projection);
        f->mvp_dirty = false;
    }
}

// --- Lights -------------------------------------------------------------------------

// A direction in the world as the model's: the modelview's rows are the model's
// axes.
static void to_model(const f3d *f, const float dir[3], float out[3]) {
    const float(*m)[4] = f->modelview[f->depth];
    for (int j = 0; j < 3; ++j) {
        out[j] = dir[0] * m[j][0] + dir[1] * m[j][1] + dir[2] * m[j][2];
    }
    const float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 0.0f) {
        out[0] /= l, out[1] /= l, out[2] /= l;
    }
}

static void update_lights(f3d *f) {
    if (!f->lights_dirty) {
        return;
    }
    for (int i = 0; i < f->num_lights; ++i) {
        to_model(f, f->lights[i].dir, f->light_dirs[i]);
    }
    for (int i = 0; i < 2; ++i) {
        to_model(f, f->lookat[i], f->lookat_dirs[i]);
    }
    f->lights_dirty = false;
}

static void read_light(struct light *light, const uint8_t *l) {
    for (int i = 0; i < 3; ++i) {
        light->color[i] = l[i] / 255.0f;
        light->dir[i] = (int8_t) l[8 + i] / 127.0f;
    }
}

// --- Target mapping -------------------------------------------------------------

// The N64's screen (320 x 240) on the target: stretched over the whole width
// for what spans the screen, 4:3 in the middle for the rest.
static float map_x(const f3d *f, float x, bool full) {
    return full ? x * f->width / N64_WIDTH : f->width * 0.5f + (x - N64_WIDTH * 0.5f) * f->height / N64_HEIGHT;
}

static float map_y(const f3d *f, float y) {
    return y * f->height / N64_HEIGHT;
}

static void set_viewport(f3d *f, const Vp_t *vp) {
    const float sx = fabsf(vp->vscale[0] / 4.0f), sy = fabsf(vp->vscale[1] / 4.0f);
    const float cx = vp->vtrans[0] / 4.0f, cy = vp->vtrans[1] / 4.0f;
    const bool full = cx - sx <= 0.5f && cx + sx >= N64_WIDTH - 0.5f;
    const float x0 = map_x(f, cx - sx, full), x1 = map_x(f, cx + sx, full);
    f->viewport[0] = x0;
    f->viewport[1] = map_y(f, cy - sy);
    f->viewport[2] = x1 - x0;
    f->viewport[3] = map_y(f, cy + sy) - f->viewport[1];
    // The game's 4:3 projection keeps its proportions on a wider target.
    f->clip_x_scale = full ? (4.0f / 3.0f) * f->height / f->width : 1.0f;
}

// --- Vertices ------------------------------------------------------------------------

static void load_vertices(f3d *f, const Vtx *src, int count, int first) {
    update_mvp(f);
    if (f->geometry & G_LIGHTING) {
        update_lights(f);
    }
    for (int i = 0; i < count && first + i < VERTEX_SLOTS; ++i) {
        const Vtx_t *v = &src[i].v;
        struct vertex *out = &f->vertices[first + i];
        const float p[4] = { v->ob[0], v->ob[1], v->ob[2], 1.0f };
        for (int j = 0; j < 4; ++j) {
            out->clip[j] = p[0] * f->mvp[0][j] + p[1] * f->mvp[1][j] + p[2] * f->mvp[2][j] + p[3] * f->mvp[3][j];
        }
        out->clip[0] *= f->clip_x_scale;

        float normal[3] = { 0, 0, 0 };
        if (f->geometry & G_LIGHTING) {
            const Vtx_tn *n = &src[i].n;
            normal[0] = n->n[0] / 127.0f, normal[1] = n->n[1] / 127.0f, normal[2] = n->n[2] / 127.0f;
            float c[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = f->lights[f->num_lights].color[k];
            }
            for (int l = 0; l < f->num_lights; ++l) {
                const float d = normal[0] * f->light_dirs[l][0] + normal[1] * f->light_dirs[l][1]
                                + normal[2] * f->light_dirs[l][2];
                if (d > 0.0f) {
                    for (int k = 0; k < 3; ++k) {
                        c[k] += d * f->lights[l].color[k];
                    }
                }
            }
            for (int k = 0; k < 3; ++k) {
                out->color[k] = c[k] > 1.0f ? 1.0f : c[k];
            }
            out->color[3] = n->a / 255.0f;
        } else {
            for (int k = 0; k < 4; ++k) {
                out->color[k] = v->cn[k] / 255.0f;
            }
        }

        if ((f->geometry & G_TEXTURE_GEN) && (f->geometry & G_LIGHTING)) {
            // The texture is looked up by the normal as seen from the eye:
            // -1..1 over the texture, as the RSP scales it.
            const float x = normal[0] * f->lookat_dirs[0][0] + normal[1] * f->lookat_dirs[0][1]
                            + normal[2] * f->lookat_dirs[0][2];
            const float y = normal[0] * f->lookat_dirs[1][0] + normal[1] * f->lookat_dirs[1][1]
                            + normal[2] * f->lookat_dirs[1][2];
            out->s = (x + 1.0f) * 16384.0f * f->tex_scale_s;
            out->t = (y + 1.0f) * 16384.0f * f->tex_scale_t;
        } else {
            out->s = v->tc[0] * f->tex_scale_s;
            out->t = v->tc[1] * f->tex_scale_t;
        }

        out->fog = 0.0f;
        if ((f->geometry & G_FOG) && out->clip[3] > 0.0f) {
            float fog = out->clip[2] / out->clip[3] * f->fog_mul + f->fog_offset;
            fog = fog < 0.0f ? 0.0f : fog > 255.0f ? 255.0f : fog;
            out->fog = fog / 255.0f;
        }
    }
}

// --- Textures ------------------------------------------------------------------------

static uint64_t fnv(uint64_t h, const void *data, size_t size) {
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint8_t expand5(uint32_t v) {
    return (uint8_t) ((v << 3) | (v >> 2));
}

static uint8_t tmem_byte(const f3d *f, uint32_t address) {
    return f->tmem[address & (TMEM_SIZE - 1)];
}

static void tlut_color(const f3d *f, uint32_t address, uint8_t out[4]) {
    const uint32_t c = (uint32_t) tmem_byte(f, address) << 8 | tmem_byte(f, address + 1);
    if (((f->other_h >> G_MDSFT_TEXTLUT) & 3) == 3) { // IA16
        out[0] = out[1] = out[2] = (uint8_t) (c >> 8);
        out[3] = (uint8_t) c;
    } else {
        out[0] = expand5(c >> 11), out[1] = expand5((c >> 6) & 31), out[2] = expand5((c >> 1) & 31);
        out[3] = (c & 1) ? 255 : 0;
    }
}

// The texel at (x, y) of a tile in texture memory, as RGBA8.
static void texel(const f3d *f, const struct tile *t, uint32_t x, uint32_t y, uint32_t stride, uint8_t out[4]) {
    const uint32_t row = t->tmem * 8u + y * stride;
    switch (t->siz) {
        case G_IM_SIZ_4b: {
            const uint8_t b = tmem_byte(f, row + x / 2);
            const uint32_t v = x % 2 == 0 ? b >> 4 : b & 15;
            if (t->fmt == G_IM_FMT_CI) {
                tlut_color(f, 0x800 + t->palette * 32u + v * 2, out);
            } else if (t->fmt == G_IM_FMT_IA) {
                out[0] = out[1] = out[2] = (uint8_t) ((v >> 1) * 255 / 7);
                out[3] = (v & 1) ? 255 : 0;
            } else {
                out[0] = out[1] = out[2] = out[3] = (uint8_t) (v * 17);
            }
            break;
        }
        case G_IM_SIZ_8b: {
            const uint8_t v = tmem_byte(f, row + x);
            if (t->fmt == G_IM_FMT_CI) {
                tlut_color(f, 0x800 + v * 2u, out);
            } else if (t->fmt == G_IM_FMT_IA) {
                out[0] = out[1] = out[2] = (uint8_t) ((v >> 4) * 17);
                out[3] = (uint8_t) ((v & 15) * 17);
            } else {
                out[0] = out[1] = out[2] = out[3] = v;
            }
            break;
        }
        case G_IM_SIZ_16b: {
            const uint32_t c = (uint32_t) tmem_byte(f, row + x * 2) << 8 | tmem_byte(f, row + x * 2 + 1);
            if (t->fmt == G_IM_FMT_IA) {
                out[0] = out[1] = out[2] = (uint8_t) (c >> 8);
                out[3] = (uint8_t) c;
            } else {
                out[0] = expand5(c >> 11), out[1] = expand5((c >> 6) & 31), out[2] = expand5((c >> 1) & 31);
                out[3] = (c & 1) ? 255 : 0;
            }
            break;
        }
        default:
            for (int i = 0; i < 4; ++i) {
                out[i] = tmem_byte(f, row + x * 4 + i);
            }
            break;
    }
}

static uint32_t tile_extent(uint16_t lo, uint16_t hi, uint8_t mask, uint8_t mode) {
    uint32_t size = hi >= lo ? ((uint32_t) (hi - lo) >> 2) + 1 : 1;
    if (mask && !(mode & G_TX_CLAMP)) {
        size = 1u << mask; // it repeats with the mask's period
    }
    return size > 1024 ? 1024 : size;
}

static void *cached_texture(f3d *f, uint64_t key, uint32_t width, uint32_t height, const struct tile *t) {
    if (f->cache_count * 2 >= f->cache_capacity) {
        // Grow, dropping what has not been used for a while.
        const uint32_t capacity = f->cache_capacity ? f->cache_capacity * 2 : 1024;
        struct cached *old = f->cache;
        const uint32_t old_capacity = f->cache_capacity;
        f->cache = calloc(capacity, sizeof(*f->cache));
        f->cache_capacity = capacity;
        f->cache_count = 0;
        for (uint32_t i = 0; i < old_capacity; ++i) {
            if (old[i].texture) {
                uint32_t s = (uint32_t) old[i].key & (capacity - 1);
                while (f->cache[s].texture) {
                    s = (s + 1) & (capacity - 1);
                }
                f->cache[s] = old[i];
                ++f->cache_count;
            }
        }
        free(old);
    }
    uint32_t s = (uint32_t) key & (f->cache_capacity - 1);
    while (f->cache[s].texture) {
        if (f->cache[s].key == key) {
            f->cache[s].used = f->frame;
            return f->cache[s].texture;
        }
        s = (s + 1) & (f->cache_capacity - 1);
    }
    const uint32_t stride = t->siz == G_IM_SIZ_32b ? t->line * 16u : t->line * 8u;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            texel(f, t, x, y, stride, &f->rgba[(y * width + x) * 4]);
        }
    }
    void *texture = f->backend.texture_create(f->backend.user, f->rgba, width, height);
    if (texture) {
        f->cache[s] = (struct cached) { key, texture, f->frame };
        ++f->cache_count;
    }
    return texture;
}

// The texture of render tile i (0, 1): from texture memory as the tile reads it.
static void resolve(f3d *f, int i) {
    const struct tile *t = &f->tiles[(f->tex_tile + i) & 7];
    struct resolved *r = &f->resolved[i];
    const uint32_t width = tile_extent(t->uls, t->lrs, t->masks, t->cms);
    const uint32_t height = tile_extent(t->ult, t->lrt, t->maskt, t->cmt);
    const uint32_t stride = t->siz == G_IM_SIZ_32b ? t->line * 16u : t->line * 8u;
    uint32_t bytes = height * stride;
    bytes = bytes > TMEM_SIZE ? TMEM_SIZE : bytes;
    const uint32_t describe[8] = { t->fmt, t->siz, width, height, stride, t->palette, (f->other_h >> G_MDSFT_TEXTLUT) & 3,
                                   t->tmem };
    uint64_t key = fnv(0xcbf29ce484222325ull, describe, sizeof(describe));
    for (uint32_t b = 0; b < bytes; ++b) {
        key = (key ^ tmem_byte(f, t->tmem * 8u + b)) * 0x100000001b3ull;
    }
    if (t->fmt == G_IM_FMT_CI) {
        key = fnv(key, &f->tmem[0x800], 0x800);
    }
    r->texture = cached_texture(f, key, width, height, t);
    r->width = (float) width;
    r->height = (float) height;
    // Copy mode moves texels as they are, whatever the filter.
    r->sampler.linear = ((f->other_h >> G_MDSFT_TEXTFILT) & 3) != 0 && ((f->other_h >> G_MDSFT_CYCLETYPE) & 3) != 2;
    r->sampler.wrap_s = (t->cms & G_TX_CLAMP) ? F3D_WRAP_CLAMP : (t->cms & G_TX_MIRROR) ? F3D_WRAP_MIRROR : F3D_WRAP_REPEAT;
    r->sampler.wrap_t = (t->cmt & G_TX_CLAMP) ? F3D_WRAP_CLAMP : (t->cmt & G_TX_MIRROR) ? F3D_WRAP_MIRROR : F3D_WRAP_REPEAT;
}

// A texture coordinate (in texels) as tile i reads it: shifted, from the tile's
// corner, normalized.
static float tile_coordinate(const f3d *f, int i, float texels, bool is_t) {
    const struct tile *t = &f->tiles[(f->tex_tile + i) & 7];
    const uint8_t shift = is_t ? t->shiftt : t->shifts;
    if (shift >= 1 && shift <= 10) {
        texels /= (float) (1u << shift);
    } else if (shift >= 11) {
        texels *= (float) (1u << (16 - shift));
    }
    texels -= (is_t ? t->ult : t->uls) / 4.0f;
    if (f->resolved[i].sampler.linear) {
        texels += 0.5f;
    }
    const float size = is_t ? f->resolved[i].height : f->resolved[i].width;
    return texels / (size > 0.0f ? size : 1.0f);
}

// Which textures the combiner reads, in either cycle (the second cycle reads
// them swapped, so both count).
static void combiner_textures(uint32_t w0, uint32_t w1, bool two_cycle, bool used[2]) {
    const uint32_t rgb[8] = { (w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7,
                              (w0 >> 5) & 15,  (w1 >> 24) & 15, w0 & 31,         (w1 >> 6) & 7 };
    const uint32_t alpha[8] = { (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7,
                                (w1 >> 21) & 7, (w1 >> 3) & 7,  (w1 >> 18) & 7, w1 & 7 };
    used[0] = used[1] = false;
    for (int i = 0; i < (two_cycle ? 8 : 4); ++i) {
        const bool second = i >= 4;
        bool t0 = rgb[i] == G_CCMUX_TEXEL0 || alpha[i] == G_ACMUX_TEXEL0;
        bool t1 = rgb[i] == G_CCMUX_TEXEL1 || alpha[i] == G_ACMUX_TEXEL1;
        if (i % 4 == 2) { // c also reads the textures' alphas
            t0 = t0 || rgb[i] == G_CCMUX_TEXEL0_ALPHA;
            t1 = t1 || rgb[i] == G_CCMUX_TEXEL1_ALPHA;
        }
        used[second ? 1 : 0] |= t0;
        used[second ? 0 : 1] |= t1;
    }
}

// --- Batches ---------------------------------------------------------------------------

static void flush(f3d *f) {
    if (f->batch_count > 0 && f->have_state) {
        f->backend.draw(f->backend.user, &f->state, f->batch, f->batch_count);
    }
    f->batch_count = 0;
}

static void color4(float out[4], uint32_t rgba) {
    out[0] = (rgba >> 24) / 255.0f;
    out[1] = ((rgba >> 16) & 255) / 255.0f;
    out[2] = ((rgba >> 8) & 255) / 255.0f;
    out[3] = (rgba & 255) / 255.0f;
}

enum shape { TRIANGLE, TEXTURE_RECTANGLE, FILL_RECTANGLE };

// The state a triangle or rectangle is drawn with; a new batch if it differs.
static void use_state(f3d *f, enum shape shape) {
    struct f3d_state s;
    memset(&s, 0, sizeof(s));
    const uint32_t cycle = (f->other_h >> G_MDSFT_CYCLETYPE) & 3;
    const bool two = cycle == 1;
    if (cycle == 3) {
        s.flags |= F3D_FILL;
    } else if (cycle == 2) {
        s.flags |= F3D_COPY;
    } else if (two) {
        s.flags |= F3D_TWO_CYCLE;
    }
    // The blender's last cycle decides whether it blends; its first whether
    // it mixes in the fog color.
    const uint32_t last = two ? f->other_l >> 16 : f->other_l >> 18;
    const uint32_t m = (last >> 4) & 3, b = last & 3;
    if ((f->other_l & FORCE_BL) && m == G_BL_CLR_MEM && b == G_BL_1MA && cycle < 2) {
        s.flags |= F3D_BLEND;
    }
    if (cycle < 2 && ((f->other_l >> 30) & 3) == G_BL_CLR_FOG) {
        s.flags |= F3D_FOG;
    }
    if ((f->other_l & 3) == G_AC_THRESHOLD) {
        s.flags |= F3D_ALPHA_TEST;
        s.alpha_threshold = f->blend[3];
    }
    if ((f->other_l & CVG_X_ALPHA) && cycle < 2) {
        s.flags |= F3D_COVERAGE_ALPHA;
    }
    if (shape == TRIANGLE && (f->geometry & G_ZBUFFER)) {
        if (f->other_l & Z_CMP) {
            s.flags |= F3D_DEPTH_TEST;
        }
        if (f->other_l & Z_UPD) {
            s.flags |= F3D_DEPTH_WRITE;
        }
        if ((f->other_l & ZMODE_DEC) == ZMODE_DEC) {
            s.flags |= F3D_DECAL;
        }
    }
    s.combine[0] = f->combine[0];
    s.combine[1] = f->combine[1];
    if (((f->combine[0] >> 20) & 15) == G_CCMUX_NOISE || ((f->combine[0] >> 5) & 15) == G_CCMUX_NOISE) {
        s.flags |= F3D_NOISE;
    }
    memcpy(s.prim, f->prim, sizeof(s.prim));
    memcpy(s.env, f->env, sizeof(s.env));
    memcpy(s.fog, f->fog, sizeof(s.fog));
    s.prim_lod_frac = f->prim_lod_frac;

    bool used[2] = { false, false };
    if (cycle == 2) {
        used[0] = true;
    } else if (cycle < 2 && (shape == TEXTURE_RECTANGLE || f->tex_on)) {
        combiner_textures(f->combine[0], f->combine[1], two, used);
    }
    if (f->textures_dirty) {
        f->resolved[0].texture = f->resolved[1].texture = NULL;
        f->textures_dirty = false;
    }
    for (int i = 0; i < 2; ++i) {
        if (used[i]) {
            if (!f->resolved[i].texture) {
                resolve(f, i);
            }
            s.textures[i] = f->resolved[i].texture;
            s.samplers[i] = f->resolved[i].sampler;
        }
    }

    if (shape == TRIANGLE) {
        for (int i = 0; i < 4; ++i) {
            s.viewport[i] = (int32_t) lroundf(f->viewport[i]);
        }
    } else {
        s.viewport[2] = (int32_t) f->width;
        s.viewport[3] = (int32_t) f->height;
    }
    for (int i = 0; i < 4; ++i) {
        s.scissor[i] = (int32_t) lroundf(f->scissor[i]);
    }
    if (!f->have_state || memcmp(&s, &f->state, sizeof(s)) != 0) {
        flush(f);
        f->state = s;
        f->have_state = true;
    }
}

static struct f3d_vertex *emit(f3d *f, uint32_t count) {
    if (f->batch_count + count > BATCH_VERTICES) {
        flush(f);
    }
    struct f3d_vertex *v = &f->batch[f->batch_count];
    f->batch_count += count;
    return v;
}

// --- Triangles ------------------------------------------------------------------------

static void triangle(f3d *f, int a, int b, int c) {
    if (a >= VERTEX_SLOTS || b >= VERTEX_SLOTS || c >= VERTEX_SLOTS) {
        return;
    }
    const struct vertex *v[3] = { &f->vertices[a], &f->vertices[b], &f->vertices[c] };
    // Entirely outside one side of the view.
    for (int axis = 0; axis < 3; ++axis) {
        if (axis < 2) {
            if (v[0]->clip[axis] > v[0]->clip[3] && v[1]->clip[axis] > v[1]->clip[3]
                && v[2]->clip[axis] > v[2]->clip[3]) {
                return;
            }
        }
        if (v[0]->clip[axis] < -v[0]->clip[3] && v[1]->clip[axis] < -v[1]->clip[3]
            && v[2]->clip[axis] < -v[2]->clip[3]) {
            return;
        }
    }
    const uint32_t cull = f->geometry & G_CULL_BOTH;
    if (cull == G_CULL_BOTH) {
        return;
    }
    if (cull && v[0]->clip[3] > 0.0f && v[1]->clip[3] > 0.0f && v[2]->clip[3] > 0.0f) {
        float x[3], y[3];
        for (int i = 0; i < 3; ++i) {
            x[i] = v[i]->clip[0] / v[i]->clip[3];
            y[i] = v[i]->clip[1] / v[i]->clip[3];
        }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if ((cull == G_CULL_BACK && area < 0.0f) || (cull == G_CULL_FRONT && area > 0.0f)) {
            return;
        }
    }
    use_state(f, TRIANGLE);
    struct f3d_vertex *out = emit(f, 3);
    const bool smooth = (f->geometry & G_SHADING_SMOOTH) != 0;
    for (int i = 0; i < 3; ++i) {
        struct f3d_vertex *o = &out[i];
        o->x = v[i]->clip[0], o->y = v[i]->clip[1], o->z = v[i]->clip[2], o->w = v[i]->clip[3];
        const float *color = smooth ? v[i]->color : v[0]->color;
        o->r = color[0], o->g = color[1], o->b = color[2], o->a = color[3];
        o->fog = v[i]->fog;
        o->u0 = tile_coordinate(f, 0, v[i]->s / 32.0f, false);
        o->v0 = tile_coordinate(f, 0, v[i]->t / 32.0f, true);
        o->u1 = tile_coordinate(f, 1, v[i]->s / 32.0f, false);
        o->v1 = tile_coordinate(f, 1, v[i]->t / 32.0f, true);
    }
}

// --- Rectangles ----------------------------------------------------------------------

// A rectangle in the N64's screen (whole pixels, lower right exclusive) with
// its corners' texture coordinates in texels, for tiles 0 and 1.
static void rectangle(f3d *f, enum shape shape, float x0, float y0, float x1, float y1, const float st[4],
                      const float color[4]) {
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    const bool full = x0 <= 0.5f && x1 >= N64_WIDTH - 0.5f;
    const float px0 = map_x(f, x0, full), px1 = map_x(f, x1, full), py0 = map_y(f, y0), py1 = map_y(f, y1);
    use_state(f, shape);
    struct f3d_vertex *out = emit(f, 6);
    const float corners[4][2] = { { px0, py0 }, { px1, py0 }, { px0, py1 }, { px1, py1 } };
    const float cs[4][2] = { { st[0], st[1] }, { st[2], st[1] }, { st[0], st[3] }, { st[2], st[3] } };
    const int order[6] = { 0, 1, 2, 1, 3, 2 };
    for (int k = 0; k < 6; ++k) {
        const int c = order[k];
        struct f3d_vertex *o = &out[k];
        o->x = corners[c][0] / f->width * 2.0f - 1.0f;
        o->y = 1.0f - corners[c][1] / f->height * 2.0f;
        o->z = -1.0f;
        o->w = 1.0f;
        o->r = color[0], o->g = color[1], o->b = color[2], o->a = color[3];
        o->fog = 0.0f;
        o->u0 = tile_coordinate(f, 0, cs[c][0], false);
        o->v0 = tile_coordinate(f, 0, cs[c][1], true);
        o->u1 = tile_coordinate(f, 1, cs[c][0], false);
        o->v1 = tile_coordinate(f, 1, cs[c][1], true);
    }
}

static void fill_rectangle(f3d *f, uint32_t w0, uint32_t w1) {
    if (f->cimg == f->zimg) {
        return; // clearing the depth buffer, which each frame starts cleared
    }
    float x0 = ((w1 >> 12) & 0xfff) / 4.0f, y0 = (w1 & 0xfff) / 4.0f;
    float x1 = ((w0 >> 12) & 0xfff) / 4.0f, y1 = (w0 & 0xfff) / 4.0f;
    const uint32_t cycle = (f->other_h >> G_MDSFT_CYCLETYPE) & 3;
    float color[4] = { 1, 1, 1, 1 };
    if (cycle >= 2) {
        x1 += 1.0f, y1 += 1.0f; // inclusive in fill and copy mode
        const uint32_t c = f->fill_color >> 16;
        color[0] = expand5(c >> 11) / 255.0f, color[1] = expand5((c >> 6) & 31) / 255.0f;
        color[2] = expand5((c >> 1) & 31) / 255.0f, color[3] = (c & 1) ? 1.0f : 0.0f;
    }
    const float st[4] = { 0, 0, 0, 0 };
    rectangle(f, FILL_RECTANGLE, x0, y0, x1, y1, st, color);
}

static void texture_rectangle(f3d *f, uint32_t w0, uint32_t w1, uint32_t st, uint32_t d, bool flip) {
    if (f->cimg == f->zimg) {
        return;
    }
    const uint8_t tile = (w1 >> 24) & 7;
    float x0 = ((w1 >> 12) & 0xfff) / 4.0f, y0 = (w1 & 0xfff) / 4.0f;
    float x1 = ((w0 >> 12) & 0xfff) / 4.0f, y1 = (w0 & 0xfff) / 4.0f;
    float s = (int16_t) (st >> 16) / 32.0f, t = (int16_t) st / 32.0f;
    float dsdx = (int16_t) (d >> 16) / 1024.0f, dtdy = (int16_t) d / 1024.0f;
    const uint32_t cycle = (f->other_h >> G_MDSFT_CYCLETYPE) & 3;
    if (cycle == 2) {
        x1 += 1.0f, y1 += 1.0f;
        dsdx /= 4.0f; // copy mode moves four texels a pixel
    }
    const uint8_t saved = f->tex_tile;
    f->tex_tile = tile;
    if (saved != tile) {
        f->textures_dirty = true;
    }
    float corners[4];
    if (flip) {
        corners[0] = s, corners[1] = t, corners[2] = s + (y1 - y0) * dsdx, corners[3] = t + (x1 - x0) * dtdy;
    } else {
        corners[0] = s, corners[1] = t, corners[2] = s + (x1 - x0) * dsdx, corners[3] = t + (y1 - y0) * dtdy;
    }
    const float white[4] = { 1, 1, 1, 1 };
    if (!flip) {
        rectangle(f, TEXTURE_RECTANGLE, x0, y0, x1, y1, corners, white);
    } else {
        // Flipped: s runs down and t across. Two triangles by hand.
        const float st_flip[4] = { corners[0], corners[1], corners[2], corners[3] };
        const bool full = x0 <= 0.5f && x1 >= N64_WIDTH - 0.5f;
        const float px0 = map_x(f, x0, full), px1 = map_x(f, x1, full), py0 = map_y(f, y0), py1 = map_y(f, y1);
        use_state(f, TEXTURE_RECTANGLE);
        struct f3d_vertex *out = emit(f, 6);
        const float pos[4][2] = { { px0, py0 }, { px1, py0 }, { px0, py1 }, { px1, py1 } };
        const float tex[4][2] = { { st_flip[0], st_flip[1] }, { st_flip[0], st_flip[3] },
                                  { st_flip[2], st_flip[1] }, { st_flip[2], st_flip[3] } };
        const int order[6] = { 0, 1, 2, 1, 3, 2 };
        for (int k = 0; k < 6; ++k) {
            const int c = order[k];
            struct f3d_vertex *o = &out[k];
            o->x = pos[c][0] / f->width * 2.0f - 1.0f;
            o->y = 1.0f - pos[c][1] / f->height * 2.0f;
            o->z = -1.0f, o->w = 1.0f;
            o->r = o->g = o->b = o->a = 1.0f;
            o->fog = 0.0f;
            o->u0 = tile_coordinate(f, 0, tex[c][0], false);
            o->v0 = tile_coordinate(f, 0, tex[c][1], true);
            o->u1 = tile_coordinate(f, 1, tex[c][0], false);
            o->v1 = tile_coordinate(f, 1, tex[c][1], true);
        }
    }
    if (saved != tile) {
        flush(f);
        f->tex_tile = saved;
        f->textures_dirty = true;
    }
}

// --- Texture memory ---------------------------------------------------------------

static void set_texture_image(f3d *f, uint32_t w0, const void *address) {
    const void *rom = sm64_texture(address);
    f->timg = rom ? rom : address;
    f->timg_siz = (w0 >> 19) & 3;
    f->timg_width = (w0 & 0xfff) + 1;
}

static uint32_t image_bytes(uint32_t texels, uint8_t siz) {
    return (texels << siz) >> 1;
}

static void load_block(f3d *f, uint32_t w0, uint32_t w1) {
    const struct tile *t = &f->tiles[(w1 >> 24) & 7];
    const uint32_t uls = (w0 >> 12) & 0xfff, ult = w0 & 0xfff, lrs = (w1 >> 12) & 0xfff;
    if (!f->timg || lrs < uls) {
        return;
    }
    const uint32_t start = image_bytes(ult * f->timg_width + uls, f->timg_siz);
    uint32_t bytes = image_bytes(lrs - uls + 1, f->timg_siz);
    const uint32_t dest = t->tmem * 8u;
    if (dest >= TMEM_SIZE) {
        return;
    }
    bytes = dest + bytes > TMEM_SIZE ? TMEM_SIZE - dest : bytes;
    memcpy(&f->tmem[dest], f->timg + start, bytes);
    f->textures_dirty = true;
}

static void load_tile(f3d *f, uint32_t w0, uint32_t w1) {
    const struct tile *t = &f->tiles[(w1 >> 24) & 7];
    const uint32_t uls = ((w0 >> 12) & 0xfff) >> 2, ult = (w0 & 0xfff) >> 2;
    const uint32_t lrs = ((w1 >> 12) & 0xfff) >> 2, lrt = (w1 & 0xfff) >> 2;
    if (!f->timg || lrs < uls || lrt < ult) {
        return;
    }
    const uint32_t row_bytes = image_bytes(lrs - uls + 1, f->timg_siz);
    const uint32_t stride = t->line * 8u;
    for (uint32_t y = 0; y <= lrt - ult; ++y) {
        const uint32_t dest = t->tmem * 8u + y * stride;
        if (dest >= TMEM_SIZE) {
            break;
        }
        const uint32_t n = dest + row_bytes > TMEM_SIZE ? TMEM_SIZE - dest : row_bytes;
        memcpy(&f->tmem[dest], f->timg + image_bytes((ult + y) * f->timg_width + uls, f->timg_siz), n);
    }
    f->textures_dirty = true;
}

static void load_tlut(f3d *f, uint32_t w1) {
    const struct tile *t = &f->tiles[(w1 >> 24) & 7];
    const uint32_t count = ((w1 >> 14) & 0x3ff) + 1;
    const uint32_t dest = t->tmem * 8u;
    if (!f->timg || dest >= TMEM_SIZE) {
        return;
    }
    const uint32_t bytes = dest + count * 2 > TMEM_SIZE ? TMEM_SIZE - dest : count * 2;
    memcpy(&f->tmem[dest], f->timg, bytes);
    f->textures_dirty = true;
}

static void set_tile(f3d *f, uint32_t w0, uint32_t w1) {
    struct tile *t = &f->tiles[(w1 >> 24) & 7];
    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1ff;
    t->tmem = w0 & 0x1ff;
    t->palette = (w1 >> 20) & 15;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 15;
    t->shiftt = (w1 >> 10) & 15;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 15;
    t->shifts = w1 & 15;
    f->textures_dirty = true;
}

static void set_tile_size(f3d *f, uint32_t w0, uint32_t w1) {
    struct tile *t = &f->tiles[(w1 >> 24) & 7];
    t->uls = (w0 >> 12) & 0xfff;
    t->ult = w0 & 0xfff;
    t->lrs = (w1 >> 12) & 0xfff;
    t->lrt = w1 & 0xfff;
    f->textures_dirty = true;
}

// --- The display list ---------------------------------------------------------------

static void set_scissor(f3d *f, uint32_t w0, uint32_t w1) {
    const float x0 = ((w0 >> 12) & 0xfff) / 4.0f, y0 = (w0 & 0xfff) / 4.0f;
    const float x1 = ((w1 >> 12) & 0xfff) / 4.0f, y1 = (w1 & 0xfff) / 4.0f;
    const bool full = x0 <= 0.5f && x1 >= N64_WIDTH - 0.5f;
    float px0 = map_x(f, x0, full), px1 = map_x(f, x1, full);
    px0 = px0 < 0.0f ? 0.0f : px0;
    px1 = px1 > f->width ? f->width : px1;
    f->scissor[0] = px0;
    f->scissor[1] = map_y(f, y0);
    f->scissor[2] = px1 > px0 ? px1 - px0 : 0.0f;
    f->scissor[3] = map_y(f, y1) - f->scissor[1];
}

static void move_word(f3d *f, uint32_t w0, uint32_t data) {
    const uint32_t index = w0 & 0xff, offset = (w0 >> 8) & 0xffff;
    switch (index) {
        case G_MW_NUMLIGHT:
            f->num_lights = (int) ((data - 0x80000000u) / 32) - 1;
            f->num_lights = f->num_lights < 0 ? 0 : f->num_lights > 7 ? 7 : f->num_lights;
            f->lights_dirty = true;
            break;
        case G_MW_FOG:
            f->fog_mul = (int16_t) (data >> 16);
            f->fog_offset = (int16_t) data;
            break;
        case G_MW_LIGHTCOL:
            if ((offset & 7) == 0 && offset / 32 < 8) {
                struct light *l = &f->lights[offset / 32];
                l->color[0] = (data >> 24) / 255.0f;
                l->color[1] = ((data >> 16) & 255) / 255.0f;
                l->color[2] = ((data >> 8) & 255) / 255.0f;
            }
            break;
    }
}

static void move_mem(f3d *f, uint32_t w0, const void *address) {
    const uint32_t index = (w0 >> 16) & 0xff;
    if (index == G_MV_VIEWPORT) {
        set_viewport(f, address);
    } else if (index == G_MV_LOOKATX || index == G_MV_LOOKATY) {
        struct light l;
        read_light(&l, address);
        memcpy(f->lookat[index == G_MV_LOOKATX ? 0 : 1], l.dir, sizeof(l.dir));
        f->lights_dirty = true;
    } else if (index >= G_MV_L0 && index <= G_MV_L7) {
        read_light(&f->lights[(index - G_MV_L0) / 2], address);
        f->lights_dirty = true;
    }
}

static void matrix(f3d *f, uint32_t w0, const void *address) {
    const uint32_t params = (w0 >> 16) & 0xff;
    float m[4][4];
    read_matrix(m, address);
    if (params & G_MTX_PROJECTION) {
        if (params & G_MTX_LOAD) {
            memcpy(f->projection, m, sizeof(m));
        } else {
            mul(f->projection, m, f->projection);
        }
    } else {
        if ((params & G_MTX_PUSH) && f->depth + 1 < STACK_DEPTH) {
            memcpy(f->modelview[f->depth + 1], f->modelview[f->depth], sizeof(m));
            ++f->depth;
        }
        if (params & G_MTX_LOAD) {
            memcpy(f->modelview[f->depth], m, sizeof(m));
        } else {
            mul(f->modelview[f->depth], m, f->modelview[f->depth]);
        }
        f->lights_dirty = true;
    }
    f->mvp_dirty = true;
}

// Where the game applies its 3D camera: seen through the camera asked for.
static void camera_tag(f3d *f, const float (*game)[4]) {
    float inverse[4][4];
    if (f->camera && invert(inverse, game)) {
        mul(f->projection, inverse, f->camera_view);
        f->mvp_dirty = true;
    }
}

static void reset(f3d *f) {
    f->depth = 0;
    identity(f->modelview[0]);
    identity(f->projection);
    f->mvp_dirty = true;
    f->geometry = 0;
    memset(f->lights, 0, sizeof(f->lights));
    f->num_lights = 1;
    f->lookat[0][0] = 1.0f, f->lookat[0][1] = 0.0f, f->lookat[0][2] = 0.0f;
    f->lookat[1][0] = 0.0f, f->lookat[1][1] = 1.0f, f->lookat[1][2] = 0.0f;
    f->lights_dirty = true;
    f->fog_mul = f->fog_offset = 0;
    f->tex_scale_s = f->tex_scale_t = 1.0f;
    f->tex_tile = 0;
    f->tex_on = false;
    const Vp_t full = { { 640, 480, 511, 0 }, { 640, 480, 511, 0 } };
    set_viewport(f, &full);
    f->other_h = f->other_l = 0;
    f->combine[0] = f->combine[1] = 0;
    memset(f->prim, 0, sizeof(f->prim));
    memset(f->env, 0, sizeof(f->env));
    memset(f->fog, 0, sizeof(f->fog));
    memset(f->blend, 0, sizeof(f->blend));
    f->prim_lod_frac = 0.0f;
    f->fill_color = 0;
    f->timg = NULL;
    f->cimg = 1;
    f->zimg = 0;
    memset(f->tiles, 0, sizeof(f->tiles));
    memset(f->tmem, 0, sizeof(f->tmem));
    f->textures_dirty = true;
    f->scissor[0] = f->scissor[1] = 0.0f;
    f->scissor[2] = f->width;
    f->scissor[3] = f->height;
    f->have_state = false;
    f->batch_count = 0;
}

static void run(f3d *f, const Gfx *list) {
    const Gfx *stack[DL_DEPTH];
    int depth = 0;
    const Gfx *cmd = list;
    for (;;) {
        const uint32_t w0 = (uint32_t) cmd->words.w0;
        const uintptr_t w1 = cmd->words.w1;
        const uint32_t d = (uint32_t) w1;
        switch ((uint8_t) (w0 >> 24)) {
            case OP(G_MTX):
                matrix(f, w0, (const void *) w1);
                break;
            case OP(G_POPMTX):
                if (f->depth > 0) {
                    --f->depth;
                    f->mvp_dirty = f->lights_dirty = true;
                }
                break;
            case OP(G_MOVEMEM):
                move_mem(f, w0, (const void *) w1);
                break;
            case OP(G_MOVEWORD):
                move_word(f, w0, d);
                break;
            case OP(G_VTX):
                load_vertices(f, (const Vtx *) w1, (int) ((w0 >> 20) & 15) + 1, (int) ((w0 >> 16) & 15));
                break;
            case OP(G_TRI1):
                triangle(f, (int) ((d >> 16) & 0xff) / 10, (int) ((d >> 8) & 0xff) / 10, (int) (d & 0xff) / 10);
                break;
            case OP(G_DL):
                if (((w0 >> 16) & 0xff) == G_DL_NOPUSH) {
                    cmd = (const Gfx *) w1;
                    continue;
                }
                if (depth < DL_DEPTH) {
                    stack[depth++] = cmd + 1;
                    cmd = (const Gfx *) w1;
                    continue;
                }
                break;
            case OP(G_ENDDL):
                if (depth == 0) {
                    return;
                }
                cmd = stack[--depth];
                continue;
            case OP(G_SETGEOMETRYMODE):
                f->geometry |= d;
                break;
            case OP(G_CLEARGEOMETRYMODE):
                f->geometry &= ~d;
                break;
            case OP(G_TEXTURE):
                f->tex_scale_s = (d >> 16) / 65536.0f;
                f->tex_scale_t = (d & 0xffff) / 65536.0f;
                f->tex_tile = (w0 >> 8) & 7;
                f->tex_on = (w0 & 0xff) != 0;
                f->textures_dirty = true;
                break;
            case OP(G_SETOTHERMODE_H):
            case OP(G_SETOTHERMODE_L): {
                const uint32_t shift = (w0 >> 8) & 0xff, length = w0 & 0xff;
                const uint32_t mask = (length >= 32 ? 0xffffffffu : ((1u << length) - 1)) << shift;
                uint32_t *mode = (uint8_t) (w0 >> 24) == OP(G_SETOTHERMODE_H) ? &f->other_h : &f->other_l;
                *mode = (*mode & ~mask) | (d & mask);
                f->textures_dirty = true; // the filter and palette type are in there
                break;
            }
            case OP(G_RDPSETOTHERMODE):
                f->other_h = w0 & 0xffffff;
                f->other_l = d;
                f->textures_dirty = true;
                break;
            case OP(G_NOOP):
                if ((w0 & 0xffffff) == SM64_CAMERA_TAG && w1) {
                    camera_tag(f, (const float (*)[4]) w1);
                }
                break;
            case OP(G_SETCOMBINE):
                f->combine[0] = w0 & 0xffffff;
                f->combine[1] = d;
                break;
            case OP(G_SETPRIMCOLOR):
                color4(f->prim, d);
                f->prim_lod_frac = (w0 & 0xff) / 255.0f;
                break;
            case OP(G_SETENVCOLOR):
                color4(f->env, d);
                break;
            case OP(G_SETFOGCOLOR):
                color4(f->fog, d);
                break;
            case OP(G_SETBLENDCOLOR):
                color4(f->blend, d);
                break;
            case OP(G_SETFILLCOLOR):
                f->fill_color = d;
                break;
            case OP(G_SETTIMG):
                set_texture_image(f, w0, (const void *) w1);
                break;
            case OP(G_SETTILE):
                set_tile(f, w0, d);
                break;
            case OP(G_SETTILESIZE):
                set_tile_size(f, w0, d);
                break;
            case OP(G_LOADBLOCK):
                load_block(f, w0, d);
                break;
            case OP(G_LOADTILE):
                load_tile(f, w0, d);
                break;
            case OP(G_LOADTLUT):
                load_tlut(f, d);
                break;
            case OP(G_SETSCISSOR):
                set_scissor(f, w0, d);
                break;
            case OP(G_SETCIMG):
                f->cimg = w1;
                break;
            case OP(G_SETZIMG):
                f->zimg = w1;
                break;
            case OP(G_FILLRECT):
                fill_rectangle(f, w0, d);
                break;
            case OP(G_TEXRECT):
            case OP(G_TEXRECTFLIP):
                // Its texture coordinates are in the next two commands.
                texture_rectangle(f, w0, d, (uint32_t) cmd[1].words.w1, (uint32_t) cmd[2].words.w1,
                                  (uint8_t) (w0 >> 24) == OP(G_TEXRECTFLIP));
                cmd += 2;
                break;
            default:
                break;
        }
        ++cmd;
    }
}

f3d *f3d_create(const struct f3d_backend *backend) {
    f3d *f = calloc(1, sizeof(*f));
    if (f) {
        f->backend = *backend;
    }
    return f;
}

void f3d_destroy(f3d *f) {
    if (!f) {
        return;
    }
    for (uint32_t i = 0; i < f->cache_capacity; ++i) {
        if (f->cache[i].texture) {
            f->backend.texture_destroy(f->backend.user, f->cache[i].texture);
        }
    }
    free(f->cache);
    free(f);
}

// Textures no frame has used for a while go; the rest move up to close gaps.
static void evict(f3d *f) {
    if (!f->cache) {
        return;
    }
    bool removed = false;
    for (uint32_t i = 0; i < f->cache_capacity; ++i) {
        if (f->cache[i].texture && f->frame - f->cache[i].used > CACHE_KEEP_FRAMES) {
            f->backend.texture_destroy(f->backend.user, f->cache[i].texture);
            f->cache[i].texture = NULL;
            --f->cache_count;
            removed = true;
        }
    }
    if (removed) {
        // Rehash in place: open addressing needs no gaps in a probe run.
        struct cached *old = f->cache;
        f->cache = calloc(f->cache_capacity, sizeof(*f->cache));
        for (uint32_t i = 0; i < f->cache_capacity; ++i) {
            if (old[i].texture) {
                uint32_t s = (uint32_t) old[i].key & (f->cache_capacity - 1);
                while (f->cache[s].texture) {
                    s = (s + 1) & (f->cache_capacity - 1);
                }
                f->cache[s] = old[i];
            }
        }
        free(old);
    }
}

void f3d_run(f3d *f, const void *display_list, uint32_t width, uint32_t height, const float (*camera)[4]) {
    ++f->frame;
    evict(f);
    f->width = (float) (width ? width : 1);
    f->height = (float) (height ? height : 1);
    f->camera = camera != NULL;
    if (camera) {
        memcpy(f->camera_view, camera, sizeof(f->camera_view));
    }
    reset(f);
    run(f, display_list);
    flush(f);
}
