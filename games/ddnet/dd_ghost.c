#include "dd_imgui.h"
#include "dd_internal.h"
#include "dd_profile.h"

#include <ddnet_ghost/ghost.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int round_to_int(float f) {
  return (int)roundf(f);
}

static const char *world_name(ft_game *game, int world) {
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  game->engine->timeline_world_info((uint32_t)world, &info);
  return info.name;
}

static const char *track_name(ft_game *game, int world, int local, char *fallback, size_t fallback_size) {
  const int track = game->engine->timeline_player_track ? game->engine->timeline_player_track((uint32_t)world, (uint32_t)local) : -1;
  if (track >= 0) {
    dd_profile_display_name(game, track, fallback, fallback_size);
    return fallback;
  }
  snprintf(fallback, fallback_size, "Track %d", local + 1);
  return fallback;
}

bool dd_ghost_scan_track(ft_game *game, int world_index, int local_player,
                         int *out_start_tick, int *out_end_tick,
                         float *out_time, bool *out_has_finish) {
  if (out_start_tick) *out_start_tick = 0;
  if (out_end_tick) *out_end_tick = 0;
  if (out_time) *out_time = 0.0f;
  if (out_has_finish) *out_has_finish = false;

  if (!game || !game->engine || !game->engine->timeline_world_pair ||
      !game->engine->timeline_range || !game->engine->timeline_world_info) {
    return false;
  }
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (!game->engine->timeline_world_info((uint32_t)world_index, &info) ||
      local_player < 0 || (uint32_t)local_player >= info.player_count) {
    return false;
  }

  int32_t first_tick = 0;
  int32_t last_tick = 0;
  if (!game->engine->timeline_range(&first_tick, &last_tick)) {
    return false;
  }

  // Scan backwards from last_tick down to first_tick + 1 for finish line crossing
  for (int32_t tick = last_tick; tick > first_tick; --tick) {
    const ft_world *prev = NULL;
    const ft_world *curr = NULL;
    if (!game->engine->timeline_world_pair((uint32_t)world_index, tick, &prev, &curr) || !prev || !curr) {
      continue;
    }
    const int client_id = ddnet_player_client(curr, local_player);
    if (client_id < 0 || ddnet_player_client(prev, local_player) != client_id) {
      continue;
    }
    const ddnet_player_t *before = &prev->core.players[client_id];
    const ddnet_player_t *after = &curr->core.players[client_id];

    if (before->finish_tick < 0 && after->finish_tick >= 0) {
      int global_finish = tick;
      int global_start = after->finish_tick - after->finish_time_ticks + info.start_offset;
      if (global_start < first_tick) global_start = first_tick;
      if (global_start >= global_finish) global_start = (global_finish > first_tick) ? global_finish - 1 : first_tick;

      if (out_start_tick) *out_start_tick = global_start;
      if (out_end_tick) *out_end_tick = global_finish;
      if (out_time) {
        *out_time = (float)after->finish_time_ticks / (float)GAME_TICK_SPEED;
      }
      if (out_has_finish) *out_has_finish = true;
      return true;
    }
  }

  // No finish transition found; find the last start if one exists
  int last_start = -1;
  for (int32_t tick = last_tick; tick >= first_tick; --tick) {
    const ft_world *prev = NULL;
    const ft_world *curr = NULL;
    if (!game->engine->timeline_world_pair((uint32_t)world_index, tick, &prev, &curr) || !curr) {
      continue;
    }
    const ddnet_character_t *c = ddnet_player_character(curr, local_player);
    if (!c) continue;
    if (c->race_state != DDNET_RACE_NONE) {
      last_start = c->start_time + info.start_offset;
      break;
    }
  }

  if (out_start_tick) *out_start_tick = (last_start >= 0) ? last_start : first_tick;
  if (out_end_tick) *out_end_tick = last_tick;
  if (out_time) *out_time = 0.0f;
  if (out_has_finish) *out_has_finish = false;
  return false;
}

bool dd_ghost_export(ft_game *game, int world_index, int local_player, int start_tick, int end_tick, const char *path) {
  start_tick -= 1;
  if (!game || !path || !game->engine || !game->engine->timeline_world_info ||
      !game->engine->timeline_world_pair || !game->engine->timeline_player_track ||
      end_tick <= start_tick || world_index < 0 || local_player < 0) {
    return false;
  }

  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (!game->engine->timeline_world_info((uint32_t)world_index, &info) ||
      (uint32_t)local_player >= info.player_count) {
    dd_log(game, FT_LOG_ERROR, "Invalid world or track for ghost export.");
    return false;
  }

  const int32_t track = game->engine->timeline_player_track((uint32_t)world_index, (uint32_t)local_player);
  dd_player_profile_t profile;
  dd_profile_for_track(game, track, &profile);
  const char *player_name = profile.name[0] ? profile.name : "nameless tee";

  const char *map_name = (game->current_level && game->current_level->name[0] && strcmp(game->current_level->name, "map") != 0)
                             ? game->current_level->name
                             : (game->engine && game->engine->get_level_name ? game->engine->get_level_name() : NULL);
  if (!map_name || !*map_name || strcmp(map_name, "unnamed_level") == 0) map_name = "unnamed_map";

  // Determine race time
  int time_ms = 0;
  const ft_world *prev = NULL;
  const ft_world *curr = NULL;
  const int finished_client = game->engine->timeline_world_pair((uint32_t)world_index, end_tick, &prev, &curr) && curr
                                  ? ddnet_player_client(curr, local_player)
                                  : -1;
  if (finished_client >= 0 && curr->core.players[finished_client].finish_tick >= 0) {
    time_ms = (int)roundf((float)curr->core.players[finished_client].finish_time_ticks * 1000.0f / (float)GAME_TICK_SPEED);
  } else {
    float dur = (float)(end_tick - start_tick) / (float)GAME_TICK_SPEED;
    time_ms = (int)roundf(dur * 1000.0f);
  }

  ghost_t *ghost = ghost_create();
  if (!ghost) {
    dd_log(game, FT_LOG_ERROR, "Could not initialize DDNet ghost.");
    return false;
  }

  ghost_set_meta(ghost, player_name, map_name, time_ms);
  ghost_set_skin(ghost, profile.skin[0] ? profile.skin : "default",
                 profile.use_custom_color, (int)profile.color_body, (int)profile.color_feet);
  ghost->start_tick = 0;

  const int local_start = start_tick - info.start_offset;
  for (int tick = start_tick + 1; tick <= end_tick; ++tick) {
    curr = NULL;
    prev = NULL;
    if (!game->engine->timeline_world_pair((uint32_t)world_index, tick, &prev, &curr) || !curr) {
      dd_log(game, FT_LOG_ERROR, "Ghost export failed: could not sample timeline at tick %d.", tick);
      ghost_free(ghost);
      return false;
    }
    const ddnet_character_t *c_cur = ddnet_player_character(curr, local_player);
    if (!c_cur) {
      // Dead for a tick or two: a ghost has no gaps, so it stays where it was.
      continue;
    }
    ghost_character_t snap;
    memset(&snap, 0, sizeof(snap));

    snap.x = round_to_int(c_cur->pos.x);
    snap.y = round_to_int(c_cur->pos.y);
    snap.vel_x = round_to_int(c_cur->core.vel.x * 256.0f);
    snap.vel_y = 0;

    snap.angle = ddnet_character_angle(c_cur);
    snap.direction = c_cur->core.input.direction;

    const bool frozen = (c_cur->core.deep_frozen || c_cur->freeze_time > 0);
    snap.weapon = frozen ? DDNET_WEAPON_NINJA : c_cur->core.active_weapon;

    snap.hook_state = c_cur->core.hook_state;
    snap.hook_x = round_to_int(c_cur->core.hook_pos.x);
    snap.hook_y = round_to_int(c_cur->core.hook_pos.y);

    if (c_cur->attack_tick > local_start)
      snap.attack_tick = c_cur->attack_tick - local_start;
    else
      snap.attack_tick = -9999;

    snap.tick = tick - start_tick;

    ghost_add_snap(ghost, &snap);
  }

  ghost->start_tick = 0;
  const int res = ghost_save(ghost, path);
  ghost_free(ghost);

  if (res != 0) {
    dd_log(game, FT_LOG_ERROR, "Failed to save ghost file to '%s'.", path);
    return false;
  }
  return true;
}

bool dd_ghost_export_request(ft_game *game, const ft_export_request *request) {
  if (!game || !request || !request->path || !game->engine) return false;
  int world_index = 0;
  int local_player = 0;
  if (request->player_count > 0 && request->players && game->engine->timeline_world_count &&
      game->engine->timeline_world_info && game->engine->timeline_player_track) {
    const int32_t target_track = request->players[0];
    const uint32_t world_count = game->engine->timeline_world_count();
    bool found = false;
    for (uint32_t wi = 0; !found && wi < world_count; ++wi) {
      ft_timeline_world_info info = {.struct_size = sizeof(info)};
      if (!game->engine->timeline_world_info(wi, &info)) continue;
      for (uint32_t local = 0; local < info.player_count; ++local) {
        if (game->engine->timeline_player_track(wi, local) == target_track) {
          world_index = (int)wi;
          local_player = (int)local;
          found = true;
          break;
        }
      }
    }
  } else if (game->engine->timeline_active_world) {
    int active = game->engine->timeline_active_world();
    if (active >= 0) world_index = active;
  }

  int start_tick = request->start_tick;
  int end_tick = request->end_tick;
  if (start_tick >= end_tick) {
    float detected_time = 0.0f;
    bool has_finish = false;
    if (!dd_ghost_scan_track(game, world_index, local_player, &start_tick, &end_tick, &detected_time, &has_finish) || !has_finish) {
      return false;
    }
  }
  return dd_ghost_export(game, world_index, local_player, start_tick, end_tick, request->path);
}

void dd_ghost_export_window_open(ft_game *game) {
  if (!game) return;
  game->ghost_export_error[0] = '\0';
  int active = (game->engine && game->engine->timeline_active_world) ? game->engine->timeline_active_world() : 0;
  if (active < 0) active = 0;
  game->ghost_export_world = active;
  game->ghost_export_track = 0;

  if (game->engine && game->engine->timeline_world_count && game->engine->timeline_world_info) {
    uint32_t wc = game->engine->timeline_world_count();
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (game->ghost_export_world >= (int)wc || !game->engine->timeline_world_info((uint32_t)game->ghost_export_world, &info) || info.player_count == 0) {
      for (uint32_t w = 0; w < wc; ++w) {
        if (game->engine->timeline_world_info(w, &info) && info.player_count > 0) {
          game->ghost_export_world = (int)w;
          break;
        }
      }
    }
  }

  dd_ghost_scan_track(game, game->ghost_export_world, game->ghost_export_track,
                      &game->ghost_export_start_tick, &game->ghost_export_end_tick,
                      &game->ghost_export_detected_time, &game->ghost_export_has_finish);
  game->ghost_export_detected_start = game->ghost_export_start_tick;
  game->ghost_export_detected_end = game->ghost_export_end_tick;
  game->open_ghost_export = true;
}

void dd_ghost_export_window_render(ft_game *game) {
  if (!game) return;
  if (game->open_ghost_export) {
    igOpenPopup_Str("Export DDNet Ghost", ImGuiPopupFlags_None);
    game->open_ghost_export = false;
  }

  igSetNextWindowSize((ImVec2){560.f, 540.f}, ImGuiCond_FirstUseEver);
  if (!igBeginPopupModal("Export DDNet Ghost", NULL, ImGuiWindowFlags_None)) return;

  if (!game->engine->timeline_world_count || !game->engine->timeline_world_info) {
    igTextColored((ImVec4){1.f, 0.35f, 0.3f, 1.f}, "Could not query timeline.");
    if (igButton("Close", (ImVec2){100.f, 0.f})) igCloseCurrentPopup();
    igEndPopup();
    return;
  }

  igTextWrapped("Select a group and track to export from start to finish as a DDNet ghost (.gho) file.");
  igSeparator();

  igText("Tracks:");
  const uint32_t world_count = game->engine->timeline_world_count();
  if (igBeginChild_Str("##ddnet_ghost_track_list", (ImVec2){0, -128.f}, true, 0)) {
    for (uint32_t w = 0; w < world_count; ++w) {
      ft_timeline_world_info info = {.struct_size = sizeof(info)};
      if (!game->engine->timeline_world_info(w, &info)) continue;
      igTextDisabled("%s", world_name(game, (int)w));
      igIndent(4.f);
      for (uint32_t local = 0; local < info.player_count; ++local) {
        char tname[64];
        track_name(game, (int)w, (int)local, tname, sizeof(tname));
        igPushID_Int((int)(w * 1000 + local));
        const bool is_selected = (game->ghost_export_world == (int)w && game->ghost_export_track == (int)local);
        if (igRadioButton_Bool(tname, is_selected)) {
          if (!is_selected) {
            game->ghost_export_world = (int)w;
            game->ghost_export_track = (int)local;
            game->ghost_export_error[0] = '\0';
            dd_ghost_scan_track(game, game->ghost_export_world, game->ghost_export_track,
                                &game->ghost_export_start_tick, &game->ghost_export_end_tick,
                                &game->ghost_export_detected_time, &game->ghost_export_has_finish);
            game->ghost_export_detected_start = game->ghost_export_start_tick;
            game->ghost_export_detected_end = game->ghost_export_end_tick;
          }
        }
        igPopID();
      }
      igUnindent(4.f);
      igSeparator();
    }
  }
  igEndChild();

  igSpacing();
  if (game->ghost_export_has_finish) {
    const int minutes = (int)game->ghost_export_detected_time / 60;
    const float seconds = fmodf(game->ghost_export_detected_time, 60.0f);
    igTextColored((ImVec4){0.3f, 1.f, 0.4f, 1.f}, "Run detected: %d:%06.3f (ticks %d to %d)",
                  minutes, seconds, game->ghost_export_detected_start, game->ghost_export_detected_end);
  } else {
    igTextColored((ImVec4){1.f, 0.5f, 0.3f, 1.f}, "No finish line crossing detected on this track.");
  }

  igSetNextItemWidth(150.f);
  igDragInt("Start tick", &game->ghost_export_start_tick, 1.f, -100000000, 100000000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSameLine(0, 12.f);
  igSetNextItemWidth(150.f);
  igDragInt("End tick (inclusive)", &game->ghost_export_end_tick, 1.f, -100000000, 100000000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSameLine(0, 12.f);
  if (igButton("Reset to Run", (ImVec2){0, 0})) {
    dd_ghost_scan_track(game, game->ghost_export_world, game->ghost_export_track,
                        &game->ghost_export_start_tick, &game->ghost_export_end_tick,
                        &game->ghost_export_detected_time, &game->ghost_export_has_finish);
    game->ghost_export_detected_start = game->ghost_export_start_tick;
    game->ghost_export_detected_end = game->ghost_export_end_tick;
  }

  const int span_ticks = game->ghost_export_end_tick - game->ghost_export_start_tick;
  if (span_ticks > 0) {
    float display_time;
    if (game->ghost_export_has_finish &&
        game->ghost_export_start_tick == game->ghost_export_detected_start &&
        game->ghost_export_end_tick == game->ghost_export_detected_end) {
      display_time = game->ghost_export_detected_time;
    } else {
      display_time = (float)span_ticks / (float)GAME_TICK_SPEED;
    }
    const int m = (int)display_time / 60;
    const float s = fmodf(display_time, 60.0f);
    igText("Ghost duration: %d ticks (~%d:%06.3f)", span_ticks, m, s);
  }

  if (game->ghost_export_error[0]) {
    igTextColored((ImVec4){1.f, 0.35f, 0.3f, 1.f}, "%s", game->ghost_export_error);
  }

  igSeparator();

  const bool valid = (game->ghost_export_has_finish && game->ghost_export_end_tick > game->ghost_export_start_tick &&
                      game->ghost_export_world >= 0 && game->ghost_export_track >= 0);

  if (!valid) igBeginDisabled(true);
  if (igButton("Export...", (ImVec2){120.f, 0.f})) {
    int32_t track = game->engine->timeline_player_track((uint32_t)game->ghost_export_world, (uint32_t)game->ghost_export_track);
    dd_player_profile_t profile;
    dd_profile_for_track(game, track, &profile);
    const char *player_name = profile.name[0] ? profile.name : "nameless";
    const char *map_name = (game->current_level && game->current_level->name[0] && strcmp(game->current_level->name, "map") != 0)
                               ? game->current_level->name
                               : (game->engine && game->engine->get_level_name ? game->engine->get_level_name() : NULL);
    if (!map_name || !*map_name || strcmp(map_name, "unnamed_level") == 0) map_name = "run";

    float time_sec;
    if (game->ghost_export_has_finish &&
        game->ghost_export_start_tick == game->ghost_export_detected_start &&
        game->ghost_export_end_tick == game->ghost_export_detected_end) {
      time_sec = game->ghost_export_detected_time;
    } else {
      time_sec = (float)(game->ghost_export_end_tick - game->ghost_export_start_tick) / (float)GAME_TICK_SPEED;
    }
    int time_ms = (int)roundf(time_sec * 1000.0f);

    char default_name[256];
    snprintf(default_name, sizeof(default_name), "%s_%s_%d.%03d.gho", map_name, player_name, time_ms / 1000, time_ms % 1000);

    char path[1024];
    if (game->engine->save_file_dialog && game->engine->save_file_dialog("DDNet Ghost", "gho", default_name, path, sizeof(path))) {
      if (dd_ghost_export(game, game->ghost_export_world, game->ghost_export_track,
                          game->ghost_export_start_tick, game->ghost_export_end_tick, path)) {
        dd_log(game, FT_LOG_INFO, "Exported DDNet ghost to '%s'.", path);
        game->ghost_export_error[0] = '\0';
        igCloseCurrentPopup();
      } else {
        snprintf(game->ghost_export_error, sizeof(game->ghost_export_error), "Ghost export failed; see the log for details.");
      }
    }
  }
  if (!valid) igEndDisabled();

  igSameLine(0, 10.f);
  if (igButton("Cancel", (ImVec2){100.f, 0.f})) {
    game->ghost_export_error[0] = '\0';
    igCloseCurrentPopup();
  }

  igEndPopup();
}
