#include "logger/logger.h"
#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include "user_interface/user_interface.h"
#include <GLFW/glfw3.h>
#include <particles/particle_system.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

int main(void) {
  logger_init();

  static struct gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0) return 1;
  handler.map_data = &handler.physics_handler.collision.m_MapData;

  bool viewport_hovered = false;
  double last_time = glfwGetTime();

  while (1) {
    double now = glfwGetTime();

    if (handler.user_interface.fps_limit > 0) {
      double target_dt = 1.0 / (double)handler.user_interface.fps_limit;
      while (now - last_time < target_dt) {
        double remaining = target_dt - (now - last_time);
        if (remaining > 0.001) {
#ifdef _WIN32
          Sleep((DWORD)((remaining - 0.0005) * 1000));
#else
          struct timespec ts;
          ts.tv_sec = 0;
          ts.tv_nsec = (long)((remaining - 0.0005) * 1e9);
          nanosleep(&ts, NULL);
#endif
        }
        now = glfwGetTime();
      }
    }
    last_time = now;

    int frame_result = gfx_begin_frame(&handler);
    if (frame_result == FRAME_EXIT) break;
    if (frame_result == FRAME_SKIP) continue;

    on_camera_update(&handler, viewport_hovered);

    float speed_scale = handler.user_interface.timeline.is_reversing ? 2.0f : 1.0f;
    float intra = fminf((igGetTime() - handler.user_interface.timeline.last_update_time) / (1.f / (handler.user_interface.timeline.playback_speed * speed_scale)), 1.f);
    if (handler.user_interface.timeline.is_reversing) intra = 1.f - intra;

    renderer_submit_map(&handler, Z_LAYER_MAP);
    render_pickups(&handler.user_interface);
    render_players(&handler.user_interface);

    handler.user_interface.particle_system.current_time = (double)(handler.user_interface.timeline.current_tick + intra) * 0.02;
    particle_system_update_sim(&handler.user_interface.particle_system, handler.map_data);
    particle_system_render(&handler.user_interface.particle_system, &handler, 0);
    particle_system_render(&handler.user_interface.particle_system, &handler, 1);
    render_cursor(&handler.user_interface);
    renderer_flush_queue(&handler, handler.current_frame_command_buffer);

    ui_render(&handler.user_interface);

    // Mouse locking logic for recording
    ImGuiIO *io = igGetIO_Nil();
    if (handler.user_interface.timeline.recording) {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    viewport_hovered = gfx_end_frame(&handler);
  }

  gfx_cleanup(&handler);
  return 0;
}
