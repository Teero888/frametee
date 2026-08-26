#include "dd_internal.h"
#include "dd_profile.h"

#include "dd_imgui.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_worlds(dd_demo_export_world_t *worlds, int count) {
  if (!worlds) return;
  for (int i = 0; i < count; ++i) {
    free(worlds[i].tracks);
    free(worlds[i].pings);
  }
  free(worlds);
}

void dd_export_window_cleanup(ft_game *game) {
  if (!game) return;
  free_worlds(game->demo_export_worlds, game->demo_export_world_count);
  game->demo_export_worlds = NULL;
  game->demo_export_world_count = 0;
}

static bool selection_matches(ft_game *game, int world_count) {
  if (game->demo_export_world_count != world_count) return false;
  for (int world = 0; world < world_count; ++world) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (!game->engine->timeline_world_info((uint32_t)world, &info) || info.player_count > INT_MAX ||
        game->demo_export_worlds[world].track_count != (int)info.player_count)
      return false;
  }
  return true;
}

static bool sync_selection(ft_game *game) {
  if (!game || !game->engine->timeline_world_count || !game->engine->timeline_world_info) return false;
  const uint32_t world_count_u32 = game->engine->timeline_world_count();
  if (world_count_u32 > INT_MAX) return false;
  const int world_count = (int)world_count_u32;
  if (selection_matches(game, world_count)) return true;

  dd_demo_export_world_t *next = world_count > 0 ? calloc((size_t)world_count, sizeof(*next)) : NULL;
  if (world_count > 0 && !next) return false;

  for (int world = 0; world < world_count; ++world) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (!game->engine->timeline_world_info((uint32_t)world, &info) || info.player_count > INT_MAX) {
      free_worlds(next, world_count);
      return false;
    }
    dd_demo_export_world_t *selection = &next[world];
    selection->enabled = world < game->demo_export_world_count ? game->demo_export_worlds[world].enabled : true;
    selection->track_count = (int)info.player_count;
    if (selection->track_count == 0) continue;
    selection->tracks = calloc((size_t)selection->track_count, sizeof(*selection->tracks));
    selection->pings = calloc((size_t)selection->track_count, sizeof(*selection->pings));
    if (!selection->tracks || !selection->pings) {
      free_worlds(next, world_count);
      return false;
    }
    for (int local = 0; local < selection->track_count; ++local)
      selection->tracks[local] = true;
    if (world < game->demo_export_world_count) {
      const dd_demo_export_world_t *previous = &game->demo_export_worlds[world];
      const int copy_count = previous->track_count < selection->track_count ? previous->track_count : selection->track_count;
      if (copy_count > 0) {
        memcpy(selection->tracks, previous->tracks, sizeof(*selection->tracks) * (size_t)copy_count);
        memcpy(selection->pings, previous->pings, sizeof(*selection->pings) * (size_t)copy_count);
      }
    }
  }

  free_worlds(game->demo_export_worlds, game->demo_export_world_count);
  game->demo_export_worlds = next;
  game->demo_export_world_count = world_count;
  return true;
}

void dd_export_window_open(ft_game *game) {
  if (!game) return;
  int32_t first_tick = 0;
  int32_t last_tick = 0;
  if (game->engine->timeline_range) game->engine->timeline_range(&first_tick, &last_tick);
  game->demo_export_start_tick = first_tick;
  game->demo_export_end_tick = last_tick + GAME_TICK_SPEED;
  game->demo_export_error[0] = '\0';
  if (!sync_selection(game))
    snprintf(game->demo_export_error, sizeof(game->demo_export_error), "Could not read the DDNet timeline.");
  game->open_demo_export = true;
}

static const char *world_name(ft_game *game, int world, char *fallback, size_t fallback_size) {
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (game->engine->timeline_world_info((uint32_t)world, &info) && info.name && info.name[0]) return info.name;
  snprintf(fallback, fallback_size, "Group %d", world + 1);
  return fallback;
}

static const char *track_name(ft_game *game, int world, int local, char *fallback, size_t fallback_size) {
  const int track = game->engine->timeline_player_track((uint32_t)world, (uint32_t)local);
  if (track >= 0) {
    dd_profile_display_name(game, track, fallback, fallback_size);
    return fallback;
  }
  snprintf(fallback, fallback_size, "Track %d", local + 1);
  return fallback;
}

void dd_export_window_render(ft_game *game) {
  if (!game) return;
  if (game->open_demo_export) {
    igOpenPopup_Str("Export DDNet Demo", ImGuiPopupFlags_None);
    game->open_demo_export = false;
  }

  igSetNextWindowSize((ImVec2){620.f, 640.f}, ImGuiCond_FirstUseEver);
  if (!igBeginPopupModal("Export DDNet Demo", NULL, ImGuiWindowFlags_None)) return;

  const bool selection_ok = sync_selection(game);
  igTextWrapped("Choose the inclusive global tick range and the DDNet groups/tracks to combine in the demo.");
  igSetNextItemWidth(150.f);
  igDragInt("Start tick", &game->demo_export_start_tick, 1.f, -100000000, 100000000, "%d", ImGuiSliderFlags_AlwaysClamp);
  igSameLine(0, 12.f);
  igSetNextItemWidth(150.f);
  igDragInt("End tick (inclusive)", &game->demo_export_end_tick, 1.f, -100000000, 100000000, "%d",
            ImGuiSliderFlags_AlwaysClamp);
  igSeparator();

  int selected_count = 0;
  if (igBeginChild_Str("##ddnet_export_tracks", (ImVec2){0, -82.f}, true, 0)) {
    for (int world = 0; selection_ok && world < game->demo_export_world_count; ++world) {
      dd_demo_export_world_t *selection = &game->demo_export_worlds[world];
      igPushID_Int(world);
      char fallback[32];
      igCheckbox(world_name(game, world, fallback, sizeof(fallback)), &selection->enabled);
      if (selection->enabled) {
        igIndent(18.f);
        for (int local = 0; local < selection->track_count; ++local) {
          igPushID_Int(local + 10000);
          char track_fallback[32];
          igCheckbox(track_name(game, world, local, track_fallback, sizeof(track_fallback)), &selection->tracks[local]);
          igSameLine(0, 12.f);
          igSetNextItemWidth(82.f);
          igDragInt("Ping", &selection->pings[local], 1.f, 0, 999, "%d ms", ImGuiSliderFlags_AlwaysClamp);
          if (selection->tracks[local]) ++selected_count;
          igPopID();
        }
        igUnindent(18.f);
      }
      igSeparator();
      igPopID();
    }
  }
  igEndChild();

  igText("Selected tracks: %d / 64", selected_count);
  if (!selection_ok) snprintf(game->demo_export_error, sizeof(game->demo_export_error), "Could not read the DDNet timeline.");
  if (game->demo_export_error[0]) {
    igSameLine(0, 12.f);
    igTextColored((ImVec4){1.f, 0.35f, 0.3f, 1.f}, "%s", game->demo_export_error);
  }

  const bool valid = selection_ok && selected_count > 0 && selected_count <= 64 &&
                     game->demo_export_end_tick >= game->demo_export_start_tick;
  if (!valid) igBeginDisabled(true);
  if (igButton("Export...", (ImVec2){120.f, 0.f})) {
    int32_t *players = malloc(sizeof(*players) * (size_t)selected_count);
    int32_t *pings = malloc(sizeof(*pings) * (size_t)selected_count);
    int player_count = 0;
    if (players && pings) {
      for (int world = 0; world < game->demo_export_world_count; ++world) {
        const dd_demo_export_world_t *selection = &game->demo_export_worlds[world];
        if (!selection->enabled) continue;
        for (int local = 0; local < selection->track_count; ++local) {
          if (!selection->tracks[local]) continue;
          players[player_count] = game->engine->timeline_player_track((uint32_t)world, (uint32_t)local);
          pings[player_count++] = selection->pings[local];
        }
      }
    }

    char default_name[256];
    snprintf(default_name, sizeof(default_name), "%s.demo",
             game->current_level && game->current_level->name[0] ? game->current_level->name : "run");
    char path[1024];
    if (!players || !pings) {
      snprintf(game->demo_export_error, sizeof(game->demo_export_error), "Could not allocate the track selection.");
    } else if (game->engine->save_file_dialog && game->engine->save_file_dialog("DDNet Demo", "demo", default_name, path, sizeof(path))) {
      const ft_export_request request = {.struct_size = sizeof(request),
                                         .path = path,
                                         .start_tick = game->demo_export_start_tick,
                                         .end_tick = game->demo_export_end_tick,
                                         .players = players,
                                         .player_count = (uint32_t)player_count};
      if (dd_demo_export_with_pings(game, &request, pings)) {
        dd_log(game, FT_LOG_INFO, "Exported DDNet demo to '%s'.", path);
        game->demo_export_error[0] = '\0';
        igCloseCurrentPopup();
      } else {
        snprintf(game->demo_export_error, sizeof(game->demo_export_error), "Export failed; see the log for details.");
      }
    }
    free(players);
    free(pings);
  }
  if (!valid) igEndDisabled();
  igSameLine(0, 10.f);
  if (igButton("Cancel", (ImVec2){100.f, 0.f})) {
    game->demo_export_error[0] = '\0';
    igCloseCurrentPopup();
  }
  igEndPopup();
}

static void write_u32(unsigned char **cursor, uint32_t value) {
  (*cursor)[0] = (unsigned char)(value & 0xffu);
  (*cursor)[1] = (unsigned char)((value >> 8) & 0xffu);
  (*cursor)[2] = (unsigned char)((value >> 16) & 0xffu);
  (*cursor)[3] = (unsigned char)((value >> 24) & 0xffu);
  *cursor += 4;
}

static bool read_u32(const unsigned char **cursor, const unsigned char *end, uint32_t *out) {
  if ((size_t)(end - *cursor) < 4) return false;
  *out = (uint32_t)(*cursor)[0] | (uint32_t)(*cursor)[1] << 8 | (uint32_t)(*cursor)[2] << 16 | (uint32_t)(*cursor)[3] << 24;
  *cursor += 4;
  return true;
}

size_t dd_export_project_save(ft_game *game, void *out, size_t out_size) {
  if (!game || !sync_selection(game)) return 0;
  size_t required = 12;
  for (int world = 0; world < game->demo_export_world_count; ++world) {
    const int tracks = game->demo_export_worlds[world].track_count;
    if (tracks < 0 || required > SIZE_MAX - 5 || (size_t)tracks > (SIZE_MAX - required - 5) / 5) return 0;
    required += 5 + (size_t)tracks * 5;
  }
  if (!out || out_size < required) return required;

  unsigned char *cursor = out;
  memcpy(cursor, "DDEX", 4);
  cursor += 4;
  write_u32(&cursor, 1);
  write_u32(&cursor, (uint32_t)game->demo_export_world_count);
  for (int world = 0; world < game->demo_export_world_count; ++world) {
    const dd_demo_export_world_t *selection = &game->demo_export_worlds[world];
    *cursor++ = selection->enabled ? 1 : 0;
    write_u32(&cursor, (uint32_t)selection->track_count);
    for (int track = 0; track < selection->track_count; ++track) {
      *cursor++ = selection->tracks[track] ? 1 : 0;
      write_u32(&cursor, (uint32_t)selection->pings[track]);
    }
  }
  return required;
}

bool dd_export_project_load(ft_game *game, const void *data, size_t size) {
  if (!game) return false;
  if (size == 0) {
    dd_export_window_cleanup(game);
    game->preserve_demo_export_on_level_load = false;
    return true;
  }
  if (!data || size < 12) return false;
  const unsigned char *cursor = data;
  const unsigned char *end = cursor + size;
  if (memcmp(cursor, "DDEX", 4) != 0) return false;
  cursor += 4;
  uint32_t version = 0;
  uint32_t world_count = 0;
  if (!read_u32(&cursor, end, &version) || version != 1 || !read_u32(&cursor, end, &world_count) || world_count > 4096)
    return false;

  dd_demo_export_world_t *worlds = world_count > 0 ? calloc(world_count, sizeof(*worlds)) : NULL;
  if (world_count > 0 && !worlds) return false;
  for (uint32_t world = 0; world < world_count; ++world) {
    uint32_t track_count = 0;
    if (cursor == end || *cursor > 1) goto malformed;
    worlds[world].enabled = *cursor++ != 0;
    if (!read_u32(&cursor, end, &track_count) || track_count > 65536) goto malformed;
    if ((size_t)(end - cursor) < (size_t)track_count * 5) goto malformed;
    worlds[world].track_count = (int)track_count;
    if (track_count == 0) continue;
    worlds[world].tracks = calloc(track_count, sizeof(*worlds[world].tracks));
    worlds[world].pings = calloc(track_count, sizeof(*worlds[world].pings));
    if (!worlds[world].tracks || !worlds[world].pings) goto malformed;
    for (uint32_t track = 0; track < track_count; ++track) {
      uint32_t ping = 0;
      if (cursor == end || *cursor > 1) goto malformed;
      worlds[world].tracks[track] = *cursor++ != 0;
      if (!read_u32(&cursor, end, &ping) || ping > 999) goto malformed;
      worlds[world].pings[track] = (int32_t)ping;
    }
  }
  if (cursor != end) goto malformed;

  dd_export_window_cleanup(game);
  game->demo_export_worlds = worlds;
  game->demo_export_world_count = (int)world_count;
  game->preserve_demo_export_on_level_load = true;
  return true;

malformed:
  free_worlds(worlds, (int)world_count);
  return false;
}
