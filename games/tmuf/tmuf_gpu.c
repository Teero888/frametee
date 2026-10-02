// The module's renderer (tmuf_gpu.h), on the engine's Vulkan device.
//
// Textures and meshes live in device memory, uploaded through host-visible
// staging buffers in batches. A frame records one command buffer into the
// engine's image and a depth buffer of our own; TG_FRAMES of them are in
// flight (the queue orders them, the render passes' dependencies the
// accesses), each with its own uniforms (host-visible memory taken front to
// back, bound with dynamic offsets), vertices and descriptor pools, reused
// once its fence says it is done. Read-backs are copied at a frame's end and
// handed out when it is (tg_target_read_queue).

#include "tmuf_gpu.h"

#include <vulkan/vulkan.h>

#include "blit_frag_spv.h"
#include "blit_vert_spv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE (8u << 20)
#define STAGING_FLUSH (256u << 20) // an upload batch is sent when its staging reaches this
#define SETS_PER_POOL 512u
#define TG_PROFILE_MARKS 64u
#define TG_FRAMES 2u
#define TG_READS 8u
#define TG_QUERIES 1024u

typedef struct chunk {
  VkBuffer buffer;
  VkDeviceMemory memory;
  uint8_t *mapped;
  VkDeviceSize size, used;
} chunk;

typedef struct chunks {
  chunk *items;
  size_t count;
  VkBufferUsageFlags usage;
} chunks;

typedef struct gtex {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  bool cube, used, target; // target: a render target's colour (tg_target_destroy frees it)
} gtex;

struct tg_target {
  tg_texture texture;
  gtex depth;
  VkFramebuffer framebuffer;
  uint32_t width, height;
  uint32_t levels;     // its mip chain (1: none), made when its draws end
  VkImageView attach;  // the first level alone, drawn into (with a mip chain)
  bool is_float;       // RGBA 32-bit float (pass_float)
  bool depth_only;     // its depth alone, sampled (tg_target_create_depth: texture is the depth)
};

struct tg_mesh {
  VkBuffer vertices, indices;
  VkDeviceMemory vertex_memory, index_memory;
};

typedef struct pipeline_entry {
  uint32_t key;
  VkRenderPass pass;
  VkPipeline pipeline;
} pipeline_entry;

struct tg_program {
  VkShaderModule vertex, fragment;
  uint32_t stride, attr_count, uniform_size, texture_count, cube_mask;
  tg_attr attrs[16];
  pipeline_entry *pipelines;
  uint32_t pipeline_count;
};

// a descriptor set made this frame, for the draws with the same buffer,
// images and samplers (only the dynamic offset differs)
#define SET_CACHE 4096u
typedef struct set_entry {
  uint32_t gen;  // the frame slot's generation it was made in (else empty)
  uint64_t hash;
  VkBuffer ubo;
  uint32_t usize; // its range (the program's uniforms)
  VkImageView views[TG_MAX_TEXTURES];
  VkSampler samplers[TG_MAX_TEXTURES];
  VkDescriptorSet set;
} set_entry;

// a frame in flight: what it records into, and its read-backs
typedef struct frame_slot {
  VkCommandBuffer cmd;
  VkFence fence;
  bool submitted;
  chunks uniforms, dynamic;
  VkDescriptorPool *pools;
  size_t pool_count;
  set_entry *sets; // SET_CACHE; emptied when its pools are reset (gen)
  uint32_t gen;
  VkBuffer readback;
  VkDeviceMemory readback_memory;
  uint8_t *readback_mapped;
  VkDeviceSize readback_size;
  uint32_t read_count;
  struct {
    uint32_t key, serial, size;
    VkDeviceSize offset;
  } reads[TG_READS];
  VkQueryPool occ; // its occlusion queries (tg_query_begin), made when first used
  uint32_t occ_count;
  uint32_t reads_serial; // the frame it last recorded
  // TM_PROFILE: its marks (queries slot * TG_PROFILE_MARKS ..)
  uint32_t mark_count;
  const char *mark_names[TG_PROFILE_MARKS];
  uint32_t mark_draws[TG_PROFILE_MARKS];
} frame_slot;


// a read-back's latest pixels (tg_target_read_result)
typedef struct read_result {
  uint32_t key, serial, size;
  uint8_t *pixels;
  bool fresh;
} read_result;

typedef struct sampler_entry {
  uint32_t key;
  VkSampler sampler;
} sampler_entry;

struct tg {
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  VkPhysicalDeviceMemoryProperties memory;
  VkDeviceSize ubo_align;
  bool bc, anisotropy_supported;
  float anisotropy, max_anisotropy;
  VkCommandPool command_pool;
  VkCommandBuffer upload_cmd, draw_cmd; // draw_cmd: the current slot's
  VkFence fence;                        // the uploads' and tg_target_read's
  frame_slot slots[TG_FRAMES];
  uint32_t slot;   // the one recording (or last recorded)
  bool sync;       // TG_SYNC: each frame waited for (as before the slots)
  uint32_t serial; // frames begun
  read_result results[TG_READS];
  uint32_t result_count;
  const struct tg_target *pending_reads[TG_READS]; // the current slot's queued (tg_target_read_queue)
  VkDescriptorSetLayout set_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool *pools;
  size_t pool_count, pool_used, sets_in_pool;
  chunks staging, uniforms, dynamic; // dynamic: vertices drawn this frame (tg_draw_dynamic)
  bool uploading, upload_open;
  VkDeviceSize staged;

  gtex *textures; // [0] unused: texture 0 is white
  uint32_t texture_count, texture_cap;
  gtex white, white_cube;
  sampler_entry samplers[64];
  uint32_t sampler_count;

  // the frame: drawn into our colour and depth (pass clears them, pass_load
  // goes on with them after a copy), then shown in the engine's image
  VkFormat depth_format;
  gtex color, copy, depth, copy_depth;
  gtex color_ms; // multisampled: drawn into, resolved into color (and the copy)
  uint32_t width, height;
  // the main passes for each sample count (1 << i); pass and pass_load are
  // the frame's, pass_copy draws into single-sampled targets
  VkRenderPass passes[5], passes_load[5];
  // colour only, loaded ([0]: the single-sampled colour after tg_frame_resolve)
  VkRenderPass passes_colour[5];
  VkFramebuffer colour_framebuffer;
  struct tg_target *drawn_target; // the target drawn into (tg_target_begin .. end)
  VkRenderPass pass, pass_load, pass_copy;
  VkRenderPass pass_float; // into float targets (tg_target_create_float), made when first needed
  VkRenderPass pass_depth; // into depth-only targets (tg_target_create_depth), likewise
  VkRenderPass drawing; // the pass draws go into now
  VkSampleCountFlagBits samples, samples_wanted, samples_supported;
  VkFramebuffer framebuffer, copy_framebuffer; // copy_*: drawing into the copy (tg_offscreen_begin)
  bool copied; // copy holds this frame
  // the engine's image
  VkImage target;
  VkFormat format;
  VkImageLayout final_layout;
  VkImageView target_view;
  VkRenderPass present_pass;
  VkFramebuffer present_framebuffer;
  tg_program *blit;
  tg_program *pack, *pack_ms; // the depth export (tg_enable_depth_export)
  VkBuffer readback;          // host-visible, for tg_target_read
  VkDeviceMemory readback_memory;
  VkDeviceSize readback_size;

  bool recording, failed;
  // a draw held back (tg_draw): the next one, the same in all but its
  // indices that follow on, is added to it; anything else recorded sends it
  struct {
    bool on;
    tg_program *program;
    tg_state state;
    tg_mesh *mesh;
    uint32_t first, count;
    uint32_t texture_count;
    tg_texture textures[TG_MAX_TEXTURES];
    tg_sampler samplers[TG_MAX_TEXTURES];
    bool has_textures, has_samplers, has_uniforms;
    uint8_t *uniforms;
    size_t uniforms_cap;
  } held;
  // the render pass open now (none between passes: the frame's own is begun
  // when it is drawn into, so the targets drawn before it don't make it
  // store and reload its samples)
  enum { PASS_NONE, PASS_MAIN, PASS_OTHER } active;
  bool main_begun;   // this frame's colour and depth cleared (later: loaded)
  bool resolved;     // tg_frame_resolve: drawn into its single-sampled colour from now on
  // the depth handed to the engine (tg_set_export_depth: the module's own
  // single-sampled one; the frame's is an attachment alone, which nothing
  // reads: a readable multisampled depth loses its compression on AMD's
  // Polaris, and with it a third of the frame)
  tg_texture export_depth;
  // occlusion queries (tg_query_begin): the newest finished frame's counts
  uint64_t occ_results[TG_QUERIES];
  uint32_t occ_result_count, occ_result_serial;
  bool occ_open;
  VkFramebuffer resolved_framebuffer; // the colour alone, single sampled (passes_colour[0])
  float clear[4];    // the frame's clear colour
  char error[256];
  VkPipeline bound_pipeline;
  tg_mesh *bound_mesh;
  // TM_PROFILE: GPU timestamps between named marks (tg_mark), averaged
  struct {
    bool on;
    VkQueryPool pool; // TG_FRAMES * TG_PROFILE_MARKS
    float period;     // ns per tick
    uint32_t frames;
    struct {
      const char *name;
      double ms;
      double draws;
    } sums[TG_PROFILE_MARKS];
    uint32_t sum_count;
    double frame_ms;
  } prof;
};

static void held_send(tg *g);

static void fail(tg *g, const char *format, ...) {
  if (g->failed) return;
  g->failed = true;
  va_list args;
  va_start(args, format);
  vsnprintf(g->error, sizeof g->error, format, args);
  va_end(args);
}

static bool check(tg *g, VkResult r, const char *what) {
  if (r == VK_SUCCESS) return true;
  fail(g, "%s failed (VkResult %d)", what, (int)r);
  return false;
}

const char *tg_error(const tg *g) { return g->error; }
bool tg_supports_bc(const tg *g) { return g->bc; }

// --- memory --------------------------------------------------------------------

static bool allocate(tg *g, VkMemoryRequirements req, VkMemoryPropertyFlags props, VkDeviceMemory *out) {
  for (uint32_t i = 0; i < g->memory.memoryTypeCount; i++) {
    if ((req.memoryTypeBits & (1u << i)) && (g->memory.memoryTypes[i].propertyFlags & props) == props) {
      VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      info.allocationSize = req.size;
      info.memoryTypeIndex = i;
      return check(g, vkAllocateMemory(g->device, &info, NULL, out), "vkAllocateMemory");
    }
  }
  fail(g, "no memory type for %#x", (unsigned)props);
  return false;
}

static bool buffer_create(tg *g, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                          VkBuffer *buffer, VkDeviceMemory *memory) {
  VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!check(g, vkCreateBuffer(g->device, &info, NULL, buffer), "vkCreateBuffer")) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(g->device, *buffer, &req);
  return allocate(g, req, props, memory) && check(g, vkBindBufferMemory(g->device, *buffer, *memory, 0),
                                                   "vkBindBufferMemory");
}

static void chunk_destroy(tg *g, chunk *c) {
  if (c->buffer) vkDestroyBuffer(g->device, c->buffer, NULL);
  if (c->memory) vkFreeMemory(g->device, c->memory, NULL);
  memset(c, 0, sizeof *c);
}

static uint8_t *chunks_take(tg *g, chunks *cs, VkDeviceSize size, VkDeviceSize align, VkBuffer *buffer,
                            VkDeviceSize *offset) {
  for (size_t i = 0; i < cs->count; i++) {
    chunk *c = &cs->items[i];
    const VkDeviceSize at = (c->used + align - 1) / align * align;
    if (at + size <= c->size) {
      c->used = at + size;
      *buffer = c->buffer;
      *offset = at;
      return c->mapped + at;
    }
  }
  chunk *grown = realloc(cs->items, (cs->count + 1) * sizeof *grown);
  if (!grown) {
    fail(g, "out of memory");
    return NULL;
  }
  cs->items = grown;
  chunk *c = &cs->items[cs->count];
  memset(c, 0, sizeof *c);
  c->size = size > CHUNK_SIZE ? size : CHUNK_SIZE;
  if (!buffer_create(g, c->size, cs->usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &c->buffer, &c->memory) ||
      !check(g, vkMapMemory(g->device, c->memory, 0, VK_WHOLE_SIZE, 0, (void **)&c->mapped), "vkMapMemory")) {
    chunk_destroy(g, c);
    return NULL;
  }
  cs->count++;
  c->used = size;
  *buffer = c->buffer;
  *offset = 0;
  return c->mapped;
}

static void chunks_reset(chunks *cs) {
  for (size_t i = 0; i < cs->count; i++)
    cs->items[i].used = 0;
}

static void chunks_free(tg *g, chunks *cs) {
  for (size_t i = 0; i < cs->count; i++)
    chunk_destroy(g, &cs->items[i]);
  free(cs->items);
  cs->items = NULL;
  cs->count = 0;
}

// --- uploads ---------------------------------------------------------------------

static bool upload_open(tg *g) {
  if (g->upload_open) return true;
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(g, vkBeginCommandBuffer(g->upload_cmd, &begin), "vkBeginCommandBuffer")) return false;
  g->upload_open = true;
  g->staged = 0;
  return true;
}

// sends what is recorded and waits for it
static bool upload_flush(tg *g) {
  if (!g->upload_open) return true;
  g->upload_open = false;
  if (!check(g, vkEndCommandBuffer(g->upload_cmd), "vkEndCommandBuffer")) return false;
  VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &g->upload_cmd;
  const bool ok = check(g, vkResetFences(g->device, 1, &g->fence), "vkResetFences") &&
                  check(g, vkQueueSubmit(g->queue, 1, &submit, g->fence), "vkQueueSubmit") &&
                  check(g, vkWaitForFences(g->device, 1, &g->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  chunks_reset(&g->staging);
  return ok;
}

void tg_upload_begin(tg *g) { g->uploading = true; }

bool tg_upload_end(tg *g) {
  g->uploading = false;
  const bool ok = upload_flush(g);
  chunks_free(g, &g->staging);
  return ok && !g->failed;
}

static uint8_t *stage(tg *g, VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset) {
  if (g->staged + size > STAGING_FLUSH && g->staged > 0 && !upload_flush(g)) return NULL;
  if (!upload_open(g)) return NULL;
  g->staged += size;
  return chunks_take(g, &g->staging, size, 16, buffer, offset);
}

static bool upload_done(tg *g) {
  if (g->uploading) return !g->failed;
  const bool ok = upload_flush(g);
  chunks_free(g, &g->staging);
  return ok;
}

// --- textures --------------------------------------------------------------------

size_t tg_level_size(tg_format format, uint32_t width, uint32_t height) {
  if (format == TG_RGBA8) return (size_t)width * height * 4;
  const size_t blocks = (size_t)((width + 3) / 4) * ((height + 3) / 4);
  return blocks * (format == TG_BC1 ? 8 : 16);
}

static VkFormat vk_format(tg_format f) {
  switch (f) {
  case TG_BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
  case TG_BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
  case TG_BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
  default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

static bool image_create_ms(tg *g, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube,
                            VkSampleCountFlagBits samples, VkImage *image, VkDeviceMemory *memory, VkImageView *view) {
  VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = (VkExtent3D){w, h, 1};
  info.mipLevels = levels;
  info.arrayLayers = layers;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!check(g, vkCreateImage(g->device, &info, NULL, image), "vkCreateImage")) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(g->device, *image, &req);
  if (!allocate(g, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory) ||
      !check(g, vkBindImageMemory(g->device, *image, *memory, 0), "vkBindImageMemory"))
    return false;
  VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = *image;
  vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, levels, 0, layers};
  return check(g, vkCreateImageView(g->device, &vi, NULL, view), "vkCreateImageView");
}

static bool image_create(tg *g, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkFormat format,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube, VkImage *image,
                         VkDeviceMemory *memory, VkImageView *view) {
  return image_create_ms(g, w, h, levels, layers, format, usage, aspect, cube, VK_SAMPLE_COUNT_1_BIT, image, memory,
                         view);
}

static void gtex_destroy(tg *g, gtex *t) {
  if (t->view) vkDestroyImageView(g->device, t->view, NULL);
  if (t->image) vkDestroyImage(g->device, t->image, NULL);
  if (t->memory) vkFreeMemory(g->device, t->memory, NULL);
  memset(t, 0, sizeof *t);
}

static bool gtex_upload(tg *g, gtex *t, const tg_image *im) {
  const uint32_t faces = im->faces == 6 ? 6 : 1, levels = im->levels ? im->levels : 1;
  size_t total = 0;
  for (uint32_t l = 0; l < levels; l++) {
    const uint32_t w = im->width >> l ? im->width >> l : 1, h = im->height >> l ? im->height >> l : 1;
    total += tg_level_size(im->format, w, h);
  }
  total *= faces;
  if (total > im->size) {
    fail(g, "image data short (%zu of %zu bytes)", im->size, total);
    return false;
  }
  t->cube = faces == 6;
  if (!image_create(g, im->width, im->height, levels, faces, vk_format(im->format),
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, t->cube,
                    &t->image, &t->memory, &t->view))
    return false;
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t *staging = stage(g, total, &buffer, &offset);
  if (!staging) return false;
  memcpy(staging, im->data, total);
  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = t->image;
  barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, faces};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                       0, NULL, 1, &barrier);
  VkBufferImageCopy copies[6 * 16];
  uint32_t n = 0;
  VkDeviceSize at = offset;
  for (uint32_t f = 0; f < faces; f++)
    for (uint32_t l = 0; l < levels && l < 16; l++) {
      const uint32_t w = im->width >> l ? im->width >> l : 1, h = im->height >> l ? im->height >> l : 1;
      VkBufferImageCopy *c = &copies[n++];
      memset(c, 0, sizeof *c);
      c->bufferOffset = at;
      c->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
      c->imageExtent = (VkExtent3D){w, h, 1};
      at += tg_level_size(im->format, w, h);
    }
  vkCmdCopyBufferToImage(g->upload_cmd, buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, copies);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 1, &barrier);
  return true;
}

static uint32_t texture_slot(tg *g) {
  for (uint32_t i = 1; i < g->texture_count; i++)
    if (!g->textures[i].used) return i;
  if (g->texture_count == g->texture_cap) {
    const uint32_t cap = g->texture_cap ? g->texture_cap * 2 : 256;
    gtex *grown = realloc(g->textures, cap * sizeof *grown);
    if (!grown) return 0;
    memset(grown + g->texture_cap, 0, (cap - g->texture_cap) * sizeof *grown);
    g->textures = grown;
    g->texture_cap = cap;
    if (!g->texture_count) g->texture_count = 1;
  }
  return g->texture_count++;
}

tg_texture tg_texture_create(tg *g, const tg_image *image) {
  if (g->failed || !image || !image->width || !image->height) return 0;
  if (image->format != TG_RGBA8 && !g->bc) return 0;
  const uint32_t slot = texture_slot(g);
  if (!slot) return 0;
  gtex *t = &g->textures[slot];
  if (!gtex_upload(g, t, image) || !upload_done(g)) {
    gtex_destroy(g, t);
    return 0;
  }
  t->used = true;
  return slot;
}

void tg_texture_destroy(tg *g, tg_texture texture) {
  held_send(g);
  if (!texture || texture >= g->texture_count || !g->textures[texture].used || g->textures[texture].target) return;
  vkDeviceWaitIdle(g->device);
  gtex_destroy(g, &g->textures[texture]);
}

// --- meshes ------------------------------------------------------------------------

static bool device_buffer(tg *g, const void *data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer,
                          VkDeviceMemory *memory) {
  if (!buffer_create(g, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer,
                     memory))
    return false;
  VkBuffer src;
  VkDeviceSize offset;
  uint8_t *staging = stage(g, size, &src, &offset);
  if (!staging) return false;
  memcpy(staging, data, size);
  const VkBufferCopy copy = {offset, 0, size};
  vkCmdCopyBuffer(g->upload_cmd, src, *buffer, 1, &copy);
  VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = *buffer;
  barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL,
                       1, &barrier, 0, NULL);
  return true;
}

void tg_mesh_destroy(tg *g, tg_mesh *m) {
  held_send(g);
  if (!m) return;
  vkDeviceWaitIdle(g->device);
  if (m->vertices) vkDestroyBuffer(g->device, m->vertices, NULL);
  if (m->indices) vkDestroyBuffer(g->device, m->indices, NULL);
  if (m->vertex_memory) vkFreeMemory(g->device, m->vertex_memory, NULL);
  if (m->index_memory) vkFreeMemory(g->device, m->index_memory, NULL);
  free(m);
}

tg_mesh *tg_mesh_create(tg *g, const void *vertices, size_t vertices_size, const uint32_t *indices,
                        uint32_t index_count) {
  if (g->failed || !vertices_size || !index_count) return NULL;
  tg_mesh *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  if (!device_buffer(g, vertices, vertices_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m->vertices, &m->vertex_memory) ||
      !device_buffer(g, indices, (VkDeviceSize)index_count * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &m->indices,
                     &m->index_memory) ||
      !upload_done(g)) {
    tg_mesh_destroy(g, m);
    return NULL;
  }
  return m;
}

// --- programs and pipelines ---------------------------------------------------------

static VkShaderModule shader_module(tg *g, const uint32_t *code, size_t size) {
  VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = size;
  info.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  check(g, vkCreateShaderModule(g->device, &info, NULL, &m), "vkCreateShaderModule");
  return m;
}

tg_program *tg_program_create(tg *g, const tg_program_desc *d) {
  if (d->attr_count > 16 || d->texture_count > TG_MAX_TEXTURES) {
    fail(g, "program: too many attributes or textures");
    return NULL;
  }
  tg_program *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->vertex = shader_module(g, d->vertex_spirv, d->vertex_spirv_size);
  p->fragment = shader_module(g, d->fragment_spirv, d->fragment_spirv_size);
  p->stride = d->vertex_stride;
  p->attr_count = d->attr_count;
  memcpy(p->attrs, d->attrs, d->attr_count * sizeof *d->attrs);
  p->uniform_size = d->uniform_size;
  p->texture_count = d->texture_count;
  p->cube_mask = d->cube_mask;
  if (!p->vertex || !p->fragment) {
    tg_program_destroy(g, p);
    return NULL;
  }
  return p;
}

void tg_program_destroy(tg *g, tg_program *p) {
  held_send(g);
  if (!p) return;
  vkDeviceWaitIdle(g->device);
  for (uint32_t i = 0; i < p->pipeline_count; i++)
    vkDestroyPipeline(g->device, p->pipelines[i].pipeline, NULL);
  free(p->pipelines);
  if (p->vertex) vkDestroyShaderModule(g->device, p->vertex, NULL);
  if (p->fragment) vkDestroyShaderModule(g->device, p->fragment, NULL);
  free(p);
}

static VkBlendFactor blend_factor(uint8_t f) {
  switch (f) {
  case TG_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
  case TG_BLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
  case TG_BLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case TG_BLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
  case TG_BLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case TG_BLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
  case TG_BLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case TG_BLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
  case TG_BLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  default: return VK_BLEND_FACTOR_ONE;
  }
}

static VkFormat attr_format(tg_attr_format f) {
  switch (f) {
  case TG_FLOAT1: return VK_FORMAT_R32_SFLOAT;
  case TG_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
  case TG_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
  case TG_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
  case TG_UINT1: return VK_FORMAT_R32_UINT;
  default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

static uint32_t state_key(const tg_state *s) {
  return (uint32_t)s->blend_src | (uint32_t)s->blend_dst << 4 | (uint32_t)s->depth_test << 8 |
         (uint32_t)s->depth_write << 9 | (uint32_t)(s->cull & 3) << 10 | (uint32_t)s->no_color_write << 12 |
         (uint32_t)s->blend_min << 13 | (uint32_t)s->alpha_only << 14 | (uint32_t)(uint8_t)s->slope_bias << 15;
}

static VkPipeline pipeline_for(tg *g, tg_program *p, const tg_state *s, VkRenderPass pass) {
  const uint32_t key = state_key(s);
  for (uint32_t i = 0; i < p->pipeline_count; i++)
    if (p->pipelines[i].key == key && p->pipelines[i].pass == pass) return p->pipelines[i].pipeline;

  VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = p->vertex;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = p->fragment;
  stages[1].pName = "main";
  VkVertexInputBindingDescription binding = {0, p->stride, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[16];
  for (uint32_t i = 0; i < p->attr_count; i++)
    attrs[i] = (VkVertexInputAttributeDescription){p->attrs[i].location, 0, attr_format(p->attrs[i].format),
                                                   p->attrs[i].offset};
  VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  input.vertexBindingDescriptionCount = p->stride ? 1 : 0; // none: the shader makes its vertices
  input.pVertexBindingDescriptions = &binding;
  input.vertexAttributeDescriptionCount = p->attr_count;
  input.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // Direct3D names the winding that is culled as it looks on screen; drawn
  // with the engine's camera, the game's triangles keep their winding in
  // Vulkan's terms (checked against the game's traces: what it culls, so is
  // it here)
  raster.cullMode = s->cull == TG_CULL_CW || s->cull == TG_CULL_CCW ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  raster.frontFace = s->cull == TG_CULL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  raster.depthBiasEnable = s->slope_bias != 0;
  raster.depthBiasSlopeFactor = (float)s->slope_bias * 0.25f;
  raster.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  for (uint32_t i = 1; i < 5; i++)
    if (pass == g->passes[i] || pass == g->passes_load[i] || pass == g->passes_colour[i])
      ms.rasterizationSamples = (VkSampleCountFlagBits)(1u << i);
  VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = s->depth_test;
  depth.depthWriteEnable = s->depth_write;
  depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed depth
  VkPipelineColorBlendAttachmentState att = {0};
  att.blendEnable = s->blend_src != 0 || s->blend_min;
  att.srcColorBlendFactor = att.srcAlphaBlendFactor = s->blend_min ? VK_BLEND_FACTOR_ONE : blend_factor(s->blend_src);
  att.dstColorBlendFactor = att.dstAlphaBlendFactor = s->blend_min ? VK_BLEND_FACTOR_ONE : blend_factor(s->blend_dst);
  att.colorBlendOp = att.alphaBlendOp = s->blend_min ? VK_BLEND_OP_MIN : VK_BLEND_OP_ADD;
  att.colorWriteMask = s->no_color_write ? 0
                       : s->alpha_only   ? VK_COLOR_COMPONENT_A_BIT
                                         : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = pass == g->pass_depth ? 0 : 1;
  color.pAttachments = &att;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &ms;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &color;
  info.pDynamicState = &dynamic;
  info.layout = g->pipeline_layout;
  info.renderPass = pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (!check(g, vkCreateGraphicsPipelines(g->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline),
             "vkCreateGraphicsPipelines"))
    return VK_NULL_HANDLE;
  pipeline_entry *grown = realloc(p->pipelines, (p->pipeline_count + 1) * sizeof *grown);
  if (!grown) {
    vkDestroyPipeline(g->device, pipeline, NULL);
    return VK_NULL_HANDLE;
  }
  p->pipelines = grown;
  p->pipelines[p->pipeline_count++] = (pipeline_entry){key, pass, pipeline};
  return pipeline;
}

// --- samplers ------------------------------------------------------------------------

static VkSamplerAddressMode address_mode(uint8_t a) {
  return a == 2 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
         : a == 3 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
         : a == 4 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                  : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

static VkSampler sampler_for(tg *g, tg_sampler s) {
  const uint32_t key = (uint32_t)s.address_u | (uint32_t)s.address_v << 4 | (uint32_t)s.filter << 8 |
                       (uint32_t)s.mip << 12;
  for (uint32_t i = 0; i < g->sampler_count; i++)
    if (g->samplers[i].key == key) return g->samplers[i].sampler;
  if (g->sampler_count == sizeof g->samplers / sizeof g->samplers[0]) return g->samplers[0].sampler;
  VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = info.minFilter = s.filter <= 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  info.mipmapMode = s.mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = address_mode(s.address_u);
  info.addressModeV = address_mode(s.address_v);
  info.addressModeW = address_mode(s.address_v);
  info.maxLod = s.mip == 0 ? 0.f : VK_LOD_CLAMP_NONE;
  info.anisotropyEnable = s.filter == 3 && g->anisotropy_supported && g->anisotropy > 1.f;
  info.maxAnisotropy = g->anisotropy > g->max_anisotropy ? g->max_anisotropy : g->anisotropy;
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  if (!check(g, vkCreateSampler(g->device, &info, NULL, &sampler), "vkCreateSampler")) return VK_NULL_HANDLE;
  g->samplers[g->sampler_count++] = (sampler_entry){key, sampler};
  return sampler;
}

uint32_t tg_set_samples(tg *g, uint32_t samples) {
  uint32_t n = 1;
  while (n * 2 <= samples && n < 16 && (g->samples_supported & (n * 2))) n *= 2;
  g->samples_wanted = (VkSampleCountFlagBits)n;
  return n;
}

void tg_set_anisotropy(tg *g, float anisotropy) {
  held_send(g);
  if (anisotropy == g->anisotropy) return;
  vkDeviceWaitIdle(g->device);
  for (uint32_t i = 0; i < g->sampler_count; i++)
    vkDestroySampler(g->device, g->samplers[i].sampler, NULL);
  g->sampler_count = 0;
  g->anisotropy = anisotropy;
}

// --- descriptors ----------------------------------------------------------------------

static VkDescriptorSet descriptor_set(tg *g) {
  for (;;) {
    if (g->pool_used == g->pool_count) {
      VkDescriptorPool *grown = realloc(g->pools, (g->pool_count + 1) * sizeof *grown);
      if (!grown) {
        fail(g, "out of memory");
        return VK_NULL_HANDLE;
      }
      g->pools = grown;
      VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SETS_PER_POOL},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TG_MAX_TEXTURES * SETS_PER_POOL}};
      VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      info.maxSets = SETS_PER_POOL;
      info.poolSizeCount = 2;
      info.pPoolSizes = sizes;
      if (!check(g, vkCreateDescriptorPool(g->device, &info, NULL, &g->pools[g->pool_count]),
                 "vkCreateDescriptorPool"))
        return VK_NULL_HANDLE;
      g->pool_count++;
      g->sets_in_pool = 0;
    }
    if (g->sets_in_pool < SETS_PER_POOL) {
      VkDescriptorSetAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      info.descriptorPool = g->pools[g->pool_used];
      info.descriptorSetCount = 1;
      info.pSetLayouts = &g->set_layout;
      VkDescriptorSet set;
      if (!check(g, vkAllocateDescriptorSets(g->device, &info, &set), "vkAllocateDescriptorSets"))
        return VK_NULL_HANDLE;
      g->sets_in_pool++;
      return set;
    }
    g->pool_used++;
    g->sets_in_pool = 0;
  }
}

// --- the frame ------------------------------------------------------------------------

static void frame_release(tg *g) {
  if (g->framebuffer) vkDestroyFramebuffer(g->device, g->framebuffer, NULL);
  if (g->colour_framebuffer) vkDestroyFramebuffer(g->device, g->colour_framebuffer, NULL);
  g->colour_framebuffer = VK_NULL_HANDLE;
  if (g->copy_framebuffer) vkDestroyFramebuffer(g->device, g->copy_framebuffer, NULL);
  if (g->resolved_framebuffer) vkDestroyFramebuffer(g->device, g->resolved_framebuffer, NULL);
  g->framebuffer = g->copy_framebuffer = g->resolved_framebuffer = VK_NULL_HANDLE;
  gtex_destroy(g, &g->copy_depth);
  gtex_destroy(g, &g->color);
  gtex_destroy(g, &g->color_ms);
  gtex_destroy(g, &g->copy);
  gtex_destroy(g, &g->depth);
  g->width = g->height = 0;
}

static void present_release(tg *g) {
  if (g->present_framebuffer) vkDestroyFramebuffer(g->device, g->present_framebuffer, NULL);
  if (g->target_view) vkDestroyImageView(g->device, g->target_view, NULL);
  g->present_framebuffer = VK_NULL_HANDLE;
  g->target_view = VK_NULL_HANDLE;
  g->target = VK_NULL_HANDLE;
}

// the main passes: colour and depth, cleared (load false) or kept, both left
// as attachments
static bool pass_create_format(tg *g, bool load, bool sampled, VkSampleCountFlagBits samples, VkFormat format,
                               VkRenderPass *out);
static bool main_pass_create(tg *g, bool load, bool sampled, VkSampleCountFlagBits samples, VkRenderPass *out) {
  return pass_create_format(g, load, sampled, samples, VK_FORMAT_R8G8B8A8_UNORM, out);
}

static bool pass_create_format(tg *g, bool load, bool sampled, VkSampleCountFlagBits samples, VkFormat format,
                               VkRenderPass *out) {
  VkAttachmentDescription a[2] = {{0}, {0}};
  a[0].format = format;
  a[0].samples = samples;
  a[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a[0].initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  a[0].finalLayout = sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  a[1] = a[0];
  a[1].format = g->depth_format;
  a[1].initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color;
  sub.pDepthStencilAttachment = &depth;
  VkSubpassDependency deps[2] = {{0}, {0}};
  VkSubpassDependency dep = {0};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  // (the previous frame's reads of it too: frames overlap, TG_FRAMES)
  dep.srcStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_SHADER_READ_BIT;
  deps[0] = dep;
  // the copy pass: what samples the copy next waits for it
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 2;
  info.pAttachments = a;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = sampled ? 2 : 1;
  info.pDependencies = deps;
  return check(g, vkCreateRenderPass(g->device, &info, NULL, out), "vkCreateRenderPass");
}

// into the engine's image: colour only, left as the engine wants it
static bool present_pass_create(tg *g, VkFormat format, VkImageLayout final_layout) {
  VkAttachmentDescription a = {0};
  a.format = format;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a.finalLayout = final_layout;
  VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color;
  // the engine's last use of its image is over before this writes it; what
  // reads it next waits for the writes
  VkSubpassDependency dep[2] = {{0}, {0}};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &a;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = 2;
  info.pDependencies = dep;
  return check(g, vkCreateRenderPass(g->device, &info, NULL, &g->present_pass), "vkCreateRenderPass");
}

// the frame's colour alone, kept, left as an attachment
static bool colour_pass_create(tg *g, VkSampleCountFlagBits samples, VkRenderPass *out) {
  VkAttachmentDescription a = {0};
  a.format = VK_FORMAT_R8G8B8A8_UNORM;
  a.samples = samples;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference colour = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colour;
  VkSubpassDependency dep = {0};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &a;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = 1;
  info.pDependencies = &dep;
  return check(g, vkCreateRenderPass(g->device, &info, NULL, out), "vkCreateRenderPass");
}

static uint32_t sample_index(VkSampleCountFlagBits samples) {
  uint32_t i = 0;
  while ((1u << i) < (uint32_t)samples && i < 4) i++;
  return i;
}

static bool frame_prepare(tg *g, uint32_t w, uint32_t h) {
  if (g->width == w && g->height == h && g->framebuffer && g->samples == g->samples_wanted) return true;
  vkDeviceWaitIdle(g->device);
  frame_release(g);
  const VkSampleCountFlagBits samples = g->samples_wanted;
  const uint32_t si = sample_index(samples);
  if (!g->passes[si] && (!main_pass_create(g, false, false, samples, &g->passes[si]) ||
                         !main_pass_create(g, true, false, samples, &g->passes_load[si])))
    return false;
  if (!g->passes_colour[si] && !colour_pass_create(g, samples, &g->passes_colour[si])) return false;
  g->pass = g->passes[si];
  g->pass_load = g->passes_load[si];
  g->samples = samples;
  const bool ms = samples != VK_SAMPLE_COUNT_1_BIT;
  if (!image_create(g, w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, false, &g->color.image, &g->color.memory, &g->color.view) ||
      (ms && !image_create_ms(g, w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, samples, &g->color_ms.image, &g->color_ms.memory,
                              &g->color_ms.view)) ||
      !image_create(g, w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, false, &g->copy.image, &g->copy.memory, &g->copy.view) ||
      !image_create(g, w, h, 1, 1, g->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, false, &g->copy_depth.image, &g->copy_depth.memory,
                    &g->copy_depth.view) ||
      !image_create_ms(g, w, h, 1, 1, g->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, false, samples, &g->depth.image, &g->depth.memory, &g->depth.view))
    return false;
  VkImageView views[2] = {ms ? g->color_ms.view : g->color.view, g->depth.view};
  VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = g->pass;
  fb.attachmentCount = 2;
  fb.pAttachments = views;
  fb.width = w;
  fb.height = h;
  fb.layers = 1;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &g->framebuffer), "vkCreateFramebuffer")) return false;
  fb.renderPass = g->passes_colour[si];
  fb.attachmentCount = 1;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &g->colour_framebuffer), "vkCreateFramebuffer"))
    return false;
  fb.attachmentCount = 2;
  VkImageView copy_views[2] = {g->copy.view, g->copy_depth.view};
  fb.renderPass = g->pass_copy;
  fb.pAttachments = copy_views;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &g->copy_framebuffer), "vkCreateFramebuffer"))
    return false;
  g->width = w;
  g->height = h;
  // the copy is sampled before anything is copied into it
  if (!upload_open(g)) return false;
  VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = g->copy.image;
  b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       NULL, 0, NULL, 1, &b);
  return upload_done(g);
}

static bool present_prepare(tg *g, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageLayout final) {
  if (!g->present_pass || g->format != format || g->final_layout != final) {
    vkDeviceWaitIdle(g->device);
    present_release(g);
    if (g->present_pass) vkDestroyRenderPass(g->device, g->present_pass, NULL);
    g->present_pass = VK_NULL_HANDLE;
    if (!present_pass_create(g, format, final)) return false;
    g->format = format;
    g->final_layout = final;
  }
  if (g->target == image && g->present_framebuffer) return true;
  present_release(g);
  VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (!check(g, vkCreateImageView(g->device, &vi, NULL, &g->target_view), "vkCreateImageView")) return false;
  VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = g->present_pass;
  fb.attachmentCount = 1;
  fb.pAttachments = &g->target_view;
  fb.width = w;
  fb.height = h;
  fb.layers = 1;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &g->present_framebuffer), "vkCreateFramebuffer"))
    return false;
  g->target = image;
  return true;
}

static void begin_main_pass(tg *g, bool load, const float clear[4]) {
  VkClearValue cv[2];
  cv[0].color = (VkClearColorValue){{clear ? clear[0] : 0.f, clear ? clear[1] : 0.f, clear ? clear[2] : 0.f,
                                     clear ? clear[3] : 1.f}};
  cv[1].depthStencil = (VkClearDepthStencilValue){0.f, 0}; // reversed depth: far is 0
  VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = load ? g->pass_load : g->pass;
  pass.framebuffer = g->framebuffer;
  g->drawing = g->pass;
  pass.renderArea.extent = (VkExtent2D){g->width, g->height};
  pass.clearValueCount = load ? 0 : 2;
  pass.pClearValues = cv;
  vkCmdBeginRenderPass(g->draw_cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.f, 0.f, (float)g->width, (float)g->height, 0.f, 1.f};
  vkCmdSetViewport(g->draw_cmd, 0, 1, &vp);
  const VkRect2D sc = {{0, 0}, {g->width, g->height}};
  vkCmdSetScissor(g->draw_cmd, 0, 1, &sc);
  g->bound_pipeline = VK_NULL_HANDLE;
  g->bound_mesh = NULL;
  g->active = PASS_MAIN;
}

static void pass_end(tg *g) {
  if (g->active == PASS_NONE) return;
  vkCmdEndRenderPass(g->draw_cmd);
  g->active = PASS_NONE;
}

// the frame's pass, open (its colour alone while its depth is read)
static void main_ensure(tg *g) {
  if (g->active != PASS_NONE) return;
  if (g->resolved) {
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = g->passes_colour[0];
    pass.framebuffer = g->resolved_framebuffer;
    pass.renderArea.extent = (VkExtent2D){g->width, g->height};
    vkCmdBeginRenderPass(g->draw_cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp = {0.f, 0.f, (float)g->width, (float)g->height, 0.f, 1.f};
    vkCmdSetViewport(g->draw_cmd, 0, 1, &vp);
    const VkRect2D sc = {{0, 0}, {g->width, g->height}};
    vkCmdSetScissor(g->draw_cmd, 0, 1, &sc);
    g->drawing = pass.renderPass;
    g->bound_pipeline = VK_NULL_HANDLE;
    g->bound_mesh = NULL;
    g->active = PASS_OTHER;
    return;
  }
  begin_main_pass(g, g->main_begun, g->clear);
  g->main_begun = true;
}

// --- TM_PROFILE ---------------------------------------------------------------------

static void profile_begin(tg *g) {
  frame_slot *f = &g->slots[g->slot];
  f->mark_count = 0;
  if (!g->prof.pool) {
    if (!getenv("TM_PROFILE")) return;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g->physical, &props);
    g->prof.period = props.limits.timestampPeriod;
    const VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = TG_FRAMES * TG_PROFILE_MARKS};
    if (vkCreateQueryPool(g->device, &info, NULL, &g->prof.pool) != VK_SUCCESS) return;
    g->prof.on = true;
  }
  vkCmdResetQueryPool(g->draw_cmd, g->prof.pool, g->slot * TG_PROFILE_MARKS, TG_PROFILE_MARKS);
  memset(f->mark_draws, 0, sizeof f->mark_draws);
  tg_mark(g, "begin");
}

void tg_mark(tg *g, const char *name) {
  held_send(g);
  frame_slot *f = &g->slots[g->slot];
  if (!g->prof.on || !g->recording || f->mark_count >= TG_PROFILE_MARKS) return;
  vkCmdWriteTimestamp(g->draw_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->prof.pool,
                      g->slot * TG_PROFILE_MARKS + f->mark_count);
  f->mark_names[f->mark_count++] = name;
}

// a finished frame's timestamps
static void profile_harvest(tg *g, uint32_t slot) {
  frame_slot *f = &g->slots[slot];
  if (!g->prof.on || f->mark_count < 2) return;
  uint64_t t[TG_PROFILE_MARKS];
  if (vkGetQueryPoolResults(g->device, g->prof.pool, slot * TG_PROFILE_MARKS, f->mark_count, sizeof t, t, sizeof t[0],
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
    return;
  for (uint32_t i = 0; i + 1 < f->mark_count; i++) {
    const double ms = (double)(t[i + 1] - t[i]) * g->prof.period * 1e-6;
    uint32_t k = 0;
    while (k < g->prof.sum_count && strcmp(g->prof.sums[k].name, f->mark_names[i])) k++;
    if (k == g->prof.sum_count) {
      if (k == TG_PROFILE_MARKS) continue;
      g->prof.sums[k].name = f->mark_names[i];
      g->prof.sums[k].ms = g->prof.sums[k].draws = 0.0;
      g->prof.sum_count++;
    }
    g->prof.sums[k].ms += ms;
    g->prof.sums[k].draws += f->mark_draws[i];
  }
  g->prof.frame_ms += (double)(t[f->mark_count - 1] - t[0]) * g->prof.period * 1e-6;
  f->mark_count = 0;
  if (++g->prof.frames < 120) return;
  fprintf(stderr, "gpu profile (%u frames): %.3f ms per frame, %ux%u, %d samples\n", g->prof.frames,
          g->prof.frame_ms / g->prof.frames, g->width, g->height, (int)g->samples);
  for (uint32_t k = 0; k < g->prof.sum_count; k++)
    fprintf(stderr, "  gpu %-20s %7.3f ms %7.1f draws\n", g->prof.sums[k].name, g->prof.sums[k].ms / g->prof.frames,
            g->prof.sums[k].draws / g->prof.frames);
  g->prof.frames = 0;
  g->prof.sum_count = 0;
  g->prof.frame_ms = 0.0;
}

// --- frames in flight ------------------------------------------------------------------

// a finished frame: its read-backs to their results, its timestamps
static void slot_harvest(tg *g, uint32_t slot) {
  frame_slot *f = &g->slots[slot];
  if (!f->submitted) return;
  f->submitted = false;
  for (uint32_t i = 0; i < f->read_count; i++) {
    uint32_t k = 0;
    while (k < g->result_count && g->results[k].key != f->reads[i].key) k++;
    if (k == g->result_count) {
      if (k == TG_READS) continue;
      memset(&g->results[k], 0, sizeof g->results[k]);
      g->results[k].key = f->reads[i].key;
      g->result_count++;
    }
    read_result *r = &g->results[k];
    // (a later frame's may be in already: keep the newest)
    if (r->pixels && (int32_t)(f->reads[i].serial - r->serial) < 0) continue;
    if (r->size < f->reads[i].size) {
      uint8_t *grown = realloc(r->pixels, f->reads[i].size);
      if (!grown) continue;
      r->pixels = grown;
    }
    r->size = f->reads[i].size;
    memcpy(r->pixels, f->readback_mapped + f->reads[i].offset, f->reads[i].size);
    r->serial = f->reads[i].serial;
    r->fresh = true;
  }
  f->read_count = 0;
  if (f->occ && f->occ_count) {
    const uint32_t serial = f->reads_serial;
    if (!g->occ_result_count || (int32_t)(serial - g->occ_result_serial) > 0) {
      if (vkGetQueryPoolResults(g->device, f->occ, 0, f->occ_count, sizeof g->occ_results, g->occ_results,
                                sizeof g->occ_results[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        g->occ_result_count = f->occ_count;
        g->occ_result_serial = serial;
      }
    }
  }
  f->occ_count = 0;
  profile_harvest(g, slot);
}

// the next slot, free: its last frame waited for (and the others' taken
// when they are done)
static bool slot_next(tg *g) {
  g->slot = (g->slot + 1) % TG_FRAMES;
  frame_slot *f = &g->slots[g->slot];
  if (f->submitted && !check(g, vkWaitForFences(g->device, 1, &f->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
    return false;
  // (oldest first: the read-backs' newest wins)
  for (uint32_t k = 1; k <= TG_FRAMES; k++) {
    const uint32_t i = (g->slot + k) % TG_FRAMES;
    if (g->slots[i].submitted && (i == g->slot || vkGetFenceStatus(g->device, g->slots[i].fence) == VK_SUCCESS))
      slot_harvest(g, i);
  }
  g->draw_cmd = f->cmd;
  return true;
}

void tg_target_read_queue(tg *g, tg_target *t, uint32_t key) {
  held_send(g);
  frame_slot *f = &g->slots[g->slot];
  if (!g->recording || g->failed || !t || f->read_count >= TG_READS) return;
  const uint32_t size = t->width * t->height * 4u;
  VkDeviceSize at = 0;
  for (uint32_t i = 0; i < f->read_count; i++)
    at = f->reads[i].offset + f->reads[i].size > at ? f->reads[i].offset + f->reads[i].size : at;
  at = (at + 15u) & ~(VkDeviceSize)15u;
  if (f->readback_size < at + size) {
    // (grown only before any read of this frame is queued: earlier ones' copies are recorded)
    if (f->read_count) return;
    if (f->readback) vkDestroyBuffer(g->device, f->readback, NULL);
    if (f->readback_memory) vkFreeMemory(g->device, f->readback_memory, NULL);
    f->readback = VK_NULL_HANDLE, f->readback_memory = VK_NULL_HANDLE, f->readback_mapped = NULL;
    f->readback_size = 0;
    const VkDeviceSize want = size < (1u << 20) ? (1u << 20) : size;
    if (!buffer_create(g, want, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &f->readback,
                       &f->readback_memory) ||
        !check(g, vkMapMemory(g->device, f->readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&f->readback_mapped),
               "vkMapMemory"))
      return;
    f->readback_size = want;
  }
  f->reads[f->read_count].key = key;
  f->reads[f->read_count].serial = g->serial;
  f->reads[f->read_count].size = size;
  f->reads[f->read_count].offset = at;
  f->read_count++;
  // (recorded at the frame's end: tg_frame_end, outside the passes)
  f->reads[f->read_count - 1].key = key;
  (void)t;
  g->pending_reads[f->read_count - 1] = t;
}

bool tg_target_read_result(tg *g, uint32_t key, uint8_t *out, uint32_t size, uint32_t *serial) {
  for (uint32_t k = 0; k < g->result_count; k++) {
    read_result *r = &g->results[k];
    if (r->key != key || !r->pixels || r->size != size) continue;
    memcpy(out, r->pixels, size);
    if (serial) *serial = r->serial;
    const bool fresh = r->fresh;
    r->fresh = false;
    return fresh;
  }
  return false;
}

uint32_t tg_frame_serial(const tg *g) { return g->serial; }

// the queued read-backs' copies, at the end of the frame
static void record_reads(tg *g) {
  frame_slot *f = &g->slots[g->slot];
  for (uint32_t i = 0; i < f->read_count; i++) {
    const tg_target *t = g->pending_reads[i];
    const VkImage image = g->textures[t->texture].image;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    const VkBufferImageCopy copy = {f->reads[i].offset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                    {t->width, t->height, 1}};
    vkCmdCopyImageToBuffer(g->draw_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->readback, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
  }
  if (f->read_count) {
    // the copies visible to the host once the fence is waited
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
  }
}

bool tg_frame_begin(tg *g, uint64_t image, uint32_t format, uint32_t width, uint32_t height, uint32_t final_layout,
                    const float clear[4]) {
  g->failed = false;
  // (the depth's half beside the colour's when exported)
  const uint32_t image_width = g->pack ? width * 2u : width;
  if (!frame_prepare(g, width, height) ||
      !present_prepare(g, (VkImage)(uintptr_t)image, (VkFormat)format, image_width, height, (VkImageLayout)final_layout))
    return false;
  // the next frame slot: its last frame done, its memory ours again
  {
    frame_slot *f = &g->slots[g->slot];
    f->uniforms = g->uniforms, f->dynamic = g->dynamic, f->pools = g->pools, f->pool_count = g->pool_count;
  }
  if (!slot_next(g)) return false;
  {
    frame_slot *f = &g->slots[g->slot];
    g->uniforms = f->uniforms, g->dynamic = f->dynamic, g->pools = f->pools, g->pool_count = f->pool_count;
  }
  g->serial++;
  chunks_reset(&g->uniforms);
  chunks_reset(&g->dynamic);
  for (size_t i = 0; i < g->pool_count; i++)
    vkResetDescriptorPool(g->device, g->pools[i], 0);
  g->pool_used = 0;
  g->sets_in_pool = 0;
  {
    frame_slot *f = &g->slots[g->slot];
    if (!f->sets) f->sets = calloc(SET_CACHE, sizeof *f->sets);
    f->gen++;
    if (!f->gen) { // (wrapped: every entry's old generation might match)
      if (f->sets) memset(f->sets, 0, SET_CACHE * sizeof *f->sets);
      f->gen = 1;
    }
  }
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(g, vkBeginCommandBuffer(g->draw_cmd, &begin), "vkBeginCommandBuffer")) return false;
  g->copied = false;
  {
    frame_slot *f = &g->slots[g->slot];
    f->reads_serial = g->serial;
    f->occ_count = 0;
    if (f->occ) vkCmdResetQueryPool(g->draw_cmd, f->occ, 0, TG_QUERIES);
  }
  g->occ_open = false;
  profile_begin(g);
  memcpy(g->clear, clear, sizeof g->clear);
  g->main_begun = false;
  g->resolved = false;
  g->export_depth = 0;
  g->active = PASS_NONE;
  g->recording = true;
  return true;
}

void tg_frame_copy(tg *g) {
  held_send(g);
  if (!g->recording || g->failed) return;
  VkCommandBuffer cmd = g->draw_cmd;
  // (the frame as it is: begun if it was not, so cleared)
  main_ensure(g);
  pass_end(g);
  VkImageMemoryBarrier b[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                               {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
  for (int i = 0; i < 2; i++) {
    b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  const bool ms = g->samples != VK_SAMPLE_COUNT_1_BIT && !g->resolved;
  const VkImage src = ms ? g->color_ms.image : g->color.image;
  b[0].image = src;
  b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[1].image = g->copy.image;
  b[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
  const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  const VkExtent3D extent = {g->width, g->height, 1};
  if (ms) {
    const VkImageResolve region = {layers, {0, 0, 0}, layers, {0, 0, 0}, extent};
    vkCmdResolveImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->copy.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else {
    const VkImageCopy region = {layers, {0, 0, 0}, layers, {0, 0, 0}, extent};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->copy.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       NULL, 0, NULL, 2, b);
  g->copied = true;
}

// the pipeline, uniforms and textures of a draw
static bool draw_setup(tg *g, tg_program *p, const tg_state *state, const tg_texture *textures,
                       const tg_sampler *samplers, const void *uniforms) {
  VkCommandBuffer cmd = g->draw_cmd;
  const VkPipeline pipeline = pipeline_for(g, p, state, g->drawing);
  if (!pipeline) return false;
  if (pipeline != g->bound_pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g->bound_pipeline = pipeline;
  }
  VkBuffer ubo = VK_NULL_HANDLE;
  VkDeviceSize ubo_offset = 0;
  const uint32_t usize = p->uniform_size ? p->uniform_size : 16;
  uint8_t *u = chunks_take(g, &g->uniforms, usize, g->ubo_align, &ubo, &ubo_offset);
  if (!u) return false;
  if (uniforms && p->uniform_size) memcpy(u, uniforms, p->uniform_size);
  // every binding the layout has gets something: unused ones white
  VkImageView views[TG_MAX_TEXTURES];
  VkSampler vks[TG_MAX_TEXTURES];
  uint64_t hash = (1469598103934665603ull ^ (uint64_t)(uintptr_t)ubo) * 1099511628211ull ^ usize;
  for (uint32_t i = 0; i < TG_MAX_TEXTURES; i++) {
    const bool cube = (p->cube_mask >> i) & 1u;
    const tg_texture t = textures && i < p->texture_count ? textures[i] : 0;
    const gtex *x = t == TG_TEXTURE_FRAME && g->copied ? &g->copy
                    : t && t < g->texture_count && g->textures[t].used && g->textures[t].cube == cube ? &g->textures[t]
                    : cube ? &g->white_cube
                           : &g->white;
    const tg_sampler s = samplers && i < p->texture_count ? samplers[i] : TG_SAMPLER_WRAP;
    views[i] = x->view;
    vks[i] = sampler_for(g, s);
    hash = (hash ^ (uint64_t)(uintptr_t)views[i]) * 1099511628211ull;
    hash = (hash ^ (uint64_t)(uintptr_t)vks[i]) * 1099511628211ull;
  }
  // the same set again when this frame made one with these
  frame_slot *f = &g->slots[g->slot];
  set_entry *e = NULL;
  if (f->sets) {
    for (uint32_t k = 0; k < 16; k++) {
      set_entry *c = &f->sets[(hash + k) & (SET_CACHE - 1u)];
      if (c->gen != f->gen) {
        e = c;
        break;
      }
      if (c->hash == hash && c->ubo == ubo && c->usize == usize && !memcmp(c->views, views, sizeof views) &&
          !memcmp(c->samplers, vks, sizeof vks)) {
        const uint32_t dynamic = (uint32_t)ubo_offset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipeline_layout, 0, 1, &c->set, 1, &dynamic);
        return true;
      }
    }
  }
  VkDescriptorSet set = descriptor_set(g);
  if (!set) return false;
  VkDescriptorBufferInfo bi = {ubo, 0, usize};
  VkDescriptorImageInfo images[TG_MAX_TEXTURES];
  VkWriteDescriptorSet writes[TG_MAX_TEXTURES + 1];
  memset(writes, 0, sizeof writes);
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[0].pBufferInfo = &bi;
  for (uint32_t i = 0; i < TG_MAX_TEXTURES; i++) {
    images[i] = (VkDescriptorImageInfo){vks[i], views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i + 1].dstSet = set;
    writes[i + 1].dstBinding = i + 1;
    writes[i + 1].descriptorCount = 1;
    writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i + 1].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(g->device, TG_MAX_TEXTURES + 1, writes, 0, NULL);
  if (e) {
    e->gen = f->gen;
    e->hash = hash;
    e->ubo = ubo;
    e->usize = usize;
    memcpy(e->views, views, sizeof views);
    memcpy(e->samplers, vks, sizeof vks);
    e->set = set;
  }
  const uint32_t dynamic = (uint32_t)ubo_offset;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipeline_layout, 0, 1, &set, 1, &dynamic);
  return true;
}

// the draw held back, recorded
static void held_send(tg *g) {
  if (!g->held.on) return;
  g->held.on = false;
  tg_program *p = g->held.program;
  if (!g->recording || g->failed) return;
  main_ensure(g);
  if (!draw_setup(g, p, &g->held.state, g->held.has_textures ? g->held.textures : NULL,
                  g->held.has_samplers ? g->held.samplers : NULL, g->held.has_uniforms ? g->held.uniforms : NULL))
    return;
  if (g->prof.on && g->slots[g->slot].mark_count) g->slots[g->slot].mark_draws[g->slots[g->slot].mark_count - 1]++;
  VkCommandBuffer cmd = g->draw_cmd;
  tg_mesh *mesh = g->held.mesh;
  if (mesh != g->bound_mesh) {
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices, &zero);
    vkCmdBindIndexBuffer(cmd, mesh->indices, 0, VK_INDEX_TYPE_UINT32);
    g->bound_mesh = mesh;
  }
  vkCmdDrawIndexed(cmd, g->held.count, 1, g->held.first, 0, 0);
}

static bool held_same(const tg *g, const tg_program *p, const tg_state *state, const tg_mesh *mesh,
                      uint32_t first_index, const tg_texture *textures, const tg_sampler *samplers,
                      const void *uniforms) {
  if (!g->held.on || g->held.program != p || g->held.mesh != mesh || first_index != g->held.first + g->held.count ||
      state_key(&g->held.state) != state_key(state) || g->held.has_textures != (textures != NULL) ||
      g->held.has_samplers != (samplers != NULL) || g->held.has_uniforms != (uniforms != NULL && p->uniform_size))
    return false;
  const uint32_t n = p->texture_count < TG_MAX_TEXTURES ? p->texture_count : TG_MAX_TEXTURES;
  if (textures && memcmp(g->held.textures, textures, n * sizeof *textures)) return false;
  if (samplers && memcmp(g->held.samplers, samplers, n * sizeof *samplers)) return false;
  return !g->held.has_uniforms || !memcmp(g->held.uniforms, uniforms, p->uniform_size);
}

void tg_draw(tg *g, tg_program *p, const tg_state *state, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
             const tg_texture *textures, const tg_sampler *samplers, const void *uniforms) {
  if (!g->recording || g->failed || !p || !index_count) return;
  if (!mesh) { // the program makes its own vertices
    held_send(g);
    main_ensure(g);
    if (!draw_setup(g, p, state, textures, samplers, uniforms)) return;
    if (g->prof.on && g->slots[g->slot].mark_count) g->slots[g->slot].mark_draws[g->slots[g->slot].mark_count - 1]++;
    vkCmdDraw(g->draw_cmd, index_count, 1, first_index, 0);
    return;
  }
  if (held_same(g, p, state, mesh, first_index, textures, samplers, uniforms)) {
    g->held.count += index_count;
    return;
  }
  held_send(g);
  if (uniforms && p->uniform_size > g->held.uniforms_cap) {
    uint8_t *grown = realloc(g->held.uniforms, p->uniform_size);
    if (!grown) {
      fail(g, "out of memory");
      return;
    }
    g->held.uniforms = grown;
    g->held.uniforms_cap = p->uniform_size;
  }
  const uint32_t n = p->texture_count < TG_MAX_TEXTURES ? p->texture_count : TG_MAX_TEXTURES;
  g->held.on = true;
  g->held.program = p;
  g->held.state = *state;
  g->held.mesh = mesh;
  g->held.first = first_index;
  g->held.count = index_count;
  g->held.has_textures = textures != NULL;
  g->held.has_samplers = samplers != NULL;
  g->held.has_uniforms = uniforms != NULL && p->uniform_size;
  memset(g->held.textures, 0, sizeof g->held.textures);
  memset(g->held.samplers, 0, sizeof g->held.samplers);
  if (textures) memcpy(g->held.textures, textures, n * sizeof *textures);
  if (samplers) memcpy(g->held.samplers, samplers, n * sizeof *samplers);
  if (g->held.has_uniforms) memcpy(g->held.uniforms, uniforms, p->uniform_size);
}

void tg_draw_dynamic(tg *g, tg_program *p, const tg_state *state, const void *vertices, uint32_t vertex_count,
                     const tg_texture *textures, const tg_sampler *samplers, const void *uniforms) {
  held_send(g);
  if (!g->recording || g->failed || !p || !vertex_count || !p->stride) return;
  main_ensure(g);
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  const VkDeviceSize size = (VkDeviceSize)vertex_count * p->stride;
  uint8_t *dst = chunks_take(g, &g->dynamic, size, 16, &buffer, &offset);
  if (!dst || !draw_setup(g, p, state, textures, samplers, uniforms)) return;
  if (g->prof.on && g->slots[g->slot].mark_count) g->slots[g->slot].mark_draws[g->slots[g->slot].mark_count - 1]++;
  memcpy(dst, vertices, size);
  vkCmdBindVertexBuffers(g->draw_cmd, 0, 1, &buffer, &offset);
  g->bound_mesh = NULL;
  vkCmdDraw(g->draw_cmd, vertex_count, 1, 0, 0);

}

void tg_offscreen_begin(tg *g, const float clear[4]) {
  held_send(g);
  if (!g->recording || g->failed) return;
  pass_end(g);
  VkClearValue cv[2];
  cv[0].color = (VkClearColorValue){{clear[0], clear[1], clear[2], clear[3]}};
  cv[1].depthStencil = (VkClearDepthStencilValue){0.f, 0};
  VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = g->pass_copy;
  pass.framebuffer = g->copy_framebuffer;
  g->drawing = g->pass_copy;
  pass.renderArea.extent = (VkExtent2D){g->width, g->height};
  pass.clearValueCount = 2;
  pass.pClearValues = cv;
  vkCmdBeginRenderPass(g->draw_cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
  g->active = PASS_OTHER;
  g->copied = false;
  g->bound_pipeline = VK_NULL_HANDLE;
  g->bound_mesh = NULL;
}

void tg_offscreen_end(tg *g) {
  held_send(g);
  if (!g->recording || g->failed) return;
  pass_end(g);
  g->copied = true;
}

uint32_t tg_samples(const tg *g) { return (uint32_t)g->samples; }

bool tg_enable_depth_export(tg *g, const void *pack, size_t pack_size, const void *pack_ms, size_t pack_ms_size) {
  // the blit's triangle over the view
  tg_program_desc desc = {.vertex_spirv = k_blit_vert_spv, .vertex_spirv_size = sizeof k_blit_vert_spv,
                          .fragment_spirv = pack, .fragment_spirv_size = pack_size, .uniform_size = 16,
                          .texture_count = 1};
  g->pack = tg_program_create(g, &desc);
  desc.fragment_spirv = pack_ms;
  desc.fragment_spirv_size = pack_ms_size;
  g->pack_ms = tg_program_create(g, &desc);
  return g->pack != NULL;
}

bool tg_depth_exported(const tg *g) { return g && g->pack; }

static tg_target *target_create(tg *g, uint32_t width, uint32_t height, uint32_t levels, bool is_float) {
  if (g->failed || !width || !height) return NULL;
  if (is_float && !g->pass_float &&
      !pass_create_format(g, false, true, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, &g->pass_float))
    return NULL;
  tg_target *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->width = width;
  t->height = height;
  t->levels = levels;
  t->is_float = is_float;
  const VkFormat format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  if (!(t->texture = texture_slot(g))) {
    free(t);
    return NULL;
  }
  gtex *c = &g->textures[t->texture];
  c->used = c->target = true;
  if (!image_create(g, width, height, levels, 1, format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0),
                    VK_IMAGE_ASPECT_COLOR_BIT, false, &c->image, &c->memory, &c->view) ||
      !image_create(g, width, height, 1, 1, g->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, false, &t->depth.image, &t->depth.memory, &t->depth.view)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  if (levels > 1) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = c->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!check(g, vkCreateImageView(g->device, &vi, NULL, &t->attach), "vkCreateImageView")) {
      tg_target_destroy(g, t);
      return NULL;
    }
  }
  VkImageView views[2] = {levels > 1 ? t->attach : c->view, t->depth.view};
  VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = is_float ? g->pass_float : g->pass_copy;
  fb.attachmentCount = 2;
  fb.pAttachments = views;
  fb.width = width;
  fb.height = height;
  fb.layers = 1;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &t->framebuffer), "vkCreateFramebuffer") ||
      !upload_open(g)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  // sampled before anything is drawn into it
  VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = c->image;
  b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       NULL, 0, NULL, 1, &b);
  if (!upload_done(g)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  return t;
}

// the depth-only targets' pass: cleared, left for sampling; after the frame
// before's reads of it (frames overlap) and before this one's
static bool depth_pass_create(tg *g) {
  VkAttachmentDescription a = {0};
  a.format = g->depth_format;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference depth = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.pDepthStencilAttachment = &depth;
  VkSubpassDependency dep[2] = {{0}, {0}};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &a;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = 2;
  info.pDependencies = dep;
  return check(g, vkCreateRenderPass(g->device, &info, NULL, &g->pass_depth), "vkCreateRenderPass");
}

tg_target *tg_target_create_depth(tg *g, uint32_t width, uint32_t height) {
  if (g->failed || !width || !height) return NULL;
  if (!g->pass_depth && !depth_pass_create(g)) return NULL;
  tg_target *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->width = width;
  t->height = height;
  t->levels = 1;
  t->depth_only = true;
  if (!(t->texture = texture_slot(g))) {
    free(t);
    return NULL;
  }
  gtex *c = &g->textures[t->texture];
  c->used = c->target = true;
  if (!image_create(g, width, height, 1, 1, g->depth_format,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    false, &c->image, &c->memory, &c->view)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = g->pass_depth;
  fb.attachmentCount = 1;
  fb.pAttachments = &c->view;
  fb.width = width;
  fb.height = height;
  fb.layers = 1;
  if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &t->framebuffer), "vkCreateFramebuffer") ||
      !upload_open(g)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  // sampled before anything is drawn into it
  VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = c->image;
  b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       NULL, 0, NULL, 1, &b);
  if (!upload_done(g)) {
    tg_target_destroy(g, t);
    return NULL;
  }
  return t;
}

tg_target *tg_target_create(tg *g, uint32_t width, uint32_t height) { return target_create(g, width, height, 1, false); }

tg_target *tg_target_create_float(tg *g, uint32_t width, uint32_t height) {
  return target_create(g, width, height, 1, true);
}

tg_target *tg_target_create_mips(tg *g, uint32_t width, uint32_t height) {
  uint32_t levels = 1;
  for (uint32_t m = width > height ? width : height; m > 1; m >>= 1)
    levels++;
  return target_create(g, width, height, levels, false);
}

// a target's mip chain from its first level, each level halved from the
// one before (box filtered), all left for sampling
static void target_mips(tg *g, tg_target *t) {
  const VkImage image = g->textures[t->texture].image;
  VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  int32_t w = (int32_t)t->width, h = (int32_t)t->height;
  for (uint32_t l = 1; l < t->levels; l++) {
    // the level before: written, to be read; this one: to be written
    VkImageMemoryBarrier two[2] = {b, b};
    two[0].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, 0, 1};
    two[0].oldLayout = l == 1 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    two[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    two[0].srcAccessMask = l == 1 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    two[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    two[1].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1};
    two[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    two[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    two[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    two[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, two);
    const int32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    VkImageBlit blit = {0};
    blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
    blit.srcOffsets[1] = (VkOffset3D){w, h, 1};
    blit.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
    blit.dstOffsets[1] = (VkOffset3D){nw, nh, 1};
    vkCmdBlitImage(g->draw_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);
    w = nw, h = nh;
  }
  // all of them back for sampling: the last written, the others read
  VkImageMemoryBarrier back[2] = {b, b};
  back[0].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels - 1, 0, 1};
  back[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  back[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  back[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  back[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  back[1].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, t->levels - 1, 1, 0, 1};
  back[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  back[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  back[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  back[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 2, back);
}

void tg_target_destroy(tg *g, tg_target *t) {
  held_send(g);
  if (!t) return;
  vkDeviceWaitIdle(g->device);
  if (t->framebuffer) vkDestroyFramebuffer(g->device, t->framebuffer, NULL);
  if (t->attach) vkDestroyImageView(g->device, t->attach, NULL);
  gtex_destroy(g, &t->depth);
  if (t->texture) gtex_destroy(g, &g->textures[t->texture]);
  free(t);
}

tg_texture tg_target_texture(const tg_target *t) { return t ? t->texture : 0; }
uint32_t tg_target_width(const tg_target *t) { return t ? t->width : 0; }
uint32_t tg_target_height(const tg_target *t) { return t ? t->height : 0; }

void tg_target_begin(tg *g, tg_target *t, const float clear[4]) {
  held_send(g);
  if (!g->recording || g->failed || !t) return;
  pass_end(g);
  g->drawn_target = t;
  VkClearValue cv[2];
  cv[0].color = (VkClearColorValue){{clear[0], clear[1], clear[2], clear[3]}};
  cv[1].depthStencil = (VkClearDepthStencilValue){0.f, 0};
  VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = t->depth_only ? g->pass_depth : t->is_float ? g->pass_float : g->pass_copy;
  pass.framebuffer = t->framebuffer;
  g->drawing = pass.renderPass;
  pass.renderArea.extent = (VkExtent2D){t->width, t->height};
  // (a depth-only target: its depth cleared to 0, the reversed far)
  pass.clearValueCount = t->depth_only ? 1 : 2;
  pass.pClearValues = t->depth_only ? &cv[1] : cv;
  vkCmdBeginRenderPass(g->draw_cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.f, 0.f, (float)t->width, (float)t->height, 0.f, 1.f};
  vkCmdSetViewport(g->draw_cmd, 0, 1, &vp);
  const VkRect2D sc = {{0, 0}, {t->width, t->height}};
  vkCmdSetScissor(g->draw_cmd, 0, 1, &sc);
  g->bound_pipeline = VK_NULL_HANDLE;
  g->bound_mesh = NULL;
  g->active = PASS_OTHER;
}

void tg_scissor(tg *g, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  held_send(g);
  if (!g->recording || g->failed) return;
  const tg_target *t = g->drawn_target;
  const uint32_t fw = t ? t->width : g->width, fh = t ? t->height : g->height;
  const VkRect2D sc = w ? (VkRect2D){{x, y}, {w, h}} : (VkRect2D){{0, 0}, {fw, fh}};
  vkCmdSetScissor(g->draw_cmd, 0, 1, &sc);
}

void tg_target_end(tg *g) {
  held_send(g);
  if (!g->recording || g->failed) return;
  pass_end(g);
  if (g->drawn_target && g->drawn_target->levels > 1) target_mips(g, g->drawn_target);
  g->drawn_target = NULL;
  // (back to the frame, or its colour alone while its depth is read, when
  // it is drawn into next)
}

bool tg_target_read(tg *g, tg_target *t, uint8_t *out) {
  if (g->failed || !t || g->recording) return false;
  const VkDeviceSize size = (VkDeviceSize)t->width * t->height * 4;
  if (g->readback_size < size) {
    if (g->readback) vkDestroyBuffer(g->device, g->readback, NULL);
    if (g->readback_memory) vkFreeMemory(g->device, g->readback_memory, NULL);
    g->readback = VK_NULL_HANDLE, g->readback_memory = VK_NULL_HANDLE, g->readback_size = 0;
    if (!buffer_create(g, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g->readback,
                       &g->readback_memory))
      return false;
    g->readback_size = size;
  }
  const VkImage image = g->textures[t->texture].image;
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(g, vkBeginCommandBuffer(g->upload_cmd, &begin), "vkBeginCommandBuffer")) return false;
  VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, NULL, 0, NULL, 1, &b);
  const VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t->width, t->height, 1}};
  vkCmdCopyImageToBuffer(g->upload_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->readback, 1, &copy);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g->upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       NULL, 0, NULL, 1, &b);
  if (!check(g, vkEndCommandBuffer(g->upload_cmd), "vkEndCommandBuffer")) return false;
  VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &g->upload_cmd;
  if (!check(g, vkResetFences(g->device, 1, &g->fence), "vkResetFences") ||
      !check(g, vkQueueSubmit(g->queue, 1, &submit, g->fence), "vkQueueSubmit") ||
      !check(g, vkWaitForFences(g->device, 1, &g->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
    return false;
  void *mapped = NULL;
  if (!check(g, vkMapMemory(g->device, g->readback_memory, 0, size, 0, &mapped), "vkMapMemory")) return false;
  memcpy(out, mapped, (size_t)size);
  vkUnmapMemory(g->device, g->readback_memory);
  return true;
}

// A full-view program in the present pass, its binding 1 the image `source`,
// its viewport at x (the frame's width: the colour's half, then the depth's)
static void present_draw(tg *g, tg_program *program, VkImageView source, uint32_t x) {
  VkCommandBuffer cmd = g->draw_cmd;
  const VkViewport vp = {(float)x, 0.f, (float)g->width, (float)g->height, 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  const VkRect2D sc = {{(int32_t)x, 0}, {g->width, g->height}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
  const tg_state state = {0};
  const VkPipeline pipeline = pipeline_for(g, program, &state, g->present_pass);
  VkDescriptorSet set = descriptor_set(g);
  VkBuffer ubo = VK_NULL_HANDLE;
  VkDeviceSize ubo_offset = 0;
  if (pipeline && set && chunks_take(g, &g->uniforms, 16, g->ubo_align, &ubo, &ubo_offset)) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorBufferInfo bi = {ubo, 0, 16};
    VkDescriptorImageInfo images[TG_MAX_TEXTURES];
    VkWriteDescriptorSet writes[TG_MAX_TEXTURES + 1];
    memset(writes, 0, sizeof writes);
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo = &bi;
    for (uint32_t i = 0; i < TG_MAX_TEXTURES; i++) {
      const VkImageView view = i == 0 ? source : g->white.view;
      images[i] = (VkDescriptorImageInfo){sampler_for(g, (tg_sampler){3, 3, 1, 0}), view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i + 1].dstSet = set;
      writes[i + 1].dstBinding = i + 1;
      writes[i + 1].descriptorCount = 1;
      writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i + 1].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(g->device, TG_MAX_TEXTURES + 1, writes, 0, NULL);
    const uint32_t dynamic = (uint32_t)ubo_offset;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipeline_layout, 0, 1, &set, 1, &dynamic);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
}

uint32_t tg_query_begin(tg *g) {
  held_send(g);
  if (!g->recording || g->failed || g->occ_open || g->resolved) return UINT32_MAX;
  frame_slot *f = &g->slots[g->slot];
  if (!f->occ) {
    // (made outside the passes, reset for this frame at once)
    const VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_OCCLUSION,
                                        .queryCount = TG_QUERIES};
    if (vkCreateQueryPool(g->device, &info, NULL, &f->occ) != VK_SUCCESS) return UINT32_MAX;
    pass_end(g);
    vkCmdResetQueryPool(g->draw_cmd, f->occ, 0, TG_QUERIES);
  }
  if (f->occ_count >= TG_QUERIES) return UINT32_MAX;
  main_ensure(g);
  vkCmdBeginQuery(g->draw_cmd, f->occ, f->occ_count, 0);
  g->occ_open = true;
  return f->occ_count++;
}

void tg_query_end(tg *g) {
  held_send(g);
  if (!g->occ_open) return;
  vkCmdEndQuery(g->draw_cmd, g->slots[g->slot].occ, g->slots[g->slot].occ_count - 1);
  g->occ_open = false;
}

uint32_t tg_query_results(const tg *g, uint32_t *serial, const uint64_t **counts) {
  if (serial) *serial = g->occ_result_serial;
  if (counts) *counts = g->occ_results;
  return g->occ_result_count;
}

void tg_set_export_depth(tg *g, tg_texture depth) { g->export_depth = depth; }

void tg_frame_resolve(tg *g) {
  held_send(g);
  if (!g->recording || g->failed || g->resolved) return;
  if (!g->resolved_framebuffer) {
    if (!g->passes_colour[0] && !colour_pass_create(g, VK_SAMPLE_COUNT_1_BIT, &g->passes_colour[0])) return;
    VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = g->passes_colour[0];
    fb.attachmentCount = 1;
    fb.pAttachments = &g->color.view;
    fb.width = g->width;
    fb.height = g->height;
    fb.layers = 1;
    if (!check(g, vkCreateFramebuffer(g->device, &fb, NULL, &g->resolved_framebuffer), "vkCreateFramebuffer")) return;
  }
  main_ensure(g); // (cleared, when nothing was drawn)
  pass_end(g);
  if (g->samples != VK_SAMPLE_COUNT_1_BIT) {
    VkImageMemoryBarrier b[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                                 {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    for (int i = 0; i < 2; i++) {
      b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    b[0].image = g->color_ms.image;
    b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[1].image = g->color.image;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcAccessMask = 0;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageResolve region = {layers, {0, 0, 0}, layers, {0, 0, 0}, {g->width, g->height, 1}};
    vkCmdResolveImage(g->draw_cmd, g->color_ms.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->color.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // (the multisampled colour back as the next frame's pass expects it)
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(g->draw_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 2, b);
  }
  g->resolved = true;
  g->copied = false;
}

bool tg_frame_end(tg *g) {
  held_send(g);
  if (!g->recording) return false;
  VkCommandBuffer cmd = g->draw_cmd;
  main_ensure(g); // (cleared, when nothing was drawn)
  pass_end(g);
  // the frame into the engine's image (resolved first when multisampled)
  VkImageMemoryBarrier b[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                               {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
  for (int i = 0; i < 2; i++) {
    b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  if (g->resolved) {
    b[0].image = g->color.image;
    b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &b[0]);
  } else if (g->samples != VK_SAMPLE_COUNT_1_BIT) {
    b[0].image = g->color_ms.image;
    b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[1].image = g->color.image;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcAccessMask = 0;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageResolve region = {layers, {0, 0, 0}, layers, {0, 0, 0}, {g->width, g->height, 1}};
    vkCmdResolveImage(cmd, g->color_ms.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->color.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b[1]);
  } else {
    b[0].image = g->color.image;
    b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &b[0]);
  }
  VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = g->present_pass;
  pass.framebuffer = g->present_framebuffer;
  pass.renderArea.extent = (VkExtent2D){g->pack ? g->width * 2u : g->width, g->height};
  vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.f, 0.f, (float)g->width, (float)g->height, 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  const VkRect2D sc = {{0, 0}, {g->width, g->height}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
  present_draw(g, g->blit, g->color.view, 0);
  // the depth beside it, packed (the module's single-sampled one; far
  // without: white is the reversed near... so none drawn then)
  if (g->pack && g->export_depth && g->export_depth < g->texture_count && g->textures[g->export_depth].used)
    present_draw(g, g->pack, g->textures[g->export_depth].view, g->width);
  vkCmdEndRenderPass(cmd);
  record_reads(g);
  tg_mark(g, "end");
  g->recording = false;
  if (!check(g, vkEndCommandBuffer(cmd), "vkEndCommandBuffer") || g->failed) {
    vkDeviceWaitIdle(g->device);
    vkResetCommandBuffer(cmd, 0);
    return false;
  }
  VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  // not waited for: the slot is, when it comes round again
  frame_slot *f = &g->slots[g->slot];
  const bool ok = check(g, vkResetFences(g->device, 1, &f->fence), "vkResetFences") &&
                  check(g, vkQueueSubmit(g->queue, 1, &submit, f->fence), "vkQueueSubmit");
  if (ok) f->submitted = true;
  if (ok && g->sync) {
    vkWaitForFences(g->device, 1, &f->fence, VK_TRUE, UINT64_MAX);
    slot_harvest(g, g->slot);
  }
  return ok;
}

// --- the renderer ------------------------------------------------------------------------

static bool create_objects(tg *g, uint32_t family) {
  vkGetPhysicalDeviceMemoryProperties(g->physical, &g->memory);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(g->physical, &props);
  g->ubo_align = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 256;
  g->max_anisotropy = props.limits.maxSamplerAnisotropy;
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(g->physical, &features);
  g->anisotropy_supported = features.samplerAnisotropy; // the engine enables what the device has
  g->anisotropy = 16.f;
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(g->physical, VK_FORMAT_BC3_UNORM_BLOCK, &fp);
  g->bc = features.textureCompressionBC && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);

  const VkFormat depth_formats[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
  g->depth_format = VK_FORMAT_UNDEFINED;
  for (int i = 0; i < 3 && g->depth_format == VK_FORMAT_UNDEFINED; i++) {
    vkGetPhysicalDeviceFormatProperties(g->physical, depth_formats[i], &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) g->depth_format = depth_formats[i];
  }
  if (g->depth_format == VK_FORMAT_UNDEFINED) {
    fail(g, "no depth format");
    return false;
  }
  VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = family;
  if (!check(g, vkCreateCommandPool(g->device, &pool, NULL, &g->command_pool), "vkCreateCommandPool")) return false;
  VkCommandBuffer buffers[1 + TG_FRAMES];
  VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = g->command_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1 + TG_FRAMES;
  if (!check(g, vkAllocateCommandBuffers(g->device, &ai, buffers), "vkAllocateCommandBuffers")) return false;
  g->upload_cmd = buffers[0];
  VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (!check(g, vkCreateFence(g->device, &fence, NULL, &g->fence), "vkCreateFence")) return false;
  for (uint32_t i = 0; i < TG_FRAMES; i++) {
    g->slots[i].cmd = buffers[1 + i];
    if (!check(g, vkCreateFence(g->device, &fence, NULL, &g->slots[i].fence), "vkCreateFence")) return false;
  }
  g->draw_cmd = g->slots[0].cmd;
  g->sync = getenv("TG_SYNC") != NULL;

  VkDescriptorSetLayoutBinding b[TG_MAX_TEXTURES + 1];
  memset(b, 0, sizeof b);
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 1; i <= TG_MAX_TEXTURES; i++) {
    b[i].binding = i;
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sl.bindingCount = TG_MAX_TEXTURES + 1;
  sl.pBindings = b;
  if (!check(g, vkCreateDescriptorSetLayout(g->device, &sl, NULL, &g->set_layout), "vkCreateDescriptorSetLayout"))
    return false;
  VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &g->set_layout;
  if (!check(g, vkCreatePipelineLayout(g->device, &pl, NULL, &g->pipeline_layout), "vkCreatePipelineLayout"))
    return false;
  g->staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  g->uniforms.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  g->dynamic.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  for (uint32_t i = 0; i < TG_FRAMES; i++) {
    g->slots[i].uniforms.usage = g->uniforms.usage;
    g->slots[i].dynamic.usage = g->dynamic.usage;
  }
  if (!main_pass_create(g, false, false, VK_SAMPLE_COUNT_1_BIT, &g->passes[0]) ||
      !main_pass_create(g, true, false, VK_SAMPLE_COUNT_1_BIT, &g->passes_load[0]) ||
      !main_pass_create(g, false, true, VK_SAMPLE_COUNT_1_BIT, &g->pass_copy))
    return false;
  g->pass = g->passes[0];
  g->pass_load = g->passes_load[0];
  g->samples = g->samples_wanted = VK_SAMPLE_COUNT_1_BIT;
  g->samples_supported = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
  const tg_program_desc blit = {.vertex_spirv = k_blit_vert_spv,
                                .vertex_spirv_size = sizeof k_blit_vert_spv,
                                .fragment_spirv = k_blit_frag_spv,
                                .fragment_spirv_size = sizeof k_blit_frag_spv,
                                .uniform_size = 16,
                                .texture_count = 1};
  if (!(g->blit = tg_program_create(g, &blit))) return false;

  // white: what an unused binding samples
  const uint8_t white[6 * 4] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
  const tg_image w2 = {TG_RGBA8, 1, 1, 1, 1, white, 4}, wc = {TG_RGBA8, 1, 1, 1, 6, white, sizeof white};
  return gtex_upload(g, &g->white, &w2) && gtex_upload(g, &g->white_cube, &wc) && upload_done(g);
}

tg *tg_create(void *physical_device, void *device, void *queue, uint32_t queue_family, char *error,
              size_t error_size) {
  tg *g = calloc(1, sizeof *g);
  if (!g) {
    snprintf(error, error_size, "out of memory");
    return NULL;
  }
  g->physical = (VkPhysicalDevice)physical_device;
  g->device = (VkDevice)device;
  g->queue = (VkQueue)queue;
  if (!create_objects(g, queue_family)) {
    snprintf(error, error_size, "%s", g->error);
    tg_destroy(g);
    return NULL;
  }
  return g;
}

void tg_destroy(tg *g) {
  if (!g) return;
  if (g->device) vkDeviceWaitIdle(g->device);
  if (g->readback) vkDestroyBuffer(g->device, g->readback, NULL);
  if (g->readback_memory) vkFreeMemory(g->device, g->readback_memory, NULL);
  for (uint32_t i = 1; i < g->texture_count; i++)
    if (g->textures[i].used) gtex_destroy(g, &g->textures[i]);
  free(g->textures);
  gtex_destroy(g, &g->white);
  gtex_destroy(g, &g->white_cube);
  for (uint32_t i = 0; i < g->sampler_count; i++)
    vkDestroySampler(g->device, g->samplers[i].sampler, NULL);
  tg_program_destroy(g, g->blit);
  frame_release(g);
  present_release(g);
  for (int i = 0; i < 5; i++) {
    if (g->passes[i]) vkDestroyRenderPass(g->device, g->passes[i], NULL);
    if (g->passes_load[i]) vkDestroyRenderPass(g->device, g->passes_load[i], NULL);
    if (g->passes_colour[i]) vkDestroyRenderPass(g->device, g->passes_colour[i], NULL);
  }
  if (g->pass_copy) vkDestroyRenderPass(g->device, g->pass_copy, NULL);
  if (g->pass_float) vkDestroyRenderPass(g->device, g->pass_float, NULL);
  if (g->pass_depth) vkDestroyRenderPass(g->device, g->pass_depth, NULL);
  if (g->present_pass) vkDestroyRenderPass(g->device, g->present_pass, NULL);
  chunks_free(g, &g->staging);
  // (the current slot's are in g's own fields)
  {
    frame_slot *f = &g->slots[g->slot];
    f->uniforms = g->uniforms, f->dynamic = g->dynamic, f->pools = g->pools, f->pool_count = g->pool_count;
  }
  for (uint32_t k = 0; k < TG_FRAMES; k++) {
    frame_slot *f = &g->slots[k];
    chunks_free(g, &f->uniforms);
    chunks_free(g, &f->dynamic);
    for (size_t i = 0; i < f->pool_count; i++)
      vkDestroyDescriptorPool(g->device, f->pools[i], NULL);
    free(f->pools);
    if (f->readback) vkDestroyBuffer(g->device, f->readback, NULL);
    if (f->readback_memory) vkFreeMemory(g->device, f->readback_memory, NULL);
    if (f->fence) vkDestroyFence(g->device, f->fence, NULL);
    if (f->occ) vkDestroyQueryPool(g->device, f->occ, NULL);
    free(f->sets);
  }
  for (uint32_t k = 0; k < g->result_count; k++)
    free(g->results[k].pixels);
  free(g->held.uniforms);
  if (g->pipeline_layout) vkDestroyPipelineLayout(g->device, g->pipeline_layout, NULL);
  if (g->set_layout) vkDestroyDescriptorSetLayout(g->device, g->set_layout, NULL);
  if (g->prof.pool) vkDestroyQueryPool(g->device, g->prof.pool, NULL);
  if (g->fence) vkDestroyFence(g->device, g->fence, NULL);
  if (g->command_pool) vkDestroyCommandPool(g->device, g->command_pool, NULL);
  free(g);
}
