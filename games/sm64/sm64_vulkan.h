// Draws the display lists libsm64_physics hands over (sm64_step_draw) with
// Vulkan, on a device someone else created: the engine's, or a test's. The
// Fast3D interpreter (fast3d/gfx_pc.c) turns a display list into triangles;
// this is its backend. One renderer at a time: the interpreter is global.
#ifndef SM64_VULKAN_H
#define SM64_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sm64_vulkan sm64_vulkan;

sm64_vulkan *sm64_vulkan_create(VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
                                uint32_t queue_family, char *error, size_t error_size);
void sm64_vulkan_destroy(sm64_vulkan *vk);

// Draws one frame's display list into image (a color attachment of format,
// width x height), cleared first, and leaves it in final_layout. Submits to
// the queue and waits for it: the image is ready when this returns. With a
// camera (row vectors, OpenGL's clip space), the game's 3D scenes are drawn
// through it instead of the game's camera (fast3d/gfx_pc.h, gfx_set_camera).
bool sm64_vulkan_draw(sm64_vulkan *vk, const void *display_list, const float (*camera)[4], VkImage image,
                      VkFormat format, uint32_t width, uint32_t height, VkImageLayout final_layout, char *error,
                      size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
