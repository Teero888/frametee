#ifndef PLAYER_INFO_H
#define PLAYER_INFO_H

#include <stdbool.h>
#include <stdint.h>
typedef struct timeline_state_t timeline_state_t;

typedef struct {
  char name[32];
  char clan[32];
  char skin[32];
  float color[3];
  bool use_custom_color;
} player_info_t;

void render_player_info(timeline_state_t *ts);

#endif // PLAYER_INFO_H
