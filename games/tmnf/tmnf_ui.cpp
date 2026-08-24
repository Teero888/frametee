// The start screen: pick a campaign, pick a track.
//
// This is also the module's only translation unit that instantiates stb_image,
// because the thumbnails shown here are JPEGs embedded in the challenge files
// themselves.

#include "tmnf_internal.h"

#include <cimgui.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace tmnf {
namespace {

// How many thumbnails may be decoded in one frame. A campaign is sixty-five
// tracks and decoding all of them at once stalls the first frame the browser
// is opened for long enough to see.
constexpr int kDecodesPerFrame = 4;

constexpr float kThumbSize = 148.f;
constexpr float kThumbSpacing = 12.f;
constexpr float kLabelHeight = 22.f;

ImTextureRef *AsRef(void *thumbnail) { return static_cast<ImTextureRef *>(thumbnail); }

void DrawMissingGameData() {
  igTextWrapped("TrackMania game data is not installed.");
  igSpacing();
  igTextWrapped("Get an official release at:");
  igTextLinkOpenURL(kOfficialReleaseUrl, kOfficialReleaseUrl);
  igSpacing();
  igTextWrapped("Copy its Packs and GameData folders into data/games/tmnf/ beside FrameTee, then reopen FrameTee.");
}

bool ExtractThumbnail(const std::vector<std::byte> &file, const std::byte **out, std::size_t *size) {
  static constexpr std::string_view kOpen = "<Thumbnail.jpg>";
  static constexpr std::string_view kClose = "</Thumbnail.jpg>";
  const std::string_view view(reinterpret_cast<const char *>(file.data()), file.size());

  const std::size_t start = view.find(kOpen);
  if (start == std::string_view::npos) return false;
  const std::size_t end = view.find(kClose, start);
  if (end == std::string_view::npos || end <= start + kOpen.size()) return false;

  *out = file.data() + start + kOpen.size();
  *size = end - start - kOpen.size();
  return true;
}

void ReleaseThumbnail(ft_game *game, TrackEntry &track) {
  if (track.thumbnail) {
    const ImTextureID id = ImTextureRef_GetTexID(AsRef(track.thumbnail));
    if (id && game->engine && game->engine->imgui_texture_release)
      game->engine->imgui_texture_release(static_cast<std::uint64_t>(id));
    ImTextureRef_destroy(AsRef(track.thumbnail));
    track.thumbnail = nullptr;
  }
  if (track.texture && game->engine && game->engine->texture_destroy) game->engine->texture_destroy(track.texture);
  track.texture = nullptr;
}

void LoadThumbnail(ft_game *game, TrackEntry &track) {
  track.thumbnail_tried = true;
  if (!game->engine || !game->engine->texture_create || !game->engine->imgui_texture_id) return;

  const std::vector<std::byte> file = ReadFileBytes(track.path.c_str());
  const std::byte *jpeg = nullptr;
  std::size_t jpeg_size = 0;
  if (file.empty() || !ExtractThumbnail(file, &jpeg, &jpeg_size)) return;

  int width = 0, height = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(jpeg), static_cast<int>(jpeg_size), &width,
                                          &height, &channels, 4);
  if (!pixels || width <= 0 || height <= 0) {
    if (pixels) stbi_image_free(pixels);
    return;
  }

  // Unlike an ordinary image file, a challenge stores its thumbnail bottom row
  // first, so it has to be turned back over before it is uploaded.
  const std::size_t stride = static_cast<std::size_t>(width) * 4u;
  std::vector<unsigned char> flipped(stride * static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    std::memcpy(flipped.data() + stride * static_cast<std::size_t>(row),
                pixels + stride * static_cast<std::size_t>(height - 1 - row), stride);
  }
  stbi_image_free(pixels);

  ft_texture_desc desc{};
  desc.struct_size = sizeof(desc);
  desc.pixels = flipped.data();
  desc.width = static_cast<std::uint32_t>(width);
  desc.height = static_cast<std::uint32_t>(height);
  desc.layers = 1;
  desc.format = FT_TEXTURE_RGBA8;
  desc.linear_filter = true;

  track.texture = game->engine->texture_create(&desc);
  if (!track.texture) return;
  const std::uint64_t id = game->engine->imgui_texture_id(track.texture);
  if (id) track.thumbnail = ImTextureRef_ImTextureRef_TextureID(static_cast<ImTextureID>(id));
}

bool Matches(const TrackEntry &track, const char *filter) {
  if (!filter || !*filter) return true;
  const auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  };
  return lower(track.name).find(lower(filter)) != std::string::npos;
}

void DrawTrackGrid(ft_game *game, Campaign &campaign, const char *filter) {
  const ImVec2_c region = igGetContentRegionAvail();
  const int columns = std::max(1, static_cast<int>((region.x + kThumbSpacing) / (kThumbSize + kThumbSpacing)));

  int decoded = 0;
  int shown = 0;
  for (int index = 0; index < static_cast<int>(campaign.tracks.size()); ++index) {
    TrackEntry &track = campaign.tracks[static_cast<std::size_t>(index)];
    if (!Matches(track, filter)) continue;

    if (shown % columns != 0) igSameLine(0.f, kThumbSpacing);
    ++shown;

    igPushID_Int(index);
    igBeginGroup();

    const bool clicked = igInvisibleButton("##card", ImVec2{kThumbSize, kThumbSize + kLabelHeight}, 0);
    const bool hovered = igIsItemHovered(0);
    const ImVec2_c min_c = igGetItemRectMin();
    const ImVec2_c max_c = igGetItemRectMax();
    const ImVec2 min{min_c.x, min_c.y};
    const ImVec2 max{max_c.x, max_c.y};

    // Only cards the user can actually see are worth a texture; the rest give
    // theirs back so opening a long campaign does not fill the atlas.
    const bool visible = igIsRectVisible_Vec2(min, max);
    track.visible_this_frame = visible;
    if (visible && !track.thumbnail_tried && decoded < kDecodesPerFrame) {
      LoadThumbnail(game, track);
      ++decoded;
    }

    // Nothing is submitted for a card that is scrolled away, which is also what
    // makes it safe to hand its texture back below: an evicted texture is never
    // one this frame's draw list still refers to.
    if (visible) {
      ImDrawList *draw = igGetWindowDrawList();
      const ImVec2 image_max{min.x + kThumbSize, min.y + kThumbSize};
      if (track.thumbnail) {
        ImDrawList_AddImageRounded(draw, *AsRef(track.thumbnail), min, image_max, ImVec2{0.f, 0.f}, ImVec2{1.f, 1.f},
                                   0xFFFFFFFFu, 6.f, ImDrawFlags_RoundCornersTop);
      } else {
        ImDrawList_AddRectFilled(draw, min, image_max, 0xFF262A31u, 6.f, ImDrawFlags_RoundCornersTop);
      }
      ImDrawList_AddRect(draw, min, max, hovered ? 0xFFE0A050u : 0xFF404850u, 6.f, 0, hovered ? 2.f : 1.f);
      ImDrawList_AddText_Vec2(draw, ImVec2{min.x + 6.f, min.y + kThumbSize + 4.f}, 0xFFE6E6E6u, track.name.c_str(),
                              nullptr);
    }

    if (hovered) igSetTooltip("%s", track.path.c_str());
    if (clicked && game->engine && game->engine->request_level) game->engine->request_level(track.path.c_str());

    igEndGroup();
    igPopID();
  }

  if (shown == 0) igTextUnformatted("No track matches that name.", nullptr);
}

// Textures for cards that have scrolled away are handed back, and the card is
// marked so it decodes again if the user scrolls back to it.
void EvictOffscreen(ft_game *game, Campaign &campaign) {
  for (TrackEntry &track : campaign.tracks) {
    if (!track.visible_this_frame && track.texture) {
      ReleaseThumbnail(game, track);
      track.thumbnail_tried = false;
    }
    track.visible_this_frame = false;
  }
}

enum class CampaignFacet { Mode, Environment, Difficulty };

const std::string &FacetValue(const Campaign &campaign, CampaignFacet facet) {
  switch (facet) {
  case CampaignFacet::Mode: return campaign.mode;
  case CampaignFacet::Environment: return campaign.environment;
  case CampaignFacet::Difficulty: return campaign.difficulty;
  }
  return campaign.mode;
}

int OrderedRank(const std::string &value, const char *const *order, int count) {
  if (value.empty()) return 0;
  for (int index = 0; index < count; ++index)
    if (value == order[index]) return index + 1;
  return count + 1;
}

int FacetRank(const std::string &value, CampaignFacet facet) {
  static const char *const modes[] = {"Race", "Platform", "Puzzle", "Stunts"};
  static const char *const environments[] = {"Stadium", "Bay", "Coast", "Desert", "Island", "Rally", "Snow"};
  static const char *const difficulties[] = {"White", "Green", "Blue", "Red", "Black"};
  switch (facet) {
  case CampaignFacet::Mode: return OrderedRank(value, modes, static_cast<int>(std::size(modes)));
  case CampaignFacet::Environment:
    return OrderedRank(value, environments, static_cast<int>(std::size(environments)));
  case CampaignFacet::Difficulty:
    return OrderedRank(value, difficulties, static_cast<int>(std::size(difficulties)));
  }
  return 0;
}

int CollectionRank(const std::string &collection) {
  static const char *const collections[] = {"Nations", "United", "StarTrack"};
  return OrderedRank(collection, collections, static_cast<int>(std::size(collections)));
}

const char *CollectionLabel(const std::string &collection) {
  return collection == "StarTrack" ? "Star Track" : collection.c_str();
}

std::vector<std::string> Collections(const ft_game *game) {
  std::vector<std::string> result;
  for (const Campaign &campaign : game->campaigns) {
    if (std::find(result.begin(), result.end(), campaign.collection) == result.end())
      result.push_back(campaign.collection);
  }
  std::sort(result.begin(), result.end(), [](const std::string &left, const std::string &right) {
    const int left_rank = CollectionRank(left);
    const int right_rank = CollectionRank(right);
    return left_rank == right_rank ? left < right : left_rank < right_rank;
  });
  return result;
}

std::vector<std::string> FacetValues(const ft_game *game, const std::string &collection, const std::string &mode,
                                     const std::string &environment, CampaignFacet facet) {
  std::vector<std::string> result;
  for (const Campaign &campaign : game->campaigns) {
    if (campaign.collection != collection) continue;
    if (!mode.empty() && campaign.mode != mode) continue;
    if (!environment.empty() && campaign.environment != environment) continue;
    const std::string &value = FacetValue(campaign, facet);
    if (!value.empty() && std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
  }
  std::sort(result.begin(), result.end(), [facet](const std::string &left, const std::string &right) {
    const int left_rank = FacetRank(left, facet);
    const int right_rank = FacetRank(right, facet);
    return left_rank == right_rank ? left < right : left_rank < right_rank;
  });
  return result;
}

int FindCampaign(const ft_game *game, const std::string &collection, const std::string &mode,
                 const std::string &environment, const std::string &difficulty) {
  int best = -1;
  int best_score = 0;
  for (int index = 0; index < static_cast<int>(game->campaigns.size()); ++index) {
    const Campaign &candidate = game->campaigns[static_cast<std::size_t>(index)];
    if (candidate.collection != collection) continue;
    if (!mode.empty() && candidate.mode != mode) continue;
    if (!environment.empty() && candidate.environment != environment) continue;
    if (!difficulty.empty() && candidate.difficulty != difficulty) continue;

    const int score = FacetRank(candidate.mode, CampaignFacet::Mode) * 10000 +
                      FacetRank(candidate.environment, CampaignFacet::Environment) * 100 +
                      FacetRank(candidate.difficulty, CampaignFacet::Difficulty);
    if (best == -1 || score < best_score) {
      best = index;
      best_score = score;
    }
  }
  return best;
}

void SelectCampaign(ft_game *game, int index) {
  if (index < 0 || index >= static_cast<int>(game->campaigns.size()) || index == game->selected_campaign) return;

  if (game->selected_campaign >= 0 && game->selected_campaign < static_cast<int>(game->campaigns.size())) {
    Campaign &previous = game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
    for (TrackEntry &track : previous.tracks) {
      ReleaseThumbnail(game, track);
      track.thumbnail_tried = false;
      track.visible_this_frame = false;
    }
  }
  game->selected_campaign = index;
}

bool FilterButton(const char *label, bool selected) {
  if (selected) {
    igPushStyleColor_Vec4(ImGuiCol_Button, ImVec4{0.35f, 0.75f, 1.00f, 0.95f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, ImVec4{0.43f, 0.81f, 1.00f, 1.00f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, ImVec4{0.28f, 0.67f, 0.95f, 1.00f});
    igPushStyleColor_Vec4(ImGuiCol_Text, ImVec4{0.05f, 0.12f, 0.20f, 1.00f});
  }
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 6.f);
  const bool clicked = igButton(label, ImVec2{0.f, 28.f});
  igPopStyleVar(1);
  if (selected) igPopStyleColor(4);
  return clicked;
}

int DrawFacetRow(ft_game *game, const char *label, const Campaign &current, CampaignFacet facet,
                 const std::vector<std::string> &values) {
  if (values.size() <= 1) return -1;

  igAlignTextToFramePadding();
  igTextDisabled("%s", label);
  igSameLine(0.f, 12.f);
  igPushID_Str(label);
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) igSameLine(0.f, 6.f);
    const std::string &value = values[index];
    if (!FilterButton(value.c_str(), FacetValue(current, facet) == value)) continue;

    igPopID();
    switch (facet) {
    case CampaignFacet::Mode: return FindCampaign(game, current.collection, value, {}, {});
    case CampaignFacet::Environment: return FindCampaign(game, current.collection, current.mode, value, {});
    case CampaignFacet::Difficulty:
      return FindCampaign(game, current.collection, current.mode, current.environment, value);
    }
  }
  igPopID();
  return -1;
}

} // namespace

// ImGui keeps its context and allocator in globals. The module's ig* calls
// resolve against the host, so this is normally already the editor's context,
// but adopting it explicitly costs nothing and keeps the module correct if it
// ever links a copy of its own.
void UiAttach(const ft_engine_api *engine) {
  static bool attached = false;
  if (attached || !engine || !engine->imgui_context) return;

  auto *context = static_cast<ImGuiContext *>(engine->imgui_context());
  if (!context) return;

  if (engine->imgui_allocators) {
    void *alloc_fn = nullptr;
    void *free_fn = nullptr;
    void *user_data = nullptr;
    engine->imgui_allocators(&alloc_fn, &free_fn, &user_data);
    if (alloc_fn && free_fn)
      igSetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(alloc_fn), reinterpret_cast<ImGuiMemFreeFunc>(free_fn),
                              user_data);
  }
  igSetCurrentContext(context);
  attached = true;
}

void ReleaseThumbnails(ft_game *game) {
  if (!game) return;
  for (Campaign &campaign : game->campaigns) {
    for (TrackEntry &track : campaign.tracks) ReleaseThumbnail(game, track);
  }
}

void Ui(ft_game *game, const ft_ui_frame *frame) {
  if (!game || !frame || game->headless) return;
  UiAttach(game->engine);

  if (frame->slot == FT_UI_STATUS_BAR) {
    if (game->level) igText("%s", game->level->name.c_str());
    return;
  }
  if (frame->slot != FT_UI_SPLASH) return;

  static char filter[64] = {};

  igAlignTextToFramePadding();
  igTextUnformatted("Choose a track", nullptr);
  igSameLine(0.f, 20.f);
  const float search_width = std::min(280.f, igGetContentRegionAvail().x);
  const float search_x = igGetCursorPosX() + igGetContentRegionAvail().x - search_width;
  igSetCursorPosX(search_x);
  igSetNextItemWidth(search_width);
  igInputTextWithHint("##filter", "Search tracks", filter, sizeof(filter), 0, nullptr, nullptr);
  igSeparator();
  igSpacing();

  if (game->packs.empty()) {
    DrawMissingGameData();
    return;
  }

  if (!game->scanned) ScanTracks(game);

  if (game->campaigns.empty()) {
    DrawMissingGameData();
    return;
  }

  const std::vector<std::string> collections = Collections(game);
  if (igBeginTabBar("##collections", 0)) {
    for (const std::string &collection : collections) {
      char tab_label[160];
      std::snprintf(tab_label, sizeof(tab_label), "%s##%s", CollectionLabel(collection), collection.c_str());
      if (!igBeginTabItem(tab_label, nullptr, 0)) continue;

      const Campaign *current = &game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      if (current->collection != collection) {
        SelectCampaign(game, FindCampaign(game, collection, {}, {}, {}));
        current = &game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      }

      igSpacing();
      int selected = DrawFacetRow(game, "Mode", *current, CampaignFacet::Mode,
                                  FacetValues(game, collection, {}, {}, CampaignFacet::Mode));
      if (selected >= 0) {
        SelectCampaign(game, selected);
        current = &game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      }

      selected = DrawFacetRow(game, "Environment", *current, CampaignFacet::Environment,
                              FacetValues(game, collection, current->mode, {}, CampaignFacet::Environment));
      if (selected >= 0) {
        SelectCampaign(game, selected);
        current = &game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      }

      selected = DrawFacetRow(game, "Difficulty", *current, CampaignFacet::Difficulty,
                              FacetValues(game, collection, current->mode, current->environment,
                                          CampaignFacet::Difficulty));
      if (selected >= 0) {
        SelectCampaign(game, selected);
        current = &game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      }

      igTextDisabled("%zu tracks", current->tracks.size());
      igBeginChild_Str("##tracks", ImVec2{0.f, 0.f}, ImGuiChildFlags_Borders, 0);
      Campaign &campaign = game->campaigns[static_cast<std::size_t>(game->selected_campaign)];
      DrawTrackGrid(game, campaign, filter);
      EvictOffscreen(game, campaign);
      igEndChild();

      igEndTabItem();
    }
    igEndTabBar();
  }
}

} // namespace tmnf
