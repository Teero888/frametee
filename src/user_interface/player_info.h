#ifndef PLAYER_INFO_H
#define PLAYER_INFO_H

#include <renderer/renderer.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct player_info_t {
  char name[16];
  char clan[12];
  int skin; // id
  uint32_t color_body;
  uint32_t color_feet;
  bool use_custom_color;
};

struct skin_info_t {
  char name[24];
  char path[512];
  void *data;
  size_t data_size;
  int id;
  texture_t *preview_texture_res;
  struct ImTextureRef *preview_texture;
};

struct skin_manager_t {
  int num_skins;
  skin_info_t *skins; // heap array
};

void render_player_info(struct gfx_handler_t *h);

void skin_manager_init(skin_manager_t *m);
int skin_manager_add(skin_manager_t *m, const skin_info_t *skin);
int skin_manager_remove(skin_manager_t *m, struct gfx_handler_t *h, int index);
void skin_manager_free(skin_manager_t *m);
#endif // PLAYER_INFO_H
