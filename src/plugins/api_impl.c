#include "api_impl.h"
#include "../user_interface/timeline.h"
#include <stdio.h>
#include <stdlib.h>

//=================================================================================================
// API IMPLEMENTATION
//=================================================================================================

// This global pointer allows the static API functions to access the application's state.
// It is set once by api_init() and is internal to this file.
static ui_handler_t *g_ui_handler_for_api = NULL;

static int api_get_current_tick() { return g_ui_handler_for_api->timeline.current_tick; }

static int api_get_track_count() { return g_ui_handler_for_api->timeline.player_track_count; }

static void api_log_info(const char *plugin_name, const char *message) {
  printf("INFO [%s]: %s\n", plugin_name, message);
}
static void api_log_warning(const char *plugin_name, const char *message) {
  printf("WARN [%s]: %s\n", plugin_name, message);
}
static void api_log_error(const char *plugin_name, const char *message) {
  printf("ERROR [%s]: %s\n", plugin_name, message);
}

static SWorldCore *api_get_world_state_at(int tick) {
  timeline_state_t *ts = &g_ui_handler_for_api->timeline;

  SWorldCore *world_copy = (SWorldCore *)malloc(sizeof(SWorldCore));
  if (!world_copy)
    return NULL;

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
      SPlayerInput input = get_input(ts, p, world_copy->m_GameTick);
      cc_on_input(&world_copy->m_pCharacters[p], &input);
    }
    wc_tick(world_copy);
  }

  return world_copy;
}

static void api_free_world_state(SWorldCore *world) {
  if (world) {
    wc_free(world);
    free(world);
  }
}

static int api_get_player_count(const SWorldCore *world_state) {
  return world_state ? world_state->m_NumCharacters : 0;
}

static const SCharacterCore *api_get_player(const SWorldCore *world_state, int player_index) {
  if (world_state && player_index >= 0 && player_index < world_state->m_NumCharacters) {
    return &world_state->m_pCharacters[player_index];
  }
  return NULL;
}

static const SProjectile *api_get_first_projectile(const SWorldCore *world_state) {
  return world_state ? world_state->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE] : NULL;
}

static const SProjectile *api_get_next_projectile(const SProjectile *current) {
  return current ? current->m_Base.m_pNextTypeEntity : NULL;
}

static undo_command_t *api_do_create_snippet(int track_index, int start_tick, int duration) {
  // TODO: This is a simplified undo. a full implementation would be needed in timeline.c that creates a proper command for adding a snippet.
  input_snippet_t snip = create_empty_snippet(&g_ui_handler_for_api->timeline, start_tick, duration);
  add_snippet_to_track(&g_ui_handler_for_api->timeline.player_tracks[track_index], &snip);
  return NULL;
}

static void api_register_undo_command(undo_command_t *command) {
  if (command) {
    undo_manager_register_command(&g_ui_handler_for_api->undo_manager, command);
  }
}

static void api_draw_line_world(vec2 start, vec2 end, vec4 color, float thickness) {
  renderer_draw_line(g_ui_handler_for_api->gfx_handler, start, end, color, thickness);
}

tas_api_t api_init(ui_handler_t *ui_handler) {
  g_ui_handler_for_api = ui_handler;

  return (tas_api_t){
      .get_current_tick = api_get_current_tick,
      .get_track_count = api_get_track_count,
      .register_undo_command = api_register_undo_command,
      .do_create_snippet = api_do_create_snippet,
      .draw_line_world = api_draw_line_world,
      .log_info = api_log_info,
      .log_warning = api_log_warning,
      .log_error = api_log_error,
  };
}
