#pragma once
#include <sm64/sm64_game.h>
#include "full_game/scene.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sm64_render_config { uint32_t width, height, mode; const sm64_scene_mario *ghosts; uint32_t ghost_count; bool collision; bool scene_only; float eye[3], target[3], up[3], span, view_proj[16]; } sm64_render_config;
sm64_physics *sm64_ft_physics_create(const sm64_world *, char *, size_t);
sm64_level *sm64_ft_open(const char *, const char *, char *, size_t);
void sm64_ft_level_free(sm64_level *);
sm64_world *sm64_ft_world_new(const sm64_level *, char *, size_t);
void sm64_ft_world_free(sm64_world *);
void sm64_ft_world_set_scratch(sm64_world *, bool);
void sm64_ft_copy(sm64_world *, const sm64_world *);
bool sm64_ft_step(sm64_world *, sm64_input, char *, size_t);
uint32_t sm64_ft_view(const sm64_world *, sm64_view *);
// Process-local render identity: preserved by copy, changed by step/edit/load.
bool sm64_ft_pose(const sm64_world *, sm64_scene_mario *);
uint64_t sm64_ft_revision(const sm64_world *);
// Reads cached simulation camera data; never activates or renders a world.
bool sm64_ft_camera(const sm64_world *, float aspect, sm64_camera *);
// Property indices match the Mario reflection class. Action (2) is read-only.
// Changes are part of the world's replay journal and survive copy/save/load.
bool sm64_ft_set(sm64_world *, uint32_t property, const sm64_view *, char *, size_t);
size_t sm64_ft_save(const sm64_world *, uint8_t *, size_t, char *, size_t);
bool sm64_ft_load(sm64_world *, const uint8_t *, size_t, char *, size_t);
bool sm64_ft_export(const sm64_world *, const char *, char *, size_t);
struct ft_gpu_device;
struct ft_gpu_image;
bool sm64_ft_render(const sm64_world *, const sm64_render_config *, uint8_t *, size_t, char *, size_t);
bool sm64_ft_render_gpu(const sm64_world *, const sm64_render_config *, const struct ft_gpu_device *, const struct ft_gpu_image *, char *, size_t);
// Call while the engine's graphics device is still alive. Simulation survives.
void sm64_ft_release_graphics(void);
const char *sm64_ft_level_name(const sm64_level *);
bool sm64_ft_write_config(const char *, const char *, const char *, const char *, uint32_t, uint32_t, char *, size_t);
#ifdef __cplusplus
}
#endif
