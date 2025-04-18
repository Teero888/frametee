// renderer.h
#ifndef RENDERER_H
#define RENDERER_H

#include <cglm/cglm.h> // Using cglm for math (matrices) - install if needed!
#include <vulkan/vulkan_core.h>

#define MAX_RENDERABLES 128
#define MAX_SHADERS 16
#define MAX_TEXTURES 32
#define MAX_MESHES 64

typedef struct gfx_handler_t gfx_handler_t;
// --- Helper Structures ---

// Simple buffer structure
typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped_memory; // Optional: Keep mapped if host_coherent
} buffer_t;

// Texture structure
typedef struct {
  uint32_t id; // Unique ID for lookup
  VkImage image;
  VkDeviceMemory memory;
  VkImageView image_view;
  VkSampler sampler;
  uint32_t width;
  uint32_t height;
  char path[256]; // Store path for potential debugging/reloading
} texture_t;

// Mesh structure (Vertices + Indices)
typedef struct {
  uint32_t id; // Unique ID for lookup
  buffer_t vertex_buffer;
  buffer_t index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
} mesh_t;

// Shader program (Vertex + Fragment)
typedef struct {
  uint32_t id; // Unique ID for lookup
  VkShaderModule vert_shader_module;
  VkShaderModule frag_shader_module;
  char vert_path[256];
  char frag_path[256];
} shader_t;

// Structure defining vertex layout
typedef struct {
  vec3 pos;
  vec3 color;
  vec2 tex_coord;
} vertex_t;

// Uniform Buffer Object structure (example: Model-View-Projection)
typedef struct {
  mat4 model;
  mat4 view;
  mat4 proj;
} uniform_buffer_object_t;

// Represents a single object to be drawn
typedef struct {
  bool active;
  mesh_t *mesh;
  shader_t *shader;
  texture_t *texture; // Optional texture
  buffer_t uniform_buffer;
  VkDescriptorSet descriptor_set;
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout; // May be shared, but stored here for convenience
  // Add specific transformation, color tint, etc. here if needed
  // Or update them via the uniform buffer
  mat4 model_matrix; // Example: per-object model matrix
} renderable_t;

// --- Renderer State (to be embedded in gfx_handler_t) ---
typedef struct {
  // Resource Storage
  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  uint32_t texture_count;

  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  renderable_t renderables[MAX_RENDERABLES];
  uint32_t renderable_count;

  // Vulkan Objects for Rendering
  VkDescriptorSetLayout global_descriptor_set_layout; // For things like camera UBO? (Optional)
  VkDescriptorSetLayout object_descriptor_set_layout; // For texture sampler + object UBO
  VkDescriptorPool resource_descriptor_pool;          // Pool for textures, UBOs

  // Command pool for transfer operations (texture/buffer uploads)
  VkCommandPool transfer_command_pool;

  // Placeholder for default resources if needed
  texture_t *default_texture;

  // Pointer back to the main handler for Vulkan objects
  gfx_handler_t *gfx;
} renderer_state_t;

void check_vk_result_line(VkResult err, int line);
void check_vk_result(VkResult err);

// Initialization and Cleanup
int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

// Resource Loading
shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count);

// Renderable Management
renderable_t *renderer_add_renderable(gfx_handler_t *handler, mesh_t *mesh, shader_t *shader,
                                      texture_t *texture);
void renderer_remove_renderable(gfx_handler_t *handler, renderable_t *renderable); // Marks as inactive

// Rendering
void renderer_update_uniforms(gfx_handler_t *handler); // Update UBOs before drawing
void renderer_draw(gfx_handler_t *handler, VkCommandBuffer command_buffer);

// Vertex Input Description (needed for pipeline creation)
VkVertexInputBindingDescription get_vertex_binding_description();
uint32_t get_vertex_attribute_description_count();
const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions();

#endif // RENDERER_H
