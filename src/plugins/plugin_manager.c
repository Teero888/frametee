#include "plugin_manager.h"
#include <logger/logger.h>
#include <system/fs.h>
#include <system/include_cimgui.h>
#include <system/config.h>
#include <tomlc17.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *LOG_SOURCE = "PluginManager";

static const char *safe_strcasestr(const char *haystack, const char *needle) {
  if (!haystack || !needle) return NULL;
  if (!*needle) return haystack;
  
  for (; *haystack; haystack++) {
    if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
      const char *h, *n;
      for (h = haystack, n = needle; *h && *n; h++, n++) {
        if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) break;
      }
      if (!*n) return haystack;
    }
  }
  return NULL;
}

static void get_plugin_key(const char *path, char *key, size_t key_size) {
  const char *filename = strrchr(path, '/');
#ifdef _WIN32
  const char *win_slash = strrchr(path, '\\');
  if (win_slash && (!filename || win_slash > filename)) {
    filename = win_slash;
  }
#endif
  if (filename) {
    filename++;
  } else {
    filename = path;
  }

  size_t i = 0;
  while (filename[i] && filename[i] != '.' && i < key_size - 1) {
    key[i] = filename[i];
    i++;
  }
  key[i] = '\0';
}

static bool load_metadata_temp(loaded_plugin_t *p) {
  void *handle = fs_load_library(p->path);
  if (!handle) {
    return false;
  }

  union {
    void *sym;
    get_plugin_info_func get_info;
  } u;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INFO_FUNC_NAME);
  if (!u.get_info) {
    fs_free_library(handle);
    return false;
  }

  plugin_info_t raw_info = u.get_info();
  strncpy(p->info_name, raw_info.name ? raw_info.name : "", sizeof(p->info_name) - 1);
  strncpy(p->info_author, raw_info.author ? raw_info.author : "", sizeof(p->info_author) - 1);
  strncpy(p->info_version, raw_info.version ? raw_info.version : "", sizeof(p->info_version) - 1);
  strncpy(p->info_description, raw_info.description ? raw_info.description : "", sizeof(p->info_description) - 1);

  p->info.name = p->info_name;
  p->info.author = p->info_author;
  p->info.version = p->info_version;
  p->info.description = p->info_description;

  fs_free_library(handle);
  return true;
}

bool plugin_manager_load_plugin(plugin_manager_t *manager, int index) {
  loaded_plugin_t *p = &manager->plugins[index];
  if (p->status == PLUGIN_STATUS_LOADED) return true;

  void *handle = fs_load_library(p->path);
  if (!handle) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "Failed to load library: %s", p->path);
    log_error(LOG_SOURCE, "Failed to load plugin: %s", p->path);
    return false;
  }

  union {
    void *sym;
    get_plugin_info_func get_info;
    plugin_init_func init;
    plugin_update_func update;
    plugin_shutdown_func shutdown;
    plugin_show_ui_func show_ui;
  } u;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INFO_FUNC_NAME);
  get_plugin_info_func get_info = u.get_info;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INIT_FUNC_NAME);
  p->init = u.init;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_UPDATE_FUNC_NAME);
  p->update = u.update;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_SHUTDOWN_FUNC_NAME);
  p->shutdown = u.shutdown;

  u.sym = fs_get_symbol(handle, "plugin_show_ui");
  p->show_ui = u.show_ui;

  if (!get_info || !p->init) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "Plugin missing required symbols.");
    log_error(LOG_SOURCE, "Plugin '%s' is missing required symbols.", p->path);
    fs_free_library(handle);
    return false;
  }

  p->handle = handle;

  plugin_info_t raw_info = get_info();
  strncpy(p->info_name, raw_info.name ? raw_info.name : "", sizeof(p->info_name) - 1);
  strncpy(p->info_author, raw_info.author ? raw_info.author : "", sizeof(p->info_author) - 1);
  strncpy(p->info_version, raw_info.version ? raw_info.version : "", sizeof(p->info_version) - 1);
  strncpy(p->info_description, raw_info.description ? raw_info.description : "", sizeof(p->info_description) - 1);

  p->info.name = p->info_name;
  p->info.author = p->info_author;
  p->info.version = p->info_version;
  p->info.description = p->info_description;

  p->data = p->init(manager->context, manager->api);

  if (p->data) {
    p->status = PLUGIN_STATUS_LOADED;
    p->error_msg[0] = '\0';
    log_info(LOG_SOURCE, "Loaded '%s' v%s by %s.", p->info.name, p->info.version, p->info.author);
    return true;
  } else {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "plugin_init returned NULL.");
    log_error(LOG_SOURCE, "Plugin '%s' failed to initialize.", p->info.name);
    fs_free_library(p->handle);
    p->handle = NULL;
    return false;
  }
}

void plugin_manager_unload_plugin(plugin_manager_t *manager, int index) {
  loaded_plugin_t *p = &manager->plugins[index];
  if (p->status != PLUGIN_STATUS_LOADED) return;

  log_info(LOG_SOURCE, "Shutting down '%s'...", p->info.name);
  if (p->shutdown && p->data) {
    p->shutdown(p->data);
  }
  if (p->handle) {
    fs_free_library(p->handle);
    p->handle = NULL;
  }
  p->data = NULL;
  p->status = PLUGIN_STATUS_UNLOADED;
}

void plugin_manager_reload_plugin(plugin_manager_t *manager, int index) {
  plugin_manager_unload_plugin(manager, index);
  plugin_manager_load_plugin(manager, index);
}

void plugin_manager_toggle_plugin(plugin_manager_t *manager, int index) {
  loaded_plugin_t *p = &manager->plugins[index];
  p->enabled = !p->enabled;
  if (p->enabled) {
    plugin_manager_load_plugin(manager, index);
  } else {
    plugin_manager_unload_plugin(manager, index);
  }
  config_save(manager->context->ui_handler);
}

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api) {
  log_info(LOG_SOURCE, "Initializing plugin system...");
  manager->plugins = NULL;
  manager->count = 0;
  manager->capacity = 0;
  manager->context = context;
  manager->api = api;
  manager->directory[0] = '\0';
}

void plugin_manager_load_all(plugin_manager_t *manager, const char *directory) {
  strncpy(manager->directory, directory, sizeof(manager->directory) - 1);
  manager->directory[sizeof(manager->directory) - 1] = '\0';

  log_info(LOG_SOURCE, "Scanning for plugins in '%s'...", directory);

  // Read config.toml to see which plugins are enabled
  char config_path[1024];
  if (fs_get_config_dir(config_path, sizeof(config_path))) {
    char sep_str[2] = {PATH_SEP, '\0'};
    strncat(config_path, sep_str, sizeof(config_path) - strlen(config_path) - 1);
    strncat(config_path, "config.toml", sizeof(config_path) - strlen(config_path) - 1);
  } else {
    strncpy(config_path, "config.toml", sizeof(config_path) - 1);
  }

  FILE *fp = fs_open(config_path, "r");
  toml_result_t res = {0};
  bool has_toml = false;
  if (fp) {
    res = toml_parse_file(fp);
    fclose(fp);
    if (res.ok) {
      has_toml = true;
    }
  }

  toml_datum_t plugins_table = {0};
  if (has_toml) {
    plugins_table = toml_get(res.toptab, "plugins");
  }

  fs_dir_t *dir = fs_opendir(directory);
  if (!dir) {
    if (has_toml) toml_free(res);
    return;
  }

  fs_dirent_t *entry;
  while ((entry = fs_readdir(dir)) != NULL) {
    if (!entry->is_directory) {
      const char *ext = strrchr(entry->name, '.');
      if (ext && (strcmp(ext, ".dll") == 0 || strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0)) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->name);

        char key[128];
        get_plugin_key(full_path, key, sizeof(key));

        // 1. Check if we already have it in the list (by path)
        int index = -1;
        for (int i = 0; i < manager->count; ++i) {
          if (strcmp(manager->plugins[i].path, full_path) == 0) {
            index = i;
            break;
          }
        }

        // 2. If not found, allocate / grow array
        if (index == -1) {
          if (manager->count >= manager->capacity) {
            manager->capacity = manager->capacity == 0 ? 4 : manager->capacity * 2;
            loaded_plugin_t *new_plugins = realloc(manager->plugins, manager->capacity * sizeof(loaded_plugin_t));
            if (new_plugins) {
              manager->plugins = new_plugins;
              // Fix internal pointers after reallocation
              for (int k = 0; k < manager->count; ++k) {
                manager->plugins[k].info.name = manager->plugins[k].info_name;
                manager->plugins[k].info.author = manager->plugins[k].info_author;
                manager->plugins[k].info.version = manager->plugins[k].info_version;
                manager->plugins[k].info.description = manager->plugins[k].info_description;
              }
            }
          }
          index = manager->count++;
          memset(&manager->plugins[index], 0, sizeof(loaded_plugin_t));
          snprintf(manager->plugins[index].path, sizeof(manager->plugins[index].path), "%s", full_path);
          snprintf(manager->plugins[index].key, sizeof(manager->plugins[index].key), "%s", key);
          manager->plugins[index].status = PLUGIN_STATUS_UNLOADED;
        }

        loaded_plugin_t *p = &manager->plugins[index];

        // 3. Determine if it is enabled in config
        bool enabled = false;
        if (has_toml && plugins_table.type == TOML_TABLE) {
          toml_datum_t val = toml_get(plugins_table, key);
          if (val.type == TOML_BOOLEAN) {
            enabled = val.u.boolean;
          }
        }
        p->enabled = enabled;

        // 4. Load it or just get its metadata
        if (enabled) {
          plugin_manager_load_plugin(manager, index);
        } else {
          plugin_manager_unload_plugin(manager, index);
          if (p->info_name[0] == '\0') {
            load_metadata_temp(p);
          }
        }
      }
    }
  }
  fs_closedir(dir);

  // Re-bind all metadata pointers to ensure they point to the correct, final allocated memory addresses.
  for (int i = 0; i < manager->count; ++i) {
    manager->plugins[i].info.name = manager->plugins[i].info_name;
    manager->plugins[i].info.author = manager->plugins[i].info_author;
    manager->plugins[i].info.version = manager->plugins[i].info_version;
    manager->plugins[i].info.description = manager->plugins[i].info_description;
  }

  if (has_toml) {
    toml_free(res);
  }
}

void plugin_manager_update_all(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    if (manager->plugins[i].status == PLUGIN_STATUS_LOADED && manager->plugins[i].update && manager->plugins[i].data) {
      manager->plugins[i].update(manager->plugins[i].data);
    }
  }
}

void plugin_manager_shutdown(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    if (manager->plugins[i].status == PLUGIN_STATUS_LOADED) {
      log_info(LOG_SOURCE, "Shutting down '%s'...", manager->plugins[i].info.name);
      if (manager->plugins[i].shutdown && manager->plugins[i].data) {
        manager->plugins[i].shutdown(manager->plugins[i].data);
      }
      fs_free_library(manager->plugins[i].handle);
    }
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

void plugin_manager_render_ui(plugin_manager_t *manager, bool *p_open) {
  if (!*p_open) return;

  igSetNextWindowSize((ImVec2){800, 500}, ImGuiCond_FirstUseEver);

  if (igBegin("Plugin Manager", p_open, 0)) {
    static int selected_index = -1;
    static char filter[128] = "";

    if (selected_index >= manager->count) {
      selected_index = manager->count > 0 ? 0 : -1;
    }

    if (igButton("Scan for Plugins", (ImVec2){0, 0})) {
      plugin_manager_load_all(manager, manager->directory);
    }
    igSameLine(0, 5);
    if (igButton("Reload All", (ImVec2){0, 0})) {
      plugin_manager_reload_all(manager, manager->directory);
    }
    
    igSameLine(0, 15);
    igText("Search:");
    igSameLine(0, 5);
    igInputText("##Filter", filter, sizeof(filter), 0, NULL, NULL);

    igSeparator();

    if (igBeginTable("LayoutTable", 2, ImGuiTableFlags_Resizable, (ImVec2){0, 0}, 0.0f)) {
      igTableSetupColumn("ListColumn", ImGuiTableColumnFlags_WidthStretch, 0.6f, 0);
      igTableSetupColumn("DetailColumn", ImGuiTableColumnFlags_WidthStretch, 0.4f, 0);
      igTableNextRow(0, 0.0f);

      // LEFT COLUMN: Plugins list
      igTableSetColumnIndex(0);
      if (igBeginChild_Str("ListChild", (ImVec2){0, 0}, true, 0)) {
        if (igBeginTable("PluginsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, (ImVec2){0, 0}, 0.0f)) {
          igTableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
          igTableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 60.0f, 0);
          igTableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
          igTableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
          igTableHeadersRow();

          for (int i = 0; i < manager->count; ++i) {
            loaded_plugin_t *p = &manager->plugins[i];
            
            if (filter[0] != '\0') {
              bool match = false;
              if (p->info.name && safe_strcasestr(p->info.name, filter)) match = true;
              if (p->info.description && safe_strcasestr(p->info.description, filter)) match = true;
              if (p->key[0] != '\0' && safe_strcasestr(p->key, filter)) match = true;
              if (!match) continue;
            }

            igTableNextRow(0, 0.0f);
            igTableSetColumnIndex(0);
            
            bool selected = (selected_index == i);
            char label[256];
            snprintf(label, sizeof(label), "%s##%d", (p->info.name && p->info.name[0]) ? p->info.name : p->key, i);
            if (igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){0, 0})) {
              selected_index = i;
            }

            igTableSetColumnIndex(1);
            igText("%s", (p->info.version && p->info.version[0]) ? p->info.version : "-");

            igTableSetColumnIndex(2);
            igText("%s", (p->info.author && p->info.author[0]) ? p->info.author : "-");

            igTableSetColumnIndex(3);
            if (p->status == PLUGIN_STATUS_LOADED) {
              igTextColored((ImVec4){0.2f, 0.8f, 0.2f, 1.0f}, "Active");
            } else if (p->status == PLUGIN_STATUS_UNLOADED) {
              igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Disabled");
            } else {
              igTextColored((ImVec4){0.9f, 0.2f, 0.2f, 1.0f}, "Error");
              if (igIsItemHovered(0)) {
                igSetTooltip("%s", p->error_msg);
              }
            }
          }
          igEndTable();
        }
        igEndChild();
      }

      // RIGHT COLUMN: Selected details
      igTableSetColumnIndex(1);
      if (igBeginChild_Str("DetailChild", (ImVec2){0, 0}, true, 0)) {
        if (selected_index >= 0 && selected_index < manager->count) {
          loaded_plugin_t *p = &manager->plugins[selected_index];
          
          igText("Plugin Details");
          igSeparator();
          
          igPushTextWrapPos(0.0f);
          
          igTextColored((ImVec4){0.9f, 0.9f, 0.9f, 1.0f}, "%s", (p->info.name && p->info.name[0]) ? p->info.name : p->key);
          
          igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Key: %s", p->key);
          igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Version: %s", (p->info.version && p->info.version[0]) ? p->info.version : "Unknown");
          igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Author: %s", (p->info.author && p->info.author[0]) ? p->info.author : "Unknown");
          igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Path: %s", p->path);
          
          igSeparator();
          igText("Description:");
          igTextColored((ImVec4){0.8f, 0.8f, 0.8f, 1.0f}, "%s", (p->info.description && p->info.description[0]) ? p->info.description : "No description provided.");
          
          igSeparator();
          igText("Status:");
          igSameLine(0, 5);
          if (p->status == PLUGIN_STATUS_LOADED) {
            igTextColored((ImVec4){0.2f, 0.8f, 0.2f, 1.0f}, "Active (Loaded)");
          } else if (p->status == PLUGIN_STATUS_UNLOADED) {
            igTextColored((ImVec4){0.6f, 0.6f, 0.6f, 1.0f}, "Disabled (Unloaded)");
          } else {
            igTextColored((ImVec4){0.9f, 0.2f, 0.2f, 1.0f}, "Error");
            igTextColored((ImVec4){0.9f, 0.4f, 0.4f, 1.0f}, "Reason: %s", p->error_msg);
          }
          
          igSeparator();
          igPopTextWrapPos();

          if (p->status == PLUGIN_STATUS_LOADED) {
            if (igButton("Disable Plugin", (ImVec2){0, 0})) {
              plugin_manager_toggle_plugin(manager, selected_index);
            }
            igSameLine(0, 5);
            if (igButton("Reload", (ImVec2){0, 0})) {
              plugin_manager_reload_plugin(manager, selected_index);
            }
            if (p->show_ui) {
              igSameLine(0, 5);
              if (igButton("Open Settings", (ImVec2){0, 0})) {
                p->show_ui(p->data);
              }
            }
          } else if (p->status == PLUGIN_STATUS_UNLOADED) {
            if (igButton("Enable Plugin", (ImVec2){0, 0})) {
              plugin_manager_toggle_plugin(manager, selected_index);
            }
          } else { // Error
            if (igButton("Retry Load", (ImVec2){0, 0})) {
              plugin_manager_load_plugin(manager, selected_index);
            }
            igSameLine(0, 5);
            if (p->enabled) {
              if (igButton("Disable", (ImVec2){0, 0})) {
                p->enabled = false;
                p->status = PLUGIN_STATUS_UNLOADED;
                config_save(manager->context->ui_handler);
              }
            } else {
              if (igButton("Enable (Retry)", (ImVec2){0, 0})) {
                p->enabled = true;
                plugin_manager_load_plugin(manager, selected_index);
                config_save(manager->context->ui_handler);
              }
            }
          }
        } else {
          igTextDisabled("Select a plugin from the list to view its details.");
        }
        igEndChild();
      }
      igEndTable();
    }
  }
  igEnd();
}
