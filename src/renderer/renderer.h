#ifndef RENDERER_H
#define RENDERER_H

#include "../animation/anim_system.h"
#include <cglm/cglm.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#define MAX_SHADERS 16
#define MAX_TEXTURES 64
#define MAX_MESHES 64
#define MAX_TEXTURES_PER_DRAW 3
#define MAX_UBOS_PER_DRAW 2
#define MAX_PRIMITIVE_VERTICES 100000
#define MAX_PRIMITIVE_INDICES 200000

#if defined(_MSC_VER) && !defined(__clang__)
  #include <malloc.h>
  #define VLA(T, name, n)  T *name = (T*)_malloca(sizeof(T) * (n))
  #define VLA_FREE(name)   _freea(name)
#else
  #define VLA(T, name, n)  T name[(n)]
  #define VLA_FREE(name)   (void)0
#endif

typedef struct gfx_handler_t gfx_handler_t;

typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped_memory;
} buffer_t;

typedef struct {
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
  char path[256];
  uint8_t gs_org; // ddnet grayscale shit for coloring skins
} texture_t;

typedef struct {
  uint32_t id;
  bool active;
  buffer_t vertex_buffer;
  buffer_t index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
} mesh_t;

typedef struct {
  uint32_t id;
  bool active;
  VkShaderModule vert_shader_module;
  VkShaderModule frag_shader_module;
  char vert_path[256];
  char frag_path[256];
} shader_t;

typedef struct {
  vec2 pos;
  vec3 color;
  vec2 tex_coord;
} vertex_t;

typedef struct {
  vec2 pos;
  vec4 color;
} primitive_vertex_t;

typedef struct {
  vec2 camPos; // normalized [0..1] camera center
  float zoom;
  float aspect;
  float maxMapSize;
  mat4 proj;
  vec2 mapSize; // width, height
} primitive_ubo_t;

typedef struct {
  vec3 transform; // x, y, zoom
  float aspect;
  float lod;
} map_buffer_object_t;

typedef struct {
  bool initialized;
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout;
  VkDescriptorSetLayout descriptor_set_layout;
  uint32_t ubo_count;
  uint32_t texture_count;
} pipeline_cache_entry_t;

typedef struct {
  vec2 pos;
  vec2 drag_start_pos;
  float zoom;
  float zoom_wanted;
  bool is_dragging;
} camera_t;

typedef struct {
  vec2 pos;
  float scale;
  int skin_index;
  int eye_state;

  // Animation data per-part (x,y offset + angle)
  vec4 body; // x, y, angle, unused
  vec4 back_foot;
  vec4 front_foot;
  vec4 attach;
  vec2 dir; // aim
  vec3 col_body;
  vec3 col_feet;
  int col_custom;
  int col_gs;
} skin_instance_t;

typedef struct {
  shader_t *skin_shader;
  buffer_t instance_buffer;
  skin_instance_t *instance_ptr;
  uint32_t instance_count;
} skin_renderer_t;

#define MAX_SKINS 128
typedef struct {
  texture_t *atlas_array; // giant 2D array texture for all skins
  bool layer_used[MAX_SKINS];
} skin_atlas_manager_t;

typedef struct {
  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  pipeline_cache_entry_t pipeline_cache[MAX_SHADERS];

  // Descriptor pool per frame-in-flight (swapchain image) to avoid resetting in-use pools
  VkDescriptorPool frame_descriptor_pools[3]; // assume triple buffering
  VkCommandPool transfer_command_pool;

  // State for primitive drawing
  shader_t *primitive_shader;
  buffer_t dynamic_vertex_buffer;
  buffer_t dynamic_index_buffer;
  primitive_vertex_t *vertex_buffer_ptr;
  uint32_t *index_buffer_ptr;
  uint32_t primitive_vertex_count;
  uint32_t primitive_index_count;
  VkCommandBuffer current_command_buffer;

  // UBO Ring Buffer
  buffer_t dynamic_ubo_buffer;
  void *ubo_buffer_ptr;
  uint32_t ubo_buffer_offset;
  VkDeviceSize min_ubo_alignment;

  camera_t camera;
  texture_t *default_texture;
  gfx_handler_t *gfx;
  skin_atlas_manager_t skin_manager;
  skin_renderer_t skin_renderer;
} renderer_state_t;

void check_vk_result(VkResult err);
int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array,
                                            uint32_t width, uint32_t height);
texture_t *renderer_load_compact_texture_from_array(gfx_handler_t *handler, const uint8_t **pixel_array,
                                                    uint32_t width, uint32_t height);
void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count);

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);
void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh,
                        shader_t *shader, texture_t **textures, uint32_t texture_count, void **ubos,
                        VkDeviceSize *ubo_sizes, uint32_t ubo_count);
void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);

void renderer_draw_map(gfx_handler_t *h);

// primitive drawing api
void renderer_draw_rect_filled(gfx_handler_t *handler, vec2 pos, vec2 size, vec4 color);
void renderer_draw_circle_filled(gfx_handler_t *handler, vec2 center, float radius, vec4 color,
                                 uint32_t segments);
void renderer_draw_line(gfx_handler_t *handler, vec2 p1, vec2 p2, vec4 color, float thickness);

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas,
                                                    uint32_t tile_width, uint32_t tile_height,
                                                    uint32_t num_tiles_x, uint32_t num_tiles_y);
void screen_to_world(gfx_handler_t *handler, float screen_x, float screen_y, float *world_x, float *world_y);
void world_to_screen(gfx_handler_t *h, float wx, float wy, float *sx, float *sy);

// skin rendering
void renderer_begin_skins(gfx_handler_t *h);
void renderer_push_skin_instance(gfx_handler_t *h, vec2 pos, float scale, int skin_index, int eye_state,
                                 vec2 dir, const anim_state_t *anim_state, vec3 col_body, vec3 col_feet,
                                 bool use_custom_color);
void renderer_flush_skins(gfx_handler_t *h, VkCommandBuffer cmd, texture_t *skin_array);
int renderer_load_skin_from_file(gfx_handler_t *h, const char *path);
void renderer_unload_skin(gfx_handler_t *h, int layer);

void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels,
                  uint32_t array_layers, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkImage *image, VkDeviceMemory *image_memory);
VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format,
                              VkImageViewType view_type, uint32_t mip_levels, uint32_t layer_count);
VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter);

#endif // RENDERER_H
