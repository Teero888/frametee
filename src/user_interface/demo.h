#ifndef DEMO_H
#define DEMO_H
#include "ddnet_physics/vmath.h"
#include <types.h>

#define MAX_HAMMERHITS_PER_TICK 128
#define MAX_DAMAGE_INDICATORS_PER_TICK 128

struct demo_exporter_t {
  // unix path limit is huge ngl
  char export_path[4096];
  char map_name[128]; // The name of the map as it will be stored in the demo file.
  int num_ticks;

  // read only for the callbacks
  mvec2 hammerhits[MAX_HAMMERHITS_PER_TICK];
  int num_hammerhits;
  mvec2 damage_indicator_positions[MAX_DAMAGE_INDICATORS_PER_TICK];
  float damage_indicator_angles[MAX_DAMAGE_INDICATORS_PER_TICK];
  int num_damage_indicators;
};

int export_to_demo(ui_handler_t *ui, const char *path, const char *map_name, int ticks);
void ui_export_demo(ui_handler_t *ui);

#endif // DEMO_H
