// Who is driving.
//
// The editor keeps a name for each track and a blob of bytes it never reads;
// what those bytes mean is decided here. DDNet's module puts a tee's skin and
// its two packed-HSL colours in its own blob and draws a panel full of them,
// none of which means anything to a car. This module stores a driver name, and
// nothing else yet: the car is drawn from the model in the installed packs, and
// painting it something other than what that model says is not something this
// renderer can do. The editor is the same either way.

#include "tmnf_internal.h"

#include <cimgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace tmnf {
namespace {

// Version two added the livery. A version one profile is still read: it is
// the same bytes without it, and a driver keeps their name.
constexpr std::uint32_t kProfileVersion = 2u;

struct ProfileV1 {
  std::uint32_t version;
  char name[32];
};

// The colour a car is painted when the editor has no accent for its world:
// blue, the stock Nations car.
constexpr ft_color kDefaultLivery{0.20f, 0.58f, 1.f, 1.f};

} // namespace

PlayerProfile DefaultProfile() {
  PlayerProfile profile{};
  profile.version = kProfileVersion;
  return profile;
}

PlayerProfile DecodeProfile(const void *data, std::uint32_t size) {
  PlayerProfile profile = DefaultProfile();
  if (!data) return profile;

  if (size == sizeof(ProfileV1)) {
    ProfileV1 old{};
    std::memcpy(&old, data, sizeof(old));
    if (old.version != 1u) return profile;
    old.name[sizeof(old.name) - 1] = '\0';
    std::snprintf(profile.name, sizeof(profile.name), "%s", old.name);
    return profile;
  }
  if (size != sizeof(PlayerProfile)) return profile;

  PlayerProfile stored{};
  std::memcpy(&stored, data, sizeof(stored));
  if (stored.version != kProfileVersion) return profile;

  stored.name[sizeof(stored.name) - 1] = '\0';
  stored.skin[sizeof(stored.skin) - 1] = '\0';
  return stored;
}

// --- liveries ----------------------------------------------------------------

namespace {

bool EndsWithZip(std::string_view name) {
  if (name.size() <= 4u) return false;
  const std::string_view tail = name.substr(name.size() - 4u);
  return (tail[0] == '.') && std::tolower(static_cast<unsigned char>(tail[1])) == 'z' &&
         std::tolower(static_cast<unsigned char>(tail[2])) == 'i' &&
         std::tolower(static_cast<unsigned char>(tail[3])) == 'p';
}

std::string SkinFolder(ft_game *game) {
  if (!game || !game->level) return {};
  const std::string car = VehicleSkinFolder(game->level->start.vehicleModel);
  if (car.empty()) return {};
  return "GameData/Skins/Vehicles/" + car;
}

} // namespace

const std::vector<std::string> &InstalledSkins(ft_game *game) {
  const std::string folder = SkinFolder(game);
  if (folder == game->skins_folder) return game->skins;
  game->skins_folder = folder;
  game->skins.clear();
  if (folder.empty() || !game->engine || !game->engine->visit_directory || !game->engine->resolve_data_path)
    return game->skins;

  char root[1024];
  if (game->engine->resolve_data_path(folder.c_str(), root, sizeof(root)) == 0u) return game->skins;

  game->engine->visit_directory(
      root,
      [](void *user, const ft_directory_entry *entry) -> bool {
        auto *out = static_cast<std::vector<std::string> *>(user);
        if (entry != nullptr && !entry->is_directory && entry->name != nullptr) {
          const std::string_view name(entry->name);
          if (EndsWithZip(name)) out->emplace_back(name.substr(0, name.size() - 4u));
        }
        return true;
      },
      &game->skins);
  std::sort(game->skins.begin(), game->skins.end());
  return game->skins;
}

std::string SkinArchivePath(ft_game *game, const std::string &skin) {
  const std::string folder = SkinFolder(game);
  if (skin.empty() || folder.empty() || !game->engine || !game->engine->resolve_data_path) return {};
  char path[1024];
  const std::string relative = folder + "/" + skin + ".zip";
  if (game->engine->resolve_data_path(relative.c_str(), path, sizeof(path)) == 0u) return {};
  return path;
}

PlayerProfile ProfileForTrack(ft_game *game, std::int32_t track) {
  if (!game || !game->engine || track < 0 || !game->engine->get_player_setup) return DefaultProfile();
  ft_player_setup setup{};
  setup.struct_size = sizeof(setup);
  if (!game->engine->get_player_setup(track, &setup)) return DefaultProfile();
  return DecodeProfile(setup.data, setup.data_size);
}

bool StoreProfile(ft_game *game, std::int32_t track, const PlayerProfile &profile) {
  if (!game || !game->engine || track < 0 || !game->engine->set_player_profile) return false;
  PlayerProfile stored = profile;
  stored.version = kProfileVersion;
  return game->engine->set_player_profile(track, &stored, static_cast<std::uint32_t>(sizeof(stored)));
}

// A car wears the colour of the timeline group it is driving in, so two
// prediction ghosts of the same run stay told apart. Painting one individually
// would mean tinting an authored model, which this renderer does not do.
ft_color LiveryFor(const ft_render_frame *frame, int player) {
  (void)player;
  return frame->accent.a > 0.01f ? frame->accent : kDefaultLivery;
}

std::uint32_t SkinLayerFor(ft_game *game, std::int32_t track) {
  if (game == nullptr || track < 0) return kNoTextureLayer;
  const PlayerProfile profile = ProfileForTrack(game, track);
  if (profile.skin[0] == '\0') return kNoTextureLayer;
  const std::string archive = SkinArchivePath(game, profile.skin);
  if (archive.empty()) return kNoTextureLayer;
  return game->textures.SkinLayer(archive, profile.skin).value_or(kNoTextureLayer);
}

// The panel this game draws for the selected driver. Nothing here resembles
// DDNet's: there is no skin, no clan and no colours, because a car has none of
// those: what it has instead is a start worth setting up.
void PlayerPanel(ft_game *game, const ft_ui_frame *frame) {
  if (!igBegin("Player Info", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igEnd();
    return;
  }

  const std::int32_t track = frame->state.selected_player;
  if (track < 0) {
    igTextDisabled("No player track selected.");
    igEnd();
    return;
  }

  PlayerProfile profile = ProfileForTrack(game, track);
  const PlayerProfile before = profile;

  igSeparatorText("Driver");
  igPushItemWidth(igGetContentRegionAvail().x - 8.f);
  igInputTextWithHint("##name", "Nickname", profile.name, sizeof(profile.name), 0, nullptr, nullptr);
  igPopItemWidth();

  // The livery. The installed archives are the game's own national skins, and
  // the default is none of them: the car the pack draws, which is what every
  // driver had before this existed.
  const std::vector<std::string> &skins = InstalledSkins(game);
  if (!skins.empty()) {
    igSeparatorText("Livery");
    const bool none = profile.skin[0] == '\0';
    igPushItemWidth(igGetContentRegionAvail().x - 8.f);
    if (igBeginCombo("##skin", none ? "Default" : profile.skin, 0)) {
      if (igSelectable_Bool("Default", none, 0, ImVec2{0.f, 0.f})) profile.skin[0] = '\0';
      for (const std::string &skin : skins) {
        const bool chosen = skin == profile.skin;
        if (igSelectable_Bool(skin.c_str(), chosen, 0, ImVec2{0.f, 0.f}))
          std::snprintf(profile.skin, sizeof(profile.skin), "%s", skin.c_str());
      }
      igEndCombo();
    }
    igPopItemWidth();
  }

  if (std::memcmp(&before, &profile, sizeof(profile)) != 0) StoreProfile(game, track, profile);

  // Where the car starts and how it is set up. The controls come from the
  // editor, built out of the properties this module publishes as startable.
  if (igCollapsingHeader_TreeNodeFlags("Starting state", ImGuiTreeNodeFlags_DefaultOpen) && game->engine->starting_state_editor)
    game->engine->starting_state_editor(track);
  igEnd();
}

} // namespace tmnf
