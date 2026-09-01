// The replay export window.
//
// A mirror of the DDNet module's demo export: a menu item opens a modal that
// picks an inclusive global tick range and the timeline worlds and tracks that
// become ghosts, then hands the whole thing to the exporter in
// tmnf_replay.cpp. There is no per-track ping here -- a TrackMania replay
// records a car, not a network connection.

#include "tmnf_internal.h"

#include <cimgui.h>

#include <algorithm>
#include <cstdio>

namespace tmnf {
namespace {

// What CollectGhosts in tmnf_replay.cpp will accept in one file.
constexpr int kMaximumGhosts = 256;

bool TimelineReadable(const ft_game *game) {
  return game && game->engine && game->engine->timeline_world_count && game->engine->timeline_world_info &&
         game->engine->timeline_player_track;
}

// Rebuild the selection when the timeline's shape has changed, keeping whatever
// the user had already chosen for the worlds and tracks that survived.
bool SyncSelection(ft_game *game) {
  if (!TimelineReadable(game)) return false;

  const std::uint32_t world_count = game->engine->timeline_world_count();
  std::vector<ExportWorldSelection> next;
  next.reserve(world_count);
  for (std::uint32_t world = 0u; world < world_count; ++world) {
    ft_timeline_world_info info{};
    info.struct_size = sizeof(info);
    if (!game->engine->timeline_world_info(world, &info)) return false;

    ExportWorldSelection selection;
    const auto *previous =
        world < game->export_worlds.size() ? &game->export_worlds[world] : nullptr;
    if (previous) selection.enabled = previous->enabled;
    selection.tracks.resize(info.player_count);
    if (previous) {
      const std::size_t carried = std::min(previous->tracks.size(), selection.tracks.size());
      std::copy_n(previous->tracks.begin(), carried, selection.tracks.begin());
    }
    next.push_back(std::move(selection));
  }

  game->export_worlds = std::move(next);
  return true;
}

const char *WorldName(ft_game *game, std::uint32_t world, char *fallback, std::size_t fallback_size) {
  ft_timeline_world_info info{};
  info.struct_size = sizeof(info);
  if (game->engine->timeline_world_info(world, &info) && info.name && info.name[0] != '\0') return info.name;
  std::snprintf(fallback, fallback_size, "World %u", world + 1u);
  return fallback;
}

const char *TrackName(ft_game *game, std::uint32_t world, std::uint32_t local, char *fallback,
                      std::size_t fallback_size) {
  const std::int32_t track = game->engine->timeline_player_track(world, local);
  if (track >= 0) {
    const PlayerProfile profile = ProfileForTrack(game, track);
    if (profile.name[0] != '\0') {
      std::snprintf(fallback, fallback_size, "%s", profile.name);
      return fallback;
    }
  }
  std::snprintf(fallback, fallback_size, "Track %u", local + 1u);
  return fallback;
}

// Every track the current selection would export, in world order.
std::vector<std::int32_t> SelectedTracks(ft_game *game) {
  std::vector<std::int32_t> tracks;
  for (std::size_t world = 0u; world < game->export_worlds.size(); ++world) {
    const ExportWorldSelection &selection = game->export_worlds[world];
    if (!selection.enabled) continue;
    for (std::size_t local = 0u; local < selection.tracks.size(); ++local) {
      if (!selection.tracks[local].selected) continue;
      const std::int32_t track =
          game->engine->timeline_player_track(static_cast<std::uint32_t>(world), static_cast<std::uint32_t>(local));
      if (track >= 0) tracks.push_back(track);
    }
  }
  return tracks;
}

void RunExport(ft_game *game, const std::vector<std::int32_t> &tracks) {
  const ft_exporter_desc *desc = ExporterDesc(game, 0u);
  if (!desc || !game->engine->save_file_dialog) {
    game->export_error = "This build cannot open a save dialog.";
    return;
  }

  char default_name[256];
  std::snprintf(default_name, sizeof(default_name), "%s.%s",
                game->level && !game->level->name.empty() ? game->level->name.c_str() : "run", desc->file_extension);

  char path[1024];
  if (!game->engine->save_file_dialog(desc->filter_name, desc->file_extension, default_name, path, sizeof(path)))
    return; // the user cancelled, which is not an error

  ft_export_request request{};
  request.struct_size = sizeof(request);
  request.path = path;
  request.start_tick = game->export_start_tick;
  request.end_tick = game->export_end_tick;
  request.players = tracks.data();
  request.player_count = static_cast<std::uint32_t>(tracks.size());

  if (ExportRun(game, 0u, &request)) {
    game->export_error.clear();
    igCloseCurrentPopup();
  } else {
    game->export_error = "Export failed; see the log for details.";
  }
}

} // namespace

void ExportWindowOpen(ft_game *game) {
  if (!game) return;

  std::int32_t first_tick = 0;
  std::int32_t last_tick = 0;
  if (game->engine && game->engine->timeline_range) game->engine->timeline_range(&first_tick, &last_tick);
  game->export_start_tick = first_tick;
  game->export_end_tick = last_tick;

  game->export_error.clear();
  if (!SyncSelection(game)) game->export_error = "Could not read the timeline.";
  game->open_export = true;
}

void ExportWindowRender(ft_game *game) {
  if (!game || game->headless) return;
  if (game->open_export) {
    igOpenPopup_Str("Export TrackMania Replay", ImGuiPopupFlags_None);
    game->open_export = false;
  }

  igSetNextWindowSize(ImVec2{560.f, 560.f}, ImGuiCond_FirstUseEver);
  if (!igBeginPopupModal("Export TrackMania Replay", nullptr, 0)) return;

  const bool selection_ok = SyncSelection(game);
  igTextWrapped("Choose the inclusive global tick range and the tracks to record as ghosts. The replay embeds the "
                "loaded challenge, so it opens on its own in TrackMania.");
  igSpacing();

  igSetNextItemWidth(150.f);
  igDragInt("Start tick", &game->export_start_tick, 1.f, -100000000, 100000000, "%d",
            ImGuiSliderFlags_AlwaysClamp);
  igSameLine(0.f, 12.f);
  igSetNextItemWidth(150.f);
  igDragInt("End tick (inclusive)", &game->export_end_tick, 1.f, -100000000, 100000000, "%d",
            ImGuiSliderFlags_AlwaysClamp);

  const std::int64_t span = static_cast<std::int64_t>(game->export_end_tick) - game->export_start_tick;
  if (span >= 0) igTextDisabled("%lld ticks, %.2f s", static_cast<long long>(span + 1),
                                static_cast<double>(span) * kTickMs / 1000.0);
  igSeparator();

  int selected_count = 0;
  if (igBeginChild_Str("##tmnf_export_tracks", ImVec2{0.f, -82.f}, ImGuiChildFlags_Borders, 0)) {
    for (std::size_t world = 0u; selection_ok && world < game->export_worlds.size(); ++world) {
      ExportWorldSelection &selection = game->export_worlds[world];
      igPushID_Int(static_cast<int>(world));
      char fallback[64];
      igCheckbox(WorldName(game, static_cast<std::uint32_t>(world), fallback, sizeof(fallback)), &selection.enabled);
      if (selection.enabled) {
        igIndent(18.f);
        for (std::size_t local = 0u; local < selection.tracks.size(); ++local) {
          igPushID_Int(static_cast<int>(local) + 10000);
          char track_fallback[64];
          igCheckbox(TrackName(game, static_cast<std::uint32_t>(world), static_cast<std::uint32_t>(local),
                               track_fallback, sizeof(track_fallback)),
                     &selection.tracks[local].selected);
          if (selection.tracks[local].selected) ++selected_count;
          igPopID();
        }
        igUnindent(18.f);
      }
      igSeparator();
      igPopID();
    }
  }
  igEndChild();

  igText("Selected tracks: %d / %d", selected_count, kMaximumGhosts);
  if (!selection_ok) game->export_error = "Could not read the timeline.";
  if (!game->export_error.empty()) {
    igSameLine(0.f, 12.f);
    igTextColored(ImVec4{1.f, 0.35f, 0.3f, 1.f}, "%s", game->export_error.c_str());
  }

  const bool valid = selection_ok && selected_count > 0 && selected_count <= kMaximumGhosts && span >= 0 &&
                     game->level != nullptr;
  if (!valid) igBeginDisabled(true);
  if (igButton("Export...", ImVec2{120.f, 0.f})) RunExport(game, SelectedTracks(game));
  if (!valid) igEndDisabled();

  igSameLine(0.f, 10.f);
  if (igButton("Cancel", ImVec2{100.f, 0.f})) {
    game->export_error.clear();
    igCloseCurrentPopup();
  }
  igEndPopup();
}

} // namespace tmnf
