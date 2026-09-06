#include "logger/logger.h"
#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include "user_interface/user_interface.h"
#include <engine/engine_api.h>
#include <engine/prediction.h>
#include <user_interface/starting_state.h>
#include <user_interface/timeline/timeline_model.h>
#include "scripting/script_engine.h"
#include <math.h>
#include <user_interface/timeline/timeline_model.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

bool g_is_headless = false;
bool g_list_games = false;

// Set from --game, consumed by the game layer during startup. A command line
// choice outranks the config so a project can be opened under a specific game
// without editing settings first.
const char *g_forced_game_id = NULL;

static int set_environment_variable(const char *name, const char *value) {
#ifdef _WIN32
  return SetEnvironmentVariableA(name, value) ? 0 : -1;
#else
  return setenv(name, value, 1);
#endif
}


// Walks the game through one frame of rendering: every visible world, in every
// pass, with the interpolation the playhead is currently between. The engine
// supplies the schedule and the draw services; the game supplies the picture.
static void render_game_passes(struct gfx_handler_t *handler, float intra) {
  ui_handler_t *ui = &handler->user_interface;
  timeline_state_t *ts = &ui->timeline;

  static const ft_render_pass passes[] = {FT_PASS_LEVEL_BACKGROUND, FT_PASS_ENTITIES, FT_PASS_LEVEL_FOREGROUND, FT_PASS_OVERLAY};
  const int selected_track = ts->selected_player_track_index;

  for (size_t pass_index = 0; pass_index < sizeof(passes) / sizeof(passes[0]); ++pass_index) {
    // Level layers are shared by every simulation group and are drawn once.
    // Entities and overlays receive each world's adjacent snapshots.
    const bool level_pass = passes[pass_index] == FT_PASS_LEVEL_BACKGROUND || passes[pass_index] == FT_PASS_LEVEL_FOREGROUND;
    const bool per_world = !level_pass;
    const int world_count = per_world ? ts->group_count : 1;

    for (int group_index = 0; group_index < world_count; ++group_index) {
      if (per_world && !ts->groups[group_index]->visible) continue;

      const ft_world *previous = NULL;
      const ft_world *current = NULL;
      if (per_world) {
        model_group_world_pair(ts, group_index, ts->current_tick, &previous, &current);
      } else if (ts->active_group_index >= 0 && ts->active_group_index < ts->group_count) {
        model_group_world_pair(ts, ts->active_group_index, ts->current_tick, &previous, &current);
      }

      ft_render_frame frame = {0};
      frame.struct_size = sizeof(frame);
      frame.pass = passes[pass_index];
      frame.level = handler->level;
      frame.world = current;
      frame.previous_world = previous;
      frame.alpha = intra;
      // A group's snapshots use its local simulation clock. Group 0 happens
      // to have no start offset, but passing the shared timeline tick ages
      // short-lived per-world effects (notably DDNet explosions) by every
      // other group's offset before they are rendered.
      frame.tick = current ? gh_world_tick(&handler->game_host, current) : ts->current_tick;
      frame.opacity = 1.f;
      frame.world_index = per_world ? group_index : -1;
      frame.world_count = ts->group_count;
      frame.active = !per_world || group_index == ts->active_group_index;
      frame.selected_player = per_world && model_track_group_index(ts, selected_track) == group_index
                                  ? model_group_local_track_index(ts, selected_track)
                                  : -1;
      if (per_world) {
        const float *color = ts->groups[group_index]->color;
        frame.accent = (ft_color){color[0], color[1], color[2], color[3]};
      } else {
        frame.accent = (ft_color){1.f, 1.f, 1.f, 1.f};
      }
      frame.player_setups = ui_player_setups(ui, group_index, &frame.player_setup_count);
      engine_api_fill_state(&frame.state);

      gh_render(&handler->game_host, &frame);
      if (per_world && passes[pass_index] == FT_PASS_ENTITIES)
        prediction_render_group(ui, group_index, previous, current, intra);
    }
  }

  // A marker for every start the user has taken over, so an override reads in
  // the level and not only in the panel that set it.
  starting_state_render_markers(ui, handler);
}

int main(int argc, char **argv) {
  // Renders the level for a few frames, writes the viewport to a file and
  // exits. This is how a render gets checked without a person looking at it.
  const char *screenshot_path = NULL;
  int screenshot_frames = 60;
  // "x,y" in captured-image pixels. Reports the world-space ray under that
  // pixel, which is how a question about a render ("what is that grey?") turns
  // into a question about the level.
  const char *pick_pixel = NULL;
  // Opens a level straight away, skipping the start screen. The active game
  // decides what the string means, exactly as it does for the level a start
  // screen requests.
  const char *level_path = NULL;
  const char *variant_id = NULL;

#define MAX_CLI_PLUGINS 64
  const char *forced_plugins[MAX_CLI_PLUGINS];
  int num_forced_plugins = 0;

  const char **plugin_argv = (const char **)malloc(sizeof(const char *) * argc);
  int plugin_argc = 0;

  bool show_help = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0) {
      g_is_headless = true;
    } else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
      g_forced_game_id = argv[++i];
    } else if (strncmp(argv[i], "--game=", 7) == 0) {
      g_forced_game_id = argv[i] + 7;
    } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
      level_path = argv[++i];
    } else if (strncmp(argv[i], "--level=", 8) == 0) {
      level_path = argv[i] + 8;
    } else if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
      variant_id = argv[++i];
    } else if (strncmp(argv[i], "--variant=", 10) == 0) {
      variant_id = argv[i] + 10;
    } else if (strcmp(argv[i], "--plugin") == 0 || strcmp(argv[i], "--plugins") == 0) {
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        char *arg_copy = strdup(argv[++i]);
        char *tok = strtok(arg_copy, ",");
        while (tok) {
          if (num_forced_plugins < MAX_CLI_PLUGINS) {
            forced_plugins[num_forced_plugins++] = tok;
          }
          tok = strtok(NULL, ",");
        }
      }
    } else if (strncmp(argv[i], "--plugin=", 9) == 0) {
      char *arg_copy = strdup(argv[i] + 9);
      char *tok = strtok(arg_copy, ",");
      while (tok) {
        if (num_forced_plugins < MAX_CLI_PLUGINS) {
          forced_plugins[num_forced_plugins++] = tok;
        }
        tok = strtok(NULL, ",");
      }
    } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      screenshot_frames = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--pick") == 0 && i + 1 < argc) {
      pick_pixel = argv[++i];
    } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      set_environment_variable("FRAMETEE_WINDOW_SIZE", argv[i + 1]);
      set_environment_variable("FRAMETEE_VIEWPORT_SIZE", argv[++i]);
    } else if (strcmp(argv[i], "--list-games") == 0) {
      g_list_games = true;
      g_is_headless = true;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      show_help = true;
    } else {
      plugin_argv[plugin_argc++] = argv[i];
    }
  }

  logger_init();
  if (show_help) {
    g_is_headless = true;
    logger_set_quiet(true);
  }

  if (g_list_games) {
    free(plugin_argv);
    game_host_t game_host;
    game_host_init(&game_host, NULL);
    game_host_discover(&game_host, "games");
    game_host_print_listing(&game_host);
    game_host_shutdown(&game_host);
    return 0;
  }

  static struct gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0) {
    free(plugin_argv);
    return 1;
  }

  if (show_help) {
    printf("Usage: frametee [options] [plugin-options]\n\n"
           "Interactive options:\n"
           "  --game <id>             Select game on startup (e.g. tmnf, ddnet)\n"
           "  --level <path>          Open level immediately\n"
           "  --list-games            List discovered game modules and exit\n"
           "  --plugin <name...>      Activate one or more plugins for this session\n\n"
           "Headless options:\n"
           "  --headless              Run without window or graphics\n"
           "  --game <id>             Game module to use (e.g. tmnf, ddnet)\n"
           "  --level <path>          Level file to load\n"
           "  --variant <id>          Ruleset variant (DDNet: ddrace, race, fastcap)\n"
           "  --plugin <name...>      Activate one or more plugins for this session\n"
           "  --help, -h              Show this help message\n");

    if (num_forced_plugins > 0) {
      plugin_manager_print_help(&handler.user_interface.plugin_manager, num_forced_plugins, forced_plugins);
    } else {
      plugin_manager_print_available(&handler.user_interface.plugin_manager);
    }
    printf("\n");
    free(plugin_argv);
    gfx_cleanup(&handler);
    return 0;
  }

  script_engine_init(&handler.user_interface, &handler.user_interface.plugin_api);

  if (g_is_headless) {
    if (!level_path) {
      log_error("Main", "--headless requires --level <path>");
      free(plugin_argv);
      gfx_cleanup(&handler);
      return 1;
    }

    if (g_forced_game_id) {
      int game_idx = game_host_find_id(&handler.game_host, g_forced_game_id);
      if (game_idx >= 0) {
        gfx_activate_game(&handler, game_idx);
      } else {
        log_error("Main", "Game module '%s' not found.", g_forced_game_id);
        free(plugin_argv);
        gfx_cleanup(&handler);
        return 1;
      }
    }

    if (variant_id) {
      game_host_set_variant(&handler.game_host, variant_id);
    }

    on_level_load_path(&handler, level_path);
    if (!handler.level) {
      log_error("Main", "Failed to load level '%s'.", level_path);
      free(plugin_argv);
      gfx_cleanup(&handler);
      return 1;
    }

    model_add_new_track(&handler.user_interface.timeline, 1);

    for (int p = 0; p < num_forced_plugins; ++p) {
      if (!plugin_manager_activate(&handler.user_interface.plugin_manager, forced_plugins[p])) {
        log_error("Main", "Failed to activate plugin '%s'.", forced_plugins[p]);
        free(plugin_argv);
        gfx_cleanup(&handler);
        return 1;
      }
    }

    int res = plugin_manager_run_cli(&handler.user_interface.plugin_manager, plugin_argc, plugin_argv);
    free(plugin_argv);
    gfx_cleanup(&handler);
    return res;
  }

  for (int p = 0; p < num_forced_plugins; ++p) {
    plugin_manager_activate(&handler.user_interface.plugin_manager, forced_plugins[p]);
  }

  if (level_path) {
    on_level_load_path(&handler, level_path);
    if (handler.level)
      handler.user_interface.show_splash = false;
    else
      log_error("Main", "Could not open level '%s'", level_path);
  }

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

    // A project the menus asked for is opened here, before the frame draws
    // anything: it can replace the level, the timeline and the active game,
    // and everything below reads all three.
    ui_run_pending_project_switch(&handler.user_interface);

    timeline_state_t *timeline = &handler.user_interface.timeline;
    float intra = 1.f;
    if ((timeline->is_playing || timeline->is_reversing) && timeline->playback_speed > 0) {
      const float speed_scale = timeline->is_reversing ? 2.0f : 1.0f;
      intra = fminf((igGetTime() - timeline->last_update_time) /
                        (1.f / (timeline->playback_speed * speed_scale)),
                    1.f);
      if (timeline->is_reversing) intra = 1.f - intra;
    }

    on_camera_update(&handler, viewport_hovered, intra);

    // Everything in the viewport is drawn by the active game. The engine
    // decides the passes and their order; what happens inside each one is the
    // game's business entirely. This runs under the start screen too: nothing
    // drawn later in the frame can replace the game whose resources these
    // commands reference, because every project switch waits for the call
    // above.
    if (handler.level != NULL) {
      render_game_passes(&handler, intra);
    }
    renderer_flush_queue(&handler, handler.current_frame_command_buffer);

    ui_check_auto_save(&handler.user_interface);
    ui_render(&handler.user_interface);

    // Mouse locking logic for recording
    ImGuiIO *io = igGetIO_Nil();
    // Cursor-driven games such as DDNet record relative mouse motion and need
    // the old capture behaviour. Keyboard-only games such as TMNF do not: a
    // normal cursor lets their recording continue while the user works in the
    // rest of the editor.
    const bool capture_recording_cursor =
        handler.user_interface.timeline.recording && engine_input_cursor_field() >= 0;
    if (capture_recording_cursor) {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
      glfwSetInputMode(handler.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    viewport_hovered = gfx_end_frame(&handler);

    if (screenshot_path != NULL && --screenshot_frames <= 0) {
      // A few frames in, so the level has settled and the viewport has been
      // sized by the layout rather than by its initial guess.
      if (renderer_capture_offscreen_ppm(&handler, screenshot_path) == 0) {
        log_info("Main", "Wrote %s", screenshot_path);
        // Where a pixel of that capture lands in the world, so a render can be
        // interrogated ("what is that grey?") instead of guessed at.
        if (pick_pixel) {
          const char *pick = pick_pixel;
          float sx = 0.f, sy = 0.f;
          if (sscanf(pick, "%f,%f", &sx, &sy) == 2) {
            vec3 origin, dir;
            if (screen_ray3(&handler, sx, sy, origin, dir)) {
              log_info("Main", "pick ray origin=(%.2f %.2f %.2f) dir=(%.4f %.4f %.4f)", (double)origin[0],
                       (double)origin[1], (double)origin[2], (double)dir[0], (double)dir[1], (double)dir[2]);
            }
          }
        }
      }
      else
        log_error("Main", "Could not capture the viewport to '%s'", screenshot_path);
      break;
    }
  }

  gfx_cleanup(&handler);
  return 0;
}
