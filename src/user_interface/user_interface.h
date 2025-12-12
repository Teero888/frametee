#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include <plugins/plugin_manager.h>
#include "demo.h"
#include "keybinds.h"
#include "player_info.h"
#include "undo_redo.h"
#include <stdbool.h>
#include <stdint.h>

struct ui_handler {
  struct gfx_handler_t *gfx_handler;
  ImFont *font;

  timeline_state_t timeline;
  skin_manager_t skin_manager;
  keybind_manager_t keybinds;
  demo_exporter_t demo_exporter;
  undo_manager_t undo_manager;
  plugin_manager_t plugin_manager;
  tas_context_t plugin_context;
  tas_api_t plugin_api;

  SPickup *pickups;
  mvec2 *pickup_positions;

  vec2 last_render_pos;
  vec2 recording_mouse_pos;

  int prediction_length;
  int pos_x;
  int pos_y;
  int freezetime;
  int reloadtime;
  int weapon;
  int num_pickups;
  int fps_limit;

  float vel_x;
  float vel_y;
  float vel_m;
  float vel_r;
  float mouse_sens;
  float mouse_max_distance;
  float lod_bias;

  bool show_timeline;
  bool show_prediction;
  bool show_skin_browser;
  bool vsync;
  bool show_fps;
  bool weapons[NUM_WEAPONS];
};

void on_camera_update(struct gfx_handler_t *handler, bool hovered);
void render_players(struct ui_handler *ui);
void render_pickups(struct ui_handler *ui);
void render_cursor(struct ui_handler *ui);

void ui_init_config(struct ui_handler *ui);
void ui_init(struct ui_handler *ui, struct gfx_handler_t *gfx_handler);
void ui_render(struct ui_handler *ui);
bool ui_render_late(struct ui_handler *ui);
void ui_post_map_load(struct ui_handler *ui);
void ui_cleanup(struct ui_handler *ui);

#endif
