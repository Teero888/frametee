#include "skin_browser.h"
#include <system/fs.h>
#include "nfd.h"
#include "player_info.h"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "widgets/imcol.h"
#include <renderer/renderer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <symbols.h>
#include <system/include_cimgui.h>
#include <system/skin/skin_fetch.h>
#include <system/compat_threads.h>

#include <dirent.h>
#include <math.h>
#include <ctype.h>

static bool contains_case_insensitive(const char *haystack, const char *needle) {
  if (!needle || !needle[0]) return true;
  if (!haystack) return false;
  for (; *haystack; haystack++) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
      h++;
      n++;
    }
    if (!*n) return true;
  }
  return false;
}

static void draw_spinning_icon(ImVec2 center, const char* icon_text) {
    ImVec2 text_size;
    igCalcTextSize(&text_size, icon_text, NULL, false, -1.0f);
    ImVec2 top_left = {center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f};

    ImDrawList* draw_list = igGetWindowDrawList();
    int vtx_start = draw_list->VtxBuffer.Size;
    ImDrawList_AddText_Vec2(draw_list, top_left, 0xFFFFFFFF, icon_text, NULL);
    int vtx_end = draw_list->VtxBuffer.Size;
    
    float angle = igGetTime() * 10.0f;
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    
    for (int j = vtx_start; j < vtx_end; ++j) {
        ImDrawVert* v = &draw_list->VtxBuffer.Data[j];
        float dx = v->pos.x - center.x;
        float dy = v->pos.y - center.y;
        v->pos.x = center.x + (dx * cos_a - dy * sin_a);
        v->pos.y = center.y + (dx * sin_a + dy * cos_a);
    }
}

typedef struct {
  char name[128];
  char path[512];
  texture_t *texture_res;
  struct ImTextureRef *preview_texture;
  int skin_id;
  bool loaded;
  bool fetching;
  bool visible_this_frame;
} browser_skin_t;

static browser_skin_t *g_browser_skins = NULL;
static int g_browser_skins_count = 0;
static int g_browser_skins_capacity = 0;

static void add_browser_skin(const char *name, const char *path) {
  for (int i = 0; i < g_browser_skins_count; i++) {
    if (strcmp(g_browser_skins[i].name, name) == 0)
      return;
  }
  if (g_browser_skins_count >= g_browser_skins_capacity) {
    g_browser_skins_capacity = g_browser_skins_capacity == 0 ? 128 : g_browser_skins_capacity * 2;
    g_browser_skins = realloc(g_browser_skins, g_browser_skins_capacity * sizeof(browser_skin_t));
  }
  browser_skin_t *s = &g_browser_skins[g_browser_skins_count++];
  memset(s, 0, sizeof(browser_skin_t));
  snprintf(s->name, sizeof(s->name), "%.*s", (int)(sizeof(s->name) - 1), name);
  snprintf(s->path, sizeof(s->path), "%.*s", (int)(sizeof(s->path) - 1), path);
  s->skin_id = -1;
  s->loaded = false;
}

static void scan_directory_for_skins(const char *path) {
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *dir;
  while ((dir = readdir(d)) != NULL) {
    if (dir->d_type == DT_REG || dir->d_type == DT_LNK || dir->d_type == DT_UNKNOWN) {
      int len = strlen(dir->d_name);
      if (len > 4 && strcasecmp(dir->d_name + len - 4, ".png") == 0) {
        char skin_name[128];
        strncpy(skin_name, dir->d_name, sizeof(skin_name) - 1);
        skin_name[sizeof(skin_name) - 1] = '\0';
        skin_name[len - 4] = '\0';

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, dir->d_name);
        add_browser_skin(skin_name, full_path);
      }
    }
  }
  closedir(d);
}

static bool g_skins_initialized = false;
static int g_skin_to_delete = -1;
static bool g_do_not_ask_again = false;

#define MAX_BROWSER_LOAD_TASKS 4
typedef struct {
  char path[512];
  volatile bool in_use;
  volatile bool done;
  browser_skin_t* target;
  texture_t* texture_res;
  int temp_skin_id;
  gfx_handler_t* h;
} browser_load_task_t;

static browser_load_task_t g_browser_load_tasks[MAX_BROWSER_LOAD_TASKS];
static pthread_t g_browser_load_threads[MAX_BROWSER_LOAD_TASKS];

static void* browser_load_thread(void* arg) {
  browser_load_task_t* task = (browser_load_task_t*)arg;
  task->temp_skin_id = renderer_load_skin_from_file(task->h, task->path, &task->texture_res);
  task->done = true;
  return NULL;
}

static void refresh_skins(gfx_handler_t *h) {
  for (int i = 0; i < MAX_BROWSER_LOAD_TASKS; ++i) {
    if (g_browser_load_tasks[i].in_use) {
      pthread_join(g_browser_load_threads[i], NULL);
      if (g_browser_load_tasks[i].done && g_browser_load_tasks[i].temp_skin_id >= 0) {
        if (h && g_browser_load_tasks[i].texture_res) {
          renderer_destroy_texture(h, g_browser_load_tasks[i].texture_res);
        }
        if (h) renderer_unload_skin(h, g_browser_load_tasks[i].temp_skin_id);
      }
      g_browser_load_tasks[i].in_use = false;
    }
  }

  for (int i = 0; i < g_browser_skins_count; ++i) {
    browser_skin_t *bs = &g_browser_skins[i];
    if (bs->preview_texture) {
      destroy_imgui_texture_ref(&bs->preview_texture);
    }
    if (h && bs->texture_res) {
      renderer_destroy_texture(h, bs->texture_res);
      bs->texture_res = NULL;
    }
    bs->loaded = false;
    bs->fetching = false;
  }
  g_browser_skins_count = 0;

  char config_dir[1018];
  if (fs_get_config_dir(config_dir, sizeof(config_dir))) {
    char skins_dir[1024];
    snprintf(skins_dir, sizeof(skins_dir), "%s/skins", config_dir);
    scan_directory_for_skins(skins_dir);
  }

#ifdef _WIN32
  const char *appdata = getenv("APPDATA");
  if (appdata) {
    const char *paths[] = {
        "Teeworlds\\skins",
        "Teeworlds\\downloadedskins",
        "DDNet\\skins",
        "DDNet\\downloadedskins"
    };
    for (int i = 0; i < 4; i++) {
      char fallback_path[2048];
      snprintf(fallback_path, sizeof(fallback_path), "%s\\%s", appdata, paths[i]);
      scan_directory_for_skins(fallback_path);
    }
  }
#else
  const char *home = getenv("HOME");
  if (home) {
    const char *paths[] = {
        ".teeworlds/skins",
        ".teeworlds/downloadedskins",
        ".local/share/ddnet/skins",
        ".local/share/ddnet/downloadedskins"
    };
    for (int i = 0; i < 4; i++) {
      char fallback_path[2048];
      snprintf(fallback_path, sizeof(fallback_path), "%s/%s", home, paths[i]);
      scan_directory_for_skins(fallback_path);
    }
  }
#endif
}

void skin_browser_cleanup(gfx_handler_t *h) {
  for (int i = 0; i < MAX_BROWSER_LOAD_TASKS; ++i) {
    if (g_browser_load_tasks[i].in_use) {
      pthread_join(g_browser_load_threads[i], NULL);
      if (g_browser_load_tasks[i].done && g_browser_load_tasks[i].temp_skin_id >= 0) {
        if (h && g_browser_load_tasks[i].texture_res) {
          renderer_destroy_texture(h, g_browser_load_tasks[i].texture_res);
        }
        if (h) renderer_unload_skin(h, g_browser_load_tasks[i].temp_skin_id);
      }
      g_browser_load_tasks[i].in_use = false;
    }
  }
  for (int i = 0; i < g_browser_skins_count; ++i) {
    browser_skin_t *bs = &g_browser_skins[i];
    if (bs->preview_texture) {
      destroy_imgui_texture_ref(&bs->preview_texture);
    }
    if (h && bs->texture_res) {
      renderer_destroy_texture(h, bs->texture_res);
      bs->texture_res = NULL;
    }
  }
  if (g_browser_skins) {
    free(g_browser_skins);
    g_browser_skins = NULL;
  }
  g_browser_skins_count = 0;
  g_browser_skins_capacity = 0;
  g_skins_initialized = false;
}

void render_skin_browser(gfx_handler_t *h) {
  timeline_state_t *t = &h->user_interface.timeline;
  skin_manager_t *m = &h->user_interface.skin_manager;
  igBegin("Skin Browser", &h->user_interface.show_skin_browser, 0);

  for (int i = 0; i < g_browser_skins_count; ++i) {
    g_browser_skins[i].visible_this_frame = false;
  }

  if (!g_skins_initialized) {
    refresh_skins(h);
    g_skins_initialized = true;
  }

  static char skin_name_buf[256] = "";
  ImVec2 avail;
  igGetContentRegionAvail(&avail);
  float dpi = gfx_get_ui_scale();
  
  float btn_width = 110.0f * dpi;
  float search_width = avail.x - (btn_width * 2.0f + 16.0f);
  if (search_width < 120.0f) search_width = 120.0f;
  
  igSetNextItemWidth(search_width);
  igInputTextWithHint("##skin_name", "Search / Fetch skin...", skin_name_buf, sizeof(skin_name_buf), ImGuiInputTextFlags_None, NULL, NULL);
  igSameLine(0, 8);
  if (igButton(ICON_FA_DOWNLOAD " Fetch", (ImVec2){btn_width, 0})) {
    if (strlen(skin_name_buf) > 0) {
      char skin_path[512] = {0};
      if (fetch_skin(skin_name_buf, skin_path, sizeof(skin_path))) {
        FILE *f = fs_open(skin_path, "rb");
        if (f) {
          fseek(f, 0, SEEK_END);
          size_t file_size = ftell(f);
          fseek(f, 0, SEEK_SET);
          unsigned char *buffer = malloc(file_size);
          if (buffer) {
            if (fread(buffer, 1, file_size, f) == file_size) {
              skin_info_t info = {0};
              info.id = renderer_load_skin_from_memory(h, buffer, file_size, &info.preview_texture_res);

              if (info.id >= 0 && info.preview_texture_res) {
                info.data = buffer;
                info.data_size = file_size;
                snprintf(info.name, sizeof(info.name), "%.*s", (int)(sizeof(info.name) - 1), skin_name_buf);
                snprintf(info.path, sizeof(info.path), "%.*s", (int)(sizeof(info.path) - 1), skin_path);
                
                info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                    info.preview_texture_res->sampler, info.preview_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                
                skin_manager_add(m, &info);
                if (t->selected_player_track_index >= 0) {
                  player_info_t *pi = &t->player_tracks[t->selected_player_track_index].player_info;
                  pi->skin = info.id;
                  snprintf(pi->skin_name, sizeof(pi->skin_name), "%.*s", (int)(sizeof(pi->skin_name) - 1), info.name);
                }
                refresh_skins(h);
              } else {
                free(buffer);
              }
            } else {
              free(buffer);
            }
          }
          fclose(f);
        }
      }
    }
  }
  if (igIsItemHovered(0)) igSetTooltip("Fetch skin online by name");

  igSameLine(0, 8);
  if (igButton(ICON_FA_ROTATE " Refresh", (ImVec2){btn_width, 0})) {
    refresh_skins(h);
  }
  if (igIsItemHovered(0)) igSetTooltip("Rescan local skins directories");

  igSeparator();

  igGetContentRegionAvail(&avail);
  float window_visible_x = avail.x;
  float item_width = 114.0f * dpi;
  float item_spacing = 12.0f;
  int columns = (int)(window_visible_x / (item_width + item_spacing));
  if (columns < 1) columns = 1;

  static browser_skin_t** filtered_skins = NULL;
  static int filtered_capacity = 0;
  if (g_browser_skins_count > filtered_capacity) {
    filtered_capacity = g_browser_skins_count + 128;
    filtered_skins = realloc(filtered_skins, sizeof(browser_skin_t*) * filtered_capacity);
  }

  int filtered_count = 0;
  for (int i = 0; i < g_browser_skins_count; i++) {
    if (skin_name_buf[0] == '\0' || contains_case_insensitive(g_browser_skins[i].name, skin_name_buf)) {
      filtered_skins[filtered_count++] = &g_browser_skins[i];
    }
  }

  igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, (ImVec2){8.0f, 10.0f});

  if (igBeginTable("SkinGrid", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY, (ImVec2){0, 0}, 0)) {
    int rows = (filtered_count + columns - 1) / columns;

    ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
    ImGuiListClipper_Begin(clipper, rows, -1.0f);

    // Process finished load tasks
    for (int i = 0; i < MAX_BROWSER_LOAD_TASKS; ++i) {
      if (g_browser_load_tasks[i].in_use && g_browser_load_tasks[i].done) {
        browser_skin_t* bs_target = g_browser_load_tasks[i].target;
        if (bs_target != NULL) {
          int temp_skin_id = g_browser_load_tasks[i].temp_skin_id;
          texture_t* texture_res = g_browser_load_tasks[i].texture_res;
          if (temp_skin_id >= 0) {
            if (texture_res) {
              bs_target->texture_res = texture_res;
              bs_target->preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                  texture_res->sampler, texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            }
            renderer_unload_skin(h, temp_skin_id);
          }
          bs_target->loaded = true;
          bs_target->fetching = false;
        }
        g_browser_load_tasks[i].in_use = false;
        pthread_join(g_browser_load_threads[i], NULL);
      }
    }

    while (ImGuiListClipper_Step(clipper)) {
      for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; row++) {
        igTableNextRow(0, 0);
        for (int col = 0; col < columns; col++) {
          int skin_idx = row * columns + col;
          if (skin_idx >= filtered_count) break;

          igTableNextColumn();
          browser_skin_t *bs = filtered_skins[skin_idx];
          bs->visible_this_frame = true;

          if (!bs->loaded && !bs->fetching) {
            // Find a free load task
            for (int i = 0; i < MAX_BROWSER_LOAD_TASKS; ++i) {
              if (!g_browser_load_tasks[i].in_use) {
                g_browser_load_tasks[i].in_use = true;
                g_browser_load_tasks[i].done = false;
                g_browser_load_tasks[i].target = bs;
                g_browser_load_tasks[i].h = h;
                snprintf(g_browser_load_tasks[i].path, sizeof(g_browser_load_tasks[i].path), "%.*s", (int)(sizeof(g_browser_load_tasks[i].path) - 1), bs->path);
                bs->fetching = true;
                pthread_create(&g_browser_load_threads[i], NULL, browser_load_thread, &g_browser_load_tasks[i]);
                break;
              }
            }
          }

          igPushID_Int(skin_idx);
          ImVec2 cursor_pos;
          igGetCursorScreenPos(&cursor_pos);
          float card_height = 92.0f * dpi;
          ImVec2 card_min = cursor_pos;
          ImVec2 card_max = {cursor_pos.x + item_width, cursor_pos.y + card_height};
          ImDrawList* draw_list = igGetWindowDrawList();

          if (bs->loaded && bs->preview_texture) {
            if (igInvisibleButton("##skin_btn", (ImVec2){item_width, card_height}, 0)) {
              if (t->selected_player_track_index >= 0) {
                player_info_t *pi = &t->player_tracks[t->selected_player_track_index].player_info;
                texture_t* unused_preview = NULL;
                int real_skin_id = renderer_load_skin_from_file(h, bs->path, &unused_preview);
                
                if (real_skin_id >= 0) {
                  skin_info_t info = {0};
                  info.id = real_skin_id;
                  info.preview_texture_res = unused_preview;
                  if (unused_preview) {
                    info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                        unused_preview->sampler, unused_preview->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                  }
                  snprintf(info.name, sizeof(info.name), "%.*s", (int)(sizeof(info.name) - 1), bs->name);
                  snprintf(info.path, sizeof(info.path), "%.*s", (int)(sizeof(info.path) - 1), bs->path);
                  
                  skin_manager_add(m, &info);
                  pi->skin = info.id;
                   snprintf(pi->skin_name, sizeof(pi->skin_name), "%.*s", (int)(sizeof(pi->skin_name) - 1), bs->name);
                } else if (unused_preview) {
                  renderer_destroy_texture(h, unused_preview);
                }
              }
            }

            bool is_hovered = igIsItemHovered(0);
            bool is_selected = false;
            if (t->selected_player_track_index >= 0) {
              player_info_t *pi = &t->player_tracks[t->selected_player_track_index].player_info;
              if (pi->skin_name[0] != '\0' && strcasecmp(pi->skin_name, bs->name) == 0) {
                is_selected = true;
              } else if (pi->skin >= 0 && pi->skin < m->num_skins) {
                if (strcasecmp(m->skins[pi->skin].name, bs->name) == 0) {
                  is_selected = true;
                }
              }
            }

            if (is_selected) {
              ImDrawList_AddRectFilled(draw_list, card_min, card_max, IM_COL32(35, 75, 120, 180), 8.0f, ImDrawFlags_None);
              ImDrawList_AddRect(draw_list, card_min, card_max, IM_COL32(75, 175, 255, 255), 8.0f, ImDrawFlags_None, 2.0f);
            } else if (is_hovered) {
              ImDrawList_AddRectFilled(draw_list, card_min, card_max, IM_COL32(50, 58, 75, 160), 8.0f, ImDrawFlags_None);
              ImDrawList_AddRect(draw_list, card_min, card_max, IM_COL32(110, 190, 255, 220), 8.0f, ImDrawFlags_None, 1.5f);
            } else {
              ImDrawList_AddRectFilled(draw_list, card_min, card_max, IM_COL32(28, 32, 42, 120), 8.0f, ImDrawFlags_None);
              ImDrawList_AddRect(draw_list, card_min, card_max, IM_COL32(255, 255, 255, 15), 8.0f, ImDrawFlags_None, 1.0f);
            }

            ImVec2 img_min = {cursor_pos.x + item_width * 0.5f - 26.0f, cursor_pos.y + 4.0f};
            ImVec2 img_max = {cursor_pos.x + item_width * 0.5f + 26.0f, cursor_pos.y + 56.0f};
            ImDrawList_AddImage(draw_list, *bs->preview_texture, img_min, img_max, (ImVec2){0,0}, (ImVec2){1,1}, IM_COL32_WHITE);

            ImVec2 text_size;
            igCalcTextSize(&text_size, bs->name, NULL, false, item_width - 6.0f);
            igSetCursorScreenPos((ImVec2){cursor_pos.x + (item_width - text_size.x) * 0.5f, cursor_pos.y + 58.0f});
            igTextWrapped(bs->name);
          } else {
            igDummy((ImVec2){item_width, card_height});
            ImDrawList_AddRectFilled(draw_list, card_min, card_max, IM_COL32(22, 25, 32, 100), 8.0f, ImDrawFlags_None);
            if (bs->fetching) {
              ImVec2 center = {cursor_pos.x + item_width * 0.5f - 10.0f, cursor_pos.y + 32.0f - 10.0f};
              draw_spinning_icon(center, ICON_FA_ROTATE);
            }
          }

          igPopID();
        }
      }
    }
    ImGuiListClipper_End(clipper);
    ImGuiListClipper_destroy(clipper);
    igEndTable();
  }

  igPopStyleVar(1);

  for (int i = 0; i < g_browser_skins_count; ++i) {
    browser_skin_t *bs = &g_browser_skins[i];
    if (bs->loaded && !bs->visible_this_frame) {
      if (bs->preview_texture) {
        destroy_imgui_texture_ref(&bs->preview_texture);
      }
      if (bs->texture_res) {
        renderer_destroy_texture(h, bs->texture_res);
        bs->texture_res = NULL;
      }
      bs->loaded = false;
    }
  }

  if (g_skin_to_delete >= 0) {
    if (g_do_not_ask_again && !igIsPopupOpen_Str("Confirm Skin Delete", 0)) {
      skin_manager_remove(m, h, g_skin_to_delete);
      g_skin_to_delete = -1;
    } else {
      igOpenPopup_Str("Confirm Skin Delete", ImGuiPopupFlags_AnyPopupLevel);
    }
  }

  if (igBeginPopupModal("Confirm Skin Delete", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
    igText("Are you sure you want to delete this skin?\nThis action cannot be undone.");
    igSeparator();
    igCheckbox("Do not ask again", &g_do_not_ask_again);
    igSpacing();
    if (igButton("Delete", (ImVec2){120, 0})) {
      if (g_skin_to_delete != -1) {
        skin_manager_remove(m, h, g_skin_to_delete);
        g_skin_to_delete = -1;
      }
      igCloseCurrentPopup();
    }
    igSameLine(0, 10.0f);
    if (igButton("Cancel", (ImVec2){120, 0})) {
      g_skin_to_delete = -1;
      g_do_not_ask_again = 0;
      igCloseCurrentPopup();
    }
    igEndPopup();
  }

  igEnd();
}
