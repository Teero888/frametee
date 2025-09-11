#ifndef RENDERER_H
#define RENDERER_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#define MAX_SHADERS 16
#define MAX_TEXTURES 64
#define MAX_MESHES 64
#define MAX_TEXTURES_PER_DRAW 8
#define MAX_UBOS_PER_DRAW 2

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
  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  pipeline_cache_entry_t pipeline_cache[MAX_SHADERS];

  VkDescriptorPool frame_descriptor_pool;
  VkCommandPool transfer_command_pool;

  camera_t camera;
  texture_t *default_texture;
  gfx_handler_t *gfx;
} renderer_state_t;

void check_vk_result(VkResult err);
int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array,
                                            uint32_t width, uint32_t height);
void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count);

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);
void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh,
                        shader_t *shader, texture_t **textures, uint32_t texture_count, void **ubos,
                        VkDeviceSize *ubo_sizes, uint32_t ubo_count);
void renderer_end_frame(gfx_handler_t *handler);

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas,
                                                    uint32_t tile_width, uint32_t tile_height,
                                                    uint32_t num_tiles_x, uint32_t num_tiles_y);

VkVertexInputBindingDescription get_vertex_binding_description(void);
const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions(void);
uint32_t get_vertex_attribute_description_count(void);

#endif // RENDERER_H