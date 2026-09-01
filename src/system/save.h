#ifndef SAVE_H
#define SAVE_H

#include <types.h>
#include <user_interface/user_interface.h>

#define TAS_PROJECT_FILE_MAGIC "TASP"
#define TAS_PROJECT_FILE_VERSION 17

bool save_project(ui_handler_t *ui, const char *path);
bool load_project(ui_handler_t *ui, const char *path);
bool import_project_as_group(ui_handler_t *ui, const char *path);

#endif // SAVE_H
