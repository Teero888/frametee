#include "user_interface.h"
#include "cimgui.h"
#include <stdbool.h>

void ui_init(ui_handler *ui) { ui->show_demo_window = true; }

void ui_render(ui_handler *ui) {
  if (ui->show_demo_window)
    igShowDemoWindow(&ui->show_demo_window);
}
