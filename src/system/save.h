#ifndef SAVE_H
#define SAVE_H

#include <types.h>
#include <user_interface/user_interface.h>

#define TAS_PROJECT_FILE_MAGIC "TASP"
#define TAS_PROJECT_FILE_VERSION 4

// main header for the project file
struct tas_project_header_t {
  char magic[4];
  uint32_t version;
  uint32_t map_data_size;
  uint32_t num_skins;
  uint32_t num_player_tracks;
  uint32_t timeline_data_size;
};

// header for each embedded skin
struct skin_file_header_t {
  int id;
  char name[24];
  uint32_t texture_data_size;
};

bool save_project(ui_handler_t *ui, const char *path);
bool load_project(ui_handler_t *ui, const char *path);

#endif // SAVE_H
