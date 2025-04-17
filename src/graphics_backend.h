#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "user_interface.h"
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cimgui.h>
#include <cimgui_impl.h>

#include <stdint.h>

typedef struct {
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
  ui_handler user_interface;
} gfx_handler;

int init_gfx_handler(gfx_handler *handler);

int gfx_next_frame(gfx_handler *handler);
void gfx_cleanup(gfx_handler *handler);

#endif
