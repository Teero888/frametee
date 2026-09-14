#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdint.h>
#include <stdbool.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;
extern const uint8_t *g_sm64_segments[32];
extern bool configFiltering;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title);
void gfx_sp_reset(void);
void gfx_dp_reset(void);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_run_dl(Gfx *commands);
void gfx_flush(void);
void gfx_end_frame(void);
void gfx_set_projection(const float p[4][4]);
void gfx_set_modelview(const float m[4][4]);
void gfx_push_modelview(const float m[4][4]);
void gfx_pop_modelview(void);
void gfx_precache_textures(void);
void gfx_clear_cache(void);
void gfx_shutdown(void);
void gfx_print_tri_stats(void);

#ifdef __cplusplus
}
#endif

#endif
