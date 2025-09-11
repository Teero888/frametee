#include "graphics_backend.h"
#include "cimgui.h"
#include "ddnet_map_loader.h"
#include "renderer.h"
#include "user_interface/user_interface.h"
#include <GLFW/glfw3.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#define ARRAYSIZE(_ARR) ((int)(sizeof(_ARR) / sizeof(*(_ARR))))
#define ENTITIES_PATH "data/textures/ddnet.png"

// --- Forward Declarations of Static Functions ---
static void glfw_error_callback(int error, const char *description);
static int init_window(gfx_handler_t *handler);
static int init_vulkan(gfx_handler_t *handler);
static int init_imgui(gfx_handler_t *handler);
static void cleanup_vulkan(gfx_handler_t *handler);
static void cleanup_vulkan_window(gfx_handler_t *handler);
// frame_render and frame_present are now folded into gfx_begin/end_frame
static void cleanup_map_resources(gfx_handler_t *handler);

// --- Vulkan Initialization Helpers ---
static VkResult create_instance(gfx_handler_t *handler, const char **extensions, uint32_t extensions_count);
static void select_physical_device(gfx_handler_t *handler);
static void create_logical_device(gfx_handler_t *handler);
static void create_descriptor_pool(gfx_handler_t *handler);
static void setup_window(gfx_handler_t *handler, struct ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface,
                         int width, int height);

// --- GLFW Error Callback ---
static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// --- Public API Implementation ---
int init_gfx_handler(gfx_handler_t *handler) {
  memset(handler, 0, sizeof(gfx_handler_t));
  handler->g_Allocator = NULL;
  handler->g_Instance = VK_NULL_HANDLE;
  handler->g_PhysicalDevice = VK_NULL_HANDLE;
  handler->g_Device = VK_NULL_HANDLE;
  handler->g_QueueFamily = (uint32_t)-1;
  handler->g_Queue = VK_NULL_HANDLE;
  handler->g_DebugReport = VK_NULL_HANDLE;
  handler->g_PipelineCache = VK_NULL_HANDLE;
  handler->g_DescriptorPool = VK_NULL_HANDLE;
  handler->g_MinImageCount = 2;
  handler->g_SwapChainRebuild = false;

  if (init_window(handler) != 0) {
    return 1;
  }

  if (init_vulkan(handler) != 0) {
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }
  if (renderer_init(handler) != 0) {
    cleanup_vulkan(handler);
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }
  if (init_imgui(handler) != 0) {
    renderer_cleanup(handler);
    cleanup_vulkan(handler);
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }

  ui_init(&handler->user_interface, handler);

  return 0;
}

int gfx_begin_frame(gfx_handler_t *handler) {
  if (glfwWindowShouldClose(handler->window))
    return FRAME_EXIT;

  glfwPollEvents();

  if (glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
    usleep(10000);
    return FRAME_SKIP;
  }
  
  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  if (fb_width > 0 && fb_height > 0 &&
      (handler->g_SwapChainRebuild || handler->g_MainWindowData.Width != fb_width ||
       handler->g_MainWindowData.Height != fb_height)) {
    vkDeviceWaitIdle(handler->g_Device);
    ImGui_ImplVulkan_SetMinImageCount(handler->g_MinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        handler->g_Instance, handler->g_PhysicalDevice, handler->g_Device, &handler->g_MainWindowData,
        handler->g_QueueFamily, handler->g_Allocator, fb_width, fb_height, handler->g_MinImageCount);
    handler->g_MainWindowData.FrameIndex = 0;
    handler->g_SwapChainRebuild = false;
  }
  
  // --- Acquire Image and Begin Command Buffer ---
  ImGui_ImplVulkanH_Window* wd = &handler->g_MainWindowData;
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  
  VkResult err = vkAcquireNextImageKHR(handler->g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_SwapChainRebuild = true;
    return FRAME_SKIP; // Still a valid frame, but we'll skip rendering
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame* fd = &wd->Frames.Data[wd->FrameIndex];
  err = vkWaitForFences(handler->g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
  check_vk_result(err);
  err = vkResetFences(handler->g_Device, 1, &fd->Fence);
  check_vk_result(err);
  
  err = vkResetCommandPool(handler->g_Device, fd->CommandPool, 0);
  check_vk_result(err);
  VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
  check_vk_result(err);

  VkRenderPassBeginInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = wd->RenderPass,
      .framebuffer = fd->Framebuffer,
      .renderArea = {{0, 0}, {(uint32_t)wd->Width, (uint32_t)wd->Height}},
      .clearValueCount = 1,
      .pClearValues = &wd->ClearValue
  };
  vkCmdBeginRenderPass(fd->CommandBuffer, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
  
  handler->current_frame_command_buffer = fd->CommandBuffer;

  // --- Start ImGui and Renderer Frames ---
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  igNewFrame();
  renderer_begin_frame(handler, handler->current_frame_command_buffer);

  // --- Draw Map ---
  if (handler->map_shader && handler->quad_mesh && handler->map_texture_count > 0) {
      float window_ratio = (float)wd->Width / (float)wd->Height;
      float map_ratio = (float)handler->map_data.width / (float)handler->map_data.height;
      if (isnan(map_ratio) || map_ratio == 0) map_ratio = 1.0f;

      float zoom = 1.0 / (handler->renderer.camera.zoom * fmax(handler->map_data.width, handler->map_data.height) * 0.001);
      if (isnan(zoom)) zoom = 1.0f;

      float aspect = 1.0f / (window_ratio / map_ratio);
      float lod = fmin(fmax(5.5f - log2f((1.0f / handler->map_data.width) / zoom * (wd->Width / 2.0f)), 0.0f), 6.0f);

      map_buffer_object_t ubo = {
          .transform = {handler->renderer.camera.pos[0], handler->renderer.camera.pos[1], zoom},
          .aspect = aspect,
          .lod = lod
      };

      void *ubos[] = {&ubo};
      VkDeviceSize ubo_sizes[] = {sizeof(ubo)};
      renderer_draw_mesh(handler, handler->current_frame_command_buffer, handler->quad_mesh, handler->map_shader,
                         handler->map_textures, handler->map_texture_count, ubos, ubo_sizes, 1);
  }
  
  return FRAME_OK;
}

void gfx_end_frame(gfx_handler_t *handler) {
  if (handler->g_SwapChainRebuild || glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
      // End the ImGui frame to avoid state issues, but don't render.
      igEndFrame();
      // We also need to end the render pass we started.
      if (handler->current_frame_command_buffer != VK_NULL_HANDLE) {
          vkCmdEndRenderPass(handler->current_frame_command_buffer);
          vkEndCommandBuffer(handler->current_frame_command_buffer);
          handler->current_frame_command_buffer = VK_NULL_HANDLE;
      }
      return;
  }

  renderer_end_frame(handler, handler->current_frame_command_buffer);

  igRender();
  ImDrawData *draw_data = igGetDrawData();
  ImGui_ImplVulkan_RenderDrawData(draw_data, handler->current_frame_command_buffer, VK_NULL_HANDLE);

  vkCmdEndRenderPass(handler->current_frame_command_buffer);

  ImGui_ImplVulkanH_Window* wd = &handler->g_MainWindowData;
  ImGui_ImplVulkanH_Frame* fd = &wd->Frames.Data[wd->FrameIndex];
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &image_acquired_semaphore,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &handler->current_frame_command_buffer,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &render_complete_semaphore
  };
  
  VkResult err = vkEndCommandBuffer(handler->current_frame_command_buffer);
  check_vk_result(err);
  err = vkQueueSubmit(handler->g_Queue, 1, &info, fd->Fence);
  check_vk_result(err);
  
  handler->current_frame_command_buffer = VK_NULL_HANDLE;

  // --- Present ---
  VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_complete_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &wd->Swapchain,
      .pImageIndices = &wd->FrameIndex
  };
  err = vkQueuePresentKHR(handler->g_Queue, &present_info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_SwapChainRebuild = true;
  } else {
    check_vk_result(err);
  }
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount;
}

void gfx_cleanup(gfx_handler_t *handler) {
  VkResult err = vkDeviceWaitIdle(handler->g_Device);
  check_vk_result(err);

  ui_cleanup(&handler->user_interface);
  cleanup_map_resources(handler);
  free_map_data(&handler->map_data);
  renderer_cleanup(handler);
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  igDestroyContext(NULL);
  cleanup_vulkan_window(handler);
  cleanup_vulkan(handler);
  glfwDestroyWindow(handler->window);
  handler->window = NULL;
  glfwTerminate();
}

// Helper to load a single map layer texture, returns default if fails
static texture_t *load_layer_texture(gfx_handler_t *handler, uint8_t *data, uint32_t width, uint32_t height) {
  if (!data)
    return handler->renderer.default_texture;
  texture_t *tex = renderer_load_texture_from_array(handler, data, width, height);
  return tex ? tex : handler->renderer.default_texture;
}

static void cleanup_map_resources(gfx_handler_t *handler) {
  if (handler->map_texture_count == 0) {
    return;
  }
  printf("Cleaning up previous map resources...\n");

  vkDeviceWaitIdle(handler->g_Device);

  for (uint32_t i = 0; i < handler->map_texture_count; ++i) {
    texture_t *tex = handler->map_textures[i];
    if (tex && tex != handler->renderer.default_texture) {
      renderer_destroy_texture(handler, tex);
    }
    handler->map_textures[i] = NULL;
  }
  handler->map_texture_count = 0;

  handler->map_shader = NULL;
  handler->quad_mesh = NULL;
}

void on_map_load(gfx_handler_t *handler, const char *map_path) {
  cleanup_map_resources(handler);
  free_map_data(&handler->map_data);
  handler->map_data = load_map(map_path);
  if (!handler->map_data.game_layer.data) {
    fprintf(stderr, "Failed to load map data: '%s'\n", map_path);
    return;
  }

  texture_t *entities_atlas = renderer_load_texture(handler, ENTITIES_PATH);
  if (!entities_atlas) {
    fprintf(stderr,
            "Failed to load entities at: '%s', you might have started the program from the wrong location.\n",
            ENTITIES_PATH);
    return;
  }
  printf("Loaded map: '%s' (%ux%u)\n", map_path, handler->map_data.width, handler->map_data.height);

  handler->map_shader =
      renderer_load_shader(handler, "data/shaders/map.vert.spv", "data/shaders/map.frag.spv");

  if (!handler->quad_mesh) {
    vertex_t quad_vertices[] = {
        {{-1.f, -1.f}, {1.0f, 1.0f, 1.0f}, {-1.f, 1.0f}}, // Top Left
        {{1.0f, -1.f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}, // Top Right
        {{1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, -1.f}}, // Bottom Right
        {{-1.f, 1.0f}, {1.0f, 1.0f, 1.0f}, {-1.f, -1.f}}  // Bottom Left
    };
    uint32_t quad_indices[] = {
        0, 1, 2, // First triangle
        2, 3, 0  // Second triangle
    };
    handler->quad_mesh = renderer_create_mesh(handler, quad_vertices, 4, quad_indices, 6);
  }

  handler->map_texture_count = 0;
  texture_t *entities_array =
      renderer_create_texture_array_from_atlas(handler, entities_atlas, 64, 64, 16, 16);

  handler->map_textures[handler->map_texture_count++] =
      entities_array ? entities_array : handler->renderer.default_texture;
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.game_layer.data, handler->map_data.width, handler->map_data.height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.front_layer.data, handler->map_data.width, handler->map_data.height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.tele_layer.type, handler->map_data.width, handler->map_data.height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.tune_layer.type, handler->map_data.width, handler->map_data.height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.speedup_layer.type, handler->map_data.width, handler->map_data.height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(
      handler, handler->map_data.switch_layer.type, handler->map_data.width, handler->map_data.height);
}


// --- Initialization and Cleanup ---
static int init_window(gfx_handler_t *handler) {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  handler->window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+Vulkan example", NULL, NULL);
  if (!handler->window) {
    glfwTerminate();
    return 1;
  }
  if (!glfwVulkanSupported()) {
    printf("GLFW: Vulkan Not Supported\n");
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }
  return 0;
}

static int init_vulkan(gfx_handler_t *handler) {
  uint32_t extensions_count = 0;
  const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
  if (glfw_extensions == NULL) {
    fprintf(stderr, "Error: glfwGetRequiredInstanceExtensions failed\n");
    return -1;
  }

  if (create_instance(handler, glfw_extensions, extensions_count) != VK_SUCCESS) {
    return -1;
  }

  select_physical_device(handler);
  create_logical_device(handler);
  create_descriptor_pool(handler);

  VkSurfaceKHR surface;
  VkResult err =
      glfwCreateWindowSurface(handler->g_Instance, handler->window, handler->g_Allocator, &surface);
  check_vk_result(err);

  int w, h;
  glfwGetFramebufferSize(handler->window, &w, &h);
  memset(&handler->g_MainWindowData, 0, sizeof(handler->g_MainWindowData));
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;

  wd->ClearValue.color.float32[0] = 0.8f;
  wd->ClearValue.color.float32[1] = 0.8f;
  wd->ClearValue.color.float32[2] = 0.9f;
  wd->ClearValue.color.float32[3] = 1.0f;
  wd->ClearEnable = true;

  setup_window(handler, wd, surface, w, h);
  return 0;
}

static int init_imgui(gfx_handler_t *handler) {
  igCreateContext(NULL);
  ImGuiIO *io = igGetIO_Nil();
  io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  igStyleColorsDark(NULL);

  ImGui_ImplGlfw_InitForVulkan(handler->window, true);
  ImGui_ImplVulkan_InitInfo init_info = {.Instance = handler->g_Instance,
                                         .PhysicalDevice = handler->g_PhysicalDevice,
                                         .Device = handler->g_Device,
                                         .QueueFamily = handler->g_QueueFamily,
                                         .Queue = handler->g_Queue,
                                         .PipelineCache = handler->g_PipelineCache,
                                         .DescriptorPool = handler->g_DescriptorPool,
                                         .RenderPass = handler->g_MainWindowData.RenderPass,
                                         .Subpass = 0,
                                         .MinImageCount = handler->g_MinImageCount,
                                         .ImageCount = handler->g_MainWindowData.ImageCount,
                                         .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                                         .Allocator = handler->g_Allocator,
                                         .CheckVkResultFn = check_vk_result};
  ImGui_ImplVulkan_Init(&init_info);
  return 0;
}

static void cleanup_vulkan(gfx_handler_t *handler) {
  if (handler->g_DescriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(handler->g_Device, handler->g_DescriptorPool, handler->g_Allocator);
    handler->g_DescriptorPool = VK_NULL_HANDLE;
  }
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  PFN_vkDestroyDebugReportCallbackEXT f_vkDestroyDebugReportCallbackEXT =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_Instance,
                                                                 "vkDestroyDebugReportCallbackEXT");
  if (f_vkDestroyDebugReportCallbackEXT && handler->g_DebugReport != VK_NULL_HANDLE) {
    f_vkDestroyDebugReportCallbackEXT(handler->g_Instance, handler->g_DebugReport, handler->g_Allocator);
    handler->g_DebugReport = VK_NULL_HANDLE;
  }
#endif
  if (handler->g_Device != VK_NULL_HANDLE) {
    vkDestroyDevice(handler->g_Device, handler->g_Allocator);
    handler->g_Device = VK_NULL_HANDLE;
  }
  if (handler->g_Instance != VK_NULL_HANDLE) {
    vkDestroyInstance(handler->g_Instance, handler->g_Allocator);
    handler->g_Instance = VK_NULL_HANDLE;
  }
}

static void cleanup_vulkan_window(gfx_handler_t *handler) {
  ImGui_ImplVulkanH_DestroyWindow(handler->g_Instance, handler->g_Device, &handler->g_MainWindowData,
                                  handler->g_Allocator);
}

// --- Vulkan Setup Helpers ---
static bool is_extension_available(const VkExtensionProperties *properties, uint32_t properties_count,
                                   const char *extension) {
  for (uint32_t i = 0; i < properties_count; i++) {
    if (strcmp(properties[i].extensionName, extension) == 0) {
      return true;
    }
  }
  return false;
}

static VkResult create_instance(gfx_handler_t *handler, const char **glfw_extensions,
                                uint32_t glfw_extensions_count) {
  VkResult err;

  uint32_t properties_count;
  vkEnumerateInstanceExtensionProperties(NULL, &properties_count, NULL);
  VkExtensionProperties *properties = malloc(sizeof(VkExtensionProperties) * properties_count);
  vkEnumerateInstanceExtensionProperties(NULL, &properties_count, properties);

  const char **extensions = malloc(sizeof(const char *) * (glfw_extensions_count + 10)); // Generous buffer
  uint32_t extensions_count = glfw_extensions_count;
  memcpy(extensions, glfw_extensions, glfw_extensions_count * sizeof(const char *));

  if (is_extension_available(properties, properties_count,
                             VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
    extensions[extensions_count++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
  if (is_extension_available(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions[extensions_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
  }
#endif
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  extensions[extensions_count++] = "VK_EXT_debug_report";
#endif

  VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .enabledExtensionCount = extensions_count,
      .ppEnabledExtensionNames = extensions,
  };

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
  if (is_extension_available(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
  const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
  create_info.enabledLayerCount = 1;
  create_info.ppEnabledLayerNames = layers;
#endif

  err = vkCreateInstance(&create_info, handler->g_Allocator, &handler->g_Instance);
  check_vk_result(err);
  free(properties);
  free(extensions);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
  PFN_vkCreateDebugReportCallbackEXT f_vkCreateDebugReportCallbackEXT =
      (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_Instance,
                                                                "vkCreateDebugReportCallbackEXT");
  assert(f_vkCreateDebugReportCallbackEXT != NULL);
  VkDebugReportCallbackCreateInfoEXT debug_report_ci = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
      .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
               VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT,
      .pfnCallback = debug_report,
  };
  err = f_vkCreateDebugReportCallbackEXT(handler->g_Instance, &debug_report_ci, handler->g_Allocator,
                                         &handler->g_DebugReport);
  check_vk_result(err);
#endif

  return err;
}

static void select_physical_device(gfx_handler_t *handler) {
  handler->g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(handler->g_Instance);
  assert(handler->g_PhysicalDevice != VK_NULL_HANDLE);
  handler->g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(handler->g_PhysicalDevice);
  assert(handler->g_QueueFamily != (uint32_t)-1);
}

static void create_logical_device(gfx_handler_t *handler) {
  const char *device_extensions[] = {"VK_KHR_swapchain"};
  uint32_t device_extensions_count = 1;

  const float queue_priority[] = {1.0f};
  VkDeviceQueueCreateInfo queue_info[1] = {{
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = handler->g_QueueFamily,
      .queueCount = 1,
      .pQueuePriorities = queue_priority,
  }};
  VkDeviceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = ARRAYSIZE(queue_info),
      .pQueueCreateInfos = queue_info,
      .enabledExtensionCount = device_extensions_count,
      .ppEnabledExtensionNames = device_extensions,
  };
  VkResult err =
      vkCreateDevice(handler->g_PhysicalDevice, &create_info, handler->g_Allocator, &handler->g_Device);
  check_vk_result(err);
  vkGetDeviceQueue(handler->g_Device, handler->g_QueueFamily, 0, &handler->g_Queue);
}

static void create_descriptor_pool(gfx_handler_t *handler) {
  // This pool is for Dear ImGui only
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
  };
  VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                          .maxSets = 1000 * ARRAYSIZE(pool_sizes),
                                          .poolSizeCount = (uint32_t)ARRAYSIZE(pool_sizes),
                                          .pPoolSizes = pool_sizes};
  VkResult err =
      vkCreateDescriptorPool(handler->g_Device, &pool_info, handler->g_Allocator, &handler->g_DescriptorPool);
  check_vk_result(err);
}

static void setup_window(gfx_handler_t *handler, ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface,
                         int width, int height) {
  wd->Surface = surface;

  VkBool32 res;
  vkGetPhysicalDeviceSurfaceSupportKHR(handler->g_PhysicalDevice, handler->g_QueueFamily, wd->Surface, &res);
  if (res != VK_TRUE) {
    fprintf(stderr, "Error: no WSI support on physical device 0\n");
    exit(-1);
  }

  const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
  const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
      handler->g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
      (size_t)ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

  VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
  wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(handler->g_PhysicalDevice, wd->Surface,
                                                        &present_modes[0], ARRAYSIZE(present_modes));

  assert(handler->g_MinImageCount >= 2);
  ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_Instance, handler->g_PhysicalDevice, handler->g_Device,
                                         wd, handler->g_QueueFamily, handler->g_Allocator, width, height,
                                         handler->g_MinImageCount);
}

// --- Frame Rendering and Presentation ---
static void frame_render(gfx_handler_t *handler, ImDrawData *draw_data) {
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;

  VkResult err = vkAcquireNextImageKHR(handler->g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore,
                                       VK_NULL_HANDLE, &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_SwapChainRebuild = true;
    return;
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames.Data[wd->FrameIndex];
  {
    err = vkWaitForFences(handler->g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    check_vk_result(err);
    err = vkResetFences(handler->g_Device, 1, &fd->Fence);
    check_vk_result(err);
  }
  {
    err = vkResetCommandPool(handler->g_Device, fd->CommandPool, 0);
    check_vk_result(err);
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
    check_vk_result(err);
  }
  {
    VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = wd->RenderPass,
                                  .framebuffer = fd->Framebuffer,
                                  .renderArea = {{0, 0}, {wd->Width, wd->Height}},
                                  .clearValueCount = 1,
                                  .pClearValues = &wd->ClearValue};
    vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
  }

  // --- Immediate Mode Drawing Logic ---
  renderer_begin_frame(handler, fd->CommandBuffer);

  if (handler->map_shader && handler->quad_mesh && handler->map_texture_count > 0) {
    int width, height;
    glfwGetFramebufferSize(handler->window, &width, &height);
    if (width > 0 && height > 0) {
      float window_ratio = (float)width / (float)height;
      float map_ratio = (float)handler->map_data.width / (float)handler->map_data.height;
      if (isnan(map_ratio) || map_ratio == 0)
        map_ratio = 1.0f;

      float zoom = 1.0 /
                   (handler->renderer.camera.zoom * fmax(handler->map_data.width, handler->map_data.height) *
                    0.001);
      if (isnan(zoom))
        zoom = 1.0f;

      float aspect = 1.0f / (window_ratio / map_ratio);
      float lod =
          fmin(fmax(5.5f - log2f((1.0f / handler->map_data.width) / zoom * (width / 2.0f)), 0.0f), 6.0f);

      map_buffer_object_t ubo = {.transform = {handler->renderer.camera.pos[0],
                                               handler->renderer.camera.pos[1], zoom},
                                 .aspect = aspect,
                                 .lod = lod};

      void *ubos[] = {&ubo};
      VkDeviceSize ubo_sizes[] = {sizeof(ubo)};
      renderer_draw_mesh(handler, fd->CommandBuffer, handler->quad_mesh, handler->map_shader,
                         handler->map_textures, handler->map_texture_count, ubos, ubo_sizes, 1);
    }
  }

  // Draw primitives on top
  renderer_end_frame(handler, fd->CommandBuffer);
  // --- End Immediate Mode Drawing ---

  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer, VK_NULL_HANDLE);

  vkCmdEndRenderPass(fd->CommandBuffer);
  {
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .waitSemaphoreCount = 1,
                         .pWaitSemaphores = &image_acquired_semaphore,
                         .pWaitDstStageMask = &wait_stage,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &fd->CommandBuffer,
                         .signalSemaphoreCount = 1,
                         .pSignalSemaphores = &render_complete_semaphore};
    err = vkEndCommandBuffer(fd->CommandBuffer);
    check_vk_result(err);
    err = vkQueueSubmit(handler->g_Queue, 1, &info, fd->Fence);
    check_vk_result(err);
  }
}

static void frame_present(gfx_handler_t *handler) {
  if (handler->g_SwapChainRebuild)
    return;
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &render_complete_semaphore,
                           .swapchainCount = 1,
                           .pSwapchains = &wd->Swapchain,
                           .pImageIndices = &wd->FrameIndex};
  VkResult err = vkQueuePresentKHR(handler->g_Queue, &info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_SwapChainRebuild = true;
  } else {
    check_vk_result(err);
  }
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}