#include "script_engine.h"
#include "ddnet_physics/gamecore.h"
#include <logger/logger.h>
#include <physics/physics.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/fs.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/net_events.h>

#define LOG_SOURCE "ScriptEngine"

typedef struct {
  char *name;
  script_command_cb callback;
} command_entry_t;

static command_entry_t *g_commands = NULL;
static int g_num_commands = 0;
static tas_context_t *g_ctx = NULL;
static const tas_api_t *g_api = NULL;

void script_engine_init(tas_context_t *ctx, const tas_api_t *api) {
  g_ctx = ctx;
  g_api = api;
}

void script_engine_register_command(const char *name, script_command_cb callback) {
  g_commands = realloc(g_commands, sizeof(command_entry_t) * (g_num_commands + 1));
  g_commands[g_num_commands].name = strdup(name);
  g_commands[g_num_commands].callback = callback;
  g_num_commands++;
}

static void cmd_load_map(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: load_map <path>");
    return;
  }
  log_info(LOG_SOURCE, "Loading map %s", argv[1]);
  on_map_load_path(g_ctx->gfx_handler, argv[1]);
}

static void cmd_spawn_character(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  log_info(LOG_SOURCE, "Spawning character");
  if (!g_ctx->gfx_handler->physics_handler.world.m_pCollision) {
    log_error(LOG_SOURCE, "Cannot spawn character without map loaded.");
    return;
  }
  model_add_new_track(g_ctx->timeline, &g_ctx->gfx_handler->physics_handler, 1);
}

static void cmd_export_demo(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!g_ctx || !g_ctx->gfx_handler) {
    log_error(LOG_SOURCE, "Cant export demo without engine being initialized");
    return;
  }
  if (!g_ctx->ui_handler->timeline.player_track_count) {
    log_error(LOG_SOURCE, "Cant export demo if no tracks are present.");
    return;
  }
  const char *map_name = (g_ctx->ui_handler->loaded_map_name[0] != '\0') ? g_ctx->ui_handler->loaded_map_name : "unnamed_map";
  char default_file_name[256];
  snprintf(default_file_name, sizeof(default_file_name), "%s.demo", map_name);
  int max_ticks = model_get_max_timeline_tick(&g_ctx->ui_handler->timeline) + 500;
  char save_path[1024];
  SWorldCore world = wc_empty();
  model_get_world_state_at_tick(&g_ctx->ui_handler->timeline, max_ticks, &world, false);
  SCharacterCore *p = &world.m_pCharacters[0];
  int ticks = p->m_FinishTick - p->m_StartTick;
  wc_free(&world);
  snprintf(save_path, sizeof(save_path), "%s_%.3f_%s.demo", map_name, (float)ticks / 50.f, g_ctx->timeline->player_tracks[0].player_info.name[0] ? g_ctx->timeline->player_tracks[0].player_info.name : "nameless tee");
  int res = export_to_demo(g_ctx->ui_handler, save_path, map_name, max_ticks);
  if (res == 0) {
    log_info(LOG_SOURCE, "Demo exported successfully to '%s'", save_path);
  } else {
    log_error(LOG_SOURCE, "Failed to export demo to '%s'", save_path);
  }
}

static void cmd_generate_finish_events(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!g_ctx || !g_ctx->ui_handler) {
    log_error(LOG_SOURCE, "Cant generate finish events without engine being initialized");
    return;
  }
  log_info(LOG_SOURCE, "Generating finish net events...");
  timeline_generate_finish_events(&g_ctx->ui_handler->timeline);
}

static void cmd_tick(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: tick <count>");
    return;
  }
  int count = atoi(argv[1]);
  log_info(LOG_SOURCE, "Ticking %d times", count);
  for (int i = 0; i < count; i++) {
    plugin_manager_update_all(&g_ctx->ui_handler->plugin_manager);
  }
}

void script_engine_run(const char *script_path) {
  // Register built-ins
  script_engine_register_command("load_map", cmd_load_map);
  script_engine_register_command("spawn_character", cmd_spawn_character);
  script_engine_register_command("tick", cmd_tick);
  script_engine_register_command("generate_finish_events", cmd_generate_finish_events);
  script_engine_register_command("export_demo", cmd_export_demo);

  FILE *f = fs_open(script_path, "r");
  if (!f) {
    log_error(LOG_SOURCE, "Failed to open script file: %s", script_path);
    return;
  }

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    // Basic tokenizer
    char *tokens[64];
    int n_tokens = 0;
    char *token = strtok(line, " \t\n\r");
    while (token && n_tokens < 64) {
      tokens[n_tokens++] = token;
      token = strtok(NULL, " \t\n\r");
    }

    if (n_tokens == 0 || tokens[0][0] == '#') continue;

    // Dispatch
    bool found = false;
    for (int i = 0; i < g_num_commands; i++) {
      if (strcmp(tokens[0], g_commands[i].name) == 0) {
        g_commands[i].callback(n_tokens, (const char **)tokens);
        found = true;
        break;
      }
    }

    if (!found) {
      log_error(LOG_SOURCE, "Unknown command '%s'", tokens[0]);
    }
  }

  fclose(f);
}

void script_engine_shutdown(void) {
  for (int i = 0; i < g_num_commands; i++) {
    free(g_commands[i].name);
  }
  free(g_commands);
  g_commands = NULL;
  g_num_commands = 0;
}
