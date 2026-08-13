#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#include <plugins/plugin_api.h>
#include <types.h>

typedef void (*script_command_cb)(int argc, const char **argv);

void script_engine_init(ui_handler_t *ui, const tas_api_t *api);
void script_engine_register_command(const char *name, script_command_cb callback);
void script_engine_run(const char *script_path);
void script_engine_shutdown(void);

#endif // SCRIPT_ENGINE_H
