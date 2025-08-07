#ifndef RENDERER_H
#define RENDERER_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#define MAX_RENDER_OBJECTS 128
#define MAX_SHADERS 16
#define MAX_TEXTURES 64
#define MAX_MESHES 64
#define MAX_MATERIALS 32
#define MAX_TEXTURES_PER_MATERIAL 8
#define MAX_UBOS_PER_MATERIAL 2

typedef struct gfx_handler_t gfx_handler_t;

typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped_memory;
} buffer_t;

typedef struct {
  uint32_t id;
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
  buffer_t vertex_buffer;
  buffer_t index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
} mesh_t;

typedef struct {
  uint32_t id;
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
  mat4 model;
  mat4 view;
  mat4 proj;
} uniform_buffer_object_t;

typedef struct {
  vec3 transform; // x, y, zoom
  float aspect;
  float lod;
} map_buffer_object_t;

typedef struct {
  uint32_t id;
  shader_t *shader;
  texture_t *textures[MAX_TEXTURES_PER_MATERIAL];
  buffer_t uniform_buffers[MAX_UBOS_PER_MATERIAL];
  uint32_t texture_count;
  uint32_t ubo_count;

  // Vulkan objects tied to this material
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout;
  VkDescriptorSetLayout descriptor_set_layout;
  VkDescriptorSet descriptor_set;
} material_t;

typedef struct {
  bool active;
  mesh_t *mesh;
  material_t *material;
} render_object_t;

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
  uint32_t texture_count;

  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  material_t materials[MAX_MATERIALS];
  uint32_t material_count;

  render_object_t render_objects[MAX_RENDER_OBJECTS];
  uint32_t render_object_count;

  VkDescriptorPool resource_descriptor_pool;
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
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count);

material_t *renderer_create_material(gfx_handler_t *handler, shader_t *shader);
void material_add_texture(material_t *material, texture_t *texture);
void material_add_ubo(gfx_handler_t *handler, material_t *material, VkDeviceSize ubo_size);
void material_finalize(gfx_handler_t *handler, material_t *material);

render_object_t *renderer_add_render_object(gfx_handler_t *handler, mesh_t *mesh, material_t *material);

void renderer_update(gfx_handler_t *handler);
void renderer_draw(gfx_handler_t *handler, VkCommandBuffer command_buffer);

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas,
                                                    uint32_t tile_width, uint32_t tile_height,
                                                    uint32_t num_tiles_x, uint32_t num_tiles_y);

VkVertexInputBindingDescription get_vertex_binding_description(void);
const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions(void);
uint32_t get_vertex_attribute_description_count(void);

#endif // RENDERER_H
