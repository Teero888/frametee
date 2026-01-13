#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "demo.h"
#include "keybinds.h"
#include "undo_redo.h"
#include <ddnet_physics/gamecore.h>
#include <particles/particle_system.h>
#include <plugins/plugin_manager.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct ui_handler_t {
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
  particle_system_t particle_system;

  SPickup *pickups;
  mvec2 *pickup_positions;
  int *ninja_pickup_indices;
  int num_ninja_pickups;

  ImVec2 viewport_window_pos;
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
  float bg_color[3];
  float prediction_alpha[2]; // 0=own,1=others

  bool show_timeline;
  bool show_prediction;
  bool show_skin_browser;
  bool show_net_events_window;
  bool vsync;
  bool show_fps;
  bool weapons[NUM_WEAPONS];
  bool selecting_override_pos;
};

void on_camera_update(struct gfx_handler_t *handler, bool hovered);
void render_players(ui_handler_t *ui);
void render_pickups(ui_handler_t *ui);
void render_cursor(ui_handler_t *ui);

void ui_init_config(ui_handler_t *ui);
void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
bool ui_render_late(ui_handler_t *ui);
void ui_post_map_load(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);

#endif
