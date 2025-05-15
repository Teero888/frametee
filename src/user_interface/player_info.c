#include "player_info.h"
#include "cimgui.h"
#include "nfd.h"
#include "timeline.h"
#include <string.h>

void render_player_info(timeline_state_t *ts) {
  player_info_t *player_info = &ts->player_tracks[ts->selected_player_track_index].player_info;
  if (igBegin("Player Info", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    igInputText("Name", player_info->name, 32, 0, NULL, NULL);
    igInputText("Clan", player_info->clan, 32, 0, NULL, NULL);
    igText("Skin: %s", player_info->skin);
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
        strncpy(player_info->skin, skin_name, 32);
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
