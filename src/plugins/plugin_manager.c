#include "plugin_manager.h"
#include "../logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

static const char *LOG_SOURCE = "PluginManager";

#ifdef _WIN32
void *open_library(const char *path) { return LoadLibrary(path); }
void *get_symbol(void *handle, const char *name) { return GetProcAddress((HMODULE)handle, name); }
void close_library(void *handle) { FreeLibrary((HMODULE)handle); }
#else
void *open_library(const char *path) { return dlopen(path, RTLD_LAZY); }
void *get_symbol(void *handle, const char *name) { return dlsym(handle, name); }
void close_library(void *handle) { dlclose(handle); }
#endif

static void load_plugin(plugin_manager_t *manager, const char *path) {
  void *handle = open_library(path);
  if (!handle) {
#ifdef _WIN32
    log_error(LOG_SOURCE, "Failed to load %s (Error: %lu)", path, GetLastError());
#else
    log_error(LOG_SOURCE, "Failed to load %s (Error: %s)", path, dlerror());
#endif
    return;
  }

  get_plugin_info_func get_info = (get_plugin_info_func)get_symbol(handle, GET_PLUGIN_INFO_FUNC_NAME);
  plugin_init_func init = (plugin_init_func)get_symbol(handle, GET_PLUGIN_INIT_FUNC_NAME);
  plugin_update_func update = (plugin_update_func)get_symbol(handle, GET_PLUGIN_UPDATE_FUNC_NAME);
  plugin_shutdown_func shutdown = (plugin_shutdown_func)get_symbol(handle, GET_PLUGIN_SHUTDOWN_FUNC_NAME);

  if (!get_info || !init || !update || !shutdown) {
    log_error(LOG_SOURCE, "Plugin '%s' is missing one or more required functions.", path);
    close_library(handle);
    return;
  }

  if (manager->count >= manager->capacity) {
    manager->capacity = manager->capacity == 0 ? 4 : manager->capacity * 2;
    manager->plugins = (loaded_plugin_t *)realloc(manager->plugins, manager->capacity * sizeof(loaded_plugin_t));
  }

  loaded_plugin_t *p = &manager->plugins[manager->count++];
  p->handle = handle;
  p->info = get_info();
  p->init = init;
  p->update = update;
  p->shutdown = shutdown;
  p->data = p->init(manager->context, manager->api);

  if (p->data) {
    log_info(LOG_SOURCE, "Loaded '%s' v%s by %s.", p->info.name, p->info.version, p->info.author);
  } else {
    log_error(LOG_SOURCE, "Plugin '%s' failed to initialize.", p->info.name);
    close_library(p->handle);
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
#ifdef _WIN32
  char search_path[MAX_PATH];
  snprintf(search_path, MAX_PATH, "%s\\*.dll", directory);
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(search_path, &find_data);

  if (find_handle == INVALID_HANDLE_VALUE) return;

  do {
    char full_path[MAX_PATH];
    snprintf(full_path, MAX_PATH, "%s\\%s", directory, find_data.cFileName);
    load_plugin(manager, full_path);
    ++plugins;
  } while (FindNextFile(find_handle, &find_data) != 0);

  FindClose(find_handle);
#else
  DIR *dir = opendir(directory);
  if (!dir) return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    const char *ext = strrchr(entry->d_name, '.');
    if (ext && (strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0)) {
      char full_path[1024];
      snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
      load_plugin(manager, full_path);
      ++plugins;
    }
  }
  closedir(dir);
#endif
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
    close_library(manager->plugins[i].handle);
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
