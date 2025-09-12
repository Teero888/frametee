#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"

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

    if (handler.map_data->game_layer.data) {
/*       // Head
      vec2 head_center = {100.0f, 100.0f};
      float head_radius = 40.0f;
      vec4 pink = {1.0f, 0.6f, 0.8f, 1.0f};
      renderer_draw_circle_filled(&handler, head_center, head_radius, pink, 64);

      // Ears
      vec2 ear_left = {head_center[0] - head_radius * 0.7f, head_center[1] - head_radius * 0.8f};
      vec2 ear_right = {head_center[0] + head_radius * 0.7f, head_center[1] - head_radius * 0.8f};
      renderer_draw_circle_filled(&handler, ear_left, head_radius * 0.4f, pink, 64);
      renderer_draw_circle_filled(&handler, ear_right, head_radius * 0.4f, pink, 64);

      // Eyes
      vec2 eye_left = {head_center[0] - head_radius * 0.4f, head_center[1] - head_radius * 0.2f};
      vec2 eye_right = {head_center[0] + head_radius * 0.4f, head_center[1] - head_radius * 0.2f};
      vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
      renderer_draw_circle_filled(&handler, eye_left, head_radius * 0.2f, white, 32);
      renderer_draw_circle_filled(&handler, eye_right, head_radius * 0.2f, white, 32);

      // Pupils
      vec4 black = {0.0f, 0.0f, 0.0f, 1.0f};
      renderer_draw_circle_filled(&handler, eye_left, head_radius * 0.08f, black, 16);
      renderer_draw_circle_filled(&handler, eye_right, head_radius * 0.08f, black, 16);

      vec4 red_color = {1.0f, 0.0f, 0.0f, 1.0f};
      renderer_draw_rect_filled(&handler, (vec2){50 * 32, 50 * 32}, (vec2){0.01, 0.01}, red_color);
 */
      vec4 border_color = {1.0f, 0.0f, 0.0f, 1.0f};
      renderer_draw_line(&handler, (vec2){0, 0}, (vec2){handler.map_data->width, 0.0}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){0, 0}, (vec2){0.0, handler.map_data->height}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){handler.map_data->width, 0},
                         (vec2){handler.map_data->width, handler.map_data->height}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){0, handler.map_data->height},
                         (vec2){handler.map_data->width, handler.map_data->height}, border_color, 1.f);
    }
    gfx_end_frame(&handler);

    err = gfx_begin_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}
