// TMNF owns this renderer. Static track vertices stay on the device; the host
// receives one completed image and continues to draw its editor overlays.
#include "tmnf_internal.h"

#include "material_frag_spv.h"
#include "material_vert_spv.h"
#include "present_frag_spv.h"
#include "present_vert_spv.h"
#include "reflection_frag_spv.h"
#include "shadow_frag_spv.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <vulkan/vulkan.h>

namespace tmnf {
namespace {
void Check(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " (Vulkan " + std::to_string(result) + ")");
}
struct Buffer {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
  void *mapped{};
};
struct Image {
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  uint32_t width{}, height{}, layers{1}, mips{1};
};
struct Vertex {
  ft_vec3 position, normal;
  ft_vec2 uv, uv1;
  uint32_t color;
};
struct Batch {
  uint32_t first{}, count{}, material{};
  Aabb bounds;
  bool backdrop{}, dynamic{};
};
struct alignas(16) Uniforms {
  float vp[16]{}, reflection[16]{}, shadow[16]{};
  float eye_time[4]{}, sun_direction[4]{}, sun_color[4]{}, ambient[4]{}, fog[4]{}, clip[4]{}, viewport[4]{};
};
struct Push {
  float animation[4]{1, 1, 1, 0};
  uint32_t flags[4]{};
  float tint[4]{1, 1, 1, 1};
};
struct DrawMaterial {
  RenderMaterial source;
  VkDescriptorSet descriptor{};
  Push push;
};
struct CarTriangle {
  Vertex vertices[3];
  uint32_t layer;
  bool blended;
  uint32_t material = kNoTextureLayer, object{};
};

void Multiply(const float *a, const float *b, float *out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      out[c * 4 + r] = 0;
      for (int k = 0; k < 4; ++k)
        out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    }
}
void ShadowMatrix(const ft_level &level, ft_vec3 light, float *out) {
  const Aabb &bounds = level.world_bounds;
  ft_vec3 center = Scale(Add(bounds.mn, bounds.mx), .5f);
  center.x = level.bounds.x + level.bounds.w * .5f;
  center.z = level.bounds.y + level.bounds.h * .5f;
  const float extent = std::max({level.bounds.w, level.bounds.h, 128.f}) * .65f + 64.f;
  ft_vec3 z = Scale(light, -1), x = Normalize(Cross(z, ft_vec3{0, 1, 0})), y = Cross(x, z);
  const float depth = std::max(2048.f, extent * 4);
  std::fill(out, out + 16, 0.f);
  out[0] = x.x / extent;
  out[4] = x.y / extent;
  out[8] = x.z / extent;
  out[12] = -Dot(x, center) / extent;
  out[1] = y.x / extent;
  out[5] = y.y / extent;
  out[9] = y.z / extent;
  out[13] = -Dot(y, center) / extent;
  out[2] = z.x / depth;
  out[6] = z.y / depth;
  out[10] = z.z / depth;
  out[14] = .5f - Dot(z, center) / depth;
  out[15] = 1;
}
ft_vec3 Center(const Batch &batch) { return Scale(Add(batch.bounds.mn, batch.bounds.mx), .5f); }
} // namespace

struct GpuRenderer {
  ft_game *game;
  const ft_engine_api *api;
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkDescriptorSetLayout set_layout{};
  VkDescriptorPool descriptors{};
  VkPipelineLayout layout{};
  VkRenderPass clear_pass{}, load_pass{}, shadow_pass{}, shadow_load_pass{};
  std::array<VkPipeline, 2> opaque{}, blended{}, reflection_pipeline{};
  VkPipeline shadow_pipeline{}, bound_pipeline{};
  VkBuffer bound_vertices{};
  VkDescriptorSet bound_descriptor{};
  VkSampler sampler{}, screen_sampler{}, shadow_sampler{};
  Buffer uniforms, static_vertices, dynamic_vertices;
  uint32_t uniform_stride{};
  Image white, scene_image, reflection, reflection_depth, refraction, depth, shadow, static_shadow;
  std::vector<Image> textures;
  std::vector<DrawMaterial> materials;
  std::vector<Batch> batches, dynamic_batches;
  std::vector<CarTriangle> cars;
  std::unordered_map<uint64_t, uint32_t> car_materials;
  VkFramebuffer main_framebuffer{}, reflection_framebuffer{}, shadow_framebuffer{}, static_shadow_framebuffer{};
  ft_texture *target{};
  ft_gpu_image target_image{};
  std::vector<ft_texture *> retired_targets;
  ft_pipeline *present{};
  ft_mesh *quad{};
  const ft_level *loaded_level{};
  ft_render_frame frame{};
  uint32_t width{}, height{};
  bool shadow_ready{}, submitted{}, collecting{}, target_created{};
  float water_height{};
  bool has_water{};
  bool failed{};
  uint32_t shadow_settings = ~0u;
  uint32_t capture_object{};
  VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

  explicit GpuRenderer(ft_game *owner) : game(owner), api(owner->engine) {}
  ~GpuRenderer() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    DestroyTargets();
    if (target) api->texture_destroy(target);
    for (auto *retired : retired_targets)
      api->texture_destroy(retired);
    if (present) api->pipeline_destroy(present);
    if (quad) api->mesh_destroy(quad);
    for (auto &image : textures)
      Destroy(image);
    Destroy(white);
    Destroy(uniforms);
    Destroy(static_vertices);
    Destroy(dynamic_vertices);
    if (shadow_framebuffer) vkDestroyFramebuffer(device, shadow_framebuffer, nullptr);
    if (static_shadow_framebuffer) vkDestroyFramebuffer(device, static_shadow_framebuffer, nullptr);
    Destroy(shadow);
    Destroy(static_shadow);
    for (const auto &pipelines : {opaque, blended, reflection_pipeline})
      for (auto pipeline : pipelines)
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (shadow_pipeline) vkDestroyPipeline(device, shadow_pipeline, nullptr);
    for (auto pass : {clear_pass, load_pass, shadow_pass, shadow_load_pass})
      if (pass) vkDestroyRenderPass(device, pass, nullptr);
    for (auto s : {sampler, screen_sampler, shadow_sampler})
      if (s) vkDestroySampler(device, s, nullptr);
    if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
    if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
  }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No compatible Vulkan memory type");
  }
  void Destroy(Buffer &b) {
    if (b.mapped) vkUnmapMemory(device, b.memory);
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = {};
  }
  void Destroy(Image &i) {
    if (i.view) vkDestroyImageView(device, i.view, nullptr);
    if (i.image) vkDestroyImage(device, i.image, nullptr);
    if (i.memory) vkFreeMemory(device, i.memory, nullptr);
    i = {};
  }
  void MakeBuffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage, bool mapped) {
    out.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = out.size;
    info.usage = usage;
    Check(vkCreateBuffer(device, &info, nullptr, &out.buffer), "Create buffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(
        requirements.memoryTypeBits, mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device, &allocation, nullptr, &out.memory), "Allocate buffer");
    Check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "Bind buffer");
    if (mapped) Check(vkMapMemory(device, out.memory, 0, out.size, 0, &out.mapped), "Map buffer");
  }
  void MakeImage(Image &out, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, VkFormat format,
                 VkImageUsageFlags usage, bool array = false) {
    out.width = w;
    out.height = h;
    out.layers = layers;
    out.mips = mips;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = mips;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    Check(vkCreateImage(device, &info, nullptr, &out.image), "Create image");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out.image, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device, &allocation, nullptr, &out.memory), "Allocate image");
    Check(vkBindImageMemory(device, out.image, out.memory, 0), "Bind image");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image;
    view.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                             0, mips, 0, layers};
    Check(vkCreateImageView(device, &view, nullptr, &out.view), "Create image view");
  }
  void Wait() {
    if (submitted) Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "Wait for TMNF frame");
    submitted = false;
  }
  void Begin() {
    Wait();
    Check(vkResetCommandPool(device, pool, 0), "Reset command pool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(command, &begin), "Begin TMNF commands");
  }
  void Submit() {
    Check(vkEndCommandBuffer(command), "End TMNF commands");
    Check(vkResetFences(device, 1, &fence), "Reset TMNF fence");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue, 1, &submit, fence), "Submit TMNF frame");
    submitted = true;
  }
  void Barrier(VkImage image, VkImageLayout before, VkImageLayout after, VkImageAspectFlags aspect, uint32_t layers = 1,
               uint32_t mip = 0, uint32_t count = 1) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask =
        before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, mip, count, 0, layers};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
  }
  void UploadImage(Image &out, const TextureLibrary::Page &page) {
    uint32_t layers = 1, w = page.width, h = page.height;
    if (page.animation.kind == TextureAnimationKind::SpriteSheet) {
      layers = page.animation.frame_count;
      w /= page.animation.columns;
      h /= page.animation.rows;
    } else if (page.animation.kind == TextureAnimationKind::StartLights) layers = page.animation.frame_count;
    if (!w || !h || page.rgba.size() < size_t(page.width) * page.height * 4)
      throw std::runtime_error("Invalid decoded image");
    uint32_t mips = 1;
    for (uint32_t d = std::max(w, h); d > 1; d >>= 1)
      ++mips;
    MakeImage(out, w, h, layers, mips, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
    Buffer staging;
    try {
      MakeBuffer(staging, size_t(w) * h * layers * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      auto *dest = static_cast<uint8_t *>(staging.mapped);
      for (uint32_t layer = 0; layer < layers; ++layer) {
        if (page.animation.kind == TextureAnimationKind::StartLights) {
          const auto &lamp = game->textures.Pages().at(page.animation.first_layer + layer);
          std::memcpy(dest + size_t(layer) * w * h * 4, lamp.rgba.data(), size_t(w) * h * 4);
          continue;
        }
        const uint32_t x = layers > 1 ? (layer % page.animation.columns) * w : 0;
        const uint32_t y = layers > 1 ? (layer / page.animation.columns) * h : 0;
        for (uint32_t row = 0; row < h; ++row)
          std::memcpy(dest + (size_t(layer) * h + row) * w * 4, &page.rgba[(size_t(y + row) * page.width + x) * 4],
                      size_t(w) * 4);
      }
      Begin();
      Barrier(out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
              layers, 0, mips);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
      copy.imageExtent = {w, h, 1};
      vkCmdCopyBufferToImage(command, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      for (uint32_t mip = 1; mip < mips; ++mip) {
        Barrier(out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, layers, mip - 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, layers};
        blit.srcOffsets[1] = {int32_t(std::max(1u, w >> (mip - 1))), int32_t(std::max(1u, h >> (mip - 1))), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
        blit.dstOffsets[1] = {int32_t(std::max(1u, w >> mip)), int32_t(std::max(1u, h >> mip)), 1};
        vkCmdBlitImage(command, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Barrier(out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, layers, mip - 1);
      }
      Barrier(out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_ASPECT_COLOR_BIT, layers, mips - 1);
      Submit();
      Wait();
    } catch (...) {
      Destroy(staging);
      throw;
    }
    Destroy(staging);
  }
  VkRenderPass MakePass(bool load, bool shadow_only = false) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = depth_format;
    attachments[1].initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = shadow_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dep{shadow_only ? 0u : 1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = shadow_only ? 0 : 1;
    sub.pColorAttachments = shadow_only ? nullptr : &color;
    sub.pDepthStencilAttachment = &dep;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = dependencies[0].dstStageMask;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = dependencies[0].dstAccessMask;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = shadow_only ? 1 : 2;
    info.pAttachments = shadow_only ? attachments + 1 : attachments;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass pass;
    Check(vkCreateRenderPass(device, &info, nullptr, &pass), "Create render pass");
    return pass;
  }
  VkPipeline MakePipeline(bool blend, bool shadow_only, bool reflected = false, bool cull = false) {
    VkShaderModule vert{}, frag{};
    VkShaderModuleCreateInfo shader{};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof(tmnf_material_vert_spv);
    shader.pCode = tmnf_material_vert_spv;
    Check(vkCreateShaderModule(device, &shader, nullptr, &vert), "Create vertex shader");
    shader.codeSize = sizeof(tmnf_material_frag_spv);
    shader.pCode = tmnf_material_frag_spv;
    if (shadow_only) {
      shader.codeSize = sizeof(tmnf_shadow_frag_spv);
      shader.pCode = tmnf_shadow_frag_spv;
    } else if (reflected) {
      shader.codeSize = sizeof(tmnf_reflection_frag_spv);
      shader.pCode = tmnf_reflection_frag_spv;
    }
    Check(vkCreateShaderModule(device, &shader, nullptr, &frag), "Create fragment shader");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &stage : stages) {
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                                                 {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
                                                 {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                                                 {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv1)},
                                                 {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)}};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = reflected ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    raster.depthBiasEnable = blend || shadow_only;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = blend ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp = shadow_only ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend_state{};
    blend_state.colorWriteMask = 15;
    blend_state.blendEnable = blend;
    blend_state.srcColorBlendFactor = blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_state.dstColorBlendFactor = blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = shadow_only ? 0 : 1;
    cb.pAttachments = shadow_only ? nullptr : &blend_state;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 3;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = shadow_only ? shadow_pass : clear_pass;
    VkPipeline pipeline{};
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    Check(result, "Create material pipeline");
    return pipeline;
  }
  void Initialize(const ft_gpu_device &gpu) {
    physical = static_cast<VkPhysicalDevice>(gpu.physical_device);
    device = static_cast<VkDevice>(gpu.device);
    queue = static_cast<VkQueue>(gpu.queue);
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex = gpu.queue_family_index;
    Check(vkCreateCommandPool(device, &cp, nullptr, &pool), "Create command pool");
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate command buffer");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    Check(vkCreateFence(device, &fi, nullptr, &fence), "Create fence");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkFormatProperties depth_properties{};
    constexpr auto depth_features =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    vkGetPhysicalDeviceFormatProperties(physical, depth_format, &depth_properties);
    if ((depth_properties.optimalTilingFeatures & depth_features) != depth_features) {
      depth_format = VK_FORMAT_D16_UNORM;
      vkGetPhysicalDeviceFormatProperties(physical, depth_format, &depth_properties);
      if ((depth_properties.optimalTilingFeatures & depth_features) != depth_features)
        throw std::runtime_error("No sampleable depth attachment format");
    }
    auto alignment = properties.limits.minUniformBufferOffsetAlignment;
    uniform_stride = static_cast<uint32_t>((sizeof(Uniforms) + alignment - 1) / alignment * alignment);
    MakeBuffer(uniforms, uniform_stride * 3, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    VkDescriptorSetLayoutBinding bindings[9]{};
    for (uint32_t i = 0; i < 9; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorCount = 1;
      bindings[i].descriptorType =
          i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = 9;
    sl.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout), "Create material layout");
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "Create pipeline layout");
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * 8}};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 4096;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors), "Create descriptor pool");
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 16;
    Check(vkCreateSampler(device, &si, nullptr, &sampler), "Create surface sampler");
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(device, &si, nullptr, &screen_sampler), "Create screen sampler");
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0;
    if (!(depth_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
      si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    Check(vkCreateSampler(device, &si, nullptr, &shadow_sampler), "Create shadow sampler");
    clear_pass = MakePass(false);
    load_pass = MakePass(true);
    shadow_pass = MakePass(false, true);
    shadow_load_pass = MakePass(true, true);
    for (unsigned cull = 0; cull < 2; ++cull) {
      opaque[cull] = MakePipeline(false, false, false, cull);
      blended[cull] = MakePipeline(true, false, false, cull);
      reflection_pipeline[cull] = MakePipeline(false, false, true, cull);
    }
    shadow_pipeline = MakePipeline(false, true);
    TextureLibrary::Page page;
    page.width = page.height = 1;
    page.rgba = {255, 255, 255, 255};
    UploadImage(white, page);
    MakeImage(shadow, 2048, 2048, 1, 1, depth_format,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    MakeImage(static_shadow, 2048, 2048, 1, 1, depth_format,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = shadow_pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &shadow.view;
    fb.width = fb.height = 2048;
    fb.layers = 1;
    Check(vkCreateFramebuffer(device, &fb, nullptr, &shadow_framebuffer), "Create shadow framebuffer");
    fb.pAttachments = &static_shadow.view;
    Check(vkCreateFramebuffer(device, &fb, nullptr, &static_shadow_framebuffer), "Create static shadow framebuffer");
    ft_pipeline_desc pd{};
    pd.struct_size = sizeof(pd);
    pd.vertex_spirv = tmnf_present_vert_spv;
    pd.vertex_spirv_size = sizeof(tmnf_present_vert_spv);
    pd.fragment_spirv = tmnf_present_frag_spv;
    pd.fragment_spirv_size = sizeof(tmnf_present_frag_spv);
    pd.texture_count = 1;
    pd.alpha_blend = true;
    present = api->pipeline_create(&pd);
    const ft_vertex vertices[] = {{{-1, -1}, {1, 1, 1}, {0, 0}},
                                  {{1, -1}, {1, 1, 1}, {1, 0}},
                                  {{1, 1}, {1, 1, 1}, {1, 1}},
                                  {{-1, 1}, {1, 1, 1}, {0, 1}}};
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    quad = api->mesh_create(vertices, 4, sizeof(ft_vertex), indices, 6);
    if (!present || !quad) throw std::runtime_error("Cannot create TMNF presentation resources");
  }
  void DestroyTargets() {
    if (main_framebuffer) vkDestroyFramebuffer(device, main_framebuffer, nullptr);
    if (reflection_framebuffer) vkDestroyFramebuffer(device, reflection_framebuffer, nullptr);
    main_framebuffer = reflection_framebuffer = {};
    Destroy(scene_image);
    Destroy(depth);
    Destroy(reflection);
    Destroy(reflection_depth);
    Destroy(refraction);
  }
  void Targets(uint32_t w, uint32_t h) {
    if (w == width && h == height && target) return;
    vkDeviceWaitIdle(device);
    DestroyTargets();
    width = w;
    height = h;
    if (!target || target_image.width < w || target_image.height < h) {
      // The host has already recorded barriers for existing external images.
      // Keep them alive until module shutdown, and grow in powers of two so
      // dragging the viewport border does not allocate an image every frame.
      if (target) retired_targets.push_back(target);
      uint32_t tw = std::max(1u, target_image.width), th = std::max(1u, target_image.height);
      while (tw < w)
        tw *= 2;
      while (th < h)
        th *= 2;
      ft_texture_desc desc{};
      desc.struct_size = sizeof(desc);
      desc.width = tw;
      desc.height = th;
      desc.layers = 1;
      desc.format = FT_TEXTURE_RGBA8;
      desc.linear_filter = true;
      target = api->texture_create(&desc);
      target_image.struct_size = sizeof(target_image);
      if (!target || !api->texture_gpu_image(target, &target_image))
        throw std::runtime_error("Cannot create TMNF render target");
      target_created = true;
    }
    MakeImage(scene_image, w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    MakeImage(depth, w, h, 1, 1, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    MakeImage(reflection, std::max(1u, w / 2), std::max(1u, h / 2), 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    MakeImage(reflection_depth, reflection.width, reflection.height, 1, 1, depth_format,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    MakeImage(refraction, w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView attachments[] = {scene_image.view, depth.view};
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = clear_pass;
    fb.attachmentCount = 2;
    fb.pAttachments = attachments;
    fb.width = w;
    fb.height = h;
    fb.layers = 1;
    Check(vkCreateFramebuffer(device, &fb, nullptr, &main_framebuffer), "Create target framebuffer");
    attachments[0] = reflection.view;
    attachments[1] = reflection_depth.view;
    fb.width = reflection.width;
    fb.height = reflection.height;
    Check(vkCreateFramebuffer(device, &fb, nullptr, &reflection_framebuffer), "Create reflection framebuffer");
    for (auto &material : materials)
      UpdateDescriptor(material);
  }
  void UpdateDescriptor(DrawMaterial &material) {
    if (!reflection.view) return;
    if (!material.descriptor) {
      VkDescriptorSetAllocateInfo allocate{};
      allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      allocate.descriptorPool = descriptors;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &set_layout;
      Check(vkAllocateDescriptorSets(device, &allocate, &material.descriptor), "Allocate material descriptor");
    }
    auto image_view = [&](uint32_t i) { return i < textures.size() ? textures[i].view : white.view; };
    const auto &m = material.source;
    VkDescriptorImageInfo images[] = {{sampler, image_view(m.diffuse), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {sampler, image_view(m.normal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {sampler, image_view(m.specular), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {sampler, image_view(m.occlusion), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {sampler, image_view(m.emission), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {screen_sampler, reflection.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {screen_sampler, refraction.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {shadow_sampler, shadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo buffer{uniforms.buffer, 0, sizeof(Uniforms)};
    VkWriteDescriptorSet writes[9]{};
    for (uint32_t i = 0; i < 9; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = material.descriptor;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      if (i) writes[i].pImageInfo = &images[i - 1];
      else writes[i].pBufferInfo = &buffer;
    }
    vkUpdateDescriptorSets(device, 9, writes, 0, nullptr);
  }
  uint32_t AddMaterial(RenderMaterial source) {
    DrawMaterial material;
    material.source = source;
    auto &flags = material.push.flags[0];
    const auto &s = source.style;
    flags = (s.unlit ? 1u : 0u) | (s.alpha_test ? 2u : 0u) | (s.transparent ? 4u : 0u) |
            (source.normal != kNoTextureLayer ? 8u : 0u) | (source.specular != kNoTextureLayer ? 16u : 0u) |
            (source.occlusion != kNoTextureLayer ? 32u : 0u) | (s.water ? 64u : 0u) | (s.double_sided ? 128u : 0u) |
            (source.emission != kNoTextureLayer ? 512u : 0u) | (s.additive ? 1024u : 0u) |
            (source.animation.kind == TextureAnimationKind::StartLights ? 2048u : 0u) |
            (source.packed_normal ? 4096u : 0u);
    if (source.animation.kind == TextureAnimationKind::SpriteSheet) {
      material.push.animation[2] = source.animation.frame_count;
      material.push.animation[3] = source.animation.frames_per_tick * 100;
    }
    for (uint32_t i = 0; i < materials.size(); ++i) {
      const auto &existing = materials[i];
      const auto &m = existing.source;
      if (m.diffuse == source.diffuse && m.normal == source.normal && m.specular == source.specular &&
          m.occlusion == source.occlusion && m.emission == source.emission && existing.push.flags[0] == flags &&
          existing.push.animation[2] == material.push.animation[2] &&
          existing.push.animation[3] == material.push.animation[3])
        return i;
    }
    UpdateDescriptor(material);
    materials.push_back(material);
    return static_cast<uint32_t>(materials.size() - 1);
  }
  bool IsBlended(uint32_t material) const {
    const auto &s = materials[material].source.style;
    return s.transparent || s.additive || s.water;
  }
  void LoadLevel(const ft_level &level) {
    if (loaded_level == &level) return;
    vkDeviceWaitIdle(device);
    Destroy(static_vertices);
    materials.clear();
    batches.clear();
    car_materials.clear();
    Check(vkResetDescriptorPool(device, descriptors, 0), "Reset material descriptors");
    std::vector<uint32_t> level_materials;
    for (const auto &material : level.materials)
      level_materials.push_back(AddMaterial(material));
    RenderMaterial fallback;
    fallback.style.unlit = true;
    const uint32_t fallback_id = AddMaterial(fallback);
    std::vector<Vertex> vertices;
    vertices.reserve((level.track.size() + level.backdrop.size() + level.translucent.size()) * 3);
    has_water = false;
    auto append = [&](const std::vector<Triangle> &triangles, bool backdrop) {
      // The CPU collision grid uses small cells. Reusing those cells for
      // Vulkan produced thousands of tiny draws and descriptor binds. Keep
      // opaque geometry in larger spatial groups, with smaller groups for
      // transparent surfaces whose draw order depends on the camera.
      const auto material_id = [&](const Triangle *tri) {
        return tri->material < level_materials.size() ? level_materials[tri->material] : fallback_id;
      };
      using Key = std::tuple<uint32_t, int, int>;
      std::vector<std::pair<Key, const Triangle *>> ordered;
      ordered.reserve(triangles.size());
      for (const auto &triangle : triangles) {
        const auto material = material_id(&triangle);
        const auto center = Scale(Add(Add(triangle.a, triangle.b), triangle.c), 1.f / 3);
        const float size = IsBlended(material) ? 32.f : 128.f;
        ordered.push_back({{material, backdrop ? 0 : int(std::floor(center.x / size)),
                            backdrop ? 0 : int(std::floor(center.z / size))},
                           &triangle});
      }
      std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
      Batch batch;
      bool have = false;
      Key previous{};
      for (const auto &entry : ordered) {
        const auto *tri = entry.second;
        const uint32_t material = std::get<0>(entry.first);
        if (!have || previous != entry.first) {
          if (have) batches.push_back(batch);
          batch = {};
          batch.first = static_cast<uint32_t>(vertices.size());
          batch.material = material;
          batch.backdrop = backdrop;
          have = true;
          previous = entry.first;
        }
        if (materials[material].source.style.water) {
          has_water = true;
          water_height = tri->a.y;
        }
        ft_vec3 positions[] = {tri->a, tri->b, tri->c};
        for (int i = 0; i < 3; ++i) {
          ft_vec3 normal = LengthSq(tri->normals[i]) > .1f ? tri->normals[i]
                                                           : Normalize(Cross(Sub(tri->b, tri->a), Sub(tri->c, tri->a)));
          vertices.push_back({positions[i], normal, tri->uv[i], tri->uv1[i],
                              tri->material == kNoTextureLayer ? tri->color : tri->colors[i]});
          batch.bounds.Add(positions[i]);
          ++batch.count;
        }
      }
      if (have) batches.push_back(batch);
    };
    append(level.backdrop, true);
    append(level.track, false);
    append(level.translucent, false);
    Buffer staging;
    try {
      MakeBuffer(staging, vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      std::memcpy(staging.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
      MakeBuffer(static_vertices, staging.size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 false);
      Begin();
      VkBufferCopy copy{0, 0, staging.size};
      vkCmdCopyBuffer(command, staging.buffer, static_vertices.buffer, 1, &copy);
      VkMemoryBarrier memory{};
      memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      memory.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &memory,
                           0, nullptr, 0, nullptr);
      Submit();
      Wait();
    } catch (...) {
      Destroy(staging);
      throw;
    }
    Destroy(staging);
    loaded_level = &level;
    shadow_ready = false;
    Log(game, FT_LOG_INFO, "TMNF Vulkan: retained %zu vertices in %zu material batches.", vertices.size(),
        batches.size());
  }
  void Cars() {
    dynamic_batches.clear();
    const size_t bytes = cars.size() * 3 * sizeof(Vertex);
    if (bytes > dynamic_vertices.size) {
      Destroy(dynamic_vertices);
      MakeBuffer(dynamic_vertices, bytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    }
    // Count and scatter into material ranges. Sorting 50,000 full triangles
    // moved megabytes of vertex data on every frame. Keep separate ranges for
    // each world so translucent ghost cars can be sorted independently.
    std::unordered_map<uint64_t, uint32_t> groups;
    std::vector<uint32_t> triangle_batches;
    triangle_batches.reserve(cars.size());
    for (const auto &tri : cars) {
      uint64_t key = tri.layer | (uint64_t(tri.blended) << 32) | (uint64_t(tri.material + 1u) << 33);
      auto entry = car_materials.find(key);
      if (entry == car_materials.end()) {
        RenderMaterial material;
        if (tri.material < game->vehicle.materials.size()) material = game->vehicle.materials[tri.material];
        else if (tri.layer == kNoTextureLayer) material.style.unlit = true;
        material.diffuse = tri.layer;
        material.style.transparent |= tri.blended;
        entry = car_materials.emplace(key, AddMaterial(material)).first;
      }
      const uint64_t group_key = entry->second | (uint64_t(tri.object) << 32);
      auto group = groups.emplace(group_key, static_cast<uint32_t>(dynamic_batches.size()));
      if (group.second) {
        Batch batch;
        batch.material = entry->second;
        batch.dynamic = true;
        dynamic_batches.push_back(batch);
      }
      const auto index = group.first->second;
      dynamic_batches[index].count += 3;
      triangle_batches.push_back(index);
    }
    std::vector<uint32_t> cursors;
    cursors.reserve(dynamic_batches.size());
    uint32_t first = 0;
    for (auto &batch : dynamic_batches) {
      batch.first = first;
      cursors.push_back(first);
      first += batch.count;
    }
    auto *vertices = static_cast<Vertex *>(dynamic_vertices.mapped);
    for (size_t i = 0; i < cars.size(); ++i) {
      const auto index = triangle_batches[i];
      auto &batch = dynamic_batches[index];
      for (const auto &vertex : cars[i].vertices) {
        vertices[cursors[index]++] = vertex;
        batch.bounds.Add(vertex.position);
      }
    }
  }
  void StartPass(VkRenderPass pass, VkFramebuffer framebuffer, uint32_t w, uint32_t h, bool load,
                 bool shadow_only = false) {
    VkClearValue clears[2]{};
    clears[0].color = {{.55f, .65f, .78f, 1}};
    clears[1].depthStencil.depth = 0;
    if (shadow_only) clears[0].depthStencil.depth = 1;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = {w, h};
    begin.clearValueCount = load ? 0 : shadow_only ? 1 : 2;
    begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    bound_pipeline = VK_NULL_HANDLE;
    bound_vertices = VK_NULL_HANDLE;
    bound_descriptor = VK_NULL_HANDLE;
    VkViewport viewport{0, 0, float(w), float(h), 0, 1};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
  }
  void Draw(const Batch &batch, uint32_t uniform_index, bool shadow_only = false) {
    auto &material = materials[batch.material];
    const bool cull = game->settings.backface_cull && !material.source.style.double_sided && !batch.backdrop;
    const auto pipeline = shadow_only                 ? shadow_pipeline
                          : uniform_index == 1        ? reflection_pipeline[cull]
                          : IsBlended(batch.material) ? blended[cull]
                                                      : opaque[cull];
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      bound_pipeline = pipeline;
    }
    VkBuffer buffer = batch.dynamic ? dynamic_vertices.buffer : static_vertices.buffer;
    VkDeviceSize offset = 0;
    if (buffer != bound_vertices) {
      vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
      bound_vertices = buffer;
    }
    const uint32_t uniform_offset = uniform_index * uniform_stride;
    if (material.descriptor != bound_descriptor) {
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &material.descriptor, 1,
                              &uniform_offset);
      bound_descriptor = material.descriptor;
    }
    Push push = material.push;
    if (shadow_only) push.flags[0] |= 256;
    vkCmdSetDepthBias(command,
                      shadow_only                      ? 2.f
                      : material.source.style.additive ? 4.f
                                                       : 0.f,
                      0,
                      shadow_only                      ? 2.f
                      : material.source.style.additive ? 1.f
                                                       : 0.f);
    push.tint[3] = batch.dynamic ? 1.f : frame.opacity;
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, batch.count, 1, batch.first, 0);
  }
  void DrawOpaque(uint32_t uniform_index, const Frustum &frustum) {
    for (const auto &batch : batches) {
      if (IsBlended(batch.material) || !(batch.backdrop ? game->settings.draw_background : game->settings.draw_track))
        continue;
      if (frustum.Valid() && !frustum.Intersects(batch.bounds)) continue;
      Draw(batch, uniform_index);
    }
    for (const auto &batch : dynamic_batches)
      if (!IsBlended(batch.material)) Draw(batch, uniform_index);
  }
  void Render() {
    const auto &camera = frame.state.camera;
    Targets(std::max(1u, uint32_t(camera.viewport.x)), std::max(1u, uint32_t(camera.viewport.y)));
    LoadLevel(*frame.level);
    Wait();
    Cars();
    Uniforms main;
    std::memcpy(main.vp, camera.view_proj, sizeof(main.vp));
    main.eye_time[0] = camera.eye.x;
    main.eye_time[1] = camera.eye.y;
    main.eye_time[2] = camera.eye.z;
    main.eye_time[3] = (float(frame.tick) + (frame.tick != 0 ? std::clamp(frame.alpha, 0.f, 1.f) - 1.f : 0.f)) * .01f;
    ft_vec3 light = Normalize(ft_vec3{-.4f, .7f, -.5f});
    main.sun_direction[0] = light.x;
    main.sun_direction[1] = light.y;
    main.sun_direction[2] = light.z;
    const auto sun = frame.level->sunlight, ambient = frame.level->ambient;
    main.sun_color[0] = sun.r;
    main.sun_color[1] = sun.g;
    main.sun_color[2] = sun.b;
    main.ambient[0] = ambient.r;
    main.ambient[1] = ambient.g;
    main.ambient[2] = ambient.b;
    main.fog[0] = .66f;
    main.fog[1] = .73f;
    main.fog[2] = .81f;
    main.fog[3] = .00016f;
    main.clip[3] = 1;
    main.viewport[0] = float(width);
    main.viewport[1] = float(height);
    main.viewport[2] = frame.state.lod_bias;
    ShadowMatrix(*frame.level, light, main.shadow);
    float mirror[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 2 * water_height, 0, 1};
    Multiply(main.vp, mirror, main.reflection);
    Uniforms reflected = main;
    std::memcpy(reflected.vp, main.reflection, sizeof(main.vp));
    reflected.eye_time[1] = 2 * water_height - camera.eye.y;
    reflected.clip[1] = 1;
    reflected.clip[3] = -water_height + .03f;
    Uniforms shadow_frame = main;
    std::memcpy(shadow_frame.vp, main.shadow, sizeof(main.vp));
    std::memcpy(uniforms.mapped, &main, sizeof(main));
    std::memcpy(static_cast<char *>(uniforms.mapped) + uniform_stride, &reflected, sizeof(reflected));
    std::memcpy(static_cast<char *>(uniforms.mapped) + 2 * uniform_stride, &shadow_frame, sizeof(shadow_frame));
    Begin();
    const uint32_t settings = uint32_t(game->settings.draw_track) | (uint32_t(game->settings.draw_background) << 1);
    if (settings != shadow_settings) {
      shadow_ready = false;
      shadow_settings = settings;
    }
    if (!shadow_ready) {
      StartPass(shadow_pass, static_shadow_framebuffer, 2048, 2048, false, true);
      for (const auto &batch : batches)
        if (!batch.backdrop && game->settings.draw_track && !IsBlended(batch.material)) Draw(batch, 2, true);
      vkCmdEndRenderPass(command);
      shadow_ready = true;
      Barrier(static_shadow.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    // Reuse the static track shadow, then add cars at this frame's poses.
    Barrier(shadow.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageCopy shadow_copy{};
    shadow_copy.srcSubresource = shadow_copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    shadow_copy.extent = {2048, 2048, 1};
    vkCmdCopyImage(command, static_shadow.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shadow.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &shadow_copy);
    Barrier(shadow.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    StartPass(shadow_load_pass, shadow_framebuffer, 2048, 2048, true, true);
    for (const auto &batch : dynamic_batches)
      if (!IsBlended(batch.material)) Draw(batch, 2, true);
    vkCmdEndRenderPass(command);
    // Initialize both sampled render targets before any material is bound.
    Barrier(refraction.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    StartPass(clear_pass, reflection_framebuffer, reflection.width, reflection.height, false);
    if (has_water) DrawOpaque(1, Frustum::FromViewProj(main.reflection));
    vkCmdEndRenderPass(command);
    Barrier(reflection.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    StartPass(clear_pass, main_framebuffer, width, height, false);
    const Frustum frustum = Frustum::FromViewProj(main.vp);
    DrawOpaque(0, frustum);
    vkCmdEndRenderPass(command);
    VkImage output = scene_image.image;
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {width, height, 1};
    if (has_water) {
      Barrier(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_ASPECT_COLOR_BIT);
      Barrier(refraction.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_ASPECT_COLOR_BIT);
      vkCmdCopyImage(command, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, refraction.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      Barrier(refraction.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_ASPECT_COLOR_BIT);
      Barrier(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_ASPECT_COLOR_BIT);
    }
    StartPass(load_pass, main_framebuffer, width, height, true);
    std::vector<const Batch *> transparent;
    for (const auto &batch : batches)
      if (IsBlended(batch.material) && (batch.backdrop ? game->settings.draw_background : game->settings.draw_track) &&
          (!frustum.Valid() || frustum.Intersects(batch.bounds)))
        transparent.push_back(&batch);
    for (const auto &batch : dynamic_batches)
      if (IsBlended(batch.material)) transparent.push_back(&batch);
    std::stable_sort(transparent.begin(), transparent.end(),
                     [&](auto a, auto b) { return Dot(Center(*a), camera.forward) > Dot(Center(*b), camera.forward); });
    for (auto *batch : transparent)
      Draw(*batch, 0);
    vkCmdEndRenderPass(command);
    const auto engine_output = reinterpret_cast<VkImage>(target_image.image);
    Barrier(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(engine_output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImage(command, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, engine_output,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // An image first registered during this callback missed the host's opening
    // barrier; make it sampleable here. Subsequent frames use the ABI contract.
    Barrier(engine_output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            target_created ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    target_created = false;
    Submit();
    const float presentation[4] = {float(width) / target_image.width, float(height) / target_image.height, 0, 0};
    api->draw_mesh(present, -1000, quad, &target, 1, presentation, sizeof(presentation));
  }
};

namespace {
thread_local GpuRenderer *capture;
void CaptureTriangle(ft_vec3 a, ft_vec3 b, ft_vec3 c, ft_vec2 ua, ft_vec2 ub, ft_vec2 uc, uint32_t layer,
                     ft_color color) {
  if (!capture) return;
  ft_vec3 normal = Normalize(Cross(Sub(b, a), Sub(c, a)));
  uint32_t packed = PackColor(color);
  capture->cars.push_back({{{a, normal, ua, {}, packed}, {b, normal, ub, {}, packed}, {c, normal, uc, {}, packed}},
                           layer,
                           color.a < .999f,
                           kNoTextureLayer,
                           capture->capture_object});
}
void CapturePlain(ft_vec3 a, ft_vec3 b, ft_vec3 c, ft_color color) {
  CaptureTriangle(a, b, c, {}, {}, {}, kNoTextureLayer, color);
}
} // namespace

bool GpuCaptureVehicle(std::uint32_t material, std::uint32_t layer, const ft_vec3 *positions, const ft_vec3 *normals,
                       const ft_vec2 *uv, ft_color color) {
  if (!capture) return false;
  CarTriangle triangle;
  triangle.material = material;
  triangle.layer = layer;
  triangle.blended = color.a < .999f;
  triangle.object = capture->capture_object;
  const auto packed = PackColor(color);
  for (int i = 0; i < 3; ++i)
    triangle.vertices[i] = {positions[i], normals[i], uv[i], uv[i], packed};
  capture->cars.push_back(triangle);
  return true;
}

bool GpuUploadTextures(ft_game *game) {
  const auto *api = game->engine;
  if (game->headless || !api->gpu_device || !api->texture_gpu_image || !api->pipeline_create || !api->mesh_create)
    return false;
  const auto *gpu = api->gpu_device();
  if (!gpu || gpu->api != FT_GPU_API_VULKAN) return false;
  if (game->renderer && game->renderer->failed) return false;
  try {
    if (!game->renderer) {
      auto renderer = std::make_unique<GpuRenderer>(game);
      renderer->Initialize(*gpu);
      game->renderer = renderer.release();
    }
    auto &renderer = *game->renderer;
    const auto &pages = game->textures.Pages();
    for (size_t i = renderer.textures.size(); i < pages.size(); ++i) {
      renderer.textures.emplace_back();
      renderer.UploadImage(renderer.textures.back(), pages[i]);
    }
    return true;
  } catch (const std::exception &e) {
    Log(game, FT_LOG_ERROR, "TMNF Vulkan initialization: %s", e.what());
    // The host may already have recorded barriers for presentation images.
    // Keep those alive until shutdown even when switching to the fallback.
    if (game->renderer) game->renderer->failed = true;
    return false;
  }
}
bool GpuRender(ft_game *game, const ft_render_frame *frame) {
  if (!game->renderer || game->renderer->failed || !frame->level) return false;
  auto &renderer = *game->renderer;
  try {
    if (frame->pass == FT_PASS_LEVEL_BACKGROUND) {
      renderer.frame = *frame;
      renderer.cars.clear();
      renderer.collecting = true;
      renderer.capture_object = 0;
      const auto *api = game->engine;
      if (api->timeline_world_count && api->timeline_world_info && api->timeline_player_track) {
        for (uint32_t i = 0; i < api->timeline_world_count(); ++i) {
          ft_timeline_world_info info{};
          info.struct_size = sizeof(info);
          if (api->timeline_world_info(i, &info))
            for (uint32_t j = 0; j < info.player_count; ++j)
              SkinLayerFor(game, api->timeline_player_track(i, j));
        }
      }
      if (game->textures.NeedsUpload()) game->textures.Upload(game);
      if (renderer.failed) return false;
    } else if (frame->pass == FT_PASS_ENTITIES && renderer.collecting && frame->world) {
      ++renderer.capture_object;
      ft_engine_api redirected = *game->engine;
      redirected.draw_triangle3 = CapturePlain;
      redirected.draw_triangle3_textured = CaptureTriangle;
      const ft_engine_api *original = game->engine;
      game->engine = &redirected;
      capture = &renderer;
      try {
        RenderCar(game, frame);
      } catch (...) {
        game->engine = original;
        capture = nullptr;
        throw;
      }
      game->engine = original;
      capture = nullptr;
    } else if (frame->pass == FT_PASS_LEVEL_FOREGROUND && renderer.collecting) {
      renderer.Render();
      renderer.collecting = false;
    }
    return true;
  } catch (const std::exception &e) {
    Log(game, FT_LOG_ERROR, "TMNF Vulkan render: %s; switching to the fallback renderer.", e.what());
    renderer.failed = true;
    renderer.collecting = false;
    game->textures.Destroy(game);
    return false;
  }
}
void GpuResetLevel(ft_game *game, bool clear_textures) {
  if (!game || !game->renderer) return;
  auto &renderer = *game->renderer;
  renderer.Wait();
  renderer.loaded_level = nullptr;
  renderer.shadow_ready = false;
  renderer.collecting = false;
  if (clear_textures) {
    renderer.Destroy(renderer.static_vertices);
    renderer.batches.clear();
    renderer.materials.clear();
    renderer.car_materials.clear();
    renderer.cars.clear();
    vkResetDescriptorPool(renderer.device, renderer.descriptors, 0);
    for (auto &texture : renderer.textures)
      renderer.Destroy(texture);
    renderer.textures.clear();
  }
}
void GpuDestroy(ft_game *game) {
  if (!game) return;
  delete game->renderer;
  game->renderer = nullptr;
}
} // namespace tmnf
