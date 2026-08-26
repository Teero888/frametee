#include "dd_imcol.h"
#include "dd_imgui.h"
#include "dd_internal.h"
#include "dd_profile.h"

#include <frametee/icons.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum {
  DD_PANEL_PREVIEW_SIZE = 128,
};

struct dd_player_panel_t {
  ft_texture *preview;
  uint64_t preview_id;
  // What the thumbnail currently shows, so it is re-rendered only when the tee
  // it belongs to actually changes.
  char preview_skin[64];
  uint32_t preview_body;
  uint32_t preview_feet;
  bool preview_custom;
  bool preview_valid;
  int32_t preview_track;

  char fetch_status[160];
  bool fetch_failed;
};

static struct dd_player_panel_t *panel_state(ft_game *game) {
  if (!game->player_panel) game->player_panel = calloc(1, sizeof(*game->player_panel));
  return game->player_panel;
}

// --- the tee thumbnail -------------------------------------------------------

static void refresh_preview(ft_game *game, struct dd_player_panel_t *panel, int32_t track, const dd_player_profile_t *profile) {
  const bool same = panel->preview_valid && panel->preview_track == track && panel->preview_custom == (profile->use_custom_color != 0) &&
                    panel->preview_body == profile->color_body && panel->preview_feet == profile->color_feet &&
                    strcmp(panel->preview_skin, profile->skin) == 0;
  if (same) return;

  panel->preview_track = track;
  panel->preview_custom = profile->use_custom_color != 0;
  panel->preview_body = profile->color_body;
  panel->preview_feet = profile->color_feet;
  snprintf(panel->preview_skin, sizeof(panel->preview_skin), "%s", profile->skin);
  panel->preview_valid = false;

  if (!panel->preview) {
    const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                  .pixels = NULL,
                                  .width = DD_PANEL_PREVIEW_SIZE,
                                  .height = DD_PANEL_PREVIEW_SIZE,
                                  .layers = 1,
                                  .format = FT_TEXTURE_RGBA8,
                                  .mipmaps = false,
                                  .linear_filter = true};
    panel->preview = game->engine->texture_create(&desc);
    if (panel->preview && game->engine->imgui_texture_id) panel->preview_id = game->engine->imgui_texture_id(panel->preview);
    if (!panel->preview_id) return;
  }
  if (!panel->preview || !panel->preview_id) return;

  char path[1024];
  if (!dd_gfx_find_skin_file(game, profile->skin, path, sizeof(path))) return;

  dd_skin_colors_t colors = {.custom = profile->use_custom_color != 0};
  dd_hsl_to_rgb(profile->color_body, colors.body);
  dd_hsl_to_rgb(profile->color_feet, colors.feet);
  panel->preview_valid = dd_gfx_render_skin_preview(game, profile->skin, path, &colors, panel->preview, 0, 0);
}

static void draw_preview(struct dd_player_panel_t *panel, float size) {
  const ImVec2 origin = igGetCursorScreenPos();
  const ImVec2 min = {origin.x, origin.y};
  const ImVec2 max = {origin.x + size, origin.y + size};
  ImDrawList *draw_list = igGetWindowDrawList();
  ImDrawList_AddRectFilled(draw_list, min, max, IM_COL32(28, 32, 42, 120), 8.f, ImDrawFlags_None);

  if (panel->preview_valid) {
    const ImTextureRef_c ref = {._TexData = NULL, ._TexID = (ImTextureID)panel->preview_id};
    ImDrawList_AddImage(draw_list, ref, min, max, (ImVec2){0.f, 0.f}, (ImVec2){1.f, 1.f}, IM_COL32_WHITE);
  } else {
    const ImVec2 icon = igCalcTextSize(ICON_FA_IMAGE, NULL, false, -1.f);
    ImDrawList_AddText_Vec2(draw_list, (ImVec2){min.x + (size - icon.x) * .5f, min.y + (size - icon.y) * .5f},
                            IM_COL32(105, 115, 135, 255), ICON_FA_IMAGE, NULL);
  }
  igDummy((ImVec2){size, size});
}

// --- the colour picker -------------------------------------------------------

// DDNet's colours are a hue, a saturation and a lightness that never reaches
// the bottom of its range, packed into three bytes. A plain RGB picker cannot
// express that, so the tee's own space is what the panel offers.
static bool packed_hsl_picker(const char *label, uint32_t *packed) {
  bool changed = false;
  igPushID_Str(label);

  float h, s, l;
  dd_hsl_unpack(*packed, &h, &s, &l);

  const float available = igGetContentRegionAvail().x;
  const float hue_width = 14.f;
  float field = available * .5f - (hue_width + 12.f);
  if (field < 60.f) field = 60.f;

  const ImVec2 origin = igGetCursorScreenPos();
  const ImVec2 hue_origin = {origin.x + field + 8.f, origin.y};
  ImDrawList *draw_list = igGetWindowDrawList();

  // Saturation runs left to right, lightness top to bottom, both at the hue the
  // strip beside it selects.
  const int columns = (int)field > 2 ? (int)field : 2;
  for (int x = 0; x < columns; ++x) {
    const float saturation = (float)x / (float)(columns - 1);
    float top[3], bottom[3];
    dd_hsl_components_to_rgb(h, saturation, DD_DARKEST_LIGHTNESS, top);
    dd_hsl_components_to_rgb(h, saturation, 1.f, bottom);
    const ImU32 col_top = IM_COL32((int)(top[0] * 255.f), (int)(top[1] * 255.f), (int)(top[2] * 255.f), 255);
    const ImU32 col_bottom = IM_COL32((int)(bottom[0] * 255.f), (int)(bottom[1] * 255.f), (int)(bottom[2] * 255.f), 255);
    ImDrawList_AddRectFilledMultiColor(draw_list, (ImVec2){origin.x + (float)x, origin.y},
                                       (ImVec2){origin.x + (float)x + 1.f, origin.y + field}, col_top, col_top, col_bottom, col_bottom);
  }

  igSetCursorScreenPos(origin);
  igInvisibleButton("##field", (ImVec2){field, field}, 0);
  if (igIsItemActive()) {
    const ImVec2 mouse = igGetIO_Nil()->MousePos;
    float nx = (mouse.x - origin.x) / field;
    float ny = (mouse.y - origin.y) / field;
    nx = nx < 0.f ? 0.f : (nx > 1.f ? 1.f : nx);
    ny = ny < 0.f ? 0.f : (ny > 1.f ? 1.f : ny);
    s = nx;
    l = DD_DARKEST_LIGHTNESS + ny * (1.f - DD_DARKEST_LIGHTNESS);
    changed = true;
  }

  const int rows = columns;
  for (int y = 0; y < rows; ++y) {
    const float hue = 1.f - (float)y / (float)(rows - 1);
    float rgb[3];
    dd_hsl_components_to_rgb(hue, 1.f, .5f, rgb);
    ImDrawList_AddRectFilled(draw_list, (ImVec2){hue_origin.x, hue_origin.y + (float)y},
                             (ImVec2){hue_origin.x + hue_width, hue_origin.y + (float)y + 1.f},
                             IM_COL32((int)(rgb[0] * 255.f), (int)(rgb[1] * 255.f), (int)(rgb[2] * 255.f), 255), 0.f, 0);
  }
  igSetCursorScreenPos(hue_origin);
  igInvisibleButton("##hue", (ImVec2){hue_width, field}, 0);
  if (igIsItemActive()) {
    const ImVec2 mouse = igGetIO_Nil()->MousePos;
    float ny = (mouse.y - hue_origin.y) / field;
    ny = ny < 0.f ? 0.f : (ny > 1.f ? 1.f : ny);
    h = 1.f - ny;
    changed = true;
  }

  igSetCursorScreenPos((ImVec2){origin.x, origin.y + field + 6.f});

  float hue_degrees = h * 360.f;
  float saturation_percent = s * 100.f;
  float lightness_percent = ((l - DD_DARKEST_LIGHTNESS) / (1.f - DD_DARKEST_LIGHTNESS)) * 100.f;
  if (igDragFloat("##hue_value", &hue_degrees, 1.f, 0.f, 360.f, "Hue: %.0f", 0)) {
    h = hue_degrees / 360.f;
    changed = true;
  }
  if (igDragFloat("##saturation", &saturation_percent, 1.f, 0.f, 100.f, "Sat: %.0f%%", 0)) {
    s = saturation_percent / 100.f;
    changed = true;
  }
  if (igDragFloat("##lightness", &lightness_percent, 1.f, 0.f, 100.f, "Light: %.0f%%", 0)) {
    l = DD_DARKEST_LIGHTNESS + (lightness_percent / 100.f) * (1.f - DD_DARKEST_LIGHTNESS);
    changed = true;
  }

  // The packed integer is what a DDNet config file carries, so it is worth
  // being able to paste one straight in.
  int packed_value = (int)*packed;
  if (igInputInt("##packed", &packed_value, 1, 100, ImGuiInputTextFlags_CharsDecimal)) {
    *packed = (uint32_t)packed_value;
    igPopID();
    return true;
  }
  if (changed) *packed = dd_hsl_pack(h, s, l);

  igPopID();
  return changed;
}

// --- the panel ---------------------------------------------------------------

// Which of the editor's worlds the selected track lives in. The editor numbers
// tracks across every world, so the only way back is to ask each world which
// tracks it holds.
static bool world_of_track(ft_game *game, int32_t track, uint32_t *out_world, ft_timeline_world_info *out_info) {
  const ft_engine_api *engine = game->engine;
  if (!engine->timeline_world_count || !engine->timeline_world_info || !engine->timeline_player_track) return false;
  const uint32_t worlds = engine->timeline_world_count();
  for (uint32_t world = 0; world < worlds; ++world) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (!engine->timeline_world_info(world, &info)) continue;
    for (uint32_t local = 0; local < info.player_count; ++local) {
      if (engine->timeline_player_track(world, local) != track) continue;
      *out_world = world;
      *out_info = info;
      return true;
    }
  }
  return false;
}

static void apply_to_group(ft_game *game, int32_t selected_track, const dd_player_profile_t *profile) {
  uint32_t world = 0;
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (!world_of_track(game, selected_track, &world, &info)) return;

  for (uint32_t local = 0; local < info.player_count; ++local) {
    const int32_t track = game->engine->timeline_player_track(world, local);
    if (track < 0 || track == selected_track) continue;
    // The nickname is what tells two tees apart in a demo, so only the look
    // travels.
    dd_player_profile_t other;
    dd_profile_for_track(game, track, &other);
    snprintf(other.skin, sizeof(other.skin), "%s", profile->skin);
    other.use_custom_color = profile->use_custom_color;
    other.color_body = profile->color_body;
    other.color_feet = profile->color_feet;
    dd_profile_store(game, track, &other);
  }
}

void dd_player_panel_render(ft_game *game, const ft_ui_frame *frame) {
  struct dd_player_panel_t *panel = panel_state(game);
  if (!panel) return;

  if (!igBegin("Player Info", NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igEnd();
    return;
  }

  const int32_t track = frame->state.selected_player;
  if (track < 0) {
    igTextDisabled("No player track selected.");
    igEnd();
    return;
  }

  dd_player_profile_t profile;
  dd_profile_for_track(game, track, &profile);
  const dd_player_profile_t before = profile;

  refresh_preview(game, panel, track, &profile);

  const float dpi = igGetFontSize() / 19.f;
  const float preview_size = 84.f * dpi;
  igBeginGroup();
  draw_preview(panel, preview_size);
  igEndGroup();
  igSameLine(0.f, 12.f);

  igBeginGroup();
  igPushItemWidth(igGetContentRegionAvail().x - 8.f);
  igInputTextWithHint("##name", "Name", profile.name, sizeof(profile.name), 0, NULL, NULL);
  igInputTextWithHint("##clan", "Clan", profile.clan, sizeof(profile.clan), 0, NULL, NULL);
  igInputTextWithHint("##skin", "Skin", profile.skin, sizeof(profile.skin), 0, NULL, NULL);
  igPopItemWidth();
  igEndGroup();

  const float button_width = 92.f * dpi;
  if (igButton(ICON_FA_DOWNLOAD " Fetch", (ImVec2){button_width, 0.f})) {
    char path[1024];
    if (dd_skin_fetch(game, profile.skin, path, sizeof(path))) {
      dd_gfx_load_skin_path(game, profile.skin, path);
      panel->fetch_failed = false;
      snprintf(panel->fetch_status, sizeof(panel->fetch_status), "Fetched '%s'.", profile.skin);
      panel->preview_valid = false;
      panel->preview_skin[0] = '\0';
    } else {
      panel->fetch_failed = true;
      snprintf(panel->fetch_status, sizeof(panel->fetch_status), "Could not fetch '%s'.", profile.skin);
    }
  }
  if (igIsItemHovered(0)) igSetTooltip("Download this skin by name from ddnet.org");
  igSameLine(0.f, 8.f);
  if (igButton(ICON_FA_IMAGE " Browse", (ImVec2){button_width, 0.f})) game->show_skin_browser = true;
  if (igIsItemHovered(0)) igSetTooltip("Pick a skin from the ones already downloaded");

  if (panel->fetch_status[0]) {
    if (panel->fetch_failed) igTextColored((ImVec4){1.f, .45f, .35f, 1.f}, "%s", panel->fetch_status);
    else igTextDisabled("%s", panel->fetch_status);
  }

  igSeparator();

  bool custom = profile.use_custom_color != 0;
  if (igCheckbox("Custom colors", &custom)) profile.use_custom_color = custom ? 1 : 0;
  if (custom) {
    igSeparatorText("Body");
    packed_hsl_picker("body", &profile.color_body);
    igSeparatorText("Feet");
    packed_hsl_picker("feet", &profile.color_feet);
  }

  igSeparator();
  if (igButton("Apply look to every track", (ImVec2){-1.f, 0.f})) apply_to_group(game, track, &profile);

  // Where the tee starts and what it starts with. The controls are the
  // editor's, built from the properties this module publishes as startable, so
  // adding one here is all it takes to have it appear.
  if (memcmp(&before, &profile, sizeof(profile)) != 0) dd_profile_store(game, track, &profile);
  if (igCollapsingHeader_TreeNodeFlags("Starting state", ImGuiTreeNodeFlags_DefaultOpen) && game->engine->starting_state_editor)
    game->engine->starting_state_editor(track);
  igEnd();
}

void dd_player_panel_cleanup(ft_game *game) {
  struct dd_player_panel_t *panel = game->player_panel;
  if (!panel) return;
  if (panel->preview_id && game->engine->imgui_texture_release) game->engine->imgui_texture_release(panel->preview_id);
  if (panel->preview) game->engine->texture_destroy(panel->preview);
  free(panel);
  game->player_panel = NULL;
}
