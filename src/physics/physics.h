#ifndef PHYSICS_H
#define PHYISCS_H

#include "../../libs/ddnet_physics/include/gamecore.h"

typedef struct {
  SCollision collision;
  SWorldCore world;
  SConfig config;
} physics_handler_t;

typedef physics_handler_t ph_t;

void physics_init(ph_t *h, const char *path);
void physics_tick(ph_t *h);
void physics_free(ph_t *h);

#endif // PHYISCS_H
