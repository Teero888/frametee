#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "ddnet_map_loader.h"
#include "renderer.h"
#include "../user_interface/user_interface.h"
#include <cimgui.h>
#include <cimgui_impl.h>
#include <vulkan/vulkan_core.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <stdbool.h>
#include <stdint.h>

// Forward declaration
struct gfx_handler_t;

enum 
{
  FRAME_OK = 0,
  FRAME_SKIP,
  FRAME_EXIT
};

// --- Public API ---
void on_map_load(struct gfx_handler_t *handler, const char *map_path);
int init_gfx_handler(struct gfx_handler_t *handler);
int gfx_begin_frame(struct gfx_handler_t *handler);
void gfx_end_frame(struct gfx_handler_t *handler);
void gfx_cleanup(struct gfx_handler_t *handler);

// --- Structures ---
struct gfx_handler_t {
  // --- Backend Stuffs ---
  GLFWwindow *window;
  VkAllocationCallbacks *g_Allocator;
  VkInstance g_Instance;
  VkPhysicalDevice g_PhysicalDevice;
  VkDevice g_Device;
  uint32_t g_QueueFamily;
  VkQueue g_Queue;
  VkDebugReportCallbackEXT g_DebugReport;
  VkPipelineCache g_PipelineCache;
  VkDescriptorPool g_DescriptorPool; // For ImGui
  struct ImGui_ImplVulkanH_Window g_MainWindowData;
  uint32_t g_MinImageCount;
  bool g_SwapChainRebuild;

  // --- Per-frame data ---
  VkCommandBuffer current_frame_command_buffer;

  // --- App Stuffs ---
  ui_handler_t user_interface;
  renderer_state_t renderer;
  map_data_t map_data;

  // --- Map Specific Render Data ---
  shader_t *map_shader;
  mesh_t *quad_mesh;
  texture_t *map_textures[MAX_TEXTURES_PER_DRAW];
  uint32_t map_texture_count;
};

#endif // GRAPHICS_H