#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "entity_inspector.h"
#include "keybinds.h"
#include "undo_redo.h"
#include <engine/game_host.h>
#include <engine/input_record.h>
#include <plugins/plugin_manager.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>
#include <user_interface/timeline/timeline.h>

struct ui_handler_t {
  struct gfx_handler_t *gfx_handler;
  ImFont *font;
  ImFont *icon_font;

  timeline_state_t timeline;
  keybind_manager_t keybinds;
  undo_manager_t undo_manager;
  plugin_manager_t plugin_manager;
  tas_context_t plugin_context;
  tas_api_t plugin_api;
  entity_inspector_t entity_inspector;

  ImVec2 viewport_window_pos;
  vec2 last_render_pos;
  vec2 recording_mouse_pos;
  bool viewport_focused;
  bool viewport_hovered;

  int current_tick;
  int fps_limit;

  float mouse_sens;
  float mouse_max_distance;
  float lod_bias;
  float bg_color[3];

  // Every editor panel at once. Tab drops it, leaving the level under the menu
  // bar, which is where it is turned back on.
  bool show_ui;
  bool show_timeline_events_window;
  bool show_snippet_editor_window;
  bool focus_snippet_editor_window;
  bool show_effects_window;
  bool focus_effects_window;
  int effects_snippet_id;
  bool vsync;
  bool show_fps;
  bool render_level;
  // Game module the user last worked with. Restored on startup so a project
  // opens under the same game it was authored in.
  char preferred_game_id[32];
  // Per-game editor state is parsed before the timeline/camera exist during
  // startup, then applied as soon as those objects are initialized.
  char configured_camera_mode_id[32];
  bool configured_linked_copy_input;
  // Engine-owned prediction preferences are global within one game rather
  // than project data. The config loader fills this before a timeline exists;
  // every later timeline starts from the same per-game copy.
  prediction_settings_t configured_prediction;

  // The start screen runs in two stages: pick a game, then whatever that game
  // starts a run with. Choosing is only offered here, where nothing is open.
  enum { SPLASH_STAGE_GAME = 0,
         SPLASH_STAGE_START = 1 } splash_stage;

  char recent_projects[10][1024];
  int num_recent_projects;
  char loaded_level_name[128];
  // Full path the current level was loaded from; empty for an in-memory level.
  char loaded_level_path[1024];
  char current_project_path[1024];
  bool has_unsaved_changes;
  bool auto_save_enabled;
  int auto_save_interval_sec;
  double last_auto_save_time;
  bool show_plugin_manager;
  // the splash doubles as the "new project" screen, so it can be raised over a loaded project and
  // dismissed again without touching it
  bool show_splash;
  bool show_new_project_prompt;
};

// `intra` is the interpolation between the previous tick and the current one,
// so a game-directed camera can track what is actually drawn.
void on_camera_update(struct gfx_handler_t *handler, bool hovered, float intra);
bool ui_quick_save(ui_handler_t *ui);
void ui_check_auto_save(ui_handler_t *ui);
struct timeline_state;
void ui_mark_unsaved(ui_handler_t *ui);
void timeline_mark_unsaved(struct timeline_state *ts);
// asks for confirmation when there is unsaved work, otherwise raises the splash screen right away
void ui_request_new_project(ui_handler_t *ui);

void ui_init_config(ui_handler_t *ui);
void camera_init(camera_t *camera);
void ui_init(ui_handler_t *ui, struct gfx_handler_t *gfx_handler);
void ui_render(ui_handler_t *ui);
bool ui_render_late(ui_handler_t *ui);
void ui_post_level_load(ui_handler_t *ui);
void ui_cleanup(ui_handler_t *ui);
void ui_add_recent_project(ui_handler_t *ui, const char *path);
// Draws the active game's own settings, described by the game and rendered here.
void ui_render_game_settings(ui_handler_t *ui);
bool ui_icon_button(ui_handler_t *ui, const char *icon, ImVec2 size);

#endif
