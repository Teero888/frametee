#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include "plugin_api.h"
#include <types.h>
#include <stdbool.h>
#include <system/sha256.h>

typedef enum {
  PLUGIN_STATUS_UNLOADED = 0,
  PLUGIN_STATUS_LOADED,
  PLUGIN_STATUS_ERROR,
  // Written for a different game than the one that is active. The plugin stays
  // listed so the user can see why it is idle, but it is never initialized.
  PLUGIN_STATUS_WRONG_GAME,
  // Enabled, but its files are not the ones that were there when it was
  // enabled. Approving a plugin approved a particular thing on disk, so the
  // editor stops rather than running whatever replaced it.
  PLUGIN_STATUS_CHANGED
} plugin_status_t;

typedef const char *(*plugin_game_id_func)(void);

struct loaded_plugin_t {
  // Directory the plugin owns, and the library inside it that the editor loads.
  // Everything else in there belongs to the plugin: its manifest, its
  // resources, and any libraries it brought along.
  char directory[1024];
  char path[1024];
  char key[128]; // the directory's name, which is also the config key
  void *handle; // DLL/SO handle
  
  // What the editor shows about a plugin, all of it read from the manifest, so
  // all of it the author's claim rather than anything the editor established.
  char info_name[128];
  char info_author[128];
  char info_version[64];
  char info_description[512];
  
  // Game this plugin is written for, empty when it is global. Owned copy, since
  // the string lives in the library and the library may be unloaded. Read from
  // the library itself, so it is only set once the plugin has been loaded.
  char game_id[32];

  // From the manifest the plugin ships beside its library, which the editor
  // reads as data so that a plugin it will not run can still be listed. Every
  // one of these is the author's claim about their own plugin: the manifest
  // sits next to the library and whoever writes one writes the other. They are
  // shown to the user and never used to decide anything.
  char repository[256];
  char manifest_game[32];

  // The editor's own SHA-256 over the plugin's directory, for comparing against
  // whatever checksum the author published. Computed here, not read from the
  // manifest, which could only ever agree with itself.
  char sha256[SHA256_HEX_SIZE];
  // What that digest was when the user enabled this plugin, remembered in
  // config.toml. Empty for a plugin that is off. While it disagrees with
  // sha256, the plugin is not loaded: consent was given to the files that were
  // there at the time, not to whatever is there now.
  char approved_sha256[SHA256_HEX_SIZE];

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

// The library whose code the host is currently running, or NULL. Used to tag
// what a plugin hands the host, so it can be dropped when that plugin unloads.
const void *plugin_manager_running_plugin(void);

void plugin_manager_init(plugin_manager_t *manager, tas_context_t *context, tas_api_t *api, ui_handler_t *host_ui);
void plugin_manager_load_all(plugin_manager_t *manager, const char *directory);
void plugin_manager_update_all(plugin_manager_t *manager);
void plugin_manager_shutdown(plugin_manager_t *manager);
void plugin_manager_reload_all(plugin_manager_t *manager, const char *directory);

bool plugin_manager_load_plugin(plugin_manager_t *manager, int index);
void plugin_manager_unload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_reload_plugin(plugin_manager_t *manager, int index);
void plugin_manager_toggle_plugin(plugin_manager_t *manager, int index);

// Accepts the plugin's current files as what the user meant to enable, and
// loads it. This is the only way out of PLUGIN_STATUS_CHANGED other than
// turning the plugin off.
void plugin_manager_approve_current_version(plugin_manager_t *manager, int index);
void plugin_manager_render_ui(plugin_manager_t *manager, bool *p_open);

// True when the plugin may run under the currently active game: either it is
// global, or its game_id matches. Game-specific plugins are skipped rather than
// failed, because switching back makes them valid again.
bool plugin_manager_matches_active_game(const plugin_manager_t *manager, const loaded_plugin_t *plugin);
// Loads plugins that became valid for the newly active game and unloads the
// ones that no longer apply. Call after the active game changes.
void plugin_manager_on_game_changed(plugin_manager_t *manager);

#endif // PLUGIN_MANAGER_H
