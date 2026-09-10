#ifndef FRAMETEE_SM64_PHYSICS_H
#define FRAMETEE_SM64_PHYSICS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sm64_input {
  uint16_t buttons;
  int8_t stick_x, stick_y;
} sm64_input;
typedef struct sm64_view {
  float pos[3], vel[3];
  uint32_t action;
  int32_t health;
  uint32_t frame;
  bool valid;
} sm64_view;

typedef struct sm64_physics sm64_physics;
typedef struct sm64_checkpoint sm64_checkpoint;
// Each instance owns a separate native image. One thread owns an instance;
// different instances can step concurrently, including alongside the editor.
// step points directly into that image: no editor API, locks, history, state
// capture, or graphics-device work. view is live and updated by step/restore.
// Keep the instance alive while using its function pointers and checkpoints.
struct sm64_physics {
  void (*step)(sm64_input input);
  const sm64_view *view;
  // Native MarioState, for plugins built against the matching SM64EX headers.
  void *mario;
  sm64_checkpoint *(*capture)(sm64_physics *, char *error, size_t error_size);
  // Checkpoints belong to their instance; a foreign checkpoint is rejected.
  bool (*restore)(sm64_physics *, const sm64_checkpoint *, char *error, size_t error_size);
  void (*free_checkpoint)(sm64_checkpoint *);
  void (*destroy)(sm64_physics *);
  void *owner;
};
#ifdef __cplusplus
}
#endif
#endif
