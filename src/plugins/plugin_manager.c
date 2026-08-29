#include "plugin_manager.h"
#include <ctype.h>
#include <float.h>
#include <frametee/icons.h>
#include <logger/logger.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/config.h>
#include <system/fs.h>
#include <system/include_cimgui.h>
#include <tomlc17.h>

static const char *LOG_SOURCE = "PluginManager";

// The library whose code the host is currently inside, or NULL when it is in
// its own. Anything a plugin registers with the host while it runs is tagged
// with this, so it can be taken back when the library goes away.
static const void *g_running_plugin = NULL;

const void *plugin_manager_running_plugin(void) { return g_running_plugin; }

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

// A plugin's directory names it, and the library inside carries that name with
// whatever prefix and suffix the platform puts on a shared library. Anything
// else in the directory belongs to the plugin -- resources, or the libraries it
// depends on -- and is never mistaken for the plugin itself.
static bool find_plugin_library(const char *plugin_directory, const char *name, char *out_path, size_t size) {
  static const char *const patterns[] = {
#ifdef _WIN32
      "%s%c%s.dll",
      "%s%clib%s.dll",
#elif defined(__APPLE__)
      "%s%clib%s.dylib",
      "%s%c%s.dylib",
#else
      "%s%clib%s.so",
      "%s%c%s.so",
#endif
  };

  for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
    const int length = snprintf(out_path, size, patterns[i], plugin_directory, PATH_SEP, name);
    if (length < 0 || (size_t)length >= size) continue;
    FILE *file = fs_open(out_path, "rb");
    if (!file) continue;
    fclose(file);
    return true;
  }
  out_path[0] = '\0';
  return false;
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
  p->game_id_known = true;
  if (!u.get_game_id) return;
  const char *id = u.get_game_id();
  if (id && *id) snprintf(p->game_id, sizeof(p->game_id), "%s", id);
}

// The game a plugin says it is for: what its library answered if it has been
// loaded, and what its manifest claims if it has not. Also what the interface
// names while a plugin waits for its game, which is a state it can be in
// without ever having run.
const char *plugin_target_game(const loaded_plugin_t *plugin) {
  return plugin->game_id_known ? plugin->game_id : plugin->manifest_game;
}

bool plugin_manager_matches_active_game(const plugin_manager_t *manager, const loaded_plugin_t *plugin) {
  const char *declared = plugin_target_game(plugin);
  if (!declared[0]) return true; // global
  if (!manager->context) return false;
  const char *active = manager->context->active_game_id;
  return active && strcmp(active, declared) == 0;
}

// A plugin's name, author, version, description and game id all live inside the
// library, so loading it is the only way to ask it anything -- and loading it
// runs it. A plugin may therefore ship a manifest beside its library, which the
// editor reads as data, so that a plugin it has not run can still be listed.
static bool plugin_metadata_known(const loaded_plugin_t *plugin) { return plugin->info_name[0] != '\0'; }

// The manifest's name if it gave one, and the directory's otherwise.
static const char *plugin_display_name(const loaded_plugin_t *plugin) {
  return plugin->info_name[0] ? plugin->info_name : plugin->key;
}

// One identity for everything a plugin ships, so the number the editor shows
// covers its library, its manifest and its resources rather than one file of
// the three.
//
// The recipe is deliberately one anybody can repeat without this editor: hash
// every file under the directory, then hash the lines "<hex>  <relative path>"
// in ascending path order -- exactly the bytes sha256sum prints. docs/plugins.md
// gives the shell equivalent.
#define DIGEST_MAX_FILES 4096
#define DIGEST_MAX_DEPTH 8

typedef struct {
  char **paths;
  int count;
  bool failed;
} digest_listing_t;

static void collect_files(const char *root, const char *prefix, int depth, digest_listing_t *listing) {
  if (listing->failed || depth > DIGEST_MAX_DEPTH) {
    listing->failed = true;
    return;
  }

  char scan_path[1024];
  const int scan_length = prefix[0] ? snprintf(scan_path, sizeof(scan_path), "%s%c%s", root, PATH_SEP, prefix)
                                    : snprintf(scan_path, sizeof(scan_path), "%s", root);
  if (scan_length < 0 || (size_t)scan_length >= sizeof(scan_path)) {
    listing->failed = true;
    return;
  }

  fs_dir_t *dir = fs_opendir(scan_path);
  if (!dir) {
    listing->failed = true;
    return;
  }

  fs_dirent_t *entry;
  while ((entry = fs_readdir(dir)) != NULL && !listing->failed) {
    if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) continue;

    // Always '/' in the hashed name, so the same directory hashes the same on
    // either platform.
    char relative[1024];
    const int relative_length = prefix[0] ? snprintf(relative, sizeof(relative), "%s/%s", prefix, entry->name)
                                          : snprintf(relative, sizeof(relative), "%s", entry->name);
    if (relative_length < 0 || (size_t)relative_length >= sizeof(relative)) {
      listing->failed = true;
      break;
    }

    if (entry->is_directory) {
      collect_files(root, relative, depth + 1, listing);
      continue;
    }

    if (listing->count >= DIGEST_MAX_FILES) {
      listing->failed = true;
      break;
    }
    char **grown = realloc(listing->paths, (size_t)(listing->count + 1) * sizeof(*listing->paths));
    if (!grown) {
      listing->failed = true;
      break;
    }
    listing->paths = grown;
    listing->paths[listing->count] = strdup(relative);
    if (!listing->paths[listing->count]) {
      listing->failed = true;
      break;
    }
    listing->count++;
  }

  fs_closedir(dir);
}

static int compare_paths(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool directory_digest_hex(const char *plugin_directory, char out_hex[SHA256_HEX_SIZE]) {
  out_hex[0] = '\0';

  digest_listing_t listing = {NULL, 0, false};
  collect_files(plugin_directory, "", 0, &listing);

  bool ok = !listing.failed && listing.count > 0;
  if (ok) {
    qsort(listing.paths, (size_t)listing.count, sizeof(*listing.paths), compare_paths);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    for (int i = 0; i < listing.count && ok; ++i) {
      char file_path[1024];
      char file_hex[SHA256_HEX_SIZE];
      const int path_length = snprintf(file_path, sizeof(file_path), "%s%c%s", plugin_directory, PATH_SEP, listing.paths[i]);
      if (path_length < 0 || (size_t)path_length >= sizeof(file_path)) {
        ok = false;
        break;
      }
      // A file that cannot be read leaves the digest unknown rather than
      // quietly standing for a directory with one file fewer in it.
      ok = sha256_file_hex(file_path, file_hex);
      if (!ok) break;

      char line[1024 + SHA256_HEX_SIZE + 4];
      const int length = snprintf(line, sizeof(line), "%s  %s\n", file_hex, listing.paths[i]);
      if (length < 0 || (size_t)length >= sizeof(line)) {
        ok = false;
        break;
      }
      sha256_update(&ctx, line, (size_t)length);
    }

    if (ok) {
      uint8_t digest[32];
      sha256_final(&ctx, digest);
      for (int i = 0; i < 32; ++i)
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    }
  }

  for (int i = 0; i < listing.count; ++i)
    free(listing.paths[i]);
  free(listing.paths);
  if (!ok) out_hex[0] = '\0';
  return ok;
}

// Manifest text is written by whoever wrote the plugin and is rendered straight
// into the interface, so control characters -- which would break a line or hide
// the rest of a value -- are dropped rather than passed along.
static void copy_manifest_string(char *destination, size_t size, toml_datum_t value) {
  destination[0] = '\0';
  if (value.type != TOML_STRING || !value.u.str.ptr) return;

  size_t written = 0;
  for (const unsigned char *cursor = (const unsigned char *)value.u.str.ptr; *cursor && written + 1 < size; ++cursor) {
    if (*cursor < 0x20 || *cursor == 0x7f) continue;
    destination[written++] = (char)*cursor;
  }
  destination[written] = '\0';
}

// The manifest is plugin.toml in the plugin's own directory, so it travels with
// the library it describes. A plugin that ships without one is simply listed by
// the name of its directory.
//
// Nothing in here is verified. The game claim has one fail-closed use before
// the library is opened: naming another game withholds the load. The library's
// own plugin_game_id() answer remains authoritative once it can be asked. The
// rest exists so the user can see what a plugin claims to be, and where its
// source is meant to live, before agreeing to run it.
static void read_manifest(loaded_plugin_t *p) {
  // A reload is also a rescan. If the file was removed or stopped parsing, its
  // old claims must not survive as though they had just been read again.
  p->repository[0] = '\0';
  p->manifest_game[0] = '\0';
  p->info_name[0] = '\0';
  p->info_author[0] = '\0';
  p->info_version[0] = '\0';
  p->info_description[0] = '\0';

  char manifest_path[sizeof(p->directory) + 16];
  snprintf(manifest_path, sizeof(manifest_path), "%s%cplugin.toml", p->directory, PATH_SEP);

  FILE *fp = fs_open(manifest_path, "r");
  if (!fp) return;
  toml_result_t result = toml_parse_file(fp);
  fclose(fp);
  if (!result.ok) {
    log_warn(LOG_SOURCE, "Ignoring the manifest for '%s': %s", p->key, result.errmsg);
    return;
  }

  copy_manifest_string(p->repository, sizeof(p->repository), toml_get(result.toptab, "repository"));
  copy_manifest_string(p->manifest_game, sizeof(p->manifest_game), toml_get(result.toptab, "game"));

  copy_manifest_string(p->info_name, sizeof(p->info_name), toml_get(result.toptab, "name"));
  copy_manifest_string(p->info_author, sizeof(p->info_author), toml_get(result.toptab, "author"));
  copy_manifest_string(p->info_version, sizeof(p->info_version), toml_get(result.toptab, "version"));
  copy_manifest_string(p->info_description, sizeof(p->info_description), toml_get(result.toptab, "description"));

  toml_free(result);
}

// Opening a library runs it. A constructor, or a DllMain, executes before this
// function can look up its first symbol, and from that moment the code is in
// this process whatever the editor decides afterwards -- unloading it again is
// not a way to un-run it. So everything that decides whether a plugin may run
// is settled here, before the open, out of what can be read as data: the
// config's record of what the user approved, the editor's digest of the
// directory as it is now, and the manifest beside the library. This is the only
// call to fs_load_library in the editor, so no caller can get past it.
bool plugin_manager_load_plugin(plugin_manager_t *manager, int index) {
  loaded_plugin_t *p = &manager->plugins[index];
  if (p->status == PLUGIN_STATUS_LOADED) return true;

  if (!p->enabled) {
    p->status = PLUGIN_STATUS_UNLOADED;
    return false;
  }

  // Read again rather than trust what an earlier scan left behind: this is the
  // last moment before the files run, and they may have been replaced since.
  const bool digest_ok = directory_digest_hex(p->directory, p->sha256);
  if (!digest_ok) p->sha256[0] = '\0';
  read_manifest(p);

  if (!digest_ok) {
    // Nothing to compare against what was approved. A plugin whose contents
    // cannot be established does not run on the strength of that.
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "This plugin's files could not be read to check them against what you approved.");
    log_error(LOG_SOURCE, "Could not read '%s' to check its contents; leaving it unloaded.", p->directory);
    return false;
  }

  // The user approved a particular set of files. Anything else is a decision
  // they have not made, and it is refused before the library is opened rather
  // than after, because after is too late.
  if (!p->approved_sha256[0] || strcmp(p->approved_sha256, p->sha256) != 0) {
    p->status = PLUGIN_STATUS_CHANGED;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "The files in this plugin's directory have changed since it was enabled, so it was not loaded.");
    log_warn(LOG_SOURCE, "'%s' changed since it was last enabled; not loading it.", p->key);
    return false;
  }

  // A plugin whose manifest names another game is not going to be used this
  // session, and the only other way to ask is to run it first. The manifest is
  // read in one direction only: it can withhold a load, never authorise one,
  // because the library's own answer is checked again below as soon as it is
  // up. That check is what a manifest cannot talk its way past; this one just
  // means an honest plugin for another game never has to be run to be skipped.
  if (!plugin_manager_matches_active_game(manager, p)) {
    p->status = PLUGIN_STATUS_WRONG_GAME;
    snprintf(p->error_msg, sizeof(p->error_msg), "Written for game '%s', which is not active.", plugin_target_game(p));
    log_info(LOG_SOURCE, "Skipping '%s': its manifest says it belongs to game '%s'.", p->key, plugin_target_game(p));
    return false;
  }

  void *handle = fs_load_library(p->path);
  if (!handle) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "Failed to load library: %s", p->path);
    log_error(LOG_SOURCE, "Failed to load plugin: %s", p->path);
    return false;
  }

  union {
    void *sym;
    plugin_init_func init;
    plugin_update_func update;
    plugin_shutdown_func shutdown;
    plugin_show_ui_func show_ui;
    plugin_abi_version_func abi_version;
  } u;

  // Before anything else is called through: a plugin built against a different
  // version of plugin_api.h would be reading structs that have moved under it.
  u.sym = fs_get_symbol(handle, GET_PLUGIN_ABI_VERSION_FUNC_NAME);
  const plugin_abi_version_func read_abi_version = u.abi_version;
  if (!read_abi_version) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "Declares no plugin ABI version. Rebuild it against this editor's plugin_api.h.");
    log_error(LOG_SOURCE, "Plugin '%s' declares no ABI version.", p->path);
    fs_free_library(handle);
    return false;
  }
  const uint32_t plugin_abi = read_abi_version();
  if (plugin_abi != FRAMETEE_PLUGIN_ABI_VERSION) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "Built for plugin ABI %u; this editor speaks %u.", plugin_abi,
             FRAMETEE_PLUGIN_ABI_VERSION);
    log_error(LOG_SOURCE, "Plugin '%s' is built for ABI %u, not %u.", p->path, plugin_abi, FRAMETEE_PLUGIN_ABI_VERSION);
    fs_free_library(handle);
    return false;
  }

  u.sym = fs_get_symbol(handle, GET_PLUGIN_INIT_FUNC_NAME);
  p->init = u.init;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_UPDATE_FUNC_NAME);
  p->update = u.update;

  u.sym = fs_get_symbol(handle, GET_PLUGIN_SHUTDOWN_FUNC_NAME);
  p->shutdown = u.shutdown;

  u.sym = fs_get_symbol(handle, "plugin_show_ui");
  p->show_ui = u.show_ui;

  if (!p->init) {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "Plugin missing required symbols.");
    log_error(LOG_SOURCE, "Plugin '%s' is missing required symbols.", p->path);
    fs_free_library(handle);
    return false;
  }

  // The library's own answer, which is the authority: a manifest that stayed
  // quiet, or claimed this game while the code says otherwise, is caught here.
  read_game_id(handle, p);
  if (!plugin_manager_matches_active_game(manager, p)) {
    p->status = PLUGIN_STATUS_WRONG_GAME;
    snprintf(p->error_msg, sizeof(p->error_msg), "Written for game '%s', which is not active.", p->game_id);
    log_info(LOG_SOURCE, "Skipping '%s': it belongs to game '%s'.", p->path, p->game_id);
    fs_free_library(handle);
    return false;
  }

  p->handle = handle;

  // A plugin's resources live beside it, and it has no other way to find out
  // where that is.
  manager->context->plugin_directory = p->directory;
  g_running_plugin = p->handle;
  p->data = p->init(manager->context, manager->api);
  g_running_plugin = NULL;
  manager->context->plugin_directory = NULL;

  if (p->data) {
    p->status = PLUGIN_STATUS_LOADED;
    p->error_msg[0] = '\0';
    log_info(LOG_SOURCE, "Loaded '%s'.", plugin_display_name(p));
    return true;
  } else {
    p->status = PLUGIN_STATUS_ERROR;
    snprintf(p->error_msg, sizeof(p->error_msg), "plugin_init returned NULL.");
    log_error(LOG_SOURCE, "Plugin '%s' failed to initialize.", plugin_display_name(p));
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

  log_info(LOG_SOURCE, "Shutting down '%s'...", plugin_display_name(p));
  // Anything this plugin put on the undo stack is three pointers into a library
  // that is about to stop existing. Take them back while the code that knows
  // how to clean them up is still mapped, and before the plugin's own shutdown
  // frees whatever they point at.
  if (p->handle && manager->host_ui) {
    const int dropped = undo_manager_purge_owner(&manager->host_ui->undo_manager, p->handle);
    if (dropped > 0)
      log_info(LOG_SOURCE, "Dropped %d undo step(s) that only '%s' knew how to reverse.", dropped, plugin_display_name(p));
  }
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
    // Turning a plugin on is the moment of consent, so it is also what gets
    // remembered: these files, as they are on disk now rather than as some
    // earlier scan found them.
    if (!directory_digest_hex(p->directory, p->sha256)) p->sha256[0] = '\0';
    snprintf(p->approved_sha256, sizeof(p->approved_sha256), "%s", p->sha256);
    // Enabling approves the files now on disk, which may not be the library
    // whose answer was cached before the plugin was switched off.
    p->game_id[0] = '\0';
    p->game_id_known = false;
    plugin_manager_load_plugin(manager, index);
  } else {
    plugin_manager_unload_plugin(manager, index);
    p->approved_sha256[0] = '\0';
  }
  config_save(manager->host_ui);
}

void plugin_manager_approve_current_version(plugin_manager_t *manager, int index) {
  loaded_plugin_t *p = &manager->plugins[index];
  // What is there now, read now: the digest on screen came from a scan, and
  // what is being approved is the directory.
  if (!directory_digest_hex(p->directory, p->sha256)) p->sha256[0] = '\0';
  snprintf(p->approved_sha256, sizeof(p->approved_sha256), "%s", p->sha256);
  p->game_id[0] = '\0';
  p->game_id_known = false;
  p->enabled = true;
  p->status = PLUGIN_STATUS_UNLOADED;
  p->error_msg[0] = '\0';
  plugin_manager_load_plugin(manager, index);
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
  toml_datum_t checksums_table = {0};
  if (has_toml) {
    plugins_table = toml_get(res.toptab, "plugins");
    checksums_table = toml_get(res.toptab, "plugin_checksums");
  }

  fs_dir_t *dir = fs_opendir(directory);
  if (!dir) {
    if (has_toml) toml_free(res);
    return;
  }

  fs_dirent_t *entry;
  while ((entry = fs_readdir(dir)) != NULL) {
    if (!entry->is_directory) {
      // A plugin is a directory now, so a loose library here is somebody's old
      // install rather than something to load.
      const char *ext = strrchr(entry->name, '.');
      if (ext && (strcmp(ext, ".dll") == 0 || strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0))
        log_warn(LOG_SOURCE, "Ignoring '%s': a plugin lives in its own directory now. See docs/plugins.md.", entry->name);
      continue;
    }
    if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) continue;

    // The directory names the plugin: it is the same on every platform, unlike
    // the library file, which grows a "lib" here and loses it there. That name
    // is the key in config.toml and the name of the library inside.
    char plugin_directory[1024];
    const int directory_length =
        snprintf(plugin_directory, sizeof(plugin_directory), "%s%c%s", directory, PATH_SEP, entry->name);
    if (directory_length < 0 || (size_t)directory_length >= sizeof(plugin_directory)) {
      log_warn(LOG_SOURCE, "Plugin path for '%s' is too long; skipping it.", entry->name);
      continue;
    }

    char full_path[1024];
    if (!find_plugin_library(plugin_directory, entry->name, full_path, sizeof(full_path))) {
      log_warn(LOG_SOURCE, "'%s' holds no library named after it; skipping it.", plugin_directory);
      continue;
    }

    const char *key = entry->name;

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
        const int new_capacity = manager->capacity == 0 ? 4 : manager->capacity * 2;
        loaded_plugin_t *new_plugins = realloc(manager->plugins, (size_t)new_capacity * sizeof(*new_plugins));
        if (!new_plugins) {
          log_error(LOG_SOURCE, "Out of memory while adding plugin '%s'.", key);
          continue;
        }
        manager->plugins = new_plugins;
        manager->capacity = new_capacity;
      }
      index = manager->count++;
      memset(&manager->plugins[index], 0, sizeof(loaded_plugin_t));
      snprintf(manager->plugins[index].path, sizeof(manager->plugins[index].path), "%s", full_path);
      snprintf(manager->plugins[index].directory, sizeof(manager->plugins[index].directory), "%s", plugin_directory);
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

    // 3b. What can be known about the plugin without running it: the editor's
    //     own digest of everything in its directory, and whatever the plugin
    //     says about itself in the manifest there.
    if (!directory_digest_hex(p->directory, p->sha256)) {
      p->sha256[0] = '\0';
      log_warn(LOG_SOURCE, "Could not read '%s' to check its contents.", p->directory);
    }
    read_manifest(p);

    // 3c. And what the user approved when they turned it on, if anything.
    p->approved_sha256[0] = '\0';
    if (checksums_table.type == TOML_TABLE) {
      toml_datum_t recorded = toml_get(checksums_table, key);
      if (recorded.type == TOML_STRING && recorded.u.str.ptr)
        snprintf(p->approved_sha256, sizeof(p->approved_sha256), "%s", recorded.u.str.ptr);
    }

    // 4. Load it, or leave the directory alone. Being listed is not consent to
    //    run, and what may run is not decided here: plugin_manager_load_plugin
    //    settles that immediately before it opens the library, so that every
    //    other way into it -- the Reload button, a game switch -- is held to
    //    the same test as this one.
    if (!enabled) {
      plugin_manager_unload_plugin(manager, index);
      p->approved_sha256[0] = '\0';
    } else {
      // A config written before the editor recorded checksums, or by hand:
      // nothing was ever approved, so take what is on disk as what was meant
      // rather than accusing the user of a change they did not make. Recorded
      // before the load, because the load is what checks it.
      if (!p->approved_sha256[0])
        snprintf(p->approved_sha256, sizeof(p->approved_sha256), "%s", p->sha256);
      plugin_manager_load_plugin(manager, index);
    }
  }
  fs_closedir(dir);

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
      snprintf(p->error_msg, sizeof(p->error_msg), "Written for game '%s', which is not active.", plugin_target_game(p));
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
      g_running_plugin = manager->plugins[i].handle;
      manager->plugins[i].update(manager->plugins[i].data);
      g_running_plugin = NULL;
    }
  }
}

void plugin_manager_shutdown(plugin_manager_t *manager) {
  for (int i = 0; i < manager->count; ++i) {
    if (manager->plugins[i].status == PLUGIN_STATUS_LOADED) {
      log_info(LOG_SOURCE, "Shutting down '%s'...", plugin_display_name(&manager->plugins[i]));
      // As in plugin_manager_unload_plugin: the undo stacks outlive this call
      // -- ui_cleanup empties them after every plugin is gone -- so a command
      // whose cleanup lives in this library has to go before the library does.
      if (manager->host_ui) {
        const int dropped = undo_manager_purge_owner(&manager->host_ui->undo_manager, manager->plugins[i].handle);
        if (dropped > 0)
          log_info(LOG_SOURCE, "Dropped %d undo step(s) that only '%s' knew how to reverse.", dropped,
                   plugin_display_name(&manager->plugins[i]));
      }
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

static bool plugin_matches_filter(const loaded_plugin_t *plugin, const char *filter) {
  if (!filter[0]) return true;
  return safe_strcasestr(plugin_display_name(plugin), filter) ||
         safe_strcasestr(plugin->info_description, filter) || safe_strcasestr(plugin->key, filter) ||
         safe_strcasestr(plugin->info_author, filter);
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
  case PLUGIN_STATUS_CHANGED:
    return "Changed";
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
  case PLUGIN_STATUS_CHANGED:
    return (ImVec4){0.96f, 0.74f, 0.28f, 1.f};
  case PLUGIN_STATUS_ERROR:
    return (ImVec4){0.95f, 0.32f, 0.30f, 1.f};
  }
  return (ImVec4){0.8f, 0.8f, 0.8f, 1.f};
}

static void render_plugin_status(const loaded_plugin_t *plugin) {
  igTextColored(plugin_status_color(plugin->status), "%s", plugin_status_label(plugin->status));
  if (plugin->status == PLUGIN_STATUS_WRONG_GAME && igIsItemHovered(0))
    igSetTooltip("Waiting for game '%s'. It will load automatically when that game becomes active.",
                 plugin_target_game(plugin));
  else if (plugin->status == PLUGIN_STATUS_CHANGED && igIsItemHovered(0))
    igSetTooltip("%s", plugin->error_msg);
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
        igTextDisabled("%s", plugin->info_version[0] ? plugin->info_version : "--");
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

// A manifest may name any string it likes, and the platform "open this" call
// behind a link will open far more than web pages -- a local program, for one.
// Only http(s) is ever passed on; anything else is shown as text.
static bool manifest_link_is_web(const char *link) {
  return strncmp(link, "https://", 8) == 0 || strncmp(link, "http://", 7) == 0;
}

static void render_plugin_metadata(const loaded_plugin_t *plugin) {
  if (!igBeginTable("PluginMetadata", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){0.f, 0.f}, 0.f)) return;
  const ImGuiStyle *style = igGetStyle();
  const ImVec2 widest_label = igCalcTextSize("REPOSITORY", NULL, false, -1.f);
  igTableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, widest_label.x + style->ItemSpacing.x * 1.5f, 0);
  igTableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("VERSION");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->info_version[0] ? plugin->info_version : "Unknown");

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("AUTHOR");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->info_author[0] ? plugin->info_author : "Unknown");

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("SCOPE");
  igTableSetColumnIndex(1);
  // The library's own answer if it has been loaded, the manifest's claim if not.
  const char *scope_game = plugin->game_id[0] ? plugin->game_id : plugin->manifest_game;
  if (scope_game[0]) igTextWrapped(ICON_FA_GAMEPAD "  Game-specific (%s)", scope_game);
  else if (!plugin_metadata_known(plugin)) igTextDisabled(ICON_FA_CIRCLE_QUESTION "  Unknown until enabled");
  else igTextWrapped(ICON_FA_GLOBE "  Global (any game)");

  if (plugin->repository[0]) {
    igTableNextRow(0, 0.f);
    igTableSetColumnIndex(0);
    igTextDisabled("REPOSITORY");
    igTableSetColumnIndex(1);
    // Only ever handed to the shell as a web address. The string came out of a
    // file next to an unrun plugin, and on Windows the shell would just as
    // happily open a path to a program.
    if (manifest_link_is_web(plugin->repository)) igTextLinkOpenURL(plugin->repository, plugin->repository);
    else igTextWrapped("%s", plugin->repository);
  }

  if (plugin->sha256[0]) {
    igTableNextRow(0, 0.f);
    igTableSetColumnIndex(0);
    igTextDisabled("SHA-256");
    igTableSetColumnIndex(1);
    // Sixty-four characters nobody is going to retype, so the text itself is
    // the button.
    igTextWrapped("%s", plugin->sha256);
    if (igIsItemHovered(0)) {
      igSetMouseCursor(ImGuiMouseCursor_Hand);
      igSetTooltip("Click to copy");
    }
    if (igIsItemClicked(0)) igSetClipboardText(plugin->sha256);
  }

  igTableNextRow(0, 0.f);
  igTableSetColumnIndex(0);
  igTextDisabled("KEY");
  igTableSetColumnIndex(1);
  igTextWrapped("%s", plugin->key);
  igEndTable();
}

// Enabling a plugin is not a preference, it is a decision to run someone else's
// program: a plugin is a native library loaded into this process, with no
// sandbox of any kind between it and the machine. The editor cannot inspect
// what it will do, or contain it once it is running, so the honest thing is to
// say so where the decision is actually made.
static void render_plugin_trust_warning(void) {
  igPushTextWrapPos(0.f);
  igTextColored((ImVec4){0.96f, 0.74f, 0.28f, 1.f}, ICON_FA_TRIANGLE_EXCLAMATION "  Plugins are not sandboxed.");
  igTextDisabled("A plugin runs as part of the editor, with the same access to your files and your system that you have. "
                 "Only enable plugins from authors you trust, and prefer ones whose source you can read.");
  igPopTextWrapPos();
  igSpacing();
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
      if (igButton(ICON_FA_SLIDERS " Open panel", (ImVec2){0.f, 0.f})) {
        g_running_plugin = plugin->handle;
        plugin->show_ui(plugin->data);
        g_running_plugin = NULL;
      }
    }
    return;
  }

  if (plugin->status == PLUGIN_STATUS_WRONG_GAME) {
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    return;
  }

  if (plugin->status == PLUGIN_STATUS_CHANGED) {
    render_plugin_trust_warning();
    if (igButton(ICON_FA_PLUG " Enable this version", (ImVec2){0.f, 0.f}))
      plugin_manager_approve_current_version(manager, index);
    if (inline_actions) igSameLine(0.f, 6.f * dpi);
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    return;
  }

  if (plugin->status == PLUGIN_STATUS_UNLOADED) {
    render_plugin_trust_warning();
    if (igButton(ICON_FA_PLUG " Enable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
    return;
  }

  if (plugin->enabled) {
    if (igButton(ICON_FA_ROTATE " Retry load", (ImVec2){0.f, 0.f})) plugin_manager_reload_plugin(manager, index);
    if (inline_actions) igSameLine(0.f, 6.f * dpi);
    if (igButton(ICON_FA_POWER_OFF " Disable", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
  } else {
    render_plugin_trust_warning();
    if (igButton(ICON_FA_PLUG " Enable and retry", (ImVec2){0.f, 0.f})) plugin_manager_toggle_plugin(manager, index);
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
  if (plugin->info_description[0]) igTextWrapped("%s", plugin->info_description);
  else if (!plugin_metadata_known(plugin))
    igTextDisabled("A plugin describes itself from inside its library, which this editor only loads once you enable it.");
  else igTextWrapped("No description provided.");

  if (plugin->status == PLUGIN_STATUS_CHANGED) {
    igSeparatorText("Changed since you enabled it");
    igPushTextWrapPos(0.f);
    igTextColored(plugin_status_color(plugin->status), ICON_FA_TRIANGLE_EXCLAMATION "  %s", plugin->error_msg);
    igTextDisabled("Enabling a plugin approves the files that are there at the time, and these are not those files. "
                   "That is expected after updating the plugin yourself. If you did not update it, find out why it "
                   "changed before enabling it again.");
    igPopTextWrapPos();
    igTextDisabled("APPROVED");
    igTextWrapped("%s", plugin->approved_sha256);
    igTextDisabled("NOW");
    igTextWrapped("%s", plugin->sha256[0] ? plugin->sha256 : "unreadable");
  }

  if (plugin->status == PLUGIN_STATUS_WRONG_GAME) {
    igSpacing();
    igPushTextWrapPos(0.f);
    igTextColored(plugin_status_color(plugin->status), ICON_FA_HOURGLASS "  Waiting for '%s' to become active.",
                  plugin_target_game(plugin));
    igPopTextWrapPos();
  } else if (plugin->status == PLUGIN_STATUS_ERROR) {
    igSeparatorText("Load error");
    igPushTextWrapPos(0.f);
    igTextColored(plugin_status_color(plugin->status), ICON_FA_TRIANGLE_EXCLAMATION "  %s", plugin->error_msg);
    igPopTextWrapPos();
  }

  if (igCollapsingHeader_TreeNodeFlags("Files", ImGuiTreeNodeFlags_None)) {
    igTextDisabled("DIRECTORY");
    igTextWrapped("%s", plugin->directory);
    igTextDisabled("LIBRARY");
    igTextWrapped("%s", plugin->path);
  }
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
  int changed_count = 0;
  for (int i = 0; i < manager->count; ++i) {
    active_count += manager->plugins[i].status == PLUGIN_STATUS_LOADED;
    error_count += manager->plugins[i].status == PLUGIN_STATUS_ERROR;
    waiting_count += manager->plugins[i].status == PLUGIN_STATUS_WRONG_GAME;
    changed_count += manager->plugins[i].status == PLUGIN_STATUS_CHANGED;
  }
  igPushTextWrapPos(0.f);
  igTextDisabled("%d plugin%s  |  %d active  |  %d waiting  |  %d changed  |  %d error%s", manager->count,
                 manager->count == 1 ? "" : "s", active_count, waiting_count, changed_count, error_count,
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
