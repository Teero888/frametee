#include "script_engine.h"
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/fs.h>
#include <system/save.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline_events.h>
#include <types.h>

#define LOG_SOURCE "ScriptEngine"

typedef struct {
  char *name;
  script_command_cb callback;
} command_entry_t;

static command_entry_t *g_commands = NULL;
static int g_num_commands = 0;
static ui_handler_t *g_ui = NULL;
static const tas_api_t *g_api = NULL;

void script_engine_init(ui_handler_t *ui, const tas_api_t *api) {
  g_ui = ui;
  g_api = api;
}

void script_engine_register_command(const char *name, script_command_cb callback) {
  g_commands = realloc(g_commands, sizeof(command_entry_t) * (g_num_commands + 1));
  g_commands[g_num_commands].name = strdup(name);
  g_commands[g_num_commands].callback = callback;
  g_num_commands++;
}

static void cmd_set_variant(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: set_variant <variant>");
    return;
  }
  game_host_t *host = &g_ui->gfx_handler->game_host;
  const ft_game_module *module = host->module;
  if (!module) {
    log_warn(LOG_SOURCE, "No game is active.");
    return;
  }

  for (uint32_t i = 0; i < module->constraints.variant_count; ++i) {
    if (strcmp(module->constraints.variants[i].id, argv[1]) != 0) continue;
    game_host_set_variant(host, argv[1]);
    log_info(LOG_SOURCE, "Ruleset set to %s", module->constraints.variants[i].display_name);
    return;
  }

  log_warn(LOG_SOURCE, "Unknown ruleset '%s' for game '%s'.", argv[1], game_host_active_id(host));
  for (uint32_t i = 0; i < module->constraints.variant_count; ++i)
    log_warn(LOG_SOURCE, "  %s", module->constraints.variants[i].id);
}

static void cmd_activate_game(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: activate_game <game-id>");
    return;
  }
  game_host_t *host = &g_ui->gfx_handler->game_host;
  const int index = game_host_find_id(host, argv[1]);
  if (index < 0) {
    log_error(LOG_SOURCE, "No game module provides id '%s'.", argv[1]);
    return;
  }
  if (!gfx_activate_game(g_ui->gfx_handler, index))
    log_error(LOG_SOURCE, "Could not activate game '%s'.", argv[1]);
}

static void cmd_load_level(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: load_level <path>");
    return;
  }
  log_info(LOG_SOURCE, "Loading level %s", argv[1]);
  on_level_load_path(g_ui->gfx_handler, argv[1]);
}

static void cmd_add_track(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  log_info(LOG_SOURCE, "Adding player track");
  if (!g_ui->gfx_handler->level) {
    log_error(LOG_SOURCE, "Cannot add a track without a level loaded.");
    return;
  }
  model_add_new_track(&g_ui->timeline, 1);
}

static void cmd_export(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!g_ui || !g_ui->gfx_handler) {
    log_error(LOG_SOURCE, "Cant export without engine being initialized");
    return;
  }
  if (!g_ui->timeline.player_track_count) {
    log_error(LOG_SOURCE, "Cant export if no tracks are present.");
    return;
  }

  // Exporting is a game feature: the engine only picks the exporter, the range
  // and the file name, and lets the game write whatever format it owns.
  game_host_t *host = &g_ui->gfx_handler->game_host;
  if (gh_exporter_count(host) == 0) {
    log_error(LOG_SOURCE, "The active game provides no exporters.");
    return;
  }

  const ft_exporter_desc *desc = gh_exporter_desc(host, 0);
  const char *level_name = (g_ui->loaded_level_name[0] != '\0') ? g_ui->loaded_level_name : "unnamed_level";
  const int max_ticks = model_get_max_timeline_tick(&g_ui->timeline) + 500;

  char save_path[1024];
  snprintf(save_path, sizeof(save_path), "%s.%s", level_name, desc && desc->file_extension ? desc->file_extension : "out");

  ft_export_request request = {.struct_size = sizeof(request), .path = save_path, .start_tick = 0, .end_tick = max_ticks};
  if (gh_export_run(host, 0, &request)) log_info(LOG_SOURCE, "Exported to '%s'", save_path);
  else log_error(LOG_SOURCE, "Export to '%s' failed", save_path);
}

static void cmd_collect_timeline_events(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!g_ui) {
    log_error(LOG_SOURCE, "Cannot collect timeline events before the engine is initialized.");
    return;
  }
  log_info(LOG_SOURCE, "Collecting timeline events...");
  timeline_rescan_events(&g_ui->timeline);
}

static void cmd_tick(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: tick <count>");
    return;
  }
  int count = atoi(argv[1]);
  log_info(LOG_SOURCE, "Ticking %d times", count);
  for (int i = 0; i < count; i++) {
    // Advance the playhead and construct the same adjacent world pair used by
    // rendering. Besides moving a headless simulation, this keeps automation
    // exercising the interpolation-cache contract.
    timeline_state_t *timeline = &g_ui->timeline;
    model_advance_tick(timeline, 1);
    const ft_world *previous = NULL;
    const ft_world *current = NULL;
    model_group_world_pair(timeline, timeline->active_group_index, timeline->current_tick, &previous, &current);
    const int current_world_tick = gh_world_tick(&g_ui->gfx_handler->game_host, current);
    if (current_world_tick > 0 &&
        (!previous || gh_world_tick(&g_ui->gfx_handler->game_host, previous) + 1 != current_world_tick)) {
      log_error(LOG_SOURCE, "Game returned a non-adjacent world pair at tick %d.", timeline->current_tick);
    }
    plugin_manager_update_all(&g_ui->plugin_manager);
  }
}

static void cmd_save_project(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: save_project <path>");
    return;
  }
  if (!save_project(g_ui, argv[1])) log_error(LOG_SOURCE, "Could not save project to '%s'.", argv[1]);
}

static void cmd_load_project(int argc, const char **argv) {
  if (argc < 2) {
    log_warn(LOG_SOURCE, "Usage: load_project <path>");
    return;
  }
  if (!load_project(g_ui, argv[1])) log_error(LOG_SOURCE, "Could not load project from '%s'.", argv[1]);
}

static void cmd_quick_save(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!ui_quick_save(g_ui)) {
    log_error(LOG_SOURCE, "Could not quick-save the current project.");
    return;
  }
  log_info(LOG_SOURCE, "Quick-saved current project to '%s'.", g_ui->current_project_path);
}

void script_engine_run(const char *script_path) {
  // Register built-ins
  script_engine_register_command("set_variant", cmd_set_variant);
  script_engine_register_command("activate_game", cmd_activate_game);
  script_engine_register_command("load_level", cmd_load_level);
  script_engine_register_command("add_track", cmd_add_track);
  script_engine_register_command("tick", cmd_tick);
  script_engine_register_command("collect_timeline_events", cmd_collect_timeline_events);
  script_engine_register_command("export", cmd_export);
  script_engine_register_command("save_project", cmd_save_project);
  script_engine_register_command("load_project", cmd_load_project);
  script_engine_register_command("quick_save", cmd_quick_save);

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
