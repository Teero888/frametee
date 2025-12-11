#include "renderer.h"
#include "../logger/logger.h"
#include "graphics_backend.h"
#include <cglm/cglm.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
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
static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t base_layer,
                          uint32_t layer_count);
static pipeline_cache_entry_t *get_or_create_pipeline(gfx_handler_t *handler, shader_t *shader, uint32_t ubo_count, uint32_t texture_count,
                                                      const VkVertexInputBindingDescription *binding_descs, uint32_t binding_desc_count,
                                                      const VkVertexInputAttributeDescription *attrib_descs, uint32_t attrib_desc_count);
static void flush_primitives(gfx_handler_t *handler, VkCommandBuffer command_buffer);

// vertex description helpers
static VkVertexInputBindingDescription primitive_binding_description;
static VkVertexInputAttributeDescription primitive_attribute_descriptions[2];
static VkVertexInputBindingDescription mesh_binding_description;
static VkVertexInputAttributeDescription mesh_attribute_descriptions[3];
// skin instancing
static VkVertexInputBindingDescription skin_binding_desc[2];
static VkVertexInputAttributeDescription skin_attrib_descs[14];

// atlas things
static VkVertexInputBindingDescription atlas_binding_desc[2];
static VkVertexInputAttributeDescription atlas_attrib_descs[8];

static void setup_vertex_descriptions();

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

  err = vkQueueSubmit(handler->g_queue, 1, &submit_info, fence);
  check_vk_result_line(err, __LINE__);

  err = vkWaitForFences(handler->g_device, 1, &fence, VK_TRUE, UINT64_MAX);
  check_vk_result_line(err, __LINE__);

  vkDestroyFence(handler->g_device, fence, handler->g_allocator);
  vkFreeCommandBuffers(handler->g_device, pool, 1, &command_buffer);
}

static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer, VkDeviceSize size) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferCopy copy_region = {.size = size};
  vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout old_layout,
                                    VkImageLayout new_layout, uint32_t mip_levels, uint32_t base_layer, uint32_t layer_count) {
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

  if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    log_error(LOG_SOURCE, "Unsupported image layout transition requested!");
    abort();
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
  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = (VkFilter)filter,
                                      .minFilter = (VkFilter)filter,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .mipLodBias = 0.0f,
                                      .anisotropyEnable = VK_FALSE,
                                      .maxAnisotropy = 1.0f,
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
  FILE *file = fopen(filename, "rb");
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
  strncpy(texArray->path, "runtime_skin_array", sizeof(texArray->path) - 1);

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

  setup_vertex_descriptions();

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(handler->g_physical_device, &properties);
  renderer->min_ubo_alignment = properties.limits.minUniformBufferOffsetAlignment;

  renderer->camera.zoom_wanted = 5.0f;
  renderer->lod_bias = -0.5f; // Default bias

  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                       .queueFamilyIndex = handler->g_queue_family};
  check_vk_result(vkCreateCommandPool(handler->g_device, &pool_info, handler->g_allocator, &renderer->transfer_command_pool));

  VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 * MAX_TEXTURES_PER_DRAW}};
  for (int i = 0; i < 3; i++) { // triple buffering
    VkDescriptorPoolCreateInfo pool_create_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                   .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                   .maxSets = 100,
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

  create_buffer(handler, MAX_PRIMITIVE_INDICES * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_index_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_index_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&renderer->index_buffer_ptr);

  create_buffer(handler, DYNAMIC_UBO_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->dynamic_ubo_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_ubo_buffer.memory, 0, VK_WHOLE_SIZE, 0, &renderer->ubo_buffer_ptr);

  // create a 2d array texture to hold max_skins atlases (each 512x512, rgba8)
  renderer->skin_manager.atlas_array = renderer_create_texture_2d_array(handler, 512, 512, MAX_SKINS, VK_FORMAT_R8G8B8A8_UNORM);
  memset(renderer->skin_manager.layer_used, 0, sizeof(renderer->skin_manager.layer_used));
  // skin renderer
  renderer->skin_renderer.skin_shader = renderer_load_shader(handler, "data/shaders/skin.vert.spv", "data/shaders/skin.frag.spv");

  // allocate big instance buffer
  create_buffer(handler, sizeof(skin_instance_t) * 100000, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->skin_renderer.instance_buffer);
  vkMapMemory(handler->g_device, renderer->skin_renderer.instance_buffer.memory, 0, VK_WHOLE_SIZE, 0, (void **)&renderer->skin_renderer.instance_ptr);

  renderer->skin_renderer.instance_count = 0;

  static sprite_definition_t gameskin_sprites[GAMESKIN_SPRITE_COUNT] = {[GAMESKIN_HAMMER_BODY] = {64, 32, 128, 96},
                                                                        [GAMESKIN_GUN_BODY] = {64, 128, 128, 64},
                                                                        [GAMESKIN_GUN_PROJ] = {192, 128, 64, 64},
                                                                        [GAMESKIN_GUN_MUZZLE1] = {256, 128, 128, 64},
                                                                        [GAMESKIN_GUN_MUZZLE2] = {384, 128, 128, 64},
                                                                        [GAMESKIN_GUN_MUZZLE3] = {512, 128, 128, 64},
                                                                        [GAMESKIN_SHOTGUN_BODY] = {64, 192, 256, 64},
                                                                        [GAMESKIN_SHOTGUN_PROJ] = {320, 192, 64, 64},
                                                                        [GAMESKIN_SHOTGUN_MUZZLE1] = {384, 192, 128, 64},
                                                                        [GAMESKIN_SHOTGUN_MUZZLE2] = {512, 192, 128, 64},
                                                                        [GAMESKIN_SHOTGUN_MUZZLE3] = {640, 192, 128, 64},
                                                                        [GAMESKIN_GRENADE_BODY] = {64, 256, 224, 64},
                                                                        [GAMESKIN_GRENADE_PROJ] = {320, 256, 64, 64},
                                                                        [GAMESKIN_LASER_BODY] = {64, 384, 224, 96},
                                                                        [GAMESKIN_LASER_PROJ] = {320, 384, 64, 64},
                                                                        [GAMESKIN_NINJA_BODY] = {64, 320, 256, 64},
                                                                        [GAMESKIN_NINJA_MUZZLE1] = {800, 0, 224, 128},
                                                                        [GAMESKIN_NINJA_MUZZLE2] = {800, 128, 224, 128},
                                                                        [GAMESKIN_NINJA_MUZZLE3] = {800, 256, 224, 128},
                                                                        [GAMESKIN_HEALTH_FULL] = {672, 0, 64, 64},
                                                                        [GAMESKIN_HEALTH_EMPTY] = {736, 0, 64, 64},
                                                                        [GAMESKIN_ARMOR_FULL] = {672, 64, 64, 64},
                                                                        [GAMESKIN_ARMOR_EMPTY] = {736, 64, 64, 64},
                                                                        [GAMESKIN_HOOK_CHAIN] = {64, 0, 32, 32},
                                                                        [GAMESKIN_HOOK_HEAD] = {96, 0, 64, 32},
                                                                        [GAMESKIN_PARTICLE_0] = {192, 0, 32, 32},
                                                                        [GAMESKIN_PARTICLE_1] = {192, 32, 32, 32},
                                                                        [GAMESKIN_PARTICLE_2] = {224, 0, 32, 32},
                                                                        [GAMESKIN_PARTICLE_3] = {224, 32, 32, 32},
                                                                        [GAMESKIN_PARTICLE_4] = {256, 0, 32, 32},
                                                                        [GAMESKIN_PARTICLE_5] = {256, 32, 32, 32},
                                                                        [GAMESKIN_PARTICLE_6] = {288, 0, 64, 64},
                                                                        [GAMESKIN_PARTICLE_7] = {352, 0, 64, 64},
                                                                        [GAMESKIN_PARTICLE_8] = {416, 0, 64, 64},
                                                                        [GAMESKIN_STAR_0] = {480, 0, 64, 64},
                                                                        [GAMESKIN_STAR_1] = {544, 0, 64, 64},
                                                                        [GAMESKIN_STAR_2] = {608, 0, 64, 64},
                                                                        [GAMESKIN_PICKUP_HEALTH] = {320, 64, 64, 64},
                                                                        [GAMESKIN_PICKUP_ARMOR] = {384, 64, 64, 64},
                                                                        [GAMESKIN_PICKUP_HAMMER] = {64, 32, 128, 96},
                                                                        [GAMESKIN_PICKUP_GUN] = {64, 128, 128, 64},
                                                                        [GAMESKIN_PICKUP_SHOTGUN] = {64, 192, 256, 64},
                                                                        [GAMESKIN_PICKUP_GRENADE] = {64, 256, 224, 64},
                                                                        [GAMESKIN_PICKUP_LASER] = {64, 384, 224, 96},
                                                                        [GAMESKIN_PICKUP_NINJA] = {64, 320, 256, 64},
                                                                        [GAMESKIN_PICKUP_ARMOR_SHOTGUN] = {480, 64, 64, 64},
                                                                        [GAMESKIN_PICKUP_ARMOR_GRENADE] = {544, 64, 64, 64},
                                                                        [GAMESKIN_PICKUP_ARMOR_NINJA] = {320, 320, 64, 64},
                                                                        [GAMESKIN_PICKUP_ARMOR_LASER] = {608, 64, 64, 64},
                                                                        [GAMESKIN_FLAG_BLUE] = {384, 256, 128, 256},
                                                                        [GAMESKIN_FLAG_RED] = {512, 256, 128, 256}};

  renderer_init_atlas_renderer(handler, &renderer->gameskin_renderer, "data/textures/game.png", gameskin_sprites, GAMESKIN_SPRITE_COUNT, 100000);

  static sprite_definition_t cursor_sprites[CURSOR_SPRITE_COUNT + 1] = {
      [CURSOR_HAMMER] = {0, 0, 64, 64},    [CURSOR_GUN] = {0, 128, 64, 64},   [CURSOR_SHOTGUN] = {0, 192, 64, 64},
      [CURSOR_GRENADE] = {0, 256, 64, 64}, [CURSOR_LASER] = {0, 384, 64, 64}, [CURSOR_NINJA] = {0, 320, 64, 64}};
  renderer_init_atlas_renderer(handler, &renderer->cursor_renderer, "data/textures/game.png", cursor_sprites, CURSOR_SPRITE_COUNT,
                               1); // we only render a single cursor

  log_info(LOG_SOURCE, "Renderer initialized successfully.");
  return 0;
}

void renderer_cleanup(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  VkDevice device = handler->g_device;
  VkAllocationCallbacks *allocator = handler->g_allocator;

  vkDeviceWaitIdle(device);

  for (uint32_t i = 0; i < MAX_SHADERS; ++i) {
    pipeline_cache_entry_t *entry = &renderer->pipeline_cache[i];
    if (entry->initialized) {
      vkDestroyPipeline(device, entry->pipeline, allocator);
      vkDestroyPipelineLayout(device, entry->pipeline_layout, allocator);
      vkDestroyDescriptorSetLayout(device, entry->descriptor_set_layout, allocator);
    }
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

  // free skin instance buffer
  if (renderer->skin_renderer.instance_buffer.buffer) {
    vkDestroyBuffer(device, renderer->skin_renderer.instance_buffer.buffer, allocator);
    vkFreeMemory(device, renderer->skin_renderer.instance_buffer.memory, allocator);
  }
  renderer_cleanup_atlas_renderer(handler, &renderer->gameskin_renderer);
  renderer_cleanup_atlas_renderer(handler, &renderer->cursor_renderer);

  log_info(LOG_SOURCE, "Renderer cleaned up successfully.");
}

static pipeline_cache_entry_t *get_or_create_pipeline(gfx_handler_t *handler, shader_t *shader, uint32_t ubo_count, uint32_t texture_count,
                                                      const VkVertexInputBindingDescription *binding_descs, uint32_t binding_desc_count,
                                                      const VkVertexInputAttributeDescription *attrib_descs, uint32_t attrib_desc_count) {
  renderer_state_t *renderer = &handler->renderer;
  pipeline_cache_entry_t *entry = &renderer->pipeline_cache[shader->id];

  VkRenderPass target_render_pass =
      handler->offscreen_render_pass != VK_NULL_HANDLE ? handler->offscreen_render_pass : handler->g_main_window_data.RenderPass;

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

  uint32_t binding_count = ubo_count + texture_count;
  VLA(VkDescriptorSetLayoutBinding, bindings, binding_count);

  // if (shader) {
  //   log_info(LOG_SOURCE, "Creating descriptor set layout for %s: ubo_count=%u texture_count=%u",
  //            shader->vert_path, ubo_count, texture_count);
  // }

  if (target_render_pass == VK_NULL_HANDLE) {
    log_error(LOG_SOURCE, "Cannot create graphics pipeline without a valid render pass.");
    entry->initialized = false;
    VLA_FREE(bindings);
    return NULL;
  }
  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    uint32_t binding_index = current_binding++;
    bindings[binding_index] = (VkDescriptorSetLayoutBinding){.binding = binding_index,
                                                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                             .descriptorCount = 1,
                                                             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  }
  for (uint32_t i = 0; i < texture_count; ++i) {
    uint32_t binding_index = current_binding++;
    bindings[binding_index] = (VkDescriptorSetLayoutBinding){.binding = binding_index,
                                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                             .descriptorCount = 1,
                                                             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = binding_count, .pBindings = bindings};
  VkResult err = vkCreateDescriptorSetLayout(handler->g_device, &layout_info, handler->g_allocator, &entry->descriptor_set_layout);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "vkCreateDescriptorSetLayout failed (shader=%s) err=%d", shader ? shader->vert_path : "<unknown>", err);
  }
  check_vk_result_line(err, __LINE__);

  VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &entry->descriptor_set_layout};
  err = vkCreatePipelineLayout(handler->g_device, &pipeline_layout_info, handler->g_allocator, &entry->pipeline_layout);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "vkCreatePipelineLayout failed (shader=%s) err=%d", shader ? shader->vert_path : "<unknown>", err);
  }
  check_vk_result_line(err, __LINE__);

  VkPipelineShaderStageCreateInfo vert_shader_stage_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                            .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                            .module = shader->vert_shader_module,
                                                            .pName = "main"};
  VkPipelineShaderStageCreateInfo frag_shader_stage_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                            .module = shader->frag_shader_module,
                                                            .pName = "main"};
  VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

  VkPipelineVertexInputStateCreateInfo vertex_input_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                            .vertexBindingDescriptionCount = binding_desc_count,
                                                            .pVertexBindingDescriptions = binding_descs,
                                                            .vertexAttributeDescriptionCount = attrib_desc_count,
                                                            .pVertexAttributeDescriptions = attrib_descs};

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                           .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                           .primitiveRestartEnable = VK_FALSE};

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};

  VkPipelineRasterizationStateCreateInfo rasterizer = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                       .depthClampEnable = VK_FALSE,
                                                       .rasterizerDiscardEnable = VK_FALSE,
                                                       .polygonMode = VK_POLYGON_MODE_FILL,
                                                       .cullMode = VK_CULL_MODE_NONE,
                                                       .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                       .lineWidth = 1.0f};

  VkPipelineMultisampleStateCreateInfo multisampling = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                         .depthTestEnable = VK_FALSE,
                                                         .depthWriteEnable = VK_FALSE,
                                                         .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};

  VkPipelineColorBlendAttachmentState color_blend_attachment = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blending = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &color_blend_attachment};

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

  err = vkCreateGraphicsPipelines(handler->g_device, handler->g_pipeline_cache, 1, &pipeline_info, handler->g_allocator, &entry->pipeline);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "vkCreateGraphicsPipelines failed (shader=%s, render_pass=%p, format=%d) err=%d", shader ? shader->vert_path : "<unknown>",
              (void *)target_render_pass, (int)handler->g_main_window_data.SurfaceFormat.format, err);
  }
  check_vk_result_line(err, __LINE__);

  entry->initialized = true;
  VLA_FREE(bindings);
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
  stbi_uc *pixels = stbi_load(image_path, &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
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

  log_info(LOG_SOURCE, "Loaded texture: %s", image_path);
  return texture;
}

texture_t *renderer_create_texture_from_rgba(gfx_handler_t *handler, const unsigned char *pixels, int width, int height) {
  renderer_state_t *renderer = &handler->renderer;
  if (!pixels) return NULL;

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

  texture_t *texture = &renderer->textures[free_slot];
  memset(texture, 0, sizeof(texture_t));
  texture->id = free_slot;
  texture->active = true;
  texture->width = width;
  texture->height = height;
  texture->mip_levels = 1; // No mipmaps for previews
  texture->layer_count = 1;
  strncpy(texture->path, "from_rgba_memory", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);

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
  texture->sampler = create_texture_sampler(handler, 1, VK_FILTER_LINEAR);
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
  renderer->ubo_buffer_offset = 0;
  renderer->current_command_buffer = command_buffer;

  VkViewport viewport = {0.f, 0.f, handler->viewport[0], handler->viewport[1], 0.0f, 1.0f};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor = {{0.0, 0.0}, {handler->viewport[0], handler->viewport[1]}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh, shader_t *shader, texture_t **textures,
                        uint32_t texture_count, void **ubos, VkDeviceSize *ubo_sizes, uint32_t ubo_count) {
  if (!mesh || !shader || !mesh->active || !shader->active) return;
  renderer_state_t *renderer = &handler->renderer;

  pipeline_cache_entry_t *pso = get_or_create_pipeline(handler, shader, ubo_count, texture_count, &mesh_binding_description, 1, // one binding
                                                       mesh_attribute_descriptions, 3);
  if (!pso) return;

  // Allocate a descriptor set for this pipeline
  VkDescriptorSet descriptor_set;
  uint32_t frame_pool_index = handler->g_main_window_data.FrameIndex % 3;
  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = renderer->frame_descriptor_pools[frame_pool_index],
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &pso->descriptor_set_layout};
  VkResult err = vkAllocateDescriptorSets(handler->g_device, &alloc_info, &descriptor_set);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "vkAllocateDescriptorSets failed (shader=%s) err=%d", shader ? shader->vert_path : "<unknown>", err);
  }
  check_vk_result_line(err, __LINE__);

  uint32_t binding_count = ubo_count + texture_count;
  VLA(VkWriteDescriptorSet, descriptor_writes, binding_count);
  VLA(VkDescriptorBufferInfo, buffer_infos, ubo_count);
  VLA(VkDescriptorImageInfo, image_infos, texture_count);
  VLA(VkDeviceSize, ubo_offsets, ubo_count);

  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    // UBO RING BUFFER LOGIC
    VkDeviceSize aligned_size = (ubo_sizes[i] + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
    assert(renderer->ubo_buffer_offset + aligned_size <= DYNAMIC_UBO_BUFFER_SIZE);

    ubo_offsets[i] = renderer->ubo_buffer_offset;
    memcpy((char *)renderer->ubo_buffer_ptr + ubo_offsets[i], ubos[i], ubo_sizes[i]);
    renderer->ubo_buffer_offset += aligned_size;

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
    image_infos[i] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .imageView = textures[i]->image_view, .sampler = textures[i]->sampler};
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

void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer) { flush_primitives(handler, command_buffer); }

void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex) {
  if (!tex || !tex->active) return;
  // Delay actual destruction until it's safe
  gfx_handler_t *h = handler;
  if (h->retire_count < 64) {
    h->retire_textures[h->retire_count].tex = tex;
    h->retire_textures[h->retire_count].frame_index = h->g_main_window_data.FrameIndex;
    h->retire_count++;
  }
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
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  float aspect = window_ratio / map_ratio;

  float max_map_size = fmax(h->map_data->width, h->map_data->height) * 0.001f;
  float ndc_x = (2.0f * sx / h->viewport[0]) - 1.0f;
  float ndc_y = (2.0f * sy / h->viewport[1]) - 1.0f;

  *wx = cam->pos[0] + (ndc_x / (cam->zoom * max_map_size));
  *wy = cam->pos[1] + (ndc_y / (cam->zoom * max_map_size * aspect));
  *wx *= h->map_data->width;
  *wy *= h->map_data->height;
}

void world_to_screen(gfx_handler_t *h, float wx, float wy, float *sx, float *sy) {
  camera_t *cam = &h->renderer.camera;
  wx /= h->map_data->width;
  wy /= h->map_data->height;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  float aspect = window_ratio / map_ratio;

  float max_map_size = fmaxf(h->map_data->width, h->map_data->height) * 0.001f;

  // World offset from camera center -> NDC
  float ndc_x = (wx - cam->pos[0]) * (cam->zoom * max_map_size);
  float ndc_y = (wy - cam->pos[1]) * (cam->zoom * max_map_size * aspect);

  // NDC [-1..1] to screen pixels [0..w],[0..h]
  *sx = (ndc_x + 1.0f) * 0.5f * h->viewport[0];
  *sy = (ndc_y + 1.0f) * 0.5f * h->viewport[1];
}

static void setup_vertex_descriptions() {
  primitive_binding_description =
      (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(primitive_vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  primitive_attribute_descriptions[0] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(primitive_vertex_t, pos)};
  primitive_attribute_descriptions[1] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(primitive_vertex_t, color)};

  mesh_binding_description = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  mesh_attribute_descriptions[0] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  mesh_attribute_descriptions[1] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vertex_t, color)};
  mesh_attribute_descriptions[2] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, tex_coord)};

  // skin instanced data
  skin_binding_desc[0] = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  skin_binding_desc[1] =
      (VkVertexInputBindingDescription){.binding = 1, .stride = sizeof(skin_instance_t), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};

  int i = 0;
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(skin_instance_t, pos)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 2, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(skin_instance_t, scale)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 3, .format = VK_FORMAT_R32_SINT, .offset = offsetof(skin_instance_t, skin_index)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 4, .format = VK_FORMAT_R32_SINT, .offset = offsetof(skin_instance_t, eye_state)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 5, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, body)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 6, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, back_foot)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 7, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, front_foot)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 8, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, attach)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 9, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(skin_instance_t, dir)};
  // colors
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 10, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, col_body)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 11, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(skin_instance_t, col_feet)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 1, .location = 12, .format = VK_FORMAT_R32_SINT, .offset = offsetof(skin_instance_t, col_custom)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1, .location = 13, .format = VK_FORMAT_R32_SINT, .offset = offsetof(skin_instance_t, col_gs)};

  // Atlas instanced data
  atlas_binding_desc[0] = (VkVertexInputBindingDescription){.binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  atlas_binding_desc[1] =
      (VkVertexInputBindingDescription){.binding = 1, .stride = sizeof(atlas_instance_t), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};

  i = 0;
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
}

// primitive drawing implementation
static void flush_primitives(gfx_handler_t *h, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &h->renderer;
  if (renderer->primitive_index_count == 0) {
    return;
  }

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, renderer->primitive_shader, 1, 0, &primitive_binding_description, 1, // one binding
                                                       primitive_attribute_descriptions, 2);
  if (!pso) return;

  primitive_ubo_t ubo;
  ubo.camPos[0] = h->renderer.camera.pos[0];
  ubo.camPos[1] = h->renderer.camera.pos[1];
  ubo.zoom = h->renderer.camera.zoom;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  ubo.aspect = window_ratio / map_ratio;

  ubo.maxMapSize = fmaxf(h->map_data->width, h->map_data->height) * 0.001f;
  ubo.mapSize[0] = h->map_data->width;
  ubo.mapSize[1] = h->map_data->height;
  ubo.lod_bias = renderer->lod_bias;

  glm_ortho_rh_no(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned_size = (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  assert(renderer->ubo_buffer_offset + aligned_size <= DYNAMIC_UBO_BUFFER_SIZE);

  uint32_t dynamic_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dynamic_offset, &ubo, ubo_size);
  renderer->ubo_buffer_offset += aligned_size;

  VkDescriptorSet descriptor_set;

  uint32_t frame_pool_index = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = renderer->frame_descriptor_pools[frame_pool_index],
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &pso->descriptor_set_layout};

  VkResult err = vkAllocateDescriptorSets(h->g_device, &alloc_info, &descriptor_set);
  if (err != VK_SUCCESS) {
    log_error(LOG_SOURCE, "vkAllocateDescriptorSets failed (primitive shader) err=%d", err);
  }
  check_vk_result_line(err, __LINE__);

  VkDescriptorBufferInfo buffer_info = {.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = dynamic_offset, .range = sizeof(primitive_ubo_t)};
  VkWriteDescriptorSet descriptor_write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = descriptor_set,
                                           .dstBinding = 0,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .descriptorCount = 1,
                                           .pBufferInfo = &buffer_info};
  vkUpdateDescriptorSets(h->g_device, 1, &descriptor_write, 0, NULL);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(command_buffer, 0, 1, &renderer->dynamic_vertex_buffer.buffer, offsets);
  vkCmdBindIndexBuffer(command_buffer, renderer->dynamic_index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
  vkCmdDrawIndexed(command_buffer, renderer->primitive_index_count, 1, 0, 0, 0);

  renderer->primitive_vertex_count = 0;
  renderer->primitive_index_count = 0;
}

void renderer_draw_rect_filled(gfx_handler_t *handler, vec2 pos, vec2 size, vec4 color) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->primitive_vertex_count + 4 > MAX_PRIMITIVE_VERTICES || renderer->primitive_index_count + 6 > MAX_PRIMITIVE_INDICES) {
    flush_primitives(handler, renderer->current_command_buffer);
  }

  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  glm_vec2_copy((vec2){pos[0], pos[1]}, vtx[0].pos);
  glm_vec4_copy(color, vtx[0].color);
  glm_vec2_copy((vec2){pos[0] + size[0], pos[1]}, vtx[1].pos);
  glm_vec4_copy(color, vtx[1].color);
  glm_vec2_copy((vec2){pos[0] + size[0], pos[1] + size[1]}, vtx[2].pos);
  glm_vec4_copy(color, vtx[2].color);
  glm_vec2_copy((vec2){pos[0], pos[1] + size[1]}, vtx[3].pos);
  glm_vec4_copy(color, vtx[3].color);

  // for (int i = 0; i < 4; i++) {
  //   world_to_screen(handler, vtx[i].pos[0], vtx[i].pos[1], &vtx[i].pos[0], &vtx[i].pos[1]);
  // }

  idx[0] = base_index + 0;
  idx[1] = base_index + 1;
  idx[2] = base_index + 2;
  idx[3] = base_index + 2;
  idx[4] = base_index + 3;
  idx[5] = base_index + 0;

  renderer->primitive_vertex_count += 4;
  renderer->primitive_index_count += 6;
}
void renderer_draw_circle_filled(gfx_handler_t *handler, vec2 center, float radius, vec4 color, uint32_t segments) {
  renderer_state_t *renderer = &handler->renderer;
  if (segments < 3) segments = 3;

  // Ensure we have enough buffer space, flush if not.
  if (renderer->primitive_vertex_count + segments + 1 > MAX_PRIMITIVE_VERTICES ||
      renderer->primitive_index_count + segments * 3 > MAX_PRIMITIVE_INDICES) {
    flush_primitives(handler, renderer->current_command_buffer);
  }

  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  // The center vertex
  glm_vec2_copy(center, vtx[0].pos);
  glm_vec4_copy(color, vtx[0].color);

  // The outer vertices
  float angle_step = 2.0f * (float)M_PI / segments;
  for (uint32_t i = 0; i < segments; i++) {
    float angle = i * angle_step;
    // Calculate vertex position relative to the center and add it
    vtx[i + 1].pos[0] = center[0] + cosf(angle) * radius;
    vtx[i + 1].pos[1] = center[1] + sinf(angle) * radius;

    glm_vec4_copy(color, vtx[i + 1].color);
  }

  // Create the triangle fan indices
  for (uint32_t i = 0; i < segments; i++) {
    idx[i * 3 + 0] = base_index; // Center point
    idx[i * 3 + 1] = base_index + i + 1;
    idx[i * 3 + 2] = base_index + ((i + 1) % segments) + 1;
  }

  renderer->primitive_vertex_count += segments + 1;
  renderer->primitive_index_count += segments * 3;
}

// TODO: ensuring the width of the thing is atleast 1px is kinda expensive. think of another way to do this
void renderer_draw_line(gfx_handler_t *handler, vec2 p1, vec2 p2, vec4 color, float thickness) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->primitive_vertex_count + 4 > MAX_PRIMITIVE_VERTICES || renderer->primitive_index_count + 6 > MAX_PRIMITIVE_INDICES) {
    flush_primitives(handler, renderer->current_command_buffer);
  }

  vec2 dir;
  glm_vec2_sub(p2, p1, dir);
  glm_vec2_normalize(dir);
  vec2 normal = {-dir[1], dir[0]};

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

  uint32_t base_index = renderer->primitive_vertex_count;
  primitive_vertex_t *vtx = renderer->vertex_buffer_ptr + base_index;
  uint32_t *idx = renderer->index_buffer_ptr + renderer->primitive_index_count;

  glm_vec2_copy((vec2){p1[0] - normal[0] * half_t1, p1[1] - normal[1] * half_t1}, vtx[0].pos);
  glm_vec4_copy(color, vtx[0].color);
  glm_vec2_copy((vec2){p2[0] - normal[0] * half_t2, p2[1] - normal[1] * half_t2}, vtx[1].pos);
  glm_vec4_copy(color, vtx[1].color);
  glm_vec2_copy((vec2){p2[0] + normal[0] * half_t2, p2[1] + normal[1] * half_t2}, vtx[2].pos);
  glm_vec4_copy(color, vtx[2].color);
  glm_vec2_copy((vec2){p1[0] + normal[0] * half_t1, p1[1] + normal[1] * half_t1}, vtx[3].pos);
  glm_vec4_copy(color, vtx[3].color);

  idx[0] = base_index + 0;
  idx[1] = base_index + 1;
  idx[2] = base_index + 2;
  idx[3] = base_index + 2;
  idx[4] = base_index + 3;
  idx[5] = base_index + 0;

  renderer->primitive_vertex_count += 4;
  renderer->primitive_index_count += 6;
}

void renderer_draw_map(gfx_handler_t *h) {
  if (!h->map_shader || !h->quad_mesh || h->map_texture_count <= 0) return;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  if (isnan(map_ratio) || map_ratio == 0) map_ratio = 1.0f;

  float zoom = 1.0 / (h->renderer.camera.zoom * fmax(h->map_data->width, h->map_data->height) * 0.001);
  if (isnan(zoom)) zoom = 1.0f;

  float aspect = 1.0f / (window_ratio / map_ratio);

  map_buffer_object_t ubo = {
      .transform = {h->renderer.camera.pos[0], h->renderer.camera.pos[1], zoom}, .aspect = aspect, .lod_bias = h->renderer.lod_bias};

  void *ubos[] = {&ubo};
  VkDeviceSize ubo_sizes[] = {sizeof(ubo)};
  renderer_draw_mesh(h, h->current_frame_command_buffer, h->quad_mesh, h->map_shader, h->map_textures, h->map_texture_count, ubos, ubo_sizes, 1);
}

static int skin_manager_alloc_layer(renderer_state_t *r) {
  for (int i = 0; i < MAX_SKINS; i++) {
    if (!r->skin_manager.layer_used[i]) {
      r->skin_manager.layer_used[i] = true;
      return i;
    }
  }
  return -1; // full
}

static void skin_manager_free_layer(renderer_state_t *r, int idx) {
  if (idx >= 0 && idx < MAX_SKINS) {
    r->skin_manager.layer_used[idx] = false;
  }
}

void renderer_begin_skins(gfx_handler_t *h) { h->renderer.skin_renderer.instance_count = 0; }

void renderer_push_skin_instance(gfx_handler_t *h, vec2 pos, float scale, int skin_index, int eye_state, vec2 dir, const anim_state_t *anim_state,
                                 vec3 col_body, vec3 col_feet, bool use_custom_color) {
  skin_renderer_t *sr = &h->renderer.skin_renderer;
  uint32_t i = sr->instance_count++;
  sr->instance_ptr[i].pos[0] = pos[0];
  sr->instance_ptr[i].pos[1] = pos[1];
  sr->instance_ptr[i].scale = scale * 1.25f;
  sr->instance_ptr[i].skin_index = skin_index;
  sr->instance_ptr[i].eye_state = eye_state + 6;

  sr->instance_ptr[i].body[0] = anim_state->body.x;
  sr->instance_ptr[i].body[1] = anim_state->body.y;
  sr->instance_ptr[i].body[2] = anim_state->body.angle;

  sr->instance_ptr[i].back_foot[0] = anim_state->back_foot.x;
  sr->instance_ptr[i].back_foot[1] = anim_state->back_foot.y;
  sr->instance_ptr[i].back_foot[2] = anim_state->back_foot.angle;

  sr->instance_ptr[i].front_foot[0] = anim_state->front_foot.x;
  sr->instance_ptr[i].front_foot[1] = anim_state->front_foot.y;
  sr->instance_ptr[i].front_foot[2] = anim_state->front_foot.angle;

  sr->instance_ptr[i].attach[0] = anim_state->attach.x;
  sr->instance_ptr[i].attach[1] = anim_state->attach.y;
  sr->instance_ptr[i].attach[2] = anim_state->attach.angle;

  sr->instance_ptr[i].dir[0] = dir[0];
  sr->instance_ptr[i].dir[1] = dir[1];

  memcpy(sr->instance_ptr[i].col_body, col_body, 3 * sizeof(float));
  memcpy(sr->instance_ptr[i].col_feet, col_feet, 3 * sizeof(float));
  sr->instance_ptr[i].col_custom = use_custom_color;
  sr->instance_ptr[i].col_gs = h->renderer.skin_manager.atlas_array[skin_index].gs_org;
}

void renderer_flush_skins(gfx_handler_t *h, VkCommandBuffer cmd, texture_t *skin_array) {
  renderer_state_t *renderer = &h->renderer;
  skin_renderer_t *sr = &renderer->skin_renderer;
  if (sr->instance_count == 0) return;

  mesh_t *quad = h->quad_mesh;

  // pipeline: 1 UBO + 1 texture
  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, sr->skin_shader, 1, 1, skin_binding_desc, 2, skin_attrib_descs, 14);
  if (!pso) return;

  // prepare camera UBO same as primitives
  primitive_ubo_t ubo;
  ubo.camPos[0] = renderer->camera.pos[0];
  ubo.camPos[1] = renderer->camera.pos[1];
  ubo.zoom = renderer->camera.zoom;
  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  ubo.aspect = window_ratio / map_ratio;
  ubo.maxMapSize = fmaxf(h->map_data->width, h->map_data->height) * 0.001f;
  ubo.mapSize[0] = h->map_data->width;
  ubo.mapSize[1] = h->map_data->height;
  ubo.lod_bias = renderer->lod_bias;
  glm_ortho_rh_no(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  // copy into dynamic UBO ring
  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned = (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  assert(renderer->ubo_buffer_offset + aligned <= DYNAMIC_UBO_BUFFER_SIZE);
  uint32_t dyn_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dyn_offset, &ubo, ubo_size);
  renderer->ubo_buffer_offset += aligned;

  // Allocate descriptor set
  uint32_t pool_idx = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet desc;
  VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                    .descriptorPool = renderer->frame_descriptor_pools[pool_idx],
                                    .descriptorSetCount = 1,
                                    .pSetLayouts = &pso->descriptor_set_layout};
  check_vk_result(vkAllocateDescriptorSets(h->g_device, &ai, &desc));

  // write UBO
  VkDescriptorBufferInfo bufInfo = {.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = dyn_offset, .range = sizeof(primitive_ubo_t)};
  VkDescriptorImageInfo img = {
      .sampler = skin_array->sampler, .imageView = skin_array->image_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .descriptorCount = 1,
                                     .pBufferInfo = &bufInfo},
                                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .pImageInfo = &img}};
  vkUpdateDescriptorSets(h->g_device, 2, writes, 0, NULL);

  // issue draw
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkBuffer bufs[2] = {quad->vertex_buffer.buffer, sr->instance_buffer.buffer};
  VkDeviceSize offs[2] = {0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
  vkCmdBindIndexBuffer(cmd, quad->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 0, NULL);
  vkCmdDrawIndexed(cmd, quad->index_count, sr->instance_count, 0, 0, 0);

  sr->instance_count = 0;
}

int renderer_load_skin_from_memory(gfx_handler_t *h, const unsigned char *buffer, size_t size, texture_t **out_preview_texture) {
  int tex_width, tex_height, channels;
  stbi_uc *pixels = stbi_load_from_memory(buffer, (int)size, &tex_width, &tex_height, &channels, STBI_rgb_alpha);
  if (out_preview_texture) *out_preview_texture = NULL;
  if (!pixels) {
    log_error(LOG_SOURCE, "Failed to load skin from memory buffer.");
    return -1;
  }

  if (tex_width <= 0 || tex_height <= 0 || tex_width % 256 != 0 || tex_height % 128 != 0) {
    log_error(LOG_SOURCE, "Skin from memory has invalid dimensions (%dx%d), must be a multiple of 256x128", tex_width, tex_height);
    stbi_image_free(pixels);
    return -1;
  }

  // create a smaller separate preview texture for the skin browser
  if (out_preview_texture) {
    int preview_width = 128;
    int preview_height = 64;
    unsigned char *resized_preview_pixels = malloc(preview_width * preview_height * 4);
    if (resized_preview_pixels) {
      stbir_resize_uint8_linear(pixels, tex_width, tex_height, 0, resized_preview_pixels, preview_width, preview_height, 0, STBIR_RGBA);
      *out_preview_texture = renderer_create_texture_from_rgba(h, resized_preview_pixels, preview_width, preview_height);
      free(resized_preview_pixels);
    } else {
      log_error(LOG_SOURCE, "Failed to allocate memory for skin preview resize.");
    }
  }

  // Pre-multiply alpha before resizing (Crucial for correct bilinear interpolation)
  for (int i = 0; i < tex_width * tex_height; i++) {
    int idx = i * 4;
    uint8_t a = pixels[idx + 3];
    // Integer multiply effectively zeros out the pixel if a is 0
    pixels[idx + 0] = (uint8_t)((int)pixels[idx + 0] * a / 255);
    pixels[idx + 1] = (uint8_t)((int)pixels[idx + 1] * a / 255);
    pixels[idx + 2] = (uint8_t)((int)pixels[idx + 2] * a / 255);
  }

  const int final_width = 512;
  const int final_height = 512;
  stbi_uc *repacked_pixels = calloc(1, final_width * final_height * 4);
  if (!repacked_pixels) {
    stbi_image_free(pixels);
    return -1;
  }

  memset(repacked_pixels, 0, final_width * final_height * 4);

  int scale = tex_width / 256;

#define COPY_PART(src_x, src_y, w, h, dst_x, dst_y)                                                                                                  \
  stbir_resize_uint8_linear(pixels + ((src_y) * scale * tex_width + (src_x) * scale) * 4, (w) * scale, (h) * scale, tex_width * 4,                   \
                            repacked_pixels + ((dst_y) * final_width + (dst_x)) * 4, (w) * 2, (h) * 2, final_width * 4, STBIR_RGBA)

  COPY_PART(0, 0, 96, 96, 8, 8);        // Body
  COPY_PART(96, 0, 96, 96, 208, 8);     // Body Shadow
  COPY_PART(192, 32, 64, 32, 8, 208);   // Foot
  COPY_PART(192, 64, 64, 32, 144, 208); // Foot Shadow
  for (int i = 0; i < 6; ++i) {
    int src_x = 64 + i * 32;
    int dst_x = 8 + i * 72;
    COPY_PART(src_x, 96, 32, 32, dst_x, 280); // Eyes
  }
#undef COPY_PART

  for (int i = 0; i < final_width * final_height; i++) {
    int idx = i * 4;
    if (repacked_pixels[idx + 3] == 0) {
      repacked_pixels[idx + 0] = 0;
      repacked_pixels[idx + 1] = 0;
      repacked_pixels[idx + 2] = 0;
    }
  }

  renderer_state_t *r = &h->renderer;
  int layer = skin_manager_alloc_layer(r);
  if (layer < 0) {
    log_error(LOG_SOURCE, "No free skin layers available (max %d reached).", MAX_SKINS);
    if (out_preview_texture && *out_preview_texture) {
      renderer_destroy_texture(h, *out_preview_texture);
      *out_preview_texture = NULL;
    }
    free(repacked_pixels);
    stbi_image_free(pixels);
    return -1;
  }

  // do ddnet grayscale retard logic
  // Note: this is done on the original 'pixels' for best quality before resize
  uint32_t freq[256] = {0};
  int body_w = (tex_width / 256) * 96;
  int body_h = (tex_height / 128) * 96;
  for (int y = 0; y < body_h; ++y) {
    size_t rowBase = (size_t)y * tex_width;
    for (int x = 0; x < body_w; ++x) {
      size_t idx = (rowBase + (size_t)x) * 4u;
      if (pixels[idx + 3] > 128) {
        uint8_t gray = (uint8_t)(0.2126f * pixels[idx + 0] + 0.7152f * pixels[idx + 1] + 0.0722f * pixels[idx + 2]);
        freq[gray]++;
      }
    }
  }
  uint8_t org_weight = 1;
  for (int i = 1; i < 256; ++i) {
    if (freq[org_weight] < freq[i]) org_weight = (uint8_t)i;
  }
  r->skin_manager.atlas_array[layer].gs_org = org_weight;

  // Upload to Vulkan
  VkDeviceSize image_size = final_width * final_height * 4;
  buffer_t staging;
  create_buffer(h, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging);

  void *data;
  vkMapMemory(h->g_device, staging.memory, 0, image_size, 0, &data);
  memcpy(data, repacked_pixels, image_size);
  vkUnmapMemory(h->g_device, staging.memory);
  free(repacked_pixels);
  stbi_image_free(pixels);

  transition_image_layout(h, r->transfer_command_pool, r->skin_manager.atlas_array->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, layer, 1);

  VkCommandBuffer cmd = begin_single_time_commands(h, r->transfer_command_pool);
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = layer, .layerCount = 1},
      .imageExtent = {(uint32_t)final_width, (uint32_t)final_height, 1},
  };
  vkCmdCopyBufferToImage(cmd, staging.buffer, r->skin_manager.atlas_array->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  end_single_time_commands(h, r->transfer_command_pool, cmd);

  if (!build_mipmaps(h, r->skin_manager.atlas_array->image, final_width, final_height, r->skin_manager.atlas_array->mip_levels, layer, 1)) {
    transition_image_layout(h, r->transfer_command_pool, r->skin_manager.atlas_array->image, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, layer, 1);
  }

  vkDestroyBuffer(h->g_device, staging.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, staging.memory, h->g_allocator);

  log_info(LOG_SOURCE, "Loaded skin from memory into layer %d", layer);
  return layer;
}

int renderer_load_skin_from_file(gfx_handler_t *h, const char *path, texture_t **out_preview_texture) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (out_preview_texture) *out_preview_texture = NULL;
    return -1;
  }
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buffer = malloc(size);
  fread(buffer, size, 1, f);
  fclose(f);
  int id = renderer_load_skin_from_memory(h, buffer, size, out_preview_texture);
  free(buffer);
  return id;
}

void renderer_unload_skin(gfx_handler_t *h, int layer) {
  renderer_state_t *r = &h->renderer;
  skin_manager_free_layer(r, layer);
  log_info(LOG_SOURCE, "Freed skin layer %d", layer);
}

void renderer_init_atlas_renderer(gfx_handler_t *h, atlas_renderer_t *ar, const char *atlas_path, const sprite_definition_t *sprites,
                                  uint32_t sprite_count, uint32_t max_instances) {
  ar->shader = renderer_load_shader(h, "data/shaders/atlas.vert.spv", "data/shaders/atlas.frag.spv");
  ar->max_instances = max_instances;

  ar->sprite_count = sprite_count;
  ar->sprite_definitions = malloc(sizeof(sprite_definition_t) * sprite_count);
  memcpy(ar->sprite_definitions, sprites, sizeof(sprite_definition_t) * sprite_count);

  texture_t *source_atlas = renderer_load_texture(h, atlas_path);
  if (!source_atlas) {
    log_error(LOG_SOURCE, "Failed to load source atlas %s for array creation.", atlas_path);
    return;
  }

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

  renderer_destroy_texture(h, source_atlas);

  ar->atlas_texture->image_view = create_image_view(h, ar->atlas_texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                    ar->atlas_texture->mip_levels, ar->atlas_texture->layer_count);
  ar->atlas_texture->sampler = create_texture_sampler(h, ar->atlas_texture->mip_levels, VK_FILTER_LINEAR);

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

void renderer_push_atlas_instance(atlas_renderer_t *ar, vec2 pos, vec2 size, float rotation, uint32_t sprite_index, bool tile_uv) {
  if (ar->instance_count >= ar->max_instances) {
    log_warn(LOG_SOURCE, "Max atlas instances reached for this renderer.");
    return;
  }
  if (sprite_index >= ar->sprite_count) {
    log_error(LOG_SOURCE, "Invalid sprite_index %d for atlas renderer.", sprite_index);
    return;
  }
  uint32_t i = ar->instance_count++;
  glm_vec2_copy(pos, ar->instance_ptr[i].pos);
  glm_vec2_copy(size, ar->instance_ptr[i].size);
  ar->instance_ptr[i].rotation = rotation;
  ar->instance_ptr[i].sprite_index = (int)sprite_index;

  // uv calc
  float layer_w = (float)ar->layer_width;
  float layer_h = (float)ar->layer_height;
  float sprite_w = (float)ar->sprite_definitions[sprite_index].w;
  float sprite_h = (float)ar->sprite_definitions[sprite_index].h;
  float padding = 1.0f; // Matches init function

  // Calculate scaling factors
  // This ensures 0..1 UV maps exactly to the sprite's content, ignoring padding.
  ar->instance_ptr[i].uv_scale[0] = sprite_w / layer_w;
  ar->instance_ptr[i].uv_scale[1] = sprite_h / layer_h;

  // Calculate offsets
  // Pushes the UVs start point past the transparent padding.
  ar->instance_ptr[i].uv_offset[0] = padding / layer_w;
  ar->instance_ptr[i].uv_offset[1] = padding / layer_h;

  // Handle hook chain. hook chain is 1.5 stretched
  if (tile_uv) {
    ar->instance_ptr[i].tiling[0] = (size[0] * 1.5f);
    ar->instance_ptr[i].tiling[1] = 1.0f;
  } else {
    ar->instance_ptr[i].tiling[0] = 1.0f;
    ar->instance_ptr[i].tiling[1] = 1.0f;
  }
}

void renderer_flush_atlas_instances(gfx_handler_t *h, VkCommandBuffer cmd, atlas_renderer_t *ar, bool screen_space) {
  renderer_state_t *renderer = &h->renderer;
  if (ar->instance_count == 0 || !ar->shader || !ar->atlas_texture) return;

  mesh_t *quad = h->quad_mesh;

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, ar->shader, 1, 1, atlas_binding_desc, 2, atlas_attrib_descs, 8);
  if (!pso) return;

  primitive_ubo_t ubo;
  if (screen_space) {
    ubo.camPos[0] = 0.5f;
    ubo.camPos[1] = 0.5f;
    ubo.zoom = 2.0f;
    ubo.aspect = 1.0f;
    ubo.maxMapSize = 1.0f;
    ubo.mapSize[0] = h->viewport[0];
    ubo.mapSize[1] = h->viewport[1];
    ubo.lod_bias = 0.0f;
  } else {
    ubo.camPos[0] = renderer->camera.pos[0];
    ubo.camPos[1] = renderer->camera.pos[1];
    ubo.zoom = renderer->camera.zoom;
    float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
    float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
    ubo.aspect = window_ratio / map_ratio;
    ubo.maxMapSize = fmaxf(h->map_data->width, h->map_data->height) * 0.001f;
    ubo.mapSize[0] = h->map_data->width;
    ubo.mapSize[1] = h->map_data->height;
    ubo.lod_bias = renderer->lod_bias;
  }
  glm_ortho_rh_no(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned = (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  if (renderer->ubo_buffer_offset + aligned > DYNAMIC_UBO_BUFFER_SIZE) return;
  uint32_t dyn_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dyn_offset, &ubo, ubo_size);
  renderer->ubo_buffer_offset += aligned;

  uint32_t pool_idx = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSet desc;
  VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                    .descriptorPool = renderer->frame_descriptor_pools[pool_idx],
                                    .descriptorSetCount = 1,
                                    .pSetLayouts = &pso->descriptor_set_layout};
  check_vk_result(vkAllocateDescriptorSets(h->g_device, &ai, &desc));

  VkDescriptorBufferInfo bufInfo = {.buffer = renderer->dynamic_ubo_buffer.buffer, .offset = dyn_offset, .range = sizeof(primitive_ubo_t)};
  VkDescriptorImageInfo imgInfo = {
      .sampler = ar->sampler, .imageView = ar->atlas_texture->image_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .descriptorCount = 1,
                                     .pBufferInfo = &bufInfo},
                                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .pImageInfo = &imgInfo}};
  vkUpdateDescriptorSets(h->g_device, 2, writes, 0, NULL);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkBuffer bufs[2] = {quad->vertex_buffer.buffer, ar->instance_buffer.buffer};
  VkDeviceSize offs[2] = {0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
  vkCmdBindIndexBuffer(cmd, quad->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 0, NULL);
  vkCmdDrawIndexed(cmd, quad->index_count, ar->instance_count, 0, 0, 0);

  ar->instance_count = 0;
}
