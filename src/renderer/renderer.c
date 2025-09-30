#include "renderer.h"
#include "graphics_backend.h"
#include <cglm/cglm.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DYNAMIC_UBO_BUFFER_SIZE (16 * 1024 * 1024) // 16 MB

// helper function prototypes
static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter,
                                 VkMemoryPropertyFlags properties);
static void create_buffer(gfx_handler_t *handler, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, buffer_t *buffer);
static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool);
static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool,
                                     VkCommandBuffer command_buffer);
static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer,
                        VkDeviceSize size);

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image,
                                    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout,
                                    uint32_t mip_levels, uint32_t base_layer, uint32_t layer_count);
static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image,
                                 uint32_t width, uint32_t height);
static char *read_file(const char *filename, size_t *length);
static VkShaderModule create_shader_module(gfx_handler_t *handler, const char *code, size_t code_size);
static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height,
                          uint32_t mip_levels, uint32_t layer_count);
static pipeline_cache_entry_t *
get_or_create_pipeline(gfx_handler_t *handler, shader_t *shader, uint32_t ubo_count, uint32_t texture_count,
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

static void setup_vertex_descriptions();

void check_vk_result(VkResult err) {
  if (err == VK_SUCCESS)
    return;
  fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
  if (err < 0)
    abort();
}
void check_vk_result_line(VkResult err, int line) {
  if (err == VK_SUCCESS)
    return;
  fprintf(stderr, "[vulkan] Error: VkResult = %d in line: (%d)\n", err, line);
  if (err < 0)
    abort();
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter,
                                 VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties mem_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

  for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
    if ((type_filter & (1 << i)) &&
        (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  fprintf(stderr, "Failed to find suitable memory type!\n");
  exit(EXIT_FAILURE);
}

static void create_buffer(gfx_handler_t *handler, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, buffer_t *buffer) {
  VkResult err;
  buffer->size = size;

  VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = size,
                                    .usage = usage,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

  err = vkCreateBuffer(handler->g_device, &buffer_info, handler->g_allocator, &buffer->buffer);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(handler->g_device, buffer->buffer, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          find_memory_type(handler->g_physical_device, mem_requirements.memoryTypeBits, properties)};

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

  VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  vkBeginCommandBuffer(command_buffer, &begin_info);
  return command_buffer;
}

static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool,
                                     VkCommandBuffer command_buffer) {
  vkEndCommandBuffer(command_buffer);

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command_buffer};

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

static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer,
                        VkDeviceSize size) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferCopy copy_region = {.size = size};
  vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image,
                                    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout,
                                    uint32_t mip_levels, uint32_t base_layer, uint32_t layer_count) {
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
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    fprintf(stderr, "Unsupported layout transition!\n");
    abort();
  }

  vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, pool, command_buffer);
}

static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image,
                                 uint32_t width, uint32_t height) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferImageCopy region = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
                              .imageExtent = {width, height, 1}};

  vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  end_single_time_commands(handler, pool, command_buffer);
}

void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels,
                  uint32_t array_layers, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkImage *image, VkDeviceMemory *image_memory) {
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

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          find_memory_type(handler->g_physical_device, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_device, &alloc_info, handler->g_allocator, image_memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindImageMemory(handler->g_device, *image, *image_memory, 0);
  check_vk_result_line(err, __LINE__);
}

VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format,
                              VkImageViewType view_type, uint32_t mip_levels, uint32_t layer_count) {
  VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image,
                                     .viewType = view_type,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = mip_levels,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = layer_count}};

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
    fprintf(stderr, "Failed to open file: %s\n", filename);
    return NULL;
  }

  fseek(file, 0, SEEK_END);
  *length = ftell(file);
  fseek(file, 0, SEEK_SET);

  char *buffer = (char *)malloc(*length);
  if (!buffer) {
    fprintf(stderr, "Failed to allocate memory for file: %s\n", filename);
    fclose(file);
    return NULL;
  }

  size_t read_count = fread(buffer, 1, *length, file);
  fclose(file);

  if (read_count != *length) {
    fprintf(stderr, "Failed to read entire file: %s\n", filename);
    free(buffer);
    return NULL;
  }

  return buffer;
}

static VkShaderModule create_shader_module(gfx_handler_t *handler, const char *code, size_t code_size) {
  VkShaderModuleCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = code_size,
                                          .pCode = (const uint32_t *)code};

  VkShaderModule shader_module;
  VkResult err = vkCreateShaderModule(handler->g_device, &create_info, handler->g_allocator, &shader_module);
  check_vk_result_line(err, __LINE__);

  return shader_module;
}

static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height,
                          uint32_t mip_levels, uint32_t layer_count) {
  if (mip_levels <= 1)
    return true;

  VkCommandBuffer cmd_buffer = begin_single_time_commands(handler, handler->renderer.transfer_command_pool);

  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .image = image,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = layer_count,
                                                       .levelCount = 1}};

  int32_t mip_width = width;
  int32_t mip_height = height;

  for (uint32_t i = 1; i < mip_levels; i++) {
    barrier.subresourceRange.baseMipLevel = i - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);

    VkImageBlit blit = {
        .srcOffsets[0] = {0, 0, 0},
        .srcOffsets[1] = {mip_width, mip_height, 1},
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = i - 1,
                           .baseArrayLayer = 0,
                           .layerCount = layer_count},
        .dstOffsets[0] = {0, 0, 0},
        .dstOffsets[1] = {mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = i,
                           .baseArrayLayer = 0,
                           .layerCount = layer_count}};

    vkCmdBlitImage(cmd_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);

    if (mip_width > 1)
      mip_width /= 2;
    if (mip_height > 1)
      mip_height /= 2;
  }

  barrier.subresourceRange.baseMipLevel = mip_levels - 1;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, handler->renderer.transfer_command_pool, cmd_buffer);
  return true;
}

texture_t *renderer_create_texture_2d_array(gfx_handler_t *handler, uint32_t width, uint32_t height,
                                            uint32_t layer_count, VkFormat format) {
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
    fprintf(stderr, "Max texture count reached.\n");
    return NULL;
  }

  texture_t *texArray = &renderer->textures[free_slot];
  memset(texArray, 0, sizeof(texture_t));
  texArray->id = free_slot;
  texArray->active = true;
  texArray->width = width;
  texArray->height = height;
  texArray->mip_levels = 1; // can expand to mip chain later
  texArray->layer_count = layer_count;
  strncpy(texArray->path, "runtime_skin_array", sizeof(texArray->path) - 1);

  // Create the VkImage (2D array)
  create_image(handler, width, height, texArray->mip_levels, layer_count, format, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texArray->image, &texArray->memory);

  // Transition all layers once
  transition_image_layout(handler, renderer->transfer_command_pool, texArray->image, format,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          texArray->mip_levels, 0, layer_count);
  // Transition to shader read (empty until uploads)
  transition_image_layout(handler, renderer->transfer_command_pool, texArray->image, format,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          texArray->mip_levels, 0, layer_count);

  // Create view as 2D array
  texArray->image_view = create_image_view(handler, texArray->image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                           texArray->mip_levels, layer_count);

  // Create sampler
  texArray->sampler = create_texture_sampler(handler, texArray->mip_levels, VK_FILTER_LINEAR);

  printf("Created 2D texture array (%ux%u x %u layers)\n", width, height, layer_count);

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

  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                       .queueFamilyIndex = handler->g_queue_family};
  check_vk_result(vkCreateCommandPool(handler->g_device, &pool_info, handler->g_allocator,
                                      &renderer->transfer_command_pool));

  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 * MAX_TEXTURES_PER_DRAW}};
  for (int i = 0; i < 3; i++) { // triple buffering
    VkDescriptorPoolCreateInfo pool_create_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                   .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                   .maxSets = 100,
                                                   .poolSizeCount =
                                                       sizeof(pool_sizes) / sizeof(pool_sizes[0]),
                                                   .pPoolSizes = pool_sizes};
    check_vk_result(vkCreateDescriptorPool(handler->g_device, &pool_create_info, handler->g_allocator,
                                           &renderer->frame_descriptor_pools[i]));
  }
  unsigned char white_pixel[] = {255, 255, 255, 255};
  texture_t *default_tex = renderer_load_texture_from_array(handler, white_pixel, 1, 1);
  strncpy(default_tex->path, "default_white", sizeof(default_tex->path) - 1);
  renderer->default_texture = default_tex;

  // Primitive & UBO Ring Buffer Setup ---
  renderer->primitive_shader =
      renderer_load_shader(handler, "data/shaders/primitive.vert.spv", "data/shaders/primitive.frag.spv");

  create_buffer(handler, MAX_PRIMITIVE_VERTICES * sizeof(primitive_vertex_t),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderer->dynamic_vertex_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_vertex_buffer.memory, 0, VK_WHOLE_SIZE, 0,
              (void **)&renderer->vertex_buffer_ptr);

  create_buffer(handler, MAX_PRIMITIVE_INDICES * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderer->dynamic_index_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_index_buffer.memory, 0, VK_WHOLE_SIZE, 0,
              (void **)&renderer->index_buffer_ptr);

  create_buffer(handler, DYNAMIC_UBO_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderer->dynamic_ubo_buffer);
  vkMapMemory(handler->g_device, renderer->dynamic_ubo_buffer.memory, 0, VK_WHOLE_SIZE, 0,
              &renderer->ubo_buffer_ptr);

  // create a 2d array texture to hold max_skins atlases (each 512x256, rgba8)
  renderer->skin_manager.atlas_array =
      renderer_create_texture_2d_array(handler, 512, 256, MAX_SKINS, VK_FORMAT_R8G8B8A8_UNORM);
  memset(renderer->skin_manager.layer_used, 0, sizeof(renderer->skin_manager.layer_used));
  // skin renderer
  renderer->skin_renderer.skin_shader =
      renderer_load_shader(handler, "data/shaders/skin.vert.spv", "data/shaders/skin.frag.spv");

  // allocate big instance buffer
  create_buffer(handler, sizeof(skin_instance_t) * 100000, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderer->skin_renderer.instance_buffer);
  vkMapMemory(handler->g_device, renderer->skin_renderer.instance_buffer.memory, 0, VK_WHOLE_SIZE, 0,
              (void **)&renderer->skin_renderer.instance_ptr);

  renderer->skin_renderer.instance_count = 0;

  printf("Renderer initialized.\n");
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

  printf("Renderer cleaned up.\n");
}

static pipeline_cache_entry_t *
get_or_create_pipeline(gfx_handler_t *handler, shader_t *shader, uint32_t ubo_count, uint32_t texture_count,
                       const VkVertexInputBindingDescription *binding_descs, uint32_t binding_desc_count,
                       const VkVertexInputAttributeDescription *attrib_descs, uint32_t attrib_desc_count) {
  renderer_state_t *renderer = &handler->renderer;
  pipeline_cache_entry_t *entry = &renderer->pipeline_cache[shader->id];

  if (entry->initialized && entry->ubo_count == ubo_count && entry->texture_count == texture_count) {
    return entry;
  }

  if (entry->initialized) {
    vkDestroyPipeline(handler->g_device, entry->pipeline, handler->g_allocator);
    vkDestroyPipelineLayout(handler->g_device, entry->pipeline_layout, handler->g_allocator);
    vkDestroyDescriptorSetLayout(handler->g_device, entry->descriptor_set_layout, handler->g_allocator);
  }

  entry->ubo_count = ubo_count;
  entry->texture_count = texture_count;

  uint32_t binding_count = ubo_count + texture_count;
  VkDescriptorSetLayoutBinding bindings[binding_count];
  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    bindings[current_binding++] = (VkDescriptorSetLayoutBinding){
        .binding = current_binding - 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, // MODIFIED
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  }
  for (uint32_t i = 0; i < texture_count; ++i) {
    bindings[current_binding++] =
        (VkDescriptorSetLayoutBinding){.binding = current_binding - 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = binding_count,
                                                 .pBindings = bindings};
  check_vk_result(vkCreateDescriptorSetLayout(handler->g_device, &layout_info, handler->g_allocator,
                                              &entry->descriptor_set_layout));

  VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &entry->descriptor_set_layout};
  check_vk_result(vkCreatePipelineLayout(handler->g_device, &pipeline_layout_info, handler->g_allocator,
                                         &entry->pipeline_layout));

  VkPipelineShaderStageCreateInfo vert_shader_stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = shader->vert_shader_module,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo frag_shader_stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = shader->frag_shader_module,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = binding_desc_count,
      .pVertexBindingDescriptions = binding_descs,
      .vertexAttributeDescriptionCount = attrib_desc_count,
      .pVertexAttributeDescriptions = attrib_descs};

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable = VK_FALSE};

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};

  VkPipelineRasterizationStateCreateInfo rasterizer = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .depthClampEnable = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f};

  VkPipelineMultisampleStateCreateInfo multisampling = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};

  VkPipelineColorBlendAttachmentState color_blend_attachment = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blending = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color_blend_attachment};

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {.sType =
                                                        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                    .dynamicStateCount = 2,
                                                    .pDynamicStates = dynamic_states};

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
                                                .renderPass = handler->g_main_window_data.RenderPass,
                                                .subpass = 0};

  check_vk_result(vkCreateGraphicsPipelines(handler->g_device, handler->g_pipeline_cache, 1, &pipeline_info,
                                            handler->g_allocator, &entry->pipeline));

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
    fprintf(stderr, "Max shader count reached.\n");
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

texture_t *renderer_load_compact_texture_from_array(gfx_handler_t *handler, const uint8_t **pixel_array,
                                                    uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (!pixel_array)
    return NULL;

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    fprintf(stderr, "Max texture count reached.\n");
    return NULL;
  }

  VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

  stbi_uc *rgba_pixels = calloc(1, image_size);
  if (height == 1 && width == 1) { // Special case for default texture
    memcpy(rgba_pixels, pixel_array, image_size);
  } else {
    for (uint32_t i = 0; i < width * height; i++) {
      if (pixel_array[0])
        rgba_pixels[i * 4 + 0] = pixel_array[0][i];
      if (pixel_array[1])
        rgba_pixels[i * 4 + 1] = pixel_array[1][i];
      if (pixel_array[2])
        rgba_pixels[i * 4 + 2] = pixel_array[2][i];
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
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  free(rgba_pixels);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width,
                       height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                          0, 1);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  texture->image_view =
      create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
  texture->sampler = create_texture_sampler(handler, 1, VK_FILTER_NEAREST);

  return texture;
}

texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array,
                                            uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (!pixel_array)
    return NULL;

  uint32_t free_slot = (uint32_t)-1;
  for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
    if (!renderer->textures[i].active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot == (uint32_t)-1) {
    fprintf(stderr, "Max texture count reached.\n");
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
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  free(rgba_pixels);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width,
                       height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                          0, 1);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  texture->image_view =
      create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
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
    fprintf(stderr, "Max texture count reached.\n");
    return NULL;
  }

  int tex_width, tex_height, tex_channels;
  stbi_uc *pixels = stbi_load(image_path, &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
  if (!pixels) {
    fprintf(stderr, "Failed to load texture image: %s\n", image_path);
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
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, image_size);
  vkUnmapMemory(handler->g_device, staging_buffer.memory);
  stbi_image_free(pixels);

  create_image(handler, tex_width, tex_height, mip_levels, 1, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, 0, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image,
                       tex_width, tex_height);

  vkDestroyBuffer(handler->g_device, staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, staging_buffer.memory, handler->g_allocator);

  if (!build_mipmaps(handler, texture->image, tex_width, tex_height, mip_levels, 1)) {
    transition_image_layout(handler, renderer->transfer_command_pool, texture->image,
                            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mip_levels, 0, 1);
  }

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_VIEW_TYPE_2D, mip_levels, 1);
  texture->sampler = create_texture_sampler(handler, mip_levels, VK_FILTER_LINEAR);

  printf("Loaded texture: %s\n", image_path);
  return texture;
}

mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count,
                             uint32_t *indices, uint32_t index_count) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->mesh_count >= MAX_MESHES) {
    fprintf(stderr, "Maximum mesh count (%d) reached.\n", MAX_MESHES);
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
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &vertex_staging_buffer);
  void *data;
  vkMapMemory(handler->g_device, vertex_staging_buffer.memory, 0, vertex_buffer_size, 0, &data);
  memcpy(data, vertices, (size_t)vertex_buffer_size);
  vkUnmapMemory(handler->g_device, vertex_staging_buffer.memory);

  create_buffer(handler, vertex_buffer_size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->vertex_buffer);

  copy_buffer(handler, renderer->transfer_command_pool, vertex_staging_buffer.buffer,
              mesh->vertex_buffer.buffer, vertex_buffer_size);

  vkDestroyBuffer(handler->g_device, vertex_staging_buffer.buffer, handler->g_allocator);
  vkFreeMemory(handler->g_device, vertex_staging_buffer.memory, handler->g_allocator);

  if (index_count > 0 && indices) {
    create_buffer(handler, index_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &index_staging_buffer);
    vkMapMemory(handler->g_device, index_staging_buffer.memory, 0, index_buffer_size, 0, &data);
    memcpy(data, indices, (size_t)index_buffer_size);
    vkUnmapMemory(handler->g_device, index_staging_buffer.memory);

    create_buffer(handler, index_buffer_size,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->index_buffer);

    copy_buffer(handler, renderer->transfer_command_pool, index_staging_buffer.buffer,
                mesh->index_buffer.buffer, index_buffer_size);

    vkDestroyBuffer(handler->g_device, index_staging_buffer.buffer, handler->g_allocator);
    vkFreeMemory(handler->g_device, index_staging_buffer.memory, handler->g_allocator);
  } else {
    mesh->index_count = 0;
  }

  printf("Created mesh: V=%u, I=%u\n", vertex_count, index_count);
  return mesh;
}

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &handler->renderer;
  uint32_t frame_pool_index = handler->g_main_window_data.FrameIndex % 3;
  check_vk_result(
      vkResetDescriptorPool(handler->g_device, renderer->frame_descriptor_pools[frame_pool_index], 0));
  renderer->primitive_vertex_count = 0;
  renderer->primitive_index_count = 0;
  renderer->ubo_buffer_offset = 0;
  renderer->current_command_buffer = command_buffer;

  VkViewport viewport = {0.f, 0.f, handler->viewport[0], handler->viewport[1], 0.0f, 1.0f};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor = {{0.0, 0.0}, {handler->viewport[0], handler->viewport[1]}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh,
                        shader_t *shader, texture_t **textures, uint32_t texture_count, void **ubos,
                        VkDeviceSize *ubo_sizes, uint32_t ubo_count) {
  if (!mesh || !shader || !mesh->active || !shader->active)
    return;
  renderer_state_t *renderer = &handler->renderer;

  pipeline_cache_entry_t *pso = get_or_create_pipeline(handler, shader, ubo_count, texture_count,
                                                       &mesh_binding_description, 1, // one binding
                                                       mesh_attribute_descriptions, 3);

  // Allocate a descriptor set for this pipeline
  VkDescriptorSet descriptor_set;
  uint32_t frame_pool_index = handler->g_main_window_data.FrameIndex % 3;
  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool =
                                                renderer->frame_descriptor_pools[frame_pool_index],
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &pso->descriptor_set_layout};
  check_vk_result(vkAllocateDescriptorSets(handler->g_device, &alloc_info, &descriptor_set));

  uint32_t binding_count = ubo_count + texture_count;
  VkWriteDescriptorSet descriptor_writes[binding_count];
  VkDescriptorBufferInfo buffer_infos[ubo_count];
  VkDescriptorImageInfo image_infos[texture_count];
  uint32_t dynamic_offsets[ubo_count];

  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < ubo_count; ++i) {
    // UBO RING BUFFER LOGIC ---
    VkDeviceSize aligned_size =
        (ubo_sizes[i] + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
    assert(renderer->ubo_buffer_offset + aligned_size <= DYNAMIC_UBO_BUFFER_SIZE);

    dynamic_offsets[i] = renderer->ubo_buffer_offset;
    memcpy((char *)renderer->ubo_buffer_ptr + dynamic_offsets[i], ubos[i], ubo_sizes[i]);
    renderer->ubo_buffer_offset += aligned_size;

    buffer_infos[i] = (VkDescriptorBufferInfo){
        .buffer = renderer->dynamic_ubo_buffer.buffer, .offset = 0, .range = ubo_sizes[i]};
    descriptor_writes[current_binding++] =
        (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = descriptor_set,
                               .dstBinding = current_binding - 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                               .descriptorCount = 1,
                               .pBufferInfo = &buffer_infos[i]};
  }
  for (uint32_t i = 0; i < texture_count; ++i) {
    image_infos[i] = (VkDescriptorImageInfo){.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             .imageView = textures[i]->image_view,
                                             .sampler = textures[i]->sampler};
    descriptor_writes[current_binding++] =
        (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = descriptor_set,
                               .dstBinding = current_binding - 1,
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

  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1,
                          &descriptor_set, ubo_count, dynamic_offsets);

  if (mesh->index_count > 0) {
    vkCmdDrawIndexed(command_buffer, mesh->index_count, 1, 0, 0, 0);
  } else {
    vkCmdDraw(command_buffer, mesh->vertex_count, 1, 0, 0);
  }
}

void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  flush_primitives(handler, command_buffer);
}

void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex) {
  if (!tex || !tex->active)
    return;
  // Delay actual destruction until it's safe
  gfx_handler_t *h = handler;
  if (h->retire_count < 64) {
    h->retire_textures[h->retire_count].tex = tex;
    h->retire_textures[h->retire_count].frame_index = h->g_main_window_data.FrameIndex;
    h->retire_count++;
  }
}

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas,
                                                    uint32_t tile_width, uint32_t tile_height,
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
    fprintf(stderr, "Max texture count reached.\n");
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

  create_image(handler, tile_width, tile_height, mip_levels, layer_count, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tex_array->image, &tex_array->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, tex_array->image,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, 0, layer_count);

  VkCommandBuffer cmd = begin_single_time_commands(handler, renderer->transfer_command_pool);
  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .image = atlas->image,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = atlas->mip_levels,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                       0, NULL, 1, &barrier);

  for (uint32_t layer = 0; layer < layer_count; layer++) {
    uint32_t tile_x = layer % num_tiles_x;
    uint32_t tile_y = layer / num_tiles_x;

    VkImageCopy copy_region = {
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .srcOffset = {(int32_t)(tile_x * tile_width), (int32_t)(tile_y * tile_height), 0},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseArrayLayer = layer, .layerCount = 1},
        .extent = {tile_width, tile_height, 1}};
    vkCmdCopyImage(cmd, atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_array->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
  }

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                       0, NULL, 1, &barrier);

  end_single_time_commands(handler, renderer->transfer_command_pool, cmd);

  build_mipmaps(handler, tex_array->image, tile_width, tile_height, mip_levels, layer_count);

  tex_array->image_view = create_image_view(handler, tex_array->image, VK_FORMAT_R8G8B8A8_UNORM,
                                            VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip_levels, layer_count);
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
  primitive_binding_description = (VkVertexInputBindingDescription){
      .binding = 0, .stride = sizeof(primitive_vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  primitive_attribute_descriptions[0] =
      (VkVertexInputAttributeDescription){.binding = 0,
                                          .location = 0,
                                          .format = VK_FORMAT_R32G32_SFLOAT,
                                          .offset = offsetof(primitive_vertex_t, pos)};
  primitive_attribute_descriptions[1] =
      (VkVertexInputAttributeDescription){.binding = 0,
                                          .location = 1,
                                          .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                          .offset = offsetof(primitive_vertex_t, color)};

  mesh_binding_description = (VkVertexInputBindingDescription){
      .binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  mesh_attribute_descriptions[0] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  mesh_attribute_descriptions[1] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vertex_t, color)};
  mesh_attribute_descriptions[2] =
      (VkVertexInputAttributeDescription){.binding = 0,
                                          .location = 2,
                                          .format = VK_FORMAT_R32G32_SFLOAT,
                                          .offset = offsetof(vertex_t, tex_coord)};

  // skin instanced data ---
  skin_binding_desc[0] = (VkVertexInputBindingDescription){
      .binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  skin_binding_desc[1] = (VkVertexInputBindingDescription){
      .binding = 1, .stride = sizeof(skin_instance_t), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};

  int i = 0;
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){
      .binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vertex_t, pos)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 1,
                                                               .format = VK_FORMAT_R32G32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, pos)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 2,
                                                               .format = VK_FORMAT_R32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, scale)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1,
                                          .location = 3,
                                          .format = VK_FORMAT_R32_SINT,
                                          .offset = offsetof(skin_instance_t, skin_index)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1,
                                          .location = 4,
                                          .format = VK_FORMAT_R32_SINT,
                                          .offset = offsetof(skin_instance_t, eye_state)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 5,
                                                               .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, body)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1,
                                          .location = 6,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT,
                                          .offset = offsetof(skin_instance_t, back_foot)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1,
                                          .location = 7,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT,
                                          .offset = offsetof(skin_instance_t, front_foot)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 8,
                                                               .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, attach)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 9,
                                                               .format = VK_FORMAT_R32G32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, dir)};
  // colors
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 10,
                                                               .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, col_body)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 11,
                                                               .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                               .offset = offsetof(skin_instance_t, col_feet)};
  skin_attrib_descs[i++] =
      (VkVertexInputAttributeDescription){.binding = 1,
                                          .location = 12,
                                          .format = VK_FORMAT_R32_SINT,
                                          .offset = offsetof(skin_instance_t, col_custom)};
  skin_attrib_descs[i++] = (VkVertexInputAttributeDescription){.binding = 1,
                                                               .location = 13,
                                                               .format = VK_FORMAT_R32_SINT,
                                                               .offset = offsetof(skin_instance_t, col_gs)};
}

// primitive drawing implementation
static void flush_primitives(gfx_handler_t *h, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &h->renderer;
  if (renderer->primitive_index_count == 0) {
    return;
  }

  pipeline_cache_entry_t *pso = get_or_create_pipeline(h, renderer->primitive_shader, 1, 0,
                                                       &primitive_binding_description, 1, // one binding
                                                       primitive_attribute_descriptions, 2);

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

  glm_ortho_rh_no(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned_size =
      (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
  assert(renderer->ubo_buffer_offset + aligned_size <= DYNAMIC_UBO_BUFFER_SIZE);

  uint32_t dynamic_offset = renderer->ubo_buffer_offset;
  memcpy((char *)renderer->ubo_buffer_ptr + dynamic_offset, &ubo, ubo_size);
  renderer->ubo_buffer_offset += aligned_size;

  VkDescriptorSet descriptor_set;

  uint32_t frame_pool_index = h->g_main_window_data.FrameIndex % 3;
  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool =
                                                renderer->frame_descriptor_pools[frame_pool_index],
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &pso->descriptor_set_layout};

  check_vk_result(vkAllocateDescriptorSets(h->g_device, &alloc_info, &descriptor_set));

  VkDescriptorBufferInfo buffer_info = {
      .buffer = renderer->dynamic_ubo_buffer.buffer, .offset = 0, .range = sizeof(primitive_ubo_t)};
  VkWriteDescriptorSet descriptor_write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = descriptor_set,
                                           .dstBinding = 0,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                           .descriptorCount = 1,
                                           .pBufferInfo = &buffer_info};
  vkUpdateDescriptorSets(h->g_device, 1, &descriptor_write, 0, NULL);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(command_buffer, 0, 1, &renderer->dynamic_vertex_buffer.buffer, offsets);
  vkCmdBindIndexBuffer(command_buffer, renderer->dynamic_index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1,
                          &descriptor_set, 1, &dynamic_offset);
  vkCmdDrawIndexed(command_buffer, renderer->primitive_index_count, 1, 0, 0, 0);

  renderer->primitive_vertex_count = 0;
  renderer->primitive_index_count = 0;
}

void renderer_draw_rect_filled(gfx_handler_t *handler, vec2 pos, vec2 size, vec4 color) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->primitive_vertex_count + 4 > MAX_PRIMITIVE_VERTICES ||
      renderer->primitive_index_count + 6 > MAX_PRIMITIVE_INDICES) {
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
void renderer_draw_circle_filled(gfx_handler_t *handler, vec2 center, float radius, vec4 color,
                                 uint32_t segments) {
  renderer_state_t *renderer = &handler->renderer;
  if (segments < 3)
    segments = 3;

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
  if (renderer->primitive_vertex_count + 4 > MAX_PRIMITIVE_VERTICES ||
      renderer->primitive_index_count + 6 > MAX_PRIMITIVE_INDICES) {
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
  if (pix_per_unit_p1 < EPS)
    pix_per_unit_p1 = (pix_per_unit_p2 > EPS ? pix_per_unit_p2 : 1.0f);
  if (pix_per_unit_p2 < EPS)
    pix_per_unit_p2 = (pix_per_unit_p1 > EPS ? pix_per_unit_p1 : 1.0f);

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
  if (!h->map_shader || !h->quad_mesh || h->map_texture_count <= 0)
    return;

  float window_ratio = (float)h->viewport[0] / (float)h->viewport[1];
  float map_ratio = (float)h->map_data->width / (float)h->map_data->height;
  if (isnan(map_ratio) || map_ratio == 0)
    map_ratio = 1.0f;

  float zoom = 1.0 / (h->renderer.camera.zoom * fmax(h->map_data->width, h->map_data->height) * 0.001);
  if (isnan(zoom))
    zoom = 1.0f;

  float aspect = 1.0f / (window_ratio / map_ratio);
  float lod =
      fmin(fmax(5.5f - log2f((1.0f / h->map_data->width) / zoom * (h->viewport[0] / 2.0f)), 0.0f), 6.0f);

  map_buffer_object_t ubo = {.transform = {h->renderer.camera.pos[0], h->renderer.camera.pos[1], zoom},
                             .aspect = aspect,
                             .lod = lod};

  void *ubos[] = {&ubo};
  VkDeviceSize ubo_sizes[] = {sizeof(ubo)};
  renderer_draw_mesh(h, h->current_frame_command_buffer, h->quad_mesh, h->map_shader, h->map_textures,
                     h->map_texture_count, ubos, ubo_sizes, 1);
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

void renderer_push_skin_instance(gfx_handler_t *h, vec2 pos, float scale, int skin_index, int eye_state,
                                 vec2 dir, const anim_state_t *anim_state, vec3 col_body, vec3 col_feet,
                                 bool use_custom_color) {
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
  if (sr->instance_count == 0)
    return;

  mesh_t *quad = h->quad_mesh;

  // pipeline: 1 UBO + 1 texture
  pipeline_cache_entry_t *pso =
      get_or_create_pipeline(h, sr->skin_shader, 1, 1, skin_binding_desc, 2, skin_attrib_descs, 14);

  // prepare camera UBO (same as primitives) ---
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
  glm_ortho_rh_no(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, ubo.proj);

  // copy into dynamic UBO ring
  VkDeviceSize ubo_size = sizeof(ubo);
  VkDeviceSize aligned = (ubo_size + renderer->min_ubo_alignment - 1) & ~(renderer->min_ubo_alignment - 1);
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
  VkDescriptorBufferInfo bufInfo = {
      .buffer = renderer->dynamic_ubo_buffer.buffer, .offset = 0, .range = sizeof(primitive_ubo_t)};
  VkDescriptorImageInfo img = {.sampler = skin_array->sampler,
                               .imageView = skin_array->image_view,
                               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                     .descriptorCount = 1,
                                     .pBufferInfo = &bufInfo},
                                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = desc,
                                     .dstBinding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .pImageInfo = &img}};
  vkUpdateDescriptorSets(h->g_device, 2, writes, 0, NULL);

  // issue draw ---
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline);
  VkBuffer bufs[2] = {quad->vertex_buffer.buffer, sr->instance_buffer.buffer};
  VkDeviceSize offs[2] = {0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
  vkCmdBindIndexBuffer(cmd, quad->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 1, &desc, 1,
                          &dyn_offset);
  vkCmdDrawIndexed(cmd, quad->index_count, sr->instance_count, 0, 0, 0);

  sr->instance_count = 0;
}

int renderer_load_skin_from_file(gfx_handler_t *h, const char *path) {
  int tex_width, tex_height, channels;
  stbi_uc *pixels = stbi_load(path, &tex_width, &tex_height, &channels, STBI_rgb_alpha);
  if (!pixels) {
    fprintf(stderr, "Failed to load skin: %s\n", path);
    return -1;
  }

  // check dimensions (must be multiple of 256x128)
  if (tex_width <= 0 || tex_height <= 0 || tex_width % 256 != 0 || tex_height % 128 != 0) {
    fprintf(stderr, "Skin %s has invalid dimensions (%dx%d), must be a multiple of 256x128\n", path,
            tex_width, tex_height);
    stbi_image_free(pixels);
    return -1;
  }

  stbi_uc *used_pixels = pixels;
  const int final_width = 512;
  const int final_height = 256;

  if (tex_width != final_width || tex_height != final_height) {
    printf("Info: resizing skin '%s' from %dx%d to %dx%d\n", path, tex_width, tex_height, final_width,
           final_height);
    stbi_uc *resized_pixels = malloc(final_width * final_height * 4);
    if (!resized_pixels) {
      fprintf(stderr, "Failed to allocate memory for skin resize.\n");
      stbi_image_free(pixels);
      return -1;
    }
    stbir_resize_uint8_srgb(pixels, tex_width, tex_height, 0, resized_pixels, final_width, final_height, 0,
                            4);
    used_pixels = resized_pixels; // from now on, use the resized pixels
  }

  renderer_state_t *r = &h->renderer;
  int layer = skin_manager_alloc_layer(r);
  if (layer < 0) {
    fprintf(stderr, "No free skin layers available\n");
    if (used_pixels != pixels)
      free(used_pixels);
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
      unsigned char a = pixels[idx + 3];
      if (a > 128) {
        unsigned char r = pixels[idx + 0];
        unsigned char g = pixels[idx + 1];
        unsigned char b = pixels[idx + 2];
        uint8_t gray = (uint8_t)(0.2126f * r + 0.7152f * g + 0.0722f * b);
        freq[gray]++;
      }
    }
  }
  uint8_t org_weight = 1;
  for (int i = 1; i < 256; ++i) {
    if (freq[org_weight] < freq[i]) {
      org_weight = (uint8_t)i;
    }
  }
  r->skin_manager.atlas_array[layer].gs_org = org_weight;

  // Create staging buffer
  VkDeviceSize image_size = final_width * final_height * 4;
  buffer_t staging;
  create_buffer(h, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging);

  void *data;
  vkMapMemory(h->g_device, staging.memory, 0, image_size, 0, &data);
  memcpy(data, used_pixels, image_size);
  vkUnmapMemory(h->g_device, staging.memory);
  if (used_pixels != pixels)
    free(used_pixels);     // free resized buffer
  stbi_image_free(pixels); // free original buffer

  // Transition -> TRANSFER_DST
  transition_image_layout(h, r->transfer_command_pool, r->skin_manager.atlas_array->image,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, layer, 1);

  // Copy staging buffer into array slice
  VkCommandBuffer cmd = begin_single_time_commands(h, r->transfer_command_pool);
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = 0,
                           .baseArrayLayer = layer,
                           .layerCount = 1},
      .imageExtent = {(uint32_t)final_width, (uint32_t)final_height, 1},
  };
  vkCmdCopyBufferToImage(cmd, staging.buffer, r->skin_manager.atlas_array->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  end_single_time_commands(h, r->transfer_command_pool, cmd);

  // Transition this layer back to shader-read
  transition_image_layout(h, r->transfer_command_pool, r->skin_manager.atlas_array->image,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, layer, 1);

  // Cleanup staging
  vkDestroyBuffer(h->g_device, staging.buffer, h->g_allocator);
  vkFreeMemory(h->g_device, staging.memory, h->g_allocator);

  printf("Loaded skin %s into layer %d\n", path, layer);
  return layer; // return usable skin index
}

void renderer_unload_skin(gfx_handler_t *h, int layer) {
  renderer_state_t *r = &h->renderer;
  skin_manager_free_layer(r, layer);
  printf("Freed skin layer %d\n", layer);
}
