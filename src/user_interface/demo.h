#ifndef DEMO_H
#define DEMO_H
#include "timeline/timeline_types.h"

typedef struct demo_exporter {
  // unix path limit is huge ngl
  char export_path[4096];
  char map_name[128]; // The name of the map as it will be stored in the demo file.
  int num_ticks;
} demo_exporter_t;

int export_to_demo(ui_handler_t *ui, const char *path, const char *map_name, int ticks);
void render_demo_window(ui_handler_t *ui);

#endif // DEMO_H
