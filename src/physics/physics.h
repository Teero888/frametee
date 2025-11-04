#ifndef PHYSICS_H
#define PHYSICS_H

#include <ddnet_physics/gamecore.h>
#include <stddef.h>

typedef struct {
  SCollision collision;
  STeeGrid grid;
  SWorldCore world; // initial world
  SConfig config;
  bool loaded;
} physics_handler_t;

typedef physics_handler_t ph_t;

void physics_init(ph_t *h, const char *path);
void physics_init_from_memory(ph_t *h, const unsigned char *map_buffer, size_t size);
void physics_tick(ph_t *h);
void physics_free(ph_t *h);

#endif // PHYISCS_H
