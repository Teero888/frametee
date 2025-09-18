#ifndef PLAYER_INFO_H
#define PLAYER_INFO_H

#include "../renderer/renderer.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char name[32];
  char clan[32];
  int skin;
  float color[3];
  bool use_custom_color;
} player_info_t;

void render_player_info(struct gfx_handler_t *h);

#endif // PLAYER_INFO_H
