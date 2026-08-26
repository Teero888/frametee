#ifndef PLAYER_PROFILE_H
#define PLAYER_PROFILE_H

// What a player looks like is the game's question, not the editor's: a skin and
// two tee colours in DDNet, a livery and a car elsewhere. So the editor keeps
// one identity of its own, the track's name, and stores the rest as bytes the
// game writes and reads back, never interpreting them.

#include <engine/game_host.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct player_profile_t {
  uint8_t data[FT_PLAYER_PROFILE_MAX];
  uint32_t size;
};

// Per-player presentation in the ABI's form. The returned array is valid until
// the next call and holds one entry per track in `group_index`, in track order.
const ft_player_setup *ui_player_setups(ui_handler_t *ui, int group_index, uint32_t *out_count);

#endif // PLAYER_PROFILE_H
