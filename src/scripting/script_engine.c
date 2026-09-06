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

bool script_engine_execute_command(const char *name, int argc, const char **argv) {
  for (int i = 0; i < g_num_commands; i++) {
    if (strcmp(name, g_commands[i].name) == 0) {
      g_commands[i].callback(argc, argv);
      return true;
    }
  }
  log_error(LOG_SOURCE, "Command '%s' not found.", name);
  return false;
}

void script_engine_shutdown(void) {
  for (int i = 0; i < g_num_commands; i++) {
    free(g_commands[i].name);
  }
  free(g_commands);
  g_commands = NULL;
  g_num_commands = 0;
}
