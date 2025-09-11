#include "graphics_backend.h"
#include "renderer.h"

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;
  on_map_load(&handler, "data/maps/Kobra 4.map");

  int err = FRAME_SKIP;
  while (err != FRAME_EXIT) {
    if (err == FRAME_SKIP) {
      err = gfx_begin_frame(&handler);
      continue;
    }
    ui_render(&handler.user_interface);

    if (handler.map_data.game_layer.data) {
      ImVec2 mouse_pos;
      igGetMousePos(&mouse_pos);
      vec2 out;
      screen_to_world(&handler, mouse_pos.x, mouse_pos.y, &out[0], &out[1]);
      printf("Mouse Pos: (%.2f, %.2f) Mouse World Pos: (%.2f, %.2f)\n", mouse_pos.x, mouse_pos.y, out[0],
             out[1]);

      vec2 circle_center = {0.0f, 0.0f};
      float circle_radius = 1.0f;
      vec4 circle_color = {0.0f, 0.0f, 1.0f, 0.8f};

      for (int i = 0; i < 10; ++i) {
        renderer_draw_circle_filled(&handler, circle_center, circle_radius, circle_color, 32);
        circle_center[0] += 0.1f;
      }
      vec4 red_color = {1.0f, 0.0f, 0.0f, 1.0f};
      renderer_draw_rect_filled(&handler, (vec2){50, 400}, (vec2){150, 50}, red_color);
    }
    gfx_end_frame(&handler);

    err = gfx_begin_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}