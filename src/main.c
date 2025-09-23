#include "cimgui.h"
#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include <GLFW/glfw3.h>

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;
  handler.map_data = &handler.physics_handler.collision.m_MapData;
  // on_map_load(&handler, "data/maps/Kobra 4.map");

  bool viewport_hovered = false;

  int err = FRAME_SKIP;
  while (1) {
    int frame_result = gfx_begin_frame(&handler);
    if (frame_result == FRAME_EXIT)
      break;
    if (frame_result == FRAME_SKIP)
      continue;

    on_camera_update(&handler, viewport_hovered);

    renderer_begin_skins(&handler);
    render_players(&handler.user_interface);
    renderer_flush_skins(&handler, handler.current_frame_command_buffer,
                         handler.renderer.skin_manager.atlas_array);
    ui_render(&handler.user_interface);

    ImGuiIO *io = igGetIO_Nil();
    if (handler.user_interface.timeline.recording) {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      io->ConfigFlags |= ImGuiConfigFlags_NoMouse; // completely ignore mouse
    } else {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse; // turn mouse back on
    }

    renderer_draw_map(&handler);
    viewport_hovered = gfx_end_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}
