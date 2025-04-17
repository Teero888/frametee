#include "graphics_backend.h"

int main(void) {
  gfx_handler handler;
  init_gfx_handler(&handler);
  while (gfx_next_frame(&handler) == 0)
    ;
  gfx_cleanup(&handler);
  return 0;
}
