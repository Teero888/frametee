#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "player_info.h"
#include "timeline.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool show_timeline;
  timeline_state_t timeline;
  struct gfx_handler_t *gfx_handler;
} ui_handler_t;

void on_camera_update(struct gfx_handler_t *handler);
void render_players(ui_handler_t *ui);

void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);

#endif
