#include "tmuf_data.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct file {
  char path[2048];
  const char *contents;
} file;

static file files[32];
static size_t file_count;

static void add_file(const char *root, const char *relative, const char *contents) {
  assert(file_count < sizeof files / sizeof files[0]);
  file *f = &files[file_count++];
  snprintf(f->path, sizeof f->path, "%s/%s", root, relative);
  f->contents = contents;
}

static void add_game(const char *root) {
  add_file(root, "Packs/packlist.dat", "keys");
  add_file(root, "Packs/Stadium.pak", "pack");
  add_file(root, "GameData/Tracks/example.Challenge.Gbx", "track");
}

static size_t resolve(const char *relative, char *out, size_t size) {
  return (size_t)snprintf(out, out ? size : 0, "local/%s", relative ? relative : "");
}

static bool read_file(const char *path, void **out, size_t *size) {
  for (size_t i = 0; i < file_count; ++i) {
    if (strcmp(path, files[i].path)) continue;
    *size = strlen(files[i].contents);
    *out = malloc(*size); // deliberately no trailing NUL, like the engine
    assert(*out);
    memcpy(*out, files[i].contents, *size);
    return true;
  }
  return false;
}

static uint32_t visit_directory(const char *path, ft_directory_visitor visitor, void *user) {
  size_t n = strlen(path);
  while (n && path[n - 1] == '/') --n;
  uint32_t count = 0;
  for (size_t i = 0; i < file_count; ++i) {
    const char *p = files[i].path;
    if (strncmp(path, p, n) || p[n] != '/') continue;
    p += n + 1;
    const char *slash = strchr(p, '/');
    char name[1024];
    snprintf(name, sizeof name, "%.*s", (int)(slash ? (size_t)(slash - p) : strlen(p)), p);
    const ft_directory_entry entry = {.name = name, .is_directory = slash != NULL};
    ++count;
    if (!visitor(user, &entry)) break;
  }
  return count;
}

static const ft_engine_api engine = {
    .resolve_data_path = resolve,
    .read_file = read_file,
    .free_file_data = free,
    .visit_directory = visit_directory,
};

static void check_path(const char *relative, const char *expected) {
  char path[2048];
  const size_t n = tm_data_resolve_path(&engine, relative, path, sizeof path);
  assert(strcmp(path, expected) == 0);
  assert(n == strlen(expected));
  assert(tm_data_resolve_path(&engine, relative, NULL, 0) == n);
  char short_path[4];
  assert(tm_data_resolve_path(&engine, relative, short_path, sizeof short_path) == n);
  assert(strncmp(short_path, expected, sizeof short_path - 1) == 0);
  assert(short_path[sizeof short_path - 1] == 0);
}

int main(void) {
#ifdef _WIN32
  assert(_putenv_s("ProgramFiles(x86)", "test-program") == 0);
  assert(_putenv_s("ProgramFiles", "test-program64") == 0);
  const char *steam = "test-program/Steam";
#elif defined(__linux__)
  assert(setenv("HOME", "test-home", 1) == 0);
  assert(setenv("XDG_DATA_HOME", "test-xdg", 1) == 0);
  const char *steam = "test-home/.local/share/Steam";
#endif
  assert(!tm_data_find(&engine));
  check_path("Packs", "local/Packs");
  add_game("local");
  assert(tm_data_find(&engine));
  check_path("GameData/Tracks", "local/GameData/Tracks");

#if defined(_WIN32) || defined(__linux__)
  char installed[1024];
  snprintf(installed, sizeof installed, "%s/steamapps/common/TrackMania United", steam);
  add_game(installed);
  // A complete local installation wins even when Steam is also installed.
  assert(tm_data_find(&engine));
  check_path("Packs", "local/Packs");

  file_count = 0;
  add_game(installed);
  // Partial local files must not hide a complete Steam installation.
  add_file("local", "Packs/Stadium.pak", "pack");
  add_file("local", "GameData/example.dds", "texture");
  assert(tm_data_find(&engine));
  char expected[2048];
  snprintf(expected, sizeof expected, "%s/Packs", installed);
  check_path("Packs", expected);
  snprintf(expected, sizeof expected, "%s/GameData/Tracks", installed);
  check_path("GameData/Tracks", expected);
  check_path("Documents", "local/Documents");
  check_path("README.txt", "local/README.txt");
  check_path("GameDatabase", "local/GameDatabase");

  // Additional modern libraries, comments, and a manifest's installation name.
  file_count = 0;
  add_file(steam, "steamapps/libraryfolders.vdf",
           "// library list\n\"libraryfolders\" { \"0\" { \"path\" \"missing\" } "
           "\"1\" { \"path\" \"extra library\" \"apps\" { \"7200\" \"1234\" } } }");
  add_file("extra library", "steamapps/appmanifest_7200.acf",
           "\"AppState\" { \"appid\" \"7200\" \"installdir\" \"Custom TMUF\" }");
  add_game("extra library/steamapps/common/Custom TMUF");
  assert(tm_data_find(&engine));
  check_path("Packs/packlist.dat", "extra library/steamapps/common/Custom TMUF/Packs/packlist.dat");

  // Legacy libraries and escaped Windows paths, on either platform.
  file_count = 0;
  add_file(steam, "steamapps/libraryfolders.vdf", "\"LibraryFolders\" { \"1\" \"D:\\\\Steam Library\" }");
  add_game("D:\\Steam Library/steamapps/common/TrackMania United");
  assert(tm_data_find(&engine));
  check_path("GameData", "D:\\Steam Library/steamapps/common/TrackMania United/GameData");

  // A malformed or oversized VDF cannot yield a truncated installation path.
  file_count = 0;
  add_file(steam, "steamapps/libraryfolders.vdf", "\"libraryfolders\" { \"1\" { \"path\" \"unterminated");
  assert(!tm_data_find(&engine));
  char oversized[1300];
  memset(oversized, 'x', sizeof oversized);
  memcpy(oversized, "\"libraryfolders\" { \"1\" \"", 24);
  oversized[sizeof oversized - 2] = '"';
  oversized[sizeof oversized - 1] = 0;
  file_count = 0;
  add_file(steam, "steamapps/libraryfolders.vdf", oversized);
  assert(!tm_data_find(&engine));

#ifdef __linux__
  // XDG, the common .steam symlink, and Flatpak are independent entrypoints.
  const char *roots[] = {"test-xdg/Steam", "test-home/.steam/steam", "test-home/.steam/root",
                         "test-home/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
  for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
    file_count = 0;
    snprintf(installed, sizeof installed, "%s/steamapps/common/TrackMania United", roots[i]);
    add_game(installed);
    assert(tm_data_find(&engine));
    snprintf(expected, sizeof expected, "%s/GameData", installed);
    check_path("GameData", expected);
  }
#endif
#endif
  // Rechecking after removal clears the cached Steam root.
  file_count = 0;
  assert(!tm_data_find(&engine));
  check_path("GameData", "local/GameData");
  return 0;
}
