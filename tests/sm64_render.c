// Draws frames of SM64 offscreen, as the game module does, into PNG files.
//
//   sm64_render_frames ROM POLLS OUT_PREFIX [--size WxH] [--from DX,DY,DZ] [--ghost-back N] FRAME...
//
// POLLS is one u32 controller read per poll in the oracle's order (the first
// ones are read during boot; game frame N reads poll N + sm64_boot_polls() -
// 1), as libs/sm64_physics' sm64_run takes it. Frame N is drawn as the module
// draws it: the world after N - 1 frames, copied, stepped once more with
// drawing. Writes OUT_PREFIX<N>.png for every FRAME. --from draws the 3D scene from a camera
// of its own, DX,DY,DZ from Mario and looking at him, as the module does for
// the editor's cameras. --ghost-back also draws Mario alone from N frames
// before (sm64_set_draw_mario_only) through the same camera on a transparent
// frame, as the module draws another group's Mario: OUT_PREFIX<N>_ghost.png,
// and OUT_PREFIX<N>_with_ghost.png over the frame (tinted, at 70%, as
// data/games/sm64/shaders/ghost.frag does).
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <sm64_physics.h>
#include <vulkan/vulkan.h>

#include "sm64_vulkan.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    *size = (size_t) ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = malloc(*size ? *size : 1);
    if (data && fread(data, 1, *size, file) != *size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

struct gpu {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties memory;
};

static bool gpu_create(struct gpu *gpu) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "sm64_render";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance.pApplicationInfo = &app;
    if (vkCreateInstance(&instance, NULL, &gpu->instance) != VK_SUCCESS) {
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu->instance, &count, NULL);
    VkPhysicalDevice devices[16];
    count = count > 16 ? 16 : count;
    vkEnumeratePhysicalDevices(gpu->instance, &count, devices);
    for (uint32_t d = 0; d < count; ++d) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, NULL);
        VkQueueFamilyProperties properties[16];
        families = families > 16 ? 16 : families;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, properties);
        for (uint32_t f = 0; f < families; ++f) {
            if (properties[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gpu->physical_device = devices[d];
                gpu->family = f;
                const float priority = 1.0f;
                VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
                queue.queueFamilyIndex = f;
                queue.queueCount = 1;
                queue.pQueuePriorities = &priority;
                VkDeviceCreateInfo device = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
                device.queueCreateInfoCount = 1;
                device.pQueueCreateInfos = &queue;
                if (vkCreateDevice(devices[d], &device, NULL, &gpu->device) != VK_SUCCESS) {
                    return false;
                }
                vkGetDeviceQueue(gpu->device, f, 0, &gpu->queue);
                vkGetPhysicalDeviceMemoryProperties(devices[d], &gpu->memory);
                return true;
            }
        }
    }
    return false;
}

static uint32_t memory_type(const struct gpu *gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < gpu->memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (gpu->memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return 0;
}

// The image drawn into, and a host buffer it is copied to.
struct target {
    VkImage image;
    VkDeviceMemory image_memory;
    VkBuffer buffer;
    VkDeviceMemory buffer_memory;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
};

static bool target_create(const struct gpu *gpu, struct target *t, uint32_t width, uint32_t height) {
    VkImageCreateInfo image = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = (VkExtent3D) { width, height, 1 };
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(gpu->device, &image, NULL, &t->image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(gpu->device, t->image, &requirements);
    VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(gpu->device, &allocate, NULL, &t->image_memory) != VK_SUCCESS
        || vkBindImageMemory(gpu->device, t->image, t->image_memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkBufferCreateInfo buffer = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer.size = (VkDeviceSize) width * height * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(gpu->device, &buffer, NULL, &t->buffer) != VK_SUCCESS) {
        return false;
    }
    vkGetBufferMemoryRequirements(gpu->device, t->buffer, &requirements);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(gpu, requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(gpu->device, &allocate, NULL, &t->buffer_memory) != VK_SUCCESS
        || vkBindBufferMemory(gpu->device, t->buffer, t->buffer_memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu->family;
    VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd.commandBufferCount = 1;
    VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(gpu->device, &pool, NULL, &t->pool) != VK_SUCCESS) {
        return false;
    }
    cmd.commandPool = t->pool;
    return vkAllocateCommandBuffers(gpu->device, &cmd, &t->cmd) == VK_SUCCESS
           && vkCreateFence(gpu->device, &fence, NULL, &t->fence) == VK_SUCCESS;
}

static bool target_read(const struct gpu *gpu, struct target *t, uint32_t width, uint32_t height, void *pixels) {
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(t->cmd, &begin);
    VkBufferImageCopy copy = { 0 };
    copy.imageSubresource = (VkImageSubresourceLayers) { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = (VkExtent3D) { width, height, 1 };
    vkCmdCopyImageToBuffer(t->cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buffer, 1, &copy);
    vkEndCommandBuffer(t->cmd);
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &t->cmd;
    vkResetFences(gpu->device, 1, &t->fence);
    if (vkQueueSubmit(gpu->queue, 1, &submit, t->fence) != VK_SUCCESS
        || vkWaitForFences(gpu->device, 1, &t->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    void *mapped;
    if (vkMapMemory(gpu->device, t->buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    memcpy(pixels, mapped, (size_t) width * height * 4);
    vkUnmapMemory(gpu->device, t->buffer_memory);
    return true;
}

// A camera at eye looking at target: 60 degrees vertically, OpenGL's clip
// space, row vectors, X narrowed to 4:3 as f3d.c widens it to the image.
static void camera_at(const float eye[3], const float target[3], float aspect, float out[4][4]) {
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; ++i) f[i] /= l;
    float r[3] = { f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0 }; // f x up
    l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int i = 0; i < 3; ++i) r[i] /= l;
    const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    // View (column vectors).
    const float view[4][4] = { { r[0], r[1], r[2], -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]) },
                               { u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]) },
                               { -f[0], -f[1], -f[2], f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2] },
                               { 0, 0, 0, 1 } };
    const float n = 50.0f, fa = 30000.0f, t = 1.0f / tanf(30.0f * (float) M_PI / 180.0f);
    const float proj[4][4] = { { t / aspect, 0, 0, 0 },
                               { 0, t, 0, 0 },
                               { 0, 0, (fa + n) / (n - fa), 2 * fa * n / (n - fa) },
                               { 0, 0, -1, 0 } };
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += proj[i][k] * view[k][j];
            out[j][i] = v * (i == 0 ? aspect * 0.75f : 1.0f); // transposed: row vectors
        }
    }
}

static int compare_frames(const void *a, const void *b) {
    const long x = *(const long *) a, y = *(const long *) b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: sm64_render_frames ROM POLLS OUT_PREFIX [--size WxH] FRAME...\n");
        return 1;
    }
    uint32_t width = 640, height = 480;
    float from[3];
    bool have_from = false;
    long ghost_back = 0;
    long frames[256];
    int frame_count = 0;
    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%ux%u", &width, &height);
        } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            have_from = sscanf(argv[++i], "%f,%f,%f", &from[0], &from[1], &from[2]) == 3;
        } else if (strcmp(argv[i], "--ghost-back") == 0 && i + 1 < argc) {
            ghost_back = atol(argv[++i]);
        } else if (frame_count < 256) {
            frames[frame_count++] = atol(argv[i]);
        }
    }
    qsort(frames, (size_t) frame_count, sizeof(frames[0]), compare_frames);
    size_t rom_size, polls_size;
    void *rom = read_file(argv[1], &rom_size);
    uint32_t *polls = read_file(argv[2], &polls_size);
    if (!rom || !polls) {
        return 1;
    }
    if (!sm64_load_rom(rom, rom_size)) {
        fprintf(stderr, "sm64_render: %s is not the ROM this library is built for\n", argv[1]);
        return 1;
    }
    const size_t poll_count = polls_size / 4;

    struct gpu gpu = { 0 };
    struct target target = { 0 };
    if (!gpu_create(&gpu) || !target_create(&gpu, &target, width, height)) {
        fprintf(stderr, "sm64_render: no Vulkan device\n");
        return 1;
    }
    char error[256];
    sm64_vulkan *vk = sm64_vulkan_create(gpu.physical_device, gpu.device, gpu.queue, gpu.family, error, sizeof(error));
    if (!vk) {
        fprintf(stderr, "sm64_render: %s\n", error);
        return 1;
    }
    sm64_vulkan *vk_ghost = NULL;
    if (ghost_back > 0) {
        if (!have_from || !(vk_ghost = sm64_vulkan_create(gpu.physical_device, gpu.device, gpu.queue, gpu.family, error,
                                                          sizeof(error)))) {
            fprintf(stderr, "sm64_render: --ghost-back needs --from and Vulkan\n");
            return 1;
        }
        sm64_vulkan_set_transparent(vk_ghost, true);
    }
    unsigned char *pixels = malloc((size_t) width * height * 4);
    unsigned char *ghost_pixels = malloc((size_t) width * height * 4);
    sm64_world *world = sm64_world_create(), *draw = sm64_world_create();
    sm64_world *ghost = sm64_world_create();
    long stepped = 0, ghost_stepped = 0;
    int result = 0;
    for (int f = 0; f < frame_count; ++f) {
        const long frame = frames[f];
        const size_t boot = (size_t) sm64_boot_polls();
        if (frame < 1 || (size_t) frame + boot > poll_count) {
            fprintf(stderr, "sm64_render: no frame %ld\n", frame);
            continue;
        }
        while (stepped < frame - 1) {
            sm64_step(world, polls[stepped + boot]);
            ++stepped;
        }
        sm64_world_copy(draw, world);
        const void *list = sm64_step_draw(draw, polls[frame - 1 + boot]);
        if (!list) {
            fprintf(stderr, "sm64_render: frame %ld drew nothing\n", frame);
            continue;
        }
        float camera[4][4];
        struct sm64_mario_info mario;
        if (have_from && sm64_mario(draw, &mario)) {
            const float eye[3] = { mario.pos[0] + from[0], mario.pos[1] + from[1], mario.pos[2] + from[2] };
            camera_at(eye, mario.pos, (float) width / (float) height, camera);
        }
        if (!sm64_vulkan_draw(vk, list, have_from ? (const float (*)[4]) camera : NULL, target.image, VK_FORMAT_R8G8B8A8_UNORM, width, height,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, error, sizeof(error))
            || !target_read(&gpu, &target, width, height, pixels)) {
            fprintf(stderr, "sm64_render: frame %ld: %s\n", frame, error);
            result = 1;
            continue;
        }
        char path[4096];
        snprintf(path, sizeof(path), "%s%ld.png", argv[3], frame);
        if (!stbi_write_png(path, (int) width, (int) height, 4, pixels, (int) width * 4)) {
            fprintf(stderr, "sm64_render: cannot write %s\n", path);
            result = 1;
        } else {
            printf("%s\n", path);
        }
        if (vk_ghost && frame - ghost_back >= 1) {
            // Mario alone, N frames earlier, from where this frame is seen.
            const long earlier = frame - ghost_back;
            while (ghost_stepped < earlier - 1) {
                sm64_step(ghost, polls[ghost_stepped + boot]);
                ++ghost_stepped;
            }
            sm64_world_copy(draw, ghost);
            sm64_set_draw_mario_only(true);
            const void *ghost_list = sm64_step_draw(draw, polls[earlier - 1 + boot]);
            sm64_set_draw_mario_only(false);
            if (!ghost_list
                || !sm64_vulkan_draw(vk_ghost, ghost_list, (const float (*)[4]) camera, target.image, VK_FORMAT_R8G8B8A8_UNORM,
                                     width, height, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, error, sizeof(error))
                || !target_read(&gpu, &target, width, height, ghost_pixels)) {
                fprintf(stderr, "sm64_render: ghost of frame %ld: %s\n", frame, error);
                result = 1;
                continue;
            }
            snprintf(path, sizeof(path), "%s%ld_ghost.png", argv[3], frame);
            stbi_write_png(path, (int) width, (int) height, 4, ghost_pixels, (int) width * 4);
            printf("%s\n", path);
            // Over the frame: tinted 35% toward orange, at 70% of its alpha.
            const float tint[3] = { 255.f, 150.f, 40.f };
            for (size_t p = 0; p < (size_t) width * height; ++p) {
                const float a = ghost_pixels[p * 4 + 3] / 255.f * 0.7f;
                for (int c = 0; c < 3; ++c) {
                    const float g = ghost_pixels[p * 4 + c] * 0.65f + tint[c] * 0.35f;
                    pixels[p * 4 + c] = (unsigned char) (pixels[p * 4 + c] * (1.f - a) + g * a + 0.5f);
                }
            }
            snprintf(path, sizeof(path), "%s%ld_with_ghost.png", argv[3], frame);
            stbi_write_png(path, (int) width, (int) height, 4, pixels, (int) width * 4);
            printf("%s\n", path);
        }
    }
    sm64_vulkan_destroy(vk_ghost);
    sm64_world_destroy(ghost);
    free(ghost_pixels);
    sm64_vulkan_destroy(vk);
    sm64_world_destroy(world);
    sm64_world_destroy(draw);
    return result;
}
