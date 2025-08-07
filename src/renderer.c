#include <vulkan/vulkan_core.h>
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include "graphics_backend.h"
#include "renderer.h"
#include <cglm/cglm.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Helper Function Prototypes ---
static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter,
                                 VkMemoryPropertyFlags properties);
static void create_buffer(gfx_handler_t *handler, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, buffer_t *buffer);
static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool);
static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool,
                                     VkCommandBuffer command_buffer);
static void copy_buffer(gfx_handler_t *handler, VkCommandPool pool, VkBuffer src_buffer, VkBuffer dst_buffer,
                        VkDeviceSize size);
static void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels,
                         uint32_t array_layers, VkFormat format, VkImageTiling tiling,
                         VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *image,
                         VkDeviceMemory *image_memory);
static VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format,
                                     VkImageViewType view_type, uint32_t mip_levels, uint32_t layer_count);
static VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter);
static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image,
                                    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout,
                                    uint32_t mip_levels, uint32_t layer_count);
static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image,
                                 uint32_t width, uint32_t height);
static char *read_file(const char *filename, size_t *length);
static VkShaderModule create_shader_module(gfx_handler_t *handler, const char *code, size_t code_size);
static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height,
                          uint32_t mip_levels, uint32_t layer_count);

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

  err = vkCreateBuffer(handler->g_Device, &buffer_info, handler->g_Allocator, &buffer->buffer);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(handler->g_Device, buffer->buffer, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          find_memory_type(handler->g_PhysicalDevice, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_Device, &alloc_info, handler->g_Allocator, &buffer->memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindBufferMemory(handler->g_Device, buffer->buffer, buffer->memory, 0);
  check_vk_result_line(err, __LINE__);

  buffer->mapped_memory = NULL;
}

static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};

  VkCommandBuffer command_buffer;
  vkAllocateCommandBuffers(handler->g_Device, &alloc_info, &command_buffer);

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
  VkResult err = vkCreateFence(handler->g_Device, &fence_info, handler->g_Allocator, &fence);
  check_vk_result_line(err, __LINE__);

  err = vkQueueSubmit(handler->g_Queue, 1, &submit_info, fence);
  check_vk_result_line(err, __LINE__);

  err = vkWaitForFences(handler->g_Device, 1, &fence, VK_TRUE, UINT64_MAX);
  check_vk_result_line(err, __LINE__);

  vkDestroyFence(handler->g_Device, fence, handler->g_Allocator);
  vkFreeCommandBuffers(handler->g_Device, pool, 1, &command_buffer);
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
                                    uint32_t mip_levels, uint32_t layer_count) {
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
                                                       .baseArrayLayer = 0,
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
  } else {
    // This can be expanded as needed
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

static void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels,
                         uint32_t array_layers, VkFormat format, VkImageTiling tiling,
                         VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *image,
                         VkDeviceMemory *image_memory) {
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

  err = vkCreateImage(handler->g_Device, &image_info, handler->g_Allocator, image);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetImageMemoryRequirements(handler->g_Device, *image, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          find_memory_type(handler->g_PhysicalDevice, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_Device, &alloc_info, handler->g_Allocator, image_memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindImageMemory(handler->g_Device, *image, *image_memory, 0);
  check_vk_result_line(err, __LINE__);
}

static VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format,
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
  VkResult err = vkCreateImageView(handler->g_Device, &view_info, handler->g_Allocator, &image_view);
  check_vk_result_line(err, __LINE__);
  return image_view;
}

static VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, uint32_t filter) {
  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = filter,
                                      .minFilter = filter,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
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
  VkResult err = vkCreateSampler(handler->g_Device, &sampler_info, handler->g_Allocator, &sampler);
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
  VkResult err = vkCreateShaderModule(handler->g_Device, &create_info, handler->g_Allocator, &shader_module);
  check_vk_result_line(err, __LINE__);

  return shader_module;
}

static VkPipeline create_graphics_pipeline(gfx_handler_t *handler, shader_t *shader,
                                           VkPipelineLayout pipeline_layout, VkRenderPass render_pass) {
  VkResult err;

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

  VkVertexInputBindingDescription binding_description = get_vertex_binding_description();
  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding_description,
      .vertexAttributeDescriptionCount = get_vertex_attribute_description_count(),
      .pVertexAttributeDescriptions = get_vertex_attribute_descriptions()};

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
      .cullMode = VK_CULL_MODE_BACK_BIT,
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
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT};

  VkPipelineColorBlendStateCreateInfo color_blending = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color_blend_attachment};

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
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
                                                .layout = pipeline_layout,
                                                .renderPass = render_pass,
                                                .subpass = 0};

  VkPipeline graphics_pipeline;
  err = vkCreateGraphicsPipelines(handler->g_Device, handler->g_PipelineCache, 1, &pipeline_info,
                                  handler->g_Allocator, &graphics_pipeline);
  check_vk_result_line(err, __LINE__);

  return graphics_pipeline;
}

// Thx jupstar
static bool build_mipmaps(gfx_handler_t *handler, VkImage image, uint32_t width, uint32_t height,
                          uint32_t mip_levels, uint32_t layer_count) {
  if (mip_levels <= 1)
    return true; // No mipmaps to generate

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
    // Transition mip level i-1 to be a transfer source
    barrier.subresourceRange.baseMipLevel = i - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);

    // Blit from mip level i-1 to mip level i
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

    // Transition mip level i-1 to be shader-readable
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

  // Finally, transition the last mip level (which was only a DST) to be shader-readable
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

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas,
                                                    uint32_t tile_width, uint32_t tile_height,
                                                    uint32_t num_tiles_x, uint32_t num_tiles_y) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->texture_count >= MAX_TEXTURES)
    return NULL;

  uint32_t layer_count = num_tiles_x * num_tiles_y;
  uint32_t mip_levels = (uint32_t)floorf(log2f(fmaxf(tile_width, tile_height))) + 1;

  texture_t *tex_array = &renderer->textures[renderer->texture_count++];
  memset(tex_array, 0, sizeof(texture_t));
  tex_array->id = renderer->texture_count - 1;
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
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, layer_count);

  VkCommandBuffer cmd = begin_single_time_commands(handler, renderer->transfer_command_pool);
  // Transition atlas to be a source
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
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = 0,
                           .baseArrayLayer = 0,
                           .layerCount = 1},
        .srcOffset = {(int32_t)(tile_x * tile_width), (int32_t)(tile_y * tile_height), 0},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = 0,
                           .baseArrayLayer = layer,
                           .layerCount = 1},
        .dstOffset = {0, 0, 0},
        .extent = {tile_width, tile_height, 1}};
    vkCmdCopyImage(cmd, atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_array->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
  }

  // Transition atlas back to shader read
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

int renderer_init(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  memset(renderer, 0, sizeof(renderer_state_t));
  renderer->gfx = handler;

  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                       .queueFamilyIndex = handler->g_QueueFamily};
  check_vk_result(vkCreateCommandPool(handler->g_Device, &pool_info, handler->g_Allocator,
                                      &renderer->transfer_command_pool));

  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_MATERIALS * MAX_UBOS_PER_MATERIAL},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_MATERIALS * MAX_TEXTURES_PER_MATERIAL}};
  VkDescriptorPoolCreateInfo pool_create_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                 .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                 .maxSets = MAX_MATERIALS,
                                                 .poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
                                                 .pPoolSizes = pool_sizes};
  check_vk_result(vkCreateDescriptorPool(handler->g_Device, &pool_create_info, handler->g_Allocator,
                                         &renderer->resource_descriptor_pool));

  // Create a 1x1 white texture to use as a default
  unsigned char white_pixel[] = {255, 255, 255, 255};
  texture_t *default_tex = renderer_load_texture_from_array(handler, white_pixel, 1, 1);
  strncpy(default_tex->path, "default_white", sizeof(default_tex->path) - 1);
  renderer->default_texture = default_tex;

  printf("Renderer initialized.\n");
  return 0;
}

void renderer_cleanup(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  VkDevice device = handler->g_Device;
  VkAllocationCallbacks *allocator = handler->g_Allocator;

  vkDeviceWaitIdle(device);

  for (uint32_t i = 0; i < renderer->material_count; ++i) {
    material_t *mat = &renderer->materials[i];
    vkDestroyPipeline(device, mat->pipeline, allocator);
    vkDestroyPipelineLayout(device, mat->pipeline_layout, allocator);
    vkDestroyDescriptorSetLayout(device, mat->descriptor_set_layout, allocator);
    for (uint32_t j = 0; j < mat->ubo_count; ++j) {
      vkDestroyBuffer(device, mat->uniform_buffers[j].buffer, allocator);
      vkFreeMemory(device, mat->uniform_buffers[j].memory, allocator);
    }
  }

  for (uint32_t i = 0; i < renderer->mesh_count; ++i) {
    mesh_t *m = &renderer->meshes[i];
    vkDestroyBuffer(device, m->vertex_buffer.buffer, allocator);
    vkFreeMemory(device, m->vertex_buffer.memory, allocator);
    if (m->index_buffer.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, m->index_buffer.buffer, allocator);
      vkFreeMemory(device, m->index_buffer.memory, allocator);
    }
  }

  for (uint32_t i = 0; i < renderer->texture_count; ++i) {
    texture_t *t = &renderer->textures[i];
    vkDestroySampler(device, t->sampler, allocator);
    vkDestroyImageView(device, t->image_view, allocator);
    vkDestroyImage(device, t->image, allocator);
    vkFreeMemory(device, t->memory, allocator);
  }

  for (uint32_t i = 0; i < renderer->shader_count; ++i) {
    shader_t *s = &renderer->shaders[i];
    vkDestroyShaderModule(device, s->vert_shader_module, allocator);
    vkDestroyShaderModule(device, s->frag_shader_module, allocator);
  }

  vkDestroyDescriptorPool(device, renderer->resource_descriptor_pool, allocator);
  vkDestroyCommandPool(device, renderer->transfer_command_pool, allocator);

  printf("Renderer cleaned up.\n");
}

material_t *renderer_create_material(gfx_handler_t *handler, shader_t *shader) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->material_count >= MAX_MATERIALS) {
    fprintf(stderr, "Maximum material count (%d) reached.\n", MAX_MATERIALS);
    return NULL;
  }
  material_t *material = &renderer->materials[renderer->material_count];
  memset(material, 0, sizeof(material_t));
  material->id = renderer->material_count++;
  material->shader = shader;
  return material;
}

void material_add_texture(material_t *material, texture_t *texture) {
  if (material->texture_count >= MAX_TEXTURES_PER_MATERIAL) {
    fprintf(stderr, "Maximum textures per material (%d) reached.\n", MAX_TEXTURES_PER_MATERIAL);
    return;
  }
  material->textures[material->texture_count++] = texture;
}

void material_add_ubo(gfx_handler_t *handler, material_t *material, VkDeviceSize ubo_size) {
  if (material->ubo_count >= MAX_UBOS_PER_MATERIAL) {
    fprintf(stderr, "Maximum UBOs per material (%d) reached.\n", MAX_UBOS_PER_MATERIAL);
    return;
  }
  buffer_t *ubo = &material->uniform_buffers[material->ubo_count++];
  create_buffer(handler, ubo_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ubo);
  check_vk_result(vkMapMemory(handler->g_Device, ubo->memory, 0, ubo_size, 0, &ubo->mapped_memory));
}

void material_finalize(gfx_handler_t *handler, material_t *material) {
  renderer_state_t *renderer = &handler->renderer;
  uint32_t binding_count = material->ubo_count + material->texture_count;
  VkDescriptorSetLayoutBinding bindings[binding_count];

  uint32_t current_binding = 0;
  for (uint32_t i = 0; i < material->ubo_count; ++i) {
    bindings[current_binding++] = (VkDescriptorSetLayoutBinding){
        .binding = current_binding - 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL};
  }
  for (uint32_t i = 0; i < material->texture_count; ++i) {
    bindings[current_binding++] =
        (VkDescriptorSetLayoutBinding){.binding = current_binding - 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .pImmutableSamplers = NULL};
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = binding_count,
                                                 .pBindings = bindings};
  check_vk_result(vkCreateDescriptorSetLayout(handler->g_Device, &layout_info, handler->g_Allocator,
                                              &material->descriptor_set_layout));

  VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &material->descriptor_set_layout};
  check_vk_result(vkCreatePipelineLayout(handler->g_Device, &pipeline_layout_info, handler->g_Allocator,
                                         &material->pipeline_layout));

  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = renderer->resource_descriptor_pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &material->descriptor_set_layout};
  check_vk_result(vkAllocateDescriptorSets(handler->g_Device, &alloc_info, &material->descriptor_set));

  VkWriteDescriptorSet descriptor_writes[binding_count];
  VkDescriptorBufferInfo buffer_infos[material->ubo_count];
  VkDescriptorImageInfo image_infos[material->texture_count];

  current_binding = 0;
  for (uint32_t i = 0; i < material->ubo_count; ++i) {
    buffer_infos[i] = (VkDescriptorBufferInfo){.buffer = material->uniform_buffers[i].buffer,
                                               .offset = 0,
                                               .range = material->uniform_buffers[i].size};
    descriptor_writes[current_binding++] =
        (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = material->descriptor_set,
                               .dstBinding = current_binding - 1,
                               .dstArrayElement = 0,
                               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                               .descriptorCount = 1,
                               .pBufferInfo = &buffer_infos[i]};
  }
  for (uint32_t i = 0; i < material->texture_count; ++i) {
    image_infos[i] = (VkDescriptorImageInfo){.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             .imageView = material->textures[i]->image_view,
                                             .sampler = material->textures[i]->sampler};
    descriptor_writes[current_binding++] =
        (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = material->descriptor_set,
                               .dstBinding = current_binding - 1,
                               .dstArrayElement = 0,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .descriptorCount = 1,
                               .pImageInfo = &image_infos[i]};
  }
  vkUpdateDescriptorSets(handler->g_Device, binding_count, descriptor_writes, 0, NULL);

  // Create Pipeline (copied from original code, now uses material's objects)
  VkPipelineShaderStageCreateInfo vert_shader_stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = material->shader->vert_shader_module,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo frag_shader_stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = material->shader->frag_shader_module,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

  VkVertexInputBindingDescription binding_description = get_vertex_binding_description();
  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding_description,
      .vertexAttributeDescriptionCount = get_vertex_attribute_description_count(),
      .pVertexAttributeDescriptions = get_vertex_attribute_descriptions()};
  // ... (rest of pipeline creation structs: input_assembly, viewport_state, etc.)
  // ... They are identical to the original create_graphics_pipeline function

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
      .cullMode = VK_CULL_MODE_NONE, // Changed for 2D quad
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
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT};

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
                                                .layout = material->pipeline_layout,
                                                .renderPass = handler->g_MainWindowData.RenderPass,
                                                .subpass = 0};

  check_vk_result(vkCreateGraphicsPipelines(handler->g_Device, handler->g_PipelineCache, 1, &pipeline_info,
                                            handler->g_Allocator, &material->pipeline));
}

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->shader_count >= MAX_SHADERS)
    return NULL;

  size_t vert_size, frag_size;
  char *vert_code = read_file(vert_path, &vert_size);
  char *frag_code = read_file(frag_path, &frag_size);
  if (!vert_code || !frag_code) {
    free(vert_code);
    free(frag_code);
    return NULL;
  }

  shader_t *shader = &renderer->shaders[renderer->shader_count++];
  shader->id = renderer->shader_count - 1;
  shader->vert_shader_module = create_shader_module(handler, vert_code, vert_size);
  shader->frag_shader_module = create_shader_module(handler, frag_code, frag_size);
  strncpy(shader->vert_path, vert_path, sizeof(shader->vert_path) - 1);
  strncpy(shader->frag_path, frag_path, sizeof(shader->frag_path) - 1);

  free(vert_code);
  free(frag_code);
  return shader;
}

texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array,
                                            uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->texture_count >= MAX_TEXTURES)
    return NULL;
  if (!pixel_array)
    return NULL;

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

  texture_t *texture = &renderer->textures[renderer->texture_count++];
  memset(texture, 0, sizeof(texture_t));
  texture->id = renderer->texture_count - 1;
  texture->width = width;
  texture->height = height;
  texture->mip_levels = 1;
  texture->layer_count = 1;
  strncpy(texture->path, "from_array", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_Device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, image_size);
  vkUnmapMemory(handler->g_Device, staging_buffer.memory);
  free(rgba_pixels);

  create_image(handler, width, height, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image, width,
                       height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                          1);

  vkDestroyBuffer(handler->g_Device, staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, staging_buffer.memory, handler->g_Allocator);

  texture->image_view =
      create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
  texture->sampler = create_texture_sampler(handler, 1, VK_FILTER_NEAREST); // Use NEAREST for map tiles

  return texture;
}

// --- Texture, Mesh, Shader Loading (Largely Unchanged, but with minor adjustments) ---

texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->texture_count >= MAX_TEXTURES)
    return NULL;

  int tex_width, tex_height, tex_channels;
  stbi_uc *pixels = stbi_load(image_path, &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
  if (!pixels) {
    fprintf(stderr, "Failed to load texture image: %s\n", image_path);
    return NULL;
  }

  VkDeviceSize image_size = tex_width * tex_height * 4;
  uint32_t mip_levels = (uint32_t)floor(log2(fmax(tex_width, tex_height))) + 1;

  texture_t *texture = &renderer->textures[renderer->texture_count++];
  memset(texture, 0, sizeof(texture_t));
  texture->id = renderer->texture_count - 1;
  texture->width = tex_width;
  texture->height = tex_height;
  texture->mip_levels = mip_levels;
  texture->layer_count = 1;
  strncpy(texture->path, image_path, sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_Device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, image_size);
  vkUnmapMemory(handler->g_Device, staging_buffer.memory);
  stbi_image_free(pixels);

  create_image(handler, tex_width, tex_height, mip_levels, 1, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels, 1);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image,
                       tex_width, tex_height);

  vkDestroyBuffer(handler->g_Device, staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, staging_buffer.memory, handler->g_Allocator);

  if (!build_mipmaps(handler, texture->image, tex_width, tex_height, mip_levels, 1)) {
    // If mipmap generation fails, transition the base level to be shader-ready
    transition_image_layout(handler, renderer->transfer_command_pool, texture->image,
                            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mip_levels, 1);
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
  vkMapMemory(handler->g_Device, vertex_staging_buffer.memory, 0, vertex_buffer_size, 0, &data);
  memcpy(data, vertices, (size_t)vertex_buffer_size);
  vkUnmapMemory(handler->g_Device, vertex_staging_buffer.memory);

  create_buffer(handler, vertex_buffer_size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->vertex_buffer);

  copy_buffer(handler, renderer->transfer_command_pool, vertex_staging_buffer.buffer,
              mesh->vertex_buffer.buffer, vertex_buffer_size);

  vkDestroyBuffer(handler->g_Device, vertex_staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, vertex_staging_buffer.memory, handler->g_Allocator);

  if (index_count > 0 && indices) {
    create_buffer(handler, index_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &index_staging_buffer);
    vkMapMemory(handler->g_Device, index_staging_buffer.memory, 0, index_buffer_size, 0, &data);
    memcpy(data, indices, (size_t)index_buffer_size);
    vkUnmapMemory(handler->g_Device, index_staging_buffer.memory);

    create_buffer(handler, index_buffer_size,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mesh->index_buffer);

    copy_buffer(handler, renderer->transfer_command_pool, index_staging_buffer.buffer,
                mesh->index_buffer.buffer, index_buffer_size);

    vkDestroyBuffer(handler->g_Device, index_staging_buffer.buffer, handler->g_Allocator);
    vkFreeMemory(handler->g_Device, index_staging_buffer.memory, handler->g_Allocator);
  } else {
    mesh->index_count = 0;
  }

  printf("Created mesh: V=%u, I=%u\n", vertex_count, index_count);
  return mesh;
}

render_object_t *renderer_add_render_object(gfx_handler_t *handler, mesh_t *mesh, material_t *material) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->render_object_count >= MAX_RENDER_OBJECTS) {
    fprintf(stderr, "Maximum render object count (%d) reached.\n", MAX_RENDER_OBJECTS);
    return NULL;
  }

  renderer->render_object_count = 0;
  render_object_t *obj = 0;
  // count all objects and emplace our new one in there
  for (int i = 0; i < MAX_RENDER_OBJECTS; ++i) {
    if (renderer->render_objects[i].active && renderer->render_objects[i].material &&
        renderer->render_objects[i].mesh)
      ++renderer->render_object_count;
    else if (!obj) {
      obj = &renderer->render_objects[renderer->render_object_count];
      obj->active = true;
      obj->mesh = mesh;
      obj->material = material;
      ++renderer->render_object_count;
    }
  }
  if (!obj)
    printf("ERROR: could not find space for render object.\n");
  return obj;
}

void renderer_update(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;

  float window_ratio = (float)width / (float)height;

  // This is just an example for the map UBO.
  // In a real app, you'd loop through render objects and update their materials as needed.
  if (renderer->render_object_count > 0) {
    render_object_t *map_obj = &renderer->render_objects[0]; // Assuming first object is the map
    printf("before rendering!, ubos: %d\n", map_obj->material->ubo_count);
    if (map_obj->active && map_obj->material->ubo_count > 0) {
      printf("rendering!\n");
      float map_ratio = (float)handler->map_data.width / (float)handler->map_data.height;
      if (isnan(map_ratio))
        map_ratio = 1.0f;

      float zoom =
          1.0 / (renderer->camera.zoom * fmax(handler->map_data.width, handler->map_data.height) * 0.001);
      if (isnan(zoom))
        zoom = 1.0f;

      float aspect = 1.0 / (window_ratio / map_ratio);
      float lod = fmin(fmax(5.5 - log2((1.0f / handler->map_data.width) / zoom * (width / 2.0f)), 0.0), 6.0);

      map_buffer_object_t ubo = {.transform = {renderer->camera.pos[0], renderer->camera.pos[1], zoom},
                                 .aspect = aspect,
                                 .lod = lod};
      memcpy(map_obj->material->uniform_buffers[0].mapped_memory, &ubo, sizeof(ubo));
    }
  }
}

void renderer_draw(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &handler->renderer;

  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;

  VkViewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor = {{0, 0}, {(uint32_t)width, (uint32_t)height}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  for (uint32_t i = 0; i < renderer->render_object_count; ++i) {
    render_object_t *obj = &renderer->render_objects[i];
    if (!obj->active || !obj->mesh || !obj->material)
      continue;

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, obj->material->pipeline);

    VkBuffer vertex_buffers[] = {obj->mesh->vertex_buffer.buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);

    if (obj->mesh->index_count > 0) {
      vkCmdBindIndexBuffer(command_buffer, obj->mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    }

    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, obj->material->pipeline_layout,
                            0, 1, &obj->material->descriptor_set, 0, NULL);

    if (obj->mesh->index_count > 0) {
      vkCmdDrawIndexed(command_buffer, obj->mesh->index_count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(command_buffer, obj->mesh->vertex_count, 1, 0, 0);
    }
  }
}

VkVertexInputBindingDescription get_vertex_binding_description(void) {
  VkVertexInputBindingDescription binding_description = {
      .binding = 0, .stride = sizeof(vertex_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  return binding_description;
}

static VkVertexInputAttributeDescription attribute_descriptions[3];

uint32_t get_vertex_attribute_description_count(void) {
  return sizeof(attribute_descriptions) / sizeof(attribute_descriptions[0]);
}

const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions(void) {
  attribute_descriptions[0].binding = 0;
  attribute_descriptions[0].location = 0;
  attribute_descriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
  attribute_descriptions[0].offset = offsetof(vertex_t, pos);

  attribute_descriptions[1].binding = 0;
  attribute_descriptions[1].location = 1;
  attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  attribute_descriptions[1].offset = offsetof(vertex_t, color);

  attribute_descriptions[2].binding = 0;
  attribute_descriptions[2].location = 2;
  attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
  attribute_descriptions[2].offset = offsetof(vertex_t, tex_coord);

  return attribute_descriptions;
}
