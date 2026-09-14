#include "player_profile.h"

#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <renderer/graphics_backend.h>
#include <stdlib.h>

typedef struct {
  uint32_t offset;
  uint32_t count;
} group_setup_slice_t;

static ft_player_setup *s_all_setups = NULL;
static size_t s_all_setups_capacity = 0;
static group_setup_slice_t *s_group_slices = NULL;
static int s_group_slices_capacity = 0;
static int s_group_slices_count = 0;
static bool s_prepared = false;

void ui_player_setups_prepare(ui_handler_t *ui) {
  if (!ui) return;
  const timeline_state_t *ts = &ui->timeline;
  const int group_count = ts->group_count;
  const int track_count = ts->player_track_count;

  if (group_count > s_group_slices_capacity) {
    int cap = group_count > 0 ? group_count : 16;
    if (cap < s_group_slices_capacity * 2) cap = s_group_slices_capacity * 2;
    group_setup_slice_t *grown = realloc(s_group_slices, sizeof(*grown) * (size_t)cap);
    if (!grown) return;
    s_group_slices = grown;
    s_group_slices_capacity = cap;
  }
  s_group_slices_count = group_count;
  if (group_count > 0) {
    memset(s_group_slices, 0, sizeof(*s_group_slices) * (size_t)group_count);
  }

  // Count tracks per group
  for (int i = 0; i < track_count; ++i) {
    int g = ts->player_tracks[i].group_index;
    if (g >= 0 && g < group_count) {
      s_group_slices[g].count++;
    }
  }

  // Prefix sum for offsets
  uint32_t total = 0;
  for (int g = 0; g < group_count; ++g) {
    s_group_slices[g].offset = total;
    total += s_group_slices[g].count;
  }

  if ((size_t)total > s_all_setups_capacity) {
    size_t cap = total > 0 ? (size_t)total : 16;
    if (cap < s_all_setups_capacity * 2) cap = s_all_setups_capacity * 2;
    ft_player_setup *grown = realloc(s_all_setups, sizeof(*grown) * cap);
    if (!grown) return;
    s_all_setups = grown;
    s_all_setups_capacity = cap;
  }

  // Cursor for filling in order
  uint32_t *cursors = NULL;
  uint32_t stack_cursors[64];
  if (group_count <= 64) {
    cursors = stack_cursors;
  } else {
    cursors = malloc(sizeof(uint32_t) * (size_t)group_count);
    if (!cursors) return;
  }
  for (int g = 0; g < group_count; ++g) {
    cursors[g] = s_group_slices[g].offset;
  }

  const bool supports_linked = game_has_cap(&ts->ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS);
  for (int i = 0; i < track_count; ++i) {
    const player_track_t *track = &ts->player_tracks[i];
    int g = track->group_index;
    if (g >= 0 && g < group_count) {
      const player_profile_t *profile = &track->player_profile;
      uint32_t idx = cursors[g]++;
      s_all_setups[idx] = (ft_player_setup){
          .struct_size = sizeof(ft_player_setup),
          .track_name = track->name,
          .data = profile->size ? profile->data : NULL,
          .data_size = profile->size,
          .linked_player = supports_linked && track->is_linked ? track->linked_source_player : -1};
    }
  }

  if (cursors != stack_cursors) free(cursors);
  s_prepared = true;
}

// Presentation data about each player, in the form the ABI hands to a game: the
// editor's own track name, and whatever bytes the game last stored against that
// track. A track that has never been given a profile arrives with none, which a
// game reads as "use your defaults".
const ft_player_setup *ui_player_setups(ui_handler_t *ui, int group_index, uint32_t *out_count) {
  if (!s_prepared) {
    ui_player_setups_prepare(ui);
  }
  if (!s_all_setups || group_index < 0 || group_index >= s_group_slices_count) {
    if (out_count) *out_count = 0;
    return NULL;
  }
  if (out_count) *out_count = s_group_slices[group_index].count;
  if (s_group_slices[group_index].count == 0) return NULL;
  return &s_all_setups[s_group_slices[group_index].offset];
}
