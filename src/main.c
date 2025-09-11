#include "graphics_backend.h"
#include "renderer.h"

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;
  on_map_load(&handler, "data/maps/Kobra 4.map");

  while (gfx_next_frame(&handler) == 0)
    ;
  gfx_cleanup(&handler);
  return 0;
}