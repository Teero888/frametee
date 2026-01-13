#include "player_info.h"
#include "cglm/types.h"
#include "ddnet_physics/collision.h"
#include "widgets/hsl_colorpicker.h"
#include <ddnet_physics/gamecore.h>
#include <ddnet_physics/vmath.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <string.h>
#include <system/include_cimgui.h>
#include <user_interface/timeline/timeline_model.h>

// static const char *LOG_SOURCE = "SkinManager";

void render_player_info(gfx_handler_t *h) {
  timeline_state_t *ts = &h->user_interface.timeline;
  if (ts->selected_player_track_index < 0 || ts->selected_player_track_index >= ts->player_track_count) {
    if (igBegin("Player Info", NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
      igTextDisabled("No player track selected.");
    }
    igEnd();
    return;
  }

  player_info_t *player_info = &ts->player_tracks[ts->selected_player_track_index].player_info;

  if (igBegin("Player Info", NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igInputText("Name", player_info->name, 16, 0, NULL, NULL);
    igInputText("Clan", player_info->clan, 12, 0, NULL, NULL);
    igInputInt("Skin Id", &player_info->skin, 1, 1, 0);
    player_info->skin = iclamp(player_info->skin, 0, MAX_SKINS - 1);
    igCheckbox("Use custom color", &player_info->use_custom_color);
    if (player_info->use_custom_color) {
      PackedHSLPicker("Color body", &player_info->color_body);
      PackedHSLPicker("Color feet", &player_info->color_feet);
    }
    if (igButton("Apply info to all players", (ImVec2){0}))
      for (int i = 0; i < h->user_interface.timeline.player_track_count; ++i)
        memcpy(&h->user_interface.timeline.player_tracks[i].player_info, player_info, sizeof(player_info_t));

    igSeparator();
    igText("Starting Configuration");
    starting_config_t *sc = &ts->player_tracks[ts->selected_player_track_index].starting_config;
    // TODO: somehow reset to default values when turning off, idk how to cleanly do that yet
    if (igCheckbox("Override Start", &sc->enabled)) {
      if (sc->enabled) {
        SWorldCore world = wc_empty();
        model_get_world_state_at_tick(ts, ts->current_tick, &world, false);
        if (ts->selected_player_track_index < world.m_NumCharacters) {
          SCharacterCore *chr = &world.m_pCharacters[ts->selected_player_track_index];
          sc->position[0] = vgetx(chr->m_Pos) - MAP_EXPAND * 32;
          sc->position[1] = vgety(chr->m_Pos) - MAP_EXPAND * 32;
          sc->velocity[0] = vgetx(chr->m_Vel);
          sc->velocity[1] = vgety(chr->m_Vel);
          sc->active_weapon = chr->m_ActiveWeapon;
          for (int i = 0; i < NUM_WEAPONS; ++i)
            sc->has_weapons[i] = chr->m_aWeaponGot[i];
        }
        wc_free(&world);
      }
    }
    if (sc->enabled) {
      static const char *weapon_names[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
      static const int weapon_count = sizeof(weapon_names) / sizeof(weapon_names[0]);

      igPushMultiItemsWidths(2, igCalcItemWidth());
      float pos[2] = {sc->position[0], sc->position[1]};
      if (igDragFloat("##UnitX", &pos[0], 1.0f, (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->height - (MAP_EXPAND - 1)) * 32, "%.0f", 0)) {
        sc->position[0] = fclamp(pos[0], (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->width - (MAP_EXPAND - 1)) * 32);
      }
      igPopItemWidth();
      igSameLine(0.0f, igGetStyle()->ItemInnerSpacing.x);
      if (igDragFloat("Position##UnitY", &pos[1], 1.0f, (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->height - (MAP_EXPAND - 1)) * 32, "%.0f", 0)) {
        sc->position[1] = fclamp(pos[1], (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->height - (MAP_EXPAND - 1)) * 32);
      }
      igPopItemWidth();

      igPushMultiItemsWidths(2, igCalcItemWidth());
      float block_pos[2] = {sc->position[0] / 32.0f, sc->position[1] / 32.0f};
      if (igDragFloat("##BlockX", &block_pos[0], 1.0f, (-MAP_EXPAND + 1), ts->ui->gfx_handler->map_data->width - (MAP_EXPAND - 1), "%.3f", 0)) {
        sc->position[0] = fclamp(block_pos[0] * 32.0f, (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->width - (MAP_EXPAND - 1)) * 32);
      }
      igPopItemWidth();
      igSameLine(0.0f, igGetStyle()->ItemInnerSpacing.x);
      if (igDragFloat("Position##BlockY", &block_pos[1], 1.0f, (-MAP_EXPAND + 1), ts->ui->gfx_handler->map_data->height - (MAP_EXPAND - 1), "%.3f", 0)) {
        sc->position[1] = fclamp(block_pos[1] * 32.0f, (-MAP_EXPAND + 1) * 32, (ts->ui->gfx_handler->map_data->width - (MAP_EXPAND - 1)) * 32);
      }
      igPopItemWidth();

      float vel[2] = {sc->velocity[0], sc->velocity[1]};
      igPushMultiItemsWidths(2, igCalcItemWidth());
      if (igDragFloat("##UnitVelX", &vel[0], 1.0f, -128, 128, "%.3f", 0)) {
        sc->velocity[0] = (float)((int)(fclamp(vel[0], -128, 128) * 256.f)) / 256.f;
      }
      igPopItemWidth();
      igSameLine(0.0f, igGetStyle()->ItemInnerSpacing.x);
      if (igDragFloat("Velocity##UnitVelY", &vel[1], 1.0f, -128, 128, "%.3f", 0)) {
        sc->velocity[1] = (float)((int)(fclamp(vel[1], -128, 128) * 256.f)) / 256.f;
      }
      igPopItemWidth();

      float bps_vel[2] = {sc->velocity[0] * (32.f / 50.f), sc->velocity[1] * (32.f / 50.f)};
      igPushMultiItemsWidths(2, igCalcItemWidth());
      if (igDragFloat("##BlockVelX", &bps_vel[0], 1.0f, -75, 75, "%.3f", 0)) {
        sc->velocity[0] = (float)((int)((fclamp(bps_vel[0], -75, 75) / (32.f / 50.f)) * 256.f)) / 256.f;
      }
      igPopItemWidth();
      igSameLine(0.0f, igGetStyle()->ItemInnerSpacing.x);
      if (igDragFloat("Velocity###BlockVelY", &bps_vel[1], 1.0f, -75, 75, "%.3f", 0)) {
        sc->velocity[1] = (float)((int)((fclamp(bps_vel[1], -75, 75) / (32.f / 50.f)) * 256.f)) / 256.f;
      }
      igPopItemWidth();

      igCombo_Str_arr("Active Weapon", &sc->active_weapon, weapon_names, weapon_count, 0);

      igText("Weapons:");
      for (int i = 0; i < NUM_WEAPONS; ++i) {
        if (i > 0 && i % 3 != 0) igSameLine(0, 5);
        igCheckbox(weapon_names[i], &sc->has_weapons[i]);
      }

      if (igButton("Take from Current State", (ImVec2){0})) {
        SWorldCore world = wc_empty();
        model_get_world_state_at_tick(ts, ts->current_tick, &world, false);
        if (ts->selected_player_track_index < world.m_NumCharacters) {
          SCharacterCore *chr = &world.m_pCharacters[ts->selected_player_track_index];
          sc->position[0] = vgetx(chr->m_Pos) - MAP_EXPAND * 32;
          sc->position[1] = vgety(chr->m_Pos) - MAP_EXPAND * 32;
          sc->velocity[0] = vgetx(chr->m_Vel);
          sc->velocity[1] = vgety(chr->m_Vel);
          sc->active_weapon = chr->m_ActiveWeapon;
          for (int i = 0; i < NUM_WEAPONS; ++i)
            sc->has_weapons[i] = chr->m_aWeaponGot[i];
        }
        wc_free(&world);
      }

      igSameLine(0, 10);
      if (ts->ui->selecting_override_pos) {
        igTextColored((ImVec4){0.2f, 1.0f, 0.2f, 1.0f}, "Click on the map to select position...");
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {

          ImGuiIO *io = igGetIO_Nil();
          float mx = io->MousePos.x - ts->ui->viewport_window_pos.x;
          float my = io->MousePos.y - ts->ui->viewport_window_pos.y;
          float wx, wy;
          screen_to_world(ts->ui->gfx_handler, mx, my, &wx, &wy);
          sc->position[0] = (wx - MAP_EXPAND) * 32;
          sc->position[1] = (wy - MAP_EXPAND) * 32;
          ts->ui->selecting_override_pos = false;
        }
      } else if (igButton("Select position", (ImVec2){0, 0}))
        ts->ui->selecting_override_pos = true;
      vec2 cpos = {sc->position[0] / 32.f + MAP_EXPAND, sc->position[1] / 32.f + MAP_EXPAND};
      renderer_submit_circle_filled(ts->ui->gfx_handler, 100.f, &cpos[0], 0.4f, (float[]){1.0f, 0.0f, 0.0f, 0.5f}, 32);

      if (igButton("Apply", (ImVec2){0, 0})) {
        model_apply_starting_config(ts, ts->selected_player_track_index);
      }
    }
  }
  igEnd();
}

void skin_manager_init(skin_manager_t *m) {
  if (!m) return;
  m->num_skins = 0;
  m->skins = NULL;
}

void skin_manager_free(skin_manager_t *m) {
  if (!m) return;
  if (m->skins) {
    for (int i = 0; i < m->num_skins; i++) {
      if (m->skins[i].data) free(m->skins[i].data);
    }
    free(m->skins);
  }
  m->skins = NULL;
  m->num_skins = 0;
}

int skin_manager_add(skin_manager_t *m, const skin_info_t *skin) {
  if (!m || !skin) return -1;
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
  if (!m || !h || index < 0 || index >= m->num_skins) return -1;

  // Unload from renderer and destroy preview
  renderer_unload_skin(h, m->skins[index].id);
  if (m->skins[index].preview_texture_res) {
    // The ImTextureID associated with this doesn't need manual cleanup,
    // ImGui's Vulkan backend handles it when the texture is destroyed.
    renderer_destroy_texture(h, m->skins[index].preview_texture_res);
  }
  if (m->skins[index].data) {
    free(m->skins[index].data);
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
