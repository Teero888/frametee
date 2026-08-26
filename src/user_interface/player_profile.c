#include "player_profile.h"

#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <renderer/graphics_backend.h>
#include <stdlib.h>

// Presentation data about each player, in the form the ABI hands to a game: the
// editor's own track name, and whatever bytes the game last stored against that
// track. A track that has never been given a profile arrives with none, which a
// game reads as "use your defaults".
const ft_player_setup *ui_player_setups(ui_handler_t *ui, int group_index, uint32_t *out_count) {
  static ft_player_setup *setups = NULL;
  static int capacity = 0;

  const timeline_state_t *ts = &ui->timeline;
  int count = 0;
  for (int i = 0; i < ts->player_track_count; ++i)
    if (ts->player_tracks[i].group_index == group_index) ++count;

  if (out_count) *out_count = (uint32_t)count;
  if (count == 0) return NULL;

  if (count > capacity) {
    ft_player_setup *grown = realloc(setups, sizeof(*grown) * (size_t)count);
    if (!grown) {
      if (out_count) *out_count = 0;
      return NULL;
    }
    setups = grown;
    capacity = count;
  }

  int local = 0;
  for (int i = 0; i < ts->player_track_count; ++i) {
    const player_track_t *track = &ts->player_tracks[i];
    if (track->group_index != group_index) continue;

    const player_profile_t *profile = &track->player_profile;
    setups[local] = (ft_player_setup){.struct_size = sizeof(ft_player_setup),
                                      .track_name = track->name,
                                      .data = profile->size ? profile->data : NULL,
                                      .data_size = profile->size,
                                      .linked_player = game_has_cap(&ts->ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS) &&
                                                               track->is_linked
                                                           ? track->linked_source_player
                                                           : -1};
    ++local;
  }
  return setups;
}
