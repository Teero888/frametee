#include "fs.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#endif

FILE *fs_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    wchar_t wmode[16];
    
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) {
        return NULL;
    }
    
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16) <= 0) {
        return NULL;
    }
    
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

bool fs_get_config_dir(char *out_path, size_t size) {
#ifdef _WIN32
    const char *config_home = getenv("APPDATA");
    if (!config_home) config_home = getenv("USERPROFILE");
    if (config_home) {
        snprintf(out_path, size, "%s\\frametee", config_home);
        return true;
    }
#else
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home) {
        snprintf(out_path, size, "%s/frametee", config_home);
        return true;
    } else {
        config_home = getenv("HOME");
        if (config_home) {
            snprintf(out_path, size, "%s/.config/frametee", config_home);
            return true;
        }
    }
#endif
    return false;
}

// Directory scanning structure definition
struct fs_dir_t {
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATA find_data;
    bool first;
    char path[1024];
#else
    DIR *dir;
#endif
    fs_dirent_t entry;
};

fs_dir_t *fs_opendir(const char *path) {
    fs_dir_t *dir = malloc(sizeof(fs_dir_t));
    if (!dir) return NULL;
#ifdef _WIN32
    snprintf(dir->path, sizeof(dir->path), "%s\\*", path);
    dir->handle = INVALID_HANDLE_VALUE;
    dir->first = true;
#else
    dir->dir = opendir(path);
    if (!dir->dir) {
        free(dir);
        return NULL;
    }
#endif
    return dir;
}

fs_dirent_t *fs_readdir(fs_dir_t *dir) {
    if (!dir) return NULL;
#ifdef _WIN32
    if (dir->first) {
        dir->handle = FindFirstFile(dir->path, &dir->find_data);
        if (dir->handle == INVALID_HANDLE_VALUE) {
            return NULL;
        }
        dir->first = false;
    } else {
        if (FindNextFile(dir->handle, &dir->find_data) == 0) {
            return NULL;
        }
    }
    snprintf(dir->entry.name, sizeof(dir->entry.name), "%s", dir->find_data.cFileName);
    dir->entry.is_directory = (dir->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return &dir->entry;
#else
    struct dirent *ent = readdir(dir->dir);
    if (!ent) return NULL;
    snprintf(dir->entry.name, sizeof(dir->entry.name), "%s", ent->d_name);
    dir->entry.is_directory = (ent->d_type == DT_DIR);
    return &dir->entry;
#endif
}

void fs_closedir(fs_dir_t *dir) {
    if (!dir) return;
#ifdef _WIN32
    if (dir->handle != INVALID_HANDLE_VALUE) {
        FindClose(dir->handle);
    }
#else
    closedir(dir->dir);
#endif
    free(dir);
}

int fs_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

void fs_remove(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) > 0) {
        _wremove(wpath);
    }
#else
    remove(path);
#endif
}

bool fs_replace(const char *source, const char *destination) {
#ifdef _WIN32
    wchar_t wsource[1024];
    wchar_t wdestination[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, source, -1, wsource, 1024) <= 0 ||
        MultiByteToWideChar(CP_UTF8, 0, destination, -1, wdestination, 1024) <= 0) {
        return false;
    }
    return MoveFileExW(wsource, wdestination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(source, destination) == 0;
#endif
}

void *fs_load_library(const char *path) {
#ifdef _WIN32
    return LoadLibrary(path);
#else
    return dlopen(path, RTLD_LAZY);
#endif
}

void *fs_get_symbol(void *handle, const char *name) {
#ifdef _WIN32
    return GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

void fs_free_library(void *handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}
