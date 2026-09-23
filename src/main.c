#include "logger/logger.h"
#include "renderer/graphics_backend.h"
#include "renderer/renderer.h"
#include "scripting/script_engine.h"
#include "user_interface/user_interface.h"
#include <engine/engine_api.h>
#include <engine/prediction.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <system/fs.h>
#include <system/save.h>
#include <user_interface/starting_state.h>
#include <user_interface/timeline/timeline_commands.h>
#include <user_interface/timeline/timeline_model.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

bool g_is_headless = false;
bool g_list_games = false;
bool g_is_render_worker = false;
bool g_hide_render_window = false;

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

static void free_cli_args(const char **argv, char **copies, int count) {
  free((void *)argv);
  for (int i = 0; i < count; ++i) free(copies[i]);
}

// Walks the game through one frame of rendering: every visible world, in every
// pass, with the interpolation the playhead is currently between. The engine
// supplies the schedule and the draw services; the game supplies the picture.
static void render_game_passes(struct gfx_handler_t *handler, float intra, bool export_frame) {
  ui_handler_t *ui = &handler->user_interface;
  timeline_state_t *ts = &ui->timeline;

  ui_player_setups_prepare(ui);

  static const ft_render_pass passes[] = {FT_PASS_LEVEL_BACKGROUND, FT_PASS_ENTITIES, FT_PASS_LEVEL_FOREGROUND, FT_PASS_OVERLAY};
  const int selected_track = ts->selected_player_track_index;
  const int selected_group = model_track_group_index(ts, selected_track);
  const int selected_local = selected_group >= 0 ? model_group_local_track_index(ts, selected_track) : -1;

  ft_engine_state base_state;
  engine_api_fill_state(&base_state);

  typedef struct {
    const ft_world *previous;
    const ft_world *current;
  } cached_world_pair_t;

  static cached_world_pair_t *s_cached_pairs = NULL;
  static int s_cached_pairs_cap = 0;
  if (ts->group_count > s_cached_pairs_cap) {
    s_cached_pairs = realloc(s_cached_pairs, (size_t)ts->group_count * sizeof(cached_world_pair_t));
    s_cached_pairs_cap = ts->group_count;
  }

  for (size_t pass_index = 0; pass_index < sizeof(passes) / sizeof(passes[0]); ++pass_index) {
    // Level layers are shared by every simulation group and are drawn once.
    // Entities and overlays receive each world's adjacent snapshots.
    const bool level_pass = passes[pass_index] == FT_PASS_LEVEL_BACKGROUND || passes[pass_index] == FT_PASS_LEVEL_FOREGROUND;
    const bool per_world = !level_pass;
    const int world_count = per_world ? ts->group_count : 1;

    int first_visible_group = -1;
    int last_visible_group = -1;
    for (int g = 0; g < world_count; ++g) {
      if (!per_world || render_group_visible(ui, g)) {
        if (first_visible_group < 0) first_visible_group = g;
        last_visible_group = g;
      }
    }

    for (int group_index = 0; group_index < world_count; ++group_index) {
      if (per_world && !render_group_visible(ui, group_index)) continue;

      const ft_world *previous = NULL;
      const ft_world *current = NULL;
      if (per_world) {
        if (passes[pass_index] == FT_PASS_OVERLAY && s_cached_pairs) {
          previous = s_cached_pairs[group_index].previous;
          current = s_cached_pairs[group_index].current;
        } else {
          model_group_world_pair(ts, group_index, ts->current_tick, &previous, &current);
          if (s_cached_pairs) {
            s_cached_pairs[group_index].previous = previous;
            s_cached_pairs[group_index].current = current;
          }
        }
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
      frame.first_world = (group_index == first_visible_group);
      frame.last_world = (group_index == last_visible_group);
      frame.selected_player = (per_world && group_index == selected_group && render_layer_enabled(ui, RENDER_LAYER_SELECTION))
                                  ? selected_local
                                  : -1;
      if (per_world) {
        const float *color = ts->groups[group_index]->color;
        frame.accent = (ft_color){color[0], color[1], color[2], color[3]};
      } else {
        frame.accent = (ft_color){1.f, 1.f, 1.f, 1.f};
      }
      frame.player_setups = ui_player_setups(ui, group_index, &frame.player_setup_count);
      frame.state = base_state;

      gh_render(&handler->game_host, &frame);

      if (per_world && passes[pass_index] == FT_PASS_ENTITIES && render_layer_enabled(ui, RENDER_LAYER_PREDICTION))
        prediction_render_group(ui, group_index, previous, current, intra);
    }
  }

  // A marker for every start the user has taken over, so an override reads in
  // the level and not only in the panel that set it. The viewport and a video
  // each decide whether to show them; the camera's own path is editor-only.
  if (render_layer_enabled(ui, RENDER_LAYER_START_MARKERS)) starting_state_render_markers(ui, handler);
  if (!export_frame) camera_editor_render_gizmos(handler);
}

static void render_export_passes(struct gfx_handler_t *handler, float intra) {
  ui_handler_t *ui = &handler->user_interface;
  bool old_focus = ui->viewport_focused;
  ui->viewport_focused = false;
  on_camera_update(handler, false, intra);
  ui->viewport_focused = old_focus;
  camera_editor_apply_for_export(handler, ui->video_job.sample_time);
  render_game_passes(handler, intra, true);
}

static void setup_benchmark_groups(ui_handler_t *ui, int target_groups) {
  timeline_state_t *ts = &ui->timeline;
  log_info("Benchmark", "Setting up %d benchmark groups...", target_groups);
  double t0 = glfwGetTime();

  game_host_t *host = &ui->gfx_handler->game_host;
  size_t rec_size = game_input_size(host);
  if (rec_size == 0) rec_size = 64;
  int num_ticks = 6000;
  uint8_t *dummy_records = calloc((size_t)num_ticks, rec_size);

  for (int g = ts->group_count; g < target_groups; ++g) {
    char name[64];
    snprintf(name, sizeof(name), "Benchmark Group %d", g + 1);
    model_add_group(ts, name);
    model_set_active_group(ts, g);
    model_sync_tracks_to_world(ts, g);
    int track_idx = -1;
    undo_command_t *cmd1 = timeline_api_create_track(ui, NULL, &track_idx);
    if (cmd1) { if (cmd1->cleanup) cmd1->cleanup(cmd1); else free(cmd1); }
    if (track_idx >= 0) {
      int snip_id = -1;
      undo_command_t *cmd2 = timeline_api_create_snippet(ui, track_idx, 0, num_ticks, &snip_id);
      if (cmd2) { if (cmd2->cleanup) cmd2->cleanup(cmd2); else free(cmd2); }
      input_snippet_t *snip = model_find_snippet_by_id(ts, snip_id, NULL);
      if (snip) {
        input_record_t *win = snippet_window(snip);
        for (int t = 0; t < num_ticks && t < snip->input_count; ++t) {
          memcpy(win[t].bytes, dummy_records + (size_t)t * rec_size, rec_size);
        }
      }
    }
  }
  free(dummy_records);
  model_set_active_group(ts, 0);
  model_recalc_physics(ts, 0);
  double t1 = glfwGetTime();
  log_info("Benchmark", "Setup %d groups in %.2f ms", target_groups, (t1 - t0) * 1000.0);
}

int main(int argc, char **argv) {
  // Renders the level for a few frames, writes the viewport to a file and
  // exits. This is how a render gets checked without a person looking at it.
  const char *screenshot_path = NULL;
  const char *capture_view = NULL;
  int screenshot_frames = 60;
  int benchmark_groups = 0;
  // "x,y" in captured-image pixels. Reports the world-space ray under that
  // pixel, which is how a question about a render ("what is that grey?") turns
  // into a question about the level.
  const char *pick_pixel = NULL;
  // Opens a level straight away, skipping the start screen. The active game
  // decides what the string means, exactly as it does for the level a start
  // screen requests.
  const char *level_path = NULL;
  const char *project_path = NULL;
  const char *variant_id = NULL;
  const char *video_path = NULL;
  const char *video_progress_path = NULL;
  bool video_range_given = false;
  video_export_options_t video_cli;
  video_export_defaults(&video_cli);

#define MAX_CLI_PLUGINS 64
  const char *forced_plugins[MAX_CLI_PLUGINS];
  int num_forced_plugins = 0;
  char *plugin_arg_copies[MAX_CLI_PLUGINS];
  int num_plugin_arg_copies = 0;

  const char **plugin_argv = (const char **)malloc(sizeof(const char *) * argc);
  int plugin_argc = 0;

  bool show_help = false;
  bool check_finish = false;

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
        if (num_plugin_arg_copies < MAX_CLI_PLUGINS) {
          plugin_arg_copies[num_plugin_arg_copies++] = arg_copy;
        }
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
      if (num_plugin_arg_copies < MAX_CLI_PLUGINS) {
        plugin_arg_copies[num_plugin_arg_copies++] = arg_copy;
      }
      char *tok = strtok(arg_copy, ",");
      while (tok) {
        if (num_forced_plugins < MAX_CLI_PLUGINS) {
          forced_plugins[num_forced_plugins++] = tok;
        }
        tok = strtok(NULL, ",");
      }
    } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    } else if (strcmp(argv[i], "--render-video") == 0 && i + 1 < argc) {
      video_path = argv[++i];
    } else if (strcmp(argv[i], "--render-worker") == 0) {
      g_is_render_worker = true;
    } else if (strcmp(argv[i], "--render-progress") == 0 && i + 1 < argc) {
      video_progress_path = argv[++i];
    } else if (strcmp(argv[i], "--render-range") == 0 && i + 1 < argc) {
      video_range_given = sscanf(argv[++i], "%lf:%lf", &video_cli.start_time, &video_cli.end_time) == 2;
    } else if (strcmp(argv[i], "--render-size") == 0 && i + 1 < argc) {
      sscanf(argv[++i], "%dx%d", &video_cli.width, &video_cli.height);
    } else if (strcmp(argv[i], "--render-fps") == 0 && i + 1 < argc) {
      const char *rate = argv[++i];
      if (sscanf(rate, "%d/%d", &video_cli.fps_num, &video_cli.fps_den) != 2) {
        video_cli.fps_num = atoi(rate); video_cli.fps_den = 1;
      }
    } else if (strcmp(argv[i], "--render-codec") == 0 && i + 1 < argc) {
      const char *codec = argv[++i];
      video_cli.codec = strcmp(codec, "hevc") == 0 ? 1 : strcmp(codec, "av1") == 0 ? 2 : 0;
    } else if (strcmp(argv[i], "--render-crf") == 0 && i + 1 < argc) {
      video_cli.quality_mode = 0; video_cli.quality = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--render-bitrate") == 0 && i + 1 < argc) {
      video_cli.quality_mode = 1; video_cli.bitrate_kbps = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--render-depth") == 0 && i + 1 < argc) {
      video_cli.bit_depth = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--render-preset") == 0 && i + 1 < argc) {
      const char *preset = argv[++i];
      video_cli.preset = strcmp(preset, "fast") == 0 ? 0 : strcmp(preset, "slow") == 0 ? 2 :
                         strcmp(preset, "medium") == 0 ? 1 : -1;
    } else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
      capture_view = argv[++i];
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      screenshot_frames = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--pick") == 0 && i + 1 < argc) {
      pick_pixel = argv[++i];
    } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      set_environment_variable("FRAMETEE_WINDOW_SIZE", argv[i + 1]);
      set_environment_variable("FRAMETEE_VIEWPORT_SIZE", argv[++i]);
    } else if (strcmp(argv[i], "--benchmark-groups") == 0 && i + 1 < argc) {
      benchmark_groups = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
      project_path = argv[++i];
    } else if (strncmp(argv[i], "--project=", 10) == 0) {
      project_path = argv[i] + 10;
    } else if (strcmp(argv[i], "--check-finish") == 0 || strcmp(argv[i], "--checkfinish") == 0 ||
               strcmp(argv[i], "checkfinish") == 0) {
      check_finish = true;
      g_is_headless = true;
    } else if (strcmp(argv[i], "--list-games") == 0) {
      g_list_games = true;
      g_is_headless = true;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      show_help = true;
    } else {
      plugin_argv[plugin_argc++] = argv[i];
    }
  }

  // CLI export has no editor to show. It still uses a GLFW Vulkan surface on
  // platforms without headless-surface support, but the window stays hidden.
  g_hide_render_window = video_path != NULL;
  if (video_path) g_is_headless = false;

  logger_init();
  if (show_help) {
    g_is_headless = true;
    logger_set_quiet(true);
  }

  if (g_list_games) {
    free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
    game_host_t game_host;
    game_host_init(&game_host, NULL);
    if (game_host_discover(&game_host, "games") == 0) {
      char exe_dir[1024];
      if (fs_get_executable_dir(exe_dir, sizeof(exe_dir))) {
        char games_dir[1024];
        snprintf(games_dir, sizeof(games_dir), "%s/games", exe_dir);
        game_host_discover(&game_host, games_dir);
      }
    }
    game_host_print_listing(&game_host);
    game_host_shutdown(&game_host);
    return 0;
  }

  static struct gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0) {
    free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
    return 1;
  }

  if (show_help) {
    printf("Usage: frametee [options] [plugin-options]\n\n"
           "Interactive options:\n"
           "  --game <id>             Select game on startup (e.g. tmnf, ddnet)\n"
           "  --level <path>          Open level immediately\n"
           "  --list-games            List discovered game modules and exit\n"
           "  --plugin <name...>      Activate one or more plugins for this session\n\n"
           "Capture options:\n"
           "  --screenshot <path>     Save viewport as PPM and exit\n"
           "  --size <width>x<height> Set capture dimensions\n"
           "  --view <x,y,width>      Pin the 2D world view for capture\n"
           "  --frames <count>        Frames before capture (default: 60)\n\n"
           "Video export options (video only):\n"
           "  --render-video <path>   Render an MP4 and exit\n"
           "  --render-range <a:b>    Inclusive camera time range in seconds\n"
           "  --render-size <WxH>     Output dimensions (even numbers)\n"
           "  --render-fps <N[/D]>    Output frame rate\n"
           "  --render-codec <name>   h264, hevc, or av1\n"
           "  --render-crf <value>    Constant quality, 0..51\n"
           "  --render-bitrate <kbps> Target bitrate\n"
           "  --render-preset <name>  fast, medium, or slow compression\n"
           "  --render-depth <8|10>   Color depth\n\n"
           "Headless options:\n"
           "  --headless              Run without window or graphics\n"
           "  --game <id>             Game module to use (e.g. tmnf, ddnet)\n"
           "  --level <path>          Level file to load\n"
           "  --project <path>        TAS project file to load\n"
           "  --check-finish          Check if the run finishes (exit 0 on finish, 1 otherwise)\n"
           "  --variant <id>          Ruleset variant (DDNet: ddrace, race, fastcap)\n"
           "  --plugin <name...>      Activate one or more plugins for this session\n"
           "  --help, -h              Show this help message\n");

    if (num_forced_plugins > 0) {
      plugin_manager_print_help(&handler.user_interface.plugin_manager, num_forced_plugins, forced_plugins);
    } else {
      plugin_manager_print_available(&handler.user_interface.plugin_manager);
    }
    printf("\n");
    free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
    gfx_cleanup(&handler);
    return 0;
  }

  script_engine_init(&handler.user_interface, &handler.user_interface.plugin_api);

  if (g_is_headless) {
    if (project_path) {
      if (!load_project(&handler.user_interface, project_path)) {
        log_error("Main", "Could not load project '%s'", project_path);
        free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
        gfx_cleanup(&handler);
        return 1;
      }
      handler.user_interface.show_splash = false;
    } else if (level_path) {
      if (g_forced_game_id) {
        int game_idx = game_host_find_id(&handler.game_host, g_forced_game_id);
        if (game_idx >= 0) {
          gfx_activate_game(&handler, game_idx);
        } else {
          log_error("Main", "Game module '%s' not found.", g_forced_game_id);
          free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
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
        free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
        gfx_cleanup(&handler);
        return 1;
      }

      model_add_new_track(&handler.user_interface.timeline, 1);
    } else {
      log_error("Main", "--headless requires --level <path> or --project <path>");
      free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
      gfx_cleanup(&handler);
      return 1;
    }

    for (int p = 0; p < num_forced_plugins; ++p) {
      if (!plugin_manager_activate(&handler.user_interface.plugin_manager, forced_plugins[p])) {
        log_error("Main", "Failed to activate plugin '%s'.", forced_plugins[p]);
        free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
        gfx_cleanup(&handler);
        return 1;
      }
    }

    if (check_finish) {
      timeline_state_t *timeline = &handler.user_interface.timeline;
      int max_tick = model_get_max_timeline_tick(timeline);
      bool finished = false;
      int finish_tick = -1;
      int group_index = timeline->active_group_index;
      if (group_index < 0 || group_index >= timeline->group_count) group_index = 0;
      for (int tick = 0; tick <= max_tick; ++tick) {
        const ft_world *world = model_group_world_at_tick(timeline, group_index, tick);
        if (!world) continue;
        ft_player_view view = {.struct_size = sizeof(view)};
        if (gh_world_player_view(&handler.game_host, world, 0, &view)) {
          if (view.flags & FT_PLAYER_FINISHED) {
            finished = true;
            finish_tick = tick;
            break;
          }
        }
      }
      free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
      gfx_cleanup(&handler);
      if (finished) {
        printf("Run finished at tick %d\n", finish_tick);
        return 0;
      } else {
        printf("Run did not finish (simulated to tick %d)\n", max_tick);
        return 1;
      }
    }

    int res = plugin_manager_run_cli(&handler.user_interface.plugin_manager, plugin_argc, plugin_argv);
    free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
    gfx_cleanup(&handler);
    return res;
  }

  for (int p = 0; p < num_forced_plugins; ++p) {
    plugin_manager_activate(&handler.user_interface.plugin_manager, forced_plugins[p]);
  }

  if (project_path) {
    if (load_project(&handler.user_interface, project_path)) {
      handler.user_interface.show_splash = false;
      if (benchmark_groups > 0) {
        setup_benchmark_groups(&handler.user_interface, benchmark_groups);
      }
    } else {
      log_error("Main", "Could not load project '%s'", project_path);
    }
  } else if (level_path) {
    on_level_load_path(&handler, level_path);
    if (handler.level) {
      handler.user_interface.show_splash = false;
      if (benchmark_groups > 0) {
        setup_benchmark_groups(&handler.user_interface, benchmark_groups);
      }
    } else
      log_error("Main", "Could not open level '%s'", level_path);
  }

  if (video_path) {
    if (!video_range_given)
      camera_editor_export_range(&handler.user_interface, &video_cli.start_time, &video_cli.end_time);
    video_export_job_t *job = &handler.user_interface.video_job;
    // This process draws only video: it holds the project's video settings.
    render_apply(&handler.user_interface, RENDER_TARGET_VIDEO);
    if (!video_export_start_inline(&handler, job, &video_cli, video_path, video_progress_path)) {
      log_error("Main", "Cannot start video export: %s", job->status);
      free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
      gfx_cleanup(&handler);
      return 1;
    }
    while (job->active && !glfwWindowShouldClose(handler.window))
      video_export_step(&handler, job, render_export_passes);
    const bool succeeded = job->finished;
    if (!succeeded && job->active) video_export_cancel(&handler, job);
    log_info("Main", "%s", job->status);
    free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
    gfx_cleanup(&handler);
    if (g_is_render_worker) {
      if (project_path) fs_remove(project_path);
      if (video_progress_path) fs_remove(video_progress_path);
    }
    return succeeded ? 0 : 1;
  }

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

    // A project the menus asked for is opened here, before the frame draws
    // anything: it can replace the level, the timeline and the active game,
    // and everything below reads all three. This also precedes the menu and
    // viewport layout in gfx_begin_frame, which may reference game resources.
    ui_run_pending_project_switch(&handler.user_interface);

    int frame_result = gfx_begin_frame(&handler);
    if (frame_result == FRAME_EXIT) break;
    if (frame_result == FRAME_SKIP) continue;

    timeline_state_t *timeline = &handler.user_interface.timeline;
    float intra = 1.f;
    if ((timeline->is_playing || timeline->is_reversing) && timeline->playback_speed > 0) {
      const float speed_scale = timeline->is_reversing ? 2.0f : 1.0f;
      intra = fminf((igGetTime() - timeline->last_update_time) /
                        (1.f / (timeline->playback_speed * speed_scale)),
                    1.f);
      if (timeline->is_reversing) intra = 1.f - intra;
    }

    // The Camera tab shows the game at a fractional tick of its own, taken from
    // the playhead as it is now: scrubbing moved it after the last update.
    if (camera_editor_owns_clock(&handler.user_interface)) {
      camera_editor_sync_game(&handler.user_interface);
      intra = handler.user_interface.camera_editor.game_intra;
    }

    on_camera_update(&handler, handler.user_interface.viewport_hovered, intra);
    camera_editor_viewport_update(&handler);
    // Pin a 2D capture to a repeatable world view, independently of window
    // layout and mouse events. Width is in the game's world units.
    if (screenshot_path && capture_view && handler.level && !game_is_3d(&handler.game_host)) {
      float x, y, width;
      if (sscanf(capture_view, "%f,%f,%f", &x, &y, &width) == 3 &&
          isfinite(x) && isfinite(y) && isfinite(width) && width > 0.f) {
        handler.renderer.camera.pos[0] = x / handler.world_width;
        handler.renderer.camera.pos[1] = y / handler.world_height;
        handler.renderer.camera.zoom = 2.f * handler.world_width /
                                       (width * fmaxf(handler.world_width, handler.world_height) * 0.001f);
        handler.renderer.camera.zoom_wanted = handler.renderer.camera.zoom;
      }
    }

    // Everything in the viewport is drawn by the active game. The engine
    // decides the passes and their order; what happens inside each one is the
    // game's business entirely. This runs under the start screen too: nothing
    // drawn later in the frame can replace the game whose resources these
    // commands reference, because every project switch waits for the call
    // above.
    double t_render_start = glfwGetTime();
    if (handler.level != NULL) {
      render_game_passes(&handler, intra, false);
    }
    double t_render_done = glfwGetTime();
    renderer_flush_queue(&handler, handler.current_frame_command_buffer);
    double t_flush_done = glfwGetTime();

    ui_check_auto_save(&handler.user_interface);
    ui_render(&handler.user_interface);
    double t_ui_done = glfwGetTime();

    if (benchmark_groups > 0 || project_path) {
      double total_ms = (t_ui_done - t_render_start) * 1000.0;
      log_info("Benchmark", "Frame %d: passes=%.2f ms, flush=%.2f ms, ui=%.2f ms, total=%.2f ms (%.1f FPS, %d groups)",
               screenshot_frames, (t_render_done - t_render_start) * 1000.0,
               (t_flush_done - t_render_done) * 1000.0,
               (t_ui_done - t_flush_done) * 1000.0,
               total_ms, total_ms > 0.0 ? 1000.0 / total_ms : 999.0, handler.user_interface.timeline.group_count);
    }

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

    gfx_end_frame(&handler);

    if (handler.user_interface.video_job.active)
      video_export_step(&handler, &handler.user_interface.video_job, NULL);

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
      } else
        log_error("Main", "Could not capture the viewport to '%s'", screenshot_path);
      break;
    }
  }

  free_cli_args(plugin_argv, plugin_arg_copies, num_plugin_arg_copies);
  gfx_cleanup(&handler);
  return 0;
}
