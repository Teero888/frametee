#include "tmuf_data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// The module uses one installation for both its start screen and game.
static char steam_data[1024];

static bool join_path(char *out, size_t size, const char *root, const char *relative) {
  const size_t len = strlen(root);
  const char *sep = len && (root[len - 1] == '/' || root[len - 1] == '\\') ? "" : "/";
  const int n = snprintf(out, size, "%s%s%s", root, sep, relative);
  return n >= 0 && (size_t)n < size;
}

typedef struct pack_files {
  bool packlist, pak;
} pack_files;

static bool visit_pack(void *user, const ft_directory_entry *entry) {
  pack_files *files = user;
  if (entry->is_directory) return true;
  if (strcmp(entry->name, "packlist.dat") == 0) files->packlist = true;
  const size_t n = strlen(entry->name);
  if (n > 4 && entry->name[n - 4] == '.' && tolower((unsigned char)entry->name[n - 3]) == 'p' &&
      tolower((unsigned char)entry->name[n - 2]) == 'a' && tolower((unsigned char)entry->name[n - 1]) == 'k')
    files->pak = true;
  return true;
}

static bool visit_any(void *user, const ft_directory_entry *entry) {
  (void)entry;
  *(bool *)user = true;
  return false;
}

static bool data_present(const ft_engine_api *engine, const char *root) {
  if (!engine->visit_directory) return false;
  char path[1200];
  pack_files packs = {0};
  bool game_data = false;
  if (!join_path(path, sizeof path, root, "Packs")) return false;
  engine->visit_directory(path, visit_pack, &packs);
  if (!packs.packlist || !packs.pak || !join_path(path, sizeof path, root, "GameData")) return false;
  engine->visit_directory(path, visit_any, &game_data);
  return game_data;
}

// Steam's VDF/ACF files use quoted key/value pairs and braces. Read within
// the supplied buffer (read_file does not promise a trailing NUL), decoding
// escaped backslashes in Windows library paths and ignoring // comments.
static int token(const char **cursor, const char *end, char *out, size_t size) {
  const char *p = *cursor;
  for (;;) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    if (end - p < 2 || p[0] != '/' || p[1] != '/') break;
    while (p < end && *p != '\n') ++p;
  }
  if (p == end) return 0;
  const char first = *p++;
  if (first == '{' || first == '}') {
    *cursor = p;
    return first;
  }
  if (first != '"') return 0;
  size_t n = 0;
  while (p < end && *p != '"') {
    char c = *p++;
    if (c == '\\' && p < end && (*p == '\\' || *p == '"')) c = *p++;
    if (n + 1 < size) out[n] = c;
    ++n;
  }
  if (p == end || n >= size) return 0;
  out[n] = 0;
  *cursor = p + 1;
  return '"';
}

static bool try_library(const ft_engine_api *engine, const char *library) {
  char path[1200], install[1024] = "TrackMania United", root[1024];
  void *data = NULL;
  size_t size = 0;
  if (join_path(path, sizeof path, library, "steamapps/appmanifest_7200.acf") &&
      engine->read_file(path, &data, &size)) {
    const char *p = data, *end = p + size;
    char key[1024], value[1024];
    int kind;
    while ((kind = token(&p, end, key, sizeof key))) {
      if (kind != '"') continue;
      if (token(&p, end, value, sizeof value) == '"' && strcmp(key, "installdir") == 0) {
        // installdir is a directory name inside steamapps/common.
        if (value[0] && !strchr(value, '/') && !strchr(value, '\\') && strcmp(value, "..") != 0)
          snprintf(install, sizeof install, "%s", value);
        break;
      }
    }
    engine->free_file_data(data);
  }
  if (!join_path(path, sizeof path, "steamapps/common", install) ||
      !join_path(root, sizeof root, library, path) || !data_present(engine, root)) return false;
  snprintf(steam_data, sizeof steam_data, "%s", root);
  return true;
}

static bool numeric_key(const char *key) {
  if (!key[0]) return false;
  for (; *key; ++key)
    if (!isdigit((unsigned char)*key)) return false;
  return true;
}

static bool try_steam(const ft_engine_api *engine, const char *root) {
  if (!root || !root[0]) return false;
  if (try_library(engine, root)) return true;
  char path[1200];
  void *data = NULL;
  size_t size = 0;
  if (!join_path(path, sizeof path, root, "steamapps/libraryfolders.vdf") ||
      !engine->read_file(path, &data, &size)) return false;
  const char *p = data, *end = p + size;
  char key[1024], value[1024];
  bool found = false;
  int kind;
  while (!found && (kind = token(&p, end, key, sizeof key))) {
    if (kind != '"') continue;
    // Modern libraries have a "path" property; older VDFs store the path
    // directly as the value of a numeric library key.
    const bool library_path = strcmp(key, "path") == 0 || numeric_key(key);
    if (token(&p, end, value, sizeof value) == '"' && library_path)
      found = try_library(engine, value);
  }
  engine->free_file_data(data);
  return found;
}

#ifdef _WIN32
static bool try_registry(const ft_engine_api *engine, HKEY hive, const char *value, REGSAM view) {
  HKEY key;
  if (RegOpenKeyExA(hive, "Software\\Valve\\Steam", 0, KEY_READ | view, &key) != ERROR_SUCCESS) return false;
  char path[1024];
  DWORD type = 0, size = sizeof path;
  const LSTATUS status = RegQueryValueExA(key, value, NULL, &type, (BYTE *)path, &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ || size == 0 || size > sizeof path) return false;
  path[sizeof path - 1] = 0;
  return try_steam(engine, path);
}
#endif

bool tm_data_find(const ft_engine_api *engine) {
  steam_data[0] = 0;
  char path[1024];
  const size_t n = engine->resolve_data_path("", path, sizeof path);
  if (n && n < sizeof path && data_present(engine, path)) return true;
  if (!engine->read_file || !engine->free_file_data) return false;
#ifdef _WIN32
  if (try_registry(engine, HKEY_CURRENT_USER, "SteamPath", 0) ||
      try_registry(engine, HKEY_LOCAL_MACHINE, "InstallPath", KEY_WOW64_32KEY) ||
      try_registry(engine, HKEY_LOCAL_MACHINE, "InstallPath", KEY_WOW64_64KEY)) return true;
  const char *vars[] = {"ProgramFiles(x86)", "ProgramFiles"};
  for (size_t i = 0; i < sizeof vars / sizeof vars[0]; ++i) {
    const char *base = getenv(vars[i]);
    if (base && base[0] && join_path(path, sizeof path, base, "Steam") && try_steam(engine, path)) return true;
  }
#elif defined(__linux__)
  const char *xdg = getenv("XDG_DATA_HOME");
  if (xdg && xdg[0] && join_path(path, sizeof path, xdg, "Steam") && try_steam(engine, path)) return true;
  const char *home = getenv("HOME");
  const char *dirs[] = {".steam/steam", ".steam/root", ".local/share/Steam",
                        ".var/app/com.valvesoftware.Steam/.local/share/Steam"};
  for (size_t i = 0; home && home[0] && i < sizeof dirs / sizeof dirs[0]; ++i)
    if (join_path(path, sizeof path, home, dirs[i]) && try_steam(engine, path)) return true;
#endif
  return false;
}

size_t tm_data_resolve_path(const ft_engine_api *engine, const char *relative, char *out, size_t out_size) {
  if (steam_data[0] && relative &&
      ((strncmp(relative, "GameData", 8) == 0 && (relative[8] == 0 || relative[8] == '/')) ||
       (strncmp(relative, "Packs", 5) == 0 && (relative[5] == 0 || relative[5] == '/')))) {
    const int n = snprintf(out, out ? out_size : 0, "%s/%s", steam_data, relative);
    return n > 0 ? (size_t)n : 0;
  }
  return engine->resolve_data_path(relative, out, out_size);
}
