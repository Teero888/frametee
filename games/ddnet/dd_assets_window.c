// The Assets window: DDNet's asset settings, with a mixer on top.
//
// A tab per kind of sheet. On the left are the packs found, as DDNet lists
// them; clicking one makes it the base of the sheet. On the right is the
// sheet as it is drawn now: clicking one of its parts picks the pack that
// part comes from instead, which is how sprites of different packs end up in
// one sheet. The sheet a mix makes can be saved as a pack of its own.

#include "dd_assets.h"
#include "dd_imcol.h"
#include "dd_imgui.h"

#include <ctype.h>
#include <frametee/icons.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COL_ACCENT IM_COL32(75, 175, 255, 255)
#define COL_ACCENT_FILL IM_COL32(35, 75, 120, 180)
#define COL_HOVER IM_COL32(110, 190, 255, 220)
#define COL_HOVER_FILL IM_COL32(50, 58, 75, 160)
#define COL_CARD IM_COL32(28, 32, 42, 120)
#define COL_CARD_BORDER IM_COL32(255, 255, 255, 15)
#define COL_MIX IM_COL32(255, 184, 77, 255)
#define COL_MIX_FILL IM_COL32(255, 184, 77, 34)
#define COL_TEXT IM_COL32(235, 240, 250, 255)
#define COL_MUTED IM_COL32(145, 153, 170, 255)
#define COL_CHECK_A IM_COL32(30, 33, 41, 255)
#define COL_CHECK_B IM_COL32(41, 45, 55, 255)
#define COL_ERROR IM_COL32(255, 115, 115, 255)
#define COL_OK IM_COL32(110, 215, 140, 255)

static ImVec4 color_vec(ImU32 color) {
  return (ImVec4){(float)((color >> IM_COL32_R_SHIFT) & 0xFF) / 255.f, (float)((color >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                  (float)((color >> IM_COL32_B_SHIFT) & 0xFF) / 255.f, (float)((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.f};
}

static bool contains_case_insensitive(const char *text, const char *needle) {
  if (!needle || !*needle) return true;
  for (; text && *text; ++text) {
    const char *a = text, *b = needle;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      ++a;
      ++b;
    }
    if (!*b) return true;
  }
  return false;
}

static const char *origin_label(int origin) {
  switch (origin) {
  case DD_ORIGIN_BUILTIN:
    return "Built-in";
  case DD_ORIGIN_FRAMETEE:
    return "FrameTee";
  case DD_ORIGIN_DDNET:
    return "DDNet";
  default:
    return "DDNet data";
  }
}

// `text`, shortened with an ellipsis until it fits `width`.
static void ellipsize(const char *text, float width, char *out, size_t out_size) {
  snprintf(out, out_size, "%s", text);
  if (igCalcTextSize(out, NULL, false, -1.f).x <= width) return;
  size_t length = strlen(out);
  while (length > 0) {
    --length;
    // (whole UTF-8 sequences only)
    while (length > 0 && ((unsigned char)out[length] & 0xC0) == 0x80)
      --length;
    snprintf(out + length, out_size - length, "...");
    if (igCalcTextSize(out, NULL, false, -1.f).x <= width) return;
  }
}

static float text_width(const char *text) { return igCalcTextSize(text, NULL, true, -1.f).x; }

static float button_width(const char *label) { return text_width(label) + igGetStyle()->FramePadding.x * 2.f; }

// What shows through transparent pixels.
static void draw_checker(ImDrawList *draw_list, ImVec2 min, ImVec2 max, float cell) {
  ImDrawList_AddRectFilled(draw_list, min, max, COL_CHECK_A, 0.f, ImDrawFlags_None);
  ImDrawList_PushClipRect(draw_list, min, max, true);
  int row = 0;
  for (float y = min.y; y < max.y; y += cell, ++row)
    for (float x = min.x + ((row & 1) ? cell : 0.f); x < max.x; x += cell * 2.f)
      ImDrawList_AddRectFilled(draw_list, (ImVec2){x, y}, (ImVec2){fminf(x + cell, max.x), fminf(y + cell, max.y)}, COL_CHECK_B,
                               0.f, ImDrawFlags_None);
  ImDrawList_PopClipRect(draw_list);
}

// The largest rectangle of aspect `w`:`h` centred in [min, max].
static void fit_rect(ImVec2 min, ImVec2 max, float w, float h, ImVec2 *out_min, ImVec2 *out_max) {
  const float box_w = max.x - min.x, box_h = max.y - min.y;
  float scale = w > 0.f && h > 0.f ? fminf(box_w / w, box_h / h) : 0.f;
  const float fit_w = w * scale, fit_h = h * scale;
  *out_min = (ImVec2){min.x + (box_w - fit_w) * .5f, min.y + (box_h - fit_h) * .5f};
  *out_max = (ImVec2){out_min->x + fit_w, out_min->y + fit_h};
}

static void draw_image(ImDrawList *draw_list, uint64_t texture_id, ImVec2 min, ImVec2 max, const float uv[4]) {
  const ImTextureRef_c ref = {._TexData = NULL, ._TexID = (ImTextureID)texture_id};
  ImDrawList_AddImage(draw_list, ref, min, max, (ImVec2){uv[0], uv[1]}, (ImVec2){uv[2], uv[3]}, IM_COL32_WHITE);
}

static void draw_centered(ImDrawList *draw_list, ImVec2 min, ImVec2 max, ImU32 color, const char *text) {
  const ImVec2 size = igCalcTextSize(text, NULL, false, -1.f);
  ImDrawList_AddText_Vec2(draw_list, (ImVec2){min.x + (max.x - min.x - size.x) * .5f, min.y + (max.y - min.y - size.y) * .5f},
                          color, text, NULL);
}

// A part of the sheet as it is drawn now, in [min, max] with its aspect.
static void draw_sheet_part(ft_game *game, int type, const dd_asset_part_t *part, ImDrawList *draw_list, ImVec2 min,
                            ImVec2 max, float dpi) {
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  ImVec2 image_min, image_max;
  fit_rect(min, max, (float)part->w, (float)part->h * ((float)info->default_h / info->grid_y) / ((float)info->default_w / info->grid_x),
           &image_min, &image_max);
  draw_checker(draw_list, image_min, image_max, 4.f * dpi);
  uint64_t texture_id;
  if (!dd_assets_sheet_texture(game, type, &texture_id)) return;
  const float uv[4] = {(float)part->x / info->grid_x, (float)part->y / info->grid_y, (float)(part->x + part->w) / info->grid_x,
                       (float)(part->y + part->h) / info->grid_y};
  draw_image(draw_list, texture_id, image_min, image_max, uv);
}

// The first part of a group stands for it in the picker.
static int group_first_part(const dd_asset_type_info_t *info, int group) {
  for (int i = 0; i < info->part_count; ++i)
    if (info->parts[i].group == group) return i;
  return -1;
}

static int group_size(const dd_asset_type_info_t *info, int group) {
  int count = 0;
  for (int i = 0; i < info->part_count; ++i)
    count += info->parts[i].group == group;
  return count;
}

// A button that opens a choice rather than doing something: as quiet as a
// field, or amber for a part that is mixed in.
static void push_choice_colors(bool mixed) {
  const ImGuiStyle *style = igGetStyle();
  if (mixed) {
    igPushStyleColor_U32(ImGuiCol_Button, IM_COL32(112, 78, 26, 255));
    igPushStyleColor_U32(ImGuiCol_ButtonHovered, IM_COL32(140, 98, 34, 255));
    igPushStyleColor_U32(ImGuiCol_ButtonActive, IM_COL32(160, 112, 40, 255));
  } else {
    igPushStyleColor_Vec4(ImGuiCol_Button, style->Colors[ImGuiCol_FrameBg]);
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, style->Colors[ImGuiCol_FrameBgHovered]);
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, style->Colors[ImGuiCol_FrameBgActive]);
  }
}

static void open_picker(dd_assets_t *assets, int part, int group) {
  assets->picker_open = true;
  assets->picker_part = part;
  assets->picker_group = group;
  assets->picker_whole = false;
  assets->picker_search[0] = '\0';
}

// --- packs ----------------------------------------------------------------------

// One pack, as a card: its preview, its name, where it is from. The base has
// the accent and a check; one parts are taken from shows how many.
static bool render_card(ft_game *game, int type, int index, ImVec2 size, float dpi, bool *open_menu) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const dd_asset_pack_t *pack = &kind->packs[index];
  igPushID_Int(index);
  const ImVec2 min = igGetCursorScreenPos();
  const ImVec2 max = {min.x + size.x, min.y + size.y};
  const bool clicked = igInvisibleButton("##pack", size, ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = igIsItemHovered(ImGuiHoveredFlags_None);
  if (igIsItemClicked(ImGuiMouseButton_Right)) {
    assets->context_pack = index;
    *open_menu = true;
  }
  igPopID();

  ImDrawList *draw_list = igGetWindowDrawList();
  const bool base = strcmp(kind->base, pack->name) == 0;
  const int used = dd_assets_pack_part_count(kind, pack->name);
  const float rounding = 8.f * dpi;
  if (base) {
    ImDrawList_AddRectFilled(draw_list, min, max, COL_ACCENT_FILL, rounding, ImDrawFlags_None);
    ImDrawList_AddRect(draw_list, min, max, COL_ACCENT, rounding, ImDrawFlags_None, 2.f * dpi);
  } else if (hovered) {
    ImDrawList_AddRectFilled(draw_list, min, max, COL_HOVER_FILL, rounding, ImDrawFlags_None);
    ImDrawList_AddRect(draw_list, min, max, COL_HOVER, rounding, ImDrawFlags_None, 1.5f * dpi);
  } else {
    ImDrawList_AddRectFilled(draw_list, min, max, COL_CARD, rounding, ImDrawFlags_None);
    ImDrawList_AddRect(draw_list, min, max, used ? COL_MIX : COL_CARD_BORDER, rounding, ImDrawFlags_None, 1.f * dpi);
  }

  const float pad = 6.f * dpi;
  const float image_w = size.x - pad * 2.f;
  const float image_h = image_w * (float)info->thumb_h / (float)info->thumb_w;
  const ImVec2 image_min = {min.x + pad, min.y + pad};
  const ImVec2 image_max = {image_min.x + image_w, image_min.y + image_h};
  draw_checker(draw_list, image_min, image_max, 6.f * dpi);
  uint64_t texture_id;
  float uv[4];
  if (pack->state == DD_PACK_READY && dd_assets_pack_uv(game, type, index, NULL, &texture_id, uv))
    draw_image(draw_list, texture_id, image_min, image_max, uv);
  else if (pack->state == DD_PACK_FAILED)
    draw_centered(draw_list, image_min, image_max, COL_ERROR, ICON_FA_TRIANGLE_EXCLAMATION);
  else
    draw_centered(draw_list, image_min, image_max, COL_MUTED, ICON_FA_SPINNER);

  if (base) {
    const float radius = 9.f * dpi;
    const ImVec2 center = {image_max.x - radius * .6f, image_min.y + radius * .6f};
    ImDrawList_AddCircleFilled(draw_list, center, radius, COL_ACCENT, 0);
    draw_centered(draw_list, (ImVec2){center.x - radius, center.y - radius}, (ImVec2){center.x + radius, center.y + radius},
                  IM_COL32_WHITE, ICON_FA_CHECK);
  }
  if (used) {
    char badge[32];
    snprintf(badge, sizeof(badge), ICON_FA_PUZZLE_PIECE " %d", used);
    const ImVec2 badge_size = igCalcTextSize(badge, NULL, false, -1.f);
    const ImVec2 badge_min = {image_min.x + 3.f * dpi, image_min.y + 3.f * dpi};
    const ImVec2 badge_max = {badge_min.x + badge_size.x + 10.f * dpi, badge_min.y + badge_size.y + 2.f * dpi};
    ImDrawList_AddRectFilled(draw_list, badge_min, badge_max, COL_MIX, 6.f * dpi, ImDrawFlags_None);
    draw_centered(draw_list, badge_min, badge_max, IM_COL32(40, 28, 8, 255), badge);
  }

  char label[DD_ASSET_NAME_SIZE + 8];
  ellipsize(pack->name, image_w, label, sizeof(label));
  const float line = igGetTextLineHeight();
  ImDrawList_AddText_Vec2(draw_list, (ImVec2){image_min.x, image_max.y + 5.f * dpi}, COL_TEXT, label, NULL);
  char detail[96];
  if (pack->state == DD_PACK_FAILED) snprintf(detail, sizeof(detail), "Could not be read");
  else if (pack->width > 0)
    snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d\xC3\x97%d%s", origin_label(pack->origin), pack->width, pack->height,
             pack->resized ? ", resized" : "");
  else snprintf(detail, sizeof(detail), "%s", origin_label(pack->origin));
  ellipsize(detail, image_w, label, sizeof(label));
  ImDrawList_AddText_Vec2(draw_list, (ImVec2){image_min.x, image_max.y + 5.f * dpi + line + 1.f * dpi},
                          pack->state == DD_PACK_FAILED ? COL_ERROR : COL_MUTED, label, NULL);
  return clicked;
}

static void render_pack_menu(ft_game *game, int type) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  if (!igBeginPopup("##pack_menu", ImGuiWindowFlags_None)) return;
  const int index = assets->context_pack;
  if (index >= 0 && index < kind->pack_count) {
    const dd_asset_pack_t *pack = &kind->packs[index];
    char name[DD_ASSET_NAME_SIZE];
    snprintf(name, sizeof(name), "%s", pack->name);
    igTextDisabled("%s", name);
    igSeparator();
    const bool base = strcmp(kind->base, name) == 0;
    if (igMenuItem_Bool("Use as base", NULL, base, !base)) dd_assets_select(game, type, name);
    if (igMenuItem_Bool("Use as base, without the mix", NULL, false, kind->override_count > 0)) {
      dd_assets_clear_mix(game, type);
      dd_assets_select(game, type, name);
    }
    igSeparator();
    if (igMenuItem_Bool("Show in file manager", NULL, false, game->engine->open_path != NULL)) {
      char directory[DD_ASSET_PATH_SIZE];
      snprintf(directory, sizeof(directory), "%s", pack->path);
      char *slash = strrchr(directory, '/');
      char *backslash = strrchr(directory, '\\');
      if (backslash && (!slash || backslash > slash)) slash = backslash;
      if (slash) *slash = '\0';
      game->engine->open_path(directory);
    }
  }
  igEndPopup();
}

static void render_packs(ft_game *game, int type, float dpi) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const ImGuiStyle *style = igGetStyle();
  const float gap = style->ItemSpacing.x;

  // Searching, reading the directories again, and the directory packs go in.
  const char *refresh_label = ICON_FA_ROTATE " Refresh";
  const char *directory_label = ICON_FA_FOLDER_OPEN " Directory " ICON_FA_CHEVRON_DOWN;
  const float buttons = button_width(refresh_label) + button_width(directory_label) + gap * 2.f;
  const float available = igGetContentRegionAvail().x;
  const bool inline_buttons = available >= 140.f * dpi + buttons;
  igSetNextItemWidth(inline_buttons ? available - buttons : available);
  igInputTextWithHint("##pack_search", ICON_FA_MAGNIFYING_GLASS " Search packs", assets->search, sizeof(assets->search), 0, NULL,
                      NULL);
  if (inline_buttons) igSameLine(0.f, gap);
  if (igButton(refresh_label, (ImVec2){0.f, 0.f})) dd_assets_scan(game, type);
  igSameLine(0.f, gap);
  if (igButton(directory_label, (ImVec2){0.f, 0.f})) igOpenPopup_Str("##directories", ImGuiPopupFlags_None);
  // The directories new packs go in: FrameTee's own, and DDNet's.
  if (igBeginPopup("##directories", ImGuiWindowFlags_None)) {
    static const int origins[] = {DD_ORIGIN_FRAMETEE, DD_ORIGIN_DDNET};
    static const char *const labels[] = {"FrameTee's assets", "DDNet's assets"};
    for (int i = 0; i < 2; ++i) {
      char directory[DD_ASSET_PATH_SIZE];
      const bool have = dd_assets_directory(game, type, origins[i], directory, sizeof(directory));
      if (igMenuItem_Bool(labels[i], have ? directory : NULL, false, have && game->engine->open_path != NULL)) {
        dd_assets_make_directory(directory);
        game->engine->open_path(directory);
      }
    }
    igEndPopup();
  }

  const int pending = dd_assets_previews_pending(kind);
  if (pending > 0)
    igTextDisabled("%d pack%s \xC2\xB7 reading %d", kind->pack_count, kind->pack_count == 1 ? "" : "s", pending);
  else
    igTextDisabled("%d pack%s", kind->pack_count, kind->pack_count == 1 ? "" : "s");

  int *shown = kind->pack_count ? malloc((size_t)kind->pack_count * sizeof(*shown)) : NULL;
  int shown_count = 0;
  for (int i = 0; shown && i < kind->pack_count; ++i)
    if (contains_case_insensitive(kind->packs[i].name, assets->search)) shown[shown_count++] = i;

  if (igBeginChild_Str("##pack_grid", (ImVec2){0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
    if (shown_count == 0) {
      igTextDisabled("%s", kind->pack_count ? "No pack matches." : "No packs found.");
    } else {
      // (clear of the scrollbar)
      const float width = igGetContentRegionAvail().x - 6.f * dpi;
      const float target = (info->thumb_w > info->thumb_h ? 176.f : 124.f) * dpi;
      int columns = (int)((width + gap) / (target + gap));
      if (columns < 1) columns = 1;
      if (columns > 64) columns = 64;
      const float card_w = fmaxf((width - gap * (float)(columns - 1)) / (float)columns, 1.f);
      const float pad = 6.f * dpi;
      const float image_h = (card_w - pad * 2.f) * (float)info->thumb_h / (float)info->thumb_w;
      const float line = igGetTextLineHeight();
      const float card_h = pad + image_h + 5.f * dpi + line * 2.f + 1.f * dpi + pad;
      const int rows = (shown_count + columns - 1) / columns;
      bool open_menu = false;
      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, rows, card_h + style->ItemSpacing.y);
      while (ImGuiListClipper_Step(clipper)) {
        for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
          for (int column = 0; column < columns; ++column) {
            const int item = row * columns + column;
            if (item >= shown_count) break;
            if (column > 0) igSameLine(0.f, gap);
            const int index = shown[item];
            if (render_card(game, type, index, (ImVec2){card_w, card_h}, dpi, &open_menu))
              dd_assets_select(game, type, kind->packs[index].name);
          }
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
      if (open_menu) igOpenPopup_Str("##pack_menu", ImGuiPopupFlags_None);
      render_pack_menu(game, type);
    }
  }
  igEndChild();
  free(shown);
}

// --- the picker -------------------------------------------------------------------

// Every pack's version of a part (or of the first part of a group), to take
// the part from. The base comes first, as taking it from the base again.
static void render_picker(ft_game *game, int type, float dpi) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  if (assets->picker_open) {
    igOpenPopup_Str("##part_picker", ImGuiPopupFlags_None);
    assets->picker_open = false;
  }
  if (!igBeginPopup("##part_picker", ImGuiWindowFlags_AlwaysAutoResize)) return;

  const int part_index = assets->picker_part;
  const bool whole = part_index < 0 || assets->picker_whole;
  const int group = part_index >= 0 ? info->parts[part_index].group : assets->picker_group;
  const int shown_part = part_index >= 0 ? part_index : group_first_part(info, group);
  if (shown_part < 0 || shown_part >= info->part_count) {
    igEndPopup();
    return;
  }
  const dd_asset_part_t *part = &info->parts[shown_part];

  // What is chosen for, and from where it comes now.
  if (whole) igText("All of %s", info->groups[group]);
  else igText("%s", part->name);
  bool overridden = false;
  const char *current = whole ? NULL : dd_assets_part_pack(kind, shown_part, &overridden);
  if (current) igTextDisabled("Now from %s%s", current, overridden ? "" : " (the base)");
  else igTextDisabled("Choose the pack the whole group comes from");
  if (part_index >= 0 && group_size(info, group) > 1) {
    char label[96];
    snprintf(label, sizeof(label), "All of %s", info->groups[group]);
    igCheckbox(label, &assets->picker_whole);
  }
  if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
  igSetNextItemWidth(4.f * 104.f * dpi + 3.f * igGetStyle()->ItemSpacing.x + igGetStyle()->ScrollbarSize);
  // (the grid below is as wide)
  igInputTextWithHint("##picker_search", ICON_FA_MAGNIFYING_GLASS " Search packs", assets->picker_search,
                      sizeof(assets->picker_search), 0, NULL, NULL);

  // Tiles shaped like the part: three wide ones a row, or four others.
  const ImGuiStyle *style = igGetStyle();
  const float gap = style->ItemSpacing.x;
  const float cell_aspect = ((float)info->default_h / info->grid_y) / ((float)info->default_w / info->grid_x);
  const float aspect = (float)part->w / ((float)part->h * cell_aspect);
  const float grid_inner = 4.f * 104.f * dpi + 3.f * gap;
  const int columns = aspect >= 2.5f ? 3 : 4;
  const float tile_w = (grid_inner - gap * (float)(columns - 1)) / (float)columns;
  const float image_h = fminf(fmaxf((tile_w - 12.f * dpi) / aspect, 36.f * dpi), 72.f * dpi);
  const float line = igGetTextLineHeight();
  const float tile_h = 6.f * dpi + image_h + 4.f * dpi + line + 6.f * dpi;
  int base_index = dd_assets_find_pack(kind, kind->base);

  int *shown = malloc(((size_t)kind->pack_count + 1) * sizeof(*shown));
  int shown_count = 0;
  if (shown) {
    // (-1: the base, as "Base")
    if (!assets->picker_search[0]) shown[shown_count++] = -1;
    for (int i = 0; i < kind->pack_count; ++i)
      if (i != base_index && contains_case_insensitive(kind->packs[i].name, assets->picker_search)) shown[shown_count++] = i;
  }
  const int rows = (shown_count + columns - 1) / columns;
  const float grid_h = fminf((float)rows * (tile_h + style->ItemSpacing.y), 420.f * dpi);
  const char *chosen = NULL;
  bool choose_base = false;
  const float grid_w = (float)columns * (tile_w + gap) - gap + style->ScrollbarSize;
  if (igBeginChild_Str("##picker_grid", (ImVec2){grid_w, fmaxf(grid_h, tile_h)}, ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
    ImDrawList *draw_list = igGetWindowDrawList();
    for (int item = 0; item < shown_count; ++item) {
      if (item % columns != 0) igSameLine(0.f, gap);
      const int pack_index = shown[item] < 0 ? base_index : shown[item];
      const dd_asset_pack_t *pack = pack_index >= 0 ? &kind->packs[pack_index] : NULL;
      const char *name = shown[item] < 0 ? kind->base : pack->name;
      const bool current_source = whole ? false : strcmp(dd_assets_part_pack(kind, shown_part, NULL), name) == 0;
      igPushID_Int(item);
      const ImVec2 min = igGetCursorScreenPos();
      const ImVec2 max = {min.x + tile_w, min.y + tile_h};
      const bool clicked = igInvisibleButton("##tile", (ImVec2){tile_w, tile_h}, ImGuiButtonFlags_MouseButtonLeft);
      const bool hovered = igIsItemHovered(ImGuiHoveredFlags_None);
      igPopID();
      if (clicked && (!pack || pack->state != DD_PACK_FAILED)) {
        chosen = name;
        choose_base = shown[item] < 0;
      }
      const float rounding = 6.f * dpi;
      if (current_source) {
        ImDrawList_AddRectFilled(draw_list, min, max, COL_ACCENT_FILL, rounding, ImDrawFlags_None);
        ImDrawList_AddRect(draw_list, min, max, COL_ACCENT, rounding, ImDrawFlags_None, 2.f * dpi);
      } else if (hovered) {
        ImDrawList_AddRectFilled(draw_list, min, max, COL_HOVER_FILL, rounding, ImDrawFlags_None);
        ImDrawList_AddRect(draw_list, min, max, COL_HOVER, rounding, ImDrawFlags_None, 1.5f * dpi);
      } else {
        ImDrawList_AddRectFilled(draw_list, min, max, COL_CARD, rounding, ImDrawFlags_None);
      }
      const ImVec2 box_min = {min.x + 6.f * dpi, min.y + 6.f * dpi};
      const ImVec2 box_max = {max.x - 6.f * dpi, box_min.y + image_h};
      ImVec2 image_min, image_max;
      fit_rect(box_min, box_max, (float)part->w, (float)part->h * cell_aspect, &image_min, &image_max);
      draw_checker(draw_list, image_min, image_max, 4.f * dpi);
      uint64_t texture_id;
      float uv[4];
      if (pack && dd_assets_pack_uv(game, type, pack_index, part, &texture_id, uv))
        draw_image(draw_list, texture_id, image_min, image_max, uv);
      else if (pack && pack->state == DD_PACK_FAILED)
        draw_centered(draw_list, image_min, image_max, COL_ERROR, ICON_FA_TRIANGLE_EXCLAMATION);
      else
        draw_centered(draw_list, image_min, image_max, COL_MUTED, ICON_FA_SPINNER);
      char label[DD_ASSET_NAME_SIZE + 16];
      char text[DD_ASSET_NAME_SIZE + 16];
      snprintf(text, sizeof(text), "%s", shown[item] < 0 ? "Base" : name);
      ellipsize(text, tile_w - 12.f * dpi, label, sizeof(label));
      const float label_w = igCalcTextSize(label, NULL, false, -1.f).x;
      ImDrawList_AddText_Vec2(draw_list, (ImVec2){min.x + (tile_w - label_w) * .5f, box_max.y + 4.f * dpi},
                              shown[item] < 0 ? COL_ACCENT : COL_TEXT, label, NULL);
    }
    if (shown_count == 0) igTextDisabled("No pack matches.");
  }
  igEndChild();
  free(shown);

  if (chosen) {
    char name[DD_ASSET_NAME_SIZE];
    snprintf(name, sizeof(name), "%s", chosen);
    const char *pack = choose_base ? NULL : name;
    if (whole) dd_assets_set_group(game, type, group, pack);
    else dd_assets_set_part(game, type, shown_part, pack);
    igCloseCurrentPopup();
  }
  igEndPopup();
}

// --- the sheet ---------------------------------------------------------------------

static void render_part_menu(ft_game *game, int type) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  if (!igBeginPopup("##part_menu", ImGuiWindowFlags_None)) return;
  const int part = assets->picker_part;
  if (part >= 0 && part < info->part_count) {
    bool overridden;
    dd_assets_part_pack(kind, part, &overridden);
    igTextDisabled("%s", info->parts[part].name);
    igSeparator();
    if (igMenuItem_Bool("Choose pack...", NULL, false, true)) open_picker(assets, part, -1);
    const int group = info->parts[part].group;
    if (group_size(info, group) > 1) {
      char label[96];
      snprintf(label, sizeof(label), "Choose pack for all of %s...", info->groups[group]);
      if (igMenuItem_Bool(label, NULL, false, true)) open_picker(assets, -1, group);
    }
    if (igMenuItem_Bool("Take from the base", NULL, false, overridden)) dd_assets_set_part(game, type, part, NULL);
  }
  igEndPopup();
}

// The sheet as it is drawn now, on which parts are chosen: hovering shows
// where the parts are, the mixed ones are marked, a click opens the picker.
static void render_sheet_view(ft_game *game, int type, float dpi, float max_height) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const float aspect = (float)info->default_h / (float)info->default_w;
  const float available = igGetContentRegionAvail().x;
  float view_w = available, view_h = available * aspect;
  if (view_h > max_height) {
    view_h = max_height;
    view_w = view_h / aspect;
  }
  const ImVec2 cursor = igGetCursorScreenPos();
  igSetCursorScreenPos((ImVec2){cursor.x + (available - view_w) * .5f, cursor.y});
  const ImVec2 min = igGetCursorScreenPos();
  const ImVec2 max = {min.x + view_w, min.y + view_h};
  igInvisibleButton("##sheet", (ImVec2){view_w, view_h}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = igIsItemHovered(ImGuiHoveredFlags_None);

  int hovered_part = -1;
  if (hovered) {
    const ImVec2 mouse = igGetMousePos();
    const float gx = (mouse.x - min.x) / view_w * (float)info->grid_x;
    const float gy = (mouse.y - min.y) / view_h * (float)info->grid_y;
    for (int i = 0; i < info->part_count; ++i) {
      const dd_asset_part_t *part = &info->parts[i];
      if (gx >= part->x && gx < part->x + part->w && gy >= part->y && gy < part->y + part->h) {
        hovered_part = i;
        break;
      }
    }
  }
  assets->hovered_part = hovered_part;
  if (hovered_part >= 0) {
    igSetMouseCursor(ImGuiMouseCursor_Hand);
    if (igIsItemClicked(ImGuiMouseButton_Left)) open_picker(assets, hovered_part, -1);
    if (igIsItemClicked(ImGuiMouseButton_Right)) {
      assets->picker_part = hovered_part;
      igOpenPopup_Str("##part_menu", ImGuiPopupFlags_None);
    }
  }

  ImDrawList *draw_list = igGetWindowDrawList();
  draw_checker(draw_list, min, max, 8.f * dpi);
  uint64_t texture_id;
  const float whole[4] = {0.f, 0.f, 1.f, 1.f};
  if (dd_assets_sheet_texture(game, type, &texture_id)) draw_image(draw_list, texture_id, min, max, whole);
  else draw_centered(draw_list, min, max, COL_MUTED, ICON_FA_SPINNER);

  const float cell_w = view_w / (float)info->grid_x, cell_h = view_h / (float)info->grid_y;
  ImDrawList_PushClipRect(draw_list, min, max, true);
  for (int i = 0; i < info->part_count; ++i) {
    const dd_asset_part_t *part = &info->parts[i];
    const ImVec2 part_min = {min.x + part->x * cell_w, min.y + part->y * cell_h};
    const ImVec2 part_max = {part_min.x + part->w * cell_w, part_min.y + part->h * cell_h};
    bool overridden;
    dd_assets_part_pack(kind, i, &overridden);
    if (overridden) {
      ImDrawList_AddRectFilled(draw_list, part_min, part_max, COL_MIX_FILL, 0.f, ImDrawFlags_None);
      ImDrawList_AddRect(draw_list, part_min, part_max, COL_MIX, 0.f, ImDrawFlags_None, 1.5f * dpi);
    } else if (hovered && type != DD_ASSET_ENTITIES) {
      // (where the parts are, while the sheet is hovered)
      ImDrawList_AddRect(draw_list, part_min, part_max, IM_COL32(255, 255, 255, 38), 0.f, ImDrawFlags_None, 1.f);
    }
  }
  const int shown = hovered_part >= 0 ? hovered_part : assets->list_hovered_part;
  if (shown >= 0 && shown < info->part_count) {
    const dd_asset_part_t *part = &info->parts[shown];
    const ImVec2 part_min = {min.x + part->x * cell_w, min.y + part->y * cell_h};
    const ImVec2 part_max = {part_min.x + part->w * cell_w, part_min.y + part->h * cell_h};
    ImDrawList_AddRectFilled(draw_list, part_min, part_max, IM_COL32(255, 255, 255, 30), 0.f, ImDrawFlags_None);
    ImDrawList_AddRect(draw_list, part_min, part_max, IM_COL32(255, 255, 255, 235), 0.f, ImDrawFlags_None, 2.f * dpi);
  }
  ImDrawList_PopClipRect(draw_list);
  ImDrawList_AddRect(draw_list, min, max, IM_COL32(255, 255, 255, 25), 0.f, ImDrawFlags_None, 1.f);

  // What the pointer is on, or what a click does.
  if (shown >= 0 && shown < info->part_count) {
    bool overridden;
    const char *from = dd_assets_part_pack(kind, shown, &overridden);
    const char *empty = dd_assets_part_empty(kind, shown) ? ", which leaves it empty" : "";
    igTextUnformatted(info->parts[shown].name, NULL);
    igSameLine(0.f, 6.f * dpi);
    if (overridden) igTextColored(color_vec(COL_MIX), "from %s%s", from, empty);
    else igTextDisabled("from %s, the base%s", from, empty);
  } else {
    igTextDisabled("Click a part to take it from another pack.");
  }
  render_part_menu(game, type);
}

// A part in the list: what it looks like now, its name, the pack it comes from
// (a button that opens the picker) and, when mixed in, a way back to the base.
static bool render_part_row(ft_game *game, int type, int part_index, float dpi) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const dd_asset_part_t *part = &info->parts[part_index];
  bool overridden;
  const char *from = dd_assets_part_pack(kind, part_index, &overridden);
  const bool empty = dd_assets_part_empty(kind, part_index);
  const float row_h = igGetFrameHeight() + 6.f * dpi;

  igPushID_Int(part_index);
  igTableNextRow(ImGuiTableRowFlags_None, row_h);
  igTableSetColumnIndex(0);
  const ImVec2 row_min = igGetCursorScreenPos();
  const ImVec2 thumb_size = {row_h * 1.6f, row_h - 4.f * dpi};
  const ImVec2 thumb_min = {row_min.x, row_min.y + 2.f * dpi};
  igDummy((ImVec2){thumb_size.x, row_h});
  draw_sheet_part(game, type, part, igGetWindowDrawList(), thumb_min,
                  (ImVec2){thumb_min.x + thumb_size.x, thumb_min.y + thumb_size.y}, dpi);
  igSameLine(0.f, 10.f * dpi);
  igSetCursorPosY(igGetCursorPosY() + (row_h - igGetTextLineHeight()) * .5f);
  igTextUnformatted(part->name, NULL);

  igTableSetColumnIndex(1);
  igSetCursorPosY(igGetCursorPosY() + (row_h - igGetFrameHeight()) * .5f);
  char text[DD_ASSET_NAME_SIZE + 32], label[DD_ASSET_NAME_SIZE + 48];
  snprintf(text, sizeof(text), "%s%s", from, empty ? " (empty)" : "");
  const float width = igGetContentRegionAvail().x;
  ellipsize(text, width - igGetStyle()->FramePadding.x * 2.f - text_width(" " ICON_FA_CHEVRON_DOWN), label, sizeof(label) - 16);
  strncat(label, " " ICON_FA_CHEVRON_DOWN "##source", sizeof(label) - strlen(label) - 1);
  push_choice_colors(overridden);
  if (igButton(label, (ImVec2){width, 0.f})) open_picker(assets, part_index, -1);
  igPopStyleColor(3);

  igTableSetColumnIndex(2);
  igSetCursorPosY(igGetCursorPosY() + (row_h - igGetFrameHeight()) * .5f);
  if (overridden && igButton("Reset", (ImVec2){igGetContentRegionAvail().x, 0.f})) dd_assets_set_part(game, type, part_index, NULL);
  igPopID();

  // The whole row, not clipped to the cell being drawn last.
  const ImVec2 row_max = {row_min.x + igGetWindowWidth(), row_min.y + row_h};
  const bool hovered = igIsWindowHovered(ImGuiHoveredFlags_None) && igIsMouseHoveringRect(row_min, row_max, false);
  if (hovered) igTableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 255, 255, 14), -1);
  return hovered;
}

static void render_parts(ft_game *game, int type, float dpi) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  int list_hovered = -1;
  if (igBeginChild_Str("##parts", (ImVec2){0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
    const float reset_w = button_width("Reset");
    const float source_w = fminf(fmaxf(igGetContentRegionAvail().x * .38f, 130.f * dpi), 280.f * dpi);
    if (type == DD_ASSET_ENTITIES && kind->override_count == 0)
      igTextDisabled("Tiles taken from other packs are listed here.");
    if (igBeginTable("##part_table", 3, ImGuiTableFlags_SizingFixedFit, (ImVec2){igGetContentRegionAvail().x - 6.f * dpi, 0.f},
                     0.f)) {
      igTableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
      igTableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, source_w, 0);
      igTableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, reset_w, 0);
      if (type == DD_ASSET_ENTITIES) {
        // 256 tiles are too many to list: only the ones mixed in, in order.
        for (int part = 0; part < info->part_count; ++part) {
          bool overridden;
          dd_assets_part_pack(kind, part, &overridden);
          if (overridden && render_part_row(game, type, part, dpi)) list_hovered = part;
        }
      } else {
        for (int group = 0; group < info->group_count; ++group) {
          igPushID_Int(1000 + group);
          igTableNextRow(ImGuiTableRowFlags_None, 0.f);
          igTableSetColumnIndex(0);
          if (group > 0) igDummy((ImVec2){0.f, 4.f * dpi});
          igAlignTextToFramePadding();
          igTextColored(color_vec(COL_ACCENT), "%s", info->groups[group]);
          igTableSetColumnIndex(1);
          if (group > 0) igDummy((ImVec2){0.f, 4.f * dpi});
          if (group_size(info, group) > 1) {
            push_choice_colors(false);
            if (igButton("Choose for all...", (ImVec2){igGetContentRegionAvail().x, 0.f})) open_picker(assets, -1, group);
            igPopStyleColor(3);
          }
          igTableSetColumnIndex(2);
          if (group > 0) igDummy((ImVec2){0.f, 4.f * dpi});
          bool any = false;
          for (int part = 0; part < info->part_count; ++part) {
            bool overridden;
            dd_assets_part_pack(kind, part, &overridden);
            any |= overridden && info->parts[part].group == group;
          }
          if (any && group_size(info, group) > 1 && igButton("Reset", (ImVec2){igGetContentRegionAvail().x, 0.f}))
            dd_assets_set_group(game, type, group, NULL);
          igPopID();
          for (int part = 0; part < info->part_count; ++part)
            if (info->parts[part].group == group && render_part_row(game, type, part, dpi)) list_hovered = part;
        }
      }
      igEndTable();
    }
  }
  igEndChild();
  assets->list_hovered_part = list_hovered;
}

static void render_sheet(ft_game *game, int type, float dpi) {
  dd_assets_t *assets = game->assets;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const ImGuiStyle *style = igGetStyle();
  const float gap = style->ItemSpacing.x;

  // The base, and what is done with the whole sheet.
  igAlignTextToFramePadding();
  igTextDisabled("Base");
  igSameLine(0.f, 6.f * dpi);
  igTextUnformatted(kind->base, NULL);
  const char *clear_label = ICON_FA_ERASER " Clear mix";
  const char *save_label = ICON_FA_FLOPPY_DISK " Save as PNG...";
  const float buttons = button_width(save_label) + (kind->override_count ? button_width(clear_label) + gap : 0.f);
  igSameLine(0.f, gap);
  const float room = igGetContentRegionAvail().x;
  if (room > buttons) igSetCursorPosX(igGetCursorPosX() + room - buttons);
  if (kind->override_count) {
    if (igButton(clear_label, (ImVec2){0.f, 0.f})) dd_assets_clear_mix(game, type);
    igSameLine(0.f, gap);
  }
  igBeginDisabled(assets->export_busy);
  if (igButton(save_label, (ImVec2){0.f, 0.f})) {
    assets->export_open = true;
    assets->export_type = type;
    assets->export_error[0] = '\0';
    snprintf(assets->export_name, sizeof(assets->export_name), "%.*s%s", DD_ASSET_NAME_SIZE - 5,
             strcmp(kind->base, "default") == 0 ? "custom" : kind->base, kind->override_count ? "_mix" : "");
    char directory[DD_ASSET_PATH_SIZE];
    assets->export_location = dd_assets_directory(game, type, DD_ORIGIN_DDNET, directory, sizeof(directory)) ? DD_ORIGIN_DDNET
                                                                                                             : DD_ORIGIN_FRAMETEE;
  }
  igEndDisabled();

  // What the sheet is made of.
  if (kind->override_count) {
    igTextColored(color_vec(COL_MIX), ICON_FA_PUZZLE_PIECE " %d part%s from other packs", kind->override_count,
                  kind->override_count == 1 ? "" : "s");
  } else {
    igTextDisabled("All parts from the base");
  }
  if (kind->sheet_w > 0) {
    igSameLine(0.f, 6.f * dpi);
    igTextDisabled("\xC2\xB7 %d\xC3\x97%d", kind->sheet_w, kind->sheet_h);
  }
  if (kind->applied != kind->revision) {
    igSameLine(0.f, 6.f * dpi);
    igTextDisabled(ICON_FA_SPINNER " Updating");
  }
  if (kind->error[0]) igTextColored(color_vec(COL_MIX), ICON_FA_TRIANGLE_EXCLAMATION " %s", kind->error);
  // Packs made before a sprite existed leave it empty; DDNet 20 shows the default's there.
  if (kind->empty_count > 0) {
    igAlignTextToFramePadding();
    igTextDisabled("%d part%s empty", kind->empty_count, kind->empty_count == 1 ? " is" : "s are");
    igSameLine(0.f, gap);
    igBeginDisabled(kind->applied != kind->revision);
    if (igButton("Fill from default", (ImVec2){0.f, 0.f})) dd_assets_fill_empty(game, type);
    igEndDisabled();
  }

  if (assets->saved_path[0] && assets->saved_type == type) {
    igTextColored(color_vec(COL_OK), ICON_FA_CIRCLE_CHECK " Saved %s", assets->saved_path);
    if (game->engine->open_path) {
      igSameLine(0.f, gap);
      if (igButton(ICON_FA_FOLDER_OPEN " Show", (ImVec2){0.f, 0.f})) {
        char directory[DD_ASSET_PATH_SIZE];
        snprintf(directory, sizeof(directory), "%s", assets->saved_path);
        char *slash = strrchr(directory, '/');
        char *backslash = strrchr(directory, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (slash) *slash = '\0';
        game->engine->open_path(directory);
      }
    }
    igSameLine(0.f, gap);
    if (igButton("Dismiss", (ImVec2){0.f, 0.f})) assets->saved_path[0] = '\0';
  }
  igSpacing();

  const float list_min = 150.f * dpi;
  const float max_height = fmaxf(igGetContentRegionAvail().y - list_min - igGetTextLineHeightWithSpacing() * 2.f, 120.f * dpi);
  render_sheet_view(game, type, dpi, max_height);
  igSpacing();
  render_parts(game, type, dpi);
  render_picker(game, type, dpi);
  (void)info;
}

// --- saving -----------------------------------------------------------------------

static bool file_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static void render_save(ft_game *game, float dpi) {
  dd_assets_t *assets = game->assets;
  if (assets->export_open && !igIsPopupOpen_Str("Save as PNG", ImGuiPopupFlags_None))
    igOpenPopup_Str("Save as PNG", ImGuiPopupFlags_None);
  // (as wide as this, as tall as what it holds)
  igSetNextWindowSizeConstraints((ImVec2){520.f * dpi, 0.f}, (ImVec2){520.f * dpi, 4096.f * dpi}, NULL, NULL);
  if (!igBeginPopupModal("Save as PNG", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
  if (!assets->export_open) {
    igCloseCurrentPopup();
    igEndPopup();
    return;
  }
  const int type = assets->export_type;
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  igTextWrapped("The %s sheet as it is drawn now, every part at the resolution of its pack, as a pack of its own.",
                info->label);
  igSpacing();

  igText("Name");
  if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
  igSetNextItemWidth(-1.f);
  const bool submitted = igInputText("##save_name", assets->export_name, sizeof(assets->export_name),
                                     ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

  char directories[2][DD_ASSET_PATH_SIZE];
  const bool have[2] = {dd_assets_directory(game, type, DD_ORIGIN_FRAMETEE, directories[0], sizeof(directories[0])),
                        dd_assets_directory(game, type, DD_ORIGIN_DDNET, directories[1], sizeof(directories[1]))};
  if (assets->export_location == DD_ORIGIN_DDNET && !have[1]) assets->export_location = DD_ORIGIN_FRAMETEE;
  igSpacing();
  igText("Directory");
  if (have[1] && igRadioButton_Bool("DDNet's assets, for the game and for FrameTee", assets->export_location == DD_ORIGIN_DDNET))
    assets->export_location = DD_ORIGIN_DDNET;
  if (igRadioButton_Bool("FrameTee's assets", assets->export_location == DD_ORIGIN_FRAMETEE))
    assets->export_location = DD_ORIGIN_FRAMETEE;
  const char *directory = directories[assets->export_location == DD_ORIGIN_DDNET ? 1 : 0];

  const bool valid = dd_assets_valid_name(assets->export_name);
  char path[DD_ASSET_PATH_SIZE + DD_ASSET_NAME_SIZE + 8];
  snprintf(path, sizeof(path), "%s%c%s.png", directory,
#ifdef _WIN32
           '\\',
#else
           '/',
#endif
           assets->export_name);
  igSpacing();
  if (!valid) {
    igTextColored(color_vec(COL_ERROR), "A name cannot be \"default\", start with a dot or hold / \\ : * ? \" < > |");
  } else {
    igPushTextWrapPos(0.f);
    igTextDisabled("%s", path);
    igPopTextWrapPos();
    if (file_exists(path))
      igTextColored(color_vec(COL_MIX), ICON_FA_TRIANGLE_EXCLAMATION " A pack of this name is there already; saving replaces it.");
  }
  if (assets->export_error[0]) igTextColored(color_vec(COL_ERROR), "%s", assets->export_error);

  igSpacing();
  igSeparator();
  igSpacing();
  const char *save_label = assets->export_busy ? ICON_FA_SPINNER " Saving..." : ICON_FA_FLOPPY_DISK " Save";
  igBeginDisabled(!valid || assets->export_busy || !directory[0]);
  if (igButton(save_label, (ImVec2){120.f * dpi, 0.f}) || (submitted && valid && !assets->export_busy))
    dd_assets_save(game, type, assets->export_name, directory);
  igEndDisabled();
  igSameLine(0.f, igGetStyle()->ItemSpacing.x);
  igBeginDisabled(assets->export_busy);
  if (igButton("Cancel", (ImVec2){120.f * dpi, 0.f}) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
    assets->export_open = false;
    igCloseCurrentPopup();
  }
  igEndDisabled();
  igEndPopup();
}

// --- the window ----------------------------------------------------------------------

void dd_assets_window_render(ft_game *game) {
  if (!game || game->headless) return;
  dd_assets_t *assets = dd_assets(game);
  if (!assets) return;
  const float font_size = igGetFontSize();
  const float dpi = font_size > 0.f ? font_size / 19.f : 1.f;
  // (a size for the sheet and a few columns of packs, inside the editor's window)
  const ImVec2 screen = igGetMainViewport()->WorkSize;
  igSetNextWindowSize((ImVec2){fminf(fmaxf(1180.f * dpi, 1080.f), screen.x * .9f), fminf(fmaxf(760.f * dpi, 700.f), screen.y * .9f)},
                      ImGuiCond_FirstUseEver);
  if (!igBegin("Assets", &game->show_assets, ImGuiWindowFlags_None)) {
    igEnd();
    return;
  }
  // The tab last shown opens again: asked for until the tab bar shows it.
  if (igIsWindowAppearing()) assets->restore_tab = true;
  if (igBeginTabBar("##asset_types", ImGuiTabBarFlags_None)) {
    for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type) {
      const dd_asset_type_info_t *info = &dd_asset_types[type];
      char label[64];
      snprintf(label, sizeof(label), "%s###%s", info->label, info->id);
      const ImGuiTabItemFlags flags =
          assets->restore_tab && type == assets->tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
      if (!igBeginTabItem(label, NULL, flags)) continue;
      if (assets->restore_tab && type != assets->tab) {
        igEndTabItem();
        continue;
      }
      assets->restore_tab = false;
      if (assets->tab != type) {
        assets->tab = type;
        assets->list_hovered_part = -1;
        assets->search[0] = '\0';
      }
      if (!assets->kinds[type].scanned) dd_assets_scan(game, type);
      dd_assets_upload_pages(game, type);
      // The panes keep a margin on either side of the line between them (a
      // table takes its padding when it begins, and the ones inside keep theirs).
      igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, (ImVec2){10.f * dpi, 0.f});
      const bool layout =
          igBeginTable("##asset_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, (ImVec2){0.f, 0.f}, 0.f);
      igPopStyleVar(1);
      if (layout) {
        igTableSetupColumn("Packs", ImGuiTableColumnFlags_WidthStretch, 0.42f, 0);
        igTableSetupColumn("Sheet", ImGuiTableColumnFlags_WidthStretch, 0.58f, 0);
        igTableNextRow(ImGuiTableRowFlags_None, 0.f);
        igTableSetColumnIndex(0);
        if (igBeginChild_Str("##packs_pane", (ImVec2){0.f, igGetContentRegionAvail().y}, ImGuiChildFlags_None, ImGuiWindowFlags_None))
          render_packs(game, type, dpi);
        igEndChild();
        igTableSetColumnIndex(1);
        if (igBeginChild_Str("##sheet_pane", (ImVec2){0.f, igGetContentRegionAvail().y}, ImGuiChildFlags_None, ImGuiWindowFlags_None))
          render_sheet(game, type, dpi);
        igEndChild();
        igEndTable();
      }
      igEndTabItem();
    }
    igEndTabBar();
  }
  render_save(game, dpi);
  igEnd();
}
