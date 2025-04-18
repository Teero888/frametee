#include "graphics_backend.h"
#include "cimgui.h"
#include "renderer.h"
#include "user_interface.h"
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define ARRAYSIZE(_ARR) ((int)(sizeof(_ARR) / sizeof(*(_ARR))))

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
static bool is_extension_available(VkExtensionProperties *properties, uint32_t properties_count,
                                   const char *extension) {
  for (uint32_t i = 0; i < properties_count; i++) {
    if (strcmp(properties[i].extensionName, extension) == 0)
      return true;
  }
  return false;
}

static void setup_vulkan(gfx_handler_t *handler, const char ***extensions, uint32_t *extensions_count) {
  VkResult err;

  // Create Vulkan Instance
  {
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    // Enumerate available extensions
    uint32_t properties_count;
    vkEnumerateInstanceExtensionProperties(NULL, &properties_count, NULL);
    VkExtensionProperties *properties =
        (VkExtensionProperties *)malloc(properties_count * sizeof(VkExtensionProperties));
    if (!properties) {
      fprintf(stderr, "Failed to allocate memory for extension properties\n");
      abort();
    }
    err = vkEnumerateInstanceExtensionProperties(NULL, &properties_count, properties);
    check_vk_result_line(err, __LINE__);

    // Enable required extensions
    uint32_t new_extensions_count = *extensions_count;
    const char **new_extensions = *extensions;

    if (is_extension_available(properties, properties_count,
                               VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
      new_extensions_count++;
      new_extensions = realloc(new_extensions, new_extensions_count * sizeof(char *));
      if (!new_extensions) {
        free(properties);
        fprintf(stderr, "Failed to reallocate extensions array\n");
        abort();
      }
      new_extensions[new_extensions_count - 1] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (is_extension_available(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      new_extensions_count++;
      new_extensions = realloc(new_extensions, new_extensions_count * sizeof(char *));
      if (!new_extensions) {
        free(properties);
        fprintf(stderr, "Failed to reallocate extensions array\n");
        abort();
      }
      new_extensions[new_extensions_count - 1] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
      create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = layers;
    new_extensions_count++;
    new_extensions = realloc(new_extensions, new_extensions_count * sizeof(char *));
    if (!new_extensions) {
      free(properties);
      fprintf(stderr, "Failed to reallocate extensions array\n");
      abort();
    }
    new_extensions[new_extensions_count - 1] = "VK_EXT_debug_report";
#endif

    free(properties);

    // Update extensions array and count
    *extensions = new_extensions;
    *extensions_count = new_extensions_count;

    // Create Vulkan Instance
    create_info.enabledExtensionCount = *extensions_count;
    create_info.ppEnabledExtensionNames = *extensions;
    err = vkCreateInstance(&create_info, handler->g_Allocator, &handler->g_Instance);
    check_vk_result_line(err, __LINE__);

    // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    PFN_vkCreateDebugReportCallbackEXT f_vkCreateDebugReportCallbackEXT =
        (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_Instance,
                                                                  "vkCreateDebugReportCallbackEXT");
    assert(f_vkCreateDebugReportCallbackEXT != NULL);
    VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
    debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                            VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
    debug_report_ci.pfnCallback = debug_report;
    debug_report_ci.pUserData = NULL;
    err = f_vkCreateDebugReportCallbackEXT(handler->g_Instance, &debug_report_ci, handler->g_Allocator,
                                           &handler->g_DebugReport);
    check_vk_result_line(err, __LINE__);
#endif
  }
  // Rest of the function remains unchanged
  // Select Physical Device (GPU)
  handler->g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(handler->g_Instance);
  assert(handler->g_PhysicalDevice != VK_NULL_HANDLE);
  // Select graphics queue family
  handler->g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(handler->g_PhysicalDevice);
  assert(handler->g_QueueFamily != (uint32_t)-1);

  // Create Logical Device (with 1 queue)
  {
    uint32_t device_extensions_count = 1;
    const char **device_extensions = (const char **)malloc(device_extensions_count * sizeof(char *));
    device_extensions[0] = "VK_KHR_swapchain";

    // Enumerate physical device extension
    uint32_t properties_count;
    vkEnumerateDeviceExtensionProperties(handler->g_PhysicalDevice, NULL, &properties_count, NULL);
    VkExtensionProperties *properties =
        (VkExtensionProperties *)malloc(properties_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(handler->g_PhysicalDevice, NULL, &properties_count, properties);
    free(properties);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (IsExtensionAvailable(properties, properties_count, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
      // device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
      device_extensions_count++;
      device_extensions = realloc(device_extensions, device_extensions_count * sizeof(const char *));
      assert(device_extensions);
      device_extensions[device_extensions_count - 1] = VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
    }
#endif

    const float queue_priority[] = {1.0f};
    VkDeviceQueueCreateInfo queue_info[1] = {};
    queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info[0].queueFamilyIndex = handler->g_QueueFamily;
    queue_info[0].queueCount = 1;
    queue_info[0].pQueuePriorities = queue_priority;
    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
    create_info.pQueueCreateInfos = queue_info;
    create_info.enabledExtensionCount = device_extensions_count; //(uint32_t)device_extensions.Size;
    create_info.ppEnabledExtensionNames = device_extensions;     // device_extensions.Data;
    err = vkCreateDevice(handler->g_PhysicalDevice, &create_info, handler->g_Allocator, &handler->g_Device);
    check_vk_result_line(err, __LINE__);
    vkGetDeviceQueue(handler->g_Device, handler->g_QueueFamily, 0, &handler->g_Queue);
    free(device_extensions);
  }
  // Create Descriptor Pool
  // If you wish to load e.g. additional textures you may need to alter pools sizes and maxSets.
  {
#define IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE (1) // Minimum per atlas
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 0;
    for (int i = 0; i < ARRAYSIZE(pool_sizes); i++) {
      VkDescriptorPoolSize pool_size = pool_sizes[i];
      pool_info.maxSets += pool_size.descriptorCount;
    }
    pool_info.poolSizeCount = (uint32_t)ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    err = vkCreateDescriptorPool(handler->g_Device, &pool_info, handler->g_Allocator,
                                 &handler->g_DescriptorPool);
    check_vk_result_line(err, __LINE__);
  }
}
static void setup_window(gfx_handler_t *handler, ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface,
                         int width, int height) {
  wd->Surface = surface;

  // check for wsi support
  VkBool32 res;
  vkGetPhysicalDeviceSurfaceSupportKHR(handler->g_PhysicalDevice, handler->g_QueueFamily, wd->Surface, &res);
  if (res != VK_TRUE) {
    fprintf(stderr, "Error no WSI support on physical device 0\n");
    exit(-1);
  }

  // select surface format
  const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
  const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
      handler->g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
      (size_t)ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

  VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
  wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(handler->g_PhysicalDevice, wd->Surface,
                                                        &present_modes[0], ARRAYSIZE(present_modes));
  // create swapchain, renderpass, framebuffer, etc.
  assert(handler->g_MinImageCount >= 2);
  ImGui_ImplVulkanH_CreateOrResizeWindow(handler->g_Instance, handler->g_PhysicalDevice, handler->g_Device,
                                         wd, handler->g_QueueFamily, handler->g_Allocator, width, height,
                                         handler->g_MinImageCount);
}

static void cleanup_vulkan_window(gfx_handler_t *handler) {
  ImGui_ImplVulkanH_DestroyWindow(handler->g_Instance, handler->g_Device, &handler->g_MainWindowData,
                                  handler->g_Allocator);
}

static void cleanup_vulkan(gfx_handler_t *handler) {
  vkDestroyDescriptorPool(handler->g_Device, handler->g_DescriptorPool, handler->g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
  // Remove the debug report callback
  PFN_vkDestroyDebugReportCallbackEXT f_vkDestroyDebugReportCallbackEXT =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(handler->g_Instance,
                                                                 "vkDestroyDebugReportCallbackEXT");
  if (f_vkDestroyDebugReportCallbackEXT && handler->g_DebugReport != VK_NULL_HANDLE) {
    f_vkDestroyDebugReportCallbackEXT(handler->g_Instance, handler->g_DebugReport, handler->g_Allocator);
    handler->g_DebugReport = VK_NULL_HANDLE;
  }
#endif

  vkDestroyDevice(handler->g_Device, handler->g_Allocator);
  handler->g_Device = VK_NULL_HANDLE;
  vkDestroyInstance(handler->g_Instance, handler->g_Allocator);
  handler->g_Instance = VK_NULL_HANDLE;
}

static void frame_render(gfx_handler_t *handler, ImDrawData *draw_data) {
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;
  VkSemaphore image_acquired_semaphore = wd->FrameSemaphores.Data[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkResult err = vkAcquireNextImageKHR(handler->g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore,
                                       VK_NULL_HANDLE, &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    handler->g_SwapChainRebuild = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR)
    return;
  if (err != VK_SUBOPTIMAL_KHR)
    check_vk_result_line(err, __LINE__);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames.Data[wd->FrameIndex];
  {
    err = vkWaitForFences(handler->g_Device, 1, &fd->Fence, VK_TRUE,
                          UINT64_MAX); // wait indefinitely instead of periodically checking
    check_vk_result_line(err, __LINE__);

    err = vkResetFences(handler->g_Device, 1, &fd->Fence);
    check_vk_result_line(err, __LINE__);
  }
  {
    err = vkResetCommandPool(handler->g_Device, fd->CommandPool, 0);
    check_vk_result_line(err, __LINE__);
    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
    check_vk_result_line(err, __LINE__);
  }
  {
    VkRenderPassBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = wd->RenderPass;
    info.framebuffer = fd->Framebuffer;
    info.renderArea.extent.width = wd->Width;
    info.renderArea.extent.height = wd->Height;
    info.clearValueCount = 1;
    info.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
  }
  renderer_draw(handler, fd->CommandBuffer);
  // Record dear imgui primitives into command buffer
  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer, VK_NULL_HANDLE);

  // Submit command buffer
  vkCmdEndRenderPass(fd->CommandBuffer);
  {
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &image_acquired_semaphore;
    info.pWaitDstStageMask = &wait_stage;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &fd->CommandBuffer;
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &render_complete_semaphore;

    err = vkEndCommandBuffer(fd->CommandBuffer);
    check_vk_result_line(err, __LINE__);
    err = vkQueueSubmit(handler->g_Queue, 1, &info, fd->Fence);
    check_vk_result_line(err, __LINE__);
  }
}

static void frame_present(gfx_handler_t *handler) {
  if (handler->g_SwapChainRebuild)
    return;
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores.Data[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR info = {};
  info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &render_complete_semaphore;
  info.swapchainCount = 1;
  info.pSwapchains = &wd->Swapchain;
  info.pImageIndices = &wd->FrameIndex;
  VkResult err = vkQueuePresentKHR(handler->g_Queue, &info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    handler->g_SwapChainRebuild = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR)
    return;
  if (err != VK_SUBOPTIMAL_KHR)
    check_vk_result_line(err, __LINE__);
  wd->SemaphoreIndex =
      (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
}

int init_gfx_handler(gfx_handler_t *handler) {
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

  handler->g_MainWindowData.ClearValue.color.float32[0] = 0.3f;
  handler->g_MainWindowData.ClearValue.color.float32[1] = 0.3f;
  handler->g_MainWindowData.ClearValue.color.float32[2] = 0.3f;
  handler->g_MainWindowData.ClearValue.color.float32[3] = 1.0f;

  ui_init(&handler->user_interface);

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // create window with vulkan context
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  handler->window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+Vulkan example", NULL, NULL);
  if (!glfwVulkanSupported()) {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // setup vulkan
  uint32_t extensions_count = 0;
  const char *const *extensions_nude = glfwGetRequiredInstanceExtensions(&extensions_count);
  if (extensions_nude == NULL) {
    printf("Error: glfw did not return any extensions\n");
    return -1;
  }
  const char **extensions = (const char **)malloc(extensions_count * sizeof(const char *));
  if (extensions == NULL) {
    printf("Error allocating space for extensions array\n");
    return -1;
  }
  for (int i = 0; i < extensions_count; i++) {
    extensions[i] = extensions_nude[i];
  }
  setup_vulkan(handler, &extensions, &extensions_count);
  free(extensions);

  // create window surface
  VkSurfaceKHR surface;
  VkResult err =
      glfwCreateWindowSurface(handler->g_Instance, handler->window, handler->g_Allocator, &surface);
  check_vk_result_line(err, __LINE__);

  // create framebuffers
  int w, h;
  glfwGetFramebufferSize(handler->window, &w, &h);
  memset((void *)&handler->g_MainWindowData, 0, sizeof(handler->g_MainWindowData));
  ImGui_ImplVulkanH_Window *wd = &handler->g_MainWindowData;
  wd->PresentMode = (VkPresentModeKHR)~0;
  wd->ClearEnable = true;

  setup_window(handler, wd, surface, w, h);

  if (renderer_init(handler) != 0) {
    fprintf(stderr, "Failed to initialize renderer\n");
    // Perform partial cleanup if necessary before returning
    cleanup_vulkan_window(handler);
    cleanup_vulkan(handler);
    glfwDestroyWindow(handler->window);
    glfwTerminate();
    return 1;
  }

  // setup dear imgui context
  igCreateContext(NULL);
  ImGuiIO *io = igGetIO_Nil();
  (void)io;
  io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
  io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking

  // Setup Dear ImGui style
  igStyleColorsDark(NULL);

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForVulkan(handler->window, true);
  ImGui_ImplVulkan_InitInfo init_info = {};
  init_info.ApiVersion = VK_API_VERSION_1_3;
  init_info.Instance = handler->g_Instance;
  init_info.PhysicalDevice = handler->g_PhysicalDevice;
  init_info.Device = handler->g_Device;
  init_info.QueueFamily = handler->g_QueueFamily;
  init_info.Queue = handler->g_Queue;
  init_info.PipelineCache = handler->g_PipelineCache;
  init_info.DescriptorPool = handler->g_DescriptorPool;
  init_info.RenderPass = wd->RenderPass;
  init_info.Subpass = 0;
  init_info.MinImageCount = handler->g_MinImageCount;
  init_info.ImageCount = wd->ImageCount;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.Allocator = handler->g_Allocator;
  init_info.CheckVkResultFn = check_vk_result;
  ImGui_ImplVulkan_Init(&init_info);

  // Load Fonts
  // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use
  // igPushFont()/PopFont() to select them.
  // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among
  // multiple.
  // - If the file cannot be loaded, the function will return a NULL. Please handle those errors in your
  // application (e.g. use an assertion, or display an error and quit).
  // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling
  // ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
  // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font
  // rendering.
  // - Read 'docs/FONTS.md' for more instructions and details.
  // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a
  // double backslash \\ !
  // io.Fonts->AddFontDefault();
  // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
  // ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL,
  // io.Fonts->GetGlyphRangesJapanese()); IM_ASSERT(font != NULL);
  return 0;
}

int gfx_next_frame(gfx_handler_t *handler) {
  if (glfwWindowShouldClose(handler->window))
    return 1;

  // Poll and handle events (inputs, window resize, etc.)
  // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use
  // your inputs.
  // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or
  // clear/overwrite your copy of the mouse data.
  // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
  // clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui,
  // and hide them from your application based on those two flags.
  glfwPollEvents();

  // Resize swap chain?
  int fb_width, fb_height;
  glfwGetFramebufferSize(handler->window, &fb_width, &fb_height);
  if (fb_width > 0 && fb_height > 0 &&
      (handler->g_SwapChainRebuild || handler->g_MainWindowData.Width != fb_width ||
       handler->g_MainWindowData.Height != fb_height)) {
    ImGui_ImplVulkan_SetMinImageCount(handler->g_MinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        handler->g_Instance, handler->g_PhysicalDevice, handler->g_Device, &handler->g_MainWindowData,
        handler->g_QueueFamily, handler->g_Allocator, fb_width, fb_height, handler->g_MinImageCount);
    handler->g_MainWindowData.FrameIndex = 0;
    handler->g_SwapChainRebuild = false;
  }
  if (glfwGetWindowAttrib(handler->window, GLFW_ICONIFIED) != 0) {
    ImGui_ImplGlfw_Sleep(10);
    return 0;
  }

  renderer_update_uniforms(handler);

  // Start the Dear ImGui frame
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  igNewFrame();

  ui_render(&handler->user_interface);

  // Rendering
  igRender();
  ImDrawData *main_draw_data = igGetDrawData();
  bool main_is_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;
  if (!main_is_minimized) {
    frame_render(handler, main_draw_data);
    frame_present(handler);
  }
  return 0;
}

void gfx_cleanup(gfx_handler_t *handler) {
  ui_cleanup(&handler->user_interface);

  VkResult err = vkDeviceWaitIdle(handler->g_Device);
  check_vk_result(err);

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
