#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "demo.h"
#include "entity_inspector.h"
#include "keybinds.h"
#include "undo_redo.h"
#include "online_maps.h"
#include <ddnet_physics/gamecore.h>
#include <particles/particle_system.h>
#include <plugins/plugin_manager.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct ui_handler_t {
  struct gfx_handler_t *gfx_handler;
  ImFont *font;
  ImFont *icon_font;

  timeline_state_t timeline;
  skin_manager_t skin_manager;
  keybind_manager_t keybinds;
  demo_exporter_t demo_exporter;
  undo_manager_t undo_manager;
  plugin_manager_t plugin_manager;
  tas_context_t plugin_context;
  tas_api_t plugin_api;
  particle_system_t particle_system;
  online_map_manager_t online_maps;
  entity_inspector_t entity_inspector;

  SPickup *pickups;
  mvec2 *pickup_positions;
  int *pickup_cooldown_keys;
  int *ninja_pickup_indices;
  int num_ninja_pickups;

  ImVec2 viewport_window_pos;
  vec2 last_render_pos;
  vec2 recording_mouse_pos;
  bool viewport_focused;
  bool viewport_hovered;

  int prediction_length;
  int pos_x;
  int pos_y;
  int current_tick;
  int start_tick;
  int finish_tick;
  int freezetime;
  int reloadtime;
  int health;
  int armor;
  int ammo;
  int weapon;
  EGameMode game_mode;
  int num_pickups;
  int fps_limit;

  float vel_x;
  float vel_y;
  float vel_m;
  float vel_r;
  float race_time;
  float mouse_sens;
  float mouse_max_distance;
  float lod_bias;
  float bg_color[3];
  float prediction_alpha[2]; // 0=own,1=others
  bool center_dot;

  bool show_timeline;
  bool show_prediction;
  bool show_skin_browser;
  bool show_net_events_window;
  bool vsync;
  bool show_fps;
  bool render_map;
  bool render_players;
  bool render_weapons;
  bool render_particles;
  bool render_pickups;
  bool render_hud;
  bool health_and_ammo_hud;
  bool weapons[NUM_WEAPONS];
  bool selecting_override_pos;

  char recent_projects[10][1024];
  int num_recent_projects;
  char loaded_map_name[128];
  // full path the current map was loaded from, empty when it did not come from a file on disk
  char loaded_map_path[1024];
  char current_project_path[1024];
  bool has_unsaved_changes;
  bool auto_save_enabled;
  int auto_save_interval_sec;
  double last_auto_save_time;
  bool show_plugin_manager;
  bool auto_generate_finish_events;
};

void on_camera_update(struct gfx_handler_t *handler, bool hovered);
void render_players(ui_handler_t *ui);
void render_pickups(ui_handler_t *ui);
void render_cursor(ui_handler_t *ui);
bool ui_quick_save(ui_handler_t *ui);
void ui_check_auto_save(ui_handler_t *ui);
struct timeline_state;
void ui_mark_unsaved(ui_handler_t *ui);
void timeline_mark_unsaved(struct timeline_state *ts);

void ui_init_config(ui_handler_t *ui);
void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
bool ui_render_late(ui_handler_t *ui);
void ui_post_map_load(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);
void ui_add_recent_project(ui_handler_t *ui, const char *path);
bool ui_icon_button(ui_handler_t *ui, const char *icon, ImVec2 size);
void ui_export_demo(ui_handler_t *ui);

#endif
