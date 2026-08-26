#include "plugin_manager.h"
#include <frametee/icons.h>
#include <logger/logger.h>
#include <stdbool.h>
#include <ctype.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/config.h>
#include <system/fs.h>
#include <system/include_cimgui.h>
#include <tomlc17.h>

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

// A plugin without the optional symbol is global. Anything that touches a
// game's world or input records should name its game, because those bytes only
// mean something under the game that defined them.
static void read_game_id(void *handle, loaded_plugin_t *p) {
  union {
    void *sym;
    plugin_game_id_func get_game_id;
  } u;
  u.sym = fs_get_symbol(handle, GET_PLUGIN_GAME_ID_FUNC_NAME);
  p->game_id[0] = '\0';
  if (!u.get_game_id) return;
  const char *id = u.get_game_id();
  if (id && *id) snprintf(p->game_id, sizeof(p->game_id), "%s", id);
}

bool plugin_manager_matches_active_game(const plugin_manager_t *manager, const loaded_plugin_t *plugin) {
  if (!plugin->game_id[0]) return true; // global
  if (!manager->context) return false;
  const char *active = manager->context->active_game_id;
  return active && strcmp(active, plugin->game_id) == 0;
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

  read_game_id(handle, p);
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

  read_game_id(handle, p);
  if (!plugin_manager_matches_active_game(manager, p)) {
    p->status = PLUGIN_STATUS_WRONG_GAME;
    snprintf(p->error_msg, sizeof(p->error_msg), "Written for game '%s', which is not active.", p->game_id);
    log_info(LOG_SOURCE, "Skipping '%s': it belongs to game '%s'.", p->path, p->game_id);
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
  if (p->status == PLUGIN_STATUS_WRONG_GAME) {
    p->status = PLUGIN_STATUS_UNLOADED;
    return;
  }
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
  config_save(manager->host_ui);
}

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api, ui_handler_t *host_ui) {
  log_info(LOG_SOURCE, "Initializing plugin system...");
  manager->plugins = NULL;
  manager->count = 0;
  manager->capacity = 0;
  manager->context = context;
  manager->api = api;
  manager->host_ui = host_ui;
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
  log_info(LOG_SOURCE, "Loading config from path: %s", config_path);
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

void plugin_manager_on_game_changed(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    loaded_plugin_t *p = &manager->plugins[i];
    const bool allowed = plugin_manager_matches_active_game(manager, p);

    if (!allowed && p->status == PLUGIN_STATUS_LOADED) {
      plugin_manager_unload_plugin(manager, i);
      p->status = PLUGIN_STATUS_WRONG_GAME;
      snprintf(p->error_msg, sizeof(p->error_msg), "Written for game '%s', which is not active.", p->game_id);
    } else if (allowed && p->enabled && p->status != PLUGIN_STATUS_LOADED) {
      // A plugin parked for the wrong game becomes loadable again; one that
      // errored out for its own reasons is left alone.
      if (p->status == PLUGIN_STATUS_WRONG_GAME || p->status == PLUGIN_STATUS_UNLOADED) {
        p->status = PLUGIN_STATUS_UNLOADED;
        p->error_msg[0] = '\0';
        plugin_manager_load_plugin(manager, i);
      }
    }
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
  char directory_copy[sizeof(manager->directory)];
  snprintf(directory_copy, sizeof(directory_copy), "%s", directory ? directory : manager->directory);
  tas_context_t *context = manager->context;
  tas_api_t *api = manager->api;
  ui_handler_t *host_ui = manager->host_ui;
  log_info(LOG_SOURCE, "Reloading all plugins...");

  plugin_manager_shutdown(manager);
  plugin_manager_init(manager, context, api, host_ui);
  plugin_manager_load_all(manager, directory_copy);
}

static const char *plugin_display_name(const loaded_plugin_t *plugin) {
  return plugin->info.name && plugin->info.name[0] ? plugin->info.name : plugin->key;
}

static bool plugin_matches_filter(const loaded_plugin_t *plugin, const char *filter) {
  if (!filter[0]) return true;
  return safe_strcasestr(plugin_display_name(plugin), filter) ||
         (plugin->info.description && safe_strcasestr(plugin->info.description, filter)) ||
         safe_strcasestr(plugin->key, filter) || (plugin->info.author && safe_strcasestr(plugin->info.author, filter));
}

static int plugin_index_from_key(const plugin_manager_t *manager, const char *key) {
  if (!key[0]) return -1;
  for (int i = 0; i < manager->count; ++i)
    if (strcmp(manager->plugins[i].key, key) == 0) return i;
  return -1;
}

static const char *plugin_status_label(plugin_status_t status) {
  switch (status) {
  case PLUGIN_STATUS_LOADED:
    return "Active";
  case PLUGIN_STATUS_WRONG_GAME:
    return "Other game";
  case PLUGIN_STATUS_UNLOADED:
    return "Disabled";
  case PLUGIN_STATUS_ERROR:
    return "Error";
  }
  return "Unknown";
}

static ImVec4 plugin_status_color(plugin_status_t status) {
  switch (status) {
  case PLUGIN_STATUS_LOADED:
    return (ImVec4){0.25f, 0.82f, 0.38f, 1.f};
  case PLUGIN_STATUS_WRONG_GAME:
    return (ImVec4){0.48f, 0.68f, 0.95f, 1.f};
  case PLUGIN_STATUS_UNLOADED:
    return (ImVec4){0.58f, 0.58f, 0.61f, 1.f};
  case PLUGIN_STATUS_ERROR:
    return (ImVec4){0.95f, 0.32f, 0.30f, 1.f};
  }
  return (ImVec4){0.8f, 0.8f, 0.8f, 1.f};
}

static void render_plugin_status(const loaded_plugin_t *plugin) {
  igTextColored(plugin_status_color(plugin->status), "%s", plugin_status_label(plugin->status));
  if (plugin->status == PLUGIN_STATUS_WRONG_GAME && igIsItemHovered(0))
    igSetTooltip("Waiting for game '%s'. It will load automatically when that game becomes active.", plugin->game_id);
  else if (plugin->status == PLUGIN_STATUS_ERROR && igIsItemHovered(0))
    igSetTooltip("%s", plugin->error_msg);
}

static bool plugin_row_selectable(const char *label, bool selected) {
  const ImVec4 transparent = {0.f, 0.f, 0.f, 0.f};
  igPushStyleColor_Vec4(ImGuiCol_Header, transparent);
  igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, transparent);
  igPushStyleColor_Vec4(ImGuiCol_HeaderActive, transparent);
  const bool clicked = igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){0.f, 0.f});
  const bool hovered = igIsItemHovered(0);
  const bool active = igIsItemActive();
  igPopStyleColor(3);

  if (active) igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_HeaderActive, 1.f), -1);
  else if (hovered)
    igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_HeaderHovered, 1.f), -1);
  else if (selected)
    igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_Header, 1.f), -1);
  return clicked;
}

static void render_plugin_list(plugin_manager_t *manager, char *selected_key, const char *filter, float height,
                               float dpi) {
  if (!igBeginChild_Str("PluginList", (ImVec2){0.f, height}, true, ImGuiWindowFlags_None)) {
    igEndChild();
    return;
  }

  const bool compact = igGetContentRegionAvail().x < 410.f * dpi;
  const int columns = compact ? 2 : 3;
  int visible = 0;
  const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
  if (igBeginTable("Plugins", columns, flags, (ImVec2){0.f, 0.f}, 0.f)) {
    igTableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
    if (!compact) igTableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 65.f * dpi, 0);
    igTableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 82.f * dpi, 0);
    igTableHeadersRow();

    for (int i = 0; i < manager->count; ++i) {
      loaded_plugin_t *plugin = &manager->plugins[i];
      if (!plugin_matches_filter(plugin, filter)) continue;
      ++visible;

      igPushID_Int(i);
      igTableNextRow(0, 0.f);
      igTableSetColumnIndex(0);
      if (plugin_row_selectable(plugin_display_name(plugin), strcmp(selected_key, plugin->key) == 0))
        snprintf(selected_key, 128, "%s", plugin->key);

      int column = 1;
      if (!compact) {
        igTableSetColumnIndex(column++);
        igTextDisabled("%s", plugin->info.version && plugin->info.version[0] ? plugin->info.version : "--");
      }
      igTableSetColumnIndex(column);
      render_plugin_status(plugin);
      igPopID();
    }
    igEndTable();
  }

  if (visible == 0) {
    igSpacing();
    igTextDisabled(filter[0] ? ICON_FA_MAGNIFYING_GLASS "  No plugins match this search."
                             : ICON_FA_PLUG "  No plugins found in the plugin directory.");
  }
  igEndChild();
}

static void render_plugin_metadata(const loaded_plugin_t *plugin) {
  if (!igBeginTable("PluginMetadata", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){0.f, 0.f}, 0.f)) return;
  const ImGuiStyle *style = igGetStyle();
  const ImVec2 widest_label = igCalcTextSize("VERSION", NULL, false, -1.f);
  igTableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, widest_label.x + style->ItemSpacing.x * 1.5f, 0);
  igTableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("VERSION");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->info.version && plugin->info.version[0] ? plugin->info.version : "Unknown");

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("AUTHOR");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->info.author && plugin->info.author[0] ? plugin->info.author : "Unknown");

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("SCOPE");
  igTableSetColumnIndex(1);
  if (plugin->game_id[0]) igTextWrapped(ICON_FA_GAMEPAD "  Game-specific (%s)", plugin->game_id);
  else igTextWrapped(ICON_FA_GLOBE "  Global (any game)");

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("KEY");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->key);
  igEndTable();
}

static void render_plugin_actions(plugin_manager_t *manager, int index, float dpi) {
  loaded_plugin_t *plugin = &manager->plugins[index];
  const bool inline_actions = igGetContentRegionAvail().x >= 350.f * dpi;

  if (plugin->status == PLUGIN_STATUS_LOADED) {
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    if (inline_actions) igSameLine(0.f, 6.f * dpi);
    if (igButton(ICON_FA_ROTATE " Reload", (ImVec2){0.f, 0.f})) plugin_manager_reload_plugin(manager, index);
    if (plugin->show_ui) {
      if (inline_actions) igSameLine(0.f, 6.f * dpi);
      if (igButton(ICON_FA_SLIDERS " Open panel", (ImVec2){0.f, 0.f})) plugin->show_ui(plugin->data);
    }
    return;
  }

  if (plugin->status == PLUGIN_STATUS_WRONG_GAME) {
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    return;
  }

  if (plugin->status == PLUGIN_STATUS_UNLOADED) {
    if (igButton(ICON_FA_PLUG " Enable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    return;
  }

  if (plugin->enabled) {
    if (igButton(ICON_FA_ROTATE " Retry load", (ImVec2){0.f, 0.f})) plugin_manager_reload_plugin(manager, index);
    if (inline_actions) igSameLine(0.f, 6.f * dpi);
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
  } else if (igButton(ICON_FA_PLUG " Enable and retry", (ImVec2){0.f, 0.f})) {
    plugin_manager_toggle_plugin(manager, index);
  }
}

static void render_plugin_detail(plugin_manager_t *manager, int index, float height, float dpi) {
  if (!igBeginChild_Str("PluginDetail", (ImVec2){0.f, height}, true, ImGuiWindowFlags_None)) {
    igEndChild();
    return;
  }
  if (index < 0 || index >= manager->count) {
    igSpacing();
    igTextDisabled(ICON_FA_CIRCLE_INFO "  Select a plugin to view its details.");
    igEndChild();
    return;
  }

  loaded_plugin_t *plugin = &manager->plugins[index];
  igTextWrapped("%s", plugin_display_name(plugin));
  render_plugin_status(plugin);
  igSeparator();
  render_plugin_metadata(plugin);

  igSeparatorText("Description");
  igTextWrapped("%s", plugin->info.description && plugin->info.description[0] ? plugin->info.description
                                                                              : "No description provided.");

  if (plugin->status == PLUGIN_STATUS_WRONG_GAME) {
    igSpacing();
    igPushTextWrapPos(0.f);
    igTextColored(plugin_status_color(plugin->status), ICON_FA_HOURGLASS "  Waiting for '%s' to become active.",
                  plugin->game_id);
    igPopTextWrapPos();
  } else if (plugin->status == PLUGIN_STATUS_ERROR) {
    igSeparatorText("Load error");
    igPushTextWrapPos(0.f);
    igTextColored(plugin_status_color(plugin->status), ICON_FA_TRIANGLE_EXCLAMATION "  %s", plugin->error_msg);
    igPopTextWrapPos();
  }

  if (igCollapsingHeader_TreeNodeFlags("Library path", ImGuiTreeNodeFlags_None)) igTextWrapped("%s", plugin->path);
  igSeparator();
  render_plugin_actions(manager, index, dpi);
  igEndChild();
}

void plugin_manager_render_ui(plugin_manager_t *manager, bool *p_open) {
  if (!*p_open) return;

  igSetNextWindowSize((ImVec2){860.f, 560.f}, ImGuiCond_FirstUseEver);
  if (!igBegin("Plugin Manager", p_open, 0)) {
    igEnd();
    return;
  }

  static char selected_key[128] = "";
  static char filter[128] = "";
  const float dpi = igGetFontSize() > 0.f ? igGetFontSize() / 19.f : 1.f;
  const bool toolbar_inline = igGetContentRegionAvail().x >= 620.f * dpi;

  if (igButton(ICON_FA_MAGNIFYING_GLASS " Scan", (ImVec2){0.f, 0.f}))
    plugin_manager_load_all(manager, manager->directory);
  igSameLine(0.f, 6.f * dpi);
  if (igButton(ICON_FA_ARROWS_ROTATE " Reload all", (ImVec2){0.f, 0.f}))
    plugin_manager_reload_all(manager, manager->directory);
  if (toolbar_inline) igSameLine(0.f, 12.f * dpi);
  else igSpacing();
  igSetNextItemWidth(toolbar_inline ? -FLT_MIN : igGetContentRegionAvail().x);
  igInputTextWithHint("##plugin_filter", ICON_FA_MAGNIFYING_GLASS " Search plugins...", filter, sizeof(filter), 0,
                      NULL, NULL);

  int active_count = 0;
  int error_count = 0;
  int waiting_count = 0;
  for (int i = 0; i < manager->count; ++i) {
    active_count += manager->plugins[i].status == PLUGIN_STATUS_LOADED;
    error_count += manager->plugins[i].status == PLUGIN_STATUS_ERROR;
    waiting_count += manager->plugins[i].status == PLUGIN_STATUS_WRONG_GAME;
  }
  igPushTextWrapPos(0.f);
  igTextDisabled("%d plugin%s  |  %d active  |  %d waiting  |  %d error%s", manager->count,
                 manager->count == 1 ? "" : "s", active_count, waiting_count, error_count,
                 error_count == 1 ? "" : "s");
  igPopTextWrapPos();
  igSeparator();

  int selected_index = plugin_index_from_key(manager, selected_key);
  if (selected_index < 0 && manager->count > 0) {
    snprintf(selected_key, sizeof(selected_key), "%s", manager->plugins[0].key);
    selected_index = 0;
  }

  const ImVec2 remaining = igGetContentRegionAvail();
  const bool split = remaining.x >= 700.f * dpi;
  if (split && igBeginTable("PluginWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                            (ImVec2){0.f, 0.f}, 0.f)) {
    igTableSetupColumn("Plugin list", ImGuiTableColumnFlags_WidthStretch, .56f, 0);
    igTableSetupColumn("Plugin detail", ImGuiTableColumnFlags_WidthStretch, .44f, 0);
    igTableNextRow(0, 0.f);
    igTableSetColumnIndex(0);
    render_plugin_list(manager, selected_key, filter, 0.f, dpi);
    igTableSetColumnIndex(1);
    selected_index = plugin_index_from_key(manager, selected_key);
    render_plugin_detail(manager, selected_index, 0.f, dpi);
    igEndTable();
  } else if (!split) {
    const float list_height = remaining.y > 220.f * dpi ? remaining.y * .46f : remaining.y * .5f;
    render_plugin_list(manager, selected_key, filter, list_height, dpi);
    igSpacing();
    selected_index = plugin_index_from_key(manager, selected_key);
    render_plugin_detail(manager, selected_index, 0.f, dpi);
  }
  igEnd();
}
