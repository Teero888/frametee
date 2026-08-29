#ifndef SAVE_H
#define SAVE_H

#include <types.h>
#include <user_interface/user_interface.h>

#define TAS_PROJECT_FILE_MAGIC "TASP"
// 10 is the first portable game-module project format. Every engine-owned
// value has an explicit little-endian representation; game-owned levels,
// worlds and project metadata remain opaque length-delimited blobs.
// Pre-release v9 files are intentionally rejected rather than guessed at.
// 12 adds engine-owned prediction-line settings and per-group/track scope.
// 13 persists game-defined payloads on authored timeline events.
// 14 adds retained snippet source windows. 15 introduced snippet effects;
// 16 stores their game-defined ids and opaque parameter payloads.
#define TAS_PROJECT_FILE_VERSION 16

bool save_project(ui_handler_t *ui, const char *path);
bool load_project(ui_handler_t *ui, const char *path);
bool import_project_as_group(ui_handler_t *ui, const char *path);

#endif // SAVE_H
