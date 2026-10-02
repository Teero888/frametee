// Panels and the start screen.
//
// The start screen shows the tracks that come with the game (GameData/Tracks
// under this game's data directory) as cards, one tab per campaign (Nations,
// United, StarTrack), the United modes and the StarTrack environments as a
// second row, grouped by series (White to Black) as the game's menus do. The
// cards show what the map's header says (tmuf_challenge_info_read): its
// thumbnail, name, environment, author and author time. Any other
// challenge or replay opens from a file dialog.

#include "tmuf_internal.h"

#include <frametee/icons.h>
#include <include_cimgui.h>

#include <math.h>
#include <ctype.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// imgui.h's, which cimgui leaves out
#ifndef IM_COL32
#define IM_COL32(R, G, B, A) (((ImU32)(A) << 24) | ((ImU32)(B) << 16) | ((ImU32)(G) << 8) | ((ImU32)(R)))
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum { TAB_NATIONS, TAB_UNITED, TAB_STAR, TAB_OTHER, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = {"Nations", "United", "StarTrack", "Other"};
// the series, in the game's order
static const char *const series_names[] = {"White", "Green", "Blue", "Red", "Black"};
#define SERIES_COUNT 5

typedef struct track_entry {
  char folder[256]; // relative to Tracks
  char name[128];   // the file's
  char path[1024];
  uint8_t tab;
  char sub[64];  // the United mode or StarTrack environment, "" for none
  int series;    // index into series_names, SERIES_COUNT for none
  // the map's header, read when first shown
  bool read;
  char title[128], environment[32], collection[32], author[64];
  uint32_t author_time, author_score, play_mode;
  ft_texture *thumb;
  ImTextureRef *thumb_ref;
} track_entry;

// a picture of the game's menus for ImGui (the whole image, or a cell of it)
typedef struct ui_icon {
  ft_texture *texture;
  ImTextureRef *ref;
  bool tried;
} ui_icon;

typedef struct ui_icon_file {
  char path[160]; // under GameData
  ui_icon icon;
} ui_icon_file;

typedef struct splash_state {
  bool scanned;
  ui_icon atlas;         // MenuForever's Icons128x128.dds: 8 x 8 icons
  ui_icon_file files[48]; // the environments' own icons
  uint32_t file_count;
  uint32_t count, cap;
  track_entry *tracks;
  int tab;
  char sub[64];
  char search[128];
  int budget; // headers read this frame (a few per frame, so it never stalls)
  const ft_engine_api *engine;
} splash_state;

typedef struct walk {
  const ft_engine_api *engine;
  splash_state *state;
  char dir[1024];
  char rel[256];
  int depth;
} walk;

static bool has_suffix(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

// the folder's place: Campaigns/<campaign>[/<mode or environment>]/<series>
static void classify(track_entry *e) {
  char parts[6][64] = {{0}};
  int n = 0;
  const char *c = e->folder;
  while (*c && n < 6) {
    const char *slash = strchr(c, '/');
    const size_t len = slash ? (size_t)(slash - c) : strlen(c);
    snprintf(parts[n++], sizeof parts[0], "%.*s", (int)len, c);
    if (!slash) break;
    c = slash + 1;
  }
  e->tab = TAB_OTHER;
  e->series = SERIES_COUNT;
  snprintf(e->sub, sizeof e->sub, "%s", e->folder);
  if (n < 3 || strcasecmp(parts[0], "Campaigns") != 0) return;
  const char *series = parts[n - 1];
  for (int i = 0; i < SERIES_COUNT; i++)
    if (strcasecmp(series, series_names[i]) == 0) e->series = i;
  if (strcasecmp(parts[1], "Nations") == 0) e->tab = TAB_NATIONS;
  else if (strcasecmp(parts[1], "United") == 0) e->tab = TAB_UNITED;
  else if (strcasecmp(parts[1], "StarTrack") == 0) e->tab = TAB_STAR;
  else return;
  // the level between the campaign and the series, or the folder under the
  // campaign when the tracks are right in it (United's Stunts)
  e->sub[0] = 0;
  if (n >= 4 || (n == 3 && e->series == SERIES_COUNT)) snprintf(e->sub, sizeof e->sub, "%s", parts[2]);
  // without a series folder, the series the name says: A White .. E Black
  // (StuntA1, StuntE5)
  if (e->series == SERIES_COUNT) {
    for (size_t i = strlen(e->name); i-- > 1;)
      if (e->name[i] >= '0' && e->name[i] <= '9' && e->name[i - 1] >= 'A' && e->name[i - 1] <= 'E' &&
          (i + 1 == strlen(e->name) || e->name[i + 1] < '0' || e->name[i + 1] > '9')) {
        e->series = e->name[i - 1] - 'A';
        break;
      }
  }
}

static void scan(walk *w);

static bool visit(void *user, const ft_directory_entry *entry) {
  walk *w = user;
  if (!entry->name || entry->name[0] == '.') return true;
  if (entry->is_directory) {
    if (w->depth >= 5) return true;
    walk sub = *w;
    snprintf(sub.dir, sizeof sub.dir, "%s/%s", w->dir, entry->name);
    snprintf(sub.rel, sizeof sub.rel, "%s%s%s", w->rel, w->rel[0] ? "/" : "", entry->name);
    sub.depth = w->depth + 1;
    scan(&sub);
    return true;
  }
  if (!has_suffix(entry->name, ".Challenge.Gbx")) return true;
  splash_state *s = w->state;
  if (s->count == s->cap) {
    const uint32_t cap = s->cap ? s->cap * 2 : 128;
    track_entry *t = realloc(s->tracks, sizeof *t * cap);
    if (!t) return false;
    s->tracks = t;
    s->cap = cap;
  }
  track_entry *e = &s->tracks[s->count++];
  memset(e, 0, sizeof *e);
  snprintf(e->folder, sizeof e->folder, "%s", w->rel);
  snprintf(e->name, sizeof e->name, "%.*s", (int)(strlen(entry->name) - strlen(".Challenge.Gbx")), entry->name);
  snprintf(e->path, sizeof e->path, "%s/%s", w->dir, entry->name);
  classify(e);
  return true;
}

static void scan(walk *w) { w->engine->visit_directory(w->dir, visit, w); }

// the game's order: campaign, mode or environment, series, then name
static int compare_tracks(const void *pa, const void *pb) {
  const track_entry *a = pa, *b = pb;
  if (a->tab != b->tab) return a->tab < b->tab ? -1 : 1;
  const int sub = strcasecmp(a->sub, b->sub);
  if (sub) return sub;
  if (a->series != b->series) return a->series < b->series ? -1 : 1;
  const int f = strcmp(a->folder, b->folder);
  return f ? f : strcmp(a->name, b->name);
}

// the environments as the game names them in its menus
static const char *environment_title(const char *collection) {
  if (strcmp(collection, "Speed") == 0) return "Desert";
  if (strcmp(collection, "Alpine") == 0) return "Snow";
  return collection;
}

// A text without the game's formatting codes: $$ is a dollar, $ and three hex
// digits a colour, $l / $h [link] a link, $ and one letter a style.
void tm_strip_formatting(const char *in, char *out, size_t size) {
  size_t n = 0;
  for (const char *c = in; *c && n + 1 < size; c++) {
    if (*c != '$') {
      out[n++] = *c;
      continue;
    }
    c++;
    if (!*c) break;
    if (*c == '$') {
      out[n++] = '$';
      continue;
    }
    const bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
    if (hex) {
      for (int k = 0; k < 2 && c[1] && ((c[1] >= '0' && c[1] <= '9') || (c[1] >= 'a' && c[1] <= 'f') ||
                                         (c[1] >= 'A' && c[1] <= 'F'));
           k++)
        c++;
      continue;
    }
    if ((*c == 'l' || *c == 'L' || *c == 'h' || *c == 'H') && c[1] == '[') {
      const char *close = strchr(c, ']');
      if (close) c = close;
    }
  }
  out[n] = 0;
}

static void read_header(const ft_engine_api *engine, track_entry *e) {
  e->read = true;
  snprintf(e->title, sizeof e->title, "%s", e->name);
  void *data = NULL;
  size_t size = 0;
  if (!engine->read_file(e->path, &data, &size)) return;
  tmuf_challenge_info *info = tmuf_challenge_info_read(data, size, NULL, 0);
  engine->free_file_data(data);
  if (!info) return;
  if (info->name[0]) tm_strip_formatting(info->name, e->title, sizeof e->title);
  snprintf(e->environment, sizeof e->environment, "%s", environment_title(info->environment));
  snprintf(e->collection, sizeof e->collection, "%s", info->environment);
  tm_strip_formatting(info->author_nickname[0] ? info->author_nickname : info->author_login, e->author,
                      sizeof e->author);
  e->author_time = info->author_time;
  e->author_score = info->author_score;
  e->play_mode = info->play_mode;
  if (info->thumbnail && engine->texture_create) {
    int w = 0, h = 0, n = 0;
    stbi_uc *rgba = stbi_load_from_memory(info->thumbnail, (int)info->thumbnail_size, &w, &h, &n, 4);
    if (rgba) {
      // the game keeps the picture bottom row first
      const size_t row = (size_t)w * 4;
      uint8_t *tmp = malloc(row);
      for (int y = 0; tmp && y < h / 2; y++) {
        memcpy(tmp, rgba + row * y, row);
        memcpy(rgba + row * y, rgba + row * (h - 1 - y), row);
        memcpy(rgba + row * (h - 1 - y), tmp, row);
      }
      free(tmp);
      const ft_texture_desc desc = {.struct_size = sizeof desc, .pixels = rgba, .width = (uint32_t)w,
                                    .height = (uint32_t)h, .layers = 1, .format = FT_TEXTURE_RGBA8,
                                    .mipmaps = false, .linear_filter = true};
      e->thumb = engine->texture_create(&desc);
      stbi_image_free(rgba);
      const uint64_t id = e->thumb && engine->imgui_texture_id ? engine->imgui_texture_id(e->thumb) : 0;
      if (id) e->thumb_ref = ImTextureRef_ImTextureRef_TextureID((ImTextureID)id);
    }
  }
  tmuf_challenge_info_free(info);
}

static void time_text(char *out, size_t size, uint32_t ms);

static void icon_load(const ft_engine_api *engine, ui_icon *icon, const char *relative) {
  icon->tried = true;
  if (!engine->texture_create || !engine->imgui_texture_id) return;
  char path[1024];
  engine->resolve_data_path(relative, path, sizeof path);
  void *data = NULL;
  size_t size = 0;
  if (!engine->read_file(path, &data, &size)) return;
  uint8_t *rgba = NULL;
  uint32_t w = 0, h = 0;
  // the file's rows as they are: top first, as ImGui draws
  const bool ok = tm_image_decode(data, size, &rgba, &w, &h);
  engine->free_file_data(data);
  if (!ok) return;
  const ft_texture_desc desc = {.struct_size = sizeof desc, .pixels = rgba, .width = w, .height = h, .layers = 1,
                                .format = FT_TEXTURE_RGBA8, .mipmaps = false, .linear_filter = true};
  icon->texture = engine->texture_create(&desc);
  free(rgba);
  const uint64_t id = icon->texture ? engine->imgui_texture_id(icon->texture) : 0;
  if (id) icon->ref = ImTextureRef_ImTextureRef_TextureID((ImTextureID)id);
}

static void icon_free(const ft_engine_api *engine, ui_icon *icon) {
  if (icon->ref) {
    const ImTextureID id = ImTextureRef_GetTexID(icon->ref);
    if (id && engine->imgui_texture_release) engine->imgui_texture_release((uint64_t)id);
    ImTextureRef_destroy(icon->ref);
  }
  if (icon->texture && engine->texture_destroy) engine->texture_destroy(icon->texture);
  memset(icon, 0, sizeof *icon);
}

// a picture under GameData, loaded once
static const ui_icon *icon_file(splash_state *s, const char *relative) {
  for (uint32_t i = 0; i < s->file_count; i++)
    if (strcmp(s->files[i].path, relative) == 0) return &s->files[i].icon;
  if (s->file_count == sizeof s->files / sizeof s->files[0]) return NULL;
  ui_icon_file *f = &s->files[s->file_count++];
  snprintf(f->path, sizeof f->path, "%s", relative);
  icon_load(s->engine, &f->icon, relative);
  return &f->icon;
}

// what a tab or header shows before its label: a cell of the atlas or a picture
typedef struct icon_ref {
  const ImTextureRef *ref;
  ImVec2 uv0, uv1;
} icon_ref;

static icon_ref atlas_cell(const splash_state *s, int row, int column) {
  return (icon_ref){s->atlas.ref, {(float)column / 8.f, (float)row / 8.f},
                    {(float)(column + 1) / 8.f, (float)(row + 1) / 8.f}};
}

static icon_ref whole(const ui_icon *icon) {
  return (icon_ref){icon ? icon->ref : NULL, {0.f, 0.f}, {1.f, 1.f}};
}

// the menus' icons: the campaigns, the series' flags, the United modes
static icon_ref tab_icon(const splash_state *s, int tab) {
  static const int cells[TAB_COUNT][2] = {{1, 1}, {1, 0}, {1, 5}, {6, 6}};
  return atlas_cell(s, cells[tab][0], cells[tab][1]);
}

static icon_ref series_icon(const splash_state *s, int series) { return atlas_cell(s, 2, 2 + series); }

// the second row's: a United mode's, or a StarTrack environment's picture
static icon_ref sub_icon(splash_state *s, const char *sub) {
  static const struct {
    const char *name;
    int row, column;
  } modes[] = {{"Race", 3, 0}, {"Platform", 3, 1}, {"Puzzle", 3, 2}, {"Stunts", 2, 7}};
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
    if (strcasecmp(sub, modes[i].name) == 0) return atlas_cell(s, modes[i].row, modes[i].column);
  static const struct {
    const char *name, *file;
  } envs[] = {{"Bay", "GameData/Bay/Media/Texture/Image/IconEnvBay.dds"},
              {"Coast", "GameData/Coast/Media/Texture/Image/IconEnvCoast.dds"},
              {"Desert", "GameData/Speed/Media/Texture/Image/IconEnvDesert.dds"},
              {"Island", "GameData/Island/Media/Texture/Image/IconEnvIsland.dds"},
              {"Rally", "GameData/Rally/Media/Texture/Image/IconEnvCountry.dds"},
              {"Snow", "GameData/Alpine/Media/Texture/Image/IconEnvSnow.dds"},
              {"Stadium", "GameData/Stadium/Media/Texture Icon/Image/IconEnvStadium.dds"}};
  for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++)
    if (strcasecmp(sub, envs[i].name) == 0) return whole(icon_file(s, envs[i].file));
  return (icon_ref){NULL, {0, 0}, {1, 1}};
}

// a tab button, DDNet's map browser's look
static bool tab_button(const char *label, icon_ref icon, bool active, float width, float height) {
  igPushID_Str(label);
  const ImVec2 at = igGetCursorScreenPos();
  const bool clicked = igInvisibleButton("##tab", (ImVec2){width, height}, 0);
  const bool hovered = igIsItemHovered(0);
  ImDrawList *dl = igGetWindowDrawList();
  const ImVec2 end = {at.x + width, at.y + height};
  const ImU32 bg = active ? IM_COL32(32, 52, 85, 245) : hovered ? IM_COL32(30, 40, 58, 220) : IM_COL32(20, 24, 34, 180);
  const ImU32 border = active ? IM_COL32(90, 175, 255, 255) : IM_COL32(50, 60, 80, 150);
  ImDrawList_AddRectFilled(dl, at, end, bg, 6.f, 0);
  ImDrawList_AddRect(dl, at, end, border, 6.f, 0, active ? 1.5f : 1.f);
  const ImVec2 ts = igCalcTextSize(label, NULL, false, -1.f);
  const float size = height - 8.f, gap = icon.ref ? size + 8.f : 0.f;
  const float x = at.x + (width - ts.x - gap) * 0.5f;
  if (icon.ref)
    ImDrawList_AddImageRounded(dl, *icon.ref, (ImVec2){x, at.y + 4.f}, (ImVec2){x + size, at.y + 4.f + size}, icon.uv0,
                               icon.uv1, 0xffffffff, 3.f, 0);
  ImDrawList_AddText_Vec2(dl, (ImVec2){x + gap, at.y + (height - ts.y) * 0.5f},
                          active ? IM_COL32(240, 245, 255, 255) : IM_COL32(160, 175, 200, 255), label, NULL);
  igPopID();
  return clicked;
}

static const ImU32 series_colours[SERIES_COUNT + 1] = {
    IM_COL32(235, 235, 235, 255), IM_COL32(70, 185, 80, 255), IM_COL32(60, 130, 230, 255),
    IM_COL32(225, 60, 60, 255),   IM_COL32(40, 40, 44, 255),  IM_COL32(120, 130, 150, 255)};

// one track's card; true when clicked
static bool card(const ft_engine_api *engine, splash_state *s, track_entry *e, float width) {
  if (!e->read && s->budget > 0) {
    s->budget--;
    read_header(engine, e);
  }
  const float thumb_h = width, text_h = 62.f, total_h = thumb_h + text_h;
  const ImVec2 at = igGetCursorScreenPos();
  const bool clicked = igInvisibleButton("##card", (ImVec2){width, total_h}, 0);
  const bool hovered = igIsItemHovered(0);
  ImDrawList *dl = igGetWindowDrawList();
  const ImVec2 end = {at.x + width, at.y + total_h};
  ImDrawList_AddRectFilled(dl, at, end, hovered ? IM_COL32(35, 46, 68, 245) : IM_COL32(22, 26, 36, 230), 8.f, 0);
  const ImVec2 thumb_end = {at.x + width, at.y + thumb_h};
  if (!e->title[0]) snprintf(e->title, sizeof e->title, "%s", e->name);
  if (e->thumb_ref)
    ImDrawList_AddImageRounded(dl, *e->thumb_ref, at, thumb_end, (ImVec2){0, 0}, (ImVec2){1, 1}, 0xffffffff, 8.f,
                               ImDrawFlags_RoundCornersTop);
  else {
    ImDrawList_AddRectFilled(dl, at, thumb_end, IM_COL32(18, 22, 30, 240), 8.f, ImDrawFlags_RoundCornersTop);
    const ImVec2 ts = igCalcTextSize(ICON_FA_FLAG_CHECKERED, NULL, false, -1.f);
    ImDrawList_AddText_Vec2(dl, (ImVec2){at.x + (width - ts.x) * 0.5f, at.y + (thumb_h - ts.y) * 0.5f},
                            IM_COL32(120, 130, 150, 255), ICON_FA_FLAG_CHECKERED, NULL);
  }
  ImDrawList_PushClipRect(dl, at, end, true);
  ImDrawList_AddText_Vec2(dl, (ImVec2){at.x + 8.f, thumb_end.y + 5.f}, IM_COL32(235, 240, 250, 255), e->title, NULL);
  char line[160];
  const float x = at.x + 8.f;
  snprintf(line, sizeof line, "%s", e->environment);
  ImDrawList_AddText_Vec2(dl, (ImVec2){x, thumb_end.y + 23.f}, IM_COL32(140, 160, 195, 255), line, NULL);
  // the author's time, or score in stunts
  char t[32] = "";
  const bool stunts = e->play_mode == 5;
  if (stunts) snprintf(t, sizeof t, "%u pts", e->author_score);
  else if (e->author_time && e->author_time != 0xffffffffu) time_text(t, sizeof t, e->author_time);
  snprintf(line, sizeof line, ICON_FA_USER " %s%s%s", e->author,
           !t[0] ? "" : stunts ? "   " ICON_FA_TROPHY " " : "   " ICON_FA_STOPWATCH " ", t);
  ImDrawList_AddText_Vec2(dl, (ImVec2){at.x + 8.f, thumb_end.y + 41.f}, IM_COL32(140, 160, 195, 255), line, NULL);
  ImDrawList_PopClipRect(dl);
  ImDrawList_AddRect(dl, at, end, hovered ? IM_COL32(90, 175, 255, 255) : IM_COL32(48, 56, 75, 140), 8.f, 0,
                     hovered ? 1.8f : 1.f);
  return clicked;
}

static int sub_rank(const char *sub) {
  static const char *const modes[] = {"Race", "Puzzle", "Platform", "Stunts"};
  for (int i = 0; i < 4; i++)
    if (strcasecmp(sub, modes[i]) == 0) return i;
  return 4;
}

static bool matches(const track_entry *e, const char *search) {
  if (!search[0]) return true;
  const size_t n = strlen(search);
  for (const char *s = e->name; *s; s++)
    if (strncasecmp(s, search, n) == 0) return true;
  for (const char *s = e->title; *s; s++)
    if (strncasecmp(s, search, n) == 0) return true;
  return false;
}

void tm_splash(const ft_engine_api *engine, void **context, const ft_ui_frame *frame) {
  tm_imgui_attach(engine);
  splash_state *s = *context;
  if (!s) {
    s = calloc(1, sizeof *s);
    if (!s) return;
    s->engine = engine;
    *context = s;
  }
  if (!s->scanned && engine->visit_directory) {
    walk w = {engine, s, "", "", 0};
    engine->resolve_data_path("GameData/Tracks", w.dir, sizeof w.dir);
    scan(&w);
    qsort(s->tracks, s->count, sizeof *s->tracks, compare_tracks);
    s->scanned = true;
    // the first campaign that has tracks
    s->tab = TAB_OTHER;
    for (uint32_t i = 0; i < s->count; i++)
      if (s->tracks[i].tab < s->tab) s->tab = s->tracks[i].tab;
  }
  if (!s->count) {
    igTextDisabled("No tracks found in data/games/tmuf/GameData/Tracks.");
    if (igButton(ICON_FA_FOLDER_OPEN " Open a challenge or replay...", (ImVec2){0, 0}) && engine->open_file_dialog) {
      char path[1024];
      if (engine->open_file_dialog("TrackMania challenge or replay", "Gbx", path, sizeof path)) engine->request_level(path);
    }
    return;
  }
  const ImVec2 avail = igGetContentRegionAvail();
  s->budget = 6;
  if (!s->atlas.tried) icon_load(engine, &s->atlas, "GameData/MenuForever/Media/Texture/Image/Icons128x128.dds");

  // the campaigns
  bool present[TAB_COUNT] = {false};
  for (uint32_t i = 0; i < s->count; i++)
    present[s->tracks[i].tab] = true;
  int shown = 0;
  for (int t = 0; t < TAB_COUNT; t++)
    shown += present[t];
  const float tab_w = fmaxf(110.f, (avail.x - 12.f * (float)(shown - 1)) / (float)(shown ? shown : 1));
  bool first = true;
  for (int t = 0; t < TAB_COUNT; t++) {
    if (!present[t]) continue;
    if (!first) igSameLine(0, 12.f);
    first = false;
    if (tab_button(tab_names[t], tab_icon(s, t), s->tab == t, tab_w, 44.f) && s->tab != t) {
      s->tab = t;
      s->sub[0] = 0;
    }
  }
  igSpacing();

  // its modes or environments
  const char *subs[32];
  int sub_count = 0;
  for (uint32_t i = 0; i < s->count && sub_count < 32; i++) {
    const track_entry *e = &s->tracks[i];
    if (e->tab != s->tab || !e->sub[0]) continue;
    bool known = false;
    for (int k = 0; k < sub_count; k++)
      known = known || strcmp(subs[k], e->sub) == 0;
    if (!known) subs[sub_count++] = e->sub;
  }
  // the United modes in the game's order, the rest by name
  for (int a = 1; a < sub_count; a++)
    for (int b = a; b > 0 && sub_rank(subs[b]) < sub_rank(subs[b - 1]); b--) {
      const char *t = subs[b];
      subs[b] = subs[b - 1];
      subs[b - 1] = t;
    }
  bool sub_found = false;
  for (int k = 0; k < sub_count; k++)
    sub_found = sub_found || strcmp(subs[k], s->sub) == 0;
  if (sub_count && !sub_found) snprintf(s->sub, sizeof s->sub, "%s", subs[0]);
  if (sub_count > 1) {
    const float sub_w = fmaxf(90.f, fminf(150.f, (avail.x - 6.f * (float)(sub_count - 1)) / (float)sub_count));
    for (int k = 0; k < sub_count; k++) {
      if (k) igSameLine(0, 6.f);
      if (tab_button(subs[k], sub_icon(s, subs[k]), strcmp(subs[k], s->sub) == 0, sub_w, 32.f))
        snprintf(s->sub, sizeof s->sub, "%s", subs[k]);
    }
    igSpacing();
  }

  // search and open
  const char *open_label = ICON_FA_FOLDER_OPEN " Open a file...";
  const float open_w = igCalcTextSize(open_label, NULL, false, -1.f).x + 24.f;
  igSetNextItemWidth(fmaxf(120.f, avail.x - open_w - 12.f));
  igInputTextWithHint("##tmuf_search", ICON_FA_MAGNIFYING_GLASS " Search tracks...", s->search, sizeof s->search, 0,
                      NULL, NULL);
  igSameLine(0, 12.f);
  if (igButton(open_label, (ImVec2){open_w, 0}) && engine->open_file_dialog) {
    char path[1024];
    if (engine->open_file_dialog("TrackMania challenge or replay", "Gbx", path, sizeof path)) engine->request_level(path);
  }
  igSpacing();

  // the cards, by series
  if (igBeginChild_Str("tmuf_tracks", (ImVec2){0, 0}, false, 0)) {
    const float grid_w = igGetContentRegionAvail().x;
    const float margin = 10.f, want = 170.f;
    int columns = (int)((grid_w + margin) / (want + margin));
    if (columns < 1) columns = 1;
    const float card_w = (grid_w - margin * (float)(columns - 1)) / (float)columns;
    const char *request = NULL;
    for (int series = 0; series <= SERIES_COUNT; series++) {
      int column = 0;
      bool header = false, drawn = false;
      for (uint32_t i = 0; i < s->count; i++) {
        track_entry *e = &s->tracks[i];
        if (e->tab != s->tab || e->series != series || (sub_count && strcmp(e->sub, s->sub) != 0) ||
            !matches(e, s->search))
          continue;
        if (!header) {
          header = true;
          if (series < SERIES_COUNT) {
            const ImVec2 at = igGetCursorScreenPos();
            const float size = 30.f, line = igGetTextLineHeight();
            const icon_ref flag = series_icon(s, series);
            if (flag.ref)
              ImDrawList_AddImage(igGetWindowDrawList(), *flag.ref, at, (ImVec2){at.x + size, at.y + size}, flag.uv0,
                                  flag.uv1, 0xffffffff);
            else
              ImDrawList_AddRectFilled(igGetWindowDrawList(), at, (ImVec2){at.x + 4.f, at.y + size},
                                       series_colours[series == 4 ? SERIES_COUNT : series], 1.f, 0);
            igSetCursorScreenPos((ImVec2){at.x + size + 8.f, at.y + (size - line) * 0.5f});
            igText("%s", series_names[series]);
            igSetCursorScreenPos((ImVec2){at.x, at.y + size + igGetStyle()->ItemSpacing.y});
          } else if (s->tab == TAB_OTHER) {
            igSpacing();
          }
        }
        if (column) igSameLine(0, margin);
        else if (drawn) // between rows only, as wide as between columns
          igSetCursorPosY(igGetCursorPosY() + margin - igGetStyle()->ItemSpacing.y);
        drawn = true;
        igPushID_Int((int)i);
        if (card(engine, s, e, card_w)) request = e->path;
        igPopID();
        column = (column + 1) % columns;
      }
    }
    if (request) engine->request_level(request);
  }
  igEndChild();
  (void)frame;
}

void tm_splash_destroy(void *context) {
  splash_state *s = context;
  if (!s) return;
  for (uint32_t i = 0; i < s->count; i++) {
    track_entry *e = &s->tracks[i];
    if (e->thumb_ref) {
      const ImTextureID id = ImTextureRef_GetTexID(e->thumb_ref);
      if (id && s->engine->imgui_texture_release) s->engine->imgui_texture_release((uint64_t)id);
      ImTextureRef_destroy(e->thumb_ref);
    }
    if (e->thumb && s->engine->texture_destroy) s->engine->texture_destroy(e->thumb);
  }
  icon_free(s->engine, &s->atlas);
  for (uint32_t i = 0; i < s->file_count; i++)
    icon_free(s->engine, &s->files[i].icon);
  free(s->tracks);
  free(s);
}

// --- the race panel -------------------------------------------------------------

static void time_text(char *out, size_t size, uint32_t ms) {
  snprintf(out, size, "%u:%02u.%02u", ms / 60000u, ms / 1000u % 60u, ms / 10u % 100u);
}

// --- skins -------------------------------------------------------------------

// The skin packs of a car, by name (the file's, without .zip): the stock
// ones (GameData) and the player's own (their documents: tm_skin_roots).
typedef struct skin_list {
  char vehicle[64];
  uint32_t count;
  struct {
    char name[64];
    bool own;
  } skins[512];
} skin_list;

typedef struct skin_visit {
  skin_list *list;
  bool own;
} skin_visit;

static bool visit_skin(void *user, const ft_directory_entry *entry) {
  skin_visit *v = user;
  skin_list *l = v->list;
  if (entry->is_directory || !has_suffix(entry->name, ".zip") || l->count == 512) return true;
  char name[64];
  snprintf(name, sizeof name, "%.*s", (int)(strlen(entry->name) - 4), entry->name);
  for (uint32_t i = 0; i < l->count; i++)
    if (strcmp(l->skins[i].name, name) == 0) return true; // (the first place's wins, as when loading)
  snprintf(l->skins[l->count].name, sizeof l->skins[0].name, "%s", name);
  l->skins[l->count++].own = v->own;
  return true;
}

static int compare_skins(const void *a, const void *b) {
  const __typeof__(((skin_list *)0)->skins[0]) *x = a, *y = b;
  if (x->own != y->own) return x->own ? -1 : 1;
  return strcmp(x->name, y->name);
}

static const skin_list *skins_of(ft_game *game, const char *vehicle) {
  static skin_list list;
  if (strcmp(list.vehicle, vehicle) == 0) return &list;
  memset(&list, 0, sizeof list);
  snprintf(list.vehicle, sizeof list.vehicle, "%s", vehicle);
  char roots[TM_SKIN_ROOTS][1024], dir[1200];
  const uint32_t n = tm_skin_roots(game, roots);
  for (uint32_t r = 0; r < n && game->engine->visit_directory; r++) {
    snprintf(dir, sizeof dir, "%s/Skins/Vehicles/%s", roots[r], vehicle);
    skin_visit v = {&list, r > 0};
    game->engine->visit_directory(dir, visit_skin, &v);
  }
  qsort(list.skins, list.count, sizeof list.skins[0], compare_skins);
  return &list;
}

// case-insensitive substring
static bool skin_matches(const char *name, const char *filter) {
  if (!filter[0]) return true;
  for (const char *s = name; *s; s++) {
    const char *a = s, *b = filter;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
    if (!*b) return true;
  }
  return false;
}

// A track's profile (tm_profile), the fields it does not have zero.
static void profile_of(const ft_engine_api *api, int32_t player, tm_profile *out) {
  memset(out, 0, sizeof *out);
  memcpy(out->magic, TM_PROFILE_MAGIC, 4);
  ft_player_setup setup = {.struct_size = sizeof setup};
  if (!api->get_player_setup || !api->get_player_setup(player, &setup)) return;
  snprintf(out->skin, sizeof out->skin, "%s", tm_profile_skin(&setup, 1, 0));
  snprintf(out->name, sizeof out->name, "%s", tm_profile_name(&setup, 1, 0));
}

// The selected track's skin: its profile's pack, picked from the car's
// (a replay's that is not on this computer shown as missing).
static void skin_picker(ft_game *game, tm_profile *profile) {
  const char *vehicle = tmuf_track_vehicle(game->level->track);
  const skin_list *l = skins_of(game, vehicle);
  // the current one's name: its file's, without .zip
  char current[64] = "";
  const char *file = profile->skin;
  for (const char *c = profile->skin; *c; c++)
    if (*c == '\\' || *c == '/') file = c + 1;
  snprintf(current, sizeof current, "%s", file);
  if (has_suffix(current, ".zip")) current[strlen(current) - 4] = 0;
  bool known = !profile->skin[0];
  for (uint32_t i = 0; i < l->count && !known; i++) known = strcmp(l->skins[i].name, current) == 0;
  char preview[96];
  if (!profile->skin[0]) snprintf(preview, sizeof preview, "Default");
  else if (known) snprintf(preview, sizeof preview, "%s", current);
  else snprintf(preview, sizeof preview, "%s (missing)", current);
  if (!igBeginCombo("Skin", preview, ImGuiComboFlags_HeightLarge)) return;
  static char filter[64];
  if (igIsWindowAppearing()) {
    filter[0] = 0;
    igSetKeyboardFocusHere(0);
  }
  igSetNextItemWidth(-FLT_MIN);
  igInputTextWithHint("##filter", "Filter", filter, sizeof filter, 0, NULL, NULL);
  if (!profile->skin[0] || skin_matches("Default", filter))
    if (igSelectable_Bool("Default", !profile->skin[0], 0, (ImVec2){0, 0})) profile->skin[0] = 0;
  if (!known) {
    char text[160];
    snprintf(text, sizeof text, "%s.zip is not on this computer", current);
    igTextDisabled("%s", text);
  }
  bool own_header = false, stock_header = false;
  for (uint32_t i = 0; i < l->count; i++) {
    if (!skin_matches(l->skins[i].name, filter)) continue;
    if (l->skins[i].own && !own_header) {
      igSeparatorText("Yours");
      own_header = true;
    } else if (!l->skins[i].own && !stock_header) {
      igSeparatorText("Stock");
      stock_header = true;
    }
    char path[256];
    snprintf(path, sizeof path, "Skins\\Vehicles\\%s\\%s.zip", vehicle, l->skins[i].name);
    if (igSelectable_Bool(l->skins[i].name, strcmp(path, profile->skin) == 0, 0, (ImVec2){0, 0}))
      snprintf(profile->skin, sizeof profile->skin, "%s", path);
  }
  igEndCombo();
}

// The selected track's player, as DDNet's Player Info: the name exported
// replays carry, the car's skin and the engine's starting-state editor. The
// race and the car are on the viewport's overlay (status_lines).
static void player_panel(ft_game *game, const ft_ui_frame *frame) {
  if (!igBegin(TM_PANEL_PLAYER, NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igEnd();
    return;
  }
  const ft_engine_api *api = game->engine;
  const int32_t player = frame->state.selected_player;
  if (player < 0 || !game->level) {
    igTextDisabled(game->level ? "No player track selected." : "No track loaded.");
    igEnd();
    return;
  }
  const ft_level *level = game->level;
  igText("%s", level->name);
  igTextDisabled("%s, %s, %s", tmuf_track_environment(level->track), tmuf_track_decoration(level->track),
                 tmuf_track_vehicle(level->track));
  igSeparator();
  tm_profile profile;
  profile_of(api, player, &profile);
  const tm_profile before = profile;
  igInputText("Name", profile.name, sizeof profile.name, 0, NULL, NULL);
  skin_picker(game, &profile);
  if (memcmp(&before, &profile, sizeof profile) != 0 && api->set_player_profile)
    api->set_player_profile(player, &profile, sizeof profile);
  if (igCollapsingHeader_TreeNodeFlags("Starting state", ImGuiTreeNodeFlags_DefaultOpen) && api->starting_state_editor) {
    api->starting_state_editor(player);
    igPushStyleColor_U32(ImGuiCol_Text, igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f));
    igTextWrapped("Replays always start on the map's start: exported replays leave these out.");
    igPopStyleColor(1);
  }
  igEnd();
}

void tm_ui(ft_game *game, const ft_ui_frame *frame) {
  tm_imgui_attach(game->engine);
  if (!frame) return;
  switch (frame->slot) {
  case FT_UI_PANELS: player_panel(game, frame); break;
  case FT_UI_MAIN_MENU:
    if (igBeginMenu("TrackMania", true)) {
      // the selected track's run, to the finish or else the playhead
      const int32_t track = frame->state.selected_player >= 0 ? frame->state.selected_player : 0;
      if (igMenuItem_Bool("Export Replay...", NULL, false, game->level != NULL) && game->engine->save_file_dialog) {
        char path[1024], name[300];
        snprintf(name, sizeof name, "%s.Replay.Gbx", game->level->name);
        if (game->engine->save_file_dialog("TrackMania replay", "Gbx", name, path, sizeof path) &&
            !tm_export_track(game, TM_EXPORT_REPLAY, path, track))
          tm_log(game, FT_LOG_ERROR, "Exporting the replay failed");
      }
      if (igMenuItem_Bool("Export TMInterface Inputs...", NULL, false, game->level != NULL) &&
          game->engine->save_file_dialog) {
        char path[1024], name[300];
        snprintf(name, sizeof name, "%s.txt", game->level->name);
        if (game->engine->save_file_dialog("TMInterface input script", "txt", name, path, sizeof path) &&
            !tm_export_track(game, TM_EXPORT_TMINTERFACE, path, track))
          tm_log(game, FT_LOG_ERROR, "Exporting the inputs failed");
      }
      igEndMenu();
    }
    break;
  default: break;
  }
}
