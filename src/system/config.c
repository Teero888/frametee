#include <renderer/graphics_backend.h>
#include "config.h"
#include "fs.h"
#include <logger/logger.h>
#include <system/include_cimgui.h>
#include <tomlc17.h>
#include <user_interface/keybinds.h>
#include <plugins/plugin_manager.h>
#include <user_interface/user_interface.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

static const char *LOG_SOURCE = "Config";

static void write_toml_string(FILE *fp, const char *text) {
  fputc('"', fp);
  for (const unsigned char *cursor = (const unsigned char *)(text ? text : ""); *cursor; ++cursor) {
    switch (*cursor) {
    case '"': fputs("\\\"", fp); break;
    case '\\': fputs("\\\\", fp); break;
    case '\b': fputs("\\b", fp); break;
    case '\t': fputs("\\t", fp); break;
    case '\n': fputs("\\n", fp); break;
    case '\f': fputs("\\f", fp); break;
    case '\r': fputs("\\r", fp); break;
    default:
      if (*cursor < 0x20) fprintf(fp, "\\u%04x", *cursor);
      else fputc(*cursor, fp);
      break;
    }
  }
  fputc('"', fp);
}

static void write_toml_key(FILE *fp, const char *key) { write_toml_string(fp, key); }

static bool write_toml_value(FILE *fp, toml_datum_t value) {
  switch (value.type) {
  case TOML_STRING: write_toml_string(fp, value.u.str.ptr); return true;
  case TOML_INT64: fprintf(fp, "%lld", (long long)value.u.int64); return true;
  case TOML_FP64: fprintf(fp, "%.17g", value.u.fp64); return true;
  case TOML_BOOLEAN: fputs(value.u.boolean ? "true" : "false", fp); return true;
  case TOML_ARRAY:
    fputc('[', fp);
    for (int i = 0; i < value.u.arr.size; ++i) {
      if (i > 0) fputs(", ", fp);
      if (!write_toml_value(fp, value.u.arr.elem[i])) return false;
    }
    fputc(']', fp);
    return true;
  default: return false;
  }
}

static bool toml_value_is_writable(toml_datum_t value) {
  if (value.type == TOML_STRING || value.type == TOML_INT64 || value.type == TOML_FP64 || value.type == TOML_BOOLEAN) return true;
  if (value.type != TOML_ARRAY) return false;
  for (int i = 0; i < value.u.arr.size; ++i)
    if (!toml_value_is_writable(value.u.arr.elem[i])) return false;
  return true;
}

static bool action_identifier_is_active(const ui_handler_t *ui, const char *identifier) {
  for (int i = 0; i < ui->keybinds.action_count; ++i)
    if (strcmp(ui->keybinds.action_infos[i].identifier, identifier) == 0) return true;
  return false;
}

static bool reserved_game_editor_key(const char *key) {
  return strcmp(key, "editor_camera_mode") == 0 || strcmp(key, "editor_linked_copy_input") == 0;
}

static void write_preserved_values(FILE *fp, toml_datum_t table, bool (*skip)(const char *key, void *user), void *user) {
  if (table.type != TOML_TABLE) return;
  for (int i = 0; i < table.u.tab.size; ++i) {
    const char *key = table.u.tab.key[i];
    toml_datum_t value = table.u.tab.value[i];
    if (!key || !toml_value_is_writable(value) || (skip && skip(key, user))) continue;
    write_toml_key(fp, key);
    fputs(" = ", fp);
    write_toml_value(fp, value);
    fputc('\n', fp);
  }
}

static bool skip_active_keybind(const char *key, void *user) {
  return action_identifier_is_active((const ui_handler_t *)user, key);
}

static bool skip_active_game_value(const char *key, void *user) {
  game_host_t *host = user;
  return reserved_game_editor_key(key) || gh_setting_find(host, key) >= 0;
}

static void get_config_path(char *buffer, size_t size) {
  char *config_home = NULL;
  char dir_path[1034]; // 1024 + 10 to allow the "frametee"
  dir_path[0] = '\0';

#ifdef _WIN32
  config_home = getenv("APPDATA");
  if (!config_home) config_home = getenv("USERPROFILE");
  if (config_home) {
    snprintf(dir_path, sizeof(dir_path), "%s%cframetee", config_home, PATH_SEP);
  }
#else
  config_home = getenv("XDG_CONFIG_HOME");
  if (config_home) {
    snprintf(dir_path, sizeof(dir_path), "%s%cframetee", config_home, PATH_SEP);
  } else {
    config_home = getenv("HOME");
    if (config_home) {
      char base_dir[1024];
      snprintf(base_dir, sizeof(base_dir), "%s%c.config", config_home, PATH_SEP);
      MKDIR(base_dir);
      snprintf(dir_path, sizeof(dir_path), "%s%cframetee", base_dir, PATH_SEP);
    }
  }
#endif

  if (dir_path[0] != '\0') {
    MKDIR(dir_path);
    snprintf(buffer, size, "%s%cconfig.toml", dir_path, PATH_SEP);
  } else {
    strncpy(buffer, "config.toml", size);
  }
}

void config_load(ui_handler_t *ui) {
  char config_path[1024];
  get_config_path(config_path, sizeof(config_path));

  game_host_t *active_host = &ui->gfx_handler->game_host;
  if (game_host_ready(active_host)) {
    ui->configured_camera_mode_id[0] = '\0';
    ui->configured_linked_copy_input = false;
    config_apply_game_editor_state(ui);
  }

  FILE *fp = fs_open(config_path, "r");
  if (!fp) {
    log_info(LOG_SOURCE, "No config file found at %s, using defaults.", config_path);
    return;
  }

  toml_result_t res = toml_parse_file(fp);
  fclose(fp);

  if (!res.ok) {
    log_error(LOG_SOURCE, "Failed to parse config file: %s", res.errmsg);
    toml_free(res);
    return;
  }

  toml_datum_t keybinds = toml_get(res.toptab, "keybinds");
  if (keybinds.type == TOML_TABLE) {
    for (int i = 0; i < ui->keybinds.action_count; ++i) {
      const char *id = ui->keybinds.action_infos[i].identifier;
      if (!*id) continue;

      toml_datum_t val = toml_get(keybinds, id);
      if (val.type == TOML_STRING) {
        keybinds_clear_action(&ui->keybinds, i);
        key_combo_t combo;
        if (keybinds_parse_combo(val.u.str.ptr, &combo)) keybinds_add(&ui->keybinds, i, combo);
      } else if (val.type == TOML_ARRAY) {
        keybinds_clear_action(&ui->keybinds, i);
        int count = val.u.arr.size;
        for (int j = 0; j < count; j++) {
          toml_datum_t elem = val.u.arr.elem[j];
          if (elem.type == TOML_STRING) {
            key_combo_t combo;
            if (keybinds_parse_combo(elem.u.str.ptr, &combo)) keybinds_add(&ui->keybinds, i, combo);
          }
        }
      }
    }
  }

  toml_datum_t mouse_settings = toml_get(res.toptab, "mouse");
  if (mouse_settings.type == TOML_TABLE) {
    toml_datum_t sens = toml_get(mouse_settings, "sensitivity");
    if (sens.type == TOML_FP64) {
      ui->mouse_sens = (float)sens.u.fp64;
    } else if (sens.type == TOML_INT64) {
      ui->mouse_sens = (float)sens.u.int64;
    }

    toml_datum_t dist = toml_get(mouse_settings, "max_distance");
    if (dist.type == TOML_FP64) {
      ui->mouse_max_distance = (float)dist.u.fp64;
    } else if (dist.type == TOML_INT64) {
      ui->mouse_max_distance = (float)dist.u.int64;
    }
  }

  toml_datum_t graphics_settings = toml_get(res.toptab, "graphics");
  if (graphics_settings.type == TOML_TABLE) {
    toml_datum_t vsync = toml_get(graphics_settings, "vsync");
    if (vsync.type == TOML_BOOLEAN) {
      ui->vsync = vsync.u.boolean;
    }

    toml_datum_t show_fps = toml_get(graphics_settings, "show_fps");
    if (show_fps.type == TOML_BOOLEAN) {
      ui->show_fps = show_fps.u.boolean;
    }

    toml_datum_t fps_limit = toml_get(graphics_settings, "fps_limit");
    if (fps_limit.type == TOML_INT64) {
      ui->fps_limit = (int)fps_limit.u.int64;
    }

    toml_datum_t lod_bias = toml_get(graphics_settings, "lod_bias");
    if (lod_bias.type == TOML_FP64) {
      ui->lod_bias = (float)lod_bias.u.fp64;
    }

    toml_datum_t bg_color = toml_get(graphics_settings, "bg_color");
    if (bg_color.type == TOML_ARRAY && bg_color.u.arr.size == 3) {
      for (int i = 0; i < 3; ++i) {
        toml_datum_t val = bg_color.u.arr.elem[i];
        if (val.type == TOML_FP64) {
          ui->bg_color[i] = (float)val.u.fp64;
        }
      }
    }

    // Presentation toggles that used to live here (prediction, particles, tee
    // rendering, cursor) belong to the game module and are stored with it.
    toml_datum_t render_level = toml_get(graphics_settings, "render_level");
    if (render_level.type == TOML_BOOLEAN) ui->render_level = render_level.u.boolean;

  }

  toml_datum_t projects_settings = toml_get(res.toptab, "projects");
  if (projects_settings.type == TOML_TABLE) {
    toml_datum_t recents = toml_get(projects_settings, "recent");
    if (recents.type == TOML_ARRAY) {
      ui->num_recent_projects = 0;
      for (int i = 0; i < recents.u.arr.size && ui->num_recent_projects < 10; ++i) {
        toml_datum_t val = recents.u.arr.elem[i];
        if (val.type == TOML_STRING) {
          strncpy(ui->recent_projects[ui->num_recent_projects], val.u.str.ptr, 1023);
          ui->recent_projects[ui->num_recent_projects][1023] = '\0';
          ui->num_recent_projects++;
        }
      }
    }
  }

  // Which game module to bring up. Read before the game layer starts, so it is
  // kept as a plain id rather than resolved here.
  toml_datum_t game_settings = toml_get(res.toptab, "game");
  if (game_settings.type == TOML_TABLE) {
    toml_datum_t id = toml_get(game_settings, "id");
    if (id.type == TOML_STRING && id.u.str.ptr) snprintf(ui->preferred_game_id, sizeof(ui->preferred_game_id), "%s", id.u.str.ptr);
  }

  // The active game's own settings live under its id, so switching games does
  // not overwrite the other one's preferences.
  game_host_t *host = &ui->gfx_handler->game_host;
  char game_table[64];
  snprintf(game_table, sizeof(game_table), "game.%s", game_host_active_id(host));
  toml_datum_t per_game = toml_seek(res.toptab, game_table);
  if (per_game.type == TOML_TABLE) {
    const unsigned setting_count = gh_setting_count(host);
    for (unsigned i = 0; i < setting_count; ++i) {
      const ft_setting_desc *desc = gh_setting_desc(host, i);
      if (!desc || !desc->id) continue;
      toml_datum_t stored = toml_get(per_game, desc->id);
      ft_value value = {.kind = desc->kind};
      switch (desc->kind) {
      case FT_VALUE_BOOL:
        if (stored.type != TOML_BOOLEAN) continue;
        value.as.b = stored.u.boolean;
        break;
      case FT_VALUE_INT:
        if (stored.type != TOML_INT64) continue;
        value.as.i = stored.u.int64;
        break;
      case FT_VALUE_FLOAT:
        if (stored.type == TOML_FP64) value.as.f = stored.u.fp64;
        else if (stored.type == TOML_INT64) value.as.f = (double)stored.u.int64;
        else continue;
        break;
      default: continue;
      }
      gh_setting_set(host, i, &value);
    }

    toml_datum_t camera_mode = toml_get(per_game, "editor_camera_mode");
    if (camera_mode.type == TOML_STRING)
      snprintf(ui->configured_camera_mode_id, sizeof(ui->configured_camera_mode_id), "%s", camera_mode.u.str.ptr);

    toml_datum_t linked_copy = toml_get(per_game, "editor_linked_copy_input");
    if (linked_copy.type == TOML_BOOLEAN) ui->configured_linked_copy_input = linked_copy.u.boolean;
  }
  config_apply_game_editor_state(ui);

  toml_datum_t auto_save = toml_get(res.toptab, "auto_save");
  if (auto_save.type == TOML_TABLE) {
    toml_datum_t enabled = toml_get(auto_save, "enabled");
    if (enabled.type == TOML_BOOLEAN) ui->auto_save_enabled = enabled.u.boolean;

    toml_datum_t interval = toml_get(auto_save, "interval_sec");
    if (interval.type == TOML_INT64) ui->auto_save_interval_sec = (int)interval.u.int64;
  }

  toml_free(res);
  log_info(LOG_SOURCE, "Config loaded successfully from %s.", config_path);
}

void config_apply_game_editor_state(ui_handler_t *ui) {
  if (!ui || !ui->gfx_handler || ui->timeline.ui != ui) return;
  game_host_t *host = &ui->gfx_handler->game_host;
  camera_t *camera = &ui->gfx_handler->renderer.camera;
  camera->mode = 0;
  if (ui->configured_camera_mode_id[0]) {
    for (unsigned i = 0; i < game_camera_mode_count(host); ++i) {
      const ft_camera_mode *mode = game_camera_mode(host, i);
      if (mode && mode->id && strcmp(mode->id, ui->configured_camera_mode_id) == 0) {
        camera->mode = i;
        break;
      }
    }
  }
  ui->timeline.linked_copy_input = game_has_cap(host, FT_CAP_LINKED_INPUTS) && ui->configured_linked_copy_input;
}

void config_save(ui_handler_t *ui) {
  char config_path[1024];
  get_config_path(config_path, sizeof(config_path));

  toml_result_t previous = {0};
  bool parsed_previous = false;
  bool have_previous = false;
  FILE *old = fs_open(config_path, "r");
  if (old) {
    previous = toml_parse_file(old);
    fclose(old);
    parsed_previous = true;
    have_previous = previous.ok;
  }

  char temporary_path[1060];
  if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", config_path) >= (int)sizeof(temporary_path)) {
    if (parsed_previous) toml_free(previous);
    return;
  }
  FILE *fp = fs_open(temporary_path, "w");
  if (!fp) {
    log_error(LOG_SOURCE, "Failed to open config file for writing at %s.", config_path);
    if (parsed_previous) toml_free(previous);
    return;
  }

  fprintf(fp, "# Frametee Configuration (https://github.com/Teero888/frametee)\n\n");
  fprintf(fp, "[keybinds]\n");

  // A keybind manager only contains the active game's controls. Carry every
  // other key through verbatim-as-data so saving one game cannot erase the
  // user's bindings for another.
  if (have_previous) {
    toml_datum_t old_keybinds = toml_get(previous.toptab, "keybinds");
    write_preserved_values(fp, old_keybinds, skip_active_keybind, ui);
  }

  keybind_manager_t defaults;
  keybinds_init(&defaults);
  keybinds_bind_game(&defaults, &ui->gfx_handler->game_host);

  for (int i = 0; i < ui->keybinds.action_count; ++i) {
    const char *id = ui->keybinds.action_infos[i].identifier;
    if (!*id) continue;

    int count = keybinds_get_count_for_action(&ui->keybinds, i);

    // Check if customized
    bool is_default = false;
    int def_count = keybinds_get_count_for_action(&defaults, i);
    if (count == def_count) {
      bool all_match = true;
      for (int k = 0; k < count; k++) {
        keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, i, k);
        keybind_entry_t *def = keybinds_get_binding_for_action(&defaults, i, k);
        if (!def || bind->combo.key != def->combo.key || bind->combo.ctrl != def->combo.ctrl ||
            bind->combo.alt != def->combo.alt || bind->combo.shift != def->combo.shift) {
          all_match = false;
          break;
        }
      }
      if (all_match) is_default = true;
    }

    if (!is_default) {
      if (count == 1) {
        keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, i, 0);
        const char *combo_str = keybind_get_combo_string(&bind->combo);
        write_toml_key(fp, id);
        fputs(" = ", fp);
        write_toml_string(fp, combo_str);
        fputc('\n', fp);
      } else if (count > 1) {
        write_toml_key(fp, id);
        fputs(" = [", fp);
        for (int k = 0; k < count; k++) {
          keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, i, k);
          const char *combo_str = keybind_get_combo_string(&bind->combo);
          write_toml_string(fp, combo_str);
          if (k < count - 1) fputs(", ", fp);
        }
        fprintf(fp, "]\n");
      } else {
        // Count 0, maybe explicitly unbound?
        // If default had > 0, we should save empty list to override default.
        // But tomlc17 writer might need care.
        if (def_count > 0) {
          write_toml_key(fp, id);
          fputs(" = []\n", fp);
        }
      }
    }
  }

  keybinds_cleanup(&defaults);

  fprintf(fp, "\n[mouse]\n");
  fprintf(fp, "sensitivity = %.2f\n", ui->mouse_sens);
  fprintf(fp, "max_distance = %.2f\n", ui->mouse_max_distance);

  fprintf(fp, "\n[graphics]\n");
  fprintf(fp, "vsync = %s\n", ui->vsync ? "true" : "false");
  fprintf(fp, "show_fps = %s\n", ui->show_fps ? "true" : "false");
  fprintf(fp, "fps_limit = %d\n", ui->fps_limit);
  fprintf(fp, "lod_bias = %.2f\n", ui->lod_bias);
  fprintf(fp, "bg_color = [%.3f, %.3f, %.3f]\n", ui->bg_color[0], ui->bg_color[1], ui->bg_color[2]);
  fprintf(fp, "render_level = %s\n", ui->render_level ? "true" : "false");

  fprintf(fp, "\n[projects]\n");
  fprintf(fp, "recent = [\n");
  for (int i = 0; i < ui->num_recent_projects; ++i) {
    fputs("  ", fp);
    write_toml_string(fp, ui->recent_projects[i]);
    fprintf(fp, "%s\n", (i < ui->num_recent_projects - 1) ? "," : "");
  }
  fprintf(fp, "]\n");

  fprintf(fp, "\n[game]\n");
  fputs("id = ", fp);
  write_toml_string(fp, ui->preferred_game_id);
  fputc('\n', fp);

  {
    game_host_t *host = &ui->gfx_handler->game_host;
    toml_datum_t old_game = {0};
    if (have_previous) old_game = toml_get(previous.toptab, "game");

    // Preserve every inactive game's complete settings table.
    if (old_game.type == TOML_TABLE) {
      for (int table_index = 0; table_index < old_game.u.tab.size; ++table_index) {
        const char *game_id = old_game.u.tab.key[table_index];
        toml_datum_t table = old_game.u.tab.value[table_index];
        if (!game_id || table.type != TOML_TABLE || strcmp(game_id, game_host_active_id(host)) == 0) continue;
        fputs("\n[game.", fp);
        write_toml_key(fp, game_id);
        fputs("]\n", fp);
        write_preserved_values(fp, table, NULL, NULL);
      }
    }

    const unsigned setting_count = gh_setting_count(host);
    fputs("\n[game.", fp);
    write_toml_key(fp, game_host_active_id(host));
    fputs("]\n", fp);
    toml_datum_t old_active = old_game.type == TOML_TABLE ? toml_get(old_game, game_host_active_id(host)) : (toml_datum_t){0};
    write_preserved_values(fp, old_active, skip_active_game_value, host);
    for (unsigned i = 0; i < setting_count; ++i) {
      const ft_setting_desc *desc = gh_setting_desc(host, i);
      ft_value value;
      if (!desc || !desc->id || !gh_setting_get(host, i, &value)) continue;
      if (value.kind != FT_VALUE_BOOL && value.kind != FT_VALUE_INT && value.kind != FT_VALUE_FLOAT) continue;
      write_toml_key(fp, desc->id);
      fputs(" = ", fp);
      switch (value.kind) {
      case FT_VALUE_BOOL: fputs(value.as.b ? "true\n" : "false\n", fp); break;
      case FT_VALUE_INT: fprintf(fp, "%lld\n", (long long)value.as.i); break;
      case FT_VALUE_FLOAT: fprintf(fp, "%.17g\n", value.as.f); break;
      default: break;
      }
    }
    const camera_t *camera = &ui->gfx_handler->renderer.camera;
    const ft_camera_mode *mode = game_camera_mode(host, camera->mode);
    fputs("editor_camera_mode = ", fp);
    write_toml_string(fp, mode && mode->id ? mode->id : "free");
    fputc('\n', fp);
    fprintf(fp, "editor_linked_copy_input = %s\n", ui->timeline.linked_copy_input ? "true" : "false");
  }

  fprintf(fp, "\n[auto_save]\n");
  fprintf(fp, "enabled = %s\n", ui->auto_save_enabled ? "true" : "false");
  fprintf(fp, "interval_sec = %d\n", ui->auto_save_interval_sec);

  fprintf(fp, "\n[plugins]\n");
  for (int i = 0; i < ui->plugin_manager.count; ++i) {
    write_toml_key(fp, ui->plugin_manager.plugins[i].key);
    fprintf(fp, " = %s\n", ui->plugin_manager.plugins[i].enabled ? "true" : "false");
  }

  const bool flushed = fflush(fp) == 0;
  const bool closed = fclose(fp) == 0;
  if (!flushed || !closed || !fs_replace(temporary_path, config_path)) {
    fs_remove(temporary_path);
    log_error(LOG_SOURCE, "Failed to finish writing config file at %s.", config_path);
  }
  if (parsed_previous) toml_free(previous);
  // log_info(LOG_SOURCE, "Config saved to %s.", config_path);
}
