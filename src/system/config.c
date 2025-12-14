#include "config.h"
#include <logger/logger.h>
#include <system/include_cimgui.h>
#include <tomlc17.h>
#include <user_interface/keybinds.h>

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

static void get_config_path(char *buffer, size_t size) {
  char *config_home = NULL;
  char dir_path[1034];// 1024 + 10 to allow the "frametee"
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

static ImGuiKey key_from_name(const char *name) {
  // Linear search through named keys. Not efficient but runs only on config load.
  for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
    const char *key_name = igGetKeyName(key);
    if (key_name && strcmp(key_name, name) == 0) {
      return key;
    }
  }
  // Fallback for number keys which might not be in NamedKey range depending on ImGui version logic,
  // but usually they are.
  // Also check standard keys if needed, but igGetKeyName handles them.
  return ImGuiKey_None;
}

static void parse_keybind_string(const char *str, key_combo_t *out) {
  out->key = ImGuiKey_None;
  out->ctrl = false;
  out->alt = false;
  out->shift = false;

  char buffer[128];
  strncpy(buffer, str, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *token = strtok(buffer, "+");
  while (token) {
    if (strcmp(token, "Ctrl") == 0) {
      out->ctrl = true;
    } else if (strcmp(token, "Alt") == 0) {
      out->alt = true;
    } else if (strcmp(token, "Shift") == 0) {
      out->shift = true;
    } else {
      // Assume it's the key
      out->key = key_from_name(token);
    }
    token = strtok(NULL, "+");
  }
}

void config_load(struct ui_handler *ui) {
  char config_path[1024];
  get_config_path(config_path, sizeof(config_path));

  FILE *fp = fopen(config_path, "r");
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
    for (int i = 0; i < ACTION_COUNT; ++i) {
      const char *id = ui->keybinds.action_infos[i].identifier;
      if (!id) continue;

      toml_datum_t val = toml_get(keybinds, id);
      if (val.type == TOML_STRING) {
        keybinds_clear_action(&ui->keybinds, i);
        key_combo_t combo;
        parse_keybind_string(val.u.str.ptr, &combo);
        keybinds_add(&ui->keybinds, i, combo);
      } else if (val.type == TOML_ARRAY) {
        keybinds_clear_action(&ui->keybinds, i);
        int count = val.u.arr.size;
        for (int j = 0; j < count; j++) {
            toml_datum_t elem = val.u.arr.elem[j];
            if (elem.type == TOML_STRING) {
                key_combo_t combo;
                parse_keybind_string(elem.u.str.ptr, &combo);
                keybinds_add(&ui->keybinds, i, combo);
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
  }

  toml_free(res);
  log_info(LOG_SOURCE, "Config loaded successfully from %s.", config_path);
}

void config_save(struct ui_handler *ui) {
  char config_path[1024];
  get_config_path(config_path, sizeof(config_path));

  FILE *fp = fopen(config_path, "w");
  if (!fp) {
    log_error(LOG_SOURCE, "Failed to open config file for writing at %s.", config_path);
    return;
  }

  fprintf(fp, "# Frametee Configuration (https://github.com/Teero888/frametee)\n\n");
  fprintf(fp, "[keybinds]\n");

  keybind_manager_t defaults;
  keybinds_init(&defaults);

  for (int i = 0; i < ACTION_COUNT; ++i) {
    const char *id = ui->keybinds.action_infos[i].identifier;
    if (!id) continue;

    int count = keybinds_get_count_for_action(&ui->keybinds, i);
    
    // Check if customized
    bool is_default = false;
    int def_count = keybinds_get_count_for_action(&defaults, i);
    if (count == def_count) {
        bool all_match = true;
        // Simple comparison: check if all bindings match exactly in order (not perfect but acceptable)
        // Or check if every binding in UI exists in default.
        // Given that init adds them in order, if user hasn't changed, order matches.
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
            fprintf(fp, "%s = \"%s\"\n", id, combo_str);
        } else if (count > 1) {
            fprintf(fp, "%s = [", id);
            for (int k = 0; k < count; k++) {
                keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, i, k);
                const char *combo_str = keybind_get_combo_string(&bind->combo);
                fprintf(fp, "\"%s\"%s", combo_str, (k < count - 1) ? ", " : "");
            }
            fprintf(fp, "]\n");
        } else {
            // Count 0, maybe explicitly unbound? 
            // If default had > 0, we should save empty list to override default.
            // But tomlc17 writer might need care.
            if (def_count > 0) {
                fprintf(fp, "%s = []\n", id);
            }
        }
    }
  }
  
  if (defaults.bindings) free(defaults.bindings);

  fprintf(fp, "\n[mouse]\n");
  fprintf(fp, "sensitivity = %.2f\n", ui->mouse_sens);
  fprintf(fp, "max_distance = %.2f\n", ui->mouse_max_distance);

  fprintf(fp, "\n[graphics]\n");
  fprintf(fp, "vsync = %s\n", ui->vsync ? "true" : "false");
  fprintf(fp, "show_fps = %s\n", ui->show_fps ? "true" : "false");
  fprintf(fp, "fps_limit = %d\n", ui->fps_limit);
  fprintf(fp, "lod_bias = %.2f\n", ui->lod_bias);

  fclose(fp);
  log_info(LOG_SOURCE, "Config saved to %s.", config_path);
}
