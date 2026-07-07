#include "plugin_manager.h"
#include <logger/logger.h>
#include <system/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *LOG_SOURCE = "PluginManager";

static void load_plugin(plugin_manager_t *manager, const char *path) {
  void *handle = fs_load_library(path);
  if (!handle) {
    log_error(LOG_SOURCE, "Failed to load plugin: %s", path);
    return;
  }

  union {
    void *sym;
    get_plugin_info_func get_info;
    plugin_init_func init;
    plugin_update_func update;
    plugin_shutdown_func shutdown;
  } u;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INFO_FUNC_NAME);
  get_plugin_info_func get_info = u.get_info;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INIT_FUNC_NAME);
  plugin_init_func init = u.init;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_UPDATE_FUNC_NAME);
  plugin_update_func update = u.update;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_SHUTDOWN_FUNC_NAME);
  plugin_shutdown_func shutdown = u.shutdown;

  if (!get_info || !init) {
    log_error(LOG_SOURCE, "Plugin '%s' is missing required symbols.", path);
    fs_free_library(handle);
    return;
  }

  if (manager->count >= manager->capacity) {
    manager->capacity = manager->capacity == 0 ? 4 : manager->capacity * 2;
    manager->plugins = realloc(manager->plugins, manager->capacity * sizeof(loaded_plugin_t));
  }

  loaded_plugin_t *p = &manager->plugins[manager->count++];
  p->handle = handle;
  p->info = get_info();
  p->update = update;
  p->shutdown = shutdown;

  p->data = init(manager->context, manager->api);

  if (p->data) {
    log_info(LOG_SOURCE, "Loaded '%s' v%s by %s.", p->info.name, p->info.version, p->info.author);
  } else {
    log_error(LOG_SOURCE, "Plugin '%s' failed to initialize.", p->info.name);
    fs_free_library(p->handle);
    manager->count--;
  }
}

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api) {
  log_info(LOG_SOURCE, "Initializing plugin system...");
  manager->plugins = NULL;
  manager->count = 0;
  manager->capacity = 0;
  manager->context = context;
  manager->api = api;
}

void plugin_manager_load_all(plugin_manager_t *manager, const char *directory) {
  log_info(LOG_SOURCE, "Scanning for plugins in '%s'...", directory);
  int plugins = 0;
  fs_dir_t *dir = fs_opendir(directory);
  if (!dir) return;

  fs_dirent_t *entry;
  while ((entry = fs_readdir(dir)) != NULL) {
    if (!entry->is_directory) {
      const char *ext = strrchr(entry->name, '.');
      if (ext && (strcmp(ext, ".dll") == 0 || strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0)) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->name);
        load_plugin(manager, full_path);
        ++plugins;
      }
    }
  }
  fs_closedir(dir);
  log_info(LOG_SOURCE, "Loaded %d plugin%s.", plugins, plugins != 1 ? "s" : "");
}

void plugin_manager_update_all(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    if (manager->plugins[i].update && manager->plugins[i].data) {
      manager->plugins[i].update(manager->plugins[i].data);
    }
  }
}

void plugin_manager_shutdown(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    log_info(LOG_SOURCE, "Shutting down '%s'...", manager->plugins[i].info.name);
    if (manager->plugins[i].shutdown && manager->plugins[i].data) {
      manager->plugins[i].shutdown(manager->plugins[i].data);
    }
    fs_free_library(manager->plugins[i].handle);
  }
  free(manager->plugins);
  manager->plugins = NULL;
  manager->count = 0;
  manager->capacity = 0;
}

void plugin_manager_reload_all(plugin_manager_t *manager, const char *directory) {
  tas_context_t *context = manager->context;
  tas_api_t *api = manager->api;
  log_info(LOG_SOURCE, "Reloading all plugins...");

  plugin_manager_shutdown(manager);
  plugin_manager_init(manager, context, api);
  plugin_manager_load_all(manager, directory);
}
