#include "cglm/affine.h"
#include "cglm/cam.h"
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include "graphics_backend.h"
#include "renderer.h"
#include <cglm/cglm.h>
#include <vulkan/vulkan_core.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void check_vk_result_line(VkResult err, int line) {
  if (err == VK_SUCCESS)
    return;
  fprintf(stderr, "[vulkan] Error: VkResult = %d in line: (%d)\n", err, line);
  if (err < 0)
    abort();
}
void check_vk_result(VkResult err) {
  if (err == VK_SUCCESS)
    return;
  fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
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
                                    .pNext = NULL,
                                    .flags = 0,
                                    .size = size,
                                    .usage = usage,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .queueFamilyIndexCount = 0,
                                    .pQueueFamilyIndices = NULL};

  err = vkCreateBuffer(handler->g_Device, &buffer_info, handler->g_Allocator, &buffer->buffer);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(handler->g_Device, buffer->buffer, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = NULL,
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
                                            .pNext = NULL,
                                            .commandPool = pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};

  VkCommandBuffer command_buffer;
  vkAllocateCommandBuffers(handler->g_Device, &alloc_info, &command_buffer);

  VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .pNext = NULL,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                         .pInheritanceInfo = NULL};

  vkBeginCommandBuffer(command_buffer, &begin_info);
  return command_buffer;
}

static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool,
                                     VkCommandBuffer command_buffer) {
  vkEndCommandBuffer(command_buffer);

  VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .pNext = NULL,
                              .waitSemaphoreCount = 0,
                              .pWaitSemaphores = NULL,
                              .pWaitDstStageMask = NULL,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &command_buffer,
                              .signalSemaphoreCount = 0,
                              .pSignalSemaphores = NULL};

  VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = NULL, .flags = 0};
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

  VkBufferCopy copy_region = {.srcOffset = 0, .dstOffset = 0, .size = size};
  vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image,
                                    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .pNext = NULL,
                                  .srcAccessMask = 0,
                                  .dstAccessMask = 0,
                                  .oldLayout = old_layout,
                                  .newLayout = new_layout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = 1,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};

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
    fprintf(stderr, "Unsupported layout transition!\n");
    exit(EXIT_FAILURE);
  }

  vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

  end_single_time_commands(handler, pool, command_buffer);
}

static void copy_buffer_to_image(gfx_handler_t *handler, VkCommandPool pool, VkBuffer buffer, VkImage image,
                                 uint32_t width, uint32_t height) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkBufferImageCopy region = {.bufferOffset = 0,
                              .bufferRowLength = 0,
                              .bufferImageHeight = 0,
                              .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .mipLevel = 0,
                                                   .baseArrayLayer = 0,
                                                   .layerCount = 1},
                              .imageOffset = {0, 0, 0},
                              .imageExtent = {width, height, 1}};

  vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                         VkImage *image, VkDeviceMemory *image_memory) {
  VkResult err;
  VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .pNext = NULL,
                                  .flags = 0,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {width, height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = tiling,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .queueFamilyIndexCount = 0,
                                  .pQueueFamilyIndices = NULL,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

  err = vkCreateImage(handler->g_Device, &image_info, handler->g_Allocator, image);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetImageMemoryRequirements(handler->g_Device, *image, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = NULL,
      .allocationSize = mem_requirements.size,
      .memoryTypeIndex =
          find_memory_type(handler->g_PhysicalDevice, mem_requirements.memoryTypeBits, properties)};

  err = vkAllocateMemory(handler->g_Device, &alloc_info, handler->g_Allocator, image_memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindImageMemory(handler->g_Device, *image, *image_memory, 0);
  check_vk_result_line(err, __LINE__);
}

static VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format) {
  VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .pNext = NULL,
                                     .flags = 0,
                                     .image = image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .components = {0, 0, 0, 0},
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};

  VkImageView image_view;
  VkResult err = vkCreateImageView(handler->g_Device, &view_info, handler->g_Allocator, &image_view);
  check_vk_result_line(err, __LINE__);
  return image_view;
}

static VkSampler create_texture_sampler(gfx_handler_t *handler) {
  VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .pNext = NULL,
                                      .flags = 0,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      .mipLodBias = 0.0f,
                                      .anisotropyEnable = VK_FALSE,
                                      .maxAnisotropy = 0.0f,
                                      .compareEnable = VK_FALSE,
                                      .compareOp = VK_COMPARE_OP_ALWAYS,
                                      .minLod = 0.0f,
                                      .maxLod = 0.0f,
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
                                          .pNext = NULL,
                                          .flags = 0,
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
      .pNext = NULL,
      .flags = 0,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = shader->vert_shader_module,
      .pName = "main",
      .pSpecializationInfo = NULL};

  VkPipelineShaderStageCreateInfo frag_shader_stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = shader->frag_shader_module,
      .pName = "main",
      .pSpecializationInfo = NULL};

  VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

  VkVertexInputBindingDescription binding_description = get_vertex_binding_description();
  VkPipelineVertexInputStateCreateInfo vertex_input_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding_description,
      .vertexAttributeDescriptionCount = get_vertex_attribute_description_count(),
      .pVertexAttributeDescriptions = get_vertex_attribute_descriptions()};

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable = VK_FALSE};

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .viewportCount = 1,
      .pViewports = NULL,
      .scissorCount = 1,
      .pScissors = NULL};

  VkPipelineRasterizationStateCreateInfo rasterizer = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .depthClampEnable = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = VK_FALSE,
      .depthBiasConstantFactor = 0.0f,
      .depthBiasClamp = 0.0f,
      .depthBiasSlopeFactor = 0.0f,
      .lineWidth = 1.0f};

  VkPipelineMultisampleStateCreateInfo multisampling = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 0.0f,
      .pSampleMask = NULL,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE};

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = VK_FALSE,
      .front = {0},
      .back = {0},
      .minDepthBounds = 0.0f,
      .maxDepthBounds = 0.0f};

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
      .pNext = NULL,
      .flags = 0,
      .logicOpEnable = VK_FALSE,
      .logicOp = 0,
      .attachmentCount = 1,
      .pAttachments = &color_blend_attachment,
      .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
      .pDynamicStates = dynamic_states};

  VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = NULL,
                                                .flags = 0,
                                                .stageCount = 2,
                                                .pStages = shader_stages,
                                                .pVertexInputState = &vertex_input_info,
                                                .pInputAssemblyState = &input_assembly,
                                                .pTessellationState = NULL,
                                                .pViewportState = &viewport_state,
                                                .pRasterizationState = &rasterizer,
                                                .pMultisampleState = &multisampling,
                                                .pDepthStencilState = &depth_stencil,
                                                .pColorBlendState = &color_blending,
                                                .pDynamicState = &dynamic_state,
                                                .layout = pipeline_layout,
                                                .renderPass = render_pass,
                                                .subpass = 0,
                                                .basePipelineHandle = VK_NULL_HANDLE,
                                                .basePipelineIndex = -1};

  VkPipeline graphics_pipeline;
  err = vkCreateGraphicsPipelines(handler->g_Device, handler->g_PipelineCache, 1, &pipeline_info,
                                  handler->g_Allocator, &graphics_pipeline);
  check_vk_result_line(err, __LINE__);

  return graphics_pipeline;
}

int renderer_init(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  renderer->gfx = handler;
  renderer->shader_count = 0;
  renderer->texture_count = 0;
  renderer->mesh_count = 0;
  renderer->renderable_count = 0;
  renderer->default_texture = NULL;
  VkResult err;

  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .pNext = NULL,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                       .queueFamilyIndex = handler->g_QueueFamily};

  err = vkCreateCommandPool(handler->g_Device, &pool_info, handler->g_Allocator,
                            &renderer->transfer_command_pool);
  check_vk_result_line(err, __LINE__);
  if (err != VK_SUCCESS)
    return 1;

  // Descriptor set layout with three bindings: UBO, sampler1, sampler2
  VkDescriptorSetLayoutBinding ubo_layout_binding = {.binding = 0,
                                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                     .descriptorCount = 1,
                                                     .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                                                     .pImmutableSamplers = NULL};

  VkDescriptorSetLayoutBinding sampler_layout_binding1 = {.binding = 1,
                                                          .descriptorType =
                                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                          .descriptorCount = 1,
                                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                          .pImmutableSamplers = NULL};

  VkDescriptorSetLayoutBinding sampler_layout_binding2 = {.binding = 2,
                                                          .descriptorType =
                                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                          .descriptorCount = 1,
                                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                          .pImmutableSamplers = NULL};

  VkDescriptorSetLayoutBinding bindings[] = {ubo_layout_binding, sampler_layout_binding1,
                                             sampler_layout_binding2};

  VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .pNext = NULL,
                                                 .flags = 0,
                                                 .bindingCount = 3, // Updated to 3 bindings
                                                 .pBindings = bindings};

  err = vkCreateDescriptorSetLayout(handler->g_Device, &layout_info, handler->g_Allocator,
                                    &renderer->object_descriptor_set_layout);
  check_vk_result_line(err, __LINE__);
  if (err != VK_SUCCESS)
    return 1;

  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_RENDERABLES},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_RENDERABLES * 2} // Account for two samplers
  };

  VkDescriptorPoolCreateInfo pool_info_renderer = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                   .pNext = NULL,
                                                   .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                   .maxSets = MAX_RENDERABLES,
                                                   .poolSizeCount =
                                                       sizeof(pool_sizes) / sizeof(pool_sizes[0]),
                                                   .pPoolSizes = pool_sizes};

  err = vkCreateDescriptorPool(handler->g_Device, &pool_info_renderer, handler->g_Allocator,
                               &renderer->resource_descriptor_pool);
  check_vk_result_line(err, __LINE__);
  if (err != VK_SUCCESS)
    return 1;

  unsigned char white_pixel[] = {255, 255, 255, 255};
  texture_t *default_tex = &renderer->textures[renderer->texture_count];
  default_tex->id = renderer->texture_count++;
  default_tex->width = 1;
  default_tex->height = 1;
  strncpy(default_tex->path, "default_white", sizeof(default_tex->path) - 1);

  VkDeviceSize image_size = default_tex->width * default_tex->height * 4;
  buffer_t staging_buffer;

  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_Device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, white_pixel, (size_t)image_size);
  vkUnmapMemory(handler->g_Device, staging_buffer.memory);

  create_image(handler, default_tex->width, default_tex->height, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &default_tex->image, &default_tex->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, default_tex->image,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, default_tex->image,
                       default_tex->width, default_tex->height);
  transition_image_layout(handler, renderer->transfer_command_pool, default_tex->image,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(handler->g_Device, staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, staging_buffer.memory, handler->g_Allocator);

  default_tex->image_view = create_image_view(handler, default_tex->image, VK_FORMAT_R8G8B8A8_UNORM);
  default_tex->sampler = create_texture_sampler(handler);
  renderer->default_texture = default_tex;

  printf("Renderer initialized.\n");
  return 0;
}

void renderer_cleanup(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;
  VkDevice device = handler->g_Device;
  VkAllocationCallbacks *allocator = handler->g_Allocator;

  vkDeviceWaitIdle(device);

  // Clean up renderables
  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    renderable_t *r = &renderer->renderables[i];
    if (!r->active)
      continue;
    vkDestroyPipeline(device, r->pipeline, allocator);
    vkDestroyPipelineLayout(device, r->pipeline_layout, allocator);
    vkDestroyBuffer(device, r->uniform_buffer.buffer, allocator);
    vkFreeMemory(device, r->uniform_buffer.memory, allocator);
  }

  // Clean up map renderable
  if (renderer->map_renderable.active) {
    vkDestroyPipeline(device, renderer->map_renderable.pipeline, allocator);
    vkDestroyPipelineLayout(device, renderer->map_renderable.pipeline_layout, allocator);
    vkDestroyBuffer(device, renderer->map_renderable.uniform_buffer.buffer, allocator);
    vkFreeMemory(device, renderer->map_renderable.uniform_buffer.memory, allocator);
    // Descriptor set is freed via the descriptor pool
  }

  // Clean up meshes
  for (uint32_t i = 0; i < renderer->mesh_count; ++i) {
    mesh_t *m = &renderer->meshes[i];
    vkDestroyBuffer(device, m->vertex_buffer.buffer, allocator);
    vkFreeMemory(device, m->vertex_buffer.memory, allocator);
    if (m->index_buffer.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, m->index_buffer.buffer, allocator);
      vkFreeMemory(device, m->index_buffer.memory, allocator);
    }
  }

  // Clean up textures, skipping invalid ones
  for (uint32_t i = 0; i < renderer->texture_count; ++i) {
    texture_t *t = &renderer->textures[i];
    if (t->image == VK_NULL_HANDLE || t->sampler == VK_NULL_HANDLE || t->image_view == VK_NULL_HANDLE ||
        t->memory == VK_NULL_HANDLE) {
      continue; // Skip textures that were already destroyed
    }
    vkDestroySampler(device, t->sampler, allocator);
    vkDestroyImageView(device, t->image_view, allocator);
    vkDestroyImage(device, t->image, allocator);
    vkFreeMemory(device, t->memory, allocator);
  }
  renderer->texture_count = 0; // Reset texture count

  // Clean up shaders
  for (uint32_t i = 0; i < renderer->shader_count; ++i) {
    shader_t *s = &renderer->shaders[i];
    vkDestroyShaderModule(device, s->vert_shader_module, allocator);
    vkDestroyShaderModule(device, s->frag_shader_module, allocator);
  }

  // Clean up descriptor pool and layout
  vkDestroyDescriptorPool(device, renderer->resource_descriptor_pool, allocator);
  vkDestroyDescriptorSetLayout(device, renderer->object_descriptor_set_layout, allocator);

  // Clean up command pool
  vkDestroyCommandPool(device, renderer->transfer_command_pool, allocator);

  printf("Renderer cleaned up.\n");
}

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->shader_count >= MAX_SHADERS) {
    fprintf(stderr, "Maximum shader count (%d) reached.\n", MAX_SHADERS);
    return NULL;
  }

  size_t vert_code_size, frag_code_size;
  char *vert_code = read_file(vert_path, &vert_code_size);
  char *frag_code = read_file(frag_path, &frag_code_size);

  if (!vert_code || !frag_code) {
    free(vert_code);
    free(frag_code);
    return NULL;
  }

  shader_t *shader = &renderer->shaders[renderer->shader_count];
  shader->id = renderer->shader_count++;
  shader->vert_shader_module = create_shader_module(handler, vert_code, vert_code_size);
  shader->frag_shader_module = create_shader_module(handler, frag_code, frag_code_size);
  strncpy(shader->vert_path, vert_path, sizeof(shader->vert_path) - 1);
  strncpy(shader->frag_path, frag_path, sizeof(shader->frag_path) - 1);

  free(vert_code);
  free(frag_code);

  printf("Loaded shader: V=%s, F=%s\n", vert_path, frag_path);
  return shader;
}

texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array,
                                            uint32_t width, uint32_t height) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->texture_count >= MAX_TEXTURES) {
    fprintf(stderr, "Maximum texture count (%d) reached.\n", MAX_TEXTURES);
    return NULL;
  }

  VkDeviceSize image_size = width * height * 4;

  stbi_uc *rgba_pixels = (stbi_uc *)malloc(image_size);
  if (!rgba_pixels) {
    fprintf(stderr, "Failed to allocate memory for RGBA pixels.\n");
    return NULL;
  }

  for (uint32_t i = 0, j = 0; i < width * height; i++, j += 4) {
    rgba_pixels[j] = pixel_array[i];
    rgba_pixels[j + 1] = 0;
    rgba_pixels[j + 2] = 0;
    rgba_pixels[j + 3] = 255;
  }

  texture_t *texture = &renderer->textures[renderer->texture_count];
  texture->id = renderer->texture_count++;
  texture->width = width;
  texture->height = height;
  strncpy(texture->path, "array_texture", sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_Device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, rgba_pixels, (size_t)image_size);
  vkUnmapMemory(handler->g_Device, staging_buffer.memory);

  free(rgba_pixels);

  create_image(handler, texture->width, texture->height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image,
                       texture->width, texture->height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(handler->g_Device, staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, staging_buffer.memory, handler->g_Allocator);

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM);
  texture->sampler = create_texture_sampler(handler);

  printf("Loaded texture from array: %dx%d\n", texture->width, texture->height);
  return texture;
}

texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path) {
  renderer_state_t *renderer = &handler->renderer;
  if (renderer->texture_count >= MAX_TEXTURES) {
    fprintf(stderr, "Maximum texture count (%d) reached.\n", MAX_TEXTURES);
    return NULL;
  }

  int tex_width, tex_height, tex_channels;
  stbi_uc *pixels = stbi_load(image_path, &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
  VkDeviceSize image_size = tex_width * tex_height * 4;

  if (!pixels) {
    fprintf(stderr, "Failed to load texture image: %s\n", image_path);
    return NULL;
  }

  texture_t *texture = &renderer->textures[renderer->texture_count];
  texture->id = renderer->texture_count++;
  texture->width = (uint32_t)tex_width;
  texture->height = (uint32_t)tex_height;
  strncpy(texture->path, image_path, sizeof(texture->path) - 1);

  buffer_t staging_buffer;
  create_buffer(handler, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buffer);

  void *data;
  vkMapMemory(handler->g_Device, staging_buffer.memory, 0, image_size, 0, &data);
  memcpy(data, pixels, (size_t)image_size);
  vkUnmapMemory(handler->g_Device, staging_buffer.memory);

  stbi_image_free(pixels);

  create_image(handler, texture->width, texture->height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->image, &texture->memory);

  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copy_buffer_to_image(handler, renderer->transfer_command_pool, staging_buffer.buffer, texture->image,
                       texture->width, texture->height);
  transition_image_layout(handler, renderer->transfer_command_pool, texture->image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(handler->g_Device, staging_buffer.buffer, handler->g_Allocator);
  vkFreeMemory(handler->g_Device, staging_buffer.memory, handler->g_Allocator);

  texture->image_view = create_image_view(handler, texture->image, VK_FORMAT_R8G8B8A8_UNORM);
  texture->sampler = create_texture_sampler(handler);

  printf("Loaded texture: %s (%dx%d)\n", image_path, texture->width, texture->height);
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

map_renderable_t *renderer_set_map_renderable(gfx_handler_t *handler, mesh_t *mesh, shader_t *shader,
                                              texture_t *entities_texture, texture_t *map_texture) {
  renderer_state_t *renderer = &handler->renderer;
  VkResult err;

  map_renderable_t *renderable = &renderer->map_renderable;

  renderable->active = true;
  renderable->mesh = mesh;
  renderable->shader = shader;
  renderable->texture[0] = entities_texture;
  renderable->texture[1] = map_texture ? map_texture : renderer->default_texture;

  VkDeviceSize ubo_size = sizeof(map_buffer_object_t);
  create_buffer(handler, ubo_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderable->uniform_buffer);

  err = vkMapMemory(handler->g_Device, renderable->uniform_buffer.memory, 0, ubo_size, 0,
                    &renderable->uniform_buffer.mapped_memory);
  check_vk_result_line(err, __LINE__);

  VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .pNext = NULL,
                                                     .flags = 0,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &renderer->object_descriptor_set_layout,
                                                     .pushConstantRangeCount = 0,
                                                     .pPushConstantRanges = NULL};

  err = vkCreatePipelineLayout(handler->g_Device, &pipeline_layout_info, handler->g_Allocator,
                               &renderable->pipeline_layout);
  check_vk_result_line(err, __LINE__);

  renderable->pipeline = create_graphics_pipeline(handler, shader, renderable->pipeline_layout,
                                                  handler->g_MainWindowData.RenderPass);

  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .pNext = NULL,
                                            .descriptorPool = renderer->resource_descriptor_pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &renderer->object_descriptor_set_layout};

  err = vkAllocateDescriptorSets(handler->g_Device, &alloc_info, &renderable->descriptor_set);
  check_vk_result_line(err, __LINE__);

  VkDescriptorBufferInfo buffer_info = {
      .buffer = renderable->uniform_buffer.buffer, .offset = 0, .range = sizeof(map_buffer_object_t)};

  VkDescriptorImageInfo image_info[2] = {{
                                             .sampler = renderable->texture[0]->sampler,
                                             .imageView = renderable->texture[0]->image_view,
                                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         },
                                         {
                                             .sampler = renderable->texture[1]->sampler,
                                             .imageView = renderable->texture[1]->image_view,
                                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         }};

  VkWriteDescriptorSet descriptor_writes[3] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .pNext = NULL,
          .dstSet = renderable->descriptor_set,
          .dstBinding = 0,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pImageInfo = NULL,
          .pBufferInfo = &buffer_info,
          .pTexelBufferView = NULL,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .pNext = NULL,
          .dstSet = renderable->descriptor_set,
          .dstBinding = 1,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &image_info[0],
          .pBufferInfo = NULL,
          .pTexelBufferView = NULL,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .pNext = NULL,
          .dstSet = renderable->descriptor_set,
          .dstBinding = 2,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &image_info[1],
          .pBufferInfo = NULL,
          .pTexelBufferView = NULL,
      }};

  vkUpdateDescriptorSets(handler->g_Device, 3, descriptor_writes, 0, NULL);

  return renderable;
}

renderable_t *renderer_add_renderable(gfx_handler_t *handler, mesh_t *mesh, shader_t *shader,
                                      texture_t *texture) {
  renderer_state_t *renderer = &handler->renderer;
  VkResult err;

  renderable_t *renderable = NULL;
  uint32_t renderable_index = (uint32_t)-1;
  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    if (!renderer->renderables[i].active) {
      renderable = &renderer->renderables[i];
      renderable_index = i;
      break;
    }
  }

  if (!renderable) {
    if (renderer->renderable_count >= MAX_RENDERABLES) {
      fprintf(stderr, "Maximum renderable count (%d) reached.\n", MAX_RENDERABLES);
      return NULL;
    }
    renderable_index = renderer->renderable_count++;
    renderable = &renderer->renderables[renderable_index];
  }

  renderable->active = true;
  renderable->mesh = mesh;
  renderable->shader = shader;
  renderable->texture = texture ? texture : renderer->default_texture;

  VkDeviceSize ubo_size = sizeof(uniform_buffer_object_t);
  create_buffer(handler, ubo_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderable->uniform_buffer);

  err = vkMapMemory(handler->g_Device, renderable->uniform_buffer.memory, 0, ubo_size, 0,
                    &renderable->uniform_buffer.mapped_memory);
  check_vk_result_line(err, __LINE__);

  VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .pNext = NULL,
                                                     .flags = 0,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &renderer->object_descriptor_set_layout,
                                                     .pushConstantRangeCount = 0,
                                                     .pPushConstantRanges = NULL};

  err = vkCreatePipelineLayout(handler->g_Device, &pipeline_layout_info, handler->g_Allocator,
                               &renderable->pipeline_layout);
  check_vk_result_line(err, __LINE__);

  renderable->pipeline = create_graphics_pipeline(handler, shader, renderable->pipeline_layout,
                                                  handler->g_MainWindowData.RenderPass);

  VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .pNext = NULL,
                                            .descriptorPool = renderer->resource_descriptor_pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &renderer->object_descriptor_set_layout};

  err = vkAllocateDescriptorSets(handler->g_Device, &alloc_info, &renderable->descriptor_set);
  check_vk_result_line(err, __LINE__);

  VkDescriptorBufferInfo buffer_info = {
      .buffer = renderable->uniform_buffer.buffer, .offset = 0, .range = sizeof(uniform_buffer_object_t)};

  VkDescriptorImageInfo image_info = {.sampler = renderable->texture->sampler,
                                      .imageView = renderable->texture->image_view,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  VkWriteDescriptorSet descriptor_writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .pNext = NULL,
                                                .dstSet = renderable->descriptor_set,
                                                .dstBinding = 0,
                                                .dstArrayElement = 0,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                .pImageInfo = NULL,
                                                .pBufferInfo = &buffer_info,
                                                .pTexelBufferView = NULL},
                                               {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .pNext = NULL,
                                                .dstSet = renderable->descriptor_set,
                                                .dstBinding = 1,
                                                .dstArrayElement = 0,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                .pImageInfo = &image_info,
                                                .pBufferInfo = NULL,
                                                .pTexelBufferView = NULL}};

  vkUpdateDescriptorSets(handler->g_Device, 2, descriptor_writes, 0, NULL);

  printf("Added renderable %u.\n", renderable_index);
  return renderable;
}

void renderer_remove_renderable(gfx_handler_t *handler, renderable_t *renderable) {
  if (!renderable || !renderable->active)
    return;
  renderable->active = false;
  printf("Marked renderable as inactive.\n");
}

void renderer_update_uniforms(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;

  mat4 view, proj, model;

  glm_mat4_identity(model);
  glm_mat4_identity(view);

  glm_scale(view, (vec3){renderer->camera.zoom, renderer->camera.zoom, 1.0f});
  glm_translate(view, (vec3){-renderer->camera.pos[0], renderer->camera.pos[1], -1.0f});

  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;
  glm_ortho(-1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, proj);
  float window_ratio = (float)width / (float)height;
  {
    map_renderable_t *r = &renderer->map_renderable;
    if (r->active) {
      float map_ratio = (float)handler->map_data.width / (float)handler->map_data.height;
      map_buffer_object_t ubo = {
          renderer->camera.pos[0], renderer->camera.pos[1],
          1.0 / (renderer->camera.zoom * fmax(handler->map_data.width, handler->map_data.height) * 0.001),
          1.0 / (window_ratio / map_ratio)};
      assert(r->uniform_buffer.mapped_memory != NULL);
      memcpy(r->uniform_buffer.mapped_memory, &ubo, sizeof(ubo));
    }
  }
  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    renderable_t *r = &renderer->renderables[i];
    if (!r->active)
      continue;

    glm_scale(model, (vec3){1.0f, window_ratio, 1.0f});
    uniform_buffer_object_t ubo = {.model = {0}, .view = {0}, .proj = {0}};
    glm_mat4_copy(model, ubo.model);
    glm_mat4_copy(view, ubo.view);
    glm_mat4_copy(proj, ubo.proj);

    assert(r->uniform_buffer.mapped_memory != NULL);
    memcpy(r->uniform_buffer.mapped_memory, &ubo, sizeof(ubo));
  }
}
void renderer_draw(gfx_handler_t *handler, VkCommandBuffer command_buffer) {
  renderer_state_t *renderer = &handler->renderer;

  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;

  VkViewport viewport = {.x = 0.0f,
                         .y = 0.0f,
                         .width = (float)width,
                         .height = (float)height,
                         .minDepth = 0.0f,
                         .maxDepth = 1.0f};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor = {.offset = {0, 0}, .extent = {(uint32_t)width, (uint32_t)height}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  {
    map_renderable_t *r = &renderer->map_renderable;
    if (r->active && r->mesh && r->pipeline) {
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);

      VkBuffer vertex_buffers[] = {r->mesh->vertex_buffer.buffer};
      VkDeviceSize offsets[] = {0};
      vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);

      if (r->mesh->index_count > 0 && r->mesh->index_buffer.buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(command_buffer, r->mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
      }

      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout, 0, 1,
                              &r->descriptor_set, 0, NULL);

      if (r->mesh->index_count > 0) {
        vkCmdDrawIndexed(command_buffer, r->mesh->index_count, 1, 0, 0, 0);
      } else {
        vkCmdDraw(command_buffer, r->mesh->vertex_count, 1, 0, 0);
      }
    }
  }
  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    renderable_t *r = &renderer->renderables[i];
    if (!r->active || !r->mesh || !r->pipeline)
      continue;

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);

    VkBuffer vertex_buffers[] = {r->mesh->vertex_buffer.buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);

    if (r->mesh->index_count > 0 && r->mesh->index_buffer.buffer != VK_NULL_HANDLE) {
      vkCmdBindIndexBuffer(command_buffer, r->mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    }

    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout, 0, 1,
                            &r->descriptor_set, 0, NULL);

    if (r->mesh->index_count > 0) {
      vkCmdDrawIndexed(command_buffer, r->mesh->index_count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(command_buffer, r->mesh->vertex_count, 1, 0, 0);
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
