#ifndef DEMO_H
#define DEMO_H
#include "ddnet_physics/vmath.h"
#include "timeline/timeline_types.h"

#define MAX_HAMMERHITS_PER_TICK 128

typedef struct demo_exporter {
  // unix path limit is huge ngl
  char export_path[4096];
  char map_name[128]; // The name of the map as it will be stored in the demo file.
  int num_ticks;

  // read only for the callbacks
  mvec2 hammerhits[MAX_HAMMERHITS_PER_TICK];
  int num_hammerhits;
} demo_exporter_t;

int export_to_demo(struct ui_handler *ui, const char *path, const char *map_name, int ticks);
void render_demo_window(struct ui_handler *ui);

#endif // DEMO_H
