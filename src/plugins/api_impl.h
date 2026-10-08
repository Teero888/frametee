#ifndef API_IMPL_H
#define API_IMPL_H

#include "plugin_api.h"
#include <types.h>
#include <user_interface/user_interface.h>

tas_api_t api_init(ui_handler_t *ui_handler);
// A file dialog a plugin opened answers nobody: plugins are being unloaded.
void api_forget_file_dialog(void);

#endif // API_IMPL_H
