#include "skin_browser.h"
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

// // // static const char *LOG_SOURCE = "SkinBrowser";
static int g_skin_to_delete = -1;
static bool g_do_not_ask_again = false;

void render_skin_browser(gfx_handler_t *h) {
  timeline_state_t *t = &h->user_interface.timeline;
  skin_manager_t *m = &h->user_interface.skin_manager;
  igBegin("Skin Browser", &h->user_interface.show_skin_browser, 0);

  if (igButton("Load Skin...", (ImVec2){-1, 0})) {
    nfdu8filteritem_t filters[] = {{"Skin Files", "png"}};
    nfdopendialogu8args_t args = {0};
    args.filterList = filters;
    args.filterCount = 1;
    const nfdpathset_t *path_set;
    nfdresult_t result = NFD_OpenDialogMultipleU8_With(&path_set, &args);
    if (result == NFD_OKAY) {
      nfdpathsetsize_t size;
      NFD_PathSet_GetCount(path_set, &size);
      for (size_t i = 0; i < size; ++i) {
        nfdchar_t *path;
        NFD_PathSet_GetPathU8(path_set, i, &path);

        FILE *f = fopen(path, "rb");
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

                info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                    info.preview_texture_res->sampler, info.preview_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

                const char *skin_name = strrchr(path, '/');
                if (!skin_name) skin_name = strrchr(path, '\\');
                skin_name = skin_name ? skin_name + 1 : path;

                // idk if overwriting path is bad so we will make a copy
                char temp_name[32];
                strncpy(temp_name, skin_name, sizeof(temp_name) - 1);
                temp_name[sizeof(temp_name) - 1] = '\0';
                char *ext = strrchr(temp_name, '.');
                if (ext) *ext = '\0';

                strncpy(info.name, temp_name, sizeof(info.name) - 1);
                info.name[sizeof(info.name) - 1] = '\0';

                strncpy(info.path, path, sizeof(info.path) - 1);
                info.path[sizeof(info.path) - 1] = '\0';

                skin_manager_add(m, &info);
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
      NFD_PathSet_Free(path_set);
    }
  }

  igSeparator();

  ImVec2 avail;
  igGetContentRegionAvail(&avail);
  float window_visible_x = avail.x;
  float item_width = 128.0f;
  float item_padding = 16.0f;
  int columns = (int)(window_visible_x / (item_width + item_padding));
  if (columns < 1) columns = 1;

  if (igBeginTable("SkinGrid", columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0, 0}, 0)) {
    for (int i = 0; i < m->num_skins; i++) {
      igTableNextColumn();
      igPushID_Int(i);

      skin_info_t *skin = &m->skins[i];
      ImVec2 cursor_pos;
      igGetCursorScreenPos(&cursor_pos);

      igPushStyleColor_U32(ImGuiCol_Button, IM_COL32(255, 255, 255, 50));
      igSetNextItemAllowOverlap();
      if (igImageButton("##skin_preview", *skin->preview_texture, (ImVec2){item_width, 64}, (ImVec2){0, 0}, (ImVec2){1, 1}, (ImVec4){0, 0, 0, 0},
                        (ImVec4){1, 1, 1, 1})) {
        if (t->selected_player_track_index >= 0) t->player_tracks[t->selected_player_track_index].player_info.skin = skin->id;
      }
      igPopStyleColor(1);

      ImVec2 pos_after_image;
      igGetCursorScreenPos(&pos_after_image);

      igSetCursorScreenPos((ImVec2){cursor_pos.x + item_width - 20, cursor_pos.y + 2});
      if (igSmallButton(ICON_KI_TRASH)) {
        g_skin_to_delete = i;
      }

      igSetCursorScreenPos(pos_after_image);

      ImVec2 text_size;
      igCalcTextSize(&text_size, skin->name, NULL, false, item_width);
      igSetCursorPosX(igGetCursorPosX() + (item_width - text_size.x) * 0.5f);
      igTextWrapped(skin->name);

      igPopID();
    }
    igEndTable();
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
