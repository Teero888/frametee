#include "sm64_vulkan.h"
#include "fast3d_spv.h"

#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

template <typename T>
inline T vk_init(VkStructureType sType) {
  T s{};
  s.sType = sType;
  return s;
}

struct GpuVertex {
  float pos[4];
  float uv[2];
  float fog[4];
  float inputs[4][4];
};

struct GpuTexture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  int width = 0;
  int height = 0;
  int sampler_idx = 0;
  bool valid = false;
  std::vector<uint8_t> pixels;
};

struct Shader {
  uint32_t id = 0;
  uint8_t input_count = 0;
  bool used_textures[2] = {false, false};
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct RenderApi {
  bool (*z_is_from_0_to_1)();
  void (*unload_shader)(void *);
  void (*load_shader)(void *);
  void *(*create_and_load_new_shader)(uint32_t);
  void *(*lookup_shader)(uint32_t);
  void (*shader_get_info)(void *, uint8_t *, bool *);
  uint32_t (*new_texture)();
  void (*select_texture)(int, uint32_t);
  void (*upload_texture)(const uint8_t *, int, int);
  void (*set_sampler_parameters)(int, bool, uint32_t, uint32_t);
  void (*set_depth_test)(bool);
  void (*set_depth_mask)(bool);
  void (*set_zmode_decal)(bool);
  void (*set_viewport)(int, int, int, int);
  void (*set_scissor)(int, int, int, int);
  void (*set_use_alpha)(bool);
  void (*draw_triangles)(float *, size_t, size_t);
  void (*init)();
  void (*on_resize)();
  void (*start_frame)();
  void (*end_frame)();
  void (*finish_render)();
  void (*shutdown)();
};

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties mem_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);
  for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
    if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return 0;
}

VkFormat find_depth_format(VkPhysicalDevice physical_device) {
  VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
  for (VkFormat format : candidates) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &props);
    if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      return format;
    }
  }
  return VK_FORMAT_D32_SFLOAT;
}

int get_sampler_index(bool linear, uint32_t cms, uint32_t cmt) {
  int l = linear ? 1 : 0;
  int s = (cms & 2) ? 0 : ((cms & 1) ? 1 : 2);
  int t = (cmt & 2) ? 0 : ((cmt & 1) ? 1 : 2);
  return l * 9 + s * 3 + t;
}

void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkImageAspectFlags aspect) {
  VkImageMemoryBarrier barrier = vk_init<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

  if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  } else {
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  }

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

struct sm64_vulkan {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;

  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;

  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

  VkRenderPass render_pass_clear = VK_NULL_HANDLE;
  VkRenderPass render_pass_load = VK_NULL_HANDLE;

  VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

  VkShaderModule vs_module = VK_NULL_HANDLE;
  VkShaderModule fs_module = VK_NULL_HANDLE;

  // 8 pipeline variants: (depth_test ? 1 : 0) | (depth_write ? 2 : 0) | (use_alpha ? 4 : 0)
  std::array<VkPipeline, 8> pipelines{};

  // 18 samplers: (linear ? 1 : 0) * 9 + cms * 3 + cmt
  std::array<VkSampler, 18> samplers{};

  // Dynamic vertex buffer (host visible, coherent)
  VkBuffer vertex_buffer = VK_NULL_HANDLE;
  VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
  uint8_t *vertex_mapped = nullptr;
  size_t vertex_buffer_size = 8 * 1024 * 1024;
  size_t vertex_offset = 0;

  // Staging buffer for texture uploads
  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  uint8_t *staging_mapped = nullptr;
  size_t staging_size = 4 * 1024 * 1024;
  size_t staging_offset = 0;

  // Depth buffer
  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;

  // Target framebuffer
  VkImage target_image = VK_NULL_HANDLE;
  VkImageView target_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  uint32_t fb_width = 0;
  uint32_t fb_height = 0;

  // Dummy 1x1 white texture
  GpuTexture dummy_texture;

  // Texture cache
  std::unordered_map<uint32_t, GpuTexture> textures;
  // Resized texture slots may still be referenced by commands in this frame.
  std::vector<GpuTexture> retired_textures;
  uint32_t next_texture_id = 1;
  uint32_t selected_textures[2]{0, 0};
  uint32_t upload_texture_id = 0;

  // Shader cache
  std::unordered_map<uint32_t, std::unique_ptr<Shader>> shaders;
  Shader *current_shader = nullptr;

  // Descriptor set cache
  std::unordered_map<uint64_t, VkDescriptorSet> desc_set_cache;

  // Render state
  bool render_pass_active = false;
  bool first_render_pass = true;
  bool depth_test = true;
  bool depth_write = true;
  bool use_alpha = false;
  bool zmode_decal = false;
  bool editor_camera = false;
  Rect viewport{0, 0, 0, 0};
  Rect scissor{0, 0, 0, 0};

  static RenderApi make_api();
};

static thread_local sm64_vulkan *active_vulkan = nullptr;

static bool z_is_from_0_to_1() { return true; }
static void unload_shader(void *) { if (active_vulkan) active_vulkan->current_shader = nullptr; }
static void load_shader(void *s) { if (active_vulkan) active_vulkan->current_shader = static_cast<Shader *>(s); }

static void *create_and_load_new_shader(uint32_t id) {
  if (!active_vulkan) return nullptr;
  auto &sh = active_vulkan->shaders[id];
  if (!sh) {
    sh = std::make_unique<Shader>();
    sh->id = id;
    for (int cycle = 0; cycle < 2; ++cycle) {
      for (int part = 0; part < 4; ++part) {
        const uint8_t item = uint8_t(id >> (cycle * 12 + part * 3)) & 7;
        if (item >= 1 && item <= 4) sh->input_count = std::max(sh->input_count, item);
        if (item == 5 || item == 6) sh->used_textures[0] = true;
        if (item == 7) sh->used_textures[1] = true;
      }
    }
  }
  active_vulkan->current_shader = sh.get();
  return sh.get();
}

static void *lookup_shader(uint32_t id) {
  if (!active_vulkan) return nullptr;
  auto it = active_vulkan->shaders.find(id);
  if (it != active_vulkan->shaders.end()) {
    return it->second.get();
  }
  return nullptr;
}

static void shader_get_info(void *s, uint8_t *inputs, bool *textures) {
  if (!s) return;
  auto *sh = static_cast<Shader *>(s);
  *inputs = sh->input_count;
  textures[0] = sh->used_textures[0];
  textures[1] = sh->used_textures[1];
}

static uint32_t new_texture() {
  return active_vulkan ? active_vulkan->next_texture_id++ : 1;
}

static void select_texture(int tile, uint32_t id) {
  if (!active_vulkan || tile < 0 || tile >= 2) return;
  active_vulkan->selected_textures[tile] = id;
  active_vulkan->upload_texture_id = id;
}

static void upload_texture(const uint8_t *pixels, int width, int height) {
  if (!active_vulkan || !pixels || width <= 0 || height <= 0) return;
  auto &vk = *active_vulkan;
  if (vk.upload_texture_id == 0) return;
  const size_t byte_size = static_cast<size_t>(width) * height * 4;
  GpuTexture &tex = vk.textures[vk.upload_texture_id];
  if (tex.valid && tex.width == width && tex.height == height && tex.pixels.size() == byte_size &&
      std::memcmp(tex.pixels.data(), pixels, byte_size) == 0) return;
  if (byte_size > vk.staging_size - vk.staging_offset)
    throw std::runtime_error("SM64 texture uploads exceed the GPU staging buffer");
  vk.desc_set_cache.clear();

  if (vk.render_pass_active) {
    vkCmdEndRenderPass(vk.cmd);
    vk.render_pass_active = false;
  }

  if (tex.image != VK_NULL_HANDLE && (tex.width != width || tex.height != height)) {
    vk.retired_textures.push_back(std::move(tex));
    tex = {};
  }

  if (tex.image == VK_NULL_HANDLE) {
    tex.width = width;
    tex.height = height;

    VkImageCreateInfo ici = vk_init<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(vk.device, &ici, nullptr, &tex.image);

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(vk.device, tex.image, &reqs);
    VkMemoryAllocateInfo mai = vk_init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = find_memory_type(vk.physical_device, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(vk.device, &mai, nullptr, &tex.memory);
    vkBindImageMemory(vk.device, tex.image, tex.memory, 0);

    VkImageViewCreateInfo ivci = vk_init<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    ivci.image = tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(vk.device, &ivci, nullptr, &tex.view);
  }

  std::memcpy(vk.staging_mapped + vk.staging_offset, pixels, byte_size);

  transition_image_layout(vk.cmd, tex.image, tex.valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

  VkBufferImageCopy region{};
  region.bufferOffset = vk.staging_offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  vkCmdCopyBufferToImage(vk.cmd, vk.staging_buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  transition_image_layout(vk.cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

  vk.staging_offset = (vk.staging_offset + byte_size + 255) & ~255;
  tex.valid = true;
  tex.pixels.assign(pixels, pixels + byte_size);
}

static void set_sampler_parameters(int tile, bool linear, uint32_t cms, uint32_t cmt) {
  if (!active_vulkan || tile < 0 || tile >= 2) return;
  auto &vk = *active_vulkan;
  int s_idx = get_sampler_index(linear, cms, cmt);
  uint32_t tex_id = vk.selected_textures[tile];
  if (tex_id && vk.textures.count(tex_id)) {
    vk.textures[tex_id].sampler_idx = s_idx;
  }
}

static void set_depth_test(bool v) { if (active_vulkan) active_vulkan->depth_test = v; }
static void set_depth_mask(bool v) { if (active_vulkan) active_vulkan->depth_write = v; }
static void set_use_alpha(bool v) { if (active_vulkan) active_vulkan->use_alpha = v; }

static void set_zmode_decal(bool v) {
  if (!active_vulkan) return;
  active_vulkan->zmode_decal = v;
  if (active_vulkan->render_pass_active && active_vulkan->cmd) {
    vkCmdSetDepthBias(active_vulkan->cmd, v ? -2.0f : 0.0f, 0.0f, v ? -2.0f : 0.0f);
  }
}

static void set_viewport(int x, int y, int w, int h) {
  if (!active_vulkan) return;
  const int fb_h = int(active_vulkan->fb_height);
  int top = fb_h - y - h;
  if (top < 0) top = 0;
  active_vulkan->viewport = {x, top, w, h};
  if (active_vulkan->render_pass_active && active_vulkan->cmd) {
    VkViewport vp{static_cast<float>(x), static_cast<float>(top), static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    vkCmdSetViewport(active_vulkan->cmd, 0, 1, &vp);
  }
}

static void set_scissor(int x, int y, int w, int h) {
  if (!active_vulkan) return;
  const int fb_w = int(active_vulkan->fb_width);
  const int fb_h = int(active_vulkan->fb_height);
  int top = fb_h - y - h;
  int left = x;
  int width = w;
  int height = h;
  if (top < 0) {
    height += top;
    top = 0;
  }
  if (left < 0) {
    width += left;
    left = 0;
  }
  if (left + width > fb_w) width = std::max(0, fb_w - left);
  if (top + height > fb_h) height = std::max(0, fb_h - top);
  if (width < 0) width = 0;
  if (height < 0) height = 0;
  active_vulkan->scissor = {left, top, width, height};
  if (active_vulkan->render_pass_active && active_vulkan->cmd) {
    VkRect2D sc{{static_cast<int32_t>(left), static_cast<int32_t>(top)},
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    vkCmdSetScissor(active_vulkan->cmd, 0, 1, &sc);
  }
}

static void draw_triangles(float *buf, size_t buf_len, size_t num_tris) {
  if (!active_vulkan || !buf || num_tris == 0) return;
  auto &vk = *active_vulkan;

  if (!vk.render_pass_active) {
    VkRenderPassBeginInfo rp_info = vk_init<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rp_info.renderPass = vk.first_render_pass ? vk.render_pass_clear : vk.render_pass_load;
    rp_info.framebuffer = vk.framebuffer;
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = {vk.fb_width, vk.fb_height};

    VkClearValue clear_vals[2]{};
    clear_vals[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clear_vals[1].depthStencil = {1.0f, 0};
    rp_info.clearValueCount = 2;
    rp_info.pClearValues = clear_vals;

    vkCmdBeginRenderPass(vk.cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    vk.first_render_pass = false;
    vk.render_pass_active = true;

    VkViewport vp{static_cast<float>(vk.viewport.x), static_cast<float>(vk.viewport.y),
                  static_cast<float>(vk.viewport.w), static_cast<float>(vk.viewport.h), 0.0f, 1.0f};
    vkCmdSetViewport(vk.cmd, 0, 1, &vp);

    VkRect2D sc{{static_cast<int32_t>(vk.scissor.x), static_cast<int32_t>(vk.scissor.y)},
                {static_cast<uint32_t>(vk.scissor.w), static_cast<uint32_t>(vk.scissor.h)}};
    vkCmdSetScissor(vk.cmd, 0, 1, &sc);

    vkCmdSetDepthBias(vk.cmd, vk.zmode_decal ? -2.0f : 0.0f, 0.0f, vk.zmode_decal ? -2.0f : 0.0f);
  }

  int pipe_idx = (vk.depth_test ? 1 : 0) | (vk.depth_write ? 2 : 0) | (vk.use_alpha ? 4 : 0);
  vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipelines[pipe_idx]);

  // Resolve textures
  VkImageView v0 = vk.dummy_texture.view, v1 = vk.dummy_texture.view;
  VkSampler s0 = vk.samplers[0], s1 = vk.samplers[0];
  uint32_t id0 = vk.selected_textures[0], id1 = vk.selected_textures[1];
  int s0_idx = 0, s1_idx = 0;

  if (id0 && vk.textures.count(id0) && vk.textures[id0].valid) {
    v0 = vk.textures[id0].view;
    s0_idx = vk.textures[id0].sampler_idx;
    s0 = vk.samplers[s0_idx];
  }
  if (id1 && vk.textures.count(id1) && vk.textures[id1].valid) {
    v1 = vk.textures[id1].view;
    s1_idx = vk.textures[id1].sampler_idx;
    s1 = vk.samplers[s1_idx];
  }

  uint64_t ds_key = (static_cast<uint64_t>(id0) & 0xFFFFull) |
                    ((static_cast<uint64_t>(s0_idx) & 0xFFull) << 16) |
                    ((static_cast<uint64_t>(id1) & 0xFFFFull) << 24) |
                    ((static_cast<uint64_t>(s1_idx) & 0xFFull) << 40);

  VkDescriptorSet ds = VK_NULL_HANDLE;
  auto ds_it = vk.desc_set_cache.find(ds_key);
  if (ds_it != vk.desc_set_cache.end()) {
    ds = ds_it->second;
  } else {
    VkDescriptorSetAllocateInfo ds_ai = vk_init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ds_ai.descriptorPool = vk.desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &vk.desc_layout;
    if (vkAllocateDescriptorSets(vk.device, &ds_ai, &ds) == VK_SUCCESS) {
      VkDescriptorImageInfo di0{s0, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo di1{s1, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

      VkWriteDescriptorSet writes[2]{};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = ds;
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[0].pImageInfo = &di0;

      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = ds;
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[1].pImageInfo = &di1;

      vkUpdateDescriptorSets(vk.device, 2, writes, 0, nullptr);
      vk.desc_set_cache[ds_key] = ds;
    }
  }

  if (ds != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, 0, 1, &ds, 0, nullptr);
  }

  uint32_t shader_id = vk.current_shader ? vk.current_shader->id : 0;
  vkCmdPushConstants(vk.cmd, vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(uint32_t), &shader_id);

  const size_t num_vertices = num_tris * 3;
  const size_t stride = buf_len / num_vertices;
  const size_t alloc_bytes = num_vertices * sizeof(GpuVertex);

  if (vk.vertex_offset + alloc_bytes > vk.vertex_buffer_size) {
    throw std::runtime_error("SM64 geometry exceeds the GPU vertex buffer");
  }

  GpuVertex *dst = reinterpret_cast<GpuVertex *>(vk.vertex_mapped + vk.vertex_offset);
  const bool used_textures = vk.current_shader && (vk.current_shader->used_textures[0] || vk.current_shader->used_textures[1]);
  const bool opt_fog = (shader_id & (1u << 25)) != 0;
  const bool opt_alpha = (shader_id & (1u << 24)) != 0;
  const uint8_t input_count = vk.current_shader ? vk.current_shader->input_count : 0;

  for (size_t v = 0; v < num_vertices; ++v) {
    const float *src = buf + v * stride;
    GpuVertex &out = dst[v];
    out.pos[0] = src[0];
    out.pos[1] = src[1];
    out.pos[2] = src[2];
    out.pos[3] = src[3];

    size_t cur = 4;
    if (used_textures) {
      out.uv[0] = src[cur];
      out.uv[1] = src[cur + 1];
      cur += 2;
    } else {
      out.uv[0] = 0.0f;
      out.uv[1] = 0.0f;
    }

    if (opt_fog) {
      out.fog[0] = src[cur];
      out.fog[1] = src[cur + 1];
      out.fog[2] = src[cur + 2];
      out.fog[3] = src[cur + 3];
      cur += 4;
    } else {
      out.fog[0] = out.fog[1] = out.fog[2] = out.fog[3] = 0.0f;
    }

    for (int i = 0; i < 4; ++i) {
      if (i < input_count) {
        out.inputs[i][0] = src[cur];
        out.inputs[i][1] = src[cur + 1];
        out.inputs[i][2] = src[cur + 2];
        if (opt_alpha) {
          out.inputs[i][3] = src[cur + 3];
          cur += 4;
        } else {
          out.inputs[i][3] = 1.0f;
          cur += 3;
        }
      } else {
        out.inputs[i][0] = out.inputs[i][1] = out.inputs[i][2] = 0.0f;
        out.inputs[i][3] = 1.0f;
      }
    }
  }

  VkDeviceSize vtx_offset = vk.vertex_offset;
  vkCmdBindVertexBuffers(vk.cmd, 0, 1, &vk.vertex_buffer, &vtx_offset);
  vkCmdDraw(vk.cmd, static_cast<uint32_t>(num_vertices), 1, 0, 0);

  vk.vertex_offset = (vk.vertex_offset + alloc_bytes + 255) & ~255;
}

static void gfx_init() {}
static void on_resize() {}
static void start_frame() {
  if (!active_vulkan) return;
  active_vulkan->first_render_pass = true;
  active_vulkan->render_pass_active = false;
  active_vulkan->vertex_offset = 0;
  active_vulkan->staging_offset = 0;
}
static void end_frame() {
  if (!active_vulkan) return;
  auto &vk = *active_vulkan;
  if (vk.render_pass_active) {
    vkCmdEndRenderPass(vk.cmd);
    vk.render_pass_active = false;
  } else if (vk.first_render_pass) {
    VkRenderPassBeginInfo rp_info = vk_init<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rp_info.renderPass = vk.render_pass_clear;
    rp_info.framebuffer = vk.framebuffer;
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = {vk.fb_width, vk.fb_height};
    VkClearValue clear_vals[2]{};
    clear_vals[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clear_vals[1].depthStencil = {1.0f, 0};
    rp_info.clearValueCount = 2;
    rp_info.pClearValues = clear_vals;
    vkCmdBeginRenderPass(vk.cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(vk.cmd);
    vk.first_render_pass = false;
  }
}
static void finish_render() {}
static void shutdown() {}

RenderApi sm64_vulkan::make_api() {
  RenderApi api{};
  api.z_is_from_0_to_1 = z_is_from_0_to_1;
  api.unload_shader = unload_shader;
  api.load_shader = load_shader;
  api.create_and_load_new_shader = create_and_load_new_shader;
  api.lookup_shader = lookup_shader;
  api.shader_get_info = shader_get_info;
  api.new_texture = new_texture;
  api.select_texture = select_texture;
  api.upload_texture = upload_texture;
  api.set_sampler_parameters = set_sampler_parameters;
  api.set_depth_test = set_depth_test;
  api.set_depth_mask = set_depth_mask;
  api.set_zmode_decal = set_zmode_decal;
  api.set_viewport = set_viewport;
  api.set_scissor = set_scissor;
  api.set_use_alpha = set_use_alpha;
  api.draw_triangles = draw_triangles;
  api.init = gfx_init;
  api.on_resize = on_resize;
  api.start_frame = start_frame;
  api.end_frame = end_frame;
  api.finish_render = finish_render;
  api.shutdown = shutdown;
  return api;
}

sm64_vulkan *sm64_vulkan_create(const ft_gpu_device *gpu, char *error, size_t error_size) {
  if (!gpu || gpu->api != FT_GPU_API_VULKAN) {
    if (error && error_size) std::snprintf(error, error_size, "FrameTee GPU device is not Vulkan");
    return nullptr;
  }

  auto vk = std::make_unique<sm64_vulkan>();
  vk->instance = static_cast<VkInstance>(gpu->instance);
  vk->physical_device = static_cast<VkPhysicalDevice>(gpu->physical_device);
  vk->device = static_cast<VkDevice>(gpu->device);
  vk->queue = static_cast<VkQueue>(gpu->queue);
  vk->queue_family = gpu->queue_family_index;
  vk->depth_format = find_depth_format(vk->physical_device);

  // Command pool and buffer
  VkCommandPoolCreateInfo cpci = vk_init<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  cpci.queueFamilyIndex = vk->queue_family;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(vk->device, &cpci, nullptr, &vk->command_pool) != VK_SUCCESS) {
    if (error && error_size) std::snprintf(error, error_size, "Cannot create Vulkan command pool");
    return nullptr;
  }

  VkCommandBufferAllocateInfo cbai = vk_init<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  cbai.commandPool = vk->command_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  vkAllocateCommandBuffers(vk->device, &cbai, &vk->cmd);

  VkFenceCreateInfo fci = vk_init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  vkCreateFence(vk->device, &fci, nullptr, &vk->fence);

  // Render passes (clear and load)
  for (int pass = 0; pass < 2; ++pass) {
    VkAttachmentDescription attachments[2]{};
    // Color attachment
    attachments[0].format = vk->color_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = pass == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth attachment
    attachments[1].format = vk->depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = pass == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = pass == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo rpci = vk_init<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    // Texture uploads split a frame into several render passes. LOAD must
    // observe the preceding pass's color/depth writes even when no attachment
    // layout changes (there is no implicit dependency for that case).
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dependency;

    if (pass == 0) {
      vkCreateRenderPass(vk->device, &rpci, nullptr, &vk->render_pass_clear);
    } else {
      vkCreateRenderPass(vk->device, &rpci, nullptr, &vk->render_pass_load);
    }
  }

  // Descriptor Set Layout
  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo dslci = vk_init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  dslci.bindingCount = 2;
  dslci.pBindings = bindings;
  vkCreateDescriptorSetLayout(vk->device, &dslci, nullptr, &vk->desc_layout);

  // Descriptor Pool
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
  VkDescriptorPoolCreateInfo dpci = vk_init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpci.maxSets = 2048;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  vkCreateDescriptorPool(vk->device, &dpci, nullptr, &vk->desc_pool);

  // Pipeline Layout
  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(uint32_t);

  VkPipelineLayoutCreateInfo plci = vk_init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &vk->desc_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  vkCreatePipelineLayout(vk->device, &plci, nullptr, &vk->pipeline_layout);

  // Shaders
  VkShaderModuleCreateInfo smci_v = vk_init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  smci_v.codeSize = sizeof(kFast3dVertSpv);
  smci_v.pCode = kFast3dVertSpv;
  vkCreateShaderModule(vk->device, &smci_v, nullptr, &vk->vs_module);

  VkShaderModuleCreateInfo smci_f = vk_init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  smci_f.codeSize = sizeof(kFast3dFragSpv);
  smci_f.pCode = kFast3dFragSpv;
  vkCreateShaderModule(vk->device, &smci_f, nullptr, &vk->fs_module);

  // Pipelines
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vk->vs_module;
  stages[0].pName = "main";

  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = vk->fs_module;
  stages[1].pName = "main";

  VkVertexInputBindingDescription vibd{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription viad[7]{};
  viad[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, pos)};
  viad[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)};
  viad[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, fog)};
  viad[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, inputs[0])};
  viad[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, inputs[1])};
  viad[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, inputs[2])};
  viad[6] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, inputs[3])};

  VkPipelineVertexInputStateCreateInfo pvisi = vk_init<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  pvisi.vertexBindingDescriptionCount = 1;
  pvisi.pVertexBindingDescriptions = &vibd;
  pvisi.vertexAttributeDescriptionCount = 7;
  pvisi.pVertexAttributeDescriptions = viad;

  VkPipelineInputAssemblyStateCreateInfo piasi = vk_init<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  piasi.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo pvsi = vk_init<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  pvsi.viewportCount = 1;
  pvsi.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo prsi = vk_init<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  prsi.polygonMode = VK_POLYGON_MODE_FILL;
  prsi.cullMode = VK_CULL_MODE_NONE;
  prsi.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  prsi.depthBiasEnable = VK_TRUE;
  prsi.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo pmsi = vk_init<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  pmsi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo pdsi = vk_init<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
  pdsi.dynamicStateCount = 3;
  pdsi.pDynamicStates = dynamic_states;

  for (int i = 0; i < 8; ++i) {
    const bool depth_test = (i & 1) != 0;
    const bool depth_write = (i & 2) != 0;
    const bool alpha_blend = (i & 4) != 0;

    VkPipelineDepthStencilStateCreateInfo pdssi = vk_init<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    pdssi.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    pdssi.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    pdssi.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbas{};
    cbas.colorWriteMask = alpha_blend ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT) : 0xF;
    cbas.blendEnable = alpha_blend ? VK_TRUE : VK_FALSE;
    if (alpha_blend) {
      cbas.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      cbas.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      cbas.colorBlendOp = VK_BLEND_OP_ADD;
      cbas.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      cbas.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      cbas.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo pcbsi = vk_init<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    pcbsi.attachmentCount = 1;
    pcbsi.pAttachments = &cbas;

    VkGraphicsPipelineCreateInfo gpci = vk_init<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &pvisi;
    gpci.pInputAssemblyState = &piasi;
    gpci.pViewportState = &pvsi;
    gpci.pRasterizationState = &prsi;
    gpci.pMultisampleState = &pmsi;
    gpci.pDepthStencilState = &pdssi;
    gpci.pColorBlendState = &pcbsi;
    gpci.pDynamicState = &pdsi;
    gpci.layout = vk->pipeline_layout;
    gpci.renderPass = vk->render_pass_clear;
    gpci.subpass = 0;

    vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &vk->pipelines[i]);
  }

  // Pre-create 18 samplers
  for (int linear = 0; linear < 2; ++linear) {
    for (int cms = 0; cms < 3; ++cms) {
      for (int cmt = 0; cmt < 3; ++cmt) {
        VkSamplerCreateInfo sci = vk_init<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

        auto to_address_mode = [](int cm) {
          if (cm == 0) return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
          if (cm == 1) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        sci.addressModeU = to_address_mode(cms);
        sci.addressModeV = to_address_mode(cmt);
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = 0.0f;

        int idx = linear * 9 + cms * 3 + cmt;
        vkCreateSampler(vk->device, &sci, nullptr, &vk->samplers[idx]);
      }
    }
  }

  // Dynamic vertex buffer
  VkBufferCreateInfo bci = vk_init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  bci.size = vk->vertex_buffer_size;
  bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateBuffer(vk->device, &bci, nullptr, &vk->vertex_buffer);

  VkMemoryRequirements v_reqs;
  vkGetBufferMemoryRequirements(vk->device, vk->vertex_buffer, &v_reqs);
  VkMemoryAllocateInfo v_mai = vk_init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  v_mai.allocationSize = v_reqs.size;
  v_mai.memoryTypeIndex = find_memory_type(vk->physical_device, v_reqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(vk->device, &v_mai, nullptr, &vk->vertex_memory);
  vkBindBufferMemory(vk->device, vk->vertex_buffer, vk->vertex_memory, 0);
  vkMapMemory(vk->device, vk->vertex_memory, 0, vk->vertex_buffer_size, 0, reinterpret_cast<void **>(&vk->vertex_mapped));

  // Staging buffer
  bci.size = vk->staging_size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(vk->device, &bci, nullptr, &vk->staging_buffer);

  VkMemoryRequirements s_reqs;
  vkGetBufferMemoryRequirements(vk->device, vk->staging_buffer, &s_reqs);
  VkMemoryAllocateInfo s_mai = vk_init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  s_mai.allocationSize = s_reqs.size;
  s_mai.memoryTypeIndex = find_memory_type(vk->physical_device, s_reqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(vk->device, &s_mai, nullptr, &vk->staging_memory);
  vkBindBufferMemory(vk->device, vk->staging_buffer, vk->staging_memory, 0);
  vkMapMemory(vk->device, vk->staging_memory, 0, vk->staging_size, 0, reinterpret_cast<void **>(&vk->staging_mapped));

  // Dummy 1x1 white texture
  {
    uint32_t white_pixel = 0xFFFFFFFF;
    VkImageCreateInfo ici = vk_init<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(vk->device, &ici, nullptr, &vk->dummy_texture.image);

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(vk->device, vk->dummy_texture.image, &reqs);
    VkMemoryAllocateInfo mai = vk_init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = find_memory_type(vk->physical_device, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(vk->device, &mai, nullptr, &vk->dummy_texture.memory);
    vkBindImageMemory(vk->device, vk->dummy_texture.image, vk->dummy_texture.memory, 0);

    VkImageViewCreateInfo ivci = vk_init<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    ivci.image = vk->dummy_texture.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(vk->device, &ivci, nullptr, &vk->dummy_texture.view);

    // Upload white pixel
    std::memcpy(vk->staging_mapped, &white_pixel, 4);

    VkCommandBufferBeginInfo cbbi = vk_init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(vk->cmd, &cbbi);

    transition_image_layout(vk->cmd, vk->dummy_texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy bic{};
    bic.bufferOffset = 0;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(vk->cmd, vk->staging_buffer, vk->dummy_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    transition_image_layout(vk->cmd, vk->dummy_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    vkEndCommandBuffer(vk->cmd);
    VkSubmitInfo si = vk_init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk->cmd;
    vkQueueSubmit(vk->queue, 1, &si, vk->fence);
    vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vk->device, 1, &vk->fence);
    vk->dummy_texture.valid = true;
  }

  return vk.release();
}

void sm64_vulkan_destroy(sm64_vulkan *vk) {
  if (!vk) return;
  vkDeviceWaitIdle(vk->device);

  if (vk->framebuffer) vkDestroyFramebuffer(vk->device, vk->framebuffer, nullptr);
  if (vk->target_view) vkDestroyImageView(vk->device, vk->target_view, nullptr);
  if (vk->depth_view) vkDestroyImageView(vk->device, vk->depth_view, nullptr);
  if (vk->depth_image) vkDestroyImage(vk->device, vk->depth_image, nullptr);
  if (vk->depth_memory) vkFreeMemory(vk->device, vk->depth_memory, nullptr);

  for (auto &p : vk->textures) {
    if (p.second.view) vkDestroyImageView(vk->device, p.second.view, nullptr);
    if (p.second.image) vkDestroyImage(vk->device, p.second.image, nullptr);
    if (p.second.memory) vkFreeMemory(vk->device, p.second.memory, nullptr);
  }
  vk->textures.clear();
  for (auto &texture : vk->retired_textures) {
    if (texture.view) vkDestroyImageView(vk->device, texture.view, nullptr);
    if (texture.image) vkDestroyImage(vk->device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(vk->device, texture.memory, nullptr);
  }

  if (vk->dummy_texture.view) vkDestroyImageView(vk->device, vk->dummy_texture.view, nullptr);
  if (vk->dummy_texture.image) vkDestroyImage(vk->device, vk->dummy_texture.image, nullptr);
  if (vk->dummy_texture.memory) vkFreeMemory(vk->device, vk->dummy_texture.memory, nullptr);

  if (vk->vertex_mapped) vkUnmapMemory(vk->device, vk->vertex_memory);
  if (vk->vertex_buffer) vkDestroyBuffer(vk->device, vk->vertex_buffer, nullptr);
  if (vk->vertex_memory) vkFreeMemory(vk->device, vk->vertex_memory, nullptr);

  if (vk->staging_mapped) vkUnmapMemory(vk->device, vk->staging_memory);
  if (vk->staging_buffer) vkDestroyBuffer(vk->device, vk->staging_buffer, nullptr);
  if (vk->staging_memory) vkFreeMemory(vk->device, vk->staging_memory, nullptr);

  for (auto s : vk->samplers) if (s) vkDestroySampler(vk->device, s, nullptr);
  for (auto p : vk->pipelines) if (p) vkDestroyPipeline(vk->device, p, nullptr);

  if (vk->vs_module) vkDestroyShaderModule(vk->device, vk->vs_module, nullptr);
  if (vk->fs_module) vkDestroyShaderModule(vk->device, vk->fs_module, nullptr);
  if (vk->pipeline_layout) vkDestroyPipelineLayout(vk->device, vk->pipeline_layout, nullptr);
  if (vk->desc_pool) vkDestroyDescriptorPool(vk->device, vk->desc_pool, nullptr);
  if (vk->desc_layout) vkDestroyDescriptorSetLayout(vk->device, vk->desc_layout, nullptr);
  if (vk->render_pass_clear) vkDestroyRenderPass(vk->device, vk->render_pass_clear, nullptr);
  if (vk->render_pass_load) vkDestroyRenderPass(vk->device, vk->render_pass_load, nullptr);

  if (vk->fence) vkDestroyFence(vk->device, vk->fence, nullptr);
  if (vk->command_pool) vkDestroyCommandPool(vk->device, vk->command_pool, nullptr);

  delete vk;
}

bool sm64_vulkan_render(sm64_vulkan *vk,
                        void (*render_display_list)(uint32_t, uint32_t),
                        void (*set_render_api)(void *),
                        uint32_t width,
                        uint32_t height,
                        const ft_gpu_image *target_image,
                        bool editor_camera,
                        char *error,
                        size_t error_size) {
  if (!vk || !render_display_list || !set_render_api || !target_image || width == 0 || height == 0) {
    if (error && error_size) std::snprintf(error, error_size, "Invalid SM64 Vulkan render call");
    return false;
  }

  // Only wait when reusing this frame slot. The other slot and the editor's
  // submission can remain in flight; all work uses the same ordered queue.
  if (vk->pending) {
    if (vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        vkResetFences(vk->device, 1, &vk->fence) != VK_SUCCESS) {
      if (error && error_size) std::snprintf(error, error_size, "SM64 Vulkan frame reuse failed");
      return false;
    }
    vk->pending = false;
  }

  vk->editor_camera = editor_camera;

  for (auto &texture : vk->retired_textures) {
    if (texture.view) vkDestroyImageView(vk->device, texture.view, nullptr);
    if (texture.image) vkDestroyImage(vk->device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(vk->device, texture.memory, nullptr);
  }
  vk->retired_textures.clear();
  // Fast3D state is isolated from simulation and starts fresh each render.
  // Reuse its texture slots so paused frames neither allocate nor upload the
  // same textures repeatedly, and match the interpreter's initial state.
  vk->next_texture_id = 1;
  vk->selected_textures[0] = vk->selected_textures[1] = 0;
  vk->upload_texture_id = 0;
  vk->depth_test = vk->depth_write = vk->use_alpha = vk->zmode_decal = false;
  vk->viewport = vk->scissor = {0, 0, static_cast<int>(width), static_cast<int>(height)};

  // Check / update depth buffer
  if (vk->depth_width != width || vk->depth_height != height || vk->depth_image == VK_NULL_HANDLE) {
    if (vk->depth_view) vkDestroyImageView(vk->device, vk->depth_view, nullptr);
    if (vk->depth_image) vkDestroyImage(vk->device, vk->depth_image, nullptr);
    if (vk->depth_memory) vkFreeMemory(vk->device, vk->depth_memory, nullptr);

    vk->depth_width = width;
    vk->depth_height = height;

    VkImageCreateInfo ici = vk_init<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk->depth_format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(vk->device, &ici, nullptr, &vk->depth_image);

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(vk->device, vk->depth_image, &reqs);
    VkMemoryAllocateInfo mai = vk_init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = find_memory_type(vk->physical_device, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(vk->device, &mai, nullptr, &vk->depth_memory);
    vkBindImageMemory(vk->device, vk->depth_image, vk->depth_memory, 0);

    VkImageViewCreateInfo ivci = vk_init<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    ivci.image = vk->depth_image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = vk->depth_format;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(vk->device, &ivci, nullptr, &vk->depth_view);
  }

  // Check / update target framebuffer
  VkImage cur_target_image = static_cast<VkImage>(target_image->image);
  if (vk->target_image != cur_target_image || vk->fb_width != width || vk->fb_height != height || vk->framebuffer == VK_NULL_HANDLE) {
    if (vk->framebuffer) vkDestroyFramebuffer(vk->device, vk->framebuffer, nullptr);
    if (vk->target_view) vkDestroyImageView(vk->device, vk->target_view, nullptr);

    vk->target_image = cur_target_image;
    vk->fb_width = width;
    vk->fb_height = height;

    VkImageViewCreateInfo ivci = vk_init<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    ivci.image = vk->target_image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = static_cast<VkFormat>(target_image->format ? target_image->format : vk->color_format);
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(vk->device, &ivci, nullptr, &vk->target_view);

    VkImageView attachments[2] = {vk->target_view, vk->depth_view};
    VkFramebufferCreateInfo fbci = vk_init<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    fbci.renderPass = vk->render_pass_clear;
    fbci.attachmentCount = 2;
    fbci.pAttachments = attachments;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;
    vkCreateFramebuffer(vk->device, &fbci, nullptr, &vk->framebuffer);
  }

  // Begin command recording
  vkResetCommandBuffer(vk->cmd, 0);
  VkCommandBufferBeginInfo cbbi = vk_init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(vk->cmd, &cbbi);

  vk->first_render_pass = true;
  vk->render_pass_active = false;
  vk->vertex_offset = 0;
  vk->staging_offset = 0;
  vkResetDescriptorPool(vk->device, vk->desc_pool, 0);
  vk->desc_set_cache.clear();

  active_vulkan = vk;
  static const RenderApi api = sm64_vulkan::make_api();
  try {
    set_render_api(const_cast<RenderApi *>(&api));
    render_display_list(width, height);
  } catch (const std::exception &exception) {
    if (vk->render_pass_active) vkCmdEndRenderPass(vk->cmd);
    vk->render_pass_active = false;
    active_vulkan = nullptr;
    vkEndCommandBuffer(vk->cmd);
    // Commands were discarded. Do not reuse textures whose pending uploads
    // never reached the GPU when the next frame retries this backend.
    for (auto &entry : vk->textures) entry.second.valid = false;
    if (error && error_size) std::snprintf(error, error_size, "%s", exception.what());
    return false;
  }

  if (vk->render_pass_active) {
    vkCmdEndRenderPass(vk->cmd);
    vk->render_pass_active = false;
  }

  active_vulkan = nullptr;

  vkEndCommandBuffer(vk->cmd);

  VkSubmitInfo si = vk_init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  si.commandBufferCount = 1;
  si.pCommandBuffers = &vk->cmd;
  const VkResult submitted = vkQueueSubmit(vk->queue, 1, &si, vk->fence);
  if (submitted != VK_SUCCESS) {
    for (auto &entry : vk->textures) entry.second.valid = false;
    if (error && error_size) std::snprintf(error, error_size, "SM64 Vulkan submit failed: %d", int(submitted));
    return false;
  }
  vk->pending = true;
  return true;
}
