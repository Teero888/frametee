#include "save.h"
#include "fs.h"
#include <ddnet_physics/gamecore.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <user_interface/net_events.h>
#include <user_interface/timeline/timeline_model.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <system/skin/skin_fetch.h>

static const char *LOG_SOURCE = "SaveFile";

static bool write_map_data(FILE *f, physics_handler_t *ph);
static bool write_skin_data(FILE *f, ui_handler_t *ui);
static bool write_timeline_data(FILE *f, timeline_state_t *ts);

static bool read_and_load_map(FILE *f, ui_handler_t *ui, uint32_t map_data_size);
static bool read_and_load_skins(FILE *f, ui_handler_t *ui, uint32_t num_skins);
static bool read_and_load_timeline(FILE *f, ui_handler_t *ui, uint32_t version);

// Saving {{{
bool save_project(ui_handler_t *ui, const char *path) {
  double t0 = glfwGetTime();
  FILE *f = fs_open(path, "wb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open file for writing: '%s'", path);
    return false;
  }
  setvbuf(f, NULL, _IOFBF, 64 * 1024);

  // write a placeholder header, we'll come back and fill it in later
  tas_project_header_t header = {0};
  fseek(f, sizeof(tas_project_header_t), SEEK_SET);

  // write map data
  long map_start = ftell(f);
  if (!write_map_data(f, &ui->gfx_handler->physics_handler)) {
    fclose(f);
    return false;
  }
  header.map_data_size = ftell(f) - map_start;

  // write skin data
  if (!write_skin_data(f, ui)) {
    fclose(f);
    return false;
  }
  header.num_skins = ui->skin_manager.num_skins;

  // write timeline data
  long timeline_start = ftell(f);
  if (!write_timeline_data(f, &ui->timeline)) {
    fclose(f);
    return false;
  }
  header.timeline_data_size = ftell(f) - timeline_start;
  header.num_player_tracks = ui->timeline.player_track_count;

  // finalize header
  fseek(f, 0, SEEK_SET);
  memcpy(header.magic, TAS_PROJECT_FILE_MAGIC, 4);
  header.version = TAS_PROJECT_FILE_VERSION;
  snprintf(header.map_name, sizeof(header.map_name), "%.*s", (int)(sizeof(header.map_name) - 1), ui->loaded_map_name);
  fwrite(&header, sizeof(tas_project_header_t), 1, f);

  fclose(f);
  double elapsed_ms = (glfwGetTime() - t0) * 1000.0;
  log_info(LOG_SOURCE, "Project saved successfully to '%s' (%.2f ms)", path, elapsed_ms);

  strncpy(ui->current_project_path, path, sizeof(ui->current_project_path) - 1);
  ui->current_project_path[sizeof(ui->current_project_path) - 1] = '\0';
  ui->has_unsaved_changes = false;

  ui_add_recent_project(ui, path);
  return true;
}

static bool write_map_data(FILE *f, physics_handler_t *ph) {
  if (!ph->loaded || !ph->collision.m_MapData._map_file_data) {
    log_error(LOG_SOURCE, "No map data loaded to save.");
    return false;
  }
  // the map file is loaded into a contiguous block of memory. we can just write that.
  fwrite(ph->collision.m_MapData._map_file_data, ph->collision.m_MapData._map_file_size, 1, f);
  return true;
}

static bool write_skin_data(FILE *f, ui_handler_t *ui) {
  skin_manager_t *sm = &ui->skin_manager;

  for (int i = 0; i < sm->num_skins; i++) {
    skin_info_t *skin_info = &sm->skins[i];

    if (!skin_info->data || skin_info->data_size == 0) {
      log_warn(LOG_SOURCE, "Skipping skin %d ('%s'): No data found.", skin_info->id, skin_info->name);
      // Wait, if it has a name, we can still save it. We just don't save data anymore.
    }

    skin_file_header_t skin_header;
    skin_header.id = skin_info->id;
    snprintf(skin_header.name, sizeof(skin_header.name), "%s", skin_info->name);
    skin_header.texture_data_size = 0; // We no longer save raw png data

    fwrite(&skin_header, sizeof(skin_file_header_t), 1, f);
  }
  return true;
}

static bool write_timeline_data(FILE *f, timeline_state_t *ts) {
  // write player info for each track
  for (int i = 0; i < ts->player_track_count; i++) {
    fwrite(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f);
    fwrite(&ts->player_tracks[i].is_dummy, sizeof(bool), 1, f);
    fwrite(&ts->player_tracks[i].dummy_copy_flags, sizeof(int), 1, f);
    fwrite(&ts->player_tracks[i].starting_config, sizeof(starting_config_t), 1, f);
  }

  // write snippet data
  for (int i = 0; i < ts->player_track_count; i++) {
    player_track_t *track = &ts->player_tracks[i];
    fwrite(&track->snippet_count, sizeof(int), 1, f);
    for (int j = 0; j < track->snippet_count; j++) {
      input_snippet_t *snippet = &track->snippets[j];
      fwrite(&snippet->id, sizeof(int), 1, f);
      fwrite(&snippet->start_tick, sizeof(int), 1, f);
      fwrite(&snippet->end_tick, sizeof(int), 1, f);
      fwrite(&snippet->is_active, sizeof(bool), 1, f);
      fwrite(&snippet->layer, sizeof(int), 1, f);
      fwrite(&snippet->input_count, sizeof(int), 1, f);
      if (snippet->input_count > 0) fwrite(snippet->inputs, sizeof(SPlayerInput) * snippet->input_count, 1, f);
    }
  }

  // write net events (version 3+)
  fwrite(&ts->net_event_count, sizeof(int), 1, f);
  if (ts->net_event_count > 0) {
    fwrite(ts->net_events, sizeof(net_event_t), ts->net_event_count, f);
  }

  return true;
}
//}}}

// Loading {{{
bool load_project(ui_handler_t *ui, const char *path) {
  FILE *f = fs_open(path, "rb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open file for reading: '%s'", path);
    return false;
  }

  struct {
    char magic[4];
    uint32_t version;
    uint32_t map_data_size;
    uint32_t num_skins;
    uint32_t num_player_tracks;
    uint32_t timeline_data_size;
  } base_header;

  if (fread(&base_header, sizeof(base_header), 1, f) != 1) {
    log_error(LOG_SOURCE, "Failed to read project header from: '%s'", path);
    fclose(f);
    return false;
  }

  if (strncmp(base_header.magic, TAS_PROJECT_FILE_MAGIC, 4) != 0 || base_header.version > TAS_PROJECT_FILE_VERSION) {
    log_error(LOG_SOURCE, "Invalid or unsupported TAS project file: '%s'", path);
    fclose(f);
    return false;
  }

  tas_project_header_t header;
  memset(&header, 0, sizeof(header));
  memcpy(&header, &base_header, sizeof(base_header));

  if (base_header.version >= 5) {
    if (fread(header.map_name, sizeof(header.map_name), 1, f) == 1) {
      strncpy(ui->loaded_map_name, header.map_name, sizeof(ui->loaded_map_name) - 1);
      ui->loaded_map_name[sizeof(ui->loaded_map_name) - 1] = '\0';
    }
  } else {
    if (ui->loaded_map_name[0] == '\0') {
      strncpy(ui->loaded_map_name, "unnamed_map", sizeof(ui->loaded_map_name) - 1);
    }
  }

  // clean up existing state before loading
  timeline_cleanup(&ui->timeline);
  skin_manager_free(&ui->skin_manager, ui->gfx_handler);
  // mark all skins as unloaded directly
  memset(ui->gfx_handler->renderer.skin_manager.layer_used + 3, 0,
         MAX_SKINS - 3); // start at id 3 so we don't delete the default,ninja and spec skin
  timeline_init(ui);
  skin_manager_init(&ui->skin_manager);
  ui->timeline.ui = ui;

  if (!read_and_load_map(f, ui, header.map_data_size)) {
    fclose(f);
    return false;
  }

  if (!read_and_load_skins(f, ui, header.num_skins)) {
    fclose(f);
    return false;
  }

  // set number of player tracks before loading timeline data
  ui->timeline.player_track_count = header.num_player_tracks;
  if (header.num_player_tracks > 0) {
    ui->timeline.player_tracks = calloc(header.num_player_tracks, sizeof(player_track_t));
  } else {
    ui->timeline.player_tracks = NULL;
  }

  if (!read_and_load_timeline(f, ui, header.version)) {
    fclose(f);
    return false;
  }

  // Apply starting configurations
  for (int i = 0; i < ui->timeline.player_track_count; i++) {
    if (ui->timeline.player_tracks[i].starting_config.enabled) {
      model_apply_starting_config(&ui->timeline, i);
    }
  }

  fclose(f);
  log_info(LOG_SOURCE, "Project loaded successfully from '%s'", path);
  strncpy(ui->current_project_path, path, sizeof(ui->current_project_path) - 1);
  ui->current_project_path[sizeof(ui->current_project_path) - 1] = '\0';
  ui->has_unsaved_changes = false;

  ui_add_recent_project(ui, path);

  model_recalc_physics(&ui->timeline, 0); // recalculate physics from the start
  return true;
}

static bool read_and_load_map(FILE *f, ui_handler_t *ui, uint32_t map_data_size) {
  unsigned char *map_buffer = malloc(map_data_size);
  if (!map_buffer) {
    log_error(LOG_SOURCE, "Failed to allocate memory for map data.");
    return false;
  }
  if (fread(map_buffer, map_data_size, 1, f) != 1) {
    log_error(LOG_SOURCE, "Failed to read map data from project file.");
    free(map_buffer);
    return false;
  }

  on_map_load_mem(ui->gfx_handler, map_buffer, map_data_size);

  return true;
}

static bool read_and_load_skins(FILE *f, ui_handler_t *ui, uint32_t num_skins) {
  for (uint32_t i = 0; i < num_skins; i++) {
    skin_file_header_t skin_header;
    if (fread(&skin_header, sizeof(skin_file_header_t), 1, f) != 1) {
      log_error(LOG_SOURCE, "Failed to read skin header %u.", i);
      return false;
    }

    skin_info_t info = {0};
    int loaded_id = -1;

    if (skin_header.texture_data_size > 0) {
      unsigned char *texture_data = malloc(skin_header.texture_data_size);
      if (texture_data) {
        if (fread(texture_data, skin_header.texture_data_size, 1, f) == 1) {
          loaded_id = renderer_load_skin_from_memory(ui->gfx_handler, texture_data, skin_header.texture_data_size, &info.preview_texture_res);
          info.data = texture_data;
          info.data_size = skin_header.texture_data_size;
        } else {
          free(texture_data);
        }
      }
    } else {
      // New format: texture_data_size is 0, we must fetch the skin by name
      char skin_path[512] = {0};
      if (fetch_skin(skin_header.name, skin_path, sizeof(skin_path))) {
        loaded_id = renderer_load_skin_from_file(ui->gfx_handler, skin_path, &info.preview_texture_res);
        if (loaded_id >= 0) {
          snprintf(info.path, sizeof(info.path), "%s", skin_path);
          // Load data into memory for skin_manager
          FILE *f_skin = fs_open(skin_path, "rb");
          if (f_skin) {
            fseek(f_skin, 0, SEEK_END);
            long sz = ftell(f_skin);
            fseek(f_skin, 0, SEEK_SET);
            info.data = malloc(sz);
            if (info.data) {
              fread(info.data, sz, 1, f_skin);
              info.data_size = sz;
            }
            fclose(f_skin);
          }
        }
      } else {
        log_warn(LOG_SOURCE, "Could not fetch or load skin: %s", skin_header.name);
      }
    }

    if (loaded_id >= 0) {
      info.id = loaded_id;
      snprintf(info.name, sizeof(info.name), "%s", skin_header.name);
      if (info.preview_texture_res) {
        info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
            info.preview_texture_res->sampler, info.preview_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
      }
      skin_manager_add(&ui->skin_manager, &info);
    }
  }
  return true;
}

static bool read_and_load_timeline(FILE *f, ui_handler_t *ui, uint32_t version) {
  timeline_state_t *ts = &ui->timeline;

  // read player info
  for (int i = 0; i < ts->player_track_count; i++) {
    if (fread(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f) != 1) return false;
    if (fread(&ts->player_tracks[i].is_dummy, sizeof(bool), 1, f) != 1) return false;
    if (fread(&ts->player_tracks[i].dummy_copy_flags, sizeof(int), 1, f) != 1) return false;
    if (version >= 4) {
      if (fread(&ts->player_tracks[i].starting_config, sizeof(starting_config_t), 1, f) != 1) return false;
    }
    // add characters to the physics world
    if (!wc_add_character(&ui->gfx_handler->physics_handler.world, 1)) {
      log_error(LOG_SOURCE, "Failed to add character '%s'", ts->player_tracks[i].player_info.name);
    }
  }

  // read snippets
  int max_id = 0;
  for (int i = 0; i < ts->player_track_count; i++) {
    player_track_t *track = &ts->player_tracks[i];
    if (fread(&track->snippet_count, sizeof(int), 1, f) != 1) return false;

    track->snippets = calloc(track->snippet_count, sizeof(input_snippet_t));
    for (int j = 0; j < track->snippet_count; j++) {
      input_snippet_t *snippet = &track->snippets[j];
      if (fread(&snippet->id, sizeof(int), 1, f) != 1) return false;
      if (fread(&snippet->start_tick, sizeof(int), 1, f) != 1) return false;
      if (fread(&snippet->end_tick, sizeof(int), 1, f) != 1) return false;
      if (fread(&snippet->is_active, sizeof(bool), 1, f) != 1) return false;
      if (fread(&snippet->layer, sizeof(int), 1, f) != 1) return false;
      if (fread(&snippet->input_count, sizeof(int), 1, f) != 1) return false;

      if (snippet->id > max_id) {
        max_id = snippet->id;
      }

      if (snippet->input_count > 0) {
        snippet->inputs = malloc(sizeof(SPlayerInput) * snippet->input_count);
        if (!snippet->inputs || fread(snippet->inputs, sizeof(SPlayerInput) * snippet->input_count, 1, f) != 1) return false;
      } else {
        snippet->inputs = NULL;
      }
    }
  }

  ts->next_snippet_id = max_id + 1;

  if (version >= 3) {
    int count = 0;
    if (fread(&count, sizeof(int), 1, f) != 1) return false;
    for (int i = 0; i < count; ++i) {
      net_event_t ev;
      if (fread(&ev, sizeof(net_event_t), 1, f) != 1) return false;
      net_events_add(ts, ev);
    }
  }

  return true;
}
//}}}
