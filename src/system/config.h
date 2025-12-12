#ifndef CONFIG_H
#define CONFIG_H

#include <user_interface/user_interface.h>

void config_load(struct ui_handler *ui);
void config_save(struct ui_handler *ui);

#endif // CONFIG_H
