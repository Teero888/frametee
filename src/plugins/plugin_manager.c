#include "plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

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
    printf("PLUG-IN ERROR: Failed to load %s (Error: %lu)\n", path, GetLastError());
#else
    printf("PLUG-IN ERROR: Failed to load %s (Error: %s)\n", path, dlerror());
#endif
    return;
  }

  get_plugin_info_func get_info = (get_plugin_info_func)get_symbol(handle, GET_PLUGIN_INFO_FUNC_NAME);
  plugin_init_func init = (plugin_init_func)get_symbol(handle, GET_PLUGIN_INIT_FUNC_NAME);
  plugin_update_func update = (plugin_update_func)get_symbol(handle, GET_PLUGIN_UPDATE_FUNC_NAME);
  plugin_shutdown_func shutdown = (plugin_shutdown_func)get_symbol(handle, GET_PLUGIN_SHUTDOWN_FUNC_NAME);

  if (!get_info || !init || !update || !shutdown) {
    printf("PLUG-IN ERROR: Plugin %s is missing one or more required functions.\n", path);
    close_library(handle);
    return;
  }

  if (manager->count >= manager->capacity) {
    manager->capacity = manager->capacity == 0 ? 4 : manager->capacity * 2;
    manager->plugins =
        (loaded_plugin_t *)realloc(manager->plugins, manager->capacity * sizeof(loaded_plugin_t));
  }

  loaded_plugin_t *p = &manager->plugins[manager->count++];
  p->handle = handle;
  p->info = get_info();
  p->init = init;
  p->update = update;
  p->shutdown = shutdown;
  p->data = p->init(manager->context, manager->api);

  if (p->data) {
    printf("PLUG-IN: Loaded '%s' v%s by %s.\n", p->info.name, p->info.version, p->info.author);
  } else {
    printf("PLUG-IN ERROR: Plugin '%s' failed to initialize.\n", p->info.name);
    close_library(p->handle);
    manager->count--;
  }
}

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api) {
  manager->plugins = NULL;
  manager->count = 0;
  manager->capacity = 0;
  manager->context = context;
  manager->api = api;
}

void plugin_manager_load_all(plugin_manager_t *manager, const char *directory) {
  printf("PLUG-IN: Scanning for plugins in '%s'...\n", directory);
#ifdef _WIN32
  char search_path[MAX_PATH];
  snprintf(search_path, MAX_PATH, "%s\\*.dll", directory);
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(search_path, &find_data);

  if (find_handle == INVALID_HANDLE_VALUE)
    return;

  do {
    char full_path[MAX_PATH];
    snprintf(full_path, MAX_PATH, "%s\\%s", directory, find_data.cFileName);
    load_plugin(manager, full_path);
  } while (FindNextFile(find_handle, &find_data) != 0);

  FindClose(find_handle);
#else
  DIR *dir = opendir(directory);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    const char *ext = strrchr(entry->d_name, '.');
    if (ext && (strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0)) {
      char full_path[1024];
      snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
      load_plugin(manager, full_path);
    }
  }
  closedir(dir);
#endif
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
    printf("PLUG-IN: Shutting down '%s'...\n", manager->plugins[i].info.name);
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
  printf("PLUG-IN: Reloading all plugins...\n");

  plugin_manager_shutdown(manager);
  plugin_manager_init(manager, context, api);
  plugin_manager_load_all(manager, directory);
}
