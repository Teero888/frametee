#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include "plugin_api.h"
#include <types.h>
#include <stdbool.h>

typedef enum {
  PLUGIN_STATUS_UNLOADED = 0,
  PLUGIN_STATUS_LOADED,
  PLUGIN_STATUS_ERROR,
  // Written for a different game than the one that is active. The plugin stays
  // listed so the user can see why it is idle, but it is never initialized.
  PLUGIN_STATUS_WRONG_GAME
} plugin_status_t;

typedef const char *(*plugin_game_id_func)(void);

struct loaded_plugin_t {
  char path[1024];
  char key[128]; // extracted name for config key (e.g. "physics_profiler")
  void *handle; // DLL/SO handle
  
  // Owned copy of info strings to survive dll unload
  char info_name[128];
  char info_author[128];
  char info_version[64];
  char info_description[512];
  
  // Game this plugin is written for, empty when it is global. Owned copy, since
  // the string lives in the library and the library may be unloaded.
  char game_id[32];

  plugin_info_t info; // keeps pointers to the owned strings above
  plugin_init_func init;
  plugin_update_func update;
  plugin_shutdown_func shutdown;
  plugin_show_ui_func show_ui;
  void *data; // plugin-specific data
  plugin_status_t status;
  bool enabled;
  char error_msg[1048];
};

struct plugin_manager_t {
  loaded_plugin_t *plugins;
  int count;
  int capacity;
  tas_context_t *context;
  tas_api_t *api;
  ui_handler_t *host_ui;
  char directory[1024];
};

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api, ui_handler_t *host_ui);
void plugin_manager_load_all(plugin_manager_t *manager, const char *directory);
void plugin_manager_update_all(plugin_manager_t *manager);
void plugin_manager_shutdown(plugin_manager_t *manager);
void plugin_manager_reload_all(plugin_manager_t *manager, const char *directory);

bool plugin_manager_load_plugin(plugin_manager_t *manager, int index);
void plugin_manager_unload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_reload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_toggle_plugin(plugin_manager_t *manager, int index);
void plugin_manager_render_ui(plugin_manager_t *manager, bool *p_open);

// True when the plugin may run under the currently active game: either it is
// global, or its game_id matches. Game-specific plugins are skipped rather than
// failed, because switching back makes them valid again.
bool plugin_manager_matches_active_game(const plugin_manager_t *manager, const loaded_plugin_t *plugin);
// Loads plugins that became valid for the newly active game and unloads the
// ones that no longer apply. Call after the active game changes.
void plugin_manager_on_game_changed(plugin_manager_t *manager);

#endif // PLUGIN_MANAGER_H
