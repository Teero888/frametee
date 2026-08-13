#ifndef FS_H
#define FS_H

#include <stdio.h>
#include <stdbool.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#define PATH_SEP '/'
#endif

/**
 * @brief Opens a file with UTF-8 path support.
 * 
 * On Windows, this converts the UTF-8 path and mode to UTF-16 and uses _wfopen.
 * On other platforms, it calls fopen directly.
 */
FILE *fs_open(const char *path, const char *mode);

/**
 * @brief Gets the config directory path.
 * 
 * Returns true if successful and out_path contains the path.
 */
bool fs_get_config_dir(char *out_path, size_t size);

/**
 * @brief Platform-independent directory scanning types and functions.
 */
typedef struct fs_dir_t fs_dir_t;

typedef struct {
    char name[256];
    bool is_directory;
} fs_dirent_t;

fs_dir_t *fs_opendir(const char *path);
fs_dirent_t *fs_readdir(fs_dir_t *dir);
void fs_closedir(fs_dir_t *dir);

/**
 * @brief Platform-independent directory creation.
 */
int fs_mkdir(const char *path);

/**
 * @brief Platform-independent file removal.
 */
void fs_remove(const char *path);

// Atomically replaces destination with source when the platform supports it.
// Both paths are UTF-8. Source is left in place on failure.
bool fs_replace(const char *source, const char *destination);

/**
 * @brief Platform-independent dynamic library loading.
 */
void *fs_load_library(const char *path);
void *fs_get_symbol(void *handle, const char *name);
void fs_free_library(void *handle);

#endif // FS_H
