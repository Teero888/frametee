#include "dd_internal.h"
#include "dd_imcol.h"
#include "dd_imgui.h"
#include "dd_profile.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <frametee/icons.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define DD_PATH_SEP '\\'
#define dd_strcasecmp _stricmp
#define dd_mkdir(path) _mkdir(path)
#else
#define DD_PATH_SEP '/'
#include <sys/stat.h>
#include <strings.h>
#define dd_strcasecmp strcasecmp
#define dd_mkdir(path) mkdir(path, 0755)
#endif

enum {
  DD_BROWSER_MAX_SKINS = 1024,
  DD_BROWSER_PREVIEW_SIZE = 128,
  DD_BROWSER_ATLAS_SIZE = 1024,
  DD_BROWSER_ATLAS_COLUMNS = DD_BROWSER_ATLAS_SIZE / DD_BROWSER_PREVIEW_SIZE,
  DD_BROWSER_PREVIEWS_PER_ATLAS = DD_BROWSER_ATLAS_COLUMNS * DD_BROWSER_ATLAS_COLUMNS,
  DD_BROWSER_MAX_ATLASES = (DD_BROWSER_MAX_SKINS + DD_BROWSER_PREVIEWS_PER_ATLAS - 1) / DD_BROWSER_PREVIEWS_PER_ATLAS,
  // Each shader preview streams two mipmapped skin-array layers and performs
  // an immediate offscreen draw. Spreading those operations over frames keeps
  // a large browser from blocking the UI when it first opens.
  DD_BROWSER_PREVIEWS_PER_FRAME = 1,
};

typedef struct dd_browser_skin_t {
  char name[64];
  char path[1024];
  bool preview_loaded;
  bool preview_attempted;
} dd_browser_skin_t;

typedef struct dd_browser_atlas_t {
  ft_texture *texture;
  uint64_t texture_id;
} dd_browser_atlas_t;

struct dd_skin_browser_t {
  dd_browser_skin_t *skins;
  dd_browser_atlas_t atlases[DD_BROWSER_MAX_ATLASES];
  int count;
  int capacity;
  char search[128];
  bool scanned;
};

typedef struct dd_scan_context_t {
  ft_game *game;
  const char *directory;
} dd_scan_context_t;

static bool has_png_extension(const char *name) {
  const size_t length = name ? strlen(name) : 0;
  return length > 4 && dd_strcasecmp(name + length - 4, ".png") == 0;
}

static bool contains_case_insensitive(const char *text, const char *needle) {
  if (!needle || !*needle) return true;
  if (!text) return false;
  for (; *text; ++text) {
    const char *a = text;
    const char *b = needle;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      ++a;
      ++b;
    }
    if (!*b) return true;
  }
  return false;
}

static bool valid_fetch_name(const char *name) {
  if (!name || !*name || strstr(name, "..")) return false;
  for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
    if (*c < 32 || *c == '/' || *c == '\\') return false;
  return true;
}

static size_t write_download(void *data, size_t size, size_t count, void *user) {
  return fwrite(data, size, count, (FILE *)user) * size;
}

bool dd_skin_fetch(ft_game *game, const char *name, char *out_path, size_t out_size) {
  if (!game || !valid_fetch_name(name) || !out_path || out_size == 0) return false;

  char directory[1024];
  game->engine->resolve_cache_path("skins", directory, sizeof(directory));
  dd_mkdir(directory);

  char destination[1024], temporary[1024];
  snprintf(destination, sizeof(destination), "%s%c%s.png", directory, DD_PATH_SEP, name);
  snprintf(temporary, sizeof(temporary), "%s.part", destination);
  FILE *existing = fopen(destination, "rb");
  if (existing) {
    fclose(existing);
    snprintf(out_path, out_size, "%s", destination);
    return true;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    dd_log(game, FT_LOG_ERROR, "Could not initialize libcurl while fetching skin '%s'.", name);
    return false;
  }
  char *encoded = curl_easy_escape(curl, name, 0);
  if (!encoded) {
    dd_log(game, FT_LOG_ERROR, "Could not URL-encode skin name '%s'.", name);
    curl_easy_cleanup(curl);
    return false;
  }

  static const char *urls[] = {
      "https://skins.ddstats.tw/%s.png",
      "https://skins.ddnet.org/skin/community/%s.png",
      "https://skins.ddnet.org/skin/%s.png",
  };
  bool downloaded = false;
  bool filesystem_error = false;
  char curl_error[CURL_ERROR_SIZE] = {0};
  char last_url[2048] = {0};
  CURLcode last_result = CURLE_OK;
  long last_response = 0;
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]) && !downloaded; ++i) {
    char url[2048];
    snprintf(url, sizeof(url), urls[i], encoded);
    FILE *file = fopen(temporary, "wb");
    if (!file) {
      dd_log(game, FT_LOG_ERROR, "Could not create skin download file '%s': %s.", temporary, strerror(errno));
      filesystem_error = true;
      break;
    }
    curl_error[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FrameTee DDNet skin browser");
    const CURLcode result = curl_easy_perform(curl);
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    const bool write_ok = fclose(file) == 0;
    const int write_error = errno;
    if (!write_ok) {
      dd_log(game, FT_LOG_ERROR, "Could not finish skin download file '%s': %s.", temporary, strerror(write_error));
      filesystem_error = true;
      remove(temporary);
      break;
    }

    if (result == CURLE_OK && response == 200) {
      if (rename(temporary, destination) == 0) {
        downloaded = true;
        dd_log(game, FT_LOG_INFO, "Downloaded skin '%s' from %s.", name, url);
      } else {
        const int rename_error = errno;
        dd_log(game, FT_LOG_ERROR, "Could not move downloaded skin into '%s': %s.", destination, strerror(rename_error));
        filesystem_error = true;
      }
    } else {
      last_result = result;
      last_response = response;
      snprintf(last_url, sizeof(last_url), "%s", url);
    }
    if (!downloaded) remove(temporary);
    if (filesystem_error) break;
  }
  if (!downloaded && !filesystem_error) {
    const char *detail = curl_error[0] ? curl_error : curl_easy_strerror(last_result);
    dd_log(game, FT_LOG_WARN, "Could not fetch skin '%s'; last GET '%s' failed: %s (curl %d, HTTP %ld).", name,
           last_url, detail, (int)last_result, last_response);
  }
  curl_free(encoded);
  curl_easy_cleanup(curl);
  if (downloaded) snprintf(out_path, out_size, "%s", destination);
  return downloaded;
}

static void add_skin(ft_game *game, const char *name, const char *path) {
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser || browser->count >= DD_BROWSER_MAX_SKINS) return;
  for (int i = 0; i < browser->count; ++i)
    if (dd_strcasecmp(browser->skins[i].name, name) == 0) return;

  if (browser->count == browser->capacity) {
    const int capacity = browser->capacity ? browser->capacity * 2 : 64;
    dd_browser_skin_t *grown = realloc(browser->skins, (size_t)capacity * sizeof(*grown));
    if (!grown) return;
    browser->skins = grown;
    browser->capacity = capacity;
  }
  dd_browser_skin_t *skin = &browser->skins[browser->count++];
  memset(skin, 0, sizeof(*skin));
  snprintf(skin->name, sizeof(skin->name), "%s", name);
  snprintf(skin->path, sizeof(skin->path), "%s", path);
}

static bool scan_entry(void *user, const ft_directory_entry *entry) {
  dd_scan_context_t *context = user;
  if (!context || !entry || entry->is_directory || !has_png_extension(entry->name)) return true;
  char name[64];
  const size_t stem = strlen(entry->name) - 4;
  snprintf(name, sizeof(name), "%.*s", (int)(stem < sizeof(name) - 1 ? stem : sizeof(name) - 1), entry->name);
  char path[1024];
  snprintf(path, sizeof(path), "%s%c%s", context->directory, DD_PATH_SEP, entry->name);
  add_skin(context->game, name, path);
  return true;
}

static void scan_directory(ft_game *game, const char *path) {
  if (!path || !*path || !game->engine->visit_directory) return;
  dd_scan_context_t context = {.game = game, .directory = path};
  game->engine->visit_directory(path, scan_entry, &context);
}

static void release_catalog(ft_game *game) {
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser) return;
  for (int i = 0; i < DD_BROWSER_MAX_ATLASES; ++i) {
    if (browser->atlases[i].texture_id && game->engine->imgui_texture_release)
      game->engine->imgui_texture_release(browser->atlases[i].texture_id);
    if (browser->atlases[i].texture) game->engine->texture_destroy(browser->atlases[i].texture);
  }
  memset(browser->atlases, 0, sizeof(browser->atlases));
  free(browser->skins);
  browser->skins = NULL;
  browser->count = 0;
  browser->capacity = 0;
}

static void scan_skins(ft_game *game) {
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser) return;
  release_catalog(game);

  char path[1024];
  game->engine->resolve_cache_path("skins", path, sizeof(path));
  scan_directory(game, path);

#ifdef _WIN32
  const char *base = getenv("APPDATA");
  if (base) {
    // Keep the user's original FrameTee skin collection visible. Downloads
    // made by the module itself remain isolated in its game cache above.
    snprintf(path, sizeof(path), "%s\\frametee\\skins", base);
    scan_directory(game, path);
    const char *subdirs[] = {"Teeworlds\\skins", "Teeworlds\\downloadedskins", "DDNet\\skins", "DDNet\\downloadedskins"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
      snprintf(path, sizeof(path), "%s\\%s", base, subdirs[i]);
      scan_directory(game, path);
    }
  }
#else
  const char *base = getenv("HOME");
  const char *config_home = getenv("XDG_CONFIG_HOME");
  if (config_home) {
    snprintf(path, sizeof(path), "%s/frametee/skins", config_home);
    scan_directory(game, path);
  } else if (base) {
    snprintf(path, sizeof(path), "%s/.config/frametee/skins", base);
    scan_directory(game, path);
  }
  if (base) {
    const char *subdirs[] = {".teeworlds/skins", ".teeworlds/downloadedskins", ".local/share/ddnet/skins",
                             ".local/share/ddnet/downloadedskins"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
      snprintf(path, sizeof(path), "%s/%s", base, subdirs[i]);
      scan_directory(game, path);
    }
  }
#endif

  game->engine->resolve_data_path("skins", path, sizeof(path));
  scan_directory(game, path);
  browser->scanned = true;
}

static dd_browser_atlas_t *ensure_preview_atlas(ft_game *game, int atlas_index) {
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser || atlas_index < 0 || atlas_index >= DD_BROWSER_MAX_ATLASES) return NULL;
  dd_browser_atlas_t *atlas = &browser->atlases[atlas_index];
  if (atlas->texture && atlas->texture_id) return atlas;

  const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                .pixels = NULL,
                                .width = DD_BROWSER_ATLAS_SIZE,
                                .height = DD_BROWSER_ATLAS_SIZE,
                                .layers = 1,
                                .format = FT_TEXTURE_RGBA8,
                                .mipmaps = false,
                                .linear_filter = true};
  atlas->texture = game->engine->texture_create(&desc);
  if (atlas->texture && game->engine->imgui_texture_id) atlas->texture_id = game->engine->imgui_texture_id(atlas->texture);
  if (!atlas->texture_id) {
    if (atlas->texture) game->engine->texture_destroy(atlas->texture);
    memset(atlas, 0, sizeof(*atlas));
    return NULL;
  }
  return atlas;
}

static void load_preview(ft_game *game, dd_browser_skin_t *skin, int skin_index) {
  if (!skin || skin->preview_attempted) return;
  skin->preview_attempted = true;
  const int atlas_index = skin_index / DD_BROWSER_PREVIEWS_PER_ATLAS;
  const int cell = skin_index % DD_BROWSER_PREVIEWS_PER_ATLAS;
  dd_browser_atlas_t *atlas = ensure_preview_atlas(game, atlas_index);
  if (!atlas) return;
  const uint32_t x = (uint32_t)(cell % DD_BROWSER_ATLAS_COLUMNS) * DD_BROWSER_PREVIEW_SIZE;
  const uint32_t y = (uint32_t)(cell / DD_BROWSER_ATLAS_COLUMNS) * DD_BROWSER_PREVIEW_SIZE;
  skin->preview_loaded = dd_gfx_render_skin_preview(game, skin->name, skin->path, NULL, atlas->texture, x, y);
}

typedef struct dd_browser_card_draw_t {
  dd_browser_skin_t *skin;
  int id;
  ImVec2 min;
  ImVec2 max;
  float width;
  bool selected;
  bool hovered;
} dd_browser_card_draw_t;

static void layout_skin_card(ft_game *game, const ft_ui_frame *frame, dd_browser_skin_t *skin, int id, float width, float height,
                             int *preview_budget, dd_browser_card_draw_t *draw) {
  if (!skin->preview_attempted && *preview_budget > 0) {
    --*preview_budget;
    load_preview(game, skin, id);
  }
  igPushID_Int(id);
  const int32_t track = frame->state.selected_player;
  const bool have_player = track >= 0;
  dd_player_profile_t profile;
  dd_profile_for_track(game, track, &profile);
  const bool selected = have_player && dd_strcasecmp(profile.skin, skin->name) == 0;

  const ImVec2 card_min = igGetCursorScreenPos();
  const ImVec2 card_max = {card_min.x + width, card_min.y + height};
  const bool clicked = igInvisibleButton("##skin", (ImVec2){width, height}, 0);
  const bool hovered = igIsItemHovered(ImGuiHoveredFlags_None);
  if (hovered) igSetTooltip("%s", skin->name);

  if (clicked && have_player) {
    dd_gfx_load_skin_path(game, skin->name, skin->path);
    snprintf(profile.skin, sizeof(profile.skin), "%s", skin->name);
    dd_profile_store(game, track, &profile);
  }
  *draw = (dd_browser_card_draw_t){.skin = skin,
                                   .id = id,
                                   .min = card_min,
                                   .max = card_max,
                                   .width = width,
                                   .selected = selected,
                                   .hovered = hovered};
  igPopID();
}

static void draw_card_backgrounds(ImDrawList *draw_list, const dd_browser_card_draw_t *cards, int count, float dpi) {
  for (int i = 0; i < count; ++i) {
    const dd_browser_card_draw_t *card = &cards[i];
    if (card->selected) {
      ImDrawList_AddRectFilled(draw_list, card->min, card->max, IM_COL32(35, 75, 120, 180), 8.f * dpi, ImDrawFlags_None);
      ImDrawList_AddRect(draw_list, card->min, card->max, IM_COL32(75, 175, 255, 255), 8.f * dpi, ImDrawFlags_None,
                         2.f * dpi);
    } else if (card->hovered) {
      ImDrawList_AddRectFilled(draw_list, card->min, card->max, IM_COL32(50, 58, 75, 160), 8.f * dpi, ImDrawFlags_None);
      ImDrawList_AddRect(draw_list, card->min, card->max, IM_COL32(110, 190, 255, 220), 8.f * dpi, ImDrawFlags_None,
                         1.5f * dpi);
    } else {
      ImDrawList_AddRectFilled(draw_list, card->min, card->max, IM_COL32(28, 32, 42, 120), 8.f * dpi, ImDrawFlags_None);
      ImDrawList_AddRect(draw_list, card->min, card->max, IM_COL32(255, 255, 255, 15), 8.f * dpi, ImDrawFlags_None,
                         1.f * dpi);
    }
  }
}

static void card_preview_rect(const dd_browser_card_draw_t *card, float dpi, ImVec2 *out_min, ImVec2 *out_max) {
  const float padding = 6.f * dpi;
  float size = 52.f * dpi;
  const float available = card->width - padding * 2.f;
  if (size > available) size = available;
  if (size < 0.f) size = 0.f;
  *out_min = (ImVec2){card->min.x + (card->width - size) * .5f, card->min.y + padding};
  *out_max = (ImVec2){out_min->x + size, out_min->y + size};
}

static void draw_card_previews(ImDrawList *draw_list, const struct dd_skin_browser_t *browser,
                               const dd_browser_card_draw_t *cards, int count, float dpi) {
  for (int i = 0; i < count; ++i) {
    const dd_browser_card_draw_t *card = &cards[i];
    if (!card->skin->preview_loaded) continue;
    const int atlas_index = card->id / DD_BROWSER_PREVIEWS_PER_ATLAS;
    const int cell = card->id % DD_BROWSER_PREVIEWS_PER_ATLAS;
    const int atlas_x = cell % DD_BROWSER_ATLAS_COLUMNS;
    const int atlas_y = cell / DD_BROWSER_ATLAS_COLUMNS;
    const float uv_cell = 1.f / DD_BROWSER_ATLAS_COLUMNS;
    const ImVec2 uv_min = {(float)atlas_x * uv_cell, (float)atlas_y * uv_cell};
    const ImVec2 uv_max = {uv_min.x + uv_cell, uv_min.y + uv_cell};
    const ImTextureRef_c ref = {._TexData = NULL, ._TexID = (ImTextureID)browser->atlases[atlas_index].texture_id};
    ImVec2 image_min, image_max;
    card_preview_rect(card, dpi, &image_min, &image_max);
    ImDrawList_AddImage(draw_list, ref, image_min, image_max, uv_min, uv_max, IM_COL32_WHITE);
  }
}

static void draw_card_labels(ImDrawList *draw_list, const dd_browser_card_draw_t *cards, int count, float dpi) {
  for (int i = 0; i < count; ++i) {
    const dd_browser_card_draw_t *card = &cards[i];
    ImVec2 preview_min, preview_max;
    card_preview_rect(card, dpi, &preview_min, &preview_max);
    if (!card->skin->preview_loaded) {
      const ImVec2 icon_size = igCalcTextSize(ICON_FA_IMAGE, NULL, false, -1.f);
      const ImVec2 icon_pos = {preview_min.x + (preview_max.x - preview_min.x - icon_size.x) * .5f,
                               preview_min.y + (preview_max.y - preview_min.y - icon_size.y) * .5f};
      ImDrawList_AddText_Vec2(draw_list, icon_pos, IM_COL32(105, 115, 135, 255), ICON_FA_IMAGE, NULL);
    }

    char label[64];
    snprintf(label, sizeof(label), "%s", card->skin->name);
    ImVec2 label_size = igCalcTextSize(label, NULL, false, -1.f);
    size_t length = strlen(label);
    const float label_width = card->width - 12.f * dpi;
    while (label_size.x > label_width && length > 3) {
      --length;
      label[length - 2] = '.';
      label[length - 1] = '.';
      label[length] = '\0';
      label_size = igCalcTextSize(label, NULL, false, -1.f);
    }
    const ImVec2 label_pos = {card->min.x + (card->width - label_size.x) * .5f,
                              card->max.y - 6.f * dpi - label_size.y};
    const ImVec2 label_clip_min = {card->min.x + 4.f * dpi, card->min.y};
    const ImVec2 label_clip_max = {card->max.x - 4.f * dpi, card->max.y};
    ImDrawList_PushClipRect(draw_list, label_clip_min, label_clip_max, true);
    ImDrawList_AddText_Vec2(draw_list, label_pos, IM_COL32(235, 240, 250, 255), label, NULL);
    ImDrawList_PopClipRect(draw_list);
  }
}

static bool skin_browser_button(const char *label, float width, const char *tooltip) {
  const bool clicked = igButton(label, (ImVec2){width, 0.f});
  if (igIsItemHovered(ImGuiHoveredFlags_None)) igSetTooltip("%s", tooltip);
  return clicked;
}

void dd_skin_browser_render(ft_game *game, const ft_ui_frame *frame) {
  if (!game || !frame) return;
  if (!game->skin_browser) game->skin_browser = calloc(1, sizeof(*game->skin_browser));
  if (!game->skin_browser) return;
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser->scanned) scan_skins(game);

  if (!igBegin("Skin Browser", &game->show_skin_browser, ImGuiWindowFlags_None)) {
    igEnd();
    return;
  }
  const float font_size = igGetFontSize();
  const float dpi = font_size > 0.f ? font_size / 19.f : 1.f;
  const ImGuiStyle *style = igGetStyle();
  const float item_gap = style->ItemSpacing.x;
  const float toolbar_width = igGetContentRegionAvail().x;
  const ImVec2 fetch_label = igCalcTextSize(ICON_FA_DOWNLOAD " Fetch", NULL, false, -1.f);
  const ImVec2 refresh_label = igCalcTextSize(ICON_FA_ROTATE " Refresh", NULL, false, -1.f);
  const float label_width = fetch_label.x > refresh_label.x ? fetch_label.x : refresh_label.x;
  const float button_width = fmaxf(96.f * dpi, label_width + style->FramePadding.x * 2.f);
  const float minimum_search_width = 140.f * dpi;
  const bool toolbar_inline = toolbar_width >= minimum_search_width + button_width * 2.f + item_gap * 2.f;
  const float search_width = toolbar_inline ? toolbar_width - button_width * 2.f - item_gap * 2.f : toolbar_width;
  igSetNextItemWidth(search_width);
  igInputTextWithHint("##skin_search", "Search / Fetch skin...", browser->search, sizeof(browser->search), 0, NULL, NULL);
  bool fetch_clicked = false;
  bool refresh_clicked = false;
  if (toolbar_inline) {
    igSameLine(0.f, item_gap);
    fetch_clicked = skin_browser_button(ICON_FA_DOWNLOAD " Fetch", button_width, "Fetch skin online by name");
    igSameLine(0.f, item_gap);
    refresh_clicked = skin_browser_button(ICON_FA_ROTATE " Refresh", button_width, "Rescan local skins directories");
  } else {
    const bool actions_inline = toolbar_width >= button_width * 2.f + item_gap;
    const float wrapped_button_width = actions_inline ? (toolbar_width - item_gap) * .5f : toolbar_width;
    fetch_clicked = skin_browser_button(ICON_FA_DOWNLOAD " Fetch", wrapped_button_width, "Fetch skin online by name");
    if (actions_inline) igSameLine(0.f, item_gap);
    refresh_clicked =
        skin_browser_button(ICON_FA_ROTATE " Refresh", wrapped_button_width, "Rescan local skins directories");
  }

  if (fetch_clicked) {
    char path[1024];
    if (dd_skin_fetch(game, browser->search, path, sizeof(path))) {
      char fetched_name[sizeof(browser->search)];
      snprintf(fetched_name, sizeof(fetched_name), "%s", browser->search);
      scan_skins(game);
      snprintf(browser->search, sizeof(browser->search), "%s", fetched_name);
      if (frame->state.selected_player >= 0) {
        dd_gfx_load_skin_path(game, fetched_name, path);
        dd_player_profile_t profile;
        dd_profile_for_track(game, frame->state.selected_player, &profile);
        snprintf(profile.skin, sizeof(profile.skin), "%s", fetched_name);
        dd_profile_store(game, frame->state.selected_player, &profile);
      }
    }
  }
  if (refresh_clicked) scan_skins(game);

  igSeparator();

  const float target_card_width = 114.f * dpi;
  const float card_height = 6.f * dpi + 52.f * dpi + 4.f * dpi + igGetTextLineHeight() + 6.f * dpi;
  const ImVec2 cell_padding = {6.f * dpi, 6.f * dpi};
  int preview_budget = DD_BROWSER_PREVIEWS_PER_FRAME;
  const float available = igGetContentRegionAvail().x;
  const float usable_width = fmaxf(available - style->ScrollbarSize, 1.f);
  int columns = (int)(usable_width / (target_card_width + cell_padding.x * 2.f));
  if (columns < 1) columns = 1;
  if (columns > 64) columns = 64;
  dd_browser_skin_t **filtered = browser->count ? malloc((size_t)browser->count * sizeof(*filtered)) : NULL;
  int filtered_count = 0;
  for (int i = 0; i < browser->count; ++i) {
    if (filtered && contains_case_insensitive(browser->skins[i].name, browser->search)) filtered[filtered_count++] = &browser->skins[i];
  }
  dd_browser_card_draw_t *card_draws = filtered_count ? malloc((size_t)filtered_count * sizeof(*card_draws)) : NULL;
  int card_draw_count = 0;
  const ImVec2 grid_clip_min = igGetCursorScreenPos();
  const ImVec2 grid_size = igGetContentRegionAvail();
  const ImVec2 grid_clip_max = {grid_clip_min.x + grid_size.x, grid_clip_min.y + grid_size.y};

  igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, cell_padding);
  if (igBeginTable("SkinGrid", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY, (ImVec2){0.f, 0.f}, 0.f)) {
    if (filtered_count == 0) {
      igTableNextColumn();
      igTextDisabled("No matching skins found.");
    } else {
      const int rows = (filtered_count + columns - 1) / columns;
      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, rows, card_height + cell_padding.y * 2.f);
      while (ImGuiListClipper_Step(clipper)) {
        for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
          igTableNextRow(0, 0.f);
          for (int column = 0; column < columns; ++column) {
            const int filtered_index = row * columns + column;
            if (filtered_index >= filtered_count) break;
            dd_browser_skin_t *skin = filtered[filtered_index];
            igTableNextColumn();
            dd_browser_card_draw_t fallback_draw;
            dd_browser_card_draw_t *card_draw = card_draws ? &card_draws[card_draw_count] : &fallback_draw;
            float card_width = igGetContentRegionAvail().x;
            if (card_width < 1.f) card_width = target_card_width;
            layout_skin_card(game, frame, skin, (int)(skin - browser->skins), card_width, card_height, &preview_budget,
                             card_draw);
            if (card_draws) ++card_draw_count;
          }
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
    }
    // Give the final cards breathing room at the maximum scroll position.
    // Without a real footer row, the table's scrolling child clips their
    // rounded lower edge against the window border.
    igTableNextRow(0, 8.f * dpi);
    igTableNextColumn();
    igDummy((ImVec2){0.f, 2.f * dpi});
    igEndTable();
  }
  if (card_draw_count > 0) {
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList_PushClipRect(draw_list, grid_clip_min, grid_clip_max, true);
    draw_card_backgrounds(draw_list, card_draws, card_draw_count, dpi);
    draw_card_previews(draw_list, browser, card_draws, card_draw_count, dpi);
    draw_card_labels(draw_list, card_draws, card_draw_count, dpi);
    ImDrawList_PopClipRect(draw_list);
  }
  igPopStyleVar(1);
  free(card_draws);
  free(filtered);
  igEnd();
}

void dd_skin_browser_cleanup(ft_game *game) {
  if (!game || !game->skin_browser) return;
  release_catalog(game);
  free(game->skin_browser);
  game->skin_browser = NULL;
}
