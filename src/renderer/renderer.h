#ifndef RENDERER_H
#define RENDERER_H

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>
#include <vulkan/vulkan_core.h>

#define MAX_SHADERS 16
// Runtime textures include game-owned thumbnail atlas pages. Handles are
// stable and never evicted, so leave ample room beside texture-heavy levels.
#define MAX_TEXTURES 512
#define MAX_MESHES 64
#define MAX_TEXTURES_PER_DRAW 8
#define MAX_UBOS_PER_DRAW 2
#define MAX_PRIMITIVE_VERTICES 100000
#define MAX_PRIMITIVE_INDICES 200000
#define MAX_RENDER_COMMANDS 65536
#define MAX_ATLAS_INSTANCES 1000000
// Resources a game module may create at runtime, on top of the engine's own.
#define MAX_CUSTOM_VERTEX_ATTRS 24
#define MAX_CUSTOM_PIPELINES 16
#define MAX_DYNAMIC_ATLASES 32

#if defined(_MSC_VER) && !defined(__clang__)
#include <malloc.h>
#define VLA(T, name, n) T *name = (T *)_malloca(sizeof(T) * (n))
#define VLA_FREE(name) _freea(name)
#else
#define VLA(T, name, n) T name[(n)]
#define VLA_FREE(name) (void)0
#endif

struct buffer_t {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped_memory;
};

struct texture_t {
  uint32_t id;
  bool active;
  VkImage image;
  VkDeviceMemory memory;
  VkImageView image_view;
  VkSampler sampler;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t layer_count;
  VkFormat format;
  char path[256];
  uint32_t last_used_frame;
};

struct mesh_t {
  uint32_t id;
  bool active;
  buffer_t vertex_buffer;
  buffer_t index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
};

// Vertex input a shader expects. Built-in shaders leave this empty and are
// matched to one of the renderer's fixed layouts by identity; shaders created
// by a game module carry their own, which is what lets a game add a render
// technique the engine has never heard of.
struct vertex_layout_t {
  VkVertexInputBindingDescription bindings[2];
  uint32_t binding_count;
  VkVertexInputAttributeDescription attrs[MAX_CUSTOM_VERTEX_ATTRS];
  uint32_t attr_count;
  bool alpha_blend;
};

struct shader_t {
  uint32_t id;
  bool active;
  VkShaderModule vert_shader_module;
  VkShaderModule frag_shader_module;
  char vert_path[256];
  char frag_path[256];
  // NULL for the built-in shaders.
  vertex_layout_t *layout;
};

// An instanced draw technique owned by a game module: its own shader pair plus
// a ring of instance data. Instances are laid over the engine's unit quad, so a
// module supplies attribute locations 1 and up while location 0 stays the quad
// corner the engine provides.
struct custom_pipeline_t {
  bool active;
  shader_t *shader;
  vertex_layout_t layout;
  uint32_t instance_stride;
  uint32_t max_instances;
  uint32_t texture_count;
  buffer_t instance_buffer;
  uint8_t *instance_ptr;
  uint32_t instance_count;
};

typedef struct renderer_texture_layer_update_t {
  texture_t *texture;
  uint32_t layer;
  const void *pixels;
  uint32_t width;
  uint32_t height;
} renderer_texture_layer_update_t;

struct vertex_t {
  vec2 pos;
  vec3 color;
  vec2 tex_coord;
};

struct primitive_vertex_t {
  vec2 pos;
  vec4 color;
};

struct primitive_ubo_t {
  vec2 camPos; // normalized [0..1] camera center
  float zoom;
  float aspect;
  float maxMapSize;
  float _pad[3];
  mat4 proj;
  vec2 mapSize; // width, height
  float lod_bias;
};

typedef struct line_segment_t {
  vec2 p1;
  vec2 p2;
  vec4 color;
  float thickness;
} line_segment_t;

struct map_buffer_object_t {
  vec3 transform; // x, y, zoom
  float aspect;
  float lod_bias;
};

struct pipeline_cache_entry_t {
  bool initialized;
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout;
  VkDescriptorSetLayout descriptor_set_layout;
  VkRenderPass render_pass;
  uint32_t ubo_count;
  uint32_t texture_count;
};

struct camera_t {
  vec2 pos;
  vec2 drag_start_pos;
  float zoom;
  float zoom_wanted;
  bool is_dragging;
  // Index into the active game's camera mode list. Which modes exist, and where
  // each one points, is the game's decision; the engine only owns panning,
  // zooming and the projection.
  uint32_t mode;
};


struct sprite_definition_t {
  uint32_t x, y, w, h;
};

struct atlas_instance_t {
  vec2 pos;         // World-space center position
  vec2 size;        // World-space size (width, height)
  float rotation;   // Rotation in radians
  int sprite_index; // Sprite index in the texture array
  vec2 uv_scale;    // Scaling factor (sprite size / layer size)
  vec2 uv_offset;   // Offset (padding / layer size)
  vec2 tiling;      // Texture repetition factor
  vec4 color;       // Instance color
};

#define MAX_ATLAS_SPRITES 512
struct atlas_renderer_t {
  shader_t *shader;
  texture_t *atlas_texture;
  VkSampler sampler;
  sprite_definition_t *sprite_definitions;
  uint32_t sprite_count;
  buffer_t instance_buffer;
  atlas_instance_t *instance_ptr;
  uint32_t instance_count;
  uint32_t max_instances;
  uint32_t layer_width;  // Max width of a sprite, used for UV scaling
  uint32_t layer_height; // Max height of a sprite, used for UV scaling
};

typedef enum {
  RENDER_CMD_ATLAS_BATCH,
  RENDER_CMD_RECT_FILLED,
  RENDER_CMD_CIRCLE_FILLED,
  RENDER_CMD_TRIANGLE_FILLED,
  RENDER_CMD_LINE,
  RENDER_CMD_LINE_BATCH,
  RENDER_CMD_INSTANCES,
  RENDER_CMD_MESH
} render_cmd_type_t;

// Uniform payload carried inline with a queued mesh draw. Small on purpose: a
// mesh technique's per-draw constants, not a general buffer.
#define MAX_QUEUED_UNIFORM_BYTES 128

struct render_command_t {
  render_cmd_type_t type;
  float z; // Depth: lower is further back
  // Submission index, used as the final sort key. Without it the queue sort leaves commands that
  // compare equal (same z, type and atlas) in an arbitrary order that changes between frames, so
  // overlapping sprites such as particles swap places at random.
  uint32_t seq;
  union {
    struct {
      atlas_renderer_t *ar;
      const atlas_instance_t *instances;
      uint32_t count;
      bool screen_space;
    } atlas_batch;
    struct {
      vec2 p1;
      vec2 p2;
      vec2 p3;
      vec4 color;
      float thickness;
      uint32_t segments;
    } prim;
    struct {
      line_segment_t *segments;
      uint32_t count;
    } line_batch;
    struct {
      custom_pipeline_t *pipeline;
      uint32_t start;
      uint32_t count;
      texture_t *textures[MAX_TEXTURES_PER_DRAW];
      uint32_t texture_count;
    } instances;
    struct {
      custom_pipeline_t *pipeline;
      mesh_t *mesh;
      texture_t *textures[MAX_TEXTURES_PER_DRAW];
      uint32_t texture_count;
      uint8_t uniforms[MAX_QUEUED_UNIFORM_BYTES];
      uint32_t uniform_size;
    } mesh_draw;
  } data;
};

struct render_queue_t {
  render_command_t *commands;
  uint32_t count;
};

struct renderer_state_t {
  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  pipeline_cache_entry_t pipeline_cache[MAX_SHADERS][3];

  // A load/store pass for drawing RGBA preview tiles into thumbnail atlases.
  VkRenderPass preview_render_pass;
  VkFormat preview_render_format;

  VkDescriptorPool frame_descriptor_pools[3];
  VkCommandPool transfer_command_pool;

  shader_t *primitive_shader;
  buffer_t dynamic_vertex_buffer;
  buffer_t dynamic_index_buffer;
  primitive_vertex_t *vertex_buffer_ptr;
  uint32_t *index_buffer_ptr;
  uint32_t primitive_vertex_count;
  uint32_t primitive_index_count;
  uint32_t primitive_index_offset_drawn;
  VkCommandBuffer current_command_buffer;

  buffer_t dynamic_ubo_buffer;
  void *ubo_buffer_ptr;
  uint32_t ubo_buffer_offset;
  VkDeviceSize min_ubo_alignment;

  camera_t camera;
  float lod_bias;
  texture_t *default_texture;
  gfx_handler_t *gfx;

  // Resources created at runtime by the active game module. They live beside
  // the engine's own so the queue can batch and reset them the same way.
  atlas_renderer_t *dynamic_atlases[MAX_DYNAMIC_ATLASES];
  uint32_t dynamic_atlas_count;
  custom_pipeline_t custom_pipelines[MAX_CUSTOM_PIPELINES];
  uint32_t custom_pipeline_count;

  uint8_t *transient_memory;
  size_t transient_offset;
  size_t transient_capacity;

  render_queue_t queue; // The new Render Queue
};

void check_vk_result(VkResult err);
int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array, uint32_t width, uint32_t height);
texture_t *renderer_load_compact_texture_from_array(gfx_handler_t *handler, const uint8_t **pixel_array, uint32_t width, uint32_t height);
texture_t *renderer_create_texture_from_rgba(gfx_handler_t *handler, const unsigned char *pixels, int width, int height);

void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count, uint32_t *indices, uint32_t index_count);

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);
void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh, shader_t *shader, texture_t **textures, uint32_t texture_count, void **ubos, VkDeviceSize *ubo_sizes, uint32_t ubo_count);
void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);


texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas, uint32_t tile_width, uint32_t tile_height, uint32_t num_tiles_x, uint32_t num_tiles_y);
void screen_to_world(gfx_handler_t *handler, float screen_x, float screen_y, float *world_x, float *world_y);
void world_to_screen(gfx_handler_t *h, float wx, float wy, float *sx, float *sy);

// thread synchronization
void renderer_lock(void);
void renderer_unlock(void);


void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t array_layers, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *image, VkDeviceMemory *image_memory);
VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format, VkImageViewType view_type, uint32_t mip_levels, uint32_t layer_count);
VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter);

void renderer_submit_atlas_batch(gfx_handler_t *h, atlas_renderer_t *ar, float z, const atlas_instance_t *instances, uint32_t count, bool screen_space);
void renderer_calculate_atlas_uvs(atlas_renderer_t *ar, uint32_t sprite_index, atlas_instance_t *out_inst);
void renderer_submit_rect_filled(gfx_handler_t *h, float z, vec2 pos, vec2 size, vec4 color);
void renderer_submit_circle_filled(gfx_handler_t *h, float z, vec2 center, float radius, vec4 color, uint32_t segments);
void renderer_submit_triangle_filled(gfx_handler_t *h, float z, vec2 p1, vec2 p2, vec2 p3, vec4 color);
void renderer_submit_line(gfx_handler_t *h, float z, vec2 p1, vec2 p2, vec4 color, float thickness);
void renderer_submit_line_batch(gfx_handler_t *h, float z, const line_segment_t *segments, uint32_t count);
void renderer_flush_queue(gfx_handler_t *h, VkCommandBuffer cmd);

// --- resources a game module creates at runtime -------------------------------
// The engine owns the graphics API; a game owns what goes through it. These are
// the entry points behind the ft_engine_api draw services.
// `format` is a VkFormat; game modules reach this through ft_texture_format.
texture_t *renderer_create_texture_layered(gfx_handler_t *h, const unsigned char *pixels, uint32_t width, uint32_t height, uint32_t layers,
                                           VkFormat format, bool mipmaps, bool linear_filter);
atlas_renderer_t *renderer_create_atlas(gfx_handler_t *h, texture_t *source, const sprite_definition_t *sprites, uint32_t sprite_count,
                                        uint32_t max_instances);
void renderer_destroy_atlas(gfx_handler_t *h, atlas_renderer_t *ar);
bool renderer_update_texture_layer(gfx_handler_t *h, texture_t *tex, uint32_t layer, const void *pixels, uint32_t width, uint32_t height);
shader_t *renderer_create_shader_spirv(gfx_handler_t *h, const void *vert_spirv, size_t vert_size, const void *frag_spirv, size_t frag_size,
                                       const vertex_layout_t *layout);
// Instance attribute locations start at 1; location 0 is the unit-quad corner
// the engine binds. attr_formats uses the ft_vertex_format enumeration.
custom_pipeline_t *renderer_create_custom_pipeline(gfx_handler_t *h, const void *vert_spirv, size_t vert_size, const void *frag_spirv,
                                                   size_t frag_size, const uint32_t *attr_locations, const uint32_t *attr_offsets,
                                                   const int *attr_formats, uint32_t attr_count, uint32_t instance_stride,
                                                   uint32_t max_instances, uint32_t texture_count, bool alpha_blend);
void renderer_destroy_custom_pipeline(gfx_handler_t *h, custom_pipeline_t *pipe);
void renderer_submit_instances(gfx_handler_t *h, custom_pipeline_t *pipe, float z, texture_t *const *textures, uint32_t texture_count,
                               const void *instances, uint32_t count);
// Immediately renders module instances with their normal pipeline into a
// standalone sampled texture or a region of an existing destination. Preview
// coordinates are centred at the origin; a unit quad at scale 1 fills the tile.
texture_t *renderer_render_instances_preview(gfx_handler_t *h, custom_pipeline_t *pipe, texture_t *const *textures,
                                             uint32_t texture_count, const void *instances, uint32_t count, uint32_t width,
                                             uint32_t height, const vec4 clear_color,
                                             const renderer_texture_layer_update_t *updates, uint32_t update_count,
                                             texture_t *destination, uint32_t destination_x, uint32_t destination_y);
// Queues a mesh draw so it sorts with everything else. A game's level pass goes
// through here, which is what lets it sit above or below the entities by z.
void renderer_submit_mesh(gfx_handler_t *h, custom_pipeline_t *pipe, float z, mesh_t *mesh, texture_t *const *textures,
                          uint32_t texture_count, const void *uniforms, size_t uniform_size);
void renderer_cleanup_atlas_renderer(gfx_handler_t *h, atlas_renderer_t *ar);
texture_t *renderer_create_texture_2d_array(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t layer_count, VkFormat format);


#endif // RENDERER_H
