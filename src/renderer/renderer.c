#include "renderer.h"
#include <system/fs.h>
#include "graphics_backend.h"
#include <cglm/cglm.h>
#include <logger/logger.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include <system/compat_threads.h>

static pthread_mutex_t g_vulkan_mutex;
static pthread_once_t g_vulkan_mutex_once = PTHREAD_ONCE_INIT;

static void init_vulkan_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_vulkan_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void renderer_lock(void) {
    pthread_once(&g_vulkan_mutex_once, init_vulkan_mutex);
    pthread_mutex_lock(&g_vulkan_mutex);
}

void renderer_unlock(void) {
    pthread_mutex_unlock(&g_vulkan_mutex);
}

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb_image_resize2.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static const char *LOG_SOURCE = "Renderer";

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DYNAMIC_UBO_BUFFER_SIZE (16 * 1024 * 1024) // 16 MB

// helper function prototypes
static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties);
static void create_buffer(gfx_handler_t *handler, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, buffer_t *buffer);
static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool);
static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool, VkCommandBuffer command_buffer);
static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer, VkDeviceSize size);

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout old_layout,
                                    VkImageLayout new_layout, uint32_t mip_levels, uint32_t base_layer, uint32_t layer_count);
static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
static char *read_file(const char *filename, size_t *length);
static VkShaderModule create_shader_module(gfx_handler_t *handler, const char *code, size_t code_size);
static primitive_ubo_t world_ubo(gfx_handler_t *h);
static void upload_texture_layer(gfx_handler_t *h, texture_t *array, VkFormat format, int layer, const void *src, VkDeviceSize bytes);
static void renderer_flush_custom_instances(gfx_handler_t *h, VkCommandBuffer cmd, const render_command_t *q);
static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t base_layer,
                          uint32_t layer_count);
static void flush_primitives(gfx_handler_t *handler, VkCommandBuffer command_buffer);
static void flush_primitives3d(gfx_handler_t *h, VkCommandBuffer command_buffer);
static primitive_ubo_t world_ubo(gfx_handler_t *h);
void renderer_cleanup_atlas_renderer(gfx_handler_t *h, atlas_renderer_t *ar);

// vertex description helpers
static VkVertexInputBindingDescription primitive_binding_description;
static VkVertexInputAttributeDescription primitive_attribute_descriptions[2];
static VkVertexInputBindingDescription primitive3d_binding_description;
static VkVertexInputAttributeDescription primitive3d_attribute_descriptions[4];
static VkVertexInputBindingDescription mesh_binding_description;
static VkVertexInputAttributeDescription mesh_attribute_descriptions[3];

// atlas things
static VkVertexInputBindingDescription atlas_binding_desc[2];
static VkVertexInputAttributeDescription atlas_attrib_descs[9];

static void setup_vertex_descriptions(void);

void check_vk_result(VkResult err) {
  if (err == VK_SUCCESS) return;
  log_error("Vulkan", "VkResult = %d", err);
  if (err < 0) abort();
}

void check_vk_result_line(VkResult err, int line) {
  if (err == VK_SUCCESS) return;
  log_error("Vulkan", "VkResult = %d in renderer.c (line: %d)", err, line);
  if (err < 0) abort();
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties mem_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

  for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
    if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  log_error(LOG_SOURCE, "Failed to find suitable memory type!");
  exit(EXIT_FAILURE);
}

static void create_buffer(gfx_handler_t *handler, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, buffer_t *buffer) {
  VkResult err;
  buffer->size = size;

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

  err = vkCreateBuffer(handler->g_device, &buffer_info, handler->g_allocator, &buffer->buffer);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(handler->g_device, buffer->buffer, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = mem_requirements.size,
                                     .memoryTypeIndex = find_memory_type(handler->g_physical_device, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_device, &alloc_info, handler->g_allocator, &buffer->memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindBufferMemory(handler->g_device, buffer->buffer, buffer->memory, 0);
  check_vk_result_line(err, __LINE__);

  buffer->mapped_memory = NULL;
}

static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};

  VkCommandBuffer command_buffer;
  vkAllocateCommandBuffers(handler->g_device, &alloc_info, &command_buffer);

  VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  vkBeginCommandBuffer(command_buffer, &begin_info);
  return command_buffer;
}

static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool, VkCommandBuffer command_buffer) {
  vkEndCommandBuffer(command_buffer);

  VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command_buffer};

  VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VkResult err = vkCreateFence(handler->g_device, &fence_info, handler->g_allocator, &fence);
  check_vk_result_line(err, __LINE__);

  renderer_lock();
  err = vkQueueSubmit(handler->g_queue, 1, &submit_info, fence);
  renderer_unlock();
  check_vk_result_line(err, __LINE__);

  err = vkWaitForFences(handler->g_device, 1, &fence, VK_TRUE, UINT64_MAX);
  check_vk_result_line(err, __LINE__);

  vkDestroyFence(handler->g_device, fence, handler->g_allocator);

  renderer_lock();
  vkFreeCommandBuffers(handler->g_device, pool, 1, &command_buffer);
  renderer_unlock();
}

static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer, VkDeviceSize size) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferCopy copy_region = {.size = size};
  vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout old_layout,
                                    VkImageLayout new_layout, uint32_t mip_levels, uint32_t base_layer, uint32_t layer_count) {
  (void)format;
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .oldLayout = old_layout,
                                  .newLayout = new_layout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = mip_levels,
                                                       .baseArrayLayer = base_layer,
                                                       .layerCount = layer_count}};

  VkPipelineStageFlags source_stage;
  VkPipelineStageFlags destination_stage;

  switch (old_layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      barrier.srcAccessMask = 0;
      source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;
    default:
      barrier.srcAccessMask = 0;
      source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      break;
  }

  switch (new_layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      barrier.dstAccessMask = 0;
      destination_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;
    default:
      barrier.dstAccessMask = 0;
      destination_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;
  }

  vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, pool, command_buffer);
}

static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferImageCopy region = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1}, .imageExtent = {width, height, 1}};

  vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  end_single_time_commands(handler, pool, command_buffer);
}

void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t array_layers, VkFormat format,
                  VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *image, VkDeviceMemory *image_memory) {
  VkResult err;
  VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {width, height, 1},
                                  .mipLevels = mip_levels,
                                  .arrayLayers = array_layers,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = tiling,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

  err = vkCreateImage(handler->g_device, &image_info, handler->g_allocator, image);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetImageMemoryRequirements(handler->g_device, *image, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = mem_requirements.size,
                                     .memoryTypeIndex = find_memory_type(handler->g_physical_device, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_device, &alloc_info, handler->g_allocator, image_memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindImageMemory(handler->g_device, *image, *image_memory, 0);
  check_vk_result_line(err, __LINE__);
}

VkImageView create_image_view_aspect(gfx_handler_t *handler, VkImage image, VkFormat format, VkImageViewType view_type, uint32_t mip_levels,
                                     uint32_t layer_count, VkImageAspectFlags aspect) {
  VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image,
                                     .viewType = view_type,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = aspect,
                                                          .baseMipLevel = 0,
                                                          .levelCount = mip_levels,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = layer_count}};
  VkImageView view;
  check_vk_result(vkCreateImageView(handler->g_device, &view_info, handler->g_allocator, &view));
  return view;
}

VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format, VkImageViewType view_type, uint32_t mip_levels,
                              uint32_t layer_count) {
  VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = view_type,
      .format = format,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = mip_levels, .baseArrayLayer = 0, .layerCount = layer_count}};

  VkImageView image_view;
  VkResult err = vkCreateImageView(handler->g_device, &view_info, handler->g_allocator, &image_view);
  check_vk_result_line(err, __LINE__);
  return image_view;
}

VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter) {
  return create_texture_sampler_wrapped(handler, mip_levels, filter, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
}

VkSampler create_texture_sampler_wrapped(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter,
                                         VkSamplerAddressMode address_mode) {
  // Repeating means a world surface, and a world surface is usually looked at
  // along its length rather than face on. That is the case anisotropy exists
  // for, and without it a road is a grey smear a few metres ahead of the car.
  VkPhysicalDeviceFeatures features = {0};
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceFeatures(handler->g_physical_device, &features);
  vkGetPhysicalDeviceProperties(handler->g_physical_device, &properties);
  const bool anisotropic = address_mode == VK_SAMPLER_ADDRESS_MODE_REPEAT && features.samplerAnisotropy;
  const float max_anisotropy = anisotropic ? fminf(16.f, properties.limits.maxSamplerAnisotropy) : 1.f;

  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = (VkFilter)filter,
                                      .minFilter = (VkFilter)filter,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = address_mode,
                                      .addressModeV = address_mode,
                                      .addressModeW = address_mode,
                                      .mipLodBias = 0.0f,
                                      .anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE,
                                      .maxAnisotropy = max_anisotropy,
                                      .compareEnable = VK_FALSE,
                                      .compareOp = VK_COMPARE_OP_ALWAYS,
                                      .minLod = 0.0f,
                                      .maxLod = (float)mip_levels,
                                      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                      .unnormalizedCoordinates = VK_FALSE};

  VkSampler sampler;
  VkResult err = vkCreateSampler(handler->g_device, &sampler_info, handler->g_allocator, &sampler);
  check_vk_result_line(err, __LINE__);
  return sampler;
}

static char *read_file(const char *filename, size_t *length) {
  FILE *file = fs_open(filename, "rb");
  if (!file) {
    log_error(LOG_SOURCE, "Failed to open file: %s", filename);
    return NULL;
  }

  fseek(file, 0, SEEK_END);
  *length = ftell(file);
  fseek(file, 0, SEEK_SET);

  char *buffer = (char *)malloc(*length);
  if (!buffer) {
    log_error(LOG_SOURCE, "Failed to allocate memory for file: %s", filename);
    fclose(file);
    return NULL;
  }

  size_t read_count = fread(buffer, 1, *length, file);
  fclose(file);

  if (read_count != *length) {
    log_error(LOG_SOURCE, "Failed to read entire file: %s", filename);
    free(buffer);
    return NULL;
  }

  return buffer;
}

static VkShaderModule create_shader_module(gfx_handler_t *handler, const char *code, size_t code_size) {
  VkShaderModuleCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_size, .pCode = (const uint32_t *)code};

  VkShaderModule shader_module;
  VkResult err = vkCreateShaderModule(handler->g_device, &create_info, handler->g_allocator, &shader_module);
  check_vk_result_line(err, __LINE__);

  return shader_module;
}

static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t base_layer,
                          uint32_t layer_count) {
  if (mip_levels <= 1) return true;

  VkCommandBuffer cmd_buffer = begin_single_time_commands(handler, handler->renderer.transfer_command_pool);

  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .image = image,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseArrayLayer = base_layer, .layerCount = layer_count, .levelCount = 1}};

  int32_t mip_width = width;
  int32_t mip_height = height;

  for (uint32_t i = 1; i < mip_levels; i++) {
    barrier.subresourceRange.baseMipLevel = i - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkImageBlit blit = {
        .srcOffsets[0] = {0, 0, 0},
        .srcOffsets[1] = {mip_width, mip_height, 1},
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = i - 1, .baseArrayLayer = base_layer, .layerCount = layer_count},
        .dstOffsets[0] = {0, 0, 0},
        .dstOffsets[1] = {mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = i, .baseArrayLayer = base_layer, .layerCount = layer_count}};

    vkCmdBlitImage(cmd_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    if (mip_width > 1) mip_width /= 2;
    if (mip_height > 1) mip_height /= 2;
  }

  barrier.subresourceRange.baseMipLevel = mip_levels - 1;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, handler->renderer.transfer_command_pool, cmd_buffer);
  return true;
}

texture_t *renderer_create_texture_2d_array(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t layer_count, VkFormat format) {
  renderer_state_t *renderer = &handler->renderer;

  // find free slot
  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }
  if (free_slot == (uint32_t)-1) {
    log_error(LOG_SOURCE, "Max texture count (%d) reached.", MAX_TEXTURES);
    return NULL;
  }

  texture_t *texArray = &renderer->textures[free_slot];
  memset(texArray, 0, sizeof(texture_t));
  texArray->id = free_slot;
  texArray->active = true;
  texArray->width = width;
  texArray->height = height;
  texArray->mip_levels = (uint32_t)floor(log2(fmax(width, height))) + 1;
  texArray->layer_count = layer_count;
  texArray->format = format;
  strncpy(texArray->path, "runtime_texture_array", sizeof(texArray->path) - 1);

  // Create the VkImage (2D array)
  create_image(handler, width, height, texArray->mip_levels, layer_count, format, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
               &texArray->image, &texArray->memory);

  // Transition all layers once
  transition_image_layout(handler, renderer->transfer_command_pool, texArray->image, format, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texArray->mip_levels, 0, layer_count);
  // Transition to shader read (empty until uploads)
  transition_image_layout(handler, renderer->transfer_command_pool, texArray->image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texArray->mip_levels, 0, layer_count);

  // Create view as 2D array
  texArray->image_view = create_image_view(handler, texArray->image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, texArray->mip_levels, layer_count);

  // Create sampler
  texArray->sampler = create_texture_sampler(handler, texArray->mip_levels, VK_FILTER_LINEAR);

  // log_info(LOG_SOURCE, "Created 2D texture array (%ux%u x %u layers)", width, height, layer_count);
  return texArray;
}

int renderer_init(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  memset(renderer, 0, sizeof(renderer_state_t));
  renderer->gfx = handler;

  // Initialize the render queue
  renderer->queue.commands = malloc(sizeof(render_command_t) * MAX_RENDER_COMMANDS);
  renderer->queue.count = 0;

  setup_vertex_descriptions();

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(handler->g_physical_device, &properties);
  renderer->min_ubo_alignment = properties.limits.minUniformBufferOffsetAlignment;

  renderer->camera.zoom_wanted = 5.0f;
  renderer->lod_bias = -0.5f; // Default bias

  // Initialize transient memory (128 MB)
  renderer->transient_capacity = 128 * 1024 * 1024;
  renderer->transient_memory = malloc(renderer->transient_capacity);
  renderer->transient_offset = 0;

  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                       .queueFamilyIndex = handler->g_queue_family};
  check_vk_result(vkCreateCommandPool(handler->g_device, &pool_info, handler->g_allocator, &renderer->transfer_command_pool));

  VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * MAX_TEXTURES_PER_DRAW}};
  for (int i = 0; i < 3; i++) { // triple buffering
    VkDescriptorPoolCreateInfo pool_create_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                   .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                   .maxSets = 4096,
                                                   .poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
                                                   .pPoolSizes = pool_sizes};
    check_vk_result(vkCreateDescriptorPool(handler->g_device, &pool_create_info, handler->g_allocator, &renderer->frame_descriptor_pools[i]));
  }
  unsigned char white_pixel[] = {255, 255, 255, 255};
  texture_t *default_tex = renderer_load_texture_from_array(handler, white_pixel, 1, 1);
  strncpy(default_tex->path, "default_white", sizeof(default_tex->path) - 1);
  renderer->default_texture = default_tex;

  // Primitive & UBO Ring Buffer Setup
  renderer->primitive_shader = renderer_load_shader(handler, "data/shaders/primitive.vert.spv", "data/shaders/primitive.frag.spv");

  create_buffer(handler, MAX_PRIMITIVE_VERTICES * sizeof(primitive_vertex_t), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_vertex_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_vertex_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&renderer->vertex_buffer_ptr);

  renderer->primitive3d_shader =
      renderer_load_shader(handler, "data/shaders/primitive3d.vert.spv", "data/shaders/primitive3d.frag.spv");
  create_buffer(handler, MAX_PRIMITIVE3D_VERTICES * sizeof(primitive3d_vertex_t), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_vertex_buffer3d);
  vkMapMemory(handler->g_device, renderer->dynamic_vertex_buffer3d.memory, 0, VK_WHOLE_SIZE, 0, (void **)&renderer->vertex3d_buffer_ptr);

  renderer->primitive3d_sampler =
      create_texture_sampler_wrapped(handler, 16, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);

  // One white page, bound whenever a game draws in 3D without a texture array
  // of its own. The 3D fragment stage declares a sampler either way.
  renderer->primitive3d_fallback_texture =
      renderer_create_texture_2d_array(handler, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
  if (renderer->primitive3d_fallback_texture) {
    const uint8_t white[4] = {255, 255, 255, 255};
    renderer_update_texture_layer(handler, renderer->primitive3d_fallback_texture, 0, white, 1, 1);
  }

  // Sensible orbit for a game that has not moved the camera yet.
  renderer->camera3.distance = 40.f;
  renderer->camera3.yaw = -1.57f;
  renderer->camera3.pitch = 0.5f;
  renderer->camera3.fov_y = 1.0f;
  renderer->camera3.near_z = 0.1f;
  renderer->camera3.far_z = 5000.f;

  create_buffer(handler, MAX_PRIMITIVE_INDICES * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_index_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_index_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&renderer->index_buffer_ptr);

  create_buffer(handler, DYNAMIC_UBO_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_ubo_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_ubo_buffer.memory, 0, VK_WHOLE_SIZE, 0, &renderer->ubo_buffer_ptr);


  // Game Skin Sprites (32x16 grid, 32px unit)

  log_info(LOG_SOURCE, "Renderer initialized successfully.");
  return 0;
}

void renderer_cleanup(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  VkDevice device = handler->g_device;
  VkAllocationCallbacks *allocator = handler->g_allocator;

  if (renderer->queue.commands) {
    free(renderer->queue.commands);
    renderer->queue.commands = NULL;
  }

  vkDeviceWaitIdle(device);

  for (uint32_t i = 0; i < MAX_SHADERS; ++i) {
    for (uint32_t j = 0; j < 3; j++) {
      pipeline_cache_entry_t *entry = &renderer->pipeline_cache[i][j];
      if (entry->initialized) {
        vkDestroyPipeline(device, entry->pipeline, allocator);
        vkDestroyPipelineLayout(device, entry->pipeline_layout, allocator);
        vkDestroyDescriptorSetLayout(device, entry->descriptor_set_layout, allocator);
      }
    }
  }

  if (renderer->preview_render_pass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device, renderer->preview_render_pass, allocator);
    renderer->preview_render_pass = VK_NULL_HANDLE;
  }

  for (uint32_t i = 0; i < MAX_MESHES; ++i) {
    mesh_t *m = &renderer->meshes[i];
    if (m->active) {
      vkDestroyBuffer(device, m->vertex_buffer.buffer, allocator);
      vkFreeMemory(device, m->vertex_buffer.memory, allocator);
      if (m->index_buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m->index_buffer.buffer, allocator);
        vkFreeMemory(device, m->index_buffer.memory, allocator);
      }
    }
  }

  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    texture_t *t = &renderer->textures[i];
    if (t->active) {
      vkDestroySampler(device, t->sampler, allocator);
      vkDestroyImageView(device, t->image_view, allocator);
      vkDestroyImage(device, t->image, allocator);
      vkFreeMemory(device, t->memory, allocator);
    }
  }

  for (uint32_t i = 0; i < MAX_SHADERS; ++i) {
    shader_t *s = &renderer->shaders[i];
    if (s->active) {
      vkDestroyShaderModule(device, s->vert_shader_module, allocator);
      vkDestroyShaderModule(device, s->frag_shader_module, allocator);
    }
  }

  vkDestroyBuffer(device, renderer->dynamic_vertex_buffer.buffer, allocator);
  vkFreeMemory(device, renderer->dynamic_vertex_buffer.memory, allocator);
  vkDestroyBuffer(device, renderer->dynamic_index_buffer.buffer, allocator);
  vkFreeMemory(device, renderer->dynamic_index_buffer.memory, allocator);
  vkDestroyBuffer(device, renderer->dynamic_ubo_buffer.buffer, allocator);
  vkFreeMemory(device, renderer->dynamic_ubo_buffer.memory, allocator);

  for (int i = 0; i < 3; i++) {
    if (renderer->frame_descriptor_pools[i] != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device, renderer->frame_descriptor_pools[i], allocator);
      renderer->frame_descriptor_pools[i] = VK_NULL_HANDLE;
    }
  }
  vkDestroyCommandPool(device, renderer->transfer_command_pool, allocator);


  if (renderer->transient_memory) {
    free(renderer->transient_memory);
    renderer->transient_memory = NULL;
  }

  log_info(LOG_SOURCE, "Renderer cleaned up successfully.");
}

static VkRenderPass get_or_create_preview_render_pass(gfx_handler_t *handler, VkFormat format) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->preview_render_pass != VK_NULL_HANDLE) {
    return renderer->preview_render_format == format ? renderer->preview_render_pass : VK_NULL_HANDLE;
  }

  const VkAttachmentDescription attachment = {
      .format = format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  const VkAttachmentReference color_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color_ref};
  const VkSubpassDependency dependencies[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
  };
  const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                       .attachmentCount = 1,
                                       .pAttachments = &attachment,
                                       .subpassCount = 1,
                                       .pSubpasses = &subpass,
                                       .dependencyCount = 2,
                                       .pDependencies = dependencies};
  if (vkCreateRenderPass(handler->g_device, &info, handler->g_allocator, &renderer->preview_render_pass) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  renderer->preview_render_format = format;
  return renderer->preview_render_pass;
}

static pipeline_cache_entry_t *get_or_create_pipeline(gfx_handler_t *handler, shader_t *shader, uint32_t ubo_count, uint32_t texture_count, VkRenderPass target_render_pass) {
  if (!shader) return NULL;
  renderer_state_t *renderer = &handler->renderer;
  int rp_idx = target_render_pass == handler->offscreen_render_pass ? 1 : target_render_pass == renderer->preview_render_pass ? 2 : 0;
  pipeline_cache_entry_t *entry = &renderer->pipeline_cache[shader->id][rp_idx];

  if (entry->initialized && entry->ubo_count == ubo_count && entry->texture_count == texture_count && entry->render_pass == target_render_pass) {
    return entry;
  }

  if (entry->initialized) {
    vkDestroyPipeline(handler->g_device, entry->pipeline, handler->g_allocator);
    vkDestroyPipelineLayout(handler->g_device, entry->pipeline_layout, handler->g_allocator);
    vkDestroyDescriptorSetLayout(handler->g_device, entry->descriptor_set_layout, handler->g_allocator);
  }

  entry->ubo_count = ubo_count;
  entry->texture_count = texture_count;
  entry->render_pass = target_render_pass;

  // Internal Layout Selection Logic
  VkVertexInputBindingDescription *binding_descs;
  uint32_t b_desc_count;
  VkVertexInputAttributeDescription *attrib_descs;
  uint32_t a_desc_count;

  if (shader->layout && shader->layout->attr_count > 0) {
    // A shader created by a game module describes its own vertex input, so the
    // renderer never has to recognise the technique to be able to draw it.
    binding_descs = shader->layout->bindings;
    b_desc_count = shader->layout->binding_count;
    attrib_descs = shader->layout->attrs;
    a_desc_count = shader->layout->attr_count;
  } else if (shader == renderer->primitive_shader) {
    binding_descs = &primitive_binding_description;
    b_desc_count = 1;
    attrib_descs = primitive_attribute_descriptions;
    a_desc_count = 2;
  } else if (shader == renderer->primitive3d_shader) {
    binding_descs = &primitive3d_binding_description;
    b_desc_count = 1;
    attrib_descs = primitive3d_attribute_descriptions;
    a_desc_count = 4;
  } else {
    binding_descs = &mesh_binding_description;
    b_desc_count = 1;
    attrib_descs = mesh_attribute_descriptions;
    a_desc_count = 3;
  }

  uint32_t total_bindings = ubo_count + texture_count;
  VLA(VkDescriptorSetLayoutBinding, layout_bindings, total_bindings);

  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    uint32_t b_idx = current_binding++;
    layout_bindings[b_idx] = (VkDescriptorSetLayoutBinding){
        .binding = b_idx,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  }
  for (uint32_t i = 0; i < texture_count; ++i) {
    uint32_t b_idx = current_binding++;
    layout_bindings[b_idx] = (VkDescriptorSetLayoutBinding){
        .binding = b_idx,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = total_bindings, .pBindings = layout_bindings};
  check_vk_result(vkCreateDescriptorSetLayout(handler->g_device, &layout_info, handler->g_allocator, &entry->descriptor_set_layout));

  VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &entry->descriptor_set_layout};
  check_vk_result(vkCreatePipelineLayout(handler->g_device, &pipeline_layout_info, handler->g_allocator, &entry->pipeline_layout));

  VkPipelineShaderStageCreateInfo shader_stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader->vert_shader_module, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader->frag_shader_module, .pName = "main"}};

  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = b_desc_count,
      .pVertexBindingDescriptions = binding_descs,
      .vertexAttributeDescriptionCount = a_desc_count,
      .pVertexAttributeDescriptions = attrib_descs};

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};

  VkPipelineRasterizationStateCreateInfo rasterizer = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};

  VkPipelineMultisampleStateCreateInfo multisampling = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};

  // 2D draws are ordered by the z a game supplies, so they must not depth-test
  // against each other. 3D geometry has no such ordering and depends on it.
  const bool depth_3d = shader == renderer->primitive3d_shader;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                         .depthTestEnable = depth_3d ? VK_TRUE : VK_FALSE,
                                                         .depthWriteEnable = depth_3d ? VK_TRUE : VK_FALSE,
                                                         .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                                         .maxDepthBounds = 1.0f};

  VkBlendFactor src_color_factor = VK_BLEND_FACTOR_ONE;

  // CORRECTED: Standard Alpha Blending
  VkPipelineColorBlendAttachmentState color_blend = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = src_color_factor,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blending = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &color_blend};

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic_states};

  VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .stageCount = 2,
                                                .pStages = shader_stages,
                                                .pVertexInputState = &vertex_input_info,
                                                .pInputAssemblyState = &input_assembly,
                                                .pViewportState = &viewport_state,
                                                .pRasterizationState = &rasterizer,
                                                .pMultisampleState = &multisampling,
                                                .pDepthStencilState = &depth_stencil,
                                                .pColorBlendState = &color_blending,
                                                .pDynamicState = &dynamic_state,
                                                .layout = entry->pipeline_layout,
                                                .renderPass = target_render_pass,
                                                .subpass = 0};

  check_vk_result(vkCreateGraphicsPipelines(handler->g_device, handler->g_pipeline_cache, 1, &pipeline_info, handler->g_allocator, &entry->pipeline));

  VLA_FREE(layout_bindings);
  entry->initialized = true;
  return entry;
}

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path) {
  renderer_state_t *renderer = &handler->renderer;

  for (uint32_t i = 0; i < renderer->shader_count; ++i) {
    if (renderer->shaders[i].active && strcmp(renderer->shaders[i].vert_path, vert_path) == 0 &&
        strcmp(renderer->shaders[i].frag_path, frag_path) == 0) {
      return &renderer->shaders[i];
    }
  }

  if (renderer->shader_count >= MAX_SHADERS) {
    log_error(LOG_SOURCE, "Max shader count (%d) reached.", MAX_SHADERS);
    return NULL;
  }

  size_t vert_size, frag_size;
  char *vert_code = read_file(vert_path, &vert_size);
  char *frag_code = read_file(frag_path, &frag_size);
  if (!vert_code || !frag_code) {
    free(vert_code);
    free(frag_code);
    return NULL;
  }

  shader_t *shader = &renderer->shaders[renderer->shader_count];
  shader->id = renderer->shader_count++;
  shader->active = true;
  shader->vert_shader_module = create_shader_module(handler, vert_code, vert_size);
  shader->frag_shader_module = create_shader_module(handler, frag_code, frag_size);
  strncpy(shader->vert_path, vert_path, sizeof(shader->vert_path) - 1);
  strncpy(shader->frag_path, frag_path, sizeof(shader->frag_path) - 1);

  free(vert_code);
  free(frag_code);
  return shader;
}

texture_t *renderer_load_compact_texture_from_array(gfx_handler_t *handler, const uint8_t **pixel_array, uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (!pixel_array) return NULL;

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    log_error(LOG_SOURCE, "Max texture count (%d) reached.", MAX_TEXTURES);
    return NULL;
  }

  VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

  stbi_uc *rgba_pixels = calloc(1, image_size);
  if (height == 1 && width == 1) { // Special case for default texture
    memcpy(rgba_pixels, pixel_array, image_size);
  } else {
    for (uint32_t i = 0; i < width * height; i++) {
      if (pixel_array[0]) rgba_pixels[i * 4 + 0] = pixel_array[0][i];
      if (pixel_array[1]) rgba_pixels[i * 4 + 1] = pixel_array[1][i];
      if (pixel_array[2]) rgba_pixels[i * 4 + 2] = pixel_array[2][i];
      rgba_pixels[i * 4 + 3] = 255;
    }
  }

  texture_t *texture = &renderer->textures[free_slot];
  memset(texture, 0, sizeof(texture_t));
  texture->id = free_slot;
  texture->active = true;
  texture->width = width;
  texture->height = height;
  texture->mip_levels = 1;
  texture->layer_count = 1;
  texture->format = VK_FORMAT_R8G8B8A8_UNORM;
  strncpy(texture->path, "from_array", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  free(rgba_pixels);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width, height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 0, 1);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
  texture->sampler = create_texture_sampler(handler, 1, VK_FILTER_NEAREST);

  return texture;
}

texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array, uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (!pixel_array) return NULL;

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    log_error(LOG_SOURCE, "Max texture count (%d) reached.", MAX_TEXTURES);
    return NULL;
  }

  VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

  stbi_uc *rgba_pixels = malloc(image_size);
  if (height == 1 && width == 1) { // Special case for default texture
    memcpy(rgba_pixels, pixel_array, image_size);
  } else { // Convert R to RGBA
    for (uint32_t i = 0; i < width * height; i++) {
      rgba_pixels[i * 4 + 0] = pixel_array[i];
      rgba_pixels[i * 4 + 1] = pixel_array[i];
      rgba_pixels[i * 4 + 2] = pixel_array[i];
      rgba_pixels[i * 4 + 3] = 255;
    }
  }

  texture_t *texture = &renderer->textures[free_slot];
  memset(texture, 0, sizeof(texture_t));
  texture->id = free_slot;
  texture->active = true;
  texture->width = width;
  texture->height = height;
  texture->mip_levels = 1;
  texture->layer_count = 1;
  texture->format = VK_FORMAT_R8G8B8A8_UNORM;
  strncpy(texture->path, "from_array", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  free(rgba_pixels);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width, height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 0, 1);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
  texture->sampler = create_texture_sampler(handler, 1, VK_FILTER_NEAREST);

  return texture;
}

texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path) {
  renderer_state_t *renderer = &handler->renderer;

  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (renderer->textures[i].active && strcmp(renderer->textures[i].path, image_path) == 0) {
      return &renderer->textures[i];
    }
  }

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    log_error(LOG_SOURCE, "Max texture count (%d) reached.", MAX_TEXTURES);
    return NULL;
  }

  int tex_width, tex_height, tex_channels;
  FILE *f = fs_open(image_path, "rb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open texture file: %s", image_path);
    return NULL;
  }
  stbi_uc *pixels = stbi_load_from_file(f, &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
  fclose(f);
  if (!pixels) {
    log_error(LOG_SOURCE, "Failed to load texture image: %s", image_path);
    return NULL;
  }

  VkDeviceSize image_size = (VkDeviceSize)tex_width * tex_height * 4;
  uint32_t mip_levels = (uint32_t)floor(log2(fmax(tex_width, tex_height))) + 1;

  texture_t *texture = &renderer->textures[free_slot];
  memset(texture, 0, sizeof(texture_t));
  texture->id = free_slot;
  texture->active = true;
  texture->width = tex_width;
  texture->height = tex_height;
  texture->mip_levels = mip_levels;
  texture->layer_count = 1;
  texture->format = VK_FORMAT_R8G8B8A8_UNORM;
  strncpy(texture->path, image_path, sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  stbi_image_free(pixels);

  create_image(handler, tex_width, tex_height, mip_levels, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
               &texture->image, &texture->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, tex_width, tex_height);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  if (!build_mipmaps(handler, texture->image, tex_width, tex_height, mip_levels, 0, 1)) {
    transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mip_levels, 0, 1);
  }

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, mip_levels, 1);
  texture->sampler = create_texture_sampler(handler, mip_levels, VK_FILTER_LINEAR);

  // log_info(LOG_SOURCE, "Loaded texture: %s", image_path);
  return texture;
}

// Filter for the next single-layer texture. Set by renderer_create_texture_layered
// so a caller can ask for nearest sampling without every existing call site
// growing a parameter it does not care about.
static VkFilter g_next_texture_filter = VK_FILTER_LINEAR;

texture_t *renderer_create_texture_from_rgba(gfx_handler_t *handler, const unsigned char *pixels, int width, int height) {
  renderer_lock();
  renderer_state_t *renderer = &handler->renderer;
  if (!pixels) {
    renderer_unlock();
    return NULL;
  }

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    // Texture pointers are public handles held by game modules, atlases and
    // ImGui descriptor sets. Evicting an arbitrary old slot invalidates those
    // handles and can turn a later descriptor update into a driver crash.
    // Callers that implement caches must release their own entries explicitly.
    log_error(LOG_SOURCE, "Max texture count (%d) reached; refusing to evict a live texture.", MAX_TEXTURES);
    renderer_unlock();
    return NULL;
  }

  VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

  texture_t *texture = &renderer->textures[free_slot];
  memset(texture, 0, sizeof(texture_t));
  texture->id = free_slot;
  texture->active = true;
  texture->last_used_frame = handler->g_main_window_data.FrameIndex;
  texture->width = width;
  texture->height = height;
  texture->mip_levels = 1; // No mipmaps for previews
  texture->layer_count = 1;
  texture->format = VK_FORMAT_R8G8B8A8_UNORM;
  strncpy(texture->path, "from_rgba_memory", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width, height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 0, 1);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
  texture->sampler = create_texture_sampler(handler, 1, g_next_texture_filter);
  g_next_texture_filter = VK_FILTER_LINEAR;
  renderer_unlock();
  return texture;
}

mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count, uint32_t *indices, uint32_t index_count) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->mesh_count >= MAX_MESHES) {
    log_error(LOG_SOURCE, "Maximum mesh count (%d) reached.", MAX_MESHES);
    return NULL;
  }

  mesh_t *mesh = &renderer->meshes[renderer->mesh_count];
  mesh->id = renderer->mesh_count++;
  mesh->active = true;
  mesh->vertex_count = vertex_count;
  mesh->index_count = index_count;
  mesh->index_buffer.buffer = VK_NULL_HANDLE;
  mesh->index_buffer.memory = VK_NULL_HANDLE;

  VkDeviceSize vertex_buffer_size = sizeof(vertex_t) * vertex_count;
  VkDeviceSize index_buffer_size = sizeof(uint32_t) * index_count;

  buffer_t vertex_staging_buffer;
  buffer_t index_staging_buffer;

  create_buffer(handler, vertex_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertex_staging_buffer);
  void *data;
  vkMapMemory(handler->g_device, vertex_staging_buffer.memory, 0, vertex_buffer_size, 0, &data);
  memcpy(data, vertices, (size_t)vertex_buffer_size);
  vkUnmapMemory(handler->g_device, vertex_staging_buffer.memory);

  create_buffer(handler, vertex_buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->vertex_buffer);

  copy_buffer(handler, renderer->transfer_command_pool, vertex_staging_buffer.buffer, mesh->vertex_buffer.buffer, vertex_buffer_size);

  vkDestroyBuffer(handler->g_device, vertex_staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, vertex_staging_buffer.memory, handler->g_allocator);

  if (index_count > 0 && indices) {
    create_buffer(handler, index_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &index_staging_buffer);
    vkMapMemory(handler->g_device, index_staging_buffer.memory, 0, index_buffer_size, 0, &data);
    memcpy(data, indices, (size_t)index_buffer_size);
    vkUnmapMemory(handler->g_device, index_staging_buffer.memory);

    create_buffer(handler, index_buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->index_buffer);

    copy_buffer(handler, renderer->transfer_command_pool, index_staging_buffer.buffer, mesh->index_buffer.buffer, index_buffer_size);

    vkDestroyBuffer(handler->g_device, index_staging_buffer.buffer, handler->g_allocator);
    vkFreeMemory(handler->g_device, index_staging_buffer.memory, handler->g_allocator);
  } else {
    mesh->index_count = 0;
  }

  // log_info(LOG_SOURCE, "Created mesh (Vertices: %u, Indices: %u)", vertex_count, index_count);
  return mesh;
}

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &handler->renderer;
  uint32_t frame_pool_index = handler->g_main_window_data.FrameIndex % 3;
  check_vk_result(vkResetDescriptorPool(handler->g_device, renderer->frame_descriptor_pools[frame_pool_index], 0));
  renderer->primitive_vertex_count = 0;
  renderer->primitive_index_count = 0;
  renderer->primitive_index_offset_drawn = 0;
  renderer->primitive3d_vertex_count = 0;
  renderer->ubo_buffer_offset = 0;
  renderer->current_command_buffer = command_buffer;
  renderer->transient_offset = 0;

  VkViewport viewport = {0.f, 0.f, handler->viewport[0], handler->viewport[1], 0.0f, 1.0f};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor = {{0.0, 0.0}, {handler->viewport[0], handler->viewport[1]}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}
void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh, shader_t *shader, texture_t **textures,
                        uint32_t texture_count, void **ubos, VkDeviceSize *ubo_sizes, uint32_t ubo_count) {
  if (!mesh || !shader || !mesh->active || !shader->active) return;
  renderer_state_t *renderer = &handler->renderer;

  VkRenderPass target_pass = handler->offscreen_initialized && handler->offscreen_render_pass != VK_NULL_HANDLE
                                 ? handler->offscreen_render_pass
                                 : handler->g_main_window_data.RenderPass;
  pipeline_cache_entry_t *pso = get_or_create_pipeline(handler, shader, ubo_count, texture_count, target_pass);
  if (!pso) return;

  // Allocate a descriptor set for this pipeline
  VkDescriptorSet descriptor_set;
  uint32_t frame_pool_index = handler->g_main_window_data.FrameIndex % 3;
  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = renderer->frame_descriptor_pools[frame_pool_index],
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &pso->descriptor_set_layout};
  VkResult err = vkAllocateDescriptorSets(handler->g_device, &alloc_info, &descriptor_set);
  check_vk_result_line(err, __LINE__);

  uint32_t binding_count = ubo_count + texture_count;
  VLA(VkWriteDescriptorSet, descriptor_writes, binding_count);
  VLA(VkDescriptorBufferInfo, buffer_infos, ubo_count);
  VLA(VkDescriptorImageInfo, image_infos, texture_count);
  VLA(VkDeviceSize, ubo_offsets, ubo_count);

  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    VkDeviceSize aligned_size = (ubo_sizes[i] + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
    assert(renderer->ubo_buffer_offset + aligned_size <= DYNAMIC_UBO_BUFFER_SIZE);

    ubo_offsets[i] = renderer->ubo_buffer_offset;
    memcpy((char *)renderer->ubo_buffer_ptr + ubo_offsets[i], ubos[i], ubo_sizes[i]);
    renderer->ubo_buffer_offset += (uint32_t)aligned_size;

    buffer_infos[i] = (VkDescriptorBufferInfo){.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = ubo_offsets[i], .range = ubo_sizes[i]};
    uint32_t binding_index = current_binding++;
    descriptor_writes[binding_index] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                              .dstSet = descriptor_set,
                                                              .dstBinding = binding_index,
                                                              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                              .descriptorCount = 1,
                                                              .pBufferInfo = &buffer_infos[i]};
  }
  for (uint32_t i = 0; i < texture_count; ++i) {
    texture_t *tex = (textures && textures[i] && textures[i]->active && textures[i]->image_view != VK_NULL_HANDLE && textures[i]->sampler != VK_NULL_HANDLE)
                          ? textures[i]
                          : handler->renderer.default_texture;
    image_infos[i] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = tex->image_view,
        .sampler = tex->sampler};
    uint32_t binding_index = current_binding++;
    descriptor_writes[binding_index] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                              .dstSet = descriptor_set,
                                                              .dstBinding = binding_index,
                                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                              .descriptorCount = 1,
                                                              .pImageInfo = &image_infos[i]};
  }
  vkUpdateDescriptorSets(handler->g_device, binding_count, descriptor_writes, 0, NULL);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);

  VkBuffer vertex_buffers[] = {mesh->vertex_buffer.buffer};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);

  if (mesh->index_count > 0) {
    vkCmdBindIndexBuffer(command_buffer, mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  }

  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);

  if (mesh->index_count > 0) {
    vkCmdDrawIndexed(command_buffer, mesh->index_count, 1, 0, 0, 0);
  } else {
    vkCmdDraw(command_buffer, mesh->vertex_count, 1, 0, 0);
  }
  VLA_FREE(ubo_offsets);
  VLA_FREE(image_infos);
  VLA_FREE(buffer_infos);
  VLA_FREE(descriptor_writes);
}

void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  // 3D first: it owns the depth buffer, and the 2D primitives a game draws on
  // top are overlays that should not be occluded by the world behind them.
  flush_primitives3d(handler, command_buffer);
  flush_primitives(handler, command_buffer);
}

void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex) {
  if (!tex || !tex->active) return;
  renderer_lock();
  gfx_handler_t *h = handler;

  if (h->retire_count >= 256) {
    renderer_unlock();
    vkDeviceWaitIdle(h->g_device);
    renderer_lock();
    for (uint32_t i = 0; i < h->retire_count; i++) {
      if (h->retire_textures[i].sampler) vkDestroySampler(h->g_device, h->retire_textures[i].sampler, h->g_allocator);
      if (h->retire_textures[i].image_view) vkDestroyImageView(h->g_device, h->retire_textures[i].image_view, h->g_allocator);
      if (h->retire_textures[i].image) vkDestroyImage(h->g_device, h->retire_textures[i].image, h->g_allocator);
      if (h->retire_textures[i].memory) vkFreeMemory(h->g_device, h->retire_textures[i].memory, h->g_allocator);
    }
    h->retire_count = 0;
  }

  h->retire_textures[h->retire_count].image = tex->image;
  h->retire_textures[h->retire_count].image_view = tex->image_view;
  h->retire_textures[h->retire_count].sampler = tex->sampler;
  h->retire_textures[h->retire_count].memory = tex->memory;
  h->retire_textures[h->retire_count].frame_serial = h->frame_serial;
  h->retire_count++;

  memset(tex, 0, sizeof(texture_t));
  renderer_unlock();
}

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas, uint32_t tile_width, uint32_t tile_height,
                                                    uint32_t num_tiles_x, uint32_t num_tiles_y) {
  renderer_state_t *renderer = &handler->renderer;
  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    log_error(LOG_SOURCE, "Max texture count (%d) reached.", MAX_TEXTURES);
    return NULL;
  }

  uint32_t layer_count = num_tiles_x * num_tiles_y;
  uint32_t mip_levels = (uint32_t)floorf(log2f(fmaxf(tile_width, tile_height))) + 1;

  texture_t *tex_array = &renderer->textures[free_slot];
  memset(tex_array, 0, sizeof(texture_t));
  tex_array->id = free_slot;
  tex_array->active = true;
  tex_array->width = tile_width;
  tex_array->height = tile_height;
  tex_array->mip_levels = mip_levels;
  tex_array->layer_count = layer_count;
  tex_array->format = VK_FORMAT_R8G8B8A8_UNORM;
  strncpy(tex_array->path, "entities_texture_array", sizeof(tex_array->path) - 1);

  create_image(handler, tile_width, tile_height, mip_levels, layer_count, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
               &tex_array->image, &tex_array->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, tex_array->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, 0, layer_count);

  VkCommandBuffer cmd = begin_single_time_commands(handler, renderer->transfer_command_pool);
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .image = atlas->image,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = atlas->mip_levels, .baseArrayLayer = 0, .layerCount = 1}};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

  for (uint32_t layer = 0; layer < layer_count; layer++) {
    uint32_t tile_x = layer % num_tiles_x;
    uint32_t tile_y = layer / num_tiles_x;

    VkImageCopy copy_region = {.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
                               .srcOffset = {(int32_t)(tile_x * tile_width), (int32_t)(tile_y * tile_height), 0},
                               .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseArrayLayer = layer, .layerCount = 1},
                               .extent = {tile_width, tile_height, 1}};
    vkCmdCopyImage(cmd, atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_array->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
  }

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, renderer->transfer_command_pool, cmd);

  build_mipmaps(handler, tex_array->image, tile_width, tile_height, mip_levels, 0, layer_count);

  tex_array->image_view =
      create_image_view(handler, tex_array->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip_levels, layer_count);
  tex_array->sampler = create_texture_sampler(handler, mip_levels, VK_FILTER_LINEAR);

  return tex_array;
}

void screen_to_world(gfx_handler_t *h, float sx, float sy, float *wx, float *wy) {
  camera_t *cam = &h->renderer.camera;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = h->world_width / h->world_height;
  float aspect = window_ratio / map_ratio;

  float max_map_size = fmax(h->world_width, h->world_height) * 0.001f;
  float ndc_x = (2.0f * sx / h->viewport[0]) - 1.0f;
  float ndc_y = (2.0f * sy / h->viewport[1]) - 1.0f;

  *wx = cam->pos[0] + (ndc_x / (cam->zoom * max_map_size));
  *wy = cam->pos[1] + (ndc_y / (cam->zoom * max_map_size * aspect));
  *wx *= h->world_width;
  *wy *= h->world_height;
}

void world_to_screen(gfx_handler_t *h, float wx, float wy, float *sx, float *sy) {
  camera_t *cam = &h->renderer.camera;
  wx /= h->world_width;
  wy /= h->world_height;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = h->world_width / h->world_height;
  float aspect = window_ratio / map_ratio;

  float max_map_size = fmaxf(h->world_width, h->world_height) * 0.001f;

  // World offset from camera center -> NDC
  float ndc_x = (wx - cam->pos[0]) * (cam->zoom * max_map_size);
  float ndc_y = (wy - cam->pos[1]) * (cam->zoom * max_map_size * aspect);

  // NDC [-1..1] to screen pixels [0..w],[0..h]
  *sx = (ndc_x + 1.0f) * 0.5f * h->viewport[0];
  *sy = (ndc_y + 1.0f) * 0.5f * h->viewport[1];
}

static void setup_vertex_descriptions(void) {
  primitive_binding_description =
      (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(primitive_vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  primitive_attribute_descriptions[0] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(primitive_vertex_t, pos)};
  primitive_attribute_descriptions[1] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(primitive_vertex_t, color)};

  primitive3d_binding_description =
      (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(primitive3d_vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  primitive3d_attribute_descriptions[0] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(primitive3d_vertex_t, pos)};
  primitive3d_attribute_descriptions[1] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(primitive3d_vertex_t, color)};
  primitive3d_attribute_descriptions[2] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(primitive3d_vertex_t, uv)};
  primitive3d_attribute_descriptions[3] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 3, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(primitive3d_vertex_t, layer)};

  mesh_binding_description = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  mesh_attribute_descriptions[0] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  mesh_attribute_descriptions[1] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vertex_t, color)};
  mesh_attribute_descriptions[2] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, tex_coord)};


  // Atlas instanced data
  uint32_t i = 0;
  atlas_binding_desc[0] = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  atlas_binding_desc[1] =
      (VkVertexInputBindingDescription){.binding = 1, .stride = sizeof(atlas_instance_t), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};

  // from vertex_t (binding 0)
  atlas_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  // from atlas_instance_t (binding 1)
  atlas_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(atlas_instance_t, pos)};
  atlas_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(atlas_instance_t, size)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 3, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(atlas_instance_t, rotation)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                                .location = 4,
                                                                .format = VK_FORMAT_R32_SINT, // Use SINT for integer index
                                                                .offset = offsetof(atlas_instance_t, sprite_index)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 5, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(atlas_instance_t, uv_scale)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 6, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(atlas_instance_t, uv_offset)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 7, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(atlas_instance_t, tiling)};
  atlas_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 8, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(atlas_instance_t, color)};
}

static void flush_primitives(gfx_handler_t *h, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &h->renderer;

  if (renderer->primitive_index_count == 0 || renderer->primitive_index_count <= renderer->primitive_index_offset_drawn) {
    return;
  }

  // Get or create the pipeline
  VkRenderPass target_pass = h->offscreen_initialized && h->offscreen_render_pass != VK_NULL_HANDLE
                                 ? h->offscreen_render_pass
                                 : h->g_main_window_data.RenderPass;
  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, renderer->primitive_shader, 1, 0, target_pass);
  if (!pso) {
    log_error(LOG_SOURCE, "Failed to get primitive pipeline");
    return;
  }

  // Setup UBO
  primitive_ubo_t ubo;
  ubo.camPos[0] = h->renderer.camera.pos[0];
  ubo.camPos[1] = h->renderer.camera.pos[1];
  ubo.zoom = h->renderer.camera.zoom;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = h->world_width / h->world_height;
  ubo.aspect = window_ratio / map_ratio;

  ubo.maxMapSize = fmaxf(h->world_width, h->world_height) * 0.001f;
  ubo.mapSize[0] = h->world_width;
  ubo.mapSize[1] = h->world_height;
  ubo.lod_bias = renderer->lod_bias;

  glm_ortho_rh_zo(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  // Allocate UBO space
  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned_size = (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);

  if (renderer->ubo_buffer_offset + aligned_size > DYNAMIC_UBO_BUFFER_SIZE) {
    log_error(LOG_SOURCE, "UBO buffer exhausted during primitive flush");
    // Stop drawing new primitives this frame but don't reset
    return;
  }

  uint32_t dynamic_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dynamic_offset, &ubo, ubo_size);
  renderer->ubo_buffer_offset += (uint32_t)aligned_size;

  // Allocate descriptor set
  uint32_t frame_pool_index = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet descriptor_set;
  VkDescriptorSetAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = renderer->frame_descriptor_pools[frame_pool_index],
      .descriptorSetCount = 1,
      .pSetLayouts = &pso->descriptor_set_layout};

  VkResult err = vkAllocateDescriptorSets(h->g_device, &alloc_info, &descriptor_set);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Failed to allocate descriptor set for primitives (err=%d)", err);
    return;
  }

  // Update descriptor set
  VkDescriptorBufferInfo buffer_info = {
      .buffer = renderer->dynamic_ubo_buffer.buffer,
      .offset = dynamic_offset,
      .range = sizeof(primitive_ubo_t)};

  VkWriteDescriptorSet descriptor_write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .pBufferInfo = &buffer_info};

  vkUpdateDescriptorSets(h->g_device, 1, &descriptor_write, 0, NULL);

  // Draw
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);

  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(command_buffer, 0, 1, &renderer->dynamic_vertex_buffer.buffer, offsets);
  vkCmdBindIndexBuffer(command_buffer, renderer->dynamic_index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);

  uint32_t count_to_draw = renderer->primitive_index_count - renderer->primitive_index_offset_drawn;
  vkCmdDrawIndexed(command_buffer, count_to_draw, 1, renderer->primitive_index_offset_drawn, 0, 0);

  // Advance the drawn offset
  renderer->primitive_index_offset_drawn = renderer->primitive_index_count;
}

static void ensure_primitive_space(gfx_handler_t *handler, uint32_t vertex_count, uint32_t index_count) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->primitive_vertex_count + vertex_count > MAX_PRIMITIVE_VERTICES ||
      renderer->primitive_index_count + index_count > MAX_PRIMITIVE_INDICES) {
    // If full, flush what we have so far
    flush_primitives(handler, renderer->current_command_buffer);

    // Check again. If still full (because we didn't reset), we are out of space for this frame.
    if (renderer->primitive_vertex_count + vertex_count > MAX_PRIMITIVE_VERTICES ||
        renderer->primitive_index_count + index_count > MAX_PRIMITIVE_INDICES) {
      log_error(LOG_SOURCE, "Primitive buffer overflow! Increase MAX_PRIMITIVE_VERTICES/INDICES.");
      // We can't safely reset here because previous draw calls depend on the data.
      // Dropping geometry is the only safe fallback without ring buffering.
    }
  }
}

void renderer_draw_rect_filled(gfx_handler_t *handler, vec2 pos, vec2 size, vec4 color) {
  ensure_primitive_space(handler, 4, 6);

  renderer_state_t *renderer = &handler->renderer;
  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  // Define vertices in world space
  vtx[0].pos[0] = pos[0];
  vtx[0].pos[1] = pos[1];
  glm_vec4_copy(color, vtx[0].color);

  vtx[1].pos[0] = pos[0] + size[0];
  vtx[1].pos[1] = pos[1];
  glm_vec4_copy(color, vtx[1].color);

  vtx[2].pos[0] = pos[0] + size[0];
  vtx[2].pos[1] = pos[1] + size[1];
  glm_vec4_copy(color, vtx[2].color);

  vtx[3].pos[0] = pos[0];
  vtx[3].pos[1] = pos[1] + size[1];
  glm_vec4_copy(color, vtx[3].color);

  // Triangle indices (two triangles)
  idx[0] = base_index + 0;
  idx[1] = base_index + 1;
  idx[2] = base_index + 2;
  idx[3] = base_index + 2;
  idx[4] = base_index + 3;
  idx[5] = base_index + 0;

  renderer->primitive_vertex_count += 4;
  renderer->primitive_index_count += 6;
}

void renderer_draw_triangle_filled(gfx_handler_t *handler, vec2 p1, vec2 p2, vec2 p3, vec4 color) {
  ensure_primitive_space(handler, 3, 3);

  renderer_state_t *renderer = &handler->renderer;
  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  const float *pts[3] = {p1, p2, p3};
  for (uint32_t i = 0; i < 3; i++) {
    vtx[i].pos[0] = pts[i][0];
    vtx[i].pos[1] = pts[i][1];
    glm_vec4_copy(color, vtx[i].color);
    idx[i] = base_index + i;
  }

  renderer->primitive_vertex_count += 3;
  renderer->primitive_index_count += 3;
}

void renderer_draw_circle_filled(gfx_handler_t *handler, vec2 center, float radius, vec4 color, uint32_t segments) {
  if (segments < 3) segments = 3;

  ensure_primitive_space(handler, segments + 1, segments * 3);

  renderer_state_t *renderer = &handler->renderer;
  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  // Center vertex
  vtx[0].pos[0] = center[0];
  vtx[0].pos[1] = center[1];
  glm_vec4_copy(color, vtx[0].color);

  // Outer vertices
  float angle_step = 2.0f * M_PI / segments;
  for (uint32_t i = 0; i < segments; i++) {
    float angle = i * angle_step;
    vtx[i + 1].pos[0] = center[0] + cosf(angle) * radius;
    vtx[i + 1].pos[1] = center[1] + sinf(angle) * radius;
    glm_vec4_copy(color, vtx[i + 1].color);
  }

  // Triangle fan indices
  for (uint32_t i = 0; i < segments; i++) {
    idx[i * 3 + 0] = base_index;
    idx[i * 3 + 1] = base_index + i + 1;
    idx[i * 3 + 2] = base_index + ((i + 1) % segments) + 1;
  }

  renderer->primitive_vertex_count += segments + 1;
  renderer->primitive_index_count += segments * 3;
}
// TODO: ensuring the width of the thing is atleast 1px is kinda expensive. think of another way to do this
void renderer_draw_line(gfx_handler_t *handler, vec2 p1, vec2 p2, vec4 color, float thickness) {
  ensure_primitive_space(handler, 4, 6);

  renderer_state_t *renderer = &handler->renderer;
  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  // Calculate perpendicular direction
  vec2 dir;
  glm_vec2_sub(p2, p1, dir);
  float len = glm_vec2_norm(dir);

  if (len < 1e-6f) {
    // Line is too short, skip
    return;
  }

  glm_vec2_scale(dir, 1.0f / len, dir);
  vec2 normal = {-dir[1], dir[0]};

  // Calculate minimum pixel thickness in world space
  const float MIN_PIXELS = 1.0f;
  float sx1, sy1, sx1n, sy1n;
  float sx2, sy2, sx2n, sy2n;

  world_to_screen(handler, p1[0], p1[1], &sx1, &sy1);
  world_to_screen(handler, p1[0] + normal[0], p1[1] + normal[1], &sx1n, &sy1n);
  world_to_screen(handler, p2[0], p2[1], &sx2, &sy2);
  world_to_screen(handler, p2[0] + normal[0], p2[1] + normal[1], &sx2n, &sy2n);

  float pix_per_unit_p1 = hypotf(sx1n - sx1, sy1n - sy1);
  float pix_per_unit_p2 = hypotf(sx2n - sx2, sy2n - sy2);

  const float EPS = 1e-6f;
  if (pix_per_unit_p1 < EPS) pix_per_unit_p1 = (pix_per_unit_p2 > EPS ? pix_per_unit_p2 : 1.0f);
  if (pix_per_unit_p2 < EPS) pix_per_unit_p2 = (pix_per_unit_p1 > EPS ? pix_per_unit_p1 : 1.0f);

  float min_world_thickness_p1 = MIN_PIXELS / pix_per_unit_p1;
  float min_world_thickness_p2 = MIN_PIXELS / pix_per_unit_p2;

  float half_t1 = fmaxf(thickness * 0.5f, min_world_thickness_p1 * 0.5f);
  float half_t2 = fmaxf(thickness * 0.5f, min_world_thickness_p2 * 0.5f);

  // Create quad vertices
  vtx[0].pos[0] = p1[0] - normal[0] * half_t1;
  vtx[0].pos[1] = p1[1] - normal[1] * half_t1;
  glm_vec4_copy(color, vtx[0].color);

  vtx[1].pos[0] = p2[0] - normal[0] * half_t2;
  vtx[1].pos[1] = p2[1] - normal[1] * half_t2;
  glm_vec4_copy(color, vtx[1].color);

  vtx[2].pos[0] = p2[0] + normal[0] * half_t2;
  vtx[2].pos[1] = p2[1] + normal[1] * half_t2;
  glm_vec4_copy(color, vtx[2].color);

  vtx[3].pos[0] = p1[0] + normal[0] * half_t1;
  vtx[3].pos[1] = p1[1] + normal[1] * half_t1;
  glm_vec4_copy(color, vtx[3].color);

  // Triangle indices
  idx[0] = base_index + 0;
  idx[1] = base_index + 1;
  idx[2] = base_index + 2;
  idx[3] = base_index + 2;
  idx[4] = base_index + 3;
  idx[5] = base_index + 0;

  renderer->primitive_vertex_count += 4;
  renderer->primitive_index_count += 6;
}







// Stage one layer of an array texture and rebuild its mip chain.
static void upload_texture_layer(gfx_handler_t *h, texture_t *array, VkFormat format, int layer, const void *src, VkDeviceSize bytes) {
  if (!array) return;
  renderer_state_t *r = &h->renderer;

  buffer_t staging;
  create_buffer(h, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging);

  void *data;
  vkMapMemory(h->g_device, staging.memory, 0, bytes, 0, &data);
  memcpy(data, src, bytes);
  vkUnmapMemory(h->g_device, staging.memory);

  transition_image_layout(h, r->transfer_command_pool, array->image, format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, array->mip_levels, layer, 1);

  VkCommandBuffer cmd = begin_single_time_commands(h, r->transfer_command_pool);
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = layer, .layerCount = 1},
      .imageExtent = {array->width, array->height, 1},
  };
  vkCmdCopyBufferToImage(cmd, staging.buffer, array->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  end_single_time_commands(h, r->transfer_command_pool, cmd);

  vkDestroyBuffer(h->g_device, staging.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, staging.memory, h->g_allocator);

  if (!build_mipmaps(h, array->image, array->width, array->height, array->mip_levels, layer, 1)) {
    transition_image_layout(h, r->transfer_command_pool, array->image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, layer, 1);
  }
}




// Slices `source_atlas` into a texture array, one sprite per layer, and gives
// the atlas renderer its instance ring. Split out from the path-based helper so
// a game module can build an atlas from pixels it produced itself.
static void renderer_build_atlas_renderer(gfx_handler_t *h, atlas_renderer_t *ar, texture_t *source_atlas, const sprite_definition_t *sprites,
                                          uint32_t sprite_count, uint32_t max_instances) {
  const char *atlas_path = source_atlas->path;
  ar->shader = renderer_load_shader(h, "data/shaders/atlas.vert.spv", "data/shaders/atlas.frag.spv");
  ar->max_instances = max_instances;

  // Describe the atlas vertex input on the shader itself. Selecting a layout by
  // comparing shader pointers only worked while the renderer owned every atlas;
  // modules create their own now, so the shader has to say what it expects.
  if (ar->shader && !ar->shader->layout) {
    static vertex_layout_t atlas_layout;
    atlas_layout.binding_count = 2;
    atlas_layout.bindings[0] = atlas_binding_desc[0];
    atlas_layout.bindings[1] = atlas_binding_desc[1];
    atlas_layout.attr_count = 9;
    for (uint32_t i = 0; i < 9; ++i) atlas_layout.attrs[i] = atlas_attrib_descs[i];
    ar->shader->layout = &atlas_layout;
  }

  ar->sprite_count = sprite_count;
  ar->sprite_definitions = malloc(sizeof(sprite_definition_t) * sprite_count);
  memcpy(ar->sprite_definitions, sprites, sizeof(sprite_definition_t) * sprite_count);

  uint32_t max_w = 0, max_h = 0;
  for (uint32_t i = 0; i < sprite_count; ++i) {
    if (sprites[i].w > max_w) max_w = sprites[i].w;
    if (sprites[i].h > max_h) max_h = sprites[i].h;
  }

  if (max_w == 0 || max_h == 0) {
    log_error(LOG_SOURCE, "Invalid sprite definitions for atlas %s, max width/height is zero.", atlas_path);
    renderer_destroy_texture(h, source_atlas);
    return;
  }

  uint32_t padding = 1;
  ar->layer_width = max_w + padding * 2;
  ar->layer_height = max_h + padding * 2;

  ar->atlas_texture = renderer_create_texture_2d_array(h, ar->layer_width, ar->layer_height, sprite_count, VK_FORMAT_R8G8B8A8_UNORM);
  if (!ar->atlas_texture) {
    log_error(LOG_SOURCE, "Failed to create texture array for atlas %s.", atlas_path);
    renderer_destroy_texture(h, source_atlas);
    return;
  }

  VkCommandBuffer cmd = begin_single_time_commands(h, h->renderer.transfer_command_pool);

  transition_image_layout(h, h->renderer.transfer_command_pool, source_atlas->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_atlas->mip_levels, 0, 1);
  transition_image_layout(h, h->renderer.transfer_command_pool, ar->atlas_texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ar->atlas_texture->mip_levels, 0,
                          ar->atlas_texture->layer_count);

  VkClearColorValue clearVal = {{0.0f, 0.0f, 0.0f, 0.0f}};
  VkImageSubresourceRange clearRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = sprite_count};
  vkCmdClearColorImage(cmd, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &clearRange);

  VkImageMemoryBarrier clear_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .image = ar->atlas_texture->image,
                                        .subresourceRange = clearRange};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &clear_barrier);

  for (uint32_t i = 0; i < sprite_count; ++i) {
    const sprite_definition_t *sprite = &sprites[i];
    VkImageBlit center = {
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .srcOffsets[0] = {(int32_t)sprite->x, (int32_t)sprite->y, 0},
        .srcOffsets[1] = {(int32_t)(sprite->x + sprite->w), (int32_t)(sprite->y + sprite->h), 1},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = i, .layerCount = 1},
        .dstOffsets[0] = {(int32_t)padding, (int32_t)padding, 0},
        .dstOffsets[1] = {(int32_t)(padding + sprite->w), (int32_t)(padding + sprite->h), 1},
    };
    vkCmdBlitImage(cmd, source_atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &center, VK_FILTER_NEAREST);

    // Top Edge
    VkImageBlit top = center;
    top.srcOffsets[1].y = top.srcOffsets[0].y + 1;
    top.dstOffsets[0].y = 0;
    top.dstOffsets[1].y = padding;
    vkCmdBlitImage(cmd, source_atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &top, VK_FILTER_NEAREST);

    // Bottom Edge
    VkImageBlit bottom = center;
    bottom.srcOffsets[0].y = center.srcOffsets[1].y - 1;
    bottom.dstOffsets[0].y = padding + sprite->h;
    bottom.dstOffsets[1].y = padding + sprite->h + padding;
    vkCmdBlitImage(cmd, source_atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &bottom, VK_FILTER_NEAREST);

    // Left Edge
    VkImageBlit left = center;
    left.srcOffsets[1].x = left.srcOffsets[0].x + 1;
    left.dstOffsets[0].x = 0;
    left.dstOffsets[1].x = padding;
    vkCmdBlitImage(cmd, source_atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &left, VK_FILTER_NEAREST);

    // Right Edge
    VkImageBlit right = center;
    right.srcOffsets[0].x = center.srcOffsets[1].x - 1;
    right.dstOffsets[0].x = padding + sprite->w;
    right.dstOffsets[1].x = padding + sprite->w + padding;
    vkCmdBlitImage(cmd, source_atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ar->atlas_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &right, VK_FILTER_NEAREST);
  }
  end_single_time_commands(h, h->renderer.transfer_command_pool, cmd);

  transition_image_layout(h, h->renderer.transfer_command_pool, source_atlas->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, source_atlas->mip_levels, 0, 1);

  if (!build_mipmaps(h, ar->atlas_texture->image, ar->layer_width, ar->layer_height, ar->atlas_texture->mip_levels, 0,
                     ar->atlas_texture->layer_count)) {
    transition_image_layout(h, h->renderer.transfer_command_pool, ar->atlas_texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ar->atlas_texture->mip_levels, 0,
                            ar->atlas_texture->layer_count);
  }

  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .mipLodBias = 0.0f,
                                      .anisotropyEnable = VK_FALSE,
                                      .maxAnisotropy = 1.0f,
                                      .compareEnable = VK_FALSE,
                                      .compareOp = VK_COMPARE_OP_ALWAYS,
                                      .minLod = 0.0f,
                                      .maxLod = (float)ar->atlas_texture->mip_levels,
                                      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                      .unnormalizedCoordinates = VK_FALSE};
  check_vk_result(vkCreateSampler(h->g_device, &sampler_info, h->g_allocator, &ar->sampler));

  create_buffer(h, sizeof(atlas_instance_t) * ar->max_instances, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ar->instance_buffer);
  vkMapMemory(h->g_device, ar->instance_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&ar->instance_ptr);
  ar->instance_count = 0;
}

void renderer_cleanup_atlas_renderer(gfx_handler_t *h, atlas_renderer_t *ar) {
  if (ar->sampler) {
    vkDestroySampler(h->g_device, ar->sampler, h->g_allocator);
  }
  if (ar->instance_buffer.buffer) {
    vkDestroyBuffer(h->g_device, ar->instance_buffer.buffer, h->g_allocator);
    vkFreeMemory(h->g_device, ar->instance_buffer.memory, h->g_allocator);
  }
  if (ar->sprite_definitions) {
    free(ar->sprite_definitions);
    ar->sprite_definitions = NULL;
  }
  // The atlas texture itself will be cleaned up by the main renderer_cleanup loop
}

void renderer_begin_atlas_instances(atlas_renderer_t *ar) { ar->instance_count = 0; }

// The projection every world-space technique shares. World coordinates are
// normalized by the playfield extent the active game published, so this is the
// only place the renderer has to think about how big a level is.
static primitive_ubo_t world_ubo(gfx_handler_t *h) {
  renderer_state_t *renderer = &h->renderer;
  primitive_ubo_t ubo;
  ubo.camPos[0] = renderer->camera.pos[0];
  ubo.camPos[1] = renderer->camera.pos[1];
  ubo.zoom = renderer->camera.zoom;
  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = h->world_width / h->world_height;
  ubo.aspect = window_ratio / map_ratio;
  ubo.maxMapSize = fmaxf(h->world_width, h->world_height) * 0.001f;
  ubo.mapSize[0] = h->world_width;
  ubo.mapSize[1] = h->world_height;
  ubo.lod_bias = renderer->lod_bias;
  glm_ortho_rh_zo(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);
  renderer_camera3_view_proj(h, ubo.view_proj);
  return ubo;
}


// --- 3D primitives -----------------------------------------------------------
//
// A 3D game's geometry is resolved by the depth buffer, so unlike the 2D path
// there is no command queue and no sorting: vertices go straight into a stream
// that is drawn once per frame.


// The direction from the target out to an orbiting eye. The camera looks back
// along it, which is what makes yaw and pitch mean the same thing in both modes.
static void camera3_offset(const camera3_t *c, vec3 out) {
  const float cp = cosf(c->pitch);
  out[0] = cp * cosf(c->yaw);
  out[1] = sinf(c->pitch);
  out[2] = cp * sinf(c->yaw);
}

void renderer_camera3_eye(gfx_handler_t *h, vec3 out) {
  const camera3_t *c = &h->renderer.camera3;
  if (c->free_mode) {
    glm_vec3_copy((float *)c->eye, out);
    return;
  }
  vec3 offset;
  camera3_offset(c, offset);
  glm_vec3_scale(offset, c->distance, offset);
  glm_vec3_add((float *)c->target, offset, out);
}

void renderer_camera3_forward(gfx_handler_t *h, vec3 out) {
  vec3 offset;
  camera3_offset(&h->renderer.camera3, offset);
  glm_vec3_negate_to(offset, out);
  glm_vec3_normalize(out);
}

void renderer_camera3_toggle_free(gfx_handler_t *h) {
  camera3_t *c = &h->renderer.camera3;
  if (!c->free_mode) {
    // Entering the freecam: stand where the orbit already is.
    renderer_camera3_eye(h, c->eye);
    c->free_mode = true;
    return;
  }
  // Leaving it: orbit whatever is currently in front, at the current distance,
  // so the view is unchanged at the instant of the swap.
  vec3 forward;
  renderer_camera3_forward(h, forward);
  glm_vec3_scale(forward, c->distance, forward);
  glm_vec3_add(c->eye, forward, c->target);
  c->free_mode = false;
}

void renderer_camera3_view_proj(gfx_handler_t *h, mat4 out) {
  const camera3_t *c = &h->renderer.camera3;
  vec3 eye;
  renderer_camera3_eye(h, eye);

  vec3 target;
  if (c->free_mode) {
    vec3 forward;
    renderer_camera3_forward(h, forward);
    glm_vec3_add(eye, forward, target);
  } else {
    glm_vec3_copy((float *)c->target, target);
  }

  mat4 view, proj;
  glm_lookat(eye, target, (vec3){0.f, 1.f, 0.f}, view);

  const float aspect = h->viewport[1] > 0.f ? h->viewport[0] / h->viewport[1] : 1.f;
  glm_perspective(c->fov_y, aspect, c->near_z, c->far_z, proj);
  // Vulkan's clip space has Y pointing down relative to OpenGL's, which is what
  // cglm builds for.
  proj[1][1] *= -1.f;
  glm_mat4_mul(proj, view, out);
}

// A layer this negative is never a real page, and the shader reads it as "no
// texture" rather than clamping into page zero.
#define PRIMITIVE3D_NO_TEXTURE (-1.f)

static void push_vertex3(renderer_state_t *r, vec3 pos, vec4 color, vec2 uv, float layer) {
  if (r->primitive3d_vertex_count >= MAX_PRIMITIVE3D_VERTICES || !r->vertex3d_buffer_ptr) {
    // Dropping geometry without saying so looks like a bug in the game rather
    // than a limit in the renderer, so say it once.
    static bool reported = false;
    if (r->vertex3d_buffer_ptr && !reported) {
      reported = true;
      log_warn("Renderer", "3D vertex buffer full at %d vertices; geometry past this is not drawn",
               MAX_PRIMITIVE3D_VERTICES);
    }
    return;
  }
  primitive3d_vertex_t *v = &r->vertex3d_buffer_ptr[r->primitive3d_vertex_count++];
  glm_vec3_copy(pos, v->pos);
  glm_vec4_copy(color, v->color);
  glm_vec2_copy(uv, v->uv);
  v->layer = layer;
}

void renderer_submit_triangle3(gfx_handler_t *h, vec3 a, vec3 b, vec3 c, vec4 color) {
  renderer_state_t *r = &h->renderer;
  vec2 no_uv = {0.f, 0.f};
  push_vertex3(r, a, color, no_uv, PRIMITIVE3D_NO_TEXTURE);
  push_vertex3(r, b, color, no_uv, PRIMITIVE3D_NO_TEXTURE);
  push_vertex3(r, c, color, no_uv, PRIMITIVE3D_NO_TEXTURE);
}

void renderer_submit_triangle3_textured(gfx_handler_t *h, vec3 a, vec3 b, vec3 c, vec2 uv_a, vec2 uv_b, vec2 uv_c,
                                        uint32_t layer, vec4 tint) {
  renderer_state_t *r = &h->renderer;
  // Without an array bound there is nothing to sample, and a page index into
  // nothing would read as garbage; the tint alone is the honest fallback.
  const texture_t *bound = r->primitive3d_texture;
  const float page = bound && layer < bound->layer_count ? (float)layer : PRIMITIVE3D_NO_TEXTURE;
  push_vertex3(r, a, tint, uv_a, page);
  push_vertex3(r, b, tint, uv_b, page);
  push_vertex3(r, c, tint, uv_c, page);
}

void renderer_set_texture3(gfx_handler_t *h, texture_t *texture) { h->renderer.primitive3d_texture = texture; }

void renderer_submit_line3(gfx_handler_t *h, vec3 a, vec3 b, vec4 color, float thickness) {
  // A line with width is a quad turned to face the viewer: there is no line
  // width to rely on without the wideLines feature, and a camera-facing quad
  // reads the same from every angle.
  vec3 eye;
  renderer_camera3_eye(h, eye);

  vec3 dir, to_eye, side;
  glm_vec3_sub(b, a, dir);
  if (glm_vec3_norm(dir) <= 1e-6f) return;
  glm_vec3_sub(eye, a, to_eye);
  glm_vec3_cross(dir, to_eye, side);
  if (glm_vec3_norm(side) <= 1e-6f) glm_vec3_cross(dir, (vec3){0.f, 1.f, 0.f}, side);
  glm_vec3_normalize(side);
  glm_vec3_scale(side, thickness * 0.5f, side);

  vec3 a0, a1, b0, b1;
  glm_vec3_add(a, side, a0);
  glm_vec3_sub(a, side, a1);
  glm_vec3_add(b, side, b0);
  glm_vec3_sub(b, side, b1);

  renderer_submit_triangle3(h, a0, a1, b0, color);
  renderer_submit_triangle3(h, b0, a1, b1, color);
}

void renderer_submit_box3(gfx_handler_t *h, vec3 center, vec3 size, vec4 color, bool wire) {
  const float hx = size[0] * 0.5f, hy = size[1] * 0.5f, hz = size[2] * 0.5f;
  vec3 corner[8];
  for (int i = 0; i < 8; ++i) {
    corner[i][0] = center[0] + ((i & 1) ? hx : -hx);
    corner[i][1] = center[1] + ((i & 2) ? hy : -hy);
    corner[i][2] = center[2] + ((i & 4) ? hz : -hz);
  }

  if (wire) {
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const float thickness = fmaxf(fmaxf(size[0], size[1]), size[2]) * 0.01f;
    for (int i = 0; i < 12; ++i)
      renderer_submit_line3(h, corner[edges[i][0]], corner[edges[i][1]], color, thickness > 0.f ? thickness : 0.02f);
    return;
  }

  // Two triangles per face, wound so the box is solid from any side. Faces are
  // shaded slightly apart so a solid box reads as a volume rather than a
  // silhouette without any lighting in the pipeline.
  static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  static const float shade[6] = {0.75f, 0.95f, 0.6f, 1.0f, 0.7f, 0.85f};
  for (int f = 0; f < 6; ++f) {
    vec4 tinted = {color[0] * shade[f], color[1] * shade[f], color[2] * shade[f], color[3]};
    renderer_submit_triangle3(h, corner[faces[f][0]], corner[faces[f][1]], corner[faces[f][2]], tinted);
    renderer_submit_triangle3(h, corner[faces[f][0]], corner[faces[f][2]], corner[faces[f][3]], tinted);
  }
}

static void flush_primitives3d(gfx_handler_t *h, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &h->renderer;
  if (renderer->primitive3d_vertex_count == 0 || !renderer->primitive3d_shader) return;

  VkRenderPass target_pass = h->offscreen_initialized && h->offscreen_render_pass != VK_NULL_HANDLE
                                 ? h->offscreen_render_pass
                                 : h->g_main_window_data.RenderPass;
  // The fragment stage always samples an array, so one is always bound: the
  // game's when it set one, a single white page otherwise. Leaving the binding
  // empty is undefined behaviour, not a shortcut.
  texture_t *bound = renderer->primitive3d_texture;
  if (!bound || !bound->active || bound->image_view == VK_NULL_HANDLE) bound = renderer->primitive3d_fallback_texture;
  if (!bound) return;

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, renderer->primitive3d_shader, 1, 1, target_pass);
  if (!pso) return;

  primitive_ubo_t ubo = world_ubo(h);

  VkDeviceSize aligned = (sizeof(ubo) + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  if (renderer->ubo_buffer_offset + aligned > DYNAMIC_UBO_BUFFER_SIZE) return;
  uint32_t dyn_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dyn_offset, &ubo, sizeof(ubo));
  renderer->ubo_buffer_offset += (uint32_t)aligned;

  uint32_t pool_idx = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet desc;
  VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                    .descriptorPool = renderer->frame_descriptor_pools[pool_idx],
                                    .descriptorSetCount = 1,
                                    .pSetLayouts = &pso->descriptor_set_layout};
  if (vkAllocateDescriptorSets(h->g_device, &ai, &desc) != VK_SUCCESS) return;

  VkDescriptorBufferInfo b_info = {.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = dyn_offset, .range = sizeof(primitive_ubo_t)};
  VkDescriptorImageInfo i_info = {.sampler = renderer->primitive3d_sampler ? renderer->primitive3d_sampler : bound->sampler,
                                  .imageView = bound->image_view,
                                  .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .descriptorCount = 1,
                                     .pBufferInfo = &b_info},
                                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .pImageInfo = &i_info}};
  vkUpdateDescriptorSets(h->g_device, 2, writes, 0, NULL);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 0, NULL);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(command_buffer, 0, 1, &renderer->dynamic_vertex_buffer3d.buffer, &offset);
  vkCmdDraw(command_buffer, renderer->primitive3d_vertex_count, 1, 0, 0);
}

void renderer_flush_atlas_instances(gfx_handler_t *h, VkCommandBuffer cmd, atlas_renderer_t *ar, uint32_t start_index, uint32_t count, bool screen_space) {
  renderer_state_t *renderer = &h->renderer;
  if (count == 0 || !ar->shader || !ar->atlas_texture) return;

  mesh_t *quad = h->quad_mesh;
  VkRenderPass target_pass = h->offscreen_initialized && h->offscreen_render_pass != VK_NULL_HANDLE
                                 ? h->offscreen_render_pass
                                 : h->g_main_window_data.RenderPass;
  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, ar->shader, 1, 1, target_pass);
  if (!pso) return;

  primitive_ubo_t ubo;
  if (screen_space) {
    ubo.camPos[0] = 0.5f;
    ubo.camPos[1] = 0.5f;
    ubo.zoom = 2.0f;
    ubo.aspect = 1.0f;
    ubo.maxMapSize = 1.0f;
    ubo.mapSize[0] = (float)h->viewport[0];
    ubo.mapSize[1] = (float)h->viewport[1];
    ubo.lod_bias = 0.0f;
    glm_ortho_rh_zo(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);
  } else {
    ubo = world_ubo(h);
  }

  VkDeviceSize aligned = (sizeof(ubo) + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  if (renderer->ubo_buffer_offset + aligned > DYNAMIC_UBO_BUFFER_SIZE) {
    log_error(LOG_SOURCE, "UBO Ring Buffer Exhausted");
    return;
  }
  uint32_t dyn_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dyn_offset, &ubo, sizeof(ubo));
  renderer->ubo_buffer_offset += (uint32_t)aligned;

  uint32_t pool_idx = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet desc;
  VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = renderer->frame_descriptor_pools[pool_idx],
      .descriptorSetCount = 1,
      .pSetLayouts = &pso->descriptor_set_layout};

  if (vkAllocateDescriptorSets(h->g_device, &ai, &desc) != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Descriptor allocation failed in atlas flush");
    return;
  }

  VkDescriptorBufferInfo b_info = {.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = dyn_offset, .range = sizeof(primitive_ubo_t)};
  VkDescriptorImageInfo i_info = {.sampler = ar->sampler, .imageView = ar->atlas_texture->image_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  VkWriteDescriptorSet writes[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = desc, .dstBinding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .pBufferInfo = &b_info},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = desc, .dstBinding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &i_info}};
  vkUpdateDescriptorSets(h->g_device, 2, writes, 0, NULL);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);

  // Calculate the offset into the instance buffer for this specific batch
  VkDeviceSize instance_offset = (VkDeviceSize)start_index * sizeof(atlas_instance_t);

  VkBuffer bufs[2] = {quad->vertex_buffer.buffer, ar->instance_buffer.buffer};
  VkDeviceSize offs[2] = {0, instance_offset}; // Bind at the correct memory location

  vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
  vkCmdBindIndexBuffer(cmd, quad->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 0, NULL);

  vkCmdDrawIndexed(cmd, quad->index_count, count, 0, 0, 0);

  // NOTE: We do NOT reset ar->instance_count = 0 here.
  // It is reset globally once at the start of renderer_flush_queue.
}

static int compare_render_commands(const void *a, const void *b) {
  const render_command_t *cmd_a = (const render_command_t *)a;
  const render_command_t *cmd_b = (const render_command_t *)b;
  if (cmd_a->z < cmd_b->z) return -1;
  if (cmd_a->z > cmd_b->z) return 1;

  if (cmd_a->type < cmd_b->type) return -1;
  if (cmd_a->type > cmd_b->type) return 1;

  if (cmd_a->type == RENDER_CMD_ATLAS_BATCH) {
    if (cmd_a->data.atlas_batch.ar < cmd_b->data.atlas_batch.ar) return -1;
    if (cmd_a->data.atlas_batch.ar > cmd_b->data.atlas_batch.ar) return 1;
    if (cmd_a->data.atlas_batch.screen_space != cmd_b->data.atlas_batch.screen_space) {
      return cmd_a->data.atlas_batch.screen_space ? 1 : -1;
    }
  }

  // Everything above can compare equal, and qsort is not stable, so without this the relative order
  // of overlapping sprites is free to change from one frame to the next.
  if (cmd_a->seq < cmd_b->seq) return -1;
  if (cmd_a->seq > cmd_b->seq) return 1;
  return 0;
}




void renderer_submit_rect_filled(struct gfx_handler_t *h, float z, vec2 pos, vec2 size, vec4 color) {
  if (h->renderer.queue.count >= MAX_RENDER_COMMANDS) return;
  render_command_t *cmd = &h->renderer.queue.commands[h->renderer.queue.count++];
  cmd->type = RENDER_CMD_RECT_FILLED;
  cmd->z = z;
  glm_vec2_copy(pos, cmd->data.prim.p1);
  glm_vec2_copy(size, cmd->data.prim.p2);
  glm_vec4_copy(color, cmd->data.prim.color);
}

void renderer_submit_circle_filled(struct gfx_handler_t *h, float z, vec2 center, float radius, vec4 color, uint32_t segments) {
  if (h->renderer.queue.count >= MAX_RENDER_COMMANDS) return;
  render_command_t *cmd = &h->renderer.queue.commands[h->renderer.queue.count++];
  cmd->type = RENDER_CMD_CIRCLE_FILLED;
  cmd->z = z;
  glm_vec2_copy(center, cmd->data.prim.p1);
  cmd->data.prim.thickness = radius; // reuse thickness for radius
  cmd->data.prim.segments = segments;
  glm_vec4_copy(color, cmd->data.prim.color);
}

void renderer_submit_triangle_filled(struct gfx_handler_t *h, float z, vec2 p1, vec2 p2, vec2 p3, vec4 color) {
  if (h->renderer.queue.count >= MAX_RENDER_COMMANDS) return;
  render_command_t *cmd = &h->renderer.queue.commands[h->renderer.queue.count++];
  cmd->type = RENDER_CMD_TRIANGLE_FILLED;
  cmd->z = z;
  glm_vec2_copy(p1, cmd->data.prim.p1);
  glm_vec2_copy(p2, cmd->data.prim.p2);
  glm_vec2_copy(p3, cmd->data.prim.p3);
  glm_vec4_copy(color, cmd->data.prim.color);
}

void renderer_submit_line(struct gfx_handler_t *h, float z, vec2 p1, vec2 p2, vec4 color, float thickness) {
  if (h->renderer.queue.count >= MAX_RENDER_COMMANDS) return;
  render_command_t *cmd = &h->renderer.queue.commands[h->renderer.queue.count++];
  cmd->type = RENDER_CMD_LINE;
  cmd->z = z;
  glm_vec2_copy(p1, cmd->data.prim.p1);
  glm_vec2_copy(p2, cmd->data.prim.p2);
  cmd->data.prim.thickness = thickness;
  glm_vec4_copy(color, cmd->data.prim.color);
}

void renderer_submit_line_batch(struct gfx_handler_t *h, float z, const line_segment_t *segments, uint32_t count) {
  renderer_state_t *renderer = &h->renderer;
  if (!segments || count == 0 || renderer->queue.count >= MAX_RENDER_COMMANDS) return;
  const size_t size = sizeof(*segments) * (size_t)count;
  if (size > renderer->transient_capacity - renderer->transient_offset) {
    log_error(LOG_SOURCE, "Transient memory exhausted while queuing %u line segments.", count);
    return;
  }
  line_segment_t *copy = (line_segment_t *)(renderer->transient_memory + renderer->transient_offset);
  memcpy(copy, segments, size);
  renderer->transient_offset += size;

  render_command_t *cmd = &renderer->queue.commands[renderer->queue.count++];
  cmd->type = RENDER_CMD_LINE_BATCH;
  cmd->z = z;
  cmd->data.line_batch.segments = copy;
  cmd->data.line_batch.count = count;
}

void renderer_calculate_atlas_uvs(atlas_renderer_t *ar, uint32_t sprite_index, atlas_instance_t *out_inst) {
  if (sprite_index >= ar->sprite_count) return;
  float layer_w = (float)ar->layer_width;
  float layer_h = (float)ar->layer_height;
  float sprite_w = (float)ar->sprite_definitions[sprite_index].w;
  float sprite_h = (float)ar->sprite_definitions[sprite_index].h;
  // A borrowed image has no padding gutter around it: the sprite is the image.
  float padding = ar->aliased ? 0.0f : 1.0f;

  out_inst->uv_scale[0] = sprite_w / layer_w;
  out_inst->uv_scale[1] = sprite_h / layer_h;
  out_inst->uv_offset[0] = padding / layer_w;
  out_inst->uv_offset[1] = padding / layer_h;
}

void renderer_submit_atlas_batch(struct gfx_handler_t *h, struct atlas_renderer_t *ar, float z, const atlas_instance_t *instances,
                                 uint32_t count, bool screen_space) {
  if (h->renderer.queue.count >= MAX_RENDER_COMMANDS) return;
  if (count == 0) return;

  // Allocate from transient memory
  size_t size = count * sizeof(atlas_instance_t);
  if (h->renderer.transient_offset + size > h->renderer.transient_capacity) {
    log_error(LOG_SOURCE, "Transient memory exhausted! Cannot submit atlas batch.");
    return;
  }

  void *dest = h->renderer.transient_memory + h->renderer.transient_offset;
  memcpy(dest, instances, size);
  h->renderer.transient_offset += size;

  render_command_t *cmd = &h->renderer.queue.commands[h->renderer.queue.count++];
  cmd->type = RENDER_CMD_ATLAS_BATCH;
  cmd->z = z;
  cmd->data.atlas_batch.ar = ar;
  cmd->data.atlas_batch.instances = (atlas_instance_t *)dest;
  cmd->data.atlas_batch.count = count;
  cmd->data.atlas_batch.screen_space = screen_space;
}

void renderer_flush_queue(struct gfx_handler_t *h, VkCommandBuffer cmd) {
  struct renderer_state_t *r = &h->renderer;
  if (r->queue.count == 0) return;

  // Stamp submission order first: the array index before sorting is exactly the order the commands
  // were queued in, and the comparator uses it to break ties.
  for (uint32_t i = 0; i < r->queue.count; ++i)
    r->queue.commands[i].seq = i;

  // Sort by Z-order
  qsort(r->queue.commands, r->queue.count, sizeof(render_command_t), compare_render_commands);

  // Reset all instance counters
  for (uint32_t i = 0; i < r->dynamic_atlas_count; ++i) r->dynamic_atlases[i]->instance_count = 0;
  for (uint32_t i = 0; i < r->custom_pipeline_count; ++i) r->custom_pipelines[i].instance_count = 0;

  struct atlas_renderer_t *active_ar = NULL;
  bool ar_screen_space = false;
  uint32_t batch_start_idx = 0;

  for (uint32_t i = 0; i < r->queue.count; i++) {
    render_command_t *q = &r->queue.commands[i];

    // Check if we need to flush atlas buffer
    bool is_atlas = q->type == RENDER_CMD_ATLAS_BATCH;
    if (active_ar != NULL) {
      bool flush = !is_atlas;
      if (is_atlas) {
        atlas_renderer_t *next_ar = q->data.atlas_batch.ar;
        bool next_ss = q->data.atlas_batch.screen_space;
        if (next_ar != active_ar || next_ss != ar_screen_space) flush = true;
      }

      if (flush) {
        uint32_t count = active_ar->instance_count - batch_start_idx;
        renderer_flush_atlas_instances(h, cmd, active_ar, batch_start_idx, count, ar_screen_space);
        active_ar = NULL;
      }
    }


    // Flush primitives if switching to non-primitive
    if (q->type != RENDER_CMD_RECT_FILLED && q->type != RENDER_CMD_CIRCLE_FILLED && q->type != RENDER_CMD_TRIANGLE_FILLED &&
        q->type != RENDER_CMD_LINE && q->type != RENDER_CMD_LINE_BATCH &&
        r->primitive_index_count > r->primitive_index_offset_drawn) {
      flush_primitives(h, cmd);
    }

    switch (q->type) {


    case RENDER_CMD_ATLAS_BATCH:
      if (active_ar == NULL) {
        active_ar = q->data.atlas_batch.ar;
        ar_screen_space = q->data.atlas_batch.screen_space;
        batch_start_idx = active_ar->instance_count;
      }

      // Check for space
      if (active_ar->instance_count + q->data.atlas_batch.count > active_ar->max_instances) {
        // Flush current batch
        uint32_t count = active_ar->instance_count - batch_start_idx;
        renderer_flush_atlas_instances(h, cmd, active_ar, batch_start_idx, count, ar_screen_space);
        batch_start_idx = active_ar->instance_count;

        // If still no space, we can't draw this batch.
        if (active_ar->instance_count + q->data.atlas_batch.count > active_ar->max_instances) {
          log_error(LOG_SOURCE, "Atlas batch too large for buffer! (Req: %d, Max: %d, Cur: %d)",
                    q->data.atlas_batch.count, active_ar->max_instances, active_ar->instance_count);
          break;
        }
      }

      memcpy(&active_ar->instance_ptr[active_ar->instance_count], q->data.atlas_batch.instances,
             q->data.atlas_batch.count * sizeof(atlas_instance_t));
      active_ar->instance_count += q->data.atlas_batch.count;
      break;

    case RENDER_CMD_RECT_FILLED:
      // log_info(LOG_SOURCE, "Processing RECT_FILLED command");
      renderer_draw_rect_filled(h, q->data.prim.p1, q->data.prim.p2, q->data.prim.color);
      break;

    case RENDER_CMD_CIRCLE_FILLED:
      renderer_draw_circle_filled(h, q->data.prim.p1, q->data.prim.thickness, q->data.prim.color, q->data.prim.segments);
      break;

    case RENDER_CMD_TRIANGLE_FILLED:
      renderer_draw_triangle_filled(h, q->data.prim.p1, q->data.prim.p2, q->data.prim.p3, q->data.prim.color);
      break;

    case RENDER_CMD_LINE:
      renderer_draw_line(h, q->data.prim.p1, q->data.prim.p2, q->data.prim.color, q->data.prim.thickness);
      break;

    case RENDER_CMD_LINE_BATCH:
      for (uint32_t segment = 0; segment < q->data.line_batch.count; ++segment) {
        line_segment_t *line = &q->data.line_batch.segments[segment];
        renderer_draw_line(h, line->p1, line->p2, line->color, line->thickness);
      }
      break;

    case RENDER_CMD_INSTANCES:
      // A module technique breaks the primitive batch the same way a mesh does,
      // otherwise its instances would land behind primitives queued before it.
      if (r->primitive_index_count > 0) flush_primitives(h, cmd);
      renderer_flush_custom_instances(h, cmd, q);
      break;

    case RENDER_CMD_MESH: {
      if (r->primitive_index_count > 0) flush_primitives(h, cmd);
      custom_pipeline_t *mesh_pipe = q->data.mesh_draw.pipeline;
      if (mesh_pipe && mesh_pipe->shader) {
        void *ubos[1] = {(void *)q->data.mesh_draw.uniforms};
        VkDeviceSize ubo_sizes[1] = {q->data.mesh_draw.uniform_size};
        const bool has_uniforms = q->data.mesh_draw.uniform_size > 0;
        renderer_draw_mesh(h, cmd, q->data.mesh_draw.mesh, mesh_pipe->shader, (texture_t **)q->data.mesh_draw.textures,
                           q->data.mesh_draw.texture_count, has_uniforms ? ubos : NULL, has_uniforms ? ubo_sizes : NULL,
                           has_uniforms ? 1 : 0);
      }
      break;
    }
    }
  }

  // Final flushes
  if (active_ar != NULL) {
    uint32_t count = active_ar->instance_count - batch_start_idx;
    renderer_flush_atlas_instances(h, cmd, active_ar, batch_start_idx, count, ar_screen_space);
  }


  if (r->primitive_index_count > 0) {
    flush_primitives(h, cmd);
  }

  r->queue.count = 0;
}

// =============================================================================
// Resources created by game modules
// =============================================================================
//
// Everything below exists so the active game can own how it looks. The engine
// supplies batching, sorting and the graphics API; the game supplies pixels,
// shaders and instance data. Nothing here knows what is being drawn.

static uint32_t format_bytes_per_pixel(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8G8_UNORM: return 2;
  case VK_FORMAT_R8G8B8A8_UNORM:
  default: return 4;
  }
}

texture_t *renderer_create_texture_layered(gfx_handler_t *h, const unsigned char *pixels, uint32_t width, uint32_t height, uint32_t layers,
                                           VkFormat format, bool mipmaps, bool linear_filter) {
  if (layers <= 1 && format == VK_FORMAT_R8G8B8A8_UNORM && !pixels) {
    renderer_state_t *renderer = &h->renderer;
    uint32_t free_slot = UINT32_MAX;
    for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
      if (!renderer->textures[i].active) {
        free_slot = i;
        break;
      }
    }
    if (free_slot == UINT32_MAX) return NULL;

    texture_t *texture = &renderer->textures[free_slot];
    memset(texture, 0, sizeof(*texture));
    texture->id = free_slot;
    texture->active = true;
    texture->width = width;
    texture->height = height;
    texture->mip_levels = 1;
    texture->layer_count = 1;
    texture->format = format;
    snprintf(texture->path, sizeof(texture->path), "empty_runtime_texture");
    create_image(h, width, height, 1, 1, format, VK_IMAGE_TILING_OPTIMAL,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
    transition_image_layout(h, renderer->transfer_command_pool, texture->image, format, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 0, 1);
    texture->image_view = create_image_view(h, texture->image, format, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
    texture->sampler = create_texture_sampler(h, 1, linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    return texture;
  }
  if (layers <= 1 && format == VK_FORMAT_R8G8B8A8_UNORM) {
    g_next_texture_filter = linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    texture_t *tex = renderer_create_texture_from_rgba(h, pixels, (int)width, (int)height);
    g_next_texture_filter = VK_FILTER_LINEAR;
    return tex;
  }

  texture_t *tex = renderer_create_texture_2d_array(h, width, height, layers, format);
  if (!tex || !pixels) return tex;

  const VkDeviceSize layer_bytes = (VkDeviceSize)width * height * format_bytes_per_pixel(format);
  const VkDeviceSize total = layer_bytes * layers;

  buffer_t staging = {0};
  create_buffer(h, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging);
  void *mapped = NULL;
  vkMapMemory(h->g_device, staging.memory, 0, total, 0, &mapped);
  memcpy(mapped, pixels, (size_t)total);
  vkUnmapMemory(h->g_device, staging.memory);

  transition_image_layout(h, h->renderer.transfer_command_pool, tex->image, format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tex->mip_levels, 0, layers);

  VkCommandBuffer cmd = begin_single_time_commands(h, h->renderer.transfer_command_pool);
  VkBufferImageCopy copy = {.bufferOffset = 0,
                            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = layers},
                            .imageExtent = {width, height, 1}};
  vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  end_single_time_commands(h, h->renderer.transfer_command_pool, cmd);

  if (!mipmaps || !build_mipmaps(h, tex->image, width, height, tex->mip_levels, 0, layers)) {
    transition_image_layout(h, h->renderer.transfer_command_pool, tex->image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tex->mip_levels, 0, layers);
  }

  vkDestroyBuffer(h->g_device, staging.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, staging.memory, h->g_allocator);
  return tex;
}

atlas_renderer_t *renderer_create_atlas(gfx_handler_t *h, texture_t *source, const sprite_definition_t *sprites, uint32_t sprite_count,
                                        uint32_t max_instances) {
  renderer_state_t *r = &h->renderer;
  if (!source || !sprites || sprite_count == 0) return NULL;
  if (r->dynamic_atlas_count >= MAX_DYNAMIC_ATLASES) {
    log_error(LOG_SOURCE, "Dynamic atlas limit (%d) reached.", MAX_DYNAMIC_ATLASES);
    return NULL;
  }

  atlas_renderer_t *ar = calloc(1, sizeof(atlas_renderer_t));
  if (!ar) return NULL;

  renderer_lock();
  renderer_build_atlas_renderer(h, ar, source, sprites, sprite_count, max_instances ? max_instances : MAX_ATLAS_INSTANCES);
  r->dynamic_atlases[r->dynamic_atlas_count++] = ar;
  renderer_unlock();
  return ar;
}

atlas_renderer_t *renderer_create_texture_atlas(gfx_handler_t *h, texture_t *src) {
  renderer_state_t *r = &h->renderer;
  if (!src || !src->active || src->image == VK_NULL_HANDLE) return NULL;
  if (r->dynamic_atlas_count >= MAX_DYNAMIC_ATLASES) {
    log_error(LOG_SOURCE, "Dynamic atlas limit (%d) reached.", MAX_DYNAMIC_ATLASES);
    return NULL;
  }

  atlas_renderer_t *ar = calloc(1, sizeof(atlas_renderer_t));
  if (!ar) return NULL;

  renderer_lock();

  // The atlas shader samples a 2D array, so the borrowed image gets a second
  // view of that type. A single-layer 2D image is a legal one-layer array, so
  // this costs a view rather than a copy.
  texture_t *view = calloc(1, sizeof(texture_t));
  if (!view) {
    renderer_unlock();
    free(ar);
    return NULL;
  }
  *view = *src;
  view->alias_atlas = NULL;
  view->image_view = create_image_view(h, src->image, src->format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1);
  view->layer_count = 1;
  view->mip_levels = 1;
  // The sampler the draw path binds lives on the atlas, not on the texture.
  // Clamped rather than repeating: a whole image drawn as one quad has nothing
  // to tile, and clamping keeps edge filtering off the opposite border.
  view->sampler = VK_NULL_HANDLE;
  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 1.0f,
                                      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK};
  check_vk_result(vkCreateSampler(h->g_device, &sampler_info, h->g_allocator, &ar->sampler));

  ar->aliased = true;
  ar->atlas_texture = view;
  ar->shader = renderer_load_shader(h, "data/shaders/atlas.vert.spv", "data/shaders/atlas.frag.spv");
  // Same vertex input the sprite path uses; a module that only ever draws whole
  // images still needs the layout described on the shader.
  if (ar->shader && !ar->shader->layout) {
    static vertex_layout_t alias_layout;
    alias_layout.binding_count = 2;
    alias_layout.bindings[0] = atlas_binding_desc[0];
    alias_layout.bindings[1] = atlas_binding_desc[1];
    alias_layout.attr_count = 9;
    for (uint32_t i = 0; i < 9; ++i) alias_layout.attrs[i] = atlas_attrib_descs[i];
    ar->shader->layout = &alias_layout;
  }
  ar->max_instances = 256;
  ar->sprite_count = 1;
  ar->sprite_definitions = malloc(sizeof(sprite_definition_t));
  ar->sprite_definitions[0] = (sprite_definition_t){0, 0, src->width, src->height};
  ar->layer_width = src->width;
  ar->layer_height = src->height;
  create_buffer(h, sizeof(atlas_instance_t) * ar->max_instances, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ar->instance_buffer);
  vkMapMemory(h->g_device, ar->instance_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&ar->instance_ptr);

  r->dynamic_atlases[r->dynamic_atlas_count++] = ar;
  renderer_unlock();
  return ar;
}

void renderer_destroy_atlas(gfx_handler_t *h, atlas_renderer_t *ar) {
  renderer_state_t *r = &h->renderer;
  if (!ar) return;

  renderer_lock();
  vkDeviceWaitIdle(h->g_device);
  for (uint32_t i = 0; i < r->dynamic_atlas_count; ++i) {
    if (r->dynamic_atlases[i] != ar) continue;
    r->dynamic_atlases[i] = r->dynamic_atlases[--r->dynamic_atlas_count];
    break;
  }
  if (ar->aliased) {
    // The image belongs to whoever created the texture; only the view and
    // sampler built for sampling it are ours to release.
    if (ar->atlas_texture) {
      // Only the view is ours here: the sampler belongs to the atlas and is
      // released by renderer_cleanup_atlas_renderer just below.
      if (ar->atlas_texture->image_view) vkDestroyImageView(h->g_device, ar->atlas_texture->image_view, h->g_allocator);
      free(ar->atlas_texture);
      ar->atlas_texture = NULL;
    }
  } else if (ar->atlas_texture) {
    renderer_destroy_texture(h, ar->atlas_texture);
  }
  renderer_cleanup_atlas_renderer(h, ar);
  free(ar);
  renderer_unlock();
}

static VkFormat vertex_format_to_vk(int format) {
  switch (format) {
  case 0: return VK_FORMAT_R32_SFLOAT;
  case 1: return VK_FORMAT_R32G32_SFLOAT;
  case 2: return VK_FORMAT_R32G32B32_SFLOAT;
  case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
  case 4: return VK_FORMAT_R32_SINT;
  case 5: return VK_FORMAT_R32_UINT;
  default: return VK_FORMAT_UNDEFINED;
  }
}

shader_t *renderer_create_shader_spirv(gfx_handler_t *h, const void *vert_spirv, size_t vert_size, const void *frag_spirv, size_t frag_size,
                                       const vertex_layout_t *layout) {
  renderer_state_t *r = &h->renderer;
  if (!vert_spirv || !frag_spirv) return NULL;
  if (r->shader_count >= MAX_SHADERS) {
    log_error(LOG_SOURCE, "Max shader count (%d) reached.", MAX_SHADERS);
    return NULL;
  }

  shader_t *shader = &r->shaders[r->shader_count];
  memset(shader, 0, sizeof(*shader));
  shader->id = r->shader_count++;
  shader->active = true;
  shader->vert_shader_module = create_shader_module(h, (const char *)vert_spirv, vert_size);
  shader->frag_shader_module = create_shader_module(h, (const char *)frag_spirv, frag_size);
  if (layout) {
    // Owned by the shader slot so the module may free its descriptor.
    static vertex_layout_t layouts[MAX_SHADERS];
    layouts[shader->id] = *layout;
    shader->layout = &layouts[shader->id];
  }
  snprintf(shader->vert_path, sizeof(shader->vert_path), "<module:%u:vert>", shader->id);
  snprintf(shader->frag_path, sizeof(shader->frag_path), "<module:%u:frag>", shader->id);
  return shader;
}

custom_pipeline_t *renderer_create_custom_pipeline(gfx_handler_t *h, const void *vert_spirv, size_t vert_size, const void *frag_spirv,
                                                   size_t frag_size, const uint32_t *attr_locations, const uint32_t *attr_offsets,
                                                   const int *attr_formats, uint32_t attr_count, uint32_t instance_stride,
                                                   uint32_t max_instances, uint32_t texture_count, bool alpha_blend) {
  renderer_state_t *r = &h->renderer;
  if (r->custom_pipeline_count >= MAX_CUSTOM_PIPELINES) {
    log_error(LOG_SOURCE, "Custom pipeline limit (%d) reached.", MAX_CUSTOM_PIPELINES);
    return NULL;
  }
  if (attr_count + 1 > MAX_CUSTOM_VERTEX_ATTRS) {
    log_error(LOG_SOURCE, "Custom pipeline declares %u attributes, limit is %d.", attr_count, MAX_CUSTOM_VERTEX_ATTRS - 1);
    return NULL;
  }
  // instance_stride == 0 asks for a mesh pipeline: attributes come from the
  // vertex buffer itself and there is no per-instance binding at all. That is
  // what a game's level pass wants, where one big mesh is drawn once.
  const bool mesh_mode = instance_stride == 0;
  if (!mesh_mode && max_instances == 0) return NULL;
  if (texture_count > MAX_TEXTURES_PER_DRAW) {
    log_error(LOG_SOURCE, "Custom pipeline wants %u textures, limit is %d.", texture_count, MAX_TEXTURES_PER_DRAW);
    return NULL;
  }

  custom_pipeline_t *pipe = &r->custom_pipelines[r->custom_pipeline_count];
  memset(pipe, 0, sizeof(*pipe));

  pipe->layout.bindings[0] = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  if (mesh_mode) {
    pipe->layout.binding_count = 1;
    pipe->layout.attr_count = 0;
    for (uint32_t i = 0; i < attr_count; ++i) {
      const VkFormat format = vertex_format_to_vk(attr_formats[i]);
      if (format == VK_FORMAT_UNDEFINED) {
        log_error(LOG_SOURCE, "Mesh pipeline attribute %u has an unknown format.", i);
        return NULL;
      }
      pipe->layout.attrs[pipe->layout.attr_count++] =
          (VkVertexInputAttributeDescription){.binding = 0, .location = attr_locations[i], .format = format, .offset = attr_offsets[i]};
    }
  } else {
    // Binding 0 is the engine's unit quad, binding 1 the module's instance data.
    pipe->layout.bindings[1] =
        (VkVertexInputBindingDescription){.binding = 1, .stride = instance_stride, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};
    pipe->layout.binding_count = 2;
    pipe->layout.attrs[0] =
        (VkVertexInputAttributeDescription){.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
    pipe->layout.attr_count = 1;
    for (uint32_t i = 0; i < attr_count; ++i) {
      const VkFormat format = vertex_format_to_vk(attr_formats[i]);
      if (format == VK_FORMAT_UNDEFINED) {
        log_error(LOG_SOURCE, "Custom pipeline attribute %u has an unknown format.", i);
        return NULL;
      }
      if (attr_locations[i] == 0) {
        log_error(LOG_SOURCE, "Custom pipeline attribute %u uses location 0, reserved for the quad corner.", i);
        return NULL;
      }
      pipe->layout.attrs[pipe->layout.attr_count++] =
          (VkVertexInputAttributeDescription){.binding = 1, .location = attr_locations[i], .format = format, .offset = attr_offsets[i]};
    }
  }
  pipe->layout.alpha_blend = alpha_blend;

  pipe->shader = renderer_create_shader_spirv(h, vert_spirv, vert_size, frag_spirv, frag_size, &pipe->layout);
  if (!pipe->shader) return NULL;

  pipe->instance_stride = instance_stride;
  pipe->max_instances = max_instances;
  pipe->texture_count = texture_count;
  if (!mesh_mode) {
    create_buffer(h, (VkDeviceSize)instance_stride * max_instances, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &pipe->instance_buffer);
    vkMapMemory(h->g_device, pipe->instance_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&pipe->instance_ptr);
  }
  pipe->active = true;
  r->custom_pipeline_count++;
  return pipe;
}

void renderer_destroy_custom_pipeline(gfx_handler_t *h, custom_pipeline_t *pipe) {
  if (!pipe || !pipe->active) return;
  vkDeviceWaitIdle(h->g_device);
  if (pipe->instance_buffer.buffer) {
    vkUnmapMemory(h->g_device, pipe->instance_buffer.memory);
    vkDestroyBuffer(h->g_device, pipe->instance_buffer.buffer, h->g_allocator);
    vkFreeMemory(h->g_device, pipe->instance_buffer.memory, h->g_allocator);
  }
  pipe->active = false;
  pipe->instance_ptr = NULL;
  pipe->instance_count = 0;
}

void renderer_submit_instances(gfx_handler_t *h, custom_pipeline_t *pipe, float z, texture_t *const *textures, uint32_t texture_count,
                               const void *instances, uint32_t count) {
  renderer_state_t *r = &h->renderer;
  if (!pipe || !pipe->active || count == 0 || !instances) return;
  if (r->queue.count >= MAX_RENDER_COMMANDS) return;
  if (pipe->instance_count + count > pipe->max_instances) {
    log_error(LOG_SOURCE, "Custom pipeline instance ring exhausted (%u).", pipe->max_instances);
    return;
  }

  const uint32_t start = pipe->instance_count;
  memcpy(pipe->instance_ptr + (size_t)start * pipe->instance_stride, instances, (size_t)count * pipe->instance_stride);
  pipe->instance_count += count;

  render_command_t *cmd = &r->queue.commands[r->queue.count++];
  cmd->type = RENDER_CMD_INSTANCES;
  cmd->z = z;
  cmd->data.instances.pipeline = pipe;
  cmd->data.instances.start = start;
  cmd->data.instances.count = count;
  cmd->data.instances.texture_count = texture_count < MAX_TEXTURES_PER_DRAW ? texture_count : MAX_TEXTURES_PER_DRAW;
  for (uint32_t i = 0; i < cmd->data.instances.texture_count; ++i) cmd->data.instances.textures[i] = textures[i];
}

static void record_preview_layer_update(VkCommandBuffer cmd, texture_t *texture, uint32_t layer, VkBuffer staging) {
  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = texture->image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = texture->mip_levels,
                                                       .baseArrayLayer = layer,
                                                       .layerCount = 1}};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

  const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = layer, .layerCount = 1},
      .imageExtent = {texture->width, texture->height, 1},
  };
  vkCmdCopyBufferToImage(cmd, staging, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  int32_t mip_width = (int32_t)texture->width;
  int32_t mip_height = (int32_t)texture->height;
  barrier.subresourceRange.levelCount = 1;
  for (uint32_t mip = 1; mip < texture->mip_levels; ++mip) {
    barrier.subresourceRange.baseMipLevel = mip - 1;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    const VkImageBlit blit = {
        .srcOffsets = {{0, 0, 0}, {mip_width, mip_height, 1}},
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip - 1, .baseArrayLayer = layer, .layerCount = 1},
        .dstOffsets = {{0, 0, 0}, {mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1}},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1}};
    vkCmdBlitImage(cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    if (mip_width > 1) mip_width /= 2;
    if (mip_height > 1) mip_height /= 2;
  }

  barrier.subresourceRange.baseMipLevel = texture->mip_levels - 1;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

texture_t *renderer_render_instances_preview(gfx_handler_t *h, custom_pipeline_t *pipe, texture_t *const *textures,
                                             uint32_t texture_count, const void *instances, uint32_t count, uint32_t width,
                                             uint32_t height, const vec4 clear_color,
                                             const renderer_texture_layer_update_t *updates, uint32_t update_count,
                                             texture_t *destination, uint32_t destination_x, uint32_t destination_y) {
  if (!h || !pipe || !pipe->active || !pipe->shader || !instances || count == 0 || count > pipe->max_instances || width == 0 ||
      height == 0 || texture_count != pipe->texture_count || texture_count > MAX_TEXTURES_PER_DRAW ||
      update_count > MAX_TEXTURES_PER_DRAW || (update_count > 0 && !updates) || h->offscreen_render_pass == VK_NULL_HANDLE)
    return NULL;

  renderer_state_t *r = &h->renderer;
  for (uint32_t i = 0; i < texture_count; ++i) {
    texture_t *texture = textures ? textures[i] : NULL;
    if (!texture || !texture->active || texture->image_view == VK_NULL_HANDLE || texture->sampler == VK_NULL_HANDLE) return NULL;
  }
  for (uint32_t i = 0; i < update_count; ++i) {
    texture_t *texture = updates[i].texture;
    if (!texture || !texture->active || !updates[i].pixels || updates[i].layer >= texture->layer_count ||
        updates[i].width != texture->width || updates[i].height != texture->height)
      return NULL;
  }

  const bool render_into_destination = destination != NULL;
  texture_t *target = destination;
  VkRenderPass render_pass = h->offscreen_render_pass;
  uint32_t framebuffer_width = width;
  uint32_t framebuffer_height = height;

  if (render_into_destination) {
    if (!destination->active || destination->image_view == VK_NULL_HANDLE || destination->sampler == VK_NULL_HANDLE ||
        destination->layer_count != 1 || destination_x > destination->width || destination_y > destination->height ||
        width > destination->width - destination_x || height > destination->height - destination_y)
      return NULL;
    render_pass = get_or_create_preview_render_pass(h, destination->format);
    if (render_pass == VK_NULL_HANDLE) return NULL;
    framebuffer_width = destination->width;
    framebuffer_height = destination->height;
  } else {
    renderer_lock();
    uint32_t free_slot = UINT32_MAX;
    for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
      if (!r->textures[i].active) {
        free_slot = i;
        break;
      }
    }
    if (free_slot == UINT32_MAX) {
      renderer_unlock();
      log_error(LOG_SOURCE, "Max texture count (%d) reached while creating an instance preview.", MAX_TEXTURES);
      return NULL;
    }

    target = &r->textures[free_slot];
    memset(target, 0, sizeof(*target));
    target->id = free_slot;
    target->active = true;
    target->width = width;
    target->height = height;
    target->mip_levels = 1;
    target->layer_count = 1;
    target->format = h->g_main_window_data.SurfaceFormat.format;
    target->last_used_frame = h->g_main_window_data.FrameIndex;
    snprintf(target->path, sizeof(target->path), "module_instance_preview");
    create_image(h, width, height, 1, 1, target->format, VK_IMAGE_TILING_OPTIMAL,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 &target->image, &target->memory);
    target->image_view = create_image_view(h, target->image, target->format, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
    target->sampler = create_texture_sampler(h, 1, VK_FILTER_LINEAR);
    renderer_unlock();
  }

  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  const VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                      .renderPass = render_pass,
                                                      .attachmentCount = 1,
                                                      .pAttachments = &target->image_view,
                                                      .width = framebuffer_width,
                                                      .height = framebuffer_height,
                                                      .layers = 1};
  if (vkCreateFramebuffer(h->g_device, &framebuffer_info, h->g_allocator, &framebuffer) != VK_SUCCESS) {
    if (!render_into_destination) renderer_destroy_texture(h, target);
    return NULL;
  }

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, pipe->shader, 1, texture_count, render_pass);
  if (!pso) {
    vkDestroyFramebuffer(h->g_device, framebuffer, h->g_allocator);
    if (!render_into_destination) renderer_destroy_texture(h, target);
    return NULL;
  }

  primitive_ubo_t ubo = {0};
  ubo.zoom = 1.f;
  ubo.aspect = (float)width / (float)height;
  ubo.maxMapSize = 1.f;
  ubo.mapSize[0] = 1.f;
  ubo.mapSize[1] = 1.f;
  glm_ortho_rh_zo(-1.f, 1.f, -1.f, 1.f, -1.f, 1.f, ubo.proj);

  buffer_t ubo_buffer = {0};
  buffer_t instance_buffer = {0};
  create_buffer(h, sizeof(ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ubo_buffer);
  create_buffer(h, (VkDeviceSize)pipe->instance_stride * count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instance_buffer);
  void *mapped = NULL;
  vkMapMemory(h->g_device, ubo_buffer.memory, 0, sizeof(ubo), 0, &mapped);
  memcpy(mapped, &ubo, sizeof(ubo));
  vkUnmapMemory(h->g_device, ubo_buffer.memory);
  vkMapMemory(h->g_device, instance_buffer.memory, 0, (VkDeviceSize)pipe->instance_stride * count, 0, &mapped);
  memcpy(mapped, instances, (size_t)pipe->instance_stride * count);
  vkUnmapMemory(h->g_device, instance_buffer.memory);

  buffer_t staging_buffers[MAX_TEXTURES_PER_DRAW] = {0};
  for (uint32_t i = 0; i < update_count; ++i) {
    const VkDeviceSize bytes = (VkDeviceSize)updates[i].width * updates[i].height * format_bytes_per_pixel(updates[i].texture->format);
    create_buffer(h, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffers[i]);
    vkMapMemory(h->g_device, staging_buffers[i].memory, 0, bytes, 0, &mapped);
    memcpy(mapped, updates[i].pixels, (size_t)bytes);
    vkUnmapMemory(h->g_device, staging_buffers[i].memory);
  }

  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_count}};
  const VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                 .maxSets = 1,
                                                 .poolSizeCount = texture_count ? 2u : 1u,
                                                 .pPoolSizes = pool_sizes};
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  check_vk_result(vkCreateDescriptorPool(h->g_device, &pool_info, h->g_allocator, &descriptor_pool));

  const VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = descriptor_pool,
                                                .descriptorSetCount = 1,
                                                .pSetLayouts = &pso->descriptor_set_layout};
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  check_vk_result(vkAllocateDescriptorSets(h->g_device, &set_info, &descriptor_set));

  VkDescriptorBufferInfo buffer_info = {.buffer = ubo_buffer.buffer, .offset = 0, .range = sizeof(ubo)};
  VkDescriptorImageInfo image_infos[MAX_TEXTURES_PER_DRAW];
  VkWriteDescriptorSet writes[1 + MAX_TEXTURES_PER_DRAW];
  writes[0] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = descriptor_set,
                                     .dstBinding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .descriptorCount = 1,
                                     .pBufferInfo = &buffer_info};
  for (uint32_t i = 0; i < texture_count; ++i) {
    image_infos[i] = (VkDescriptorImageInfo){.sampler = textures[i]->sampler,
                                             .imageView = textures[i]->image_view,
                                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[i + 1] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = descriptor_set,
                                           .dstBinding = i + 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           .descriptorCount = 1,
                                           .pImageInfo = &image_infos[i]};
  }
  vkUpdateDescriptorSets(h->g_device, texture_count + 1, writes, 0, NULL);

  VkCommandBuffer cmd = begin_single_time_commands(h, r->transfer_command_pool);
  for (uint32_t i = 0; i < update_count; ++i)
    record_preview_layer_update(cmd, updates[i].texture, updates[i].layer, staging_buffers[i].buffer);
  const VkClearValue clear = {.color = {.float32 = {clear_color[0], clear_color[1], clear_color[2], clear_color[3]}}};
  const VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = render_pass,
                                      .framebuffer = framebuffer,
                                      .renderArea = {{0, 0}, {framebuffer_width, framebuffer_height}},
                                      .clearValueCount = render_into_destination ? 0u : 1u,
                                      .pClearValues = render_into_destination ? NULL : &clear};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  if (render_into_destination) {
    const VkClearAttachment clear_attachment = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue = {.color = {.float32 = {clear_color[0], clear_color[1], clear_color[2], clear_color[3]}}},
    };
    const VkClearRect clear_rect = {
        .rect = {{(int32_t)destination_x, (int32_t)destination_y}, {width, height}}, .baseArrayLayer = 0, .layerCount = 1};
    vkCmdClearAttachments(cmd, 1, &clear_attachment, 1, &clear_rect);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkBuffer vertex_buffers[2] = {h->quad_mesh->vertex_buffer.buffer, instance_buffer.buffer};
  const VkDeviceSize offsets[2] = {0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, vertex_buffers, offsets);
  vkCmdBindIndexBuffer(cmd, h->quad_mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
  const VkViewport viewport = {(float)destination_x, (float)destination_y, (float)width, (float)height, 0.f, 1.f};
  const VkRect2D scissor = {{(int32_t)destination_x, (int32_t)destination_y}, {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDrawIndexed(cmd, h->quad_mesh->index_count, count, 0, 0, 0);
  vkCmdEndRenderPass(cmd);
  end_single_time_commands(h, r->transfer_command_pool, cmd);

  for (uint32_t i = 0; i < update_count; ++i) {
    vkDestroyBuffer(h->g_device, staging_buffers[i].buffer, h->g_allocator);
    vkFreeMemory(h->g_device, staging_buffers[i].memory, h->g_allocator);
  }
  vkDestroyDescriptorPool(h->g_device, descriptor_pool, h->g_allocator);
  vkDestroyBuffer(h->g_device, instance_buffer.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, instance_buffer.memory, h->g_allocator);
  vkDestroyBuffer(h->g_device, ubo_buffer.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, ubo_buffer.memory, h->g_allocator);
  vkDestroyFramebuffer(h->g_device, framebuffer, h->g_allocator);
  return target;
}

void renderer_submit_mesh(gfx_handler_t *h, custom_pipeline_t *pipe, float z, mesh_t *mesh, texture_t *const *textures,
                          uint32_t texture_count, const void *uniforms, size_t uniform_size) {
  renderer_state_t *r = &h->renderer;
  if (!pipe || !pipe->active || !mesh) return;
  if (r->queue.count >= MAX_RENDER_COMMANDS) return;
  if (uniform_size > MAX_QUEUED_UNIFORM_BYTES) {
    log_error(LOG_SOURCE, "Mesh draw carries %zu uniform bytes, limit is %d.", uniform_size, MAX_QUEUED_UNIFORM_BYTES);
    return;
  }

  render_command_t *cmd = &r->queue.commands[r->queue.count++];
  cmd->type = RENDER_CMD_MESH;
  cmd->z = z;
  cmd->data.mesh_draw.pipeline = pipe;
  cmd->data.mesh_draw.mesh = mesh;
  cmd->data.mesh_draw.texture_count = texture_count < MAX_TEXTURES_PER_DRAW ? texture_count : MAX_TEXTURES_PER_DRAW;
  for (uint32_t i = 0; i < cmd->data.mesh_draw.texture_count; ++i) cmd->data.mesh_draw.textures[i] = textures[i];
  cmd->data.mesh_draw.uniform_size = (uint32_t)uniform_size;
  if (uniforms && uniform_size > 0) memcpy(cmd->data.mesh_draw.uniforms, uniforms, uniform_size);
}

static void renderer_flush_custom_instances(gfx_handler_t *h, VkCommandBuffer cmd, const render_command_t *q) {
  renderer_state_t *r = &h->renderer;
  custom_pipeline_t *pipe = q->data.instances.pipeline;
  if (!pipe || !pipe->active || !pipe->shader) return;

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, pipe->shader, 1, q->data.instances.texture_count, h->g_main_window_data.RenderPass);
  if (!pso) return;

  primitive_ubo_t ubo = world_ubo(h);

  VkDeviceSize aligned = (sizeof(ubo) + r->min_ubo_alignment - 1) & ~(r->min_ubo_alignment - 1);
  if (r->ubo_buffer_offset + aligned > DYNAMIC_UBO_BUFFER_SIZE) {
    log_error(LOG_SOURCE, "UBO Ring Buffer Exhausted");
    return;
  }
  const uint32_t dyn_offset = r->ubo_buffer_offset;
  memcpy((char *)r->ubo_buffer_ptr + dyn_offset, &ubo, sizeof(ubo));
  r->ubo_buffer_offset += (uint32_t)aligned;

  const uint32_t pool_idx = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet desc;
  VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                    .descriptorPool = r->frame_descriptor_pools[pool_idx],
                                    .descriptorSetCount = 1,
                                    .pSetLayouts = &pso->descriptor_set_layout};
  if (vkAllocateDescriptorSets(h->g_device, &ai, &desc) != VK_SUCCESS) {
    log_error(LOG_SOURCE, "Descriptor allocation failed for a module pipeline");
    return;
  }

  VkDescriptorBufferInfo b_info = {.buffer = r->dynamic_ubo_buffer.buffer, .offset = dyn_offset, .range = sizeof(primitive_ubo_t)};
  VkWriteDescriptorSet writes[1 + MAX_TEXTURES_PER_DRAW];
  VkDescriptorImageInfo image_infos[MAX_TEXTURES_PER_DRAW];
  uint32_t write_count = 0;
  writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = desc,
                                                 .dstBinding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                 .descriptorCount = 1,
                                                 .pBufferInfo = &b_info};
  for (uint32_t i = 0; i < q->data.instances.texture_count; ++i) {
    texture_t *tex = q->data.instances.textures[i];
    if (!tex || !tex->active || tex->image_view == VK_NULL_HANDLE || tex->sampler == VK_NULL_HANDLE) tex = r->default_texture;
    if (!tex || !tex->active || tex->image_view == VK_NULL_HANDLE || tex->sampler == VK_NULL_HANDLE) {
      log_error(LOG_SOURCE, "Module draw has no valid texture for binding %u.", i);
      return;
    }
    image_infos[i] = (VkDescriptorImageInfo){
        .sampler = tex->sampler, .imageView = tex->image_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                   .dstSet = desc,
                                                   .dstBinding = 1 + i,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .pImageInfo = &image_infos[i]};
  }
  vkUpdateDescriptorSets(h->g_device, write_count, writes, 0, NULL);

  mesh_t *quad = h->quad_mesh;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkBuffer bufs[2] = {quad->vertex_buffer.buffer, pipe->instance_buffer.buffer};
  VkDeviceSize offs[2] = {0, (VkDeviceSize)q->data.instances.start * pipe->instance_stride};
  vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
  vkCmdBindIndexBuffer(cmd, quad->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 0, NULL);
  vkCmdDrawIndexed(cmd, quad->index_count, q->data.instances.count, 0, 0, 0);
}

bool renderer_update_texture_layer(gfx_handler_t *h, texture_t *tex, uint32_t layer, const void *pixels, uint32_t width, uint32_t height) {
  if (!tex || !pixels) return false;
  if (layer >= tex->layer_count) return false;
  // Partial-extent uploads would need their own copy region; the callers that
  // stream appearances always replace a whole layer.
  if (width != tex->width || height != tex->height) return false;

  renderer_lock();
  upload_texture_layer(h, tex, tex->format, (int)layer, pixels, (VkDeviceSize)width * height * format_bytes_per_pixel(tex->format));
  renderer_unlock();
  return true;
}

bool renderer_mark_texture_external(gfx_handler_t *h, texture_t *tex) {
  if (!h || !tex || !tex->active || tex->image == VK_NULL_HANDLE) return false;
  if (tex->external) return true;

  renderer_lock();
  // Textures are created sampleable. Parking this one as a colour attachment
  // now is what lets every later frame assume the same starting layout, so the
  // module never has to special-case the first frame it draws.
  transition_image_layout(h, h->renderer.transfer_command_pool, tex->image, tex->format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, tex->mip_levels, 0, tex->layer_count);
  tex->external = true;
  renderer_unlock();
  return true;
}

void renderer_sync_external_textures(gfx_handler_t *h, VkCommandBuffer cmd, bool for_sampling) {
  if (!h || cmd == VK_NULL_HANDLE) return;

  renderer_state_t *renderer = &h->renderer;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    texture_t *tex = &renderer->textures[i];
    if (!tex->active || !tex->external || tex->image == VK_NULL_HANDLE) continue;

    // The module's own submission is ordered ahead of this command buffer, so
    // its writes are already in flight by the time these barriers execute and
    // an execution dependency on the colour attachment stage is enough.
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = for_sampling ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = for_sampling ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = tex->image,
        .srcAccessMask = for_sampling ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = for_sampling ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = tex->mip_levels,
                             .baseArrayLayer = 0,
                             .layerCount = tex->layer_count}};

    const VkPipelineStageFlags src_stage =
        for_sampling ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags dst_stage =
        for_sampling ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
  }
}
