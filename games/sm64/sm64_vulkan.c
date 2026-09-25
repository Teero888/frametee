// The Vulkan backend of the Fast3D interpreter (sm64_vulkan.h). What
// fast3d/gfx_pc.c asks of a backend, as sm64-port's OpenGL one does it: a
// black frame with a cleared depth buffer, depth test less-or-equal, source
// alpha blending, a polygon offset for decals, viewports and scissors with the
// origin at the bottom left. One shader (shaders/fast3d.*) does every color
// combiner, which it gets as a push constant; every vertex has the same layout.
//
// A frame records two command buffers, uploads first, then the draws, and
// submits both at its end. A texture uploaded again gets a new image, so draws
// recorded earlier in the frame keep the one they named.
#include "sm64_vulkan.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fast3d/gfx_cc.h"
#include "fast3d/gfx_pc.h"
#include "fast3d/gfx_rendering_api.h"
#include "fast3d_frag_spv.h"
#include "fast3d_vert_spv.h"

#define VERTEX_FLOATS 26
#define CHUNK_SIZE (4u << 20)
#define SETS_PER_POOL 1024u
// Pipelines differ in blending, depth test, depth writes and decal offset.
#define PIPELINE_COUNT 16

struct ShaderProgram {
    uint32_t shader_id;
    uint8_t num_inputs;
    bool used_textures[2];
};

// What gfx_pc.c keeps pointers to (its color combiners): the same for every
// renderer, as a shader is only its combiner's features.
static struct ShaderProgram sShaders[256];
static size_t sShaderCount;

struct texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    bool linear;
    uint32_t cms, cmt;
};

// Host-visible memory handed out front to back and taken back every frame.
struct chunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char *mapped;
    VkDeviceSize size, used;
};

struct chunks {
    struct chunk *items;
    size_t count;
    VkBufferUsageFlags usage;
};

struct sm64_vulkan {
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkCommandPool command_pool;
    VkCommandBuffer upload, draw;
    VkFence fence;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkShaderModule vertex_shader, fragment_shader;
    VkSampler samplers[18];
    VkDescriptorPool *pools;
    size_t pool_count, pool_used, sets_in_pool;
    struct texture *textures;
    uint32_t texture_count, texture_capacity;
    struct texture white;
    struct chunks vertices, staging;
    struct texture *garbage;
    size_t garbage_count, garbage_capacity;

    // The image drawn into, and what depends on it.
    VkImage target;
    VkFormat format;
    uint32_t width, height;
    VkImageLayout final_layout;
    VkImageView target_view;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkPipeline pipelines[PIPELINE_COUNT];
    VkFormat depth_format;
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    uint32_t depth_width, depth_height;

    // The frame being recorded.
    bool failed;
    char error[256];
    int selected[2];
    int current_texture;
    struct ShaderProgram *shader;
    bool blend, depth_test, depth_mask, decal;
    VkViewport viewport;
    VkRect2D scissor;
    bool viewport_dirty, scissor_dirty;
    VkPipeline bound_pipeline;
    VkImageView bound_views[2];
    VkSampler bound_samplers[2];
    uint32_t pushed_shader;
    bool pushed;
};

// gfx_pc.c's backend: the renderer drawing now.
static sm64_vulkan *sVk;

static void fail(sm64_vulkan *vk, const char *format, ...) {
    if (vk->failed) {
        return;
    }
    vk->failed = true;
    va_list args;
    va_start(args, format);
    vsnprintf(vk->error, sizeof(vk->error), format, args);
    va_end(args);
}

static bool check(sm64_vulkan *vk, VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        fail(vk, "%s failed (VkResult %d)", what, (int) result);
        return false;
    }
    return true;
}

// --- Memory ------------------------------------------------------------------------

static bool allocate(sm64_vulkan *vk, VkMemoryRequirements requirements, VkMemoryPropertyFlags properties,
                     VkDeviceMemory *memory) {
    for (uint32_t i = 0; i < vk->memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i))
            && (vk->memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = i;
            return check(vk, vkAllocateMemory(vk->device, &info, NULL, memory), "vkAllocateMemory");
        }
    }
    fail(vk, "no memory type for %#x", (unsigned) properties);
    return false;
}

static bool chunk_create(sm64_vulkan *vk, struct chunk *chunk, VkDeviceSize size, VkBufferUsageFlags usage) {
    memset(chunk, 0, sizeof(*chunk));
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vk, vkCreateBuffer(vk->device, &info, NULL, &chunk->buffer), "vkCreateBuffer")) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device, chunk->buffer, &requirements);
    if (!allocate(vk, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &chunk->memory)
        || !check(vk, vkBindBufferMemory(vk->device, chunk->buffer, chunk->memory, 0), "vkBindBufferMemory")
        || !check(vk, vkMapMemory(vk->device, chunk->memory, 0, VK_WHOLE_SIZE, 0, (void **) &chunk->mapped),
                  "vkMapMemory")) {
        return false;
    }
    chunk->size = size;
    return true;
}

static void chunk_destroy(sm64_vulkan *vk, struct chunk *chunk) {
    if (chunk->buffer) {
        vkDestroyBuffer(vk->device, chunk->buffer, NULL);
    }
    if (chunk->memory) {
        vkFreeMemory(vk->device, chunk->memory, NULL);
    }
}

// size bytes of host-visible memory for this frame: the buffer and offset.
static unsigned char *chunks_take(sm64_vulkan *vk, struct chunks *chunks, VkDeviceSize size, VkBuffer *buffer,
                                  VkDeviceSize *offset) {
    size = (size + 15) & ~(VkDeviceSize) 15;
    for (size_t i = 0; i < chunks->count; ++i) {
        struct chunk *chunk = &chunks->items[i];
        if (chunk->size - chunk->used >= size) {
            *buffer = chunk->buffer;
            *offset = chunk->used;
            chunk->used += size;
            return chunk->mapped + *offset;
        }
    }
    struct chunk *grown = realloc(chunks->items, (chunks->count + 1) * sizeof(*grown));
    if (!grown) {
        fail(vk, "out of memory");
        return NULL;
    }
    chunks->items = grown;
    struct chunk *chunk = &chunks->items[chunks->count];
    if (!chunk_create(vk, chunk, size > CHUNK_SIZE ? size : CHUNK_SIZE, chunks->usage)) {
        chunk_destroy(vk, chunk);
        return NULL;
    }
    ++chunks->count;
    *buffer = chunk->buffer;
    *offset = 0;
    chunk->used = size;
    return chunk->mapped;
}

static void chunks_reset(struct chunks *chunks) {
    for (size_t i = 0; i < chunks->count; ++i) {
        chunks->items[i].used = 0;
    }
}

static void chunks_destroy(sm64_vulkan *vk, struct chunks *chunks) {
    for (size_t i = 0; i < chunks->count; ++i) {
        chunk_destroy(vk, &chunks->items[i]);
    }
    free(chunks->items);
    memset(chunks, 0, sizeof(*chunks));
}

// --- Images ------------------------------------------------------------------------

static bool image_create(sm64_vulkan *vk, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *image, VkDeviceMemory *memory, VkImageView *view) {
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = (VkExtent3D) { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vk, vkCreateImage(vk->device, &info, NULL, image), "vkCreateImage")) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk->device, *image, &requirements);
    if (!allocate(vk, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory)
        || !check(vk, vkBindImageMemory(vk->device, *image, *memory, 0), "vkBindImageMemory")) {
        return false;
    }
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = (VkImageSubresourceRange) { aspect, 0, 1, 0, 1 };
    return check(vk, vkCreateImageView(vk->device, &view_info, NULL, view), "vkCreateImageView");
}

static void texture_destroy(sm64_vulkan *vk, struct texture *texture) {
    if (texture->view) {
        vkDestroyImageView(vk->device, texture->view, NULL);
    }
    if (texture->image) {
        vkDestroyImage(vk->device, texture->image, NULL);
    }
    if (texture->memory) {
        vkFreeMemory(vk->device, texture->memory, NULL);
    }
    texture->view = VK_NULL_HANDLE;
    texture->image = VK_NULL_HANDLE;
    texture->memory = VK_NULL_HANDLE;
}

// Records into the upload command buffer: pixels (RGBA8) into a new image.
static bool texture_upload(sm64_vulkan *vk, struct texture *texture, const uint8_t *rgba, uint32_t width,
                           uint32_t height) {
    if (!image_create(vk, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      &texture->image, &texture->memory, &texture->view)) {
        return false;
    }
    VkBuffer buffer;
    VkDeviceSize offset;
    unsigned char *staging = chunks_take(vk, &vk->staging, (VkDeviceSize) width * height * 4, &buffer, &offset);
    if (!staging) {
        return false;
    }
    memcpy(staging, rgba, (size_t) width * height * 4);
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture->image;
    barrier.subresourceRange = (VkImageSubresourceRange) { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(vk->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    VkBufferImageCopy copy = { 0 };
    copy.bufferOffset = offset;
    copy.imageSubresource = (VkImageSubresourceLayers) { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = (VkExtent3D) { width, height, 1 };
    vkCmdCopyBufferToImage(vk->upload, buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(vk->upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);
    return true;
}

// --- The target and its pipelines ----------------------------------------------

static void target_release(sm64_vulkan *vk) {
    for (int i = 0; i < PIPELINE_COUNT; ++i) {
        if (vk->pipelines[i]) {
            vkDestroyPipeline(vk->device, vk->pipelines[i], NULL);
            vk->pipelines[i] = VK_NULL_HANDLE;
        }
    }
    if (vk->framebuffer) {
        vkDestroyFramebuffer(vk->device, vk->framebuffer, NULL);
    }
    if (vk->render_pass) {
        vkDestroyRenderPass(vk->device, vk->render_pass, NULL);
    }
    if (vk->target_view) {
        vkDestroyImageView(vk->device, vk->target_view, NULL);
    }
    vk->framebuffer = VK_NULL_HANDLE;
    vk->render_pass = VK_NULL_HANDLE;
    vk->target_view = VK_NULL_HANDLE;
    vk->target = VK_NULL_HANDLE;
}

static void depth_release(sm64_vulkan *vk) {
    struct texture depth = { vk->depth, vk->depth_memory, vk->depth_view, false, 0, 0 };
    texture_destroy(vk, &depth);
    vk->depth = VK_NULL_HANDLE;
    vk->depth_memory = VK_NULL_HANDLE;
    vk->depth_view = VK_NULL_HANDLE;
    vk->depth_width = vk->depth_height = 0;
}

static VkPipeline pipeline_create(sm64_vulkan *vk, int key) {
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vk->vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vk->fragment_shader;
    stages[1].pName = "main";

    // Position, texture coordinates, fog, four inputs (fast3d/gfx_pc.c).
    VkVertexInputBindingDescription binding = { 0, VERTEX_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[7] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },       { 1, 0, VK_FORMAT_R32G32_SFLOAT, 16 },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24 },      { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 40 },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56 },      { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 72 },
        { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 88 },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 7;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    const bool blend = key & 1, depth_test = key & 2, depth_mask = key & 4, decal = key & 8;
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // gfx_pc.c culls itself
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = decal;
    raster.depthBiasConstantFactor = -2.0f;
    raster.depthBiasSlopeFactor = -2.0f;
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable = depth_test;
    depth.depthWriteEnable = depth_test && depth_mask;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment = { 0 };
    attachment.blendEnable = blend;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    // The frame stays opaque: alpha is the clear's.
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    color.attachmentCount = 1;
    color.pAttachments = &attachment;
    const VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color;
    info.pDynamicState = &dynamic;
    info.layout = vk->pipeline_layout;
    info.renderPass = vk->render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vk, vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline),
          "vkCreateGraphicsPipelines");
    return pipeline;
}

static bool target_prepare(sm64_vulkan *vk, VkImage image, VkFormat format, uint32_t width, uint32_t height,
                           VkImageLayout final_layout) {
    if (vk->depth_width != width || vk->depth_height != height) {
        depth_release(vk);
        if (!image_create(vk, width, height, vk->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT, &vk->depth, &vk->depth_memory, &vk->depth_view)) {
            return false;
        }
        vk->depth_width = width;
        vk->depth_height = height;
        target_release(vk);
    }
    const bool same_pass = vk->render_pass && vk->format == format && vk->final_layout == final_layout;
    if (vk->target == image && vk->width == width && vk->height == height && same_pass) {
        return true;
    }
    if (vk->framebuffer) {
        vkDestroyFramebuffer(vk->device, vk->framebuffer, NULL);
        vk->framebuffer = VK_NULL_HANDLE;
    }
    if (vk->target_view) {
        vkDestroyImageView(vk->device, vk->target_view, NULL);
        vk->target_view = VK_NULL_HANDLE;
    }
    if (!same_pass) {
        target_release(vk);
        VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
        attachments[0].format = format;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = final_layout;
        attachments[1] = attachments[0];
        attachments[1].format = vk->depth_format;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = { 0 };
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        // Whoever sampled the image before (the engine's last frame) is done
        // before this writes it; what reads it next waits for the writes.
        VkSubpassDependency dependencies[2] = { { 0 }, { 0 } };
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                       | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                       | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                       | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                                       | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT
                                        | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;
        if (!check(vk, vkCreateRenderPass(vk->device, &info, NULL, &vk->render_pass), "vkCreateRenderPass")) {
            return false;
        }
        for (int key = 0; key < PIPELINE_COUNT; ++key) {
            if (!(vk->pipelines[key] = pipeline_create(vk, key))) {
                return false;
            }
        }
        vk->format = format;
        vk->final_layout = final_layout;
    }
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = (VkImageSubresourceRange) { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (!check(vk, vkCreateImageView(vk->device, &view_info, NULL, &vk->target_view), "vkCreateImageView")) {
        return false;
    }
    VkImageView views[2] = { vk->target_view, vk->depth_view };
    VkFramebufferCreateInfo framebuffer = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebuffer.renderPass = vk->render_pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.layers = 1;
    if (!check(vk, vkCreateFramebuffer(vk->device, &framebuffer, NULL, &vk->framebuffer), "vkCreateFramebuffer")) {
        return false;
    }
    vk->target = image;
    vk->width = width;
    vk->height = height;
    return true;
}

// --- gfx_pc.c's backend ---------------------------------------------------------

static bool api_z_is_from_0_to_1(void) {
    return true;
}

static void api_unload_shader(struct ShaderProgram *old) {
    (void) old;
}

static void api_load_shader(struct ShaderProgram *program) {
    sVk->shader = program;
}

static struct ShaderProgram *api_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < sShaderCount; ++i) {
        if (sShaders[i].shader_id == shader_id) {
            return &sShaders[i];
        }
    }
    return NULL;
}

static struct ShaderProgram *api_create_and_load_new_shader(uint32_t shader_id) {
    struct ShaderProgram *program = api_lookup_shader(shader_id);
    if (!program) {
        if (sShaderCount == sizeof(sShaders) / sizeof(sShaders[0])) {
            program = &sShaders[0]; // never with SM64's few combiners
        } else {
            program = &sShaders[sShaderCount++];
        }
        struct CCFeatures features;
        gfx_cc_get_features(shader_id, &features);
        program->shader_id = shader_id;
        program->num_inputs = (uint8_t) features.num_inputs;
        program->used_textures[0] = features.used_textures[0];
        program->used_textures[1] = features.used_textures[1];
    }
    api_load_shader(program);
    return program;
}

static void api_shader_get_info(struct ShaderProgram *program, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = program->num_inputs;
    used_textures[0] = program->used_textures[0];
    used_textures[1] = program->used_textures[1];
}

static uint32_t api_new_texture(void) {
    sm64_vulkan *vk = sVk;
    if (vk->texture_count == vk->texture_capacity) {
        const uint32_t capacity = vk->texture_capacity ? vk->texture_capacity * 2 : 256;
        struct texture *grown = realloc(vk->textures, capacity * sizeof(*grown));
        if (!grown) {
            fail(vk, "out of memory");
            return 0;
        }
        vk->textures = grown;
        vk->texture_capacity = capacity;
    }
    memset(&vk->textures[vk->texture_count], 0, sizeof(vk->textures[0]));
    return vk->texture_count++;
}

static void api_select_texture(int tile, uint32_t id) {
    sVk->selected[tile] = (int) id;
    sVk->current_texture = (int) id;
}

static void api_upload_texture(const uint8_t *rgba, int width, int height) {
    sm64_vulkan *vk = sVk;
    if (vk->failed || vk->current_texture < 0 || (uint32_t) vk->current_texture >= vk->texture_count || width <= 0
        || height <= 0) {
        return;
    }
    struct texture *texture = &vk->textures[vk->current_texture];
    if (texture->image) {
        // Draws recorded earlier this frame still name it.
        if (vk->garbage_count == vk->garbage_capacity) {
            const size_t capacity = vk->garbage_capacity ? vk->garbage_capacity * 2 : 64;
            struct texture *grown = realloc(vk->garbage, capacity * sizeof(*grown));
            if (!grown) {
                fail(vk, "out of memory");
                return;
            }
            vk->garbage = grown;
            vk->garbage_capacity = capacity;
        }
        vk->garbage[vk->garbage_count++] = *texture;
        texture->image = VK_NULL_HANDLE;
        texture->memory = VK_NULL_HANDLE;
        texture->view = VK_NULL_HANDLE;
    }
    texture_upload(vk, texture, rgba, (uint32_t) width, (uint32_t) height);
}

static void api_set_sampler_parameters(int tile, bool linear, uint32_t cms, uint32_t cmt) {
    sm64_vulkan *vk = sVk;
    const int id = vk->selected[tile];
    if (id >= 0 && (uint32_t) id < vk->texture_count) {
        vk->textures[id].linear = linear;
        vk->textures[id].cms = cms;
        vk->textures[id].cmt = cmt;
    }
}

static void api_set_depth_test(bool depth_test) {
    sVk->depth_test = depth_test;
}

static void api_set_depth_mask(bool depth_mask) {
    sVk->depth_mask = depth_mask;
}

static void api_set_zmode_decal(bool decal) {
    sVk->decal = decal;
}

// gfx_pc.c's rectangles have their origin at the bottom left, as in OpenGL.
static void api_set_viewport(int x, int y, int width, int height) {
    sm64_vulkan *vk = sVk;
    vk->viewport = (VkViewport) { (float) x, (float) ((int) vk->height - y - height), (float) width, (float) height,
                                  0.0f, 1.0f };
    vk->viewport_dirty = true;
}

static void api_set_scissor(int x, int y, int width, int height) {
    sm64_vulkan *vk = sVk;
    int top = (int) vk->height - y - height;
    int x1 = x + width, y1 = top + height;
    x = x < 0 ? 0 : x;
    top = top < 0 ? 0 : top;
    x1 = x1 > (int) vk->width ? (int) vk->width : x1;
    y1 = y1 > (int) vk->height ? (int) vk->height : y1;
    vk->scissor.offset = (VkOffset2D) { x, top };
    vk->scissor.extent = (VkExtent2D) { (uint32_t) (x1 > x ? x1 - x : 0), (uint32_t) (y1 > top ? y1 - top : 0) };
    vk->scissor_dirty = true;
}

static void api_set_use_alpha(bool use_alpha) {
    sVk->blend = use_alpha;
}

// G_TX_CLAMP clamps, G_TX_MIRROR mirrors, else repeat: sampler (linear, s, t).
static int wrap_index(uint32_t cm) {
    return (cm & 2) ? 0 : (cm & 1) ? 1 : 2;
}

static VkSampler sampler_for(sm64_vulkan *vk, const struct texture *texture) {
    return vk->samplers[(texture->linear ? 9 : 0) + wrap_index(texture->cms) * 3 + wrap_index(texture->cmt)];
}

static VkDescriptorSet descriptor_set(sm64_vulkan *vk) {
    for (;;) {
        if (vk->pool_used == vk->pool_count) {
            VkDescriptorPool *grown = realloc(vk->pools, (vk->pool_count + 1) * sizeof(*grown));
            if (!grown) {
                fail(vk, "out of memory");
                return VK_NULL_HANDLE;
            }
            vk->pools = grown;
            VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * SETS_PER_POOL };
            VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            info.maxSets = SETS_PER_POOL;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            if (!check(vk, vkCreateDescriptorPool(vk->device, &info, NULL, &vk->pools[vk->pool_count]),
                       "vkCreateDescriptorPool")) {
                return VK_NULL_HANDLE;
            }
            ++vk->pool_count;
            vk->sets_in_pool = 0;
        }
        if (vk->sets_in_pool < SETS_PER_POOL) {
            VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            info.descriptorPool = vk->pools[vk->pool_used];
            info.descriptorSetCount = 1;
            info.pSetLayouts = &vk->set_layout;
            VkDescriptorSet set;
            if (!check(vk, vkAllocateDescriptorSets(vk->device, &info, &set), "vkAllocateDescriptorSets")) {
                return VK_NULL_HANDLE;
            }
            ++vk->sets_in_pool;
            return set;
        }
        ++vk->pool_used;
        vk->sets_in_pool = 0;
    }
}

static void api_draw_triangles(float vertices[], size_t length, size_t triangles) {
    sm64_vulkan *vk = sVk;
    if (vk->failed || !vk->shader || triangles == 0) {
        return;
    }
    VkCommandBuffer cmd = vk->draw;
    const int key = (vk->blend ? 1 : 0) | (vk->depth_test ? 2 : 0) | (vk->depth_mask ? 4 : 0) | (vk->decal ? 8 : 0);
    if (vk->bound_pipeline != vk->pipelines[key]) {
        vk->bound_pipeline = vk->pipelines[key];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->bound_pipeline);
    }
    if (vk->viewport_dirty) {
        vkCmdSetViewport(cmd, 0, 1, &vk->viewport);
        vk->viewport_dirty = false;
    }
    if (vk->scissor_dirty) {
        vkCmdSetScissor(cmd, 0, 1, &vk->scissor);
        vk->scissor_dirty = false;
    }
    VkImageView views[2];
    VkSampler samplers[2];
    for (int i = 0; i < 2; ++i) {
        const int id = vk->selected[i];
        const struct texture *texture = &vk->white;
        if (vk->shader->used_textures[i] && id >= 0 && (uint32_t) id < vk->texture_count && vk->textures[id].view) {
            texture = &vk->textures[id];
        }
        views[i] = texture->view;
        samplers[i] = sampler_for(vk, texture);
    }
    if (memcmp(views, vk->bound_views, sizeof(views)) != 0 || memcmp(samplers, vk->bound_samplers, sizeof(samplers)) != 0) {
        VkDescriptorSet set = descriptor_set(vk);
        if (!set) {
            return;
        }
        VkDescriptorImageInfo images[2];
        VkWriteDescriptorSet writes[2];
        for (int i = 0; i < 2; ++i) {
            images[i] = (VkDescriptorImageInfo) { samplers[i], views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            writes[i] = (VkWriteDescriptorSet) { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet = set;
            writes[i].dstBinding = (uint32_t) i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipeline_layout, 0, 1, &set, 0, NULL);
        memcpy(vk->bound_views, views, sizeof(views));
        memcpy(vk->bound_samplers, samplers, sizeof(samplers));
    }
    if (!vk->pushed || vk->pushed_shader != vk->shader->shader_id) {
        vk->pushed_shader = vk->shader->shader_id;
        vk->pushed = true;
        vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t),
                           &vk->pushed_shader);
    }
    VkBuffer buffer;
    VkDeviceSize offset;
    unsigned char *data = chunks_take(vk, &vk->vertices, length * sizeof(float), &buffer, &offset);
    if (!data) {
        return;
    }
    memcpy(data, vertices, length * sizeof(float));
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    vkCmdDraw(cmd, (uint32_t) (3 * triangles), 1, 0, 0);
}

static void api_init(void) {
}

static void api_on_resize(void) {
}

static void api_start_frame(void) {
    sm64_vulkan *vk = sVk;
    chunks_reset(&vk->vertices);
    chunks_reset(&vk->staging);
    for (size_t i = 0; i < vk->pool_count; ++i) {
        vkResetDescriptorPool(vk->device, vk->pools[i], 0);
    }
    vk->pool_used = 0;
    vk->sets_in_pool = 0;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vk, vkBeginCommandBuffer(vk->upload, &begin), "vkBeginCommandBuffer")
        || !check(vk, vkBeginCommandBuffer(vk->draw, &begin), "vkBeginCommandBuffer")) {
        return;
    }
    VkClearValue clear[2];
    clear[0].color = (VkClearColorValue) { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clear[1].depthStencil = (VkClearDepthStencilValue) { 1.0f, 0 };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass.renderPass = vk->render_pass;
    pass.framebuffer = vk->framebuffer;
    pass.renderArea.extent = (VkExtent2D) { vk->width, vk->height };
    pass.clearValueCount = 2;
    pass.pClearValues = clear;
    vkCmdBeginRenderPass(vk->draw, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vk->bound_pipeline = VK_NULL_HANDLE;
    memset(vk->bound_views, 0, sizeof(vk->bound_views));
    memset(vk->bound_samplers, 0, sizeof(vk->bound_samplers));
    vk->pushed = false;
    vk->viewport = (VkViewport) { 0.0f, 0.0f, (float) vk->width, (float) vk->height, 0.0f, 1.0f };
    vk->scissor = (VkRect2D) { { 0, 0 }, { vk->width, vk->height } };
    vk->viewport_dirty = vk->scissor_dirty = true;
}

static void api_end_frame(void) {
    sm64_vulkan *vk = sVk;
    vkCmdEndRenderPass(vk->draw);
    if (!check(vk, vkEndCommandBuffer(vk->upload), "vkEndCommandBuffer")
        || !check(vk, vkEndCommandBuffer(vk->draw), "vkEndCommandBuffer")) {
        return;
    }
    if (vk->failed) {
        return;
    }
    VkCommandBuffer buffers[2] = { vk->upload, vk->draw };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    if (!check(vk, vkResetFences(vk->device, 1, &vk->fence), "vkResetFences")
        || !check(vk, vkQueueSubmit(vk->queue, 1, &submit, vk->fence), "vkQueueSubmit")) {
        return;
    }
    check(vk, vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

static void api_finish_render(void) {
}

static struct GfxRenderingAPI sApi = {
    api_z_is_from_0_to_1,   api_unload_shader,      api_load_shader,
    api_create_and_load_new_shader, api_lookup_shader, api_shader_get_info,
    api_new_texture,        api_select_texture,     api_upload_texture,
    api_set_sampler_parameters, api_set_depth_test, api_set_depth_mask,
    api_set_zmode_decal,    api_set_viewport,       api_set_scissor,
    api_set_use_alpha,      api_draw_triangles,     api_init,
    api_on_resize,          api_start_frame,        api_end_frame,
    api_finish_render,
};

// --- The renderer ------------------------------------------------------------------

static VkShaderModule shader_module(sm64_vulkan *vk, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vk, vkCreateShaderModule(vk->device, &info, NULL, &module), "vkCreateShaderModule");
    return module;
}

static bool create_objects(sm64_vulkan *vk, uint32_t queue_family) {
    vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &vk->memory_properties);
    const VkFormat depth_formats[3] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    vk->depth_format = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3 && vk->depth_format == VK_FORMAT_UNDEFINED; ++i) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(vk->physical_device, depth_formats[i], &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            vk->depth_format = depth_formats[i];
        }
    }
    if (vk->depth_format == VK_FORMAT_UNDEFINED) {
        fail(vk, "no depth format");
        return false;
    }

    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family;
    if (!check(vk, vkCreateCommandPool(vk->device, &pool, NULL, &vk->command_pool), "vkCreateCommandPool")) {
        return false;
    }
    VkCommandBuffer buffers[2];
    VkCommandBufferAllocateInfo allocate_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate_info.commandPool = vk->command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 2;
    if (!check(vk, vkAllocateCommandBuffers(vk->device, &allocate_info, buffers), "vkAllocateCommandBuffers")) {
        return false;
    }
    vk->upload = buffers[0];
    vk->draw = buffers[1];
    VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (!check(vk, vkCreateFence(vk->device, &fence, NULL, &vk->fence), "vkCreateFence")) {
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2] = { { 0 }, { 0 } };
    for (int i = 0; i < 2; ++i) {
        bindings[i].binding = (uint32_t) i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_layout.bindingCount = 2;
    set_layout.pBindings = bindings;
    if (!check(vk, vkCreateDescriptorSetLayout(vk->device, &set_layout, NULL, &vk->set_layout),
               "vkCreateDescriptorSetLayout")) {
        return false;
    }
    VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &vk->set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(vk, vkCreatePipelineLayout(vk->device, &layout, NULL, &vk->pipeline_layout),
               "vkCreatePipelineLayout")) {
        return false;
    }
    vk->vertex_shader = shader_module(vk, kFast3dVertSpv, sizeof(kFast3dVertSpv));
    vk->fragment_shader = shader_module(vk, kFast3dFragSpv, sizeof(kFast3dFragSpv));
    if (vk->failed) {
        return false;
    }

    const VkSamplerAddressMode modes[3] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                            VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    for (int linear = 0; linear < 2; ++linear) {
        for (int s = 0; s < 3; ++s) {
            for (int t = 0; t < 3; ++t) {
                VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                info.magFilter = info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                info.addressModeU = modes[s];
                info.addressModeV = modes[t];
                info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                info.maxLod = 0.0f;
                if (!check(vk, vkCreateSampler(vk->device, &info, NULL, &vk->samplers[linear * 9 + s * 3 + t]),
                           "vkCreateSampler")) {
                    return false;
                }
            }
        }
    }

    vk->vertices.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vk->staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    // A white texel for the samplers a combiner does not use.
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const uint8_t white[4] = { 255, 255, 255, 255 };
    if (!check(vk, vkBeginCommandBuffer(vk->upload, &begin), "vkBeginCommandBuffer")
        || !texture_upload(vk, &vk->white, white, 1, 1)
        || !check(vk, vkEndCommandBuffer(vk->upload), "vkEndCommandBuffer")) {
        return false;
    }
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk->upload;
    return check(vk, vkQueueSubmit(vk->queue, 1, &submit, vk->fence), "vkQueueSubmit")
           && check(vk, vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

sm64_vulkan *sm64_vulkan_create(VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
                                uint32_t queue_family, char *error, size_t error_size) {
    sm64_vulkan *vk = calloc(1, sizeof(*vk));
    if (!vk) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    vk->physical_device = physical_device;
    vk->device = device;
    vk->queue = queue;
    vk->selected[0] = vk->selected[1] = vk->current_texture = -1;
    if (!create_objects(vk, queue_family)) {
        snprintf(error, error_size, "%s", vk->error);
        sm64_vulkan_destroy(vk);
        return NULL;
    }
    // The interpreter's textures were the last renderer's.
    sVk = vk;
    gfx_reset_textures();
    gfx_init(&sApi);
    return vk;
}

void sm64_vulkan_destroy(sm64_vulkan *vk) {
    if (!vk) {
        return;
    }
    if (vk->device) {
        vkDeviceWaitIdle(vk->device);
    }
    target_release(vk);
    depth_release(vk);
    for (uint32_t i = 0; i < vk->texture_count; ++i) {
        texture_destroy(vk, &vk->textures[i]);
    }
    free(vk->textures);
    for (size_t i = 0; i < vk->garbage_count; ++i) {
        texture_destroy(vk, &vk->garbage[i]);
    }
    free(vk->garbage);
    texture_destroy(vk, &vk->white);
    chunks_destroy(vk, &vk->vertices);
    chunks_destroy(vk, &vk->staging);
    for (size_t i = 0; i < vk->pool_count; ++i) {
        vkDestroyDescriptorPool(vk->device, vk->pools[i], NULL);
    }
    free(vk->pools);
    for (int i = 0; i < 18; ++i) {
        if (vk->samplers[i]) {
            vkDestroySampler(vk->device, vk->samplers[i], NULL);
        }
    }
    if (vk->vertex_shader) {
        vkDestroyShaderModule(vk->device, vk->vertex_shader, NULL);
    }
    if (vk->fragment_shader) {
        vkDestroyShaderModule(vk->device, vk->fragment_shader, NULL);
    }
    if (vk->pipeline_layout) {
        vkDestroyPipelineLayout(vk->device, vk->pipeline_layout, NULL);
    }
    if (vk->set_layout) {
        vkDestroyDescriptorSetLayout(vk->device, vk->set_layout, NULL);
    }
    if (vk->fence) {
        vkDestroyFence(vk->device, vk->fence, NULL);
    }
    if (vk->command_pool) {
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    }
    if (sVk == vk) {
        sVk = NULL;
    }
    free(vk);
}

bool sm64_vulkan_draw(sm64_vulkan *vk, const void *display_list, const float (*camera)[4], VkImage image,
                      VkFormat format, uint32_t width, uint32_t height, VkImageLayout final_layout, char *error,
                      size_t error_size) {
    if (sVk != vk) {
        // Another renderer drew last: its textures are not this one's.
        sVk = vk;
        gfx_reset_textures();
        for (uint32_t i = 0; i < vk->texture_count; ++i) {
            texture_destroy(vk, &vk->textures[i]);
        }
        vk->texture_count = 0;
    }
    vk->failed = false;
    if (!target_prepare(vk, image, format, width, height, final_layout)) {
        snprintf(error, error_size, "%s", vk->error);
        return false;
    }
    gfx_set_camera(camera);
    gfx_run((Gfx *) display_list, width, height);
    gfx_set_camera(NULL);
    for (size_t i = 0; i < vk->garbage_count; ++i) {
        texture_destroy(vk, &vk->garbage[i]);
    }
    vk->garbage_count = 0;
    if (vk->failed) {
        snprintf(error, error_size, "%s", vk->error);
        // A frame that failed half way leaves its command buffers recording.
        vkDeviceWaitIdle(vk->device);
        vkResetCommandBuffer(vk->upload, 0);
        vkResetCommandBuffer(vk->draw, 0);
        return false;
    }
    return true;
}
