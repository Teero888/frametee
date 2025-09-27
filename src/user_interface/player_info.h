#ifndef PLAYER_INFO_H
#define PLAYER_INFO_H

#include "../renderer/renderer.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char name[32];
  char clan[32];
  int skin; // id
  float color_body[3];
  float color_feet[3];
  bool use_custom_color;
} player_info_t;

typedef struct {
  char name[32];
  int id;
} skin_info_t;

typedef struct {
  int num_skins;
  skin_info_t *skins; // heap array
} skin_manager_t;

void render_player_info(struct gfx_handler_t *h);
void render_skin_manager(gfx_handler_t *h);

void skin_manager_init(skin_manager_t *m);

void skin_manager_free(skin_manager_t *m);
#endif // PLAYER_INFO_H
