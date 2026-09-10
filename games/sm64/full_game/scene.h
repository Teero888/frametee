#pragma once
#include <stdint.h>
#include <stdbool.h>

// Column-major matrices. Native SM64 uses OpenGL clip coordinates; the bridge
// converts the engine's reversed Vulkan depth and aspect correction at its edge.
typedef struct sm64_scene_camera {
    float view[16], projection[16], eye[3], target[3];
    bool valid;
} sm64_scene_camera;

struct sm64_scene_mario;
typedef struct sm64_scene_config {
    uint32_t mode, width, height;
    float view[16], projection[16], eye[3], target[3];
    // Appended fields preserve the original scene-hook ABI for older locked
    // runtimes. They are ignored by those runtimes.
    const struct sm64_scene_mario *ghosts;
    uint32_t ghost_count;
    bool scene_only;
} sm64_scene_config;

// Pointer-free pose that survives switching the native world's globals.
typedef struct sm64_scene_mario {
    float pos[3], scale[3];
    int16_t angle[3], anim_id, anim_frame;
    int32_t anim_accel;
    uint8_t area;
    bool valid;
} sm64_scene_mario;
