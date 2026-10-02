#ifndef TMUF_GPU_H
#define TMUF_GPU_H

// The module's own renderer, on the engine's Vulkan device: what the game's
// Direct3D 9 renderer does, closely enough to reproduce its pictures. Draws
// go into an image the engine then shows (as games/sm64 does), with:
//
//   textures   as the game loads them: BC1/2/3 blocks straight from the DDS
//              files with their mip chains, cube maps, RGBA8 otherwise;
//   samplers   per draw, Direct3D's address modes and filters;
//   programs   a vertex and a fragment shader (SPIR-V) with the vertex
//              layout they read and a block of uniforms per draw;
//   states     Direct3D's blending (D3DBLEND factors), depth test and
//              writes, culling, colour writes; a pipeline is made per
//              program and state the first time they are drawn together.
//
// The world is drawn with the engine's camera (reversed depth: near 1, far
// 0) into a colour and a depth buffer of the module's own, then into the
// engine's image.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tg tg;
typedef struct tg_mesh tg_mesh;
typedef struct tg_program tg_program;
typedef uint32_t tg_texture; // 0: none (white)
// the frame as it was at the last tg_frame_copy
#define TG_TEXTURE_FRAME 0xfffffffeu

typedef enum tg_format { TG_RGBA8, TG_BC1, TG_BC2, TG_BC3 } tg_format;

// An image as a DDS file holds it: faces (1, or 6 for a cube: +x -x +y -y +z
// -z), each with its levels from the largest, one after the other.
typedef struct tg_image {
  tg_format format;
  uint32_t width, height, levels, faces;
  const uint8_t *data;
  size_t size;
} tg_image;

// Bytes of one level of an image.
size_t tg_level_size(tg_format format, uint32_t width, uint32_t height);

// Vulkan handles as uint64_t/void * so that this header needs no Vulkan.
tg *tg_create(void *physical_device, void *device, void *queue, uint32_t queue_family, char *error,
              size_t error_size);
void tg_destroy(tg *g);
const char *tg_error(const tg *g);
// Whether BC images can be sampled (else give RGBA8)
bool tg_supports_bc(const tg *g);

// Uploads go out in batches: between tg_upload_begin and tg_upload_end
// textures and meshes are recorded and sent together (a call outside a batch
// is a batch of its own).
void tg_upload_begin(tg *g);
bool tg_upload_end(tg *g);
tg_texture tg_texture_create(tg *g, const tg_image *image);
void tg_texture_destroy(tg *g, tg_texture texture);
tg_mesh *tg_mesh_create(tg *g, const void *vertices, size_t vertices_size, const uint32_t *indices,
                        uint32_t index_count);
void tg_mesh_destroy(tg *g, tg_mesh *mesh);

typedef enum tg_attr_format { TG_FLOAT1, TG_FLOAT2, TG_FLOAT3, TG_FLOAT4, TG_UINT1, TG_UNORM4 } tg_attr_format;
typedef struct tg_attr {
  uint32_t location, offset;
  tg_attr_format format;
} tg_attr;

#define TG_MAX_TEXTURES 16
// Uniforms: one block per draw at binding 0, seen by both stages; textures at
// bindings 1.. in the order a draw gives them (2D or cube, as the shader
// declares them and the textures are).
typedef struct tg_program_desc {
  const uint32_t *vertex_spirv, *fragment_spirv;
  size_t vertex_spirv_size, fragment_spirv_size;
  uint32_t vertex_stride;
  const tg_attr *attrs;
  uint32_t attr_count;
  uint32_t uniform_size;
  uint32_t texture_count;
  uint32_t cube_mask; // the textures the shader samples as cubes (bit i: texture i)
} tg_program_desc;
tg_program *tg_program_create(tg *g, const tg_program_desc *desc);
void tg_program_destroy(tg *g, tg_program *program);

// Direct3D's D3DBLEND factors.
enum {
  TG_BLEND_ZERO = 1,
  TG_BLEND_ONE = 2,
  TG_BLEND_SRCCOLOR = 3,
  TG_BLEND_INVSRCCOLOR = 4,
  TG_BLEND_SRCALPHA = 5,
  TG_BLEND_INVSRCALPHA = 6,
  TG_BLEND_DESTALPHA = 7,
  TG_BLEND_INVDESTALPHA = 8,
  TG_BLEND_DESTCOLOR = 9,
  TG_BLEND_INVDESTCOLOR = 10,
};
// Direct3D's D3DCULL: 1 none, 2 clockwise, 3 counter-clockwise triangles
// (as they appear on screen) are not drawn.
enum { TG_CULL_NONE = 1, TG_CULL_CW = 2, TG_CULL_CCW = 3 };

typedef struct tg_state {
  uint8_t blend_src, blend_dst; // 0: no blending
  bool depth_test, depth_write;
  uint8_t cull;
  bool no_color_write;
  bool blend_min;  // D3DBLENDOP_MIN (the factors unused)
  bool alpha_only; // the alpha written alone
  int8_t slope_bias; // the depth's slope-scaled bias, in quarters (depthBiasSlopeFactor)
} tg_state;

// Direct3D's D3DTEXTUREADDRESS (1 wrap, 2 mirror, 3 clamp) and filters
// (D3DTEXF: 1 point, 2 linear, 3 anisotropic; mip 0 none).
typedef struct tg_sampler {
  uint8_t address_u, address_v;
  uint8_t filter, mip;
} tg_sampler;
#define TG_SAMPLER_WRAP ((tg_sampler){1, 1, 3, 2})
#define TG_SAMPLER_CLAMP ((tg_sampler){3, 3, 3, 2})

// A frame: into `image` (a VkImage of `format`, left in `final_layout`),
// cleared to `clear`.
bool tg_frame_begin(tg *g, uint64_t image, uint32_t format, uint32_t width, uint32_t height, uint32_t final_layout,
                    const float clear[4]);
// What is drawn so far, into TG_TEXTURE_FRAME (as the game copies its frame
// for the draws that show what is behind them: grass, water)
void tg_frame_copy(tg *g);
// Draws between these go into TG_TEXTURE_FRAME instead, cleared first, with
// a depth of their own (the sea's refraction: what is under the water)
void tg_offscreen_begin(tg *g, const float clear[4]);
void tg_offscreen_end(tg *g);
// Render targets: a colour (sampled as tg_target_texture) and a depth of
// their own. Draws between tg_target_begin and tg_target_end go into one,
// cleared first; then on into the frame.
typedef struct tg_target tg_target;
tg_target *tg_target_create(tg *g, uint32_t width, uint32_t height);
// with a mip chain, made from what is drawn when its draws end
tg_target *tg_target_create_mips(tg *g, uint32_t width, uint32_t height);
// RGBA 32-bit float (the lightmap bake's shadow maps and sums)
tg_target *tg_target_create_float(tg *g, uint32_t width, uint32_t height);
// its depth alone (reversed: cleared to 0, nearer greater), sampled as its
// texture (.r); drawn with programs that write no colour
tg_target *tg_target_create_depth(tg *g, uint32_t width, uint32_t height);
void tg_target_destroy(tg *g, tg_target *target);
tg_texture tg_target_texture(const tg_target *target);
uint32_t tg_target_width(const tg_target *target);
uint32_t tg_target_height(const tg_target *target);
void tg_target_begin(tg *g, tg_target *target, const float clear[4]);
void tg_target_end(tg *g);
// A target's pixels (RGBA8, rows top first) after the frame (tg_frame_end):
// width x height x 4 bytes into out. False when they cannot be read.
bool tg_target_read(tg *g, tg_target *target, uint8_t *out);
// mesh NULL: index_count vertices the program makes itself (gl_VertexIndex
// from first_index), for programs without a vertex layout
void tg_draw(tg *g, tg_program *program, const tg_state *state, tg_mesh *mesh, uint32_t first_index,
             uint32_t index_count, const tg_texture *textures, const tg_sampler *samplers, const void *uniforms);
// Occlusion: the frame's samples a draw between these passes the depth test
// with (in the frame's pass, before tg_frame_resolve); its index this frame,
// UINT32_MAX when none can be made. The counts come back a frame or two
// later: tg_query_results, the newest finished frame's, with its serial
uint32_t tg_query_begin(tg *g);
void tg_query_end(tg *g);
uint32_t tg_query_results(const tg *g, uint32_t *serial, const uint64_t **counts);
// The depth handed to the engine beside the colour this frame (a depth
// target's texture, single sampled); none: no depth
void tg_set_export_depth(tg *g, tg_texture depth);
uint32_t tg_samples(const tg *g);
// Vertices of this frame only (the program's layout): drawn as triangles
void tg_draw_dynamic(tg *g, tg_program *program, const tg_state *state, const void *vertices, uint32_t vertex_count,
                     const tg_texture *textures, const tg_sampler *samplers, const void *uniforms);
// The frame's samples resolved now: what is drawn after (the flares, the
// post effects, the HUD: over what is there) goes into its single-sampled
// colour, without depth
void tg_frame_resolve(tg *g);
bool tg_frame_end(tg *g);
// The rectangle drawn into from now on (in pixels of the target or frame),
// until the next tg_target_begin, tg_target_end or tg_scissor; w 0: all of it
void tg_scissor(tg *g, int32_t x, int32_t y, uint32_t w, uint32_t h);
// TM_PROFILE: a GPU timestamp named `name` (a string literal): the time up to
// the next mark is reported under it, averaged over 120 frames, to stderr
void tg_mark(tg *g, const char *name);
// A read-back without waiting: the target's pixels (RGBA8) as the frame being
// recorded leaves them, copied at its end; tg_target_read_result hands them
// out under `key` once the GPU is done (a frame or two later)
void tg_target_read_queue(tg *g, tg_target *target, uint32_t key);
// the newest finished read-back of `key` (size bytes) into out, and the frame
// it was queued in (tg_frame_serial then); true when not handed out before
bool tg_target_read_result(tg *g, uint32_t key, uint8_t *out, uint32_t size, uint32_t *serial);
// the frame being recorded, counted from 1
uint32_t tg_frame_serial(const tg *g);
// The frame's depth handed on with its colour: once enabled, the frame's
// image is twice the frame's width, the colour in its left half and its right
// half written by the fragment program `pack` (`pack_ms` when multisampled),
// which samples the frame's depth at binding 1 (the engine then depth-tests
// its own 3D drawing against the scene). False when the programs cannot be
// made (the colour alone then).
bool tg_enable_depth_export(tg *g, const void *pack, size_t pack_size, const void *pack_ms, size_t pack_ms_size);
bool tg_depth_exported(const tg *g);

// Multisampling of the frame: the samples per pixel (1, 2, 4, 8, 16), the
// most the device has up to that; returns what it gets.
uint32_t tg_set_samples(tg *g, uint32_t samples);
// The anisotropy anisotropic filtering uses (1..16, as the device allows).
void tg_set_anisotropy(tg *g, float anisotropy);

#endif
