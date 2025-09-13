#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include <GLFW/glfw3.h>

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;
  on_map_load(&handler, "data/maps/Kobra 4.map");

  int err = FRAME_SKIP;
  while (1) {
    int frame_result = gfx_begin_frame(&handler);
    if (frame_result == FRAME_EXIT)
      break;
    if (frame_result == FRAME_SKIP)
      continue;

    on_camera_update(&handler);
    renderer_draw_map(&handler);
    ui_render(&handler.user_interface);

    ImGuiIO *io = igGetIO_Nil();
    if (handler.user_interface.timeline.recording) {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      io->ConfigFlags |= ImGuiConfigFlags_NoMouse; // completely ignore mouse
    } else {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse; // turn mouse back on
    }

    // draw primitives
    if (handler.map_data->game_layer.data) {
      vec4 border_color = {1.0f, 0.0f, 0.0f, 1.0f};
      renderer_draw_line(&handler, (vec2){0, 0}, (vec2){handler.map_data->width, 0.0}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){0, 0}, (vec2){0.0, handler.map_data->height}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){handler.map_data->width, 0},
                         (vec2){handler.map_data->width, handler.map_data->height}, border_color, 1.f);
      renderer_draw_line(&handler, (vec2){0, handler.map_data->height},
                         (vec2){handler.map_data->width, handler.map_data->height}, border_color, 1.f);
    }

    gfx_end_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}
