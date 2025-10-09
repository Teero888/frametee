#include "player_info.h"
#include "../renderer/graphics_backend.h"
#include "cimgui.h"
#include "timeline.h"
#include "widgets/hsl_colorpicker.h"
#include <string.h>

static const char *LOG_SOURCE = "SkinManager";

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
      PackedHSLPicker("Color body", &player_info->color_body);
      PackedHSLPicker("Color feet", &player_info->color_feet);
    }
    if (igButton("Apply info to all players", (ImVec2){}))
      for (int i = 0; i < h->user_interface.timeline.player_track_count; ++i)
        memcpy(&h->user_interface.timeline.player_tracks[i].player_info, player_info, sizeof(player_info_t));
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
  if (m->skins) {
    free(m->skins);
  }
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

int skin_manager_remove(skin_manager_t *m, gfx_handler_t *h, int index) {
  if (!m || !h || index < 0 || index >= m->num_skins)
    return -1;

  // Unload from renderer and destroy preview
  renderer_unload_skin(h, m->skins[index].id);
  if (m->skins[index].preview_texture_res) {
    // The ImTextureID associated with this doesn't need manual cleanup,
    // ImGui's Vulkan backend handles it when the texture is destroyed.
    renderer_destroy_texture(h, m->skins[index].preview_texture_res);
  }
  // shift elements down
  for (int i = index; i < m->num_skins - 1; i++) {
    m->skins[i] = m->skins[i + 1];
  }
  --m->num_skins;
  if (m->num_skins == 0) {
    free(m->skins);
    m->skins = NULL;
  } else {
    m->skins = realloc(m->skins, m->num_skins * sizeof(skin_info_t));
  }
  return 0;
}
