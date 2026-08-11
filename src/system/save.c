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
static bool read_and_load_timeline(FILE *f, ui_handler_t *ui, uint32_t version, uint32_t num_groups);

typedef struct {
  char name[MAX_TIMELINE_GROUP_NAME];
  float color[4];
  bool visible;
  bool demo_export_enabled;
  int start_offset;
} group_file_header_t;

// net_event_t gained group ownership in project version 8. Keep the old byte layout explicit so
// projects from versions 3..7 remain readable on every supported platform.
typedef struct {
  int tick;
  net_event_type_t type;
  int team;
  int client_id;
  char message[256];
  int killer;
  int victim;
  int weapon;
  int mode_special;
  int sound_id;
  int emoticon;
  int vote_timeout;
  char reason[256];
  int vote_yes;
  int vote_no;
  int vote_pass;
  int vote_total;
  int time;
  int check;
  int finish;
  int server_time_best;
  int player_time_best;
} legacy_net_event_t;

static void legacy_event_to_current(const legacy_net_event_t *legacy, net_event_t *ev);

typedef struct {
  group_file_header_t *groups;
  int group_count;
  player_track_t *tracks;
  int track_count;
  net_event_t *events;
  int event_count;
} imported_timeline_t;

static bool read_exact(FILE *f, void *data, size_t size) { return size == 0 || fread(data, size, 1, f) == 1; }

static void free_imported_timeline(imported_timeline_t *imported) {
  if (!imported) return;
  for (int i = 0; i < imported->track_count; ++i) {
    player_track_t *track = &imported->tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) free(track->snippets[j].inputs);
    free(track->snippets);
  }
  free(imported->tracks);
  free(imported->groups);
  free(imported->events);
  memset(imported, 0, sizeof(*imported));
}

static void legacy_event_to_current(const legacy_net_event_t *legacy, net_event_t *ev) {
  memset(ev, 0, sizeof(*ev));
  ev->tick = legacy->tick;
  ev->type = legacy->type;
  ev->team = legacy->team;
  ev->client_id = legacy->client_id;
  memcpy(ev->message, legacy->message, sizeof(ev->message));
  ev->killer = legacy->killer;
  ev->victim = legacy->victim;
  ev->weapon = legacy->weapon;
  ev->mode_special = legacy->mode_special;
  ev->sound_id = legacy->sound_id;
  ev->emoticon = legacy->emoticon;
  ev->vote_timeout = legacy->vote_timeout;
  memcpy(ev->reason, legacy->reason, sizeof(ev->reason));
  ev->vote_yes = legacy->vote_yes;
  ev->vote_no = legacy->vote_no;
  ev->vote_pass = legacy->vote_pass;
  ev->vote_total = legacy->vote_total;
  ev->time = legacy->time;
  ev->check = legacy->check;
  ev->finish = legacy->finish;
  ev->server_time_best = legacy->server_time_best;
  ev->player_time_best = legacy->player_time_best;
}

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
  header.num_groups = ui->timeline.group_count;

  // finalize header
  fseek(f, 0, SEEK_SET);
  memcpy(header.magic, TAS_PROJECT_FILE_MAGIC, 4);
  header.version = TAS_PROJECT_FILE_VERSION;
  snprintf(header.map_name, sizeof(header.map_name), "%.*s", (int)(sizeof(header.map_name) - 1), ui->loaded_map_name);
  header.game_mode = (uint32_t)ui->game_mode;
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
  for (int i = 0; i < ts->group_count; ++i) {
    timeline_group_t *group = ts->groups[i];
    group_file_header_t file_group = {0};
    memcpy(file_group.name, group->name, sizeof(file_group.name));
    memcpy(file_group.color, group->color, sizeof(file_group.color));
    file_group.visible = group->visible;
    file_group.demo_export_enabled = group->demo_export_enabled;
    file_group.start_offset = group->start_offset;
    if (fwrite(&file_group, sizeof(file_group), 1, f) != 1) return false;
  }

  // write player info for each track
  for (int i = 0; i < ts->player_track_count; i++) {
    fwrite(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f);
    fwrite(&ts->player_tracks[i].is_dummy, sizeof(bool), 1, f);
    fwrite(&ts->player_tracks[i].dummy_copy_flags, sizeof(int), 1, f);
    fwrite(&ts->player_tracks[i].starting_config, sizeof(starting_config_t), 1, f);
    fwrite(&ts->player_tracks[i].group_index, sizeof(int), 1, f);
    fwrite(ts->player_tracks[i].name, sizeof(ts->player_tracks[i].name), 1, f);
    fwrite(&ts->player_tracks[i].demo_export_enabled, sizeof(bool), 1, f);
    fwrite(&ts->player_tracks[i].demo_ping, sizeof(int), 1, f);
    fwrite(&ts->player_tracks[i].demo_player_flags, sizeof(int), 1, f);
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
      // The trimmed-away source travels with the project (version 7+), otherwise widening a snippet
      // after a reload would come back blank.
      fwrite(&snippet->source_offset, sizeof(int), 1, f);
      fwrite(&snippet->source_count, sizeof(int), 1, f);
      if (snippet->source_count > 0) fwrite(snippet->inputs, sizeof(SPlayerInput) * snippet->source_count, 1, f);
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
    if (fread(header.map_name, sizeof(header.map_name), 1, f) != 1) {
      log_error(LOG_SOURCE, "Failed to read project map name from: '%s'", path);
      fclose(f);
      return false;
    }
    strncpy(ui->loaded_map_name, header.map_name, sizeof(ui->loaded_map_name) - 1);
    ui->loaded_map_name[sizeof(ui->loaded_map_name) - 1] = '\0';
  } else {
    if (ui->loaded_map_name[0] == '\0') {
      strncpy(ui->loaded_map_name, "unnamed_map", sizeof(ui->loaded_map_name) - 1);
    }
  }

  if (base_header.version >= 6) {
    if (fread(&header.game_mode, sizeof(header.game_mode), 1, f) != 1 || header.game_mode >= NUM_GAME_MODES) {
      log_error(LOG_SOURCE, "Invalid or missing project game mode in: '%s'", path);
      fclose(f);
      return false;
    }
    ui->game_mode = (EGameMode)header.game_mode;
  }
  if (base_header.version >= 8) {
    if (fread(&header.num_groups, sizeof(header.num_groups), 1, f) != 1 || header.num_groups == 0 || header.num_groups > 1024) {
      log_error(LOG_SOURCE, "Invalid project group count in: '%s'", path);
      fclose(f);
      return false;
    }
  } else {
    header.num_groups = 1;
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

  if (!read_and_load_timeline(f, ui, header.version, header.num_groups)) {
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

static bool read_and_load_timeline(FILE *f, ui_handler_t *ui, uint32_t version, uint32_t num_groups) {
  timeline_state_t *ts = &ui->timeline;

  if (version >= 8) {
    for (uint32_t i = 0; i < num_groups; ++i) {
      group_file_header_t file_group;
      if (fread(&file_group, sizeof(file_group), 1, f) != 1) return false;
      timeline_group_t *group = i == 0 ? ts->groups[0] : model_add_group(ts, file_group.name);
      if (!group) return false;
      memcpy(group->name, file_group.name, sizeof(group->name));
      group->name[sizeof(group->name) - 1] = '\0';
      memcpy(group->color, file_group.color, sizeof(group->color));
      group->visible = file_group.visible;
      group->demo_export_enabled = file_group.demo_export_enabled;
      group->start_offset = i == 0 ? 0 : file_group.start_offset;
    }
  }

  // read player info
  for (int i = 0; i < ts->player_track_count; i++) {
    if (fread(&ts->player_tracks[i].player_info, sizeof(player_info_t), 1, f) != 1) return false;
    if (fread(&ts->player_tracks[i].is_dummy, sizeof(bool), 1, f) != 1) return false;
    if (fread(&ts->player_tracks[i].dummy_copy_flags, sizeof(int), 1, f) != 1) return false;
    if (version >= 4) {
      if (fread(&ts->player_tracks[i].starting_config, sizeof(starting_config_t), 1, f) != 1) return false;
    }
    player_track_t *track = &ts->player_tracks[i];
    track->group_index = 0;
    track->demo_export_enabled = true;
    track->demo_ping = 0;
    snprintf(track->name, sizeof(track->name), "Track %d", i + 1);
    if (version >= 8) {
      if (fread(&track->group_index, sizeof(int), 1, f) != 1 || track->group_index < 0 || track->group_index >= ts->group_count) return false;
      if (fread(track->name, sizeof(track->name), 1, f) != 1) return false;
      track->name[sizeof(track->name) - 1] = '\0';
      if (fread(&track->demo_export_enabled, sizeof(bool), 1, f) != 1) return false;
      if (fread(&track->demo_ping, sizeof(int), 1, f) != 1) return false;
      if (fread(&track->demo_player_flags, sizeof(int), 1, f) != 1) return false;
    }
    // add characters to the physics world
    if (!wc_add_character(&ts->groups[track->group_index]->initial_world, 1)) {
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

      if (version >= 7) {
        if (fread(&snippet->source_offset, sizeof(int), 1, f) != 1) return false;
        if (fread(&snippet->source_count, sizeof(int), 1, f) != 1) return false;
      } else {
        // Older projects stored only the visible window, which is exactly a snippet with no
        // trimmed-away source.
        snippet->source_offset = 0;
        snippet->source_count = snippet->input_count;
      }

      if (snippet->source_count > 0) {
        snippet->inputs = malloc(sizeof(SPlayerInput) * snippet->source_count);
        if (!snippet->inputs || fread(snippet->inputs, sizeof(SPlayerInput) * snippet->source_count, 1, f) != 1) return false;
      } else {
        snippet->inputs = NULL;
      }
      model_snippet_normalize(snippet);
    }
  }

  ts->next_snippet_id = max_id + 1;

  if (version >= 3) {
    int count = 0;
    if (fread(&count, sizeof(int), 1, f) != 1) return false;
    for (int i = 0; i < count; ++i) {
      net_event_t ev = {0};
      if (version >= 8) {
        if (fread(&ev, sizeof(net_event_t), 1, f) != 1) return false;
      } else {
        legacy_net_event_t legacy;
        if (fread(&legacy, sizeof(legacy), 1, f) != 1) return false;
        legacy_event_to_current(&legacy, &ev);
      }
      net_events_add(ts, ev);
    }
  }

  return true;
}
//}}}

static const char *project_file_stem(const char *path, char *buffer, size_t buffer_size) {
  const char *name = path;
  for (const char *p = path; *p; ++p)
    if (*p == '/' || *p == '\\') name = p + 1;
  snprintf(buffer, buffer_size, "%s", name[0] ? name : "Imported group");
  char *dot = strrchr(buffer, '.');
  if (dot && dot != buffer) *dot = '\0';
  return buffer;
}

static int find_or_load_imported_skin(ui_handler_t *ui, const char *name) {
  if (!name || !name[0]) return 0;
  for (int i = 0; i < ui->skin_manager.num_skins; ++i)
    if (strcmp(ui->skin_manager.skins[i].name, name) == 0) return ui->skin_manager.skins[i].id;

  char skin_path[512] = {0};
  if (!fetch_skin(name, skin_path, sizeof(skin_path))) {
    log_warn(LOG_SOURCE, "Could not fetch imported skin '%s'; using the default skin", name);
    return 0;
  }

  skin_info_t info = {0};
  int loaded_id = renderer_load_skin_from_file(ui->gfx_handler, skin_path, &info.preview_texture_res);
  if (loaded_id < 0) return 0;
  info.id = loaded_id;
  snprintf(info.name, sizeof(info.name), "%.*s", (int)sizeof(info.name) - 1, name);
  snprintf(info.path, sizeof(info.path), "%s", skin_path);
  if (info.preview_texture_res) {
    info.preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
        info.preview_texture_res->sampler, info.preview_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  }
  if (skin_manager_add(&ui->skin_manager, &info) != 0) {
    if (info.preview_texture) destroy_imgui_texture_ref(&info.preview_texture);
    if (info.preview_texture_res) renderer_destroy_texture(ui->gfx_handler, info.preview_texture_res);
    renderer_unload_skin(ui->gfx_handler, loaded_id);
    return 0;
  }
  return loaded_id;
}

bool import_project_as_group(ui_handler_t *ui, const char *path) {
  if (!ui || !path || !ui->gfx_handler || !ui->gfx_handler->physics_handler.loaded) return false;

  FILE *f = fs_open(path, "rb");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open project for group import: '%s'", path);
    return false;
  }

  struct {
    char magic[4];
    uint32_t version;
    uint32_t map_data_size;
    uint32_t num_skins;
    uint32_t num_player_tracks;
    uint32_t timeline_data_size;
  } header = {0};
  char source_map_name[64] = {0};
  uint32_t source_game_mode = (uint32_t)ui->game_mode;
  uint32_t source_group_count = 1;
  imported_timeline_t imported = {0};
  bool ok = false;
  int rollback_group_count = -1;

  if (!read_exact(f, &header, sizeof(header)) || memcmp(header.magic, TAS_PROJECT_FILE_MAGIC, 4) != 0 || header.version == 0 ||
      header.version > TAS_PROJECT_FILE_VERSION || header.num_player_tracks > 10000) {
    log_error(LOG_SOURCE, "Invalid or unsupported project for group import: '%s'", path);
    goto done;
  }
  if (header.version >= 5 && !read_exact(f, source_map_name, sizeof(source_map_name))) goto malformed;
  if (header.version >= 6 && (!read_exact(f, &source_game_mode, sizeof(source_game_mode)) || source_game_mode >= NUM_GAME_MODES)) goto malformed;
  if (header.version >= 8 &&
      (!read_exact(f, &source_group_count, sizeof(source_group_count)) || source_group_count == 0 || source_group_count > 1024))
    goto malformed;
  if (header.version >= 6 && source_game_mode != (uint32_t)ui->game_mode) {
    log_error(LOG_SOURCE, "Cannot import '%s': its game mode differs from the current project", path);
    goto done;
  }

  physics_handler_t *ph = &ui->gfx_handler->physics_handler;
  const size_t current_map_size = ph->collision.m_MapData._map_file_size;
  if (header.map_data_size != current_map_size) {
    log_error(LOG_SOURCE, "Cannot import '%s': it uses a different map", path);
    goto done;
  }
  unsigned char *map_data = malloc(header.map_data_size);
  if (!map_data || !read_exact(f, map_data, header.map_data_size)) {
    free(map_data);
    goto malformed;
  }
  bool same_map = memcmp(map_data, ph->collision.m_MapData._map_file_data, header.map_data_size) == 0;
  free(map_data);
  if (!same_map) {
    log_error(LOG_SOURCE, "Cannot import '%s': it uses a different map", path);
    goto done;
  }

  for (uint32_t i = 0; i < header.num_skins; ++i) {
    skin_file_header_t skin_header;
    if (!read_exact(f, &skin_header, sizeof(skin_header)) ||
        (skin_header.texture_data_size > 0 && fseek(f, (long)skin_header.texture_data_size, SEEK_CUR) != 0))
      goto malformed;
  }

  imported.group_count = (int)source_group_count;
  imported.groups = calloc(source_group_count, sizeof(*imported.groups));
  imported.track_count = (int)header.num_player_tracks;
  imported.tracks = calloc(header.num_player_tracks, sizeof(*imported.tracks));
  if (!imported.groups || (header.num_player_tracks > 0 && !imported.tracks)) goto malformed;

  char stem[MAX_TIMELINE_GROUP_NAME];
  project_file_stem(path, stem, sizeof(stem));
  for (int i = 0; i < imported.group_count; ++i) {
    group_file_header_t *group = &imported.groups[i];
    group->visible = true;
    group->demo_export_enabled = true;
    if (header.version >= 8) {
      if (!read_exact(f, group, sizeof(*group))) goto malformed;
      group->name[sizeof(group->name) - 1] = '\0';
      if (!group->name[0]) snprintf(group->name, sizeof(group->name), "%s %d", stem, i + 1);
    } else {
      snprintf(group->name, sizeof(group->name), "%s", stem);
    }
  }

  for (int i = 0; i < imported.track_count; ++i) {
    player_track_t *track = &imported.tracks[i];
    if (!read_exact(f, &track->player_info, sizeof(track->player_info)) || !read_exact(f, &track->is_dummy, sizeof(track->is_dummy)) ||
        !read_exact(f, &track->dummy_copy_flags, sizeof(track->dummy_copy_flags)))
      goto malformed;
    if (header.version >= 4 && !read_exact(f, &track->starting_config, sizeof(track->starting_config))) goto malformed;
    track->group_index = 0;
    track->demo_export_enabled = true;
    snprintf(track->name, sizeof(track->name), "Track %d", i + 1);
    if (header.version >= 8) {
      if (!read_exact(f, &track->group_index, sizeof(track->group_index)) || track->group_index < 0 ||
          track->group_index >= imported.group_count || !read_exact(f, track->name, sizeof(track->name)) ||
          !read_exact(f, &track->demo_export_enabled, sizeof(track->demo_export_enabled)) ||
          !read_exact(f, &track->demo_ping, sizeof(track->demo_ping)) ||
          !read_exact(f, &track->demo_player_flags, sizeof(track->demo_player_flags)))
        goto malformed;
      track->name[sizeof(track->name) - 1] = '\0';
    }
  }

  for (int i = 0; i < imported.track_count; ++i) {
    player_track_t *track = &imported.tracks[i];
    if (!read_exact(f, &track->snippet_count, sizeof(track->snippet_count)) || track->snippet_count < 0 || track->snippet_count > 100000)
      goto malformed;
    if (track->snippet_count > 0) {
      track->snippets = calloc((size_t)track->snippet_count, sizeof(*track->snippets));
      if (!track->snippets) goto malformed;
    }
    track->snippet_capacity = track->snippet_count;
    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *snippet = &track->snippets[j];
      if (!read_exact(f, &snippet->id, sizeof(snippet->id)) || !read_exact(f, &snippet->start_tick, sizeof(snippet->start_tick)) ||
          !read_exact(f, &snippet->end_tick, sizeof(snippet->end_tick)) || !read_exact(f, &snippet->is_active, sizeof(snippet->is_active)) ||
          !read_exact(f, &snippet->layer, sizeof(snippet->layer)) || !read_exact(f, &snippet->input_count, sizeof(snippet->input_count)))
        goto malformed;
      if (header.version >= 7) {
        if (!read_exact(f, &snippet->source_offset, sizeof(snippet->source_offset)) ||
            !read_exact(f, &snippet->source_count, sizeof(snippet->source_count)))
          goto malformed;
      } else {
        snippet->source_offset = 0;
        snippet->source_count = snippet->input_count;
      }
      if (snippet->input_count < 0 || snippet->source_count < 0 || snippet->source_count > 10000000 || snippet->source_offset < 0 ||
          snippet->source_offset > snippet->source_count || snippet->input_count > snippet->source_count - snippet->source_offset)
        goto malformed;
      if (snippet->source_count > 0) {
        snippet->inputs = malloc(sizeof(*snippet->inputs) * (size_t)snippet->source_count);
        if (!snippet->inputs || !read_exact(f, snippet->inputs, sizeof(*snippet->inputs) * (size_t)snippet->source_count)) goto malformed;
      }
      model_snippet_normalize(snippet);
    }
  }

  if (header.version >= 3) {
    if (!read_exact(f, &imported.event_count, sizeof(imported.event_count)) || imported.event_count < 0 || imported.event_count > 1000000)
      goto malformed;
    imported.events = calloc((size_t)imported.event_count, sizeof(*imported.events));
    if (imported.event_count > 0 && !imported.events) goto malformed;
    for (int i = 0; i < imported.event_count; ++i) {
      if (header.version >= 8) {
        if (!read_exact(f, &imported.events[i], sizeof(imported.events[i])) || imported.events[i].group_index < 0 ||
            imported.events[i].group_index >= imported.group_count)
          goto malformed;
      } else {
        legacy_net_event_t legacy;
        if (!read_exact(f, &legacy, sizeof(legacy))) goto malformed;
        legacy_event_to_current(&legacy, &imported.events[i]);
      }
    }
  }

  // Parsing and compatibility checks are complete. From here on, append the imported state.
  timeline_state_t *ts = &ui->timeline;
  int first_new_group = ts->group_count;
  rollback_group_count = first_new_group;
  int *track_map = malloc(sizeof(*track_map) * (size_t)imported.track_count);
  if (imported.track_count > 0 && !track_map) goto malformed;
  for (int i = 0; i < imported.track_count; ++i) track_map[i] = -1;

  for (int source_group = 0; source_group < imported.group_count; ++source_group) {
    group_file_header_t *source = &imported.groups[source_group];
    timeline_group_t *group = model_add_group(ts, source->name);
    if (!group) {
      free(track_map);
      goto malformed;
    }
    if (header.version >= 8) memcpy(group->color, source->color, sizeof(group->color));
    group->visible = source->visible;
    group->demo_export_enabled = source->demo_export_enabled;
    group->start_offset = source->start_offset;

    model_set_active_group(ts, first_new_group + source_group);
    for (int source_track = 0; source_track < imported.track_count; ++source_track) {
      player_track_t *from = &imported.tracks[source_track];
      if (from->group_index != source_group) continue;
      if (from->player_info.skin >= 3) from->player_info.skin = find_or_load_imported_skin(ui, from->player_info.skin_name);
      player_track_t *to = model_add_new_track(ts, ph, 1);
      if (!to) {
        free(track_map);
        goto malformed;
      }
      int destination = (int)(to - ts->player_tracks);
      track_map[source_track] = destination;
      int destination_group = to->group_index;
      *to = *from;
      to->group_index = destination_group;
      to->recording_snippets = NULL;
      to->recording_snippet_count = 0;
      to->recording_snippet_capacity = 0;
      for (int j = 0; j < to->snippet_count; ++j) to->snippets[j].id = ts->next_snippet_id++;
      from->snippets = NULL;
      from->snippet_count = 0;
    }
  }

  for (int i = 0; i < imported.track_count; ++i)
    if (track_map[i] >= 0 && ts->player_tracks[track_map[i]].starting_config.enabled) model_apply_starting_config(ts, track_map[i]);
  free(track_map);

  for (int i = 0; i < imported.event_count; ++i) {
    imported.events[i].group_index += first_new_group;
    net_events_add(ts, imported.events[i]);
  }
  model_set_active_group(ts, first_new_group);
  if (model_group_track_count(ts, first_new_group) > 0)
    ts->selected_player_track_index = model_group_track_index(ts, first_new_group, 0);
  model_recalc_physics(ts, 0);
  log_info(LOG_SOURCE, "Imported %d group(s) and %d track(s) from '%s'", imported.group_count, imported.track_count, path);
  ok = true;
  rollback_group_count = -1;
  goto done;

malformed:
  log_error(LOG_SOURCE, "Malformed or incomplete project while importing '%s'", path);
done:
  if (!ok && rollback_group_count >= 0) {
    while (ui->timeline.group_count > rollback_group_count)
      model_remove_group(&ui->timeline, ui->timeline.group_count - 1);
  }
  free_imported_timeline(&imported);
  fclose(f);
  return ok;
}
