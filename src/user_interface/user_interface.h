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
  skin_manager_t skin_manager;

  ImFont *font;

  // configs
  bool show_prediction;
  int prediction_length; // TODO: make proper ui for this setting

  bool show_skin_manager;

  int pos_x, pos_y;
  float vel_x, vel_y, vel_m, vel_r;
  int freezetime;
  int reloadtime;
  int weapon;
  bool weapons[NUM_WEAPONS];
} ui_handler_t;

void on_camera_update(struct gfx_handler_t *handler, bool hovered);
void render_players(ui_handler_t *ui);

void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
bool ui_render_late(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);

#endif
