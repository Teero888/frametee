#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include <stdbool.h>
typedef struct {
  bool show_demo_window;
} ui_handler;

void ui_init(ui_handler *ui);
void ui_render(ui_handler *ui);

#endif
