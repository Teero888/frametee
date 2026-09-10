#pragma once
#include <frametee/game_abi.h>
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sm64_vulkan sm64_vulkan;

sm64_vulkan *sm64_vulkan_create(const ft_gpu_device *gpu, char *error, size_t error_size);
void sm64_vulkan_destroy(sm64_vulkan *vk);

bool sm64_vulkan_render(sm64_vulkan *vk,
                        void (*render_display_list)(uint32_t, uint32_t),
                        void (*set_render_api)(void *),
                        uint32_t width,
                        uint32_t height,
                        const ft_gpu_image *target_image,
                        bool editor_camera,
                        char *error,
                        size_t error_size);

#ifdef __cplusplus
}
#endif
