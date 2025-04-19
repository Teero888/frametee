
#ifndef RENDERER_H
#define RENDERER_H

#include <cglm/cglm.h>
#include <vulkan/vulkan_core.h>

#define MAX_RENDERABLES 128
#define MAX_SHADERS 16
#define MAX_TEXTURES 32
#define MAX_MESHES 64

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
  bool active;
  mesh_t *mesh;
  shader_t *shader;
  texture_t *texture;
  buffer_t uniform_buffer;
  VkDescriptorSet descriptor_set;
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout;

  mat4 model_matrix;
} renderable_t;

typedef struct {

  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  uint32_t texture_count;

  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  renderable_t renderables[MAX_RENDERABLES];
  uint32_t renderable_count;

  VkDescriptorSetLayout global_descriptor_set_layout;
  VkDescriptorSetLayout object_descriptor_set_layout;
  VkDescriptorPool resource_descriptor_pool;

  VkCommandPool transfer_command_pool;

  texture_t *default_texture;

  gfx_handler_t *gfx;
} renderer_state_t;

void check_vk_result_line(VkResult err, int line);
void check_vk_result(VkResult err);

int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count);

renderable_t *renderer_add_renderable(gfx_handler_t *handler, mesh_t *mesh, shader_t *shader,
                                      texture_t *texture);
void renderer_remove_renderable(gfx_handler_t *handler, renderable_t *renderable);

void renderer_update_uniforms(gfx_handler_t *handler);
void renderer_draw(gfx_handler_t *handler, VkCommandBuffer command_buffer);

VkVertexInputBindingDescription get_vertex_binding_description();
uint32_t get_vertex_attribute_description_count();
const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions();

#endif
