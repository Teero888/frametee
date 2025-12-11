#include "cimgui.h"
#include "logger/logger.h"
#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include "user_interface/user_interface.h"
#include <GLFW/glfw3.h>

int main(void) {
  logger_init();

  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0) return 1;
  handler.map_data = &handler.physics_handler.collision.m_MapData;
  // on_map_load(&handler, "data/maps/Kobra 4.map");

  bool viewport_hovered = false;

  int err = FRAME_SKIP;
  while (1) {
    int frame_result = gfx_begin_frame(&handler);
    if (frame_result == FRAME_EXIT) break;
    if (frame_result == FRAME_SKIP) continue;

    on_camera_update(&handler, viewport_hovered);

    // render players and weapons
    renderer_begin_skins(&handler);
    renderer_begin_atlas_instances(&handler.renderer.gameskin_renderer);
    render_players(&handler.user_interface);
    renderer_flush_atlas_instances(&handler, handler.current_frame_command_buffer, &handler.renderer.gameskin_renderer, false);
    renderer_flush_skins(&handler, handler.current_frame_command_buffer, handler.renderer.skin_manager.atlas_array);
    // draw ui
    ui_render(&handler.user_interface);

    // lock mouse when recording
    ImGuiIO *io = igGetIO_Nil();
    if (handler.user_interface.timeline.recording) {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      io->ConfigFlags |= ImGuiConfigFlags_NoMouse; // completely ignore mouse
    } else {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse; // turn mouse back on
    }

    renderer_draw_map(&handler);

    // render cursor
    renderer_begin_atlas_instances(&handler.renderer.cursor_renderer);
    render_cursor(&handler.user_interface);
    renderer_flush_atlas_instances(&handler, handler.current_frame_command_buffer, &handler.renderer.cursor_renderer, true);

    viewport_hovered = gfx_end_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}
