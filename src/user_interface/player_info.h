#ifndef PLAYER_INFO_H
#define PLAYER_INFO_H

// Per-player presentation the editor owns. All names are deliberately generic:
// a tag may be a team/clan/number and an appearance may be a skin, livery or
// character depending on the active game.

#include <engine/game_host.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct player_info_t {
  char name[16];
  char tag[16];
  char appearance_id[64];
  float primary_color[4];
  float secondary_color[4];
  bool use_custom_color;
};

void render_player_info(struct gfx_handler_t *h);

// Per-player presentation in the ABI's form. The returned array is valid until
// the next call and holds one entry per track in `group_index`, in track order.
const ft_player_setup *ui_player_setups(ui_handler_t *ui, int group_index, uint32_t *out_count);

#endif // PLAYER_INFO_H
