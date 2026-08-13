#include "dd_internal.h"
#include "dd_imgui.h"

#include <ctype.h>
#include <curl/curl.h>
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

enum { DD_BROWSER_MAX_SKINS = 1024 };

typedef struct dd_browser_skin_t {
  char name[64];
  char path[1024];
  ft_texture *preview;
  uint64_t preview_id;
  uint32_t width;
  uint32_t height;
  bool preview_attempted;
  bool visible;
} dd_browser_skin_t;

struct dd_skin_browser_t {
  dd_browser_skin_t *skins;
  int count;
  int capacity;
  char search[128];
  char status[192];
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

static bool fetch_skin(ft_game *game, const char *name, char *out_path, size_t out_size) {
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
  if (!curl) return false;
  char *encoded = curl_easy_escape(curl, name, 0);
  if (!encoded) {
    curl_easy_cleanup(curl);
    return false;
  }

  static const char *urls[] = {
      "https://skins.ddstats.tw/%s.png",
      "https://skins.ddnet.org/skin/community/%s.png",
      "https://skins.ddnet.org/skin/%s.png",
  };
  bool downloaded = false;
  for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]) && !downloaded; ++i) {
    char url[2048];
    snprintf(url, sizeof(url), urls[i], encoded);
    FILE *file = fopen(temporary, "wb");
    if (!file) break;
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
    downloaded = result == CURLE_OK && response == 200 && write_ok && rename(temporary, destination) == 0;
    if (!downloaded) remove(temporary);
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
  for (int i = 0; i < browser->count; ++i) {
    if (browser->skins[i].preview_id && game->engine->imgui_texture_release)
      game->engine->imgui_texture_release(browser->skins[i].preview_id);
    if (browser->skins[i].preview) game->engine->texture_destroy(browser->skins[i].preview);
  }
  free(browser->skins);
  browser->skins = NULL;
  browser->count = 0;
  browser->capacity = 0;
}

static void release_preview(ft_game *game, dd_browser_skin_t *skin) {
  if (skin->preview_id && game->engine->imgui_texture_release)
    game->engine->imgui_texture_release(skin->preview_id);
  if (skin->preview) game->engine->texture_destroy(skin->preview);
  skin->preview = NULL;
  skin->preview_id = 0;
  skin->preview_attempted = false;
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
    const char *subdirs[] = {"Teeworlds\\skins", "Teeworlds\\downloadedskins", "DDNet\\skins", "DDNet\\downloadedskins"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
      snprintf(path, sizeof(path), "%s\\%s", base, subdirs[i]);
      scan_directory(game, path);
    }
  }
#else
  const char *base = getenv("HOME");
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

static void load_preview(ft_game *game, dd_browser_skin_t *skin) {
  if (!skin || skin->preview_attempted) return;
  skin->preview_attempted = true;
  void *file = NULL;
  size_t file_size = 0;
  if (!game->engine->read_file(skin->path, &file, &file_size)) return;
  int width = 0, height = 0, channels = 0;
  unsigned char *pixels = dd_decode_png(file, file_size, &width, &height, &channels);
  game->engine->free_file_data(file);
  if (!pixels || width <= 0 || height <= 0 || width != height * 2) {
    dd_free_png(pixels);
    return;
  }
  ft_texture_desc desc = {.struct_size = sizeof(desc),
                          .pixels = pixels,
                          .width = (uint32_t)width,
                          .height = (uint32_t)height,
                          .layers = 1,
                          .format = FT_TEXTURE_RGBA8,
                          .mipmaps = true,
                          .linear_filter = true};
  skin->preview = game->engine->texture_create(&desc);
  if (skin->preview && game->engine->imgui_texture_id)
    skin->preview_id = game->engine->imgui_texture_id(skin->preview);
  skin->width = (uint32_t)width;
  skin->height = (uint32_t)height;
  dd_free_png(pixels);
}

static void draw_skin_card(ft_game *game, const ft_ui_frame *frame, dd_browser_skin_t *skin, int id, float width) {
  skin->visible = true;
  load_preview(game, skin);
  igPushID_Int(id);
  ft_player_setup setup = {0};
  const bool have_player = frame->state.selected_player >= 0 && game->engine->get_player_setup &&
                           game->engine->get_player_setup(frame->state.selected_player, &setup);
  const bool selected = have_player && setup.appearance_id && dd_strcasecmp(setup.appearance_id, skin->name) == 0;

  if (selected) igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){0.12f, 0.38f, 0.68f, 1.f});
  bool clicked = false;
  if (skin->preview_id) {
    const ImTextureRef_c ref = {._TexData = NULL, ._TexID = (ImTextureID)skin->preview_id};
    const float uv_x = 96.0f / 256.0f;
    const float uv_y = 96.0f / 128.0f;
    clicked = igImageButton("##skin", ref, (ImVec2){width, width}, (ImVec2){0.f, 0.f}, (ImVec2){uv_x, uv_y},
                            (ImVec4){0.08f, 0.09f, 0.11f, 1.f}, (ImVec4){1.f, 1.f, 1.f, 1.f});
  } else {
    clicked = igButton("No preview", (ImVec2){width, width});
  }
  if (selected) igPopStyleColor(1);
  igTextWrapped("%s", skin->name);

  if (clicked && have_player) {
    dd_gfx_load_skin_path(game, skin->name, skin->path);
    game->engine->set_player_appearance(frame->state.selected_player, skin->name);
  }
  igPopID();
}

void dd_skin_browser_render(ft_game *game, const ft_ui_frame *frame) {
  if (!game || !frame) return;
  if (!game->skin_browser) game->skin_browser = calloc(1, sizeof(*game->skin_browser));
  if (!game->skin_browser) return;
  struct dd_skin_browser_t *browser = game->skin_browser;
  if (!browser->scanned) scan_skins(game);

  if (!igBegin("Skin Browser", &game->show_skin_browser, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igEnd();
    return;
  }
  igSetNextItemWidth(-174.f);
  igInputTextWithHint("##skin_search", "Search / fetch skin...", browser->search, sizeof(browser->search), 0, NULL, NULL);
  igSameLine(0.f, 8.f);
  if (igButton("Fetch", (ImVec2){76.f, 0.f})) {
    char path[1024];
    if (fetch_skin(game, browser->search, path, sizeof(path))) {
      char fetched_name[sizeof(browser->search)];
      snprintf(fetched_name, sizeof(fetched_name), "%s", browser->search);
      scan_skins(game);
      snprintf(browser->search, sizeof(browser->search), "%s", fetched_name);
      snprintf(browser->status, sizeof(browser->status), "Fetched '%s'.", fetched_name);
      if (frame->state.selected_player >= 0) {
        dd_gfx_load_skin_path(game, fetched_name, path);
        game->engine->set_player_appearance(frame->state.selected_player, fetched_name);
      }
    } else {
      snprintf(browser->status, sizeof(browser->status), "Could not fetch '%s'.", browser->search);
    }
  }
  igSameLine(0.f, 8.f);
  if (igButton("Refresh", (ImVec2){76.f, 0.f})) {
    scan_skins(game);
    snprintf(browser->status, sizeof(browser->status), "Local skin folders rescanned.");
  }

  if (frame->state.selected_player < 0) igTextDisabled("Select a player to choose its skin.");
  if (browser->status[0]) igTextWrapped("%s", browser->status);
  igSeparator();

  const float card_width = 88.f;
  const float available = igGetContentRegionAvail().x;
  int columns = (int)(available / (card_width + 14.f));
  if (columns < 1) columns = 1;
  dd_browser_skin_t **filtered = browser->count ? malloc((size_t)browser->count * sizeof(*filtered)) : NULL;
  int filtered_count = 0;
  for (int i = 0; filtered && i < browser->count; ++i) {
    browser->skins[i].visible = false;
    if (contains_case_insensitive(browser->skins[i].name, browser->search)) filtered[filtered_count++] = &browser->skins[i];
  }

  if (igBeginTable("##ddnet_skin_grid", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY, (ImVec2){0.f, 0.f}, 0.f)) {
    if (filtered_count == 0) {
      igTableNextColumn();
      igTextDisabled("No matching skins found.");
    } else {
      const int rows = (filtered_count + columns - 1) / columns;
      ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
      ImGuiListClipper_Begin(clipper, rows, card_width + 42.f);
      while (ImGuiListClipper_Step(clipper)) {
        for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
          igTableNextRow(0, 0.f);
          for (int column = 0; column < columns; ++column) {
            const int filtered_index = row * columns + column;
            if (filtered_index >= filtered_count) break;
            dd_browser_skin_t *skin = filtered[filtered_index];
            igTableNextColumn();
            draw_skin_card(game, frame, skin, (int)(skin - browser->skins), card_width);
          }
        }
      }
      ImGuiListClipper_End(clipper);
      ImGuiListClipper_destroy(clipper);
    }
    igEndTable();
  }
  free(filtered);
  for (int i = 0; i < browser->count; ++i)
    if (!browser->skins[i].visible && browser->skins[i].preview) release_preview(game, &browser->skins[i]);
  igEnd();
}

void dd_skin_browser_cleanup(ft_game *game) {
  if (!game || !game->skin_browser) return;
  release_catalog(game);
  free(game->skin_browser);
  game->skin_browser = NULL;
}
