#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "timeline.h"
#include <ddnet_map_loader.h>
#include <stdbool.h>

typedef struct {
  bool show_timeline;
  timeline_state_t timeline;
  map_data_t map_data;

  struct gfx_handler_t *gfx_handler;
} ui_handler_t;

void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);

#endif
