#ifndef PHYSICS_H
#define PHYSICS_H

#include <ddnet_physics/gamecore.h>
#include <stddef.h>
#include <types.h>

struct physics_handler_t {
  SCollision collision;
  STeeGrid grid;
  SWorldCore world; // initial world
  SConfig config;
  bool loaded;
};

void physics_init(physics_handler_t *h, const char *path, EGameMode game_mode);
void physics_init_from_memory(physics_handler_t *h, const unsigned char *map_buffer, size_t size, EGameMode game_mode);
void physics_tick(physics_handler_t *h);
void physics_free(physics_handler_t *h);
float physics_character_race_time(const SCharacterCore *character, float game_tick);
bool physics_pickup_on_cooldown(const SWorldCore *world, int character_id, int cooldown_key);

#endif // PHYISCS_H
