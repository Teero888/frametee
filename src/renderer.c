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

  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  err = vkCreateBuffer(handler->g_Device, &buffer_info, handler->g_Allocator, &buffer->buffer);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(handler->g_Device, buffer->buffer, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = mem_requirements.size;
  alloc_info.memoryTypeIndex =
      find_memory_type(handler->g_PhysicalDevice, mem_requirements.memoryTypeBits, properties);

  err = vkAllocateMemory(handler->g_Device, &alloc_info, handler->g_Allocator, &buffer->memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindBufferMemory(handler->g_Device, buffer->buffer, buffer->memory, 0);
  check_vk_result_line(err, __LINE__);

  buffer->mapped_memory = NULL;
}

static VkCommandBuffer begin_single_time_commands(gfx_handler_t *handler, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandPool = pool;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer command_buffer;
  vkAllocateCommandBuffers(handler->g_Device, &alloc_info, &command_buffer);

  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(command_buffer, &begin_info);
  return command_buffer;
}

static void end_single_time_commands(gfx_handler_t *handler, VkCommandPool pool,
                                     VkCommandBuffer command_buffer) {
  vkEndCommandBuffer(command_buffer);

  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
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

  VkBufferCopy copy_region = {};
  copy_region.size = size;
  vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy_region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void transition_image_layout(gfx_handler_t *handler, VkCommandPool pool, VkImage image,
                                    VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
  VkCommandBuffer command_buffer = begin_single_time_commands(handler, pool);

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

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

  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = (VkOffset3D){0, 0, 0};
  region.imageExtent = (VkExtent3D){width, height, 1};

  vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  end_single_time_commands(handler, pool, command_buffer);
}

static void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                         VkImage *image, VkDeviceMemory *image_memory) {
  VkResult err;
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent.width = width;
  image_info.extent.height = height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = format;
  image_info.tiling = tiling;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = usage;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  err = vkCreateImage(handler->g_Device, &image_info, handler->g_Allocator, image);
  check_vk_result_line(err, __LINE__);

  VkMemoryRequirements mem_requirements;
  vkGetImageMemoryRequirements(handler->g_Device, *image, &mem_requirements);

  VkMemoryAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = mem_requirements.size;
  alloc_info.memoryTypeIndex =
      find_memory_type(handler->g_PhysicalDevice, mem_requirements.memoryTypeBits, properties);

  err = vkAllocateMemory(handler->g_Device, &alloc_info, handler->g_Allocator, image_memory);
  check_vk_result_line(err, __LINE__);

  err = vkBindImageMemory(handler->g_Device, *image, *image_memory, 0);
  check_vk_result_line(err, __LINE__);
}

static VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format) {
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  VkImageView image_view;
  VkResult err = vkCreateImageView(handler->g_Device, &view_info, handler->g_Allocator, &image_view);
  check_vk_result_line(err, __LINE__);
  return image_view;
}

static VkSampler create_texture_sampler(gfx_handler_t *handler) {
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.anisotropyEnable = VK_FALSE;

  sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  sampler_info.unnormalizedCoordinates = VK_FALSE;
  sampler_info.compareEnable = VK_FALSE;
  sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.mipLodBias = 0.0f;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.0f;

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
  VkShaderModuleCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  create_info.codeSize = code_size;

  create_info.pCode = (const uint32_t *)code;

  VkShaderModule shader_module;
  VkResult err = vkCreateShaderModule(handler->g_Device, &create_info, handler->g_Allocator, &shader_module);
  check_vk_result_line(err, __LINE__);

  return shader_module;
}

static VkPipeline create_graphics_pipeline(gfx_handler_t *handler, shader_t *shader,
                                           VkPipelineLayout pipeline_layout, VkRenderPass render_pass) {
  VkResult err;

  VkPipelineShaderStageCreateInfo vert_shader_stage_info = {};
  vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vert_shader_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vert_shader_stage_info.module = shader->vert_shader_module;
  vert_shader_stage_info.pName = "main";

  VkPipelineShaderStageCreateInfo frag_shader_stage_info = {};
  frag_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  frag_shader_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  frag_shader_stage_info.module = shader->frag_shader_module;
  frag_shader_stage_info.pName = "main";

  VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

  VkPipelineVertexInputStateCreateInfo vertex_input_info = {};
  vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkVertexInputBindingDescription binding_description = get_vertex_binding_description();
  uint32_t attribute_description_count = get_vertex_attribute_description_count();
  const VkVertexInputAttributeDescription *attribute_descriptions = get_vertex_attribute_descriptions();

  vertex_input_info.vertexBindingDescriptionCount = 1;
  vertex_input_info.pVertexBindingDescriptions = &binding_description;
  vertex_input_info.vertexAttributeDescriptionCount = attribute_description_count;
  vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer = {};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling = {};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = VK_FALSE;
  depth_stencil.depthWriteEnable = VK_FALSE;
  depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depth_stencil.depthBoundsTestEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState color_blend_attachment = {};
  color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color_blend_attachment.blendEnable = VK_TRUE;
  color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo color_blending = {};
  color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blending.logicOpEnable = VK_FALSE;
  color_blending.attachmentCount = 1;
  color_blending.pAttachments = &color_blend_attachment;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]);
  dynamic_state.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = shader_stages;
  pipeline_info.pVertexInputState = &vertex_input_info;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &rasterizer;
  pipeline_info.pMultisampleState = &multisampling;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &color_blending;
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = pipeline_layout;
  pipeline_info.renderPass = render_pass;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_info.basePipelineIndex = -1;

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

  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = handler->g_QueueFamily;

  err = vkCreateCommandPool(handler->g_Device, &pool_info, handler->g_Allocator,
                            &renderer->transfer_command_pool);
  check_vk_result_line(err, __LINE__);
  if (err != VK_SUCCESS)
    return 1;

  VkDescriptorSetLayoutBinding ubo_layout_binding = {};
  ubo_layout_binding.binding = 0;
  ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  ubo_layout_binding.descriptorCount = 1;
  ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  ubo_layout_binding.pImmutableSamplers = NULL;

  VkDescriptorSetLayoutBinding sampler_layout_binding = {};
  sampler_layout_binding.binding = 1;
  sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sampler_layout_binding.descriptorCount = 1;
  sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  sampler_layout_binding.pImmutableSamplers = NULL;

  VkDescriptorSetLayoutBinding bindings[] = {ubo_layout_binding, sampler_layout_binding};
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 2;
  layout_info.pBindings = bindings;

  err = vkCreateDescriptorSetLayout(handler->g_Device, &layout_info, handler->g_Allocator,
                                    &renderer->object_descriptor_set_layout);
  check_vk_result_line(err, __LINE__);
  if (err != VK_SUCCESS)
    return 1;

  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_RENDERABLES},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_RENDERABLES * MAX_TEXTURES}};

  VkDescriptorPoolCreateInfo pool_info_renderer = {};
  pool_info_renderer.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info_renderer.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info_renderer.maxSets = MAX_RENDERABLES;
  pool_info_renderer.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
  pool_info_renderer.pPoolSizes = pool_sizes;

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

  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    renderable_t *r = &renderer->renderables[i];
    if (!r->active)
      continue;
    vkDestroyPipeline(device, r->pipeline, allocator);
    vkDestroyPipelineLayout(device, r->pipeline_layout, allocator);

    vkDestroyBuffer(device, r->uniform_buffer.buffer, allocator);
    vkFreeMemory(device, r->uniform_buffer.memory, allocator);
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
  vkDestroyDescriptorSetLayout(device, renderer->object_descriptor_set_layout, allocator);

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
  glm_mat4_identity(renderable->model_matrix);

  VkDeviceSize ubo_size = sizeof(uniform_buffer_object_t);
  create_buffer(handler, ubo_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &renderable->uniform_buffer);

  err = vkMapMemory(handler->g_Device, renderable->uniform_buffer.memory, 0, ubo_size, 0,
                    &renderable->uniform_buffer.mapped_memory);
  check_vk_result_line(err, __LINE__);

  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &renderer->object_descriptor_set_layout;

  err = vkCreatePipelineLayout(handler->g_Device, &pipeline_layout_info, handler->g_Allocator,
                               &renderable->pipeline_layout);
  check_vk_result_line(err, __LINE__);

  renderable->pipeline = create_graphics_pipeline(handler, shader, renderable->pipeline_layout,
                                                  handler->g_MainWindowData.RenderPass);

  VkDescriptorSetAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = renderer->resource_descriptor_pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &renderer->object_descriptor_set_layout;

  err = vkAllocateDescriptorSets(handler->g_Device, &alloc_info, &renderable->descriptor_set);
  check_vk_result_line(err, __LINE__);

  VkDescriptorBufferInfo buffer_info = {};
  buffer_info.buffer = renderable->uniform_buffer.buffer;
  buffer_info.offset = 0;
  buffer_info.range = sizeof(uniform_buffer_object_t);

  VkDescriptorImageInfo image_info = {};
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image_info.imageView = renderable->texture->image_view;
  image_info.sampler = renderable->texture->sampler;

  VkWriteDescriptorSet descriptor_writes[2] = {};

  descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_writes[0].dstSet = renderable->descriptor_set;
  descriptor_writes[0].dstBinding = 0;
  descriptor_writes[0].dstArrayElement = 0;
  descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptor_writes[0].descriptorCount = 1;
  descriptor_writes[0].pBufferInfo = &buffer_info;

  descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_writes[1].dstSet = renderable->descriptor_set;
  descriptor_writes[1].dstBinding = 1;
  descriptor_writes[1].dstArrayElement = 0;
  descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_writes[1].descriptorCount = 1;
  descriptor_writes[1].pImageInfo = &image_info;

  vkUpdateDescriptorSets(handler->g_Device, 2, descriptor_writes, 0, NULL);

  printf("Added renderable %u.\n", renderable_index);
  return renderable;
}

void renderer_remove_renderable(gfx_handler_t *handler, renderable_t *renderable) {
  if (!renderable || !renderable->active)
    return;

  renderer_state_t *renderer = &handler->renderer;

  renderable->active = false;

  printf("Marked renderable as inactive.\n");
}

void renderer_update_uniforms(gfx_handler_t *handler) {
  renderer_state_t *renderer = &handler->renderer;

  mat4 view, proj;

  glm_mat4_identity(view);
  glm_translate(view, (vec3){0.0f, 0.0f, -1.0f});

  int width, height;
  glfwGetFramebufferSize(handler->window, &width, &height);
  if (width == 0 || height == 0)
    return;

  glm_ortho(0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f, proj);

  for (uint32_t i = 0; i < renderer->renderable_count; ++i) {
    renderable_t *r = &renderer->renderables[i];
    if (!r->active)
      continue;

    uniform_buffer_object_t ubo = {};
    glm_mat4_copy(r->model_matrix, ubo.model);
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

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)width;
  viewport.height = (float)height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor = {};
  scissor.offset = (VkOffset2D){0, 0};
  scissor.extent = (VkExtent2D){(uint32_t)width, (uint32_t)height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

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

VkVertexInputBindingDescription get_vertex_binding_description() {
  VkVertexInputBindingDescription binding_description = {};
  binding_description.binding = 0;
  binding_description.stride = sizeof(vertex_t);
  binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  return binding_description;
}

static VkVertexInputAttributeDescription attribute_descriptions[3];

uint32_t get_vertex_attribute_description_count() {
  return sizeof(attribute_descriptions) / sizeof(attribute_descriptions[0]);
}

const VkVertexInputAttributeDescription *get_vertex_attribute_descriptions() {
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
