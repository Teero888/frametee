#include "player_info.h"
#include "../renderer/graphics_backend.h"
#include "cimgui.h"
#include "nfd.h"
#include "timeline.h"
#include <string.h>

// todo: make list of already loaded skins to avoid duplicates
void render_player_info(gfx_handler_t *h) {
  player_info_t *player_info =
      &h->user_interface.timeline.player_tracks[h->user_interface.timeline.selected_player_track_index]
           .player_info;
  if (igBegin("Player Info", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    igInputText("Name", player_info->name, 32, 0, NULL, NULL);
    igInputText("Clan", player_info->clan, 32, 0, NULL, NULL);
    igText("Skin:");
    igSameLine(0, 10);

    if (igButton("...", (ImVec2){0, 0})) {
      nfdu8char_t *out_path;
      nfdu8filteritem_t filters[] = {{"skin files", "png"}};
      nfdopendialogu8args_t args = {0};
      args.filterList = filters;
      args.filterCount = 1;
      nfdresult_t result = NFD_OpenDialogU8_With(&out_path, &args);
      if (result == NFD_OKAY) {
        char *skin_name = strrchr(out_path, '/') + 1;
        player_info->skin = renderer_load_skin_from_file(h, out_path);
        NFD_FreePathU8(out_path);
      } else if (result == NFD_CANCEL)
        puts("Canceled skin load.");
      else
        printf("Error: %s\n", NFD_GetError());
    }
    igCheckbox("Use custom color", &player_info->use_custom_color);
    if (player_info->use_custom_color) {
      igColorPicker3("Color", player_info->color, 0);
    }
  }
  igEnd();
}
