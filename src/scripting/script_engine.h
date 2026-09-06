#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#include <plugins/plugin_api.h>
#include <types.h>
#include <stdbool.h>

typedef void (*script_command_cb)(int argc, const char **argv);

void script_engine_init(ui_handler_t *ui, const tas_api_t *api);
void script_engine_register_command(const char *name, script_command_cb callback);
bool script_engine_execute_command(const char *name, int argc, const char **argv);
void script_engine_shutdown(void);

#endif // SCRIPT_ENGINE_H
