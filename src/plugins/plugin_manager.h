#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include "plugin_api.h"
#include <types.h>
#include <stdbool.h>

typedef enum {
  PLUGIN_STATUS_UNLOADED = 0,
  PLUGIN_STATUS_LOADED,
  PLUGIN_STATUS_ERROR
} plugin_status_t;

struct loaded_plugin_t {
  char path[1024];
  char key[128]; // extracted name for config key (e.g. "physics_profiler")
  void *handle; // DLL/SO handle
  
  // Owned copy of info strings to survive dll unload
  char info_name[128];
  char info_author[128];
  char info_version[64];
  char info_description[512];
  
  plugin_info_t info; // keeps pointers to the owned strings above
  plugin_init_func init;
  plugin_update_func update;
  plugin_shutdown_func shutdown;
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
  char directory[1024];
};

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api);
void plugin_manager_load_all(plugin_manager_t *manager, const char *directory);
void plugin_manager_update_all(plugin_manager_t *manager);
void plugin_manager_shutdown(plugin_manager_t *manager);
void plugin_manager_reload_all(plugin_manager_t *manager, const char *directory);

bool plugin_manager_load_plugin(plugin_manager_t *manager, int index);
void plugin_manager_unload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_reload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_toggle_plugin(plugin_manager_t *manager, int index);
void plugin_manager_render_ui(plugin_manager_t *manager, bool *p_open);

#endif // PLUGIN_MANAGER_H
