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

#include <cstdio>
#include <cstring>

namespace tmnf {
namespace {

constexpr std::uint32_t kProfileVersion = 1u;

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
  if (!data || size != sizeof(PlayerProfile)) return profile;

  PlayerProfile stored{};
  std::memcpy(&stored, data, sizeof(stored));
  if (stored.version != kProfileVersion) return profile;

  stored.name[sizeof(stored.name) - 1] = '\0';
  return stored;
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

  if (std::memcmp(&before, &profile, sizeof(profile)) != 0) StoreProfile(game, track, profile);

  // Where the car starts and how it is set up. The controls come from the
  // editor, built out of the properties this module publishes as startable.
  if (igCollapsingHeader_TreeNodeFlags("Starting state", ImGuiTreeNodeFlags_DefaultOpen) && game->engine->starting_state_editor)
    game->engine->starting_state_editor(track);
  igEnd();
}

} // namespace tmnf
