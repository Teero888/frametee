#include "player_info.h"
#include "../renderer/graphics_backend.h"
#include "cimgui.h"
#include "nfd.h"
#include "timeline.h"
#include <stdio.h>
#include <string.h>

// todo: make list of already loaded skins to avoid duplicates
void render_player_info(gfx_handler_t *h) {
  player_info_t *player_info =
      &h->user_interface.timeline.player_tracks[h->user_interface.timeline.selected_player_track_index]
           .player_info;
  if (igBegin("Player Info", NULL,
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                  ImGuiWindowFlags_NoFocusOnAppearing)) {
    igInputText("Name", player_info->name, 32, 0, NULL, NULL);
    igInputText("Clan", player_info->clan, 32, 0, NULL, NULL);
    igInputInt("Skin Id", &player_info->skin, 1, 1, 0);
    igCheckbox("Use custom color", &player_info->use_custom_color);
    if (player_info->use_custom_color) {
      igColorPicker3("Color", player_info->color, 0);
    }
  }
  igEnd();
}

void skin_manager_init(skin_manager_t *m) {
  if (!m)
    return;
  m->num_skins = 0;
  m->skins = NULL;
}

void skin_manager_free(skin_manager_t *m) {
  if (!m)
    return;
  free(m->skins);
  m->skins = NULL;
  m->num_skins = 0;
}

int skin_manager_add(skin_manager_t *m, const skin_info_t *skin) {
  if (!m || !skin)
    return -1;
  skin_info_t *new_skins = realloc(m->skins, (m->num_skins + 1) * sizeof(skin_info_t));
  if (!new_skins) {
    return -1;
  }
  m->skins = new_skins;
  m->skins[m->num_skins] = *skin; // copy struct
  ++m->num_skins;
  return 0;
}

int skin_manager_remove(skin_manager_t *m, int index) {
  if (!m || index < 0 || index >= m->num_skins)
    return -1;
  // shift elements down
  for (int i = index; i < m->num_skins - 1; i++) {
    m->skins[i] = m->skins[i + 1];
  }
  --m->num_skins;
  if (m->num_skins == 0) {
    free(m->skins);
    m->skins = NULL;
    return 0;
  }
  skin_info_t *new_skins = realloc(m->skins, m->num_skins * sizeof(skin_info_t));
  if (!new_skins) {
    // keep old pointer. memory is still valid
    return -1;
  }
  m->skins = new_skins;
  return 0;
}

void render_skin_manager(gfx_handler_t *h) {
  timeline_state_t *t = &h->user_interface.timeline;
  skin_manager_t *m = &h->user_interface.skin_manager;
  igBegin("Skin manager", &h->user_interface.show_skin_manager, 0);
  for (int i = 0; i < m->num_skins; i++) {
    igPushID_Int(i);
    char buf[70];
    snprintf(buf, 70, "%d : %s", m->skins[i].id, m->skins[i].name);

    // Selectable only
    igSetNextItemAllowOverlap();
    if (igSelectable_Bool(buf, false, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){0, 0})) {
      if (t->selected_player_track_index >= 0)
        t->player_tracks[t->selected_player_track_index].player_info.skin = m->skins[i].id;
    }

    // TODO: WE CANT DELETE SKINS RIGHT NOW PLS FIX XDDDDDDDDDDD
    // ImVec2 vMin;
    // igGetContentRegionAvail(&vMin);
    // igSameLine(vMin.x - 20.f, -1.0f); // shift right
    // if (igSmallButton("X")) {
    //   skin_manager_remove(m, i);
    //   h->renderer.skin_manager.layer_used[m->skins[i].id] = false;
    // }
    igPopID();
  }

  if (igButton("Load Skin", (ImVec2){})) {
    nfdu8filteritem_t filters[] = {{"skin files", "png"}};
    nfdopendialogu8args_t args = {0};
    args.filterList = filters;
    args.filterCount = 1;
    nfdpathset_t *path_set;
    nfdresult_t result = NFD_OpenDialogMultipleU8_With(&path_set, &args);
    if (result == NFD_OKAY) {
      nfdpathsetsize_t size;
      nfdresult_t r = NFD_PathSet_GetCount(path_set, &size);
      if (r == NFD_OKAY)
        for (size_t i = 0; i < size; ++i) {
          nfdchar_t *path;
          NFD_PathSet_GetPathU8(path_set, i, &path);
          skin_info_t info = {};
          info.id = renderer_load_skin_from_file(h, path);
          if (info.id >= 0) {
            char *skin_name = strrchr(path, '/') + 1;
            int len = strlen(skin_name);
            memcpy(info.name, skin_name, imin(len, 32));
            // does a copy
            skin_manager_add(m, &info);
          }
        }
      NFD_PathSet_Free(path_set);
    } else if (result == NFD_CANCEL)
      puts("Canceled skin load.");
    else
      printf("Error: %s\n", NFD_GetError());
  }
  igEnd();
}
