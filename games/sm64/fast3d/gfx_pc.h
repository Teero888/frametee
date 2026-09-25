#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stdint.h>

#include <PR/gbi.h>

struct GfxRenderingAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(struct GfxRenderingAPI *rapi);
// Draws the game's 3D scenes through this view-projection instead of the
// game's camera (row vectors, OpenGL's clip space as the game's projections
// are), or through the game's again with NULL.
void gfx_set_camera(const float view_projection[4][4]);
// Forgets every texture: for a new backend, which has none of the old one's.
void gfx_reset_textures(void);
// Draws one frame's display list (sm64_step_draw) at width x height pixels.
void gfx_run(Gfx *commands, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
