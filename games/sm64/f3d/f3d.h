// Draws the display lists libsm64_physics hands over (sm64_step_draw): the
// N64's Fast3D geometry microcode (F3D, and F3D_NEW for EU) and the RDP as
// far as Super Mario 64 uses them, in C. It transforms and lights vertices,
// keeps the RDP's texture memory and state, and hands a backend batches of
// triangles with everything they are drawn with; the color combiner itself
// is left to the backend's shader (shaders/f3d.frag), which evaluates the
// combine words as the RDP does.
//
// Written from the display list format of the SDK's gbi.h (the library's
// game/include/PR/gbi.h).
#ifndef F3D_H
#define F3D_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A vertex as drawn: position in OpenGL's clip space (Y up, Z from -W to W),
// texture coordinates for the two tiles, normalized to their textures, shade
// color and fog factor, 0 to 1.
struct f3d_vertex {
    float x, y, z, w;
    float u0, v0, u1, v1;
    float r, g, b, a;
    float fog;
};

// How a batch is drawn (flags).
enum {
    F3D_TWO_CYCLE = 1 << 0,     // the combiner's second cycle follows the first
    F3D_FILL = 1 << 1,          // fill mode: the shade color as it is
    F3D_COPY = 1 << 2,          // copy mode: texture 0 as it is
    F3D_FOG = 1 << 3,           // the blender mixes in the fog color by the fog factor
    F3D_ALPHA_TEST = 1 << 4,    // alpha compare: drop pixels below alpha_threshold
    F3D_COVERAGE_ALPHA = 1 << 5, // coverage times alpha: drop pixels below half alpha
    F3D_BLEND = 1 << 6,         // blend with what is drawn by alpha
    F3D_DEPTH_TEST = 1 << 7,
    F3D_DEPTH_WRITE = 1 << 8,
    F3D_DECAL = 1 << 9,         // coplanar with what is drawn: pulled toward the eye
    F3D_NOISE = 1 << 10,        // the combiner reads noise
};

// A texture's sampler: linear filtering, and per axis repeat, mirror or clamp.
enum { F3D_WRAP_REPEAT = 0, F3D_WRAP_MIRROR = 1, F3D_WRAP_CLAMP = 2 };
struct f3d_sampler {
    bool linear;
    uint8_t wrap_s, wrap_t;
};

// Everything a batch of triangles shares. Rectangles are in the target's
// pixels, from its top left.
struct f3d_state {
    uint32_t flags;
    uint32_t combine[2]; // G_SETCOMBINE's words: the combiner's inputs per cycle
    float prim[4], env[4], fog[4];
    float alpha_threshold, prim_lod_frac;
    void *textures[2]; // the backend's, or NULL
    struct f3d_sampler samplers[2];
    int32_t viewport[4], scissor[4]; // x, y, width, height
};

struct f3d_backend {
    void *user;
    // A texture of width x height RGBA8 pixels; NULL when it cannot.
    void *(*texture_create)(void *user, const uint8_t *rgba, uint32_t width, uint32_t height);
    // Only between frames: no draw of the frame being made names it.
    void (*texture_destroy)(void *user, void *texture);
    // count vertices, three per triangle.
    void (*draw)(void *user, const struct f3d_state *state, const struct f3d_vertex *vertices, uint32_t count);
};

typedef struct f3d f3d;

f3d *f3d_create(const struct f3d_backend *backend);
void f3d_destroy(f3d *f3d);

// Draws one frame's display list into a target of width x height pixels.
// With a camera (row vectors: a point in the world times it is the point in
// OpenGL's clip space), the game's 3D scenes are drawn through it instead of
// the game's camera (SM64_CAMERA_TAG). The 3D takes the target's aspect ratio;
// what the game draws in 2D stays 4:3 in the middle, but for rectangles over
// the whole screen.
void f3d_run(f3d *f3d, const void *display_list, uint32_t width, uint32_t height, const float (*camera)[4]);

#ifdef __cplusplus
}
#endif

#endif
