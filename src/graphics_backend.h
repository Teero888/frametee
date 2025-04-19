#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "renderer.h"
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include "user_interface/user_interface.h"
#include <GLFW/glfw3.h>
#include <cimgui.h>
#include <cimgui_impl.h>
#include <stdint.h>

struct gfx_handler_t {
  // --- backend stuffs ---
  GLFWwindow *window;
  VkAllocationCallbacks *g_Allocator;
  VkInstance g_Instance;
  VkPhysicalDevice g_PhysicalDevice;
  VkDevice g_Device;
  uint32_t g_QueueFamily;
  VkQueue g_Queue;
  VkDebugReportCallbackEXT g_DebugReport;
  VkPipelineCache g_PipelineCache;
  VkDescriptorPool g_DescriptorPool;
  ImGui_ImplVulkanH_Window g_MainWindowData;
  uint32_t g_MinImageCount;
  bool g_SwapChainRebuild;
  // --- stuff that is relevant ---
  ui_handler_t user_interface;
  renderer_state_t renderer;

  map_data_t map_data;
};

void on_map_load(gfx_handler_t *handler, const char *map_path);

int init_gfx_handler(gfx_handler_t *handler);
int gfx_next_frame(gfx_handler_t *handler);
void gfx_cleanup(gfx_handler_t *handler);

#endif
