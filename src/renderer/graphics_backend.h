#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "../../libs/ddnet_physics/libs/ddnet_map_loader/ddnet_map_loader.h"
#include "../physics/physics.h"
#include "../user_interface/user_interface.h"
#include "renderer.h"
#include <cimgui.h>
#include <cimgui_impl.h>
#include <vulkan/vulkan_core.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <stdbool.h>
#include <stdint.h>

// forward declaration
struct gfx_handler_t;

enum { FRAME_OK = 0, FRAME_SKIP, FRAME_EXIT };

// public api
void on_map_load(struct gfx_handler_t *handler, const char *map_path);
int init_gfx_handler(struct gfx_handler_t *handler);
int gfx_begin_frame(struct gfx_handler_t *handler);
bool gfx_end_frame(struct gfx_handler_t *handler);
void gfx_cleanup(struct gfx_handler_t *handler);

typedef struct {
  double x, y;   // last cursor pos
  double dx, dy; // delta since last poll
} raw_mouse_t;

struct gfx_handler_t {
  // Backend Stuffs ---
  GLFWwindow *window;
  VkAllocationCallbacks *g_allocator;
  VkInstance g_instance;
  VkPhysicalDevice g_physical_device;
  VkDevice g_device;
  uint32_t g_queue_family;
  VkQueue g_queue;
  VkDebugReportCallbackEXT g_debug_report;
  VkPipelineCache g_pipeline_cache;
  VkDescriptorPool g_descriptor_pool; // For ImGui
  struct ImGui_ImplVulkanH_Window g_main_window_data;
  uint32_t g_min_image_count;
  bool g_swap_chain_rebuild;

  // Per-frame data ---
  VkCommandBuffer current_frame_command_buffer;

  // App Stuffs ---
  ui_handler_t user_interface;
  renderer_state_t renderer;
  physics_handler_t physics_handler;
  map_data_t *map_data; // ptr to ^ collision data for quick typing
  texture_t *entities_atlas;
  texture_t *entities_array;

  vec2 viewport; // width,height

  int default_skin;
  int x_ninja_skin;
  int x_spec_skin;

  raw_mouse_t raw_mouse;

  // Map Specific Render Data ---
  shader_t *map_shader;
  mesh_t *quad_mesh;
  // TODO: this should be 2
  texture_t *map_textures[MAX_TEXTURES_PER_DRAW];
  uint32_t map_texture_count;

  // retirement list for delayed frees
  struct {
    texture_t *tex;
    uint32_t frame_index;
  } retire_textures[64];
  uint32_t retire_count;

  // Offscreen rendering (for ImGui game view) ---
  VkImage offscreen_image;
  VkDeviceMemory offscreen_memory;
  VkImageView offscreen_image_view;
  VkSampler offscreen_sampler;
  VkFramebuffer offscreen_framebuffer;
  VkRenderPass offscreen_render_pass;
  // ImGui texture id returned by ImGui_ImplVulkan_AddTexture
  ImTextureID offscreen_texture_id;
  uint32_t offscreen_width;
  uint32_t offscreen_height;
  bool offscreen_initialized;
};

#endif // GRAPHICS_H
