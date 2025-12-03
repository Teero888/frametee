#include "graphics_backend.h"
#include "../logger/logger.h"
#include "../user_interface/user_interface.h"
#include "cimgui.h"
#include "renderer.h"
#include <GLFW/glfw3.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define ARRAYSIZE(_ARR) ((int)(sizeof(_ARR) / sizeof(*(_ARR))))
#define ENTITIES_PATH "data/textures/ddnet.png"

static const char *LOG_SOURCE = "GfxBackend";

// forward declarations of static functions
static void glfw_error_callback(int error, const char *description);
static int init_window(gfx_handler_t *handler);
static int init_vulkan(gfx_handler_t *handler);
static int init_imgui(gfx_handler_t *handler);
static void cleanup_vulkan(gfx_handler_t *handler);
static void cleanup_vulkan_window(gfx_handler_t *handler);
// frame_render and frame_present are now folded into gfx_begin/end_frame
static void cleanup_map_resources(gfx_handler_t *handler);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT object_type, uint64_t object,
                                                   size_t location, int32_t message_code, const char *layer_prefix, const char *message,
                                                   void *user_data);
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                           const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data);
#endif

// offscreen helpers
static int init_offscreen_resources(gfx_handler_t *handler, uint32_t width, uint32_t height);
static void destroy_offscreen_resources(gfx_handler_t *handler);
static int recreate_offscreen_if_needed(gfx_handler_t *handler, uint32_t width, uint32_t height);

// vulkan initialization helpers
static VkResult create_instance(gfx_handler_t *handler, const char **extensions, uint32_t extensions_count);
static void select_physical_device(gfx_handler_t *handler);
static void create_logical_device(gfx_handler_t *handler);
static void create_descriptor_pool(gfx_handler_t *handler);
static void setup_window(gfx_handler_t *handler, struct ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int width, int height);

// glfw error callback
static void glfw_error_callback(int error, const char *description) { log_error("GLFW", "%d: %s", error, description); }

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT object_type, uint64_t object,
                                                   size_t location, int32_t message_code, const char *layer_prefix, const char *message,
                                                   void *user_data) {
  (void)flags;
  (void)object_type;
  (void)object;
  (void)location;
  (void)user_data;

  log_error(LOG_SOURCE, "[vulkan][%s] code %d: %s\n", layer_prefix ? layer_prefix : "unknown", message_code, message);
  fflush(stderr);
  return VK_FALSE; // do not abort
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                           const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) {
  (void)type;
  (void)user_data;

  const char *severity_str = "";
  switch (severity) {
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
    severity_str = "VERBOSE";
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
    severity_str = "INFO";
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
    severity_str = "WARN";
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
    severity_str = "ERROR";
    break;
  default:
    severity_str = "UNKNOWN";
    break;
  }

  log_error(LOG_SOURCE, "[vulkan][%s] %s\n", severity_str, callback_data && callback_data->pMessage ? callback_data->pMessage : "(null)");
  fflush(stderr);
  return VK_FALSE;
}
#endif

static void cursor_position_callback(GLFWwindow *window, double xpos, double ypos) {
  gfx_handler_t *handler = glfwGetWindowUserPointer(window);
  if (!handler) return;

  handler->raw_mouse.dx += xpos - handler->raw_mouse.x;
  handler->raw_mouse.dy += ypos - handler->raw_mouse.y;
  handler->raw_mouse.x = xpos;
  handler->raw_mouse.y = ypos;
}

int init_gfx_handler(gfx_handler_t *handler) {
  memset(handler, 0, sizeof(gfx_handler_t));
  handler->g_allocator = NULL;
  handler->g_instance = VK_NULL_HANDLE;
  handler->g_physical_device = VK_NULL_HANDLE;
  handler->g_device = VK_NULL_HANDLE;
  handler->g_queue_family = (uint32_t)-1;
  handler->g_queue = VK_NULL_HANDLE;
  handler->g_debug_report = VK_NULL_HANDLE;
  handler->g_debug_messenger = VK_NULL_HANDLE;
  handler->g_pipeline_cache = VK_NULL_HANDLE;
  handler->g_descriptor_pool = VK_NULL_HANDLE;
  handler->g_min_image_count = 2;
  handler->g_swap_chain_rebuild = false;
  handler->offscreen_initialized = false;
  handler->offscreen_image = VK_NULL_HANDLE;
  handler->offscreen_image_view = VK_NULL_HANDLE;
  handler->offscreen_sampler = VK_NULL_HANDLE;
  handler->offscreen_framebuffer = VK_NULL_HANDLE;
  handler->offscreen_render_pass = VK_NULL_HANDLE;
  handler->offscreen_texture = NULL;

  if (init_window(handler) != 0) {
    return 1;
  }

  glfwSetWindowUserPointer(handler->window, handler);
  glfwSetCursorPosCallback(handler->window, cursor_position_callback);
  handler->raw_mouse.x = handler->raw_mouse.y = 0.0;
  handler->raw_mouse.dx = handler->raw_mouse.dy = 0.0;

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

  texture_t *entities_atlas = renderer_load_texture(handler, ENTITIES_PATH);
  if (!entities_atlas) {
    log_error(LOG_SOURCE,
              "Failed to load entities atlas at '%s'. The program might have been started from the wrong "
              "directory.",
              ENTITIES_PATH);
    return 1;
  }

  handler->map_shader = renderer_load_shader(handler, "data/shaders/map.vert.spv", "data/shaders/map.frag.spv");

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

  handler->entities_array = renderer_create_texture_array_from_atlas(handler, entities_atlas, 64, 64, 16, 16);
  handler->entities_atlas = entities_atlas;

  handler->default_skin = renderer_load_skin_from_file(handler, "data/textures/default.png", NULL);
  handler->x_ninja_skin = renderer_load_skin_from_file(handler, "data/textures/x_ninja.png", NULL);
  handler->x_spec_skin = renderer_load_skin_from_file(handler, "data/textures/x_spec.png", NULL);
  if (handler->default_skin == -1) {
    log_error(LOG_SOURCE, "Default skin 'default.png' not found. The program might have been started from the wrong path.");
  }
  if (handler->x_ninja_skin == -1) {
    log_error(LOG_SOURCE, "Ninja skin 'x_ninja.png' not found. The program might have been started from the wrong path.");
  }
  if (handler->x_spec_skin == -1) {
    log_error(LOG_SOURCE, "Spec skin 'x_spec.png' not found. The program might have been started from the wrong path.");
  }

  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  handler->viewport[0] = fb_width;
  handler->viewport[1] = fb_height;

  // initialize offscreen target to match the viewport size
  if (init_offscreen_resources(handler, (uint32_t)fb_width, (uint32_t)fb_height) != 0) {
    log_warn(LOG_SOURCE, "Failed to create offscreen resources. The ImGui game view will be disabled.");
  }

  ui_init(&handler->user_interface, handler);

  return 0;
}

int gfx_begin_frame(gfx_handler_t *handler) {
  if (glfwWindowShouldClose(handler->window)) return FRAME_EXIT;

  glfwPollEvents();

  if (glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
    ImGui_ImplGlfw_Sleep(10);
    return FRAME_SKIP;
  }

  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  if (fb_width > 0 && fb_height > 0 &&
      (handler->g_swap_chain_rebuild || handler->g_main_window_data.Width != fb_width || handler->g_main_window_data.Height != fb_height)) {
    vkDeviceWaitIdle(handler->g_device);
    ImGui_ImplVulkan_SetMinImageCount(handler->g_min_image_count);
    ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_instance, handler->g_physical_device, handler->g_device, &handler->g_main_window_data,
                                           handler->g_queue_family, handler->g_allocator, fb_width, fb_height, handler->g_min_image_count);
    handler->g_main_window_data.FrameIndex = 0;
    handler->g_swap_chain_rebuild = false;

    int fb_width, fb_height;
    glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
    recreate_offscreen_if_needed(handler, (uint32_t)fb_width, (uint32_t)fb_height);
  }

  // Acquire Image and Begin Command Buffer
  ImGui_ImplVulkanH_Window *wd = &handler->g_main_window_data;
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  // Ensure the previous use of this frame's fence is completed, so reuse of semaphores is safe
  ImGui_ImplVulkanH_Frame *acquire_fd = &wd->Frames.Data[wd->FrameIndex];
  vkWaitForFences(handler->g_device, 1, &acquire_fd->Fence, VK_TRUE, UINT64_MAX);

  VkResult err = vkAcquireNextImageKHR(handler->g_device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_swap_chain_rebuild = true;
    // Skip this frame if the swapchain is invalid
    return FRAME_SKIP;
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames.Data[wd->FrameIndex];
  err = vkWaitForFences(handler->g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
  check_vk_result(err);
  err = vkResetFences(handler->g_device, 1, &fd->Fence);
  check_vk_result(err);

  err = vkResetCommandPool(handler->g_device, fd->CommandPool, 0);
  check_vk_result(err);
  VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
  check_vk_result(err);

  handler->current_frame_command_buffer = fd->CommandBuffer;

  // Begin offscreen render pass (for game rendering)
  if (handler->offscreen_initialized && handler->offscreen_render_pass != VK_NULL_HANDLE && handler->offscreen_framebuffer != VK_NULL_HANDLE) {
    VkClearValue clear = {.color = {.float32 = {30.f / 255.f, 35.f / 255.f, 40.f / 255.f, 1.0f}}};
    VkRenderPassBeginInfo rp_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = handler->offscreen_render_pass,
                                     .framebuffer = handler->offscreen_framebuffer,
                                     .renderArea = {{0, 0}, {handler->offscreen_width, handler->offscreen_height}},
                                     .clearValueCount = 1,
                                     .pClearValues = &clear};
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
  }

  // Start ImGui and Renderer Frames
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  igNewFrame();
  renderer_begin_frame(handler, handler->current_frame_command_buffer);

  return FRAME_OK;
}

bool gfx_end_frame(gfx_handler_t *handler) {
  bool hovered = false;

  if (handler->g_swap_chain_rebuild || glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
    // End the ImGui frame to avoid state issues, but don't render.
    igEndFrame();
    // We also need to end the render pass we started.
    if (handler->current_frame_command_buffer != VK_NULL_HANDLE) {
      // End any offscreen pass if open
      if (handler->offscreen_initialized) {
        vkCmdEndRenderPass(handler->current_frame_command_buffer);
      }
      vkEndCommandBuffer(handler->current_frame_command_buffer);
      handler->current_frame_command_buffer = VK_NULL_HANDLE;
    }
    return hovered;
  }

  // Finish game rendering into offscreen target
  if (handler->offscreen_initialized) {
    renderer_end_frame(handler, handler->current_frame_command_buffer);
    vkCmdEndRenderPass(handler->current_frame_command_buffer);
  } else {
    renderer_end_frame(handler, handler->current_frame_command_buffer);
  }

  // Begin swapchain render pass for ImGui
  ImGui_ImplVulkanH_Window *wd = &handler->g_main_window_data;
  ImGui_ImplVulkanH_Frame *fd = &wd->Frames.Data[wd->FrameIndex];
  VkRenderPassBeginInfo rp_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = wd->RenderPass,
                                   .framebuffer = fd->Framebuffer,
                                   .renderArea = {{0, 0}, {(uint32_t)wd->Width, (uint32_t)wd->Height}},
                                   .clearValueCount = 1,
                                   .pClearValues = &wd->ClearValue};
  vkCmdBeginRenderPass(handler->current_frame_command_buffer, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

  hovered = ui_render_late(&handler->user_interface);

  igRender();
  ImDrawData *draw_data = igGetDrawData();
  ImGui_ImplVulkan_RenderDrawData(draw_data, handler->current_frame_command_buffer, VK_NULL_HANDLE);

  // End swapchain render pass
  vkCmdEndRenderPass(handler->current_frame_command_buffer);

  // retire textures whose frame fences are now done
  uint32_t cur_frame = handler->g_main_window_data.FrameIndex;
  for (uint32_t i = 0; i < handler->retire_count;) {
    if ((cur_frame - handler->retire_textures[i].frame_index) > 2) {
      texture_t *tex = handler->retire_textures[i].tex;
      vkDestroySampler(handler->g_device, tex->sampler, handler->g_allocator);
      vkDestroyImageView(handler->g_device, tex->image_view, handler->g_allocator);
      vkDestroyImage(handler->g_device, tex->image, handler->g_allocator);
      vkFreeMemory(handler->g_device, tex->memory, handler->g_allocator);
      memset(tex, 0, sizeof(texture_t));
      handler->retire_textures[i] = handler->retire_textures[--handler->retire_count];
      continue;
    }
    i++;
  }

  // End the command buffer and submit like before.
  VkResult err = vkEndCommandBuffer(handler->current_frame_command_buffer);
  check_vk_result(err);
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .waitSemaphoreCount = 1,
                              .pWaitSemaphores = &image_acquired_semaphore,
                              .pWaitDstStageMask = &wait_stage,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &handler->current_frame_command_buffer,
                              .signalSemaphoreCount = 1,
                              .pSignalSemaphores = &render_complete_semaphore};

  err = vkQueueSubmit(handler->g_queue, 1, &submit_info, fd->Fence);
  check_vk_result(err);

  handler->current_frame_command_buffer = VK_NULL_HANDLE;
  ImGuiIO *io = igGetIO_Nil();
  if (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    igUpdatePlatformWindows();
    igRenderPlatformWindowsDefault(NULL, NULL);
  }
  // Present
  VkPresentInfoKHR present_info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &render_complete_semaphore,
                                   .swapchainCount = 1,
                                   .pSwapchains = &wd->Swapchain,
                                   .pImageIndices = &wd->FrameIndex};
  err = vkQueuePresentKHR(handler->g_queue, &present_info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_swap_chain_rebuild = true;
  } else {
    check_vk_result(err);
  }
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount;
  return hovered;
}

void gfx_cleanup(gfx_handler_t *handler) {
  VkResult err = vkDeviceWaitIdle(handler->g_device);
  check_vk_result(err);

  ui_cleanup(&handler->user_interface);

  cleanup_map_resources(handler);
  if (handler->entities_array) renderer_destroy_texture(handler, handler->entities_array);
  handler->map_textures[0] = NULL;

  physics_free(&handler->physics_handler);
  handler->map_data = 0x0;

  renderer_cleanup(handler);

  // destroy offscreen resources before ImGui shutdown (ImGui holds descriptor references)
  destroy_offscreen_resources(handler);

  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  igDestroyContext(NULL);
  cleanup_vulkan_window(handler);
  cleanup_vulkan(handler);
  glfwDestroyWindow(handler->window);
  handler->window = NULL;
  glfwTerminate();
}

static texture_t *load_layer_texture(gfx_handler_t *handler, uint8_t **data, uint32_t width, uint32_t height) {
  if (!data) return handler->renderer.default_texture;
  texture_t *tex = renderer_load_compact_texture_from_array(handler, data, width, height);
  return tex ? tex : handler->renderer.default_texture;
}

static void cleanup_map_resources(gfx_handler_t *handler) {
  if (handler->map_texture_count == 0) {
    return;
  }
  log_info(LOG_SOURCE, "Cleaning up previous map resources...");

  vkDeviceWaitIdle(handler->g_device);
  for (uint32_t i = 1; i < handler->map_texture_count; ++i) {
    texture_t *tex = handler->map_textures[i];
    if (tex && tex != handler->renderer.default_texture && tex != handler->entities_array) {
      renderer_destroy_texture(handler, tex);
    }
    handler->map_textures[i] = NULL;
  }
  handler->map_texture_count = 0;
}

void on_map_load(gfx_handler_t *handler) {
  cleanup_map_resources(handler);

  handler->renderer.camera.pos[0] = 0.5f;
  handler->renderer.camera.pos[1] = 0.5f;
  handler->map_data = &handler->physics_handler.collision.m_MapData;

  // entities texture
  handler->map_textures[handler->map_texture_count++] = handler->entities_array ? handler->entities_array : handler->renderer.default_texture;

  uint8_t *map[2][3] = {
      {handler->map_data->game_layer.data, handler->map_data->front_layer.data, handler->map_data->tele_layer.type},
      {handler->map_data->tune_layer.type, handler->map_data->speedup_layer.type, handler->map_data->switch_layer.type},
  };
  // collision textures
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(handler, map[0], handler->map_data->width, handler->map_data->height);
  handler->map_textures[handler->map_texture_count++] = load_layer_texture(handler, map[1], handler->map_data->width, handler->map_data->height);

  // update physics data
  wc_copy_world(&handler->user_interface.timeline.vec.data[0], &handler->physics_handler.world);
  wc_copy_world(&handler->user_interface.timeline.previous_world, &handler->physics_handler.world);
}

void on_map_load_path(gfx_handler_t *handler, const char *map_path) {
  timeline_cleanup(&handler->user_interface.timeline);
  timeline_init(&handler->user_interface);
  physics_free(&handler->physics_handler);
  physics_init(&handler->physics_handler, map_path);

  if (!handler->physics_handler.collision.m_MapData.game_layer.data) {
    log_error(LOG_SOURCE, "Failed to load map data from '%s'", map_path);
    return;
  }
  log_info(LOG_SOURCE, "Loaded map '%s' (%ux%u)", map_path, handler->map_data->width, handler->map_data->height);

  on_map_load(handler);
}

void on_map_load_mem(struct gfx_handler_t *handler, const unsigned char *map_buffer, size_t size) {
  physics_free(&handler->physics_handler);
  physics_init_from_memory(&handler->physics_handler, map_buffer, size);
  if (!handler->physics_handler.collision.m_MapData.game_layer.data) {
    log_error(LOG_SOURCE, "Failed to load map data from save file");
    return;
  }
  on_map_load(handler);
}

// initialization and cleanup
static int init_window(gfx_handler_t *handler) {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) return 1;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  handler->window = glfwCreateWindow(1920, 1080, "frametee", NULL, NULL);
  if (!handler->window) {
    glfwTerminate();
    return 1;
  }
  if (!glfwVulkanSupported()) {
    log_error("GLFW", "Vulkan is not supported on this system.");
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
    log_error("Vulkan", "glfwGetRequiredInstanceExtensions failed.");
    return -1;
  }

  if (create_instance(handler, glfw_extensions, extensions_count) != VK_SUCCESS) {
    return -1;
  }

  select_physical_device(handler);
  create_logical_device(handler);
  create_descriptor_pool(handler);

  VkSurfaceKHR surface;
  VkResult err = glfwCreateWindowSurface(handler->g_instance, handler->window, handler->g_allocator, &surface);
  check_vk_result(err);

  int w, h;
  glfwGetFramebufferSize(handler->window, &w, &h);
  memset(&handler->g_main_window_data, 0, sizeof(handler->g_main_window_data));
  ImGui_ImplVulkanH_Window *wd = &handler->g_main_window_data;

  // Background color
  wd->ClearValue.color.float32[0] = 0.f;
  wd->ClearValue.color.float32[1] = 0.f;
  wd->ClearValue.color.float32[2] = 0.f;
  wd->ClearValue.color.float32[3] = 1.0f;
  wd->ClearEnable = true;

  setup_window(handler, wd, surface, w, h);
  return 0;
}

static ImVec4 hex_vec4(const char *hex, float alpha) {
  unsigned int r, g, b;
  sscanf(hex, "%02x%02x%02x", &r, &g, &b);
  return (ImVec4){(float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, alpha};
}

void ayu_dark(void) {
  ImGuiStyle *style = igGetStyle();

  // Base Colors
  ImVec4 bg_main = hex_vec4("0A0E14", 1.0f);
  ImVec4 bg_panel = hex_vec4("0F131A", 1.0f);
  ImVec4 bg_line = hex_vec4("151A1F", 1.0f);

  ImVec4 fg_text = hex_vec4("E6E1CF", 1.0f);
  ImVec4 fg_inactive = hex_vec4("565B66", 1.0f);
  ImVec4 shadow = hex_vec4("000000", 0.5f);

  // Accent & Syntax
  ImVec4 accent_yellow = hex_vec4("E6B450", 1.0f);
  ImVec4 accent_orange = hex_vec4("FF8F40", 1.0f);
  ImVec4 accent_green = hex_vec4("AAD94C", 1.0f);
  ImVec4 accent_blue = hex_vec4("39BAE6", 1.0f);

  // Style Metrics
  style->WindowPadding = (ImVec2){8, 8};
  style->FramePadding = (ImVec2){6, 4};
  style->ItemSpacing = (ImVec2){8, 4};
  style->ScrollbarSize = 14;
  style->GrabMinSize = 12;

  style->WindowRounding = 3;
  style->FrameRounding = 3;
  style->TabRounding = 3;
  style->ScrollbarRounding = 8;

  ImVec4 *colors = style->Colors;

  // Text
  colors[ImGuiCol_Text] = fg_text;
  colors[ImGuiCol_TextDisabled] = fg_inactive;

  // Backgrounds
  colors[ImGuiCol_WindowBg] = bg_main;
  colors[ImGuiCol_ChildBg] = bg_panel;
  colors[ImGuiCol_PopupBg] = bg_panel;
  colors[ImGuiCol_Border] = bg_line;
  colors[ImGuiCol_BorderShadow] = shadow;

  // Frames & widgets
  colors[ImGuiCol_FrameBg] = bg_line;
  colors[ImGuiCol_FrameBgHovered] = hex_vec4("475266", 0.25f);
  colors[ImGuiCol_FrameBgActive] = accent_yellow;

  // Titles
  colors[ImGuiCol_TitleBg] = bg_panel;
  colors[ImGuiCol_TitleBgActive] = bg_panel;
  colors[ImGuiCol_TitleBgCollapsed] = bg_main;

  // Scrollbar
  colors[ImGuiCol_ScrollbarBg] = hex_vec4("0F131A", 0.8f);
  colors[ImGuiCol_ScrollbarGrab] = fg_inactive;
  colors[ImGuiCol_ScrollbarGrabHovered] = accent_yellow;
  colors[ImGuiCol_ScrollbarGrabActive] = accent_orange;

  // Buttons
  colors[ImGuiCol_Button] = hex_vec4("D4652F", 1.0f);
  colors[ImGuiCol_ButtonHovered] = hex_vec4("E67D4A", 1.0f);
  colors[ImGuiCol_ButtonActive] = hex_vec4("C25A29", 1.0f);

  // Tabs
  colors[ImGuiCol_Tab] = bg_line;
  colors[ImGuiCol_TabHovered] = accent_blue;
  colors[ImGuiCol_TabSelected] = bg_panel;
  colors[ImGuiCol_TabDimmed] = bg_line;
  colors[ImGuiCol_TabDimmedSelected] = bg_panel;

  // Selections
  colors[ImGuiCol_Header] = hex_vec4("409FFF", 0.15f);
  colors[ImGuiCol_HeaderHovered] = hex_vec4("409FFF", 0.25f);
  colors[ImGuiCol_HeaderActive] = hex_vec4("FF6F40", 1.0f);

  colors[ImGuiCol_TextSelectedBg] = hex_vec4("409FFF", 0.35f);

  // Special
  colors[ImGuiCol_CheckMark] = accent_green;
  colors[ImGuiCol_SliderGrab] = accent_yellow;
  colors[ImGuiCol_SliderGrabActive] = accent_orange;

  colors[ImGuiCol_PlotLines] = accent_blue;
  colors[ImGuiCol_PlotHistogram] = accent_green;

  // Navigation highlight
  colors[ImGuiCol_NavCursor] = accent_yellow;
  colors[ImGuiCol_NavWindowingHighlight] = accent_yellow;
  colors[ImGuiCol_ModalWindowDimBg] = shadow;
}

static int init_imgui(gfx_handler_t *handler) {
  igCreateContext(NULL);
  ImGuiIO *io = igGetIO_Nil();
  io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // io->ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ayu_dark();

  ImGui_ImplGlfw_InitForVulkan(handler->window, true);
  ImGui_ImplVulkan_InitInfo init_info = {.Instance = handler->g_instance,
                                         .PhysicalDevice = handler->g_physical_device,
                                         .Device = handler->g_device,
                                         .QueueFamily = handler->g_queue_family,
                                         .Queue = handler->g_queue,
                                         .PipelineCache = handler->g_pipeline_cache,
                                         .DescriptorPool = handler->g_descriptor_pool,
                                         .RenderPass = handler->g_main_window_data.RenderPass,
                                         .Subpass = 0,
                                         .MinImageCount = handler->g_min_image_count,
                                         .ImageCount = handler->g_main_window_data.ImageCount,
                                         .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                                         .Allocator = handler->g_allocator,
                                         .CheckVkResultFn = check_vk_result};
  ImGui_ImplVulkan_Init(&init_info);
  return 0;
}

static void cleanup_vulkan(gfx_handler_t *handler) {
  if (handler->g_descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(handler->g_device, handler->g_descriptor_pool, handler->g_allocator);
    handler->g_descriptor_pool = VK_NULL_HANDLE;
  }
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  PFN_vkDestroyDebugReportCallbackEXT f_vkDestroyDebugReportCallbackEXT =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_instance, "vkDestroyDebugReportCallbackEXT");
  if (f_vkDestroyDebugReportCallbackEXT && handler->g_debug_report != VK_NULL_HANDLE) {
    f_vkDestroyDebugReportCallbackEXT(handler->g_instance, handler->g_debug_report, handler->g_allocator);
    handler->g_debug_report = VK_NULL_HANDLE;
  }
  PFN_vkDestroyDebugUtilsMessengerEXT f_vkDestroyDebugUtilsMessengerEXT =
      (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(handler->g_instance, "vkDestroyDebugUtilsMessengerEXT");
  if (f_vkDestroyDebugUtilsMessengerEXT && handler->g_debug_messenger != VK_NULL_HANDLE) {
    f_vkDestroyDebugUtilsMessengerEXT(handler->g_instance, handler->g_debug_messenger, handler->g_allocator);
    handler->g_debug_messenger = VK_NULL_HANDLE;
  }
#endif
  if (handler->g_device != VK_NULL_HANDLE) {
    vkDestroyDevice(handler->g_device, handler->g_allocator);
    handler->g_device = VK_NULL_HANDLE;
  }
  if (handler->g_instance != VK_NULL_HANDLE) {
    vkDestroyInstance(handler->g_instance, handler->g_allocator);
    handler->g_instance = VK_NULL_HANDLE;
  }
}

static void cleanup_vulkan_window(gfx_handler_t *handler) {
  ImGui_ImplVulkanH_DestroyWindow(handler->g_instance, handler->g_device, &handler->g_main_window_data, handler->g_allocator);
}

// offscreen resource helpers
static int init_offscreen_resources(gfx_handler_t *handler, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return 1;
  }

  // destroy previous if any
  destroy_offscreen_resources(handler);

  handler->offscreen_width = width;
  handler->offscreen_height = height;

  // Match swapchain format to keep pipelines compatible
  VkFormat format = handler->g_main_window_data.SurfaceFormat.format;

  // create image (color attachment + sampled)
  create_image(handler, width, height, 1, 1, format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &handler->offscreen_image, &handler->offscreen_memory);

  // create image view
  handler->offscreen_image_view = create_image_view(handler, handler->offscreen_image, format, VK_IMAGE_VIEW_TYPE_2D, 1, 1);

  // create sampler
  handler->offscreen_sampler = create_texture_sampler(handler, 1, VK_FILTER_LINEAR);

  // Create a render pass for the offscreen image. Final layout will be SHADER_READ_ONLY_OPTIMAL
  VkAttachmentDescription color_attachment = {
      .flags = 0,
      .format = format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  VkAttachmentReference color_attachment_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment_ref,
  };

  VkSubpassDependency dependency = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dependencyFlags = 0,
  };

  VkRenderPassCreateInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color_attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dependency,
  };

  VkResult err = vkCreateRenderPass(handler->g_device, &rp_info, handler->g_allocator, &handler->offscreen_render_pass);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Failed to create offscreen render pass (%d)\n", err);
    return 1;
  }

  // Create framebuffer
  VkImageView attachments[1];
  attachments[0] = handler->offscreen_image_view;

  VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = handler->offscreen_render_pass,
      .attachmentCount = 1,
      .pAttachments = attachments,
      .width = width,
      .height = height,
      .layers = 1,
  };

  err = vkCreateFramebuffer(handler->g_device, &fb_info, handler->g_allocator, &handler->offscreen_framebuffer);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Failed to create offscreen framebuffer (%d)\n", err);
    vkDestroyRenderPass(handler->g_device, handler->offscreen_render_pass, handler->g_allocator);
    handler->offscreen_render_pass = VK_NULL_HANDLE;
    return 1;
  }

  // Add ImGui texture handle from sampler + image view
  // ImGui_ImplVulkan_AddTexture returns an ImTextureID (void*).

  ImTextureID id =
      (ImTextureID)ImGui_ImplVulkan_AddTexture(handler->offscreen_sampler, handler->offscreen_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  handler->offscreen_texture = ImTextureRef_ImTextureRef_TextureID(id);
  handler->offscreen_initialized = true;
  // log_info(LOG_SOURCE,"Offscreen resources created: %ux%u\n", width, height);
  return 0;
}

static void destroy_offscreen_resources(gfx_handler_t *handler) {
  if (!handler->offscreen_initialized) return;

  // Note: ImGui_ImplVulkan does not provide an explicit remove for a texture id, but the descriptor set
  // allocated by AddTexture will be freed when the descriptor pool is destroyed / ImGui shuts down.
  // To avoid leaking descriptors across re-creation, we simply destroy the Vulkan objects here.
  if (handler->offscreen_framebuffer != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(handler->g_device, handler->offscreen_framebuffer, handler->g_allocator);
    handler->offscreen_framebuffer = VK_NULL_HANDLE;
  }
  if (handler->offscreen_render_pass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(handler->g_device, handler->offscreen_render_pass, handler->g_allocator);
    handler->offscreen_render_pass = VK_NULL_HANDLE;
  }
  if (handler->offscreen_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(handler->g_device, handler->offscreen_sampler, handler->g_allocator);
    handler->offscreen_sampler = VK_NULL_HANDLE;
  }
  if (handler->offscreen_image_view != VK_NULL_HANDLE) {
    vkDestroyImageView(handler->g_device, handler->offscreen_image_view, handler->g_allocator);
    handler->offscreen_image_view = VK_NULL_HANDLE;
  }
  if (handler->offscreen_image != VK_NULL_HANDLE) {
    vkDestroyImage(handler->g_device, handler->offscreen_image, handler->g_allocator);
    handler->offscreen_image = VK_NULL_HANDLE;
  }
  if (handler->offscreen_memory != VK_NULL_HANDLE) {
    vkFreeMemory(handler->g_device, handler->offscreen_memory, handler->g_allocator);
    handler->offscreen_memory = VK_NULL_HANDLE;
  }

  ImTextureRef_destroy(handler->offscreen_texture);
  handler->offscreen_initialized = false;
  handler->offscreen_width = 0;
  handler->offscreen_height = 0;
  // log_info(LOG_SOURCE, "Offscreen resources destroyed\n");
}

static int recreate_offscreen_if_needed(gfx_handler_t *handler, uint32_t width, uint32_t height) {
  if (!handler->offscreen_initialized) return init_offscreen_resources(handler, width, height);

  if (handler->offscreen_width != width || handler->offscreen_height != height) {
    // recreate
    destroy_offscreen_resources(handler);
    return init_offscreen_resources(handler, width, height);
  }
  return 0;
}

// vulkan setup helpers
static bool is_extension_available(const VkExtensionProperties *properties, uint32_t properties_count, const char *extension) {
  for (uint32_t i = 0; i < properties_count; i++) {
    if (strcmp(properties[i].extensionName, extension) == 0) {
      return true;
    }
  }
  return false;
}

static VkResult create_instance(gfx_handler_t *handler, const char **glfw_extensions, uint32_t glfw_extensions_count) {
  VkResult err;

  uint32_t properties_count;
  vkEnumerateInstanceExtensionProperties(NULL, &properties_count, NULL);
  VkExtensionProperties *properties = malloc(sizeof(VkExtensionProperties) * properties_count);
  vkEnumerateInstanceExtensionProperties(NULL, &properties_count, properties);

  const char **extensions = malloc(sizeof(const char *) * (glfw_extensions_count + 10)); // Generous buffer
  uint32_t extensions_count = glfw_extensions_count;
  memcpy(extensions, glfw_extensions, glfw_extensions_count * sizeof(const char *));

  if (is_extension_available(properties, properties_count, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
    extensions[extensions_count++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
  if (is_extension_available(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions[extensions_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
  }
#endif
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  extensions[extensions_count++] = "VK_EXT_debug_report";
  extensions[extensions_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif

  VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .enabledExtensionCount = extensions_count,
      .ppEnabledExtensionNames = extensions,
      .pNext = NULL,
  };
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
  create_info.enabledLayerCount = 1;
  create_info.ppEnabledLayerNames = validation_layers;
  VkDebugUtilsMessengerCreateInfoEXT debug_utils_ci = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = debug_utils_callback,
  };
  create_info.pNext = &debug_utils_ci;
#else
  create_info.enabledLayerCount = 0;
  create_info.ppEnabledLayerNames = NULL;
#endif

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
  if (is_extension_available(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
#endif

  err = vkCreateInstance(&create_info, handler->g_allocator, &handler->g_instance);
  check_vk_result(err);
  free(properties);
  free(extensions);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
  PFN_vkCreateDebugReportCallbackEXT f_vkCreateDebugReportCallbackEXT =
      (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_instance, "vkCreateDebugReportCallbackEXT");
  assert(f_vkCreateDebugReportCallbackEXT != NULL);
  VkDebugReportCallbackCreateInfoEXT debug_report_ci = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
      .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT,
      .pfnCallback = debug_report,
  };
  err = f_vkCreateDebugReportCallbackEXT(handler->g_instance, &debug_report_ci, handler->g_allocator, &handler->g_debug_report);
  check_vk_result(err);

  PFN_vkCreateDebugUtilsMessengerEXT f_vkCreateDebugUtilsMessengerEXT =
      (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(handler->g_instance, "vkCreateDebugUtilsMessengerEXT");
  if (f_vkCreateDebugUtilsMessengerEXT) {
    VkDebugUtilsMessengerCreateInfoEXT messenger_ci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_utils_callback,
    };
    err = f_vkCreateDebugUtilsMessengerEXT(handler->g_instance, &messenger_ci, handler->g_allocator, &handler->g_debug_messenger);
    check_vk_result(err);
  }
#endif

  return err;
}

static void select_physical_device(gfx_handler_t *handler) {
  handler->g_physical_device = ImGui_ImplVulkanH_SelectPhysicalDevice(handler->g_instance);
  assert(handler->g_physical_device != VK_NULL_HANDLE);
  handler->g_queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(handler->g_physical_device);
  assert(handler->g_queue_family != (uint32_t)-1);
}

static void create_logical_device(gfx_handler_t *handler) {
  const char *device_extensions[] = {"VK_KHR_swapchain"};
  uint32_t device_extensions_count = 1;

  const float queue_priority[] = {1.0f};
  VkDeviceQueueCreateInfo queue_info[1] = {{
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = handler->g_queue_family,
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
  VkResult err = vkCreateDevice(handler->g_physical_device, &create_info, handler->g_allocator, &handler->g_device);
  check_vk_result(err);
  vkGetDeviceQueue(handler->g_device, handler->g_queue_family, 0, &handler->g_queue);
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
  VkResult err = vkCreateDescriptorPool(handler->g_device, &pool_info, handler->g_allocator, &handler->g_descriptor_pool);
  check_vk_result(err);
}

static void setup_window(gfx_handler_t *handler, ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int width, int height) {
  wd->Surface = surface;

  VkBool32 res;
  vkGetPhysicalDeviceSurfaceSupportKHR(handler->g_physical_device, handler->g_queue_family, wd->Surface, &res);
  if (res != VK_TRUE) {
    log_error("Vulkan", "No WSI support on the selected physical device.");
    exit(-1);
  }

  const VkFormat request_surface_image_format[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
                                                   VK_FORMAT_R8G8B8_UNORM};
  const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(handler->g_physical_device, wd->Surface, request_surface_image_format,
                                                            (size_t)ARRAYSIZE(request_surface_image_format), request_surface_color_space);

  // vsync present mode
  VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
  wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(handler->g_physical_device, wd->Surface, &present_modes[0], ARRAYSIZE(present_modes));

  assert(handler->g_min_image_count >= 2);
  ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_instance, handler->g_physical_device, handler->g_device, wd, handler->g_queue_family,
                                         handler->g_allocator, width, height, handler->g_min_image_count);
}

// frame rendering and presentation
static void frame_render(gfx_handler_t *handler, ImDrawData *draw_data) {
  ImGui_ImplVulkanH_Window *wd = &handler->g_main_window_data;
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;

  VkResult err = vkAcquireNextImageKHR(handler->g_device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_swap_chain_rebuild = true;
    return;
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames.Data[wd->FrameIndex];
  {
    err = vkWaitForFences(handler->g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    check_vk_result(err);
    err = vkResetFences(handler->g_device, 1, &fd->Fence);
    check_vk_result(err);
  }
  {
    err = vkResetCommandPool(handler->g_device, fd->CommandPool, 0);
    check_vk_result(err);
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
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

  // Immediate Mode Drawing Logic
  renderer_begin_frame(handler, fd->CommandBuffer);

  if (handler->map_shader && handler->quad_mesh && handler->map_texture_count > 0) {
    int width, height;
    glfwGetFramebufferSize(handler->window, &width, &height);
    if (width > 0 && height > 0) {
      float window_ratio = (float)width / (float)height;
      float map_ratio = (float)handler->map_data->width / (float)handler->map_data->height;
      if (isnan(map_ratio) || map_ratio == 0) map_ratio = 1.0f;

      float zoom = 1.0 / (handler->renderer.camera.zoom * fmax(handler->map_data->width, handler->map_data->height) * 0.001);
      if (isnan(zoom)) zoom = 1.0f;

      float aspect = 1.0f / (window_ratio / map_ratio);
      
      map_buffer_object_t ubo = {.transform = {handler->renderer.camera.pos[0], handler->renderer.camera.pos[1], zoom},
                                 .aspect = aspect,
                                 .lod_bias = handler->renderer.lod_bias};

      void *ubos[] = {&ubo};
      VkDeviceSize ubo_sizes[] = {sizeof(ubo)};
      renderer_draw_mesh(handler, fd->CommandBuffer, handler->quad_mesh, handler->map_shader, handler->map_textures, handler->map_texture_count, ubos,
                         ubo_sizes, 1);
    }
  }

  // Draw primitives on top
  renderer_end_frame(handler, fd->CommandBuffer);
  // End Immediate Mode Drawing

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
    err = vkQueueSubmit(handler->g_queue, 1, &info, fd->Fence);
    check_vk_result(err);
  }
}

static void frame_present(gfx_handler_t *handler) {
  if (handler->g_swap_chain_rebuild) return;
  ImGui_ImplVulkanH_Window *wd = &handler->g_main_window_data;
  VkSemaphore render_complete_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &render_complete_semaphore,
                           .swapchainCount = 1,
                           .pSwapchains = &wd->Swapchain,
                           .pImageIndices = &wd->FrameIndex};
  VkResult err = vkQueuePresentKHR(handler->g_queue, &info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    handler->g_swap_chain_rebuild = true;
  } else {
    check_vk_result(err);
  }
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}
