#include "api_impl.h"
#include "../logger/logger.h"
#include "../renderer/graphics_backend.h"
#include "../user_interface/timeline/timeline_commands.h"
#include "../user_interface/timeline/timeline_model.h"
#include "../scripting/script_engine.h"
#include "renderer/renderer.h"
#include <ddnet_physics/gamecore.h>
#include <stdio.h>
#include <stdlib.h>

//=================================================================================================
// API IMPLEMENTATION
//=================================================================================================

// this global pointer allows the static API functions to access the application's state.
// it is set once by api_init() and is internal to this file.
static ui_handler_t *g_ui_handler_for_api = NULL;

static void api_register_script_command(const char *name, void (*callback)(int argc, const char **argv)) {
  script_engine_register_command(name, callback);
}

static int api_get_current_tick(void) { return g_ui_handler_for_api->timeline.current_tick; }

static int api_get_track_count(void) { return g_ui_handler_for_api->timeline.player_track_count; }

// READ ONLY PLEASE
static SWorldCore *api_get_initial_world(void) {
  return g_ui_handler_for_api->gfx_handler->physics_handler.loaded ? &g_ui_handler_for_api->gfx_handler->physics_handler.world : NULL;
}

static void api_log_info(const char *plugin_name, const char *message) { log_info(plugin_name, "%s", message); }
static void api_log_warning(const char *plugin_name, const char *message) { log_warn(plugin_name, "%s", message); }
static void api_log_error(const char *plugin_name, const char *message) { log_error(plugin_name, "%s", message); }

static SWorldCore *api_get_world_state_at(int tick) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;

  SWorldCore *world_copy = (SWorldCore *)malloc(sizeof(SWorldCore));
  if (!world_copy) return NULL;
  *world_copy = wc_empty();

  const int step = 50;
  int snapshot_index = (tick - 1) / step;
  snapshot_index = imin(snapshot_index, (int)ts->vec.current_size - 1);
  snapshot_index = imax(snapshot_index, 0);

  if (ts->vec.current_size == 0) {
    free(world_copy);
    return NULL;
  }

  wc_copy_world(world_copy, &ts->vec.data[snapshot_index]);

  while (world_copy->m_GameTick < tick) {
    for (int p = 0; p < world_copy->m_NumCharacters; ++p) {
      SPlayerInput input = model_get_input_at_tick(ts, p, world_copy->m_GameTick);
      cc_on_input(&world_copy->m_pCharacters[p], &input);
    }
    wc_tick(world_copy);
  }

  return world_copy;
}

static struct undo_command_t *api_do_create_track(const player_info_t *info, int *out_track_index) {
  return timeline_api_create_track(g_ui_handler_for_api, info, out_track_index);
}

static struct undo_command_t *api_do_create_snippet(int track_index, int start_tick, int duration, int *out_snippet_id) {
  return timeline_api_create_snippet(g_ui_handler_for_api, track_index, start_tick, duration, out_snippet_id);
}

static struct undo_command_t *api_do_set_inputs(int snippet_id, int tick_offset, int count, const SPlayerInput *new_inputs) {
  return timeline_api_set_snippet_inputs(g_ui_handler_for_api, snippet_id, tick_offset, count, new_inputs);
}

static void api_register_undo_command(struct undo_command_t *command) {
  if (command) {
    undo_manager_register_command(&g_ui_handler_for_api->undo_manager, command);
  }
}

static void api_draw_line_world(vec2 start, vec2 end, float z, vec4 color, float thickness) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_line(g_ui_handler_for_api->gfx_handler, z + 10.0f, start, end, color, thickness);
}

static void api_draw_circle_world(vec2 center, float radius, vec4 color) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_circle_filled(g_ui_handler_for_api->gfx_handler, 10.0f, center, radius, color, 16);
}

static void api_draw_rect_filled_world(vec2 pos, vec2 size, float z, vec4 color) {
  extern bool g_is_headless;
  if (g_is_headless) return;
  renderer_submit_rect_filled(g_ui_handler_for_api->gfx_handler, z + 10.0f, pos, size, color);
}

static void api_draw_text_world(vec2 pos, const char *text, vec4 color) {
  // Not implemented yet in renderer, but we should have a stub to avoid NULL call
  (void)pos; (void)text; (void)color;
}

static void api_screen_to_world(float screen_x, float screen_y, float *world_x, float *world_y) {
  float lx = screen_x - g_ui_handler_for_api->viewport_window_pos.x;
  float ly = screen_y - g_ui_handler_for_api->viewport_window_pos.y;
  
  static ImGuiWindow *s_viewport_window = NULL;
  if (!s_viewport_window) {
    s_viewport_window = igFindWindowByName("Viewport");
  }
  if (s_viewport_window) {
    lx -= s_viewport_window->DecoOuterSizeX1;
    ly -= s_viewport_window->DecoOuterSizeY1;
  }
  
  screen_to_world(g_ui_handler_for_api->gfx_handler, lx, ly, world_x, world_y);
}

static void api_world_to_screen(float world_x, float world_y, float *screen_x, float *screen_y) {
  world_to_screen(g_ui_handler_for_api->gfx_handler, world_x, world_y, screen_x, screen_y);
  *screen_x += g_ui_handler_for_api->viewport_window_pos.x;
  *screen_y += g_ui_handler_for_api->viewport_window_pos.y;
  
  static ImGuiWindow *s_viewport_window = NULL;
  if (!s_viewport_window) {
    s_viewport_window = igFindWindowByName("Viewport");
  }
  if (s_viewport_window) {
    *screen_x += s_viewport_window->DecoOuterSizeX1;
    *screen_y += s_viewport_window->DecoOuterSizeY1;
  }
}

static double api_get_time(void) {
  return glfwGetTime();
}

static void api_get_camera_info(vec2 pos, float *zoom) {
  glm_vec2_copy(g_ui_handler_for_api->gfx_handler->renderer.camera.pos, pos);
  *zoom = g_ui_handler_for_api->gfx_handler->renderer.camera.zoom;
}

tas_api_t api_init(ui_handler_t *ui_handler) {
  g_ui_handler_for_api = ui_handler;

  return (tas_api_t){
      .get_current_tick = api_get_current_tick,
      .get_track_count = api_get_track_count,
      .get_initial_world = api_get_initial_world,
      .get_world_state_at = api_get_world_state_at,
      .do_create_track = api_do_create_track,
      .register_undo_command = api_register_undo_command,
      .do_create_snippet = api_do_create_snippet,
      .do_set_inputs = api_do_set_inputs,
      .draw_line_world = api_draw_line_world,
      .draw_circle_world = api_draw_circle_world,
      .draw_rect_filled_world = api_draw_rect_filled_world,
      .draw_text_world = api_draw_text_world,
      .screen_to_world = api_screen_to_world,
      .world_to_screen = api_world_to_screen,
      .get_time = api_get_time,
      .get_camera_info = api_get_camera_info,
      .log_info = api_log_info,
      .log_warning = api_log_warning,
      .log_error = api_log_error,
      .register_script_command = api_register_script_command,
  };
}
