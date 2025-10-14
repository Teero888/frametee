#include "save.h"
#include "../logger/logger.h"
#include "../renderer/graphics_backend.h"
#include "../renderer/renderer.h"
#include "../user_interface/timeline/timeline_model.h"
#include "gamecore.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *LOG_SOURCE = "SaveFile";

static bool write_map_data(FILE *f, physics_handler_t *ph);
static bool write_skin_data(FILE *f, ui_handler_t *ui);
static bool write_timeline_data(FILE *f, timeline_state_t *ts);

static bool read_and_load_map(FILE *f, ui_handler_t *ui, uint32_t map_data_size);
static bool read_and_load_skins(FILE *f, ui_handler_t *ui, uint32_t num_skins);
static bool read_and_load_timeline(FILE *f, ui_handler_t *ui);

// Saving {{{
bool save_project(ui_handler_t *ui, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open file for writing: '%s'", path);
    return false;
  }

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
  fwrite(&header, sizeof(tas_project_header_t), 1, f);

  fclose(f);
  log_info(LOG_SOURCE, "Project saved successfully to '%s'", path);
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
  renderer_state_t *renderer = &ui->gfx_handler->renderer;
  skin_manager_t *sm = &ui->skin_manager;

  for (int i = 0; i < sm->num_skins; i++) {
    skin_info_t *skin_info = &sm->skins[i];
    texture_t *texture = &renderer->textures[skin_info->id];

    if (!texture || !texture->active || strlen(texture->path) == 0) {
      log_warn(LOG_SOURCE, "Skipping invalid skin with id %d", skin_info->id);
      continue;
    }

    FILE *skin_file = fopen(texture->path, "rb");
    if (!skin_file) {
      log_warn(LOG_SOURCE, "Could not open skin file for reading: '%s'", texture->path);
      continue;
    }

    fseek(skin_file, 0, SEEK_END);
    long texture_size = ftell(skin_file);
    fseek(skin_file, 0, SEEK_SET);
    unsigned char *texture_data = malloc(texture_size);
    if (!texture_data) {
      fclose(skin_file);
      continue;
    }
    fread(texture_data, texture_size, 1, skin_file);
    fclose(skin_file);

    skin_file_header_t skin_header;
    skin_header.id = skin_info->id;
    strncpy(skin_header.name, skin_info->name, sizeof(skin_header.name) - 1);
    skin_header.texture_data_size = texture_size;

    fwrite(&skin_header, sizeof(skin_file_header_t), 1, f);
    fwrite(texture_data, texture_size, 1, f);

    free(texture_data);
  }
  return true;
}

static bool write_timeline_data(FILE *f, timeline_state_t *ts) {
  // write player info for each track
  for (int i = 0; i < ts->player_track_count; i++) {
    fwrite(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f);
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
  return true;
}
//}}}

// Loading {{{
bool load_project(ui_handler_t *ui, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open file for reading: '%s'", path);
    return false;
  }

  tas_project_header_t header;
  if (fread(&header, sizeof(tas_project_header_t), 1, f) != 1) {
    log_error(LOG_SOURCE, "Failed to read project header from: '%s'", path);
    fclose(f);
    return false;
  }

  if (strncmp(header.magic, TAS_PROJECT_FILE_MAGIC, 4) != 0 || header.version != TAS_PROJECT_FILE_VERSION) {
    log_error(LOG_SOURCE, "Invalid or unsupported TAS project file: '%s'", path);
    fclose(f);
    return false;
  }

  // clean up existing state before loading
  timeline_cleanup(&ui->timeline);
  skin_manager_free(&ui->skin_manager);
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

  if (!read_and_load_timeline(f, ui)) {
    fclose(f);
    return false;
  }

  fclose(f);
  log_info(LOG_SOURCE, "Project loaded successfully from '%s'", path);

  wc_copy_world(&ui->timeline.previous_world, &ui->gfx_handler->physics_handler.world);
  wc_copy_world(&ui->timeline.vec.data[0], &ui->gfx_handler->physics_handler.world);
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

    unsigned char *texture_data = malloc(skin_header.texture_data_size);
    if (!texture_data) {
      log_error(LOG_SOURCE, "Failed to allocate memory for skin texture %u.", i);
      return false;
    }

    if (fread(texture_data, skin_header.texture_data_size, 1, f) != 1) {
      log_error(LOG_SOURCE, "Failed to read skin texture data for skin %u.", i);
      free(texture_data);
      return false;
    }

    skin_info_t info = {};
    int loaded_id = renderer_load_skin_from_memory(ui->gfx_handler, texture_data, skin_header.texture_data_size, &info.preview_texture_res);
    free(texture_data);
    if (loaded_id >= 0) {
      info.id = loaded_id;
      strncpy(info.name, skin_header.name, sizeof(info.name) - 1);
      if (info.preview_texture_res) {
        info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
            info.preview_texture_res->sampler, info.preview_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
      }
      skin_manager_add(&ui->skin_manager, &info);
    }
  }
  return true;
}

static bool read_and_load_timeline(FILE *f, ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;

  // read player info
  for (int i = 0; i < ts->player_track_count; i++) {
    if (fread(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f) != 1) return false;
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
  return true;
}
//}}}
