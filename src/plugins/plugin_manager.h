#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include "plugin_api.h"

typedef struct {
  void *handle; // DLL/SO handle
  plugin_info_t info;
  plugin_init_func init;
  plugin_update_func update;
  plugin_shutdown_func shutdown;
  void *data; // plugin-specific data
} loaded_plugin_t;

typedef struct {
  loaded_plugin_t *plugins;
  int count;
  int capacity;
  tas_context_t *context;
  tas_api_t *api;
} plugin_manager_t;

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api);
void plugin_manager_load_all(plugin_manager_t *manager, const char *directory);
void plugin_manager_update_all(plugin_manager_t *manager);
void plugin_manager_shutdown(plugin_manager_t *manager);
void plugin_manager_reload_all(plugin_manager_t *manager, const char *directory);

#endif // PLUGIN_MANAGER_H
