#include "graphics_backend.h"
#include <engine/engine_api.h>
#include "renderer.h"
#include <logger/logger.h>
#include <math.h>
#include <stdbool.h>
#include <system/config.h>
#include <system/input.h>
#include <user_interface/user_interface.h>
#include <user_interface/snippet_editor.h>
#include <user_interface/timeline/timeline_model.h>

extern bool g_is_headless;

#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(_ARR) ((int)(sizeof(_ARR) / sizeof(*(_ARR))))
#endif

static const char *LOG_SOURCE = "GfxBackend";

// forward declarations of static functions
static void glfw_error_callback(int error, const char *description);
static int init_window(gfx_handler_t *handler);
static int init_vulkan(gfx_handler_t *handler);
static int init_imgui(gfx_handler_t *handler);
static void cleanup_vulkan(gfx_handler_t *handler);
static void cleanup_vulkan_window(gfx_handler_t *handler);
// frame_render and frame_present are now folded into gfx_begin/end_frame

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

  log_error(LOG_SOURCE, "[vulkan][%s] code %d: %s", layer_prefix ? layer_prefix : "unknown", message_code, message);
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

  log_error(LOG_SOURCE, "[vulkan][%s] %s", severity_str, callback_data && callback_data->pMessage ? callback_data->pMessage : "(null)");
  fflush(stderr);
  return VK_FALSE;
}
#endif


static struct {
  GLFWwindow *window;
  double x, y;
  bool pending;
} g_imgui_mouse;

static void flush_imgui_mouse_pos(void) {
  if (!g_imgui_mouse.pending) return;
  g_imgui_mouse.pending = false;
  ImGui_ImplGlfw_CursorPosCallback((void *)g_imgui_mouse.window, g_imgui_mouse.x, g_imgui_mouse.y);
}

static void cursor_position_callback(GLFWwindow *window, double xpos, double ypos) {
  gfx_handler_t *handler = glfwGetWindowUserPointer(window);
  if (!handler) return;

  g_imgui_mouse.window = window;
  g_imgui_mouse.x = xpos;
  g_imgui_mouse.y = ypos;
  g_imgui_mouse.pending = true;

  double diff_x, diff_y;
  input_accumulate_mouse_pos(xpos, ypos, &diff_x, &diff_y);

  if (!handler->user_interface.timeline.recording || engine_input_cursor_field() < 0) return;

  float sens = handler->user_interface.mouse_sens * 0.01f;
  handler->user_interface.recording_mouse_pos[0] += diff_x * sens;
  handler->user_interface.recording_mouse_pos[1] += diff_y * sens;

  // Clamp the recorded aim to the configured reach. Plain float maths now that
  // the engine no longer borrows a game's vector type for this.
  const float max_distance = handler->user_interface.mouse_max_distance;
  const float aim_x = handler->user_interface.recording_mouse_pos[0];
  const float aim_y = handler->user_interface.recording_mouse_pos[1];
  const float length = sqrtf(aim_x * aim_x + aim_y * aim_y);
  if (length > max_distance && length > 0.0001f) {
    handler->user_interface.recording_mouse_pos[0] = aim_x / length * max_distance;
    handler->user_interface.recording_mouse_pos[1] = aim_y / length * max_distance;
  }
  // TODO: make this go in the right direction actually
  if ((int)handler->user_interface.recording_mouse_pos[0] == 0 && (int)handler->user_interface.recording_mouse_pos[1] == 0) {
    handler->user_interface.recording_mouse_pos[0] = 1;
  }
}

static void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
  input_accumulate_scroll(xoffset, yoffset);
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_ScrollCallback((void *)window, xoffset, yoffset);
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
  // Down/up state is polled; only the OS repeat timing has to come from events.
  if (action == GLFW_REPEAT) input_accumulate_key_repeat(key);
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_KeyCallback((void *)window, key, scancode, action, mods);
}

static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods) {
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_MouseButtonCallback((void *)window, button, action, mods);
}

static void char_callback(GLFWwindow *window, unsigned int c) {
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_CharCallback((void *)window, c);
}

static void cursor_enter_callback(GLFWwindow *window, int entered) {
  // Leaving the window has to arrive after the last position inside it, or imgui keeps a cursor that
  // is no longer there.
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_CursorEnterCallback((void *)window, entered);
}

static void window_focus_callback(GLFWwindow *window, int focused) {
  flush_imgui_mouse_pos();
  ImGui_ImplGlfw_WindowFocusCallback((void *)window, focused);
}

// Taken back before the imgui backend is shut down, since every one of these hands the event to a
// backend that would no longer be there. Kept in step with the set installed in init_imgui().
static void clear_glfw_callbacks(GLFWwindow *window) {
  glfwSetCursorPosCallback(window, NULL);
  glfwSetCursorEnterCallback(window, NULL);
  glfwSetMouseButtonCallback(window, NULL);
  glfwSetScrollCallback(window, NULL);
  glfwSetKeyCallback(window, NULL);
  glfwSetCharCallback(window, NULL);
  glfwSetWindowFocusCallback(window, NULL);
}

static bool imgui_queue_needs_trickling(void) {
  ImGuiContext *ctx = igGetCurrentContext();
  if (!ctx) return true;

  const ImGuiInputEvent *events = ctx->InputEventsQueue.Data;
  const int event_count = ctx->InputEventsQueue.Size;

  ImGuiKey keys_seen[32];
  int keys_seen_count = 0;
  unsigned int buttons_seen = 0;
  bool has_key = false, has_text = false;

  for (int i = 0; i < event_count; ++i) {
    const ImGuiInputEvent *e = &events[i];
    switch (e->Type) {
    case ImGuiInputEventType_MousePos:
      // A click belongs to the position the cursor was at when it happened, not to the one it
      // reached by the end of the frame, so a click with movement queued behind it has to keep its
      // own frame. Long frames are where this is worth the most and where the gap is widest.
      if (buttons_seen != 0) return true;
      break;

    case ImGuiInputEventType_MouseButton: {
      // Imgui drops events that only repeat the state already queued for that button, so a button
      // showing up twice is a press and a release, which flattening would collapse into no click.
      const unsigned int bit = 1u << e->MouseButton.Button;
      if (buttons_seen & bit) return true;
      buttons_seen |= bit;
      break;
    }

    case ImGuiInputEventType_Key:
      // Same reasoning as the mouse buttons: a key that appears twice was tapped inside one frame.
      has_key = true;
      for (int k = 0; k < keys_seen_count; ++k)
        if (keys_seen[k] == e->Key.Key) return true;
      if (keys_seen_count == ARRAYSIZE(keys_seen)) return true;
      keys_seen[keys_seen_count++] = e->Key.Key;
      break;

    case ImGuiInputEventType_Text: has_text = true; break;

    default: break;
    }
  }

  return has_key && has_text && ctx->WantTextInputNextFrame == 1;
}

// Brings up the game layer: builds the service table, finds the installed game
// modules and activates one. The engine can run with none active — it just has
// nothing to simulate or draw until a game is chosen.
static void init_game_layer(gfx_handler_t *handler) {
  const ft_engine_api *api = engine_api_init(handler);
  game_host_init(&handler->game_host, api);
  game_host_discover(&handler->game_host, "games");

  // A --game argument wins over the config, which in turn wins over "whatever
  // loaded first" so a fresh install still comes up with something usable.
  extern const char *g_forced_game_id;

  // FRAMETEE_DEFAULT_GAME_ID is a build-time setting, not engine knowledge: a
  // distribution names the game it ships as its default, and without it the
  // engine simply takes the first module in id order.
#ifndef FRAMETEE_DEFAULT_GAME_ID
#define FRAMETEE_DEFAULT_GAME_ID ""
#endif
  const char *const wanted[3] = {g_forced_game_id, handler->user_interface.preferred_game_id, FRAMETEE_DEFAULT_GAME_ID};
  for (int w = 0; w < 3; ++w) {
    if (!wanted[w] || !*wanted[w]) continue;
    if (game_host_activate_id(&handler->game_host, wanted[w])) {
      snprintf(handler->user_interface.preferred_game_id, sizeof(handler->user_interface.preferred_game_id), "%s", wanted[w]);
      keybinds_bind_game(&handler->user_interface.keybinds, &handler->game_host);
      config_load(&handler->user_interface);
      return;
    }
  }
  for (int i = 0; i < handler->game_host.count; ++i) {
    if (!handler->game_host.slots[i].usable) continue;
    if (!game_host_activate_index(&handler->game_host, i)) continue;
    snprintf(handler->user_interface.preferred_game_id, sizeof(handler->user_interface.preferred_game_id), "%s",
             handler->game_host.slots[i].id);
    keybinds_bind_game(&handler->user_interface.keybinds, &handler->game_host);
    config_load(&handler->user_interface);
    return;
  }
  log_warn(LOG_SOURCE, "No game module could be activated; the editor has nothing to simulate.");
}

int init_gfx_handler(gfx_handler_t *handler) {
  memset(handler, 0, sizeof(gfx_handler_t));
  // Unit playfield until a level is loaded, so the projection stays finite even
  // when nothing has been opened yet.
  handler->world_width = 1.f;
  handler->world_height = 1.f;

  if (g_is_headless) {
    igCreateContext(NULL);
    handler->user_interface.gfx_handler = handler;
    ui_init_config(&handler->user_interface);
    init_game_layer(handler);
    ui_init(&handler->user_interface, handler);
    return 0;
  }

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
  handler->offscreen_depth_image = VK_NULL_HANDLE;
  handler->offscreen_depth_memory = VK_NULL_HANDLE;
  handler->offscreen_depth_view = VK_NULL_HANDLE;
  handler->offscreen_texture = NULL;

  if (init_window(handler) != 0) {
    return 1;
  }

  glfwSetWindowUserPointer(handler->window, handler);
  input_init(handler->window);

  // Initialize ImGui context early for config keybind parsing
  igCreateContext(NULL);

  handler->user_interface.gfx_handler = handler;
  ui_init_config(&handler->user_interface);

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

  // Apply LOD Bias from config (renderer_init resets it)
  handler->renderer.lod_bias = handler->user_interface.lod_bias;

  if (init_imgui(handler) != 0) {
    renderer_cleanup(handler);
    cleanup_vulkan(handler);
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }


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

  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  handler->viewport[0] = (float)fb_width;
  handler->viewport[1] = (float)fb_height;

  // initialize offscreen target to match the viewport size
  if (init_offscreen_resources(handler, (uint32_t)fb_width, (uint32_t)fb_height) != 0) {
    log_warn(LOG_SOURCE, "Failed to create offscreen resources. The ImGui game view will be disabled.");
  }

  init_game_layer(handler);
  gh_resources_create(&handler->game_host);
  ui_init(&handler->user_interface, handler);

  return 0;
}

int gfx_begin_frame(gfx_handler_t *handler) {
  if (glfwWindowShouldClose(handler->window)) return FRAME_EXIT;

  glfwPollEvents();
  input_new_frame();

  if (glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
    ImGui_ImplGlfw_Sleep(10);
    return FRAME_SKIP;
  }

  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  if (fb_width > 0 && fb_height > 0 &&
      (handler->g_swap_chain_rebuild || handler->g_main_window_data.Width != fb_width || handler->g_main_window_data.Height != fb_height)) {
    vkDeviceWaitIdle(handler->g_device);

    // Update Present Mode based on settings
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (handler->user_interface.vsync) {
      present_mode = VK_PRESENT_MODE_FIFO_KHR;
    }
    VkPresentModeKHR present_modes[] = {present_mode};
    handler->g_main_window_data.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(handler->g_physical_device, handler->g_main_window_data.Surface,
                                                                                  &present_modes[0], ARRAYSIZE(present_modes));

    ImGui_ImplVulkan_SetMinImageCount(handler->g_min_image_count);
    ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_instance, handler->g_physical_device, handler->g_device, &handler->g_main_window_data,
                                           handler->g_queue_family, handler->g_allocator, fb_width, fb_height, handler->g_min_image_count, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    handler->g_main_window_data.FrameIndex = 0;
    handler->g_swap_chain_rebuild = false;

    int fb_width, fb_height;
    glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
    handler->viewport[0] = (float)fb_width;
    handler->viewport[1] = (float)fb_height;
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

  // A game rendering with its own renderer has already submitted this frame's
  // work to the shared queue, so its images become sampleable here, before the
  // pass that reads them opens. Layout transitions cannot be recorded inside a
  // render pass, which is why this sits ahead of it.
  renderer_sync_external_textures(handler, fd->CommandBuffer, true);

  // Begin offscreen render pass (for game rendering)
  if (handler->offscreen_initialized && handler->offscreen_render_pass != VK_NULL_HANDLE && handler->offscreen_framebuffer != VK_NULL_HANDLE) {
    VkClearValue clears[2] = {{.color = {.float32 = {handler->user_interface.bg_color[0], handler->user_interface.bg_color[1],
                                                     handler->user_interface.bg_color[2], 1.0f}}},
                              {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    VkRenderPassBeginInfo rp_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = handler->offscreen_render_pass,
                                     .framebuffer = handler->offscreen_framebuffer,
                                     .renderArea = {{0, 0}, {handler->offscreen_width, handler->offscreen_height}},
                                     .clearValueCount = 2,
                                     .pClearValues = clears};
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
  }

  // Start ImGui and Renderer Frames
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  // Whatever motion is still held goes out here, so imgui starts the frame on the position the
  // cursor is at now. Both of these run after the backend has queued its own events, so the queue
  // being judged is the one igNewFrame() is about to process.
  flush_imgui_mouse_pos();
  igGetIO_Nil()->ConfigInputTrickleEventQueue = imgui_queue_needs_trickling();
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

  // Hand the externally rendered images back in the layout their owner draws
  // into, so the next frame starts from the same state this one assumed.
  renderer_sync_external_textures(handler, handler->current_frame_command_buffer, false);

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

  // Retire resources only after enough complete submissions have passed for
  // every swapchain image that could reference them.
  renderer_lock();
  for (uint32_t i = 0; i < handler->retire_count;) {
    if (handler->frame_serial - handler->retire_textures[i].frame_serial >= 3) {
      if (handler->retire_textures[i].sampler) vkDestroySampler(handler->g_device, handler->retire_textures[i].sampler, handler->g_allocator);
      if (handler->retire_textures[i].image_view) vkDestroyImageView(handler->g_device, handler->retire_textures[i].image_view, handler->g_allocator);
      if (handler->retire_textures[i].image) vkDestroyImage(handler->g_device, handler->retire_textures[i].image, handler->g_allocator);
      if (handler->retire_textures[i].memory) vkFreeMemory(handler->g_device, handler->retire_textures[i].memory, handler->g_allocator);

      handler->retire_textures[i] = handler->retire_textures[--handler->retire_count];
      continue;
    }
    i++;
  }
  for (uint32_t i = 0; i < handler->retire_imgui_count;) {
    if (handler->frame_serial - handler->retire_imgui_textures[i].frame_serial >= 3) {
      ImGui_ImplVulkan_RemoveTexture(handler->retire_imgui_textures[i].descriptor_set);
      handler->retire_imgui_textures[i] = handler->retire_imgui_textures[--handler->retire_imgui_count];
      continue;
    }
    i++;
  }
  renderer_unlock();

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
  handler->frame_serial++;
  return hovered;
}

void gfx_retire_imgui_texture(gfx_handler_t *handler, uint64_t texture_id) {
  if (!handler || !texture_id || g_is_headless) return;
  renderer_lock();
  if (handler->retire_imgui_count >= 256) {
    renderer_unlock();
    vkDeviceWaitIdle(handler->g_device);
    renderer_lock();
    for (uint32_t i = 0; i < handler->retire_imgui_count; ++i)
      ImGui_ImplVulkan_RemoveTexture(handler->retire_imgui_textures[i].descriptor_set);
    handler->retire_imgui_count = 0;
  }
  const uint32_t index = handler->retire_imgui_count++;
  handler->retire_imgui_textures[index].descriptor_set = (VkDescriptorSet)texture_id;
  handler->retire_imgui_textures[index].frame_serial = handler->frame_serial;
  renderer_unlock();
}

void gfx_cleanup(gfx_handler_t *handler) {
  if (!g_is_headless) {
    VkResult err = vkDeviceWaitIdle(handler->g_device);
    check_vk_result(err);
  }

  ui_cleanup(&handler->user_interface);


  gh_level_destroy(&handler->game_host, handler->level);
  handler->level = NULL;
  game_host_shutdown(&handler->game_host);

  if (!g_is_headless) {
    for (uint32_t i = 0; i < handler->retire_count; ++i) {
      if (handler->retire_textures[i].sampler)
        vkDestroySampler(handler->g_device, handler->retire_textures[i].sampler, handler->g_allocator);
      if (handler->retire_textures[i].image_view)
        vkDestroyImageView(handler->g_device, handler->retire_textures[i].image_view, handler->g_allocator);
      if (handler->retire_textures[i].image)
        vkDestroyImage(handler->g_device, handler->retire_textures[i].image, handler->g_allocator);
      if (handler->retire_textures[i].memory)
        vkFreeMemory(handler->g_device, handler->retire_textures[i].memory, handler->g_allocator);
    }
    handler->retire_count = 0;
    for (uint32_t i = 0; i < handler->retire_imgui_count; ++i)
      ImGui_ImplVulkan_RemoveTexture(handler->retire_imgui_textures[i].descriptor_set);
    handler->retire_imgui_count = 0;
    renderer_cleanup(handler);
    destroy_offscreen_resources(handler);
    clear_glfw_callbacks(handler->window);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
  }

  igDestroyContext(NULL);

  if (!g_is_headless) {
    cleanup_vulkan_window(handler);
    cleanup_vulkan(handler);
    if (handler->window) glfwDestroyWindow(handler->window);
    handler->window = NULL;
    glfwTerminate();
  }
}

// Publishes a freshly loaded level to the parts of the engine that need to know
// how big it is, then lets the timeline rebuild its worlds on top of it.
static void on_level_loaded(gfx_handler_t *handler) {
  handler->renderer.camera.pos[0] = 0.5f;
  handler->renderer.camera.pos[1] = 0.5f;

  ft_level_info info;
  if (gh_level_info(&handler->game_host, handler->level, &info) && info.bounds.w > 0.f && info.bounds.h > 0.f) {
    engine_api_set_world_extent(handler, info.bounds.w, info.bounds.h);
    // A 3D game's bounds describe its ground plane, so the orbit camera frames
    // that plane: centred on it, backed off far enough to see all of it, and
    // looking slightly down. Without this the orbit sits at the origin and the
    // level is off to one side.
    if (game_is_3d(&handler->game_host)) {
      camera3_t *c = &handler->renderer.camera3;
      const float span = fmaxf(info.bounds.w, info.bounds.h);
      c->target[0] = info.bounds.x + info.bounds.w * 0.5f;
      c->target[1] = span * 0.2f;
      c->target[2] = info.bounds.y + info.bounds.h * 0.5f;
      c->distance = span * 1.6f;
      c->far_z = fmaxf(c->far_z, span * 20.f);
      // Crossing the level in a couple of seconds is a reasonable starting
      // point; the wheel trims it from there while flying.
      c->move_speed = span * 0.5f;
      c->free_mode = false;
    }
    snprintf(handler->user_interface.loaded_level_name, sizeof(handler->user_interface.loaded_level_name), "%s", info.name ? info.name : "level");
  } else {
    engine_api_set_world_extent(handler, 1.f, 1.f);
  }

  model_reset_groups_for_level(&handler->user_interface.timeline);
}

bool gfx_activate_game(gfx_handler_t *handler, int game_index) {
  if (!handler || game_index < 0 || game_index >= handler->game_host.count) return false;
  if (game_index == handler->game_host.active) return true;

  ui_handler_t *ui = &handler->user_interface;

  // Worlds and levels are opaque module-owned objects. They must be destroyed
  // while the old module is still active; otherwise the new game would receive
  // foreign pointers on its first render or simulation callback.
  snippet_editor_reset();
  undo_manager_cleanup(&ui->undo_manager);
  undo_manager_init(&ui->undo_manager);
  timeline_cleanup(&ui->timeline);
  gh_level_destroy(&handler->game_host, handler->level);
  handler->level = NULL;

  if (!game_host_activate_index(&handler->game_host, game_index)) {
    timeline_init(ui);
    return false;
  }
  if (!gh_resources_create(&handler->game_host)) {
    log_error(LOG_SOURCE, "The selected game could not create its rendering resources.");
    game_host_deactivate(&handler->game_host);
    timeline_init(ui);
    return false;
  }

  timeline_init(ui);
  camera_init(&handler->renderer.camera);
  keybinds_bind_game(&ui->keybinds, &handler->game_host);
  config_load(ui);
  entity_inspector_clear(&ui->entity_inspector);
  engine_api_set_world_extent(handler, 1.f, 1.f);

  snprintf(ui->loaded_level_name, sizeof(ui->loaded_level_name), "%s", "unnamed_level");
  ui->loaded_level_path[0] = '\0';
  ui->current_project_path[0] = '\0';
  ui->has_unsaved_changes = false;
  plugin_manager_on_game_changed(&ui->plugin_manager);
  return true;
}

void on_level_load_path(gfx_handler_t *handler, const char *level_path) {
  snprintf(handler->user_interface.loaded_level_path, sizeof(handler->user_interface.loaded_level_path), "%s", level_path);
  timeline_cleanup(&handler->user_interface.timeline);
  timeline_init(&handler->user_interface);

  gh_level_destroy(&handler->game_host, handler->level);
  handler->level = gh_level_load_path(&handler->game_host, level_path);
  if (!handler->level) {
    log_error(LOG_SOURCE, "The active game could not load '%s'", level_path);
    return;
  }
  log_info(LOG_SOURCE, "Loaded level '%s'", level_path);

  on_level_loaded(handler);
  ui_post_level_load(&handler->user_interface);
}

void on_level_load_memory(struct gfx_handler_t *handler, const unsigned char *level_buffer, size_t size) {
  handler->user_interface.loaded_level_path[0] = '\0';
  gh_level_destroy(&handler->game_host, handler->level);
  handler->level = gh_level_load_memory(&handler->game_host, level_buffer, size);
  if (!handler->level) {
    log_error(LOG_SOURCE, "The active game could not load the level stored in this project");
    return;
  }
  on_level_loaded(handler);
  ui_post_level_load(&handler->user_interface);
}

// initialization and cleanup
static int init_window(gfx_handler_t *handler) {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) return 1;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

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

/*
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
  ImVec4 accent_yellow = hex_vec4("D61430", 1.0f);
  ImVec4 accent_orange = hex_vec4("FF8F40", 1.0f);
  ImVec4 accent_green = hex_vec4("AAD94C", 1.0f);
  ImVec4 accent_blue = hex_vec4("39BAE6", 1.0f);

  // Style Metrics
  style->WindowPadding = (ImVec2){8, 8};
  style->FramePadding = (ImVec2){6, 4};
  style->ItemSpacing = (ImVec2){8, 4};
  style->ScrollbarSize = 14;
  style->GrabMinSize = 20;

  style->WindowRounding = 10;
  style->FrameRounding = 10;
  style->TabRounding = 10;
  style->ScrollbarRounding = 6;

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
*/

float gfx_get_ui_scale(void) {
  extern bool g_is_headless;
  if (g_is_headless) return 1.0f;

  static float dpi = -1.f;
  if (dpi != -1.f)
    return dpi;
#define UI_DIV 5.0
  GLFWmonitor *mon = glfwGetPrimaryMonitor();
  // There is no primary monitor when GLFW enumerates none — a headless X
  // server, a stripped build without RandR. Scaling is guesswork then, so take
  // 1.0 rather than dereferencing NULL inside GLFW.
  if (!mon) {
    dpi = 1.f;
    return dpi;
  }
  int width_mm, height_mm;
  glfwGetMonitorPhysicalSize(mon, &width_mm, &height_mm);
  float dia_inch = sqrtf(width_mm * width_mm + height_mm * height_mm) * UI_DIV;
  int p0, p1;
  int width_px, height_px;
  glfwGetMonitorWorkarea(mon, &p0, &p1, &width_px, &height_px);
  float dia_px = sqrtf(width_px * width_px + height_px * height_px);
  dpi = dia_px / dia_inch;
  return dpi;
}

static int init_imgui(gfx_handler_t *handler) {
  ImGuiIO *io = igGetIO_Nil();
  io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // io->ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io->ConfigDpiScaleFonts = true;
  io->ConfigDpiScaleViewports = true;
  io->ConfigWindowsMoveFromTitleBarOnly = true;
  // Set per frame in gfx_begin_frame(); see imgui_queue_needs_trickling().
  io->ConfigInputTrickleEventQueue = true;

  ImGuiStyle *style = igGetStyle();
  ImGuiStyle_ScaleAllSizes(style, gfx_get_ui_scale() * 0.5);

  // The backend installs no callbacks of its own: every GLFW event goes to the callbacks above,
  // which pass it on to the backend once they have put the mouse position in front of it. That keeps
  // one path from the window to imgui, and the whole set has to be installed here, since anything
  // left out is an event imgui stops receiving.
  ImGui_ImplGlfw_InitForVulkan((void *)handler->window, false);
  glfwSetCursorPosCallback(handler->window, cursor_position_callback);
  glfwSetCursorEnterCallback(handler->window, cursor_enter_callback);
  glfwSetMouseButtonCallback(handler->window, mouse_button_callback);
  glfwSetScrollCallback(handler->window, scroll_callback);
  glfwSetKeyCallback(handler->window, key_callback);
  glfwSetCharCallback(handler->window, char_callback);
  glfwSetWindowFocusCallback(handler->window, window_focus_callback);
  ImGui_ImplVulkan_InitInfo init_info = {.Instance = handler->g_instance,
                                         .PhysicalDevice = handler->g_physical_device,
                                         .Device = handler->g_device,
                                         .QueueFamily = handler->g_queue_family,
                                         .Queue = handler->g_queue,
                                         .PipelineCache = handler->g_pipeline_cache,
                                         .DescriptorPool = handler->g_descriptor_pool,
                                         .PipelineInfoMain = {
                                             .RenderPass = handler->g_main_window_data.RenderPass,
                                             .Subpass = 0,
                                             .MSAASamples = VK_SAMPLE_COUNT_1_BIT},
                                         .MinImageCount = handler->g_min_image_count,
                                         .ImageCount = handler->g_main_window_data.ImageCount,
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

  handler->offscreen_depth_format = VK_FORMAT_D32_SFLOAT;
  VkAttachmentDescription depth_attachment = {
      .format = handler->offscreen_depth_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };
  VkAttachmentReference depth_attachment_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment_ref,
      .pDepthStencilAttachment = &depth_attachment_ref,
  };

  VkSubpassDependency dependency = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dependencyFlags = 0,
  };

  const VkAttachmentDescription offscreen_attachments[2] = {color_attachment, depth_attachment};
  VkRenderPassCreateInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = offscreen_attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dependency,
  };

  VkResult err = vkCreateRenderPass(handler->g_device, &rp_info, handler->g_allocator, &handler->offscreen_render_pass);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Failed to create offscreen render pass (%d)", err);
    return 1;
  }

  // Depth image for that attachment, matching the colour target's extent.
  create_image(handler, handler->offscreen_width, handler->offscreen_height, 1, 1, handler->offscreen_depth_format,
               VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
               &handler->offscreen_depth_image, &handler->offscreen_depth_memory);
  handler->offscreen_depth_view =
      create_image_view_aspect(handler, handler->offscreen_depth_image, handler->offscreen_depth_format, VK_IMAGE_VIEW_TYPE_2D, 1, 1,
                               VK_IMAGE_ASPECT_DEPTH_BIT);

  // Create framebuffer
  VkImageView attachments[2];
  attachments[0] = handler->offscreen_image_view;
  attachments[1] = handler->offscreen_depth_view;

  VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = handler->offscreen_render_pass,
      .attachmentCount = 2,
      .pAttachments = attachments,
      .width = width,
      .height = height,
      .layers = 1,
  };

  err = vkCreateFramebuffer(handler->g_device, &fb_info, handler->g_allocator, &handler->offscreen_framebuffer);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Failed to create offscreen framebuffer (%d)", err);
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
  // log_info(LOG_SOURCE,"Offscreen resources created: %ux%u", width, height);
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
  if (handler->offscreen_depth_view != VK_NULL_HANDLE) {
    vkDestroyImageView(handler->g_device, handler->offscreen_depth_view, handler->g_allocator);
    handler->offscreen_depth_view = VK_NULL_HANDLE;
  }
  if (handler->offscreen_depth_image != VK_NULL_HANDLE) {
    vkDestroyImage(handler->g_device, handler->offscreen_depth_image, handler->g_allocator);
    handler->offscreen_depth_image = VK_NULL_HANDLE;
  }
  if (handler->offscreen_depth_memory != VK_NULL_HANDLE) {
    vkFreeMemory(handler->g_device, handler->offscreen_depth_memory, handler->g_allocator);
    handler->offscreen_depth_memory = VK_NULL_HANDLE;
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

  destroy_imgui_texture_ref(&handler->offscreen_texture);
  handler->offscreen_initialized = false;
  handler->offscreen_width = 0;
  handler->offscreen_height = 0;
  // log_info(LOG_SOURCE, "Offscreen resources destroyed");
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
  // Anisotropic filtering, when the device has it. A ground plane seen at a
  // grazing angle — which is most of what a driving game shows — collapses to a
  // flat average without it, however much detail the texture has.
  VkPhysicalDeviceFeatures supported = {0};
  vkGetPhysicalDeviceFeatures(handler->g_physical_device, &supported);
  VkPhysicalDeviceFeatures enabled = {0};
  enabled.samplerAnisotropy = supported.samplerAnisotropy;

  VkDeviceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = ARRAYSIZE(queue_info),
      .pQueueCreateInfos = queue_info,
      .enabledExtensionCount = device_extensions_count,
      .ppEnabledExtensionNames = device_extensions,
      .pEnabledFeatures = &enabled,
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
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  if (handler->user_interface.vsync) {
    present_mode = VK_PRESENT_MODE_FIFO_KHR;
  }
  VkPresentModeKHR present_modes[] = {present_mode, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  VkPresentModeKHR selected_mode = ImGui_ImplVulkanH_SelectPresentMode(handler->g_physical_device, wd->Surface, present_modes, ARRAYSIZE(present_modes));
  if ((int)selected_mode == -1) {
    selected_mode = VK_PRESENT_MODE_FIFO_KHR;
  }
  wd->PresentMode = selected_mode;

  assert(handler->g_min_image_count >= 2);
  ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_instance, handler->g_physical_device, handler->g_device, wd, handler->g_queue_family,
                                         handler->g_allocator, width, height, handler->g_min_image_count, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}

// frame rendering and presentation
/*
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
*/

void gfx_toggle_fullscreen(gfx_handler_t *handler) {
  GLFWmonitor *monitor = glfwGetWindowMonitor(handler->window);
  if (monitor) {
    // Switch to windowed mode
    glfwSetWindowMonitor(handler->window, NULL, 100, 100, 1280, 720, 0);
  } else {
    // Switch to fullscreen mode
    monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);
    glfwSetWindowMonitor(handler->window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
  }
}

// Mock GLFW monitor functions to avoid excessive X11 polling in ImGui_ImplGlfw_UpdateMonitors()
typedef struct {
  GLFWmonitor *monitor;
  int x, y;
  GLFWvidmode vid_mode;
  int work_x, work_y, work_w, work_h;
  float scale_x, scale_y;
} cached_monitor_info_t;

static GLFWmonitor **g_cached_monitors = NULL;
static int g_cached_monitors_count = 0;
static cached_monitor_info_t *g_cached_infos = NULL;
static int g_cached_infos_count = 0;
static int g_last_update_frame = -1000;

static void update_cached_monitors(void) {
  int count = 0;
  GLFWmonitor **monitors = glfwGetMonitors(&count);
  if (count <= 0) return;

  g_cached_monitors = monitors;
  g_cached_monitors_count = count;

  cached_monitor_info_t *new_infos = realloc(g_cached_infos, count * sizeof(cached_monitor_info_t));
  if (new_infos) {
    g_cached_infos = new_infos;
    g_cached_infos_count = count;
  }

  for (int i = 0; i < count; i++) {
    g_cached_infos[i].monitor = monitors[i];
    glfwGetMonitorPos(monitors[i], &g_cached_infos[i].x, &g_cached_infos[i].y);

    const GLFWvidmode *vm = glfwGetVideoMode(monitors[i]);
    if (vm) {
      g_cached_infos[i].vid_mode = *vm;
    } else {
      memset(&g_cached_infos[i].vid_mode, 0, sizeof(GLFWvidmode));
    }

    glfwGetMonitorWorkarea(monitors[i], &g_cached_infos[i].work_x, &g_cached_infos[i].work_y, &g_cached_infos[i].work_w, &g_cached_infos[i].work_h);
    glfwGetMonitorContentScale(monitors[i], &g_cached_infos[i].scale_x, &g_cached_infos[i].scale_y);
  }
}

static void check_monitor_cache(void) {
  int current_frame = igGetFrameCount();
  if (!g_cached_monitors || (current_frame - g_last_update_frame) >= 65536 || (current_frame < g_last_update_frame)) {
    update_cached_monitors();
    g_last_update_frame = current_frame;
  }
}

GLFWmonitor **mock_glfwGetMonitors(int *count) {
  check_monitor_cache();
  if (count) *count = g_cached_monitors_count;
  return g_cached_monitors;
}

void mock_glfwGetMonitorPos(GLFWmonitor *monitor, int *xpos, int *ypos) {
  check_monitor_cache();
  for (int i = 0; i < g_cached_infos_count; i++) {
    if (g_cached_infos[i].monitor == monitor) {
      if (xpos) *xpos = g_cached_infos[i].x;
      if (ypos) *ypos = g_cached_infos[i].y;
      return;
    }
  }
  glfwGetMonitorPos(monitor, xpos, ypos);
}

const GLFWvidmode *mock_glfwGetVideoMode(GLFWmonitor *monitor) {
  check_monitor_cache();
  for (int i = 0; i < g_cached_infos_count; i++) {
    if (g_cached_infos[i].monitor == monitor) {
      return &g_cached_infos[i].vid_mode;
    }
  }
  return glfwGetVideoMode(monitor);
}

void mock_glfwGetMonitorWorkarea(GLFWmonitor *monitor, int *xpos, int *ypos, int *width, int *height) {
  check_monitor_cache();
  for (int i = 0; i < g_cached_infos_count; i++) {
    if (g_cached_infos[i].monitor == monitor) {
      if (xpos) *xpos = g_cached_infos[i].work_x;
      if (ypos) *ypos = g_cached_infos[i].work_y;
      if (width) *width = g_cached_infos[i].work_w;
      if (height) *height = g_cached_infos[i].work_h;
      return;
    }
  }
  glfwGetMonitorWorkarea(monitor, xpos, ypos, width, height);
}

void mock_glfwGetMonitorContentScale(GLFWmonitor *monitor, float *xscale, float *yscale) {
  check_monitor_cache();
  for (int i = 0; i < g_cached_infos_count; i++) {
    if (g_cached_infos[i].monitor == monitor) {
      if (xscale) *xscale = g_cached_infos[i].scale_x;
      if (yscale) *yscale = g_cached_infos[i].scale_y;
      return;
    }
  }
  glfwGetMonitorContentScale(monitor, xscale, yscale);
}
