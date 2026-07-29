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
#include <system/skin/skin_fetch.h>
#include <system/compat_threads.h>

#include <symbols.h>
#include <math.h>

static void draw_spinning_icon(ImVec2 center, const char* icon_text) {
    ImVec2 text_size = igCalcTextSize(icon_text, NULL, false, -1.0f);
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
  char skin_name[64];
  char fetched_path[512];
  volatile bool done;
  volatile bool success;
  player_info_t* target;
} skin_fetch_task_t;

#define MAX_TASKS 32
static skin_fetch_task_t g_fetch_tasks[MAX_TASKS];
static pthread_t g_fetch_threads[MAX_TASKS];

static void* fetch_skin_thread(void* arg) {
  skin_fetch_task_t* task = (skin_fetch_task_t*)arg;
  bool ok = fetch_skin(task->skin_name, task->fetched_path, sizeof(task->fetched_path));
  task->success = ok;
  task->done = true;
  return NULL;
}

static void start_skin_fetch(player_info_t* info) {
  info->fetching_skin = true;
  info->fetching_anim_time = 0.0f;
  
  for (int i = 0; i < MAX_TASKS; ++i) {
    if (!g_fetch_tasks[i].done && g_fetch_tasks[i].target != NULL) continue;
    
    // Found empty slot or completed slot
    if (g_fetch_tasks[i].target != NULL) {
      pthread_join(g_fetch_threads[i], NULL); // cleanup old thread
    }
    
    g_fetch_tasks[i].target = info;
    g_fetch_tasks[i].done = false;
    g_fetch_tasks[i].success = false;
    snprintf(g_fetch_tasks[i].skin_name, sizeof(g_fetch_tasks[i].skin_name), "%s", info->skin_name);
    
    pthread_create(&g_fetch_threads[i], NULL, fetch_skin_thread, &g_fetch_tasks[i]);
    break;
  }
}

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

    if (h->user_interface.finish_tick > 0) {
      float time = h->user_interface.race_time;
      int m = (int)time / 60;
      float s = fmodf(time, 60.0f);
      igText("Finish Time: %02d:%06.3f", m, s);
    }

    bool name_changed = igInputText("Skin Name", player_info->skin_name, sizeof(player_info->skin_name), ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    if (igIsItemDeactivatedAfterEdit() || name_changed) {
      start_skin_fetch(player_info);
    }
    
    if (player_info->fetching_skin) {
      igSameLine(0, 10.0f);
      igText("Fetching");
      igSameLine(0, 10.0f);
      ImVec2 pos = igGetCursorScreenPos();
      draw_spinning_icon(pos, ICON_FA_ROTATE);
    }
    
    // Check completed tasks
    for (int i = 0; i < MAX_TASKS; ++i) {
      if (g_fetch_tasks[i].target == player_info && g_fetch_tasks[i].done) {
        if (g_fetch_tasks[i].success) {
          // Load it into Vulkan!
          texture_t* unused_preview = NULL;
          int real_id = renderer_load_skin_from_file(h, g_fetch_tasks[i].fetched_path, &unused_preview);
          if (real_id >= 0) {
            skin_info_t info = {0};
            info.id = real_id;
            info.preview_texture_res = unused_preview;
            if (unused_preview) {
              info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                  unused_preview->sampler, unused_preview->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            }
            snprintf(info.name, sizeof(info.name), "%.*s", (int)(sizeof(info.name) - 1), g_fetch_tasks[i].skin_name);
            snprintf(info.path, sizeof(info.path), "%.*s", (int)(sizeof(info.path) - 1), g_fetch_tasks[i].fetched_path);
            skin_manager_add(&h->user_interface.skin_manager, &info);
            player_info->skin = info.id;
          }
        }
        player_info->fetching_skin = false;
        g_fetch_tasks[i].target = NULL; // free slot
        pthread_join(g_fetch_threads[i], NULL);
      }
    }
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

void skin_manager_free(skin_manager_t *m, gfx_handler_t *h) {
  if (!m) return;
  if (m->skins) {
    for (int i = 0; i < m->num_skins; i++) {
      if (m->skins[i].preview_texture) {
        destroy_imgui_texture_ref(&m->skins[i].preview_texture);
      }
      if (h && m->skins[i].preview_texture_res) {
        renderer_destroy_texture(h, m->skins[i].preview_texture_res);
        m->skins[i].preview_texture_res = NULL;
      }
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
  if (m->skins[index].preview_texture) {
    destroy_imgui_texture_ref(&m->skins[index].preview_texture);
  }
  if (m->skins[index].preview_texture_res) {
    renderer_destroy_texture(h, m->skins[index].preview_texture_res);
    m->skins[index].preview_texture_res = NULL;
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
