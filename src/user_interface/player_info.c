#include "player_info.h"

#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

// The identity panel. Everything here is engine-owned: which player is
// selected, what it is called, and the two colours a game may tint it with.
void render_player_info(struct gfx_handler_t *h) {
  ui_handler_t *ui = &h->user_interface;
  timeline_state_t *ts = &ui->timeline;
  const int track_index = ts->selected_player_track_index;
  if (track_index < 0 || track_index >= ts->player_track_count) return;

  player_track_t *track = &ts->player_tracks[track_index];
  player_info_t *info = &track->player_info;

  // Name and flags must match what the layout in imgui.ini was saved under, or
  // the panel comes back as a fresh floating window instead of docked.
  if (igBegin("Player Info", NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
    if (igInputText("Name", info->name, sizeof(info->name), 0, NULL, NULL)) ui_mark_unsaved(ui);
    if (igInputText("Tag", info->tag, sizeof(info->tag), 0, NULL, NULL)) ui_mark_unsaved(ui);

    // The label comes from the game, because only it knows what it is going to
    // look this string up in.
    if (igInputText("Appearance", info->appearance_id, sizeof(info->appearance_id), 0, NULL, NULL)) ui_mark_unsaved(ui);
    if (igIsItemHovered(0)) igSetTooltip("Passed to %s as the player's appearance id.", ui->plugin_context.active_game_id);

    if (igCheckbox("Custom colors", &info->use_custom_color)) ui_mark_unsaved(ui);
    if (info->use_custom_color) {
      if (igColorEdit4("Primary", info->primary_color, ImGuiColorEditFlags_AlphaBar)) ui_mark_unsaved(ui);
      if (igColorEdit4("Secondary", info->secondary_color, ImGuiColorEditFlags_AlphaBar)) ui_mark_unsaved(ui);
    }

    igSeparator();

    // Whatever the game marked as worth summarising about the selected player.
    game_host_t *host = &h->game_host;
    const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
    const ft_world *world = model_world_at_tick(ts, ts->current_tick);
    const int local_index = model_group_local_track_index(ts, track_index);
    if (player_class && world && local_index >= 0) {
      for (uint32_t i = 0; i < player_class->prop_count; ++i) {
        if (!(player_class->props[i].flags & FT_PROP_SUMMARY)) continue;
        ft_value value;
        if (!gh_entity_prop_get(host, world, FT_ENTITY_CLASS_PLAYER, local_index, i, &value)) continue;

        const char *label = player_class->props[i].display_name ? player_class->props[i].display_name : player_class->props[i].id;
        const char *unit = player_class->props[i].unit ? player_class->props[i].unit : "";
        switch (value.kind) {
        case FT_VALUE_BOOL: igText("%s: %s", label, value.as.b ? "yes" : "no"); break;
        case FT_VALUE_INT: igText("%s: %lld %s", label, (long long)value.as.i, unit); break;
        case FT_VALUE_FLOAT: igText("%s: %.3f %s", label, value.as.f, unit); break;
        case FT_VALUE_VEC2: igText("%s: %.2f, %.2f %s", label, value.as.v.x, value.as.v.y, unit); break;
        case FT_VALUE_VEC3: igText("%s: %.2f, %.2f, %.2f %s", label, value.as.v3.x, value.as.v3.y, value.as.v3.z, unit); break;
        case FT_VALUE_STRING: igText("%s: %s", label, value.as.s ? value.as.s : ""); break;
        default: break;
        }
      }
    }
  }
  igEnd();
}

// Presentation data the engine owns about each player, in the form the ABI
// hands to a game. Names, colours and appearance choice are the editor's to
// store; what a game does with them is not.
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

    const player_info_t *info = &track->player_info;
    setups[local] = (ft_player_setup){.struct_size = sizeof(ft_player_setup),
                                      .name = info->name,
                                      .tag = info->tag,
                                      .appearance_id = info->appearance_id,
                                      .primary_color = {info->primary_color[0], info->primary_color[1], info->primary_color[2],
                                                        info->primary_color[3]},
                                      .secondary_color = {info->secondary_color[0], info->secondary_color[1], info->secondary_color[2],
                                                          info->secondary_color[3]},
                                      .use_custom_color = info->use_custom_color,
                                      .linked_player = game_has_cap(&ts->ui->gfx_handler->game_host, FT_CAP_LINKED_INPUTS) && track->is_linked
                                                           ? track->linked_source_player
                                                           : -1};
    ++local;
  }
  return setups;
}
