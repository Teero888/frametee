#ifndef GRAPHICS_H
#define GRAPHICS_H
#include "renderer.h"
#include <engine/game_host.h>
#include <types.h>
#include <user_interface/user_interface.h>
#include <vulkan/vulkan_core.h>

#include <stdbool.h>
#include <stdint.h>

// These have to be in this order
#include <system/include_cimgui.h>
// -------------
// we are doing this so we dont get redefinition of structs errors. they are illegal in C99
#define GLFWwindow INVALID_TYPE_DONT_EVER_USE_WINDOW
#define GLFWmonitor INVALID_TYPE_DONT_EVER_USE_MONITOR
#include <cimgui_impl.h>
#undef GLFWwindow
#undef GLFWmonitor

static inline void destroy_imgui_texture_ref(struct ImTextureRef_c **tex_ref_ptr) {
  if (tex_ref_ptr && *tex_ref_ptr) {
    ImTextureID tex_id = ImTextureRef_GetTexID(*tex_ref_ptr);
    if (tex_id) {
      ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)tex_id);
    }
    ImTextureRef_destroy(*tex_ref_ptr);
    *tex_ref_ptr = NULL;
  }
}

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

enum { FRAME_OK = 0,
       FRAME_SKIP,
       FRAME_EXIT };

// public api
void on_level_load_memory(gfx_handler_t *handler, const unsigned char *level_buffer, size_t size);
void on_level_load_path(gfx_handler_t *handler, const char *level_path);
// Starts a clean project under another game. All objects owned by the old
// module are destroyed before it is deactivated.
bool gfx_activate_game(gfx_handler_t *handler, int game_index);
int init_gfx_handler(gfx_handler_t *handler);
int gfx_begin_frame(gfx_handler_t *handler);
bool gfx_end_frame(gfx_handler_t *handler);
void gfx_cleanup(gfx_handler_t *handler);
void gfx_toggle_fullscreen(gfx_handler_t *handler);
float gfx_get_ui_scale(void);

struct gfx_handler_t {
  // Backend Stuffs
  GLFWwindow *window;
  VkAllocationCallbacks *g_allocator;
  VkInstance g_instance;
  VkPhysicalDevice g_physical_device;
  VkDevice g_device;
  uint32_t g_queue_family;
  VkQueue g_queue;
  VkDebugReportCallbackEXT g_debug_report;
  VkDebugUtilsMessengerEXT g_debug_messenger;
  VkPipelineCache g_pipeline_cache;
  VkDescriptorPool g_descriptor_pool; // For ImGui
  struct ImGui_ImplVulkanH_Window g_main_window_data;
  uint32_t g_min_image_count;
  bool g_swap_chain_rebuild;

  // Per-frame data
  VkCommandBuffer current_frame_command_buffer;

  // App Stuffs
  ui_handler_t user_interface;
  renderer_state_t renderer;
  // The active game. Everything game-specific — physics, level format, how a
  // world looks — lives behind this and never in the engine.
  game_host_t game_host;
  // The level the project is built on, owned by the game module. NULL until one
  // is opened.
  ft_level *level;

  // Extent of the playfield in world units, taken from the active game's level.
  // The renderer normalizes every world coordinate by this, which is the only
  // thing it needs to know about a level, so nothing in the render path has to
  // understand any particular game's level format.
  float world_width;
  float world_height;

  vec2 viewport; // width,height

  // The unit quad every instanced technique is laid over.
  mesh_t *quad_mesh;

  // retirement list for delayed frees
  struct {
    VkImage image;
    VkImageView image_view;
    VkSampler sampler;
    VkDeviceMemory memory;
    uint32_t frame_index;
  } retire_textures[256];
  uint32_t retire_count;

  // Offscreen rendering (for ImGui game view)
  VkImage offscreen_image;
  VkDeviceMemory offscreen_memory;
  VkImageView offscreen_image_view;
  VkSampler offscreen_sampler;
  VkFramebuffer offscreen_framebuffer;
  VkRenderPass offscreen_render_pass;
  // ImGui texture id returned by ImGui_ImplVulkan_AddTexture
  ImTextureRef *offscreen_texture;
  uint32_t offscreen_width;
  uint32_t offscreen_height;
  bool offscreen_initialized;
};

#endif // GRAPHICS_H
