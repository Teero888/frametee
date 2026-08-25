#include "game_host.h"

#include "input_record.h"
#include <logger/logger.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/fs.h>

static const char *LOG_SOURCE = "GameHost";

// A game the engine cannot ask anything of. Every constraint query falls back
// to these, which is why the callers below need no null checks of their own.
#define FALLBACK_TICKS_PER_SECOND 50

static bool id_is_valid(const char *id) {
  if (!id || !*id) return false;
  size_t len = strlen(id);
  if (len >= FT_ID_MAX) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool semver_identifier_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
}

static bool semver_core_number(const char **cursor) {
  const char *start = *cursor;
  if (*start < '0' || *start > '9') return false;
  if (*start == '0' && start[1] >= '0' && start[1] <= '9') return false;
  while (**cursor >= '0' && **cursor <= '9') ++*cursor;
  return *cursor > start;
}

static bool semver_identifiers(const char **cursor, bool reject_numeric_leading_zero) {
  for (;;) {
    const char *start = *cursor;
    bool numeric = true;
    while (semver_identifier_char(**cursor)) {
      if (**cursor < '0' || **cursor > '9') numeric = false;
      ++*cursor;
    }
    if (*cursor == start) return false;
    if (reject_numeric_leading_zero && numeric && *start == '0' && *cursor - start > 1) return false;
    if (**cursor != '.') return **cursor == '\0' || **cursor == '+';
    ++*cursor;
  }
}

static bool semver_is_valid(const char *version) {
  if (!version || !*version) return false;
  const char *cursor = version;
  if (!semver_core_number(&cursor) || *cursor++ != '.' || !semver_core_number(&cursor) || *cursor++ != '.' ||
      !semver_core_number(&cursor))
    return false;
  if (*cursor == '-') {
    ++cursor;
    if (!semver_identifiers(&cursor, true)) return false;
  }
  if (*cursor == '+') {
    ++cursor;
    if (!semver_identifiers(&cursor, false)) return false;
  }
  return *cursor == '\0';
}

// Checks everything the engine relies on before it will talk to a module. A
// module that fails here is kept in the list with the reason, so the game
// picker can show why it is unusable instead of pretending it does not exist.
static bool validate_module(const ft_game_module *m, char *error, size_t error_size) {
#define FAIL(...)                            \
  do {                                       \
    snprintf(error, error_size, __VA_ARGS__); \
    return false;                            \
  } while (0)

  if (!m) FAIL("entry point returned NULL");
  if (m->struct_size != sizeof(ft_game_module))
    FAIL("module struct is %u bytes, engine expects exactly %zu; rebuild against ABI %u revision %u", m->struct_size,
         sizeof(ft_game_module), FT_GAME_ABI_VERSION, FT_GAME_ABI_REVISION);
  if (m->abi_version != FT_GAME_ABI_VERSION) FAIL("ABI version %u, engine expects %u", m->abi_version, FT_GAME_ABI_VERSION);
  if (m->abi_revision != FT_GAME_ABI_REVISION)
    FAIL("ABI revision %u, engine expects %u", m->abi_revision, FT_GAME_ABI_REVISION);
  if (m->info.struct_size != sizeof(ft_game_info))
    FAIL("game info struct is %u bytes, engine expects %zu", m->info.struct_size, sizeof(ft_game_info));
  if (!id_is_valid(m->info.id)) FAIL("invalid game id (lowercase ascii, digits, '_' and '-', under %d chars)", FT_ID_MAX);
  if (!m->info.display_name || !*m->info.display_name) FAIL("missing display name");
  if (!semver_is_valid(m->info.version) || strlen(m->info.version) >= FT_NAME_MAX)
    FAIL("invalid or missing SemVer 2.0.0 game version (must fit in %d bytes)", FT_NAME_MAX);
  if (m->constraints.struct_size != sizeof(ft_game_constraints))
    FAIL("game constraints struct is %u bytes, engine expects %zu", m->constraints.struct_size, sizeof(ft_game_constraints));
  if (!m->create || !m->destroy) FAIL("missing create/destroy");
  if (!m->world_create || !m->world_destroy || !m->world_copy || !m->world_step) FAIL("missing required world entry points");
  if (!m->world_tick || !m->world_player_count) FAIL("missing world_tick/world_player_count");
  if (!m->input_schema) FAIL("missing input schema");
  if (m->input_schema->struct_size != sizeof(ft_input_schema))
    FAIL("input schema struct is %u bytes, engine expects %zu", m->input_schema->struct_size, sizeof(ft_input_schema));
  if (m->input_schema->record_size == 0) FAIL("input schema declares a zero-sized record");
  if (m->input_schema->record_size > ENGINE_MAX_INPUT_RECORD)
    FAIL("input records are %u bytes; the editor supports at most %u", m->input_schema->record_size, ENGINE_MAX_INPUT_RECORD);
  if (m->input_schema->record_align == 0 || (m->input_schema->record_align & (m->input_schema->record_align - 1)) != 0)
    FAIL("input record alignment %u is not a non-zero power of two", m->input_schema->record_align);
  if (m->input_schema->record_align > ENGINE_INPUT_RECORD_ALIGNMENT)
    FAIL("input record alignment %u exceeds the editor maximum %u", m->input_schema->record_align,
         (unsigned)ENGINE_INPUT_RECORD_ALIGNMENT);
  if (m->input_schema->record_size % m->input_schema->record_align != 0)
    FAIL("input record size %u is not a multiple of its %u-byte alignment", m->input_schema->record_size,
         m->input_schema->record_align);
  if (m->input_schema->field_count > 0 && !m->input_schema->fields) FAIL("input schema declares fields but has no field table");
  if (m->input_schema->control_count > 0 && !m->input_schema->controls) FAIL("input schema declares controls but has no control table");
  if (m->input_schema->control_count > 64) FAIL("input schema declares %u controls; the editor supports at most 64", m->input_schema->control_count);
  bool needs_integer_access = false;
  bool needs_float_access = false;
  bool needs_vec2_access = false;
  for (uint32_t i = 0; i < m->input_schema->field_count; ++i) {
    const ft_input_field *field = &m->input_schema->fields[i];
    if (!id_is_valid(field->id)) FAIL("input field %u has an invalid id", i);
    if (!field->display_name || !*field->display_name) FAIL("input field '%s' has no display name", field->id);
    if (field->kind < FT_INPUT_BOOL || field->kind > FT_INPUT_VEC2) FAIL("input field '%s' has invalid kind %d", field->id, field->kind);
    if (field->kind == FT_INPUT_BOOL || field->kind == FT_INPUT_INT || field->kind == FT_INPUT_ENUM) needs_integer_access = true;
    if (field->kind == FT_INPUT_FLOAT) needs_float_access = true;
    if (field->kind == FT_INPUT_VEC2) needs_vec2_access = true;
    if ((field->kind == FT_INPUT_INT || field->kind == FT_INPUT_ENUM) && field->min_value > field->max_value)
      FAIL("input field '%s' has minimum %d above maximum %d", field->id, field->min_value, field->max_value);
    if ((field->kind == FT_INPUT_INT || field->kind == FT_INPUT_ENUM) &&
        (field->default_value < field->min_value || field->default_value > field->max_value))
      FAIL("input field '%s' has default %d outside [%d, %d]", field->id, field->default_value, field->min_value,
           field->max_value);
    if ((field->kind == FT_INPUT_FLOAT || field->kind == FT_INPUT_VEC2) && field->min_float > field->max_float)
      FAIL("input field '%s' has minimum %g above maximum %g", field->id, (double)field->min_float, (double)field->max_float);
    if ((field->kind == FT_INPUT_FLOAT || field->kind == FT_INPUT_VEC2) &&
        (field->default_float < field->min_float || field->default_float > field->max_float))
      FAIL("input field '%s' has default %g outside [%g, %g]", field->id, (double)field->default_float,
           (double)field->min_float, (double)field->max_float);
    if (field->kind == FT_INPUT_ENUM && field->enum_count > 0 && !field->enum_labels)
      FAIL("enum input field '%s' declares labels but has no label table", field->id);
    if (field->kind == FT_INPUT_ENUM && field->enum_count > 0 &&
        (uint64_t)field->enum_count != (uint64_t)((int64_t)field->max_value - (int64_t)field->min_value + 1))
      FAIL("enum input field '%s' has %u labels for range [%d, %d]", field->id, field->enum_count, field->min_value,
           field->max_value);
    if ((field->flags & FT_INPUT_FLAG_RECORDING_CURSOR) && field->kind != FT_INPUT_VEC2)
      FAIL("input field '%s' requests cursor recording but is not VEC2", field->id);
    for (uint32_t previous = 0; previous < i; ++previous)
      if (strcmp(m->input_schema->fields[previous].id, field->id) == 0) FAIL("duplicate input field id '%s'", field->id);
  }
  if (needs_integer_access && (!m->input_get || !m->input_set))
    FAIL("input schema contains BOOL/INT/ENUM fields without integer get/set entry points");
  if (needs_float_access && (!m->input_get_float || !m->input_set_float))
    FAIL("input schema contains FLOAT fields without float get/set entry points");
  if (needs_vec2_access && (!m->input_get_vec2 || !m->input_set_vec2))
    FAIL("input schema contains VEC2 fields without vec2 get/set entry points");
  for (uint32_t i = 0; i < m->input_schema->control_count; ++i) {
    const ft_input_control *control = &m->input_schema->controls[i];
    if (!id_is_valid(control->id)) FAIL("input control %u has an invalid id", i);
    if (!control->display_name || !*control->display_name) FAIL("input control '%s' has no display name", control->id);
    if (control->field >= m->input_schema->field_count)
      FAIL("input control '%s' targets field %u, but the schema has %u fields", control->id, control->field, m->input_schema->field_count);
    for (uint32_t previous = 0; previous < i; ++previous)
      if (strcmp(m->input_schema->controls[previous].id, control->id) == 0) FAIL("duplicate input control id '%s'", control->id);
  }
  if (m->constraints.ticks_per_second <= 0) FAIL("ticks_per_second must be positive");
  if (m->constraints.min_players < 0) FAIL("min_players must not be negative");
  if (m->constraints.max_players != 0 && m->constraints.max_players < m->constraints.min_players)
    FAIL("max_players (%d) below min_players (%d)", m->constraints.max_players, m->constraints.min_players);
  if (!(m->constraints.caps & FT_CAP_DYNAMIC_PLAYERS) &&
      (m->constraints.min_players <= 0 || m->constraints.max_players != m->constraints.min_players))
    FAIL("a non-dynamic game must declare one fixed positive cast size (min_players == max_players > 0)");
  if (m->constraints.variant_count > 0 && !m->constraints.variants) FAIL("declares variants but has no variant table");
  for (uint32_t i = 0; i < m->constraints.variant_count; ++i) {
    const ft_game_variant *variant = &m->constraints.variants[i];
    if (!id_is_valid(variant->id)) FAIL("variant %u has an invalid id", i);
    if (!variant->display_name || !*variant->display_name) FAIL("variant '%s' has no display name", variant->id);
    for (uint32_t previous = 0; previous < i; ++previous)
      if (strcmp(m->constraints.variants[previous].id, variant->id) == 0) FAIL("duplicate variant id '%s'", variant->id);
  }
  if (m->constraints.camera_mode_count > 0 && !m->constraints.camera_modes)
    FAIL("declares camera modes but has no camera-mode table");
  for (uint32_t i = 0; i < m->constraints.camera_mode_count; ++i) {
    const ft_camera_mode *mode = &m->constraints.camera_modes[i];
    if (!id_is_valid(mode->id)) FAIL("camera mode %u has an invalid id", i);
    if (strcmp(mode->id, FT_CAMERA_MODE_FREECAM_ID) == 0)
      FAIL("camera mode id '%s' is the engine's own freecam", mode->id);
    if (!mode->display_name || !*mode->display_name) FAIL("camera mode '%s' has no display name", mode->id);
    for (uint32_t previous = 0; previous < i; ++previous)
      if (strcmp(m->constraints.camera_modes[previous].id, mode->id) == 0) FAIL("duplicate camera mode id '%s'", mode->id);
  }
  if (m->entity_class_count > 0 && !m->entity_classes) FAIL("declares entity classes but has no entity-class table");
  for (uint32_t i = 0; i < m->entity_class_count; ++i) {
    const ft_entity_class *entity_class = &m->entity_classes[i];
    if (!id_is_valid(entity_class->id)) FAIL("entity class %u has an invalid id", i);
    if (!entity_class->display_name || !*entity_class->display_name)
      FAIL("entity class '%s' has no display name", entity_class->id);
    if (entity_class->prop_count > 0 && !entity_class->props)
      FAIL("entity class '%s' declares properties but has no property table", entity_class->id);
    for (uint32_t previous = 0; previous < i; ++previous)
      if (strcmp(m->entity_classes[previous].id, entity_class->id) == 0)
        FAIL("duplicate entity class id '%s'", entity_class->id);
    for (uint32_t prop = 0; prop < entity_class->prop_count; ++prop) {
      const ft_prop_desc *descriptor = &entity_class->props[prop];
      if (!id_is_valid(descriptor->id)) FAIL("property %u of entity class '%s' has an invalid id", prop, entity_class->id);
      if (!descriptor->display_name || !*descriptor->display_name)
        FAIL("property '%s' of entity class '%s' has no display name", descriptor->id, entity_class->id);
      // FT_VALUE_VEC3 sits above STRING in the enum, so this bound is the
      // highest kind rather than the last one that reads naturally.
      if (descriptor->kind < FT_VALUE_BOOL || descriptor->kind > FT_VALUE_VEC3)
        FAIL("property '%s' of entity class '%s' has invalid kind %d", descriptor->id, entity_class->id, descriptor->kind);
      for (uint32_t previous = 0; previous < prop; ++previous)
        if (strcmp(entity_class->props[previous].id, descriptor->id) == 0)
          FAIL("duplicate property id '%s' in entity class '%s'", descriptor->id, entity_class->id);
    }
  }
  const uint32_t known_caps = FT_CAP_DYNAMIC_PLAYERS | FT_CAP_LINKED_INPUTS | FT_CAP_WORLD_SERIALIZE | FT_CAP_LEVEL_FROM_MEMORY |
                              FT_CAP_TIMELINE_EVENTS | FT_CAP_EXPORTERS | FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS;
  if (m->constraints.caps & ~known_caps) FAIL("declares unknown capability bits 0x%x", m->constraints.caps & ~known_caps);
  if ((m->constraints.caps & FT_CAP_DYNAMIC_PLAYERS) && (!m->world_add_player || !m->world_remove_player))
    FAIL("advertises dynamic players without add/remove entry points");
  if ((m->constraints.caps & FT_CAP_WORLD_SERIALIZE) && (!m->world_serialize || !m->world_deserialize))
    FAIL("advertises serialization without serialize/deserialize entry points");
  if ((m->constraints.caps & FT_CAP_LEVEL_FROM_MEMORY) && !m->level_load_memory)
    FAIL("advertises in-memory levels without level_load_memory");
  if (m->level_serialize && !m->level_load_memory)
    FAIL("provides level serialization without level_load_memory");
  if ((m->level_load_path || m->level_load_memory) && !m->level_destroy)
    FAIL("provides a level loader without level_destroy");
  if ((m->world_serialize == NULL) != (m->world_deserialize == NULL))
    FAIL("world serialization entry points must be provided as a pair");
  if ((m->project_save == NULL) != (m->project_load == NULL))
    FAIL("project serialization entry points must be provided as a pair");
  if ((m->resources_create == NULL) != (m->resources_destroy == NULL))
    FAIL("render-resource create/destroy entry points must be provided as a pair");
  if ((m->constraints.caps & FT_CAP_RENDERS_LEVEL) && !m->render)
    FAIL("advertises level rendering without a render entry point");
  if ((m->constraints.caps & FT_CAP_TIMELINE_EVENTS) && !m->collect_events)
    FAIL("advertises timeline events without collect_events");
  if ((m->constraints.caps & FT_CAP_EXPORTERS) && (!m->exporter_count || !m->exporter_desc || !m->export_run))
    FAIL("advertises exporters without the exporter entry points");
  const bool has_settings = m->setting_count || m->setting_desc || m->setting_get || m->setting_set;
  if (has_settings && (!m->setting_count || !m->setting_desc || !m->setting_get || !m->setting_set))
    FAIL("game settings require count/descriptor/get/set entry points together");
  if (m->entity_class_count > 0 && (!m->entity_count || !m->entity_prop_get))
    FAIL("entity classes require entity_count and entity_prop_get entry points");
  bool has_writable_properties = false;
  for (uint32_t entity_class = 0; entity_class < m->entity_class_count; ++entity_class)
    for (uint32_t prop = 0; prop < m->entity_classes[entity_class].prop_count; ++prop)
      if (m->entity_classes[entity_class].props[prop].flags & (FT_PROP_WRITABLE | FT_PROP_STARTING)) has_writable_properties = true;
  if (has_writable_properties && !m->entity_prop_set)
    FAIL("writable/starting entity properties require entity_prop_set");
  if (m->constraints.caps & FT_CAP_LINKED_INPUTS) {
    if (m->input_schema->field_count > 64) FAIL("linked input copying supports at most 64 input fields");
    if (m->linked_action_count > 64) FAIL("declares %u linked actions; the editor supports at most 64", m->linked_action_count);
    if (m->linked_action_count > 0 && (!m->linked_actions || !m->linked_input_update))
      FAIL("declares linked actions without descriptors and an update callback");
    for (uint32_t i = 0; i < m->linked_action_count; ++i) {
      const ft_linked_action *action = &m->linked_actions[i];
      if (!id_is_valid(action->id)) FAIL("linked action %u has an invalid id", i);
      if (!action->display_name || !*action->display_name) FAIL("linked action '%s' has no display name", action->id);
      for (uint32_t previous = 0; previous < i; ++previous)
        if (strcmp(m->linked_actions[previous].id, action->id) == 0) FAIL("duplicate linked action id '%s'", action->id);
    }
  }

  error[0] = '\0';
  return true;
#undef FAIL
}

void game_host_init(game_host_t *host, const ft_engine_api *engine_api) {
  memset(host, 0, sizeof(*host));
  host->active = -1;
  host->engine_api = engine_api;
}

static game_module_slot_t *push_slot(game_host_t *host) {
  if (host->count >= host->capacity) {
    const int new_capacity = host->capacity ? host->capacity * 2 : 4;
    game_module_slot_t *grown = realloc(host->slots, (size_t)new_capacity * sizeof(*grown));
    if (!grown) return NULL;
    host->slots = grown;
    host->capacity = new_capacity;
  }
  game_module_slot_t *slot = &host->slots[host->count++];
  memset(slot, 0, sizeof(*slot));
  return slot;
}

// Loads one library and records the outcome. The library stays open for usable
// modules: their ft_game_module lives inside it, and games are few enough that
// keeping a handful of handles resident costs nothing.
static void try_load(game_host_t *host, const char *path) {
  for (int i = 0; i < host->count; ++i) {
    if (strcmp(host->slots[i].path, path) == 0) return; // already known
  }

  game_module_slot_t *slot = push_slot(host);
  if (!slot) return;
  snprintf(slot->path, sizeof(slot->path), "%s", path);

  void *handle = fs_load_library(path);
  if (!handle) {
    snprintf(slot->error, sizeof(slot->error), "failed to load library");
    log_warn(LOG_SOURCE, "Could not load '%s'.", path);
    return;
  }

  union {
    void *sym;
    ft_game_module_entry_fn entry;
  } u;
  u.sym = fs_get_symbol(handle, FT_GAME_MODULE_ENTRY_NAME);
  if (!u.entry) {
    // A game may keep shared runtime dependencies beside its module so the
    // platform loader can resolve them through $ORIGIN. They are not broken
    // games merely because they use the same file extension, so leave them out
    // of the picker entirely. Libraries that do export the entry point still
    // go through full validation below and report genuine module errors.
    fs_free_library(handle);
    --host->count;
    return;
  }

  const ft_game_module *module = u.entry(FT_GAME_ABI_VERSION);
  if (!validate_module(module, slot->error, sizeof(slot->error))) {
    log_warn(LOG_SOURCE, "Rejected '%s': %s", path, slot->error);
    fs_free_library(handle);
    return;
  }

  // Two modules claiming the same id would make project files ambiguous, so the
  // first one discovered wins and the second is reported rather than shadowed.
  for (int i = 0; i < host->count - 1; ++i) {
    if (host->slots[i].usable && strcmp(host->slots[i].id, module->info.id) == 0) {
      snprintf(slot->error, sizeof(slot->error), "duplicate game id '%.31s', already provided by %.400s", module->info.id,
               host->slots[i].path);
      log_warn(LOG_SOURCE, "%s", slot->error);
      fs_free_library(handle);
      return;
    }
  }

  slot->handle = handle;
  slot->module = module;
  slot->usable = true;
  snprintf(slot->id, sizeof(slot->id), "%s", module->info.id);
  snprintf(slot->display_name, sizeof(slot->display_name), "%s", module->info.display_name);
  log_info(LOG_SOURCE, "Found game '%s' (%s) v%s by %s.", slot->display_name, slot->id, module->info.version ? module->info.version : "?",
           module->info.author ? module->info.author : "?");
}

static int compare_slots(const void *a, const void *b) {
  const game_module_slot_t *left = a;
  const game_module_slot_t *right = b;
  if (left->usable != right->usable) return left->usable ? -1 : 1;
  const int by_id = strcmp(left->id, right->id);
  return by_id != 0 ? by_id : strcmp(left->path, right->path);
}

int game_host_discover(game_host_t *host, const char *directory) {
  snprintf(host->directory, sizeof(host->directory), "%s", directory);
  log_info(LOG_SOURCE, "Scanning for game modules in '%s'...", directory);

  fs_dir_t *dir = fs_opendir(directory);
  if (!dir) {
    log_warn(LOG_SOURCE, "Game directory '%s' does not exist.", directory);
    return 0;
  }

  fs_dirent_t *entry;
  while ((entry = fs_readdir(dir)) != NULL) {
    if (entry->is_directory) continue;
    const char *ext = strrchr(entry->name, '.');
    if (!ext) continue;
    if (strcmp(ext, ".so") != 0 && strcmp(ext, ".dll") != 0 && strcmp(ext, ".dylib") != 0) continue;

    char full_path[GAME_HOST_MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s%c%s", directory, PATH_SEP, entry->name);
    try_load(host, full_path);
  }
  fs_closedir(dir);

  if (host->slots && host->count > 1) {
    qsort(host->slots, (size_t)host->count, sizeof(*host->slots), compare_slots);
  }

  int usable = 0;
  for (int i = 0; i < host->count; ++i)
    if (host->slots[i].usable) usable++;
  log_info(LOG_SOURCE, "%d game module(s) available.", usable);
  return usable;
}

int game_host_find_id(const game_host_t *host, const char *id) {
  if (!host || !id) return -1;
  for (int i = 0; i < host->count; ++i) {
    if (host->slots[i].usable && strcmp(host->slots[i].id, id) == 0) return i;
  }
  return -1;
}

bool game_host_activate_index(game_host_t *host, int index) {
  if (index < 0 || index >= host->count) return false;
  game_module_slot_t *slot = &host->slots[index];
  if (!slot->usable) return false;
  if (host->active == index && host->instance) return true;

  game_host_deactivate(host);

  // Module construction may resolve its own data files. Publish the module's
  // identity before calling create so those paths already belong to the game
  // being activated, rather than to the previous game (or to an empty id on
  // startup).
  host->active = index;
  host->module = slot->module;
  host->variant_id[0] = '\0';
  snprintf(host->active_id, sizeof(host->active_id), "%s", slot->id);

  ft_game *instance = slot->module->create(host->engine_api);
  if (!instance) {
    log_error(LOG_SOURCE, "Game '%s' failed to initialize.", slot->id);
    host->active = -1;
    host->module = NULL;
    host->active_id[0] = '\0';
    return false;
  }

  host->instance = instance;
  // Bind the module-declared control table to its schema before recording can
  // write any fields.
  engine_input_bind(host);
  game_host_set_variant(host, NULL);
  log_info(LOG_SOURCE, "Activated game '%s'.", slot->display_name);
  return true;
}

bool game_host_activate_id(game_host_t *host, const char *id) {
  const int index = game_host_find_id(host, id);
  if (index < 0) {
    log_error(LOG_SOURCE, "No game module provides id '%s'.", id ? id : "(null)");
    return false;
  }
  return game_host_activate_index(host, index);
}

void game_host_deactivate(game_host_t *host) {
  if (!host->instance) return;
  if (host->module->resources_destroy) host->module->resources_destroy(host->instance);
  host->module->destroy(host->instance);
  host->instance = NULL;
  host->module = NULL;
  host->active = -1;
  host->variant_id[0] = '\0';
  host->active_id[0] = '\0';
}

void game_host_shutdown(game_host_t *host) {
  game_host_deactivate(host);
  // This is process shutdown, not a hot reload. Unloading a Rust cdylib here
  // tears down its private copy of the Rust runtime while allocator and TLS
  // state can still be referenced by process-exit destructors. Besides making
  // LeakSanitizer lose the module roots, that can double-free runtime state on
  // some libc/toolchain combinations. Game libraries have deliberately stayed
  // loaded for the whole session; let the OS reclaim their mappings at exit.
  free(host->slots);
  memset(host, 0, sizeof(*host));
  host->active = -1;
}

const char *game_host_active_id(const game_host_t *host) {
  if (!host) return "";
  return host->active_id;
}

const char *game_host_active_version(const game_host_t *host) {
  return host && host->module && host->module->info.version ? host->module->info.version : "";
}

void game_host_set_variant(game_host_t *host, const char *variant_id) {
  if (!host || !host->module) return;
  const ft_game_constraints *c = &host->module->constraints;
  if (variant_id) {
    for (uint32_t i = 0; i < c->variant_count; ++i) {
      if (c->variants[i].id && strcmp(c->variants[i].id, variant_id) == 0) {
        snprintf(host->variant_id, sizeof(host->variant_id), "%s", variant_id);
        return;
      }
    }
  }
  if (c->variant_count > 0 && c->variants[0].id)
    snprintf(host->variant_id, sizeof(host->variant_id), "%s", c->variants[0].id);
  else
    host->variant_id[0] = '\0';
}

const char *game_host_variant(const game_host_t *host) {
  if (!host || !host->variant_id[0]) return NULL;
  return host->variant_id;
}

// --- constraints ------------------------------------------------------------

static const ft_game_constraints *constraints_of(const game_host_t *host) {
  return (host && host->module) ? &host->module->constraints : NULL;
}

bool game_has_cap(const game_host_t *host, uint32_t cap) {
  const ft_game_constraints *c = constraints_of(host);
  return c && (c->caps & cap) != 0;
}

int game_min_players(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return c ? c->min_players : 0;
}

int game_max_players(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return c ? c->max_players : 0;
}

int game_clamp_player_count(const game_host_t *host, int count) {
  const ft_game_constraints *c = constraints_of(host);
  if (!c) return 0;
  if (count < c->min_players) count = c->min_players;
  if (c->max_players > 0 && count > c->max_players) count = c->max_players;
  return count;
}

bool game_has_fixed_players(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  if (!c) return true;
  if (!(c->caps & FT_CAP_DYNAMIC_PLAYERS)) return true;
  return c->max_players > 0 && c->max_players == c->min_players;
}

bool game_can_add_player(const game_host_t *host, int current_count) {
  const ft_game_constraints *c = constraints_of(host);
  if (!c || !(c->caps & FT_CAP_DYNAMIC_PLAYERS)) return false;
  return c->max_players <= 0 || current_count < c->max_players;
}

bool game_can_remove_player(const game_host_t *host, int current_count) {
  const ft_game_constraints *c = constraints_of(host);
  if (!c || !(c->caps & FT_CAP_DYNAMIC_PLAYERS)) return false;
  return current_count > c->min_players;
}

bool game_is_3d(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  // Anything that does not say otherwise is a plane, which is what every game
  // written before dimensions existed assumes.
  return c && c->dimensions == FT_DIMENSIONS_3D;
}

int game_ticks_per_second(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return c ? c->ticks_per_second : FALLBACK_TICKS_PER_SECOND;
}

float game_units_per_tile(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return (c && c->units_per_tile > 0.f) ? c->units_per_tile : 1.f;
}

float game_default_camera_height(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return (c && c->default_camera_height > 0.f) ? c->default_camera_height : 20.f;
}


const ft_input_schema *game_input_schema(const game_host_t *host) {
  return (host && host->module) ? host->module->input_schema : NULL;
}

unsigned game_input_size(const game_host_t *host) {
  const ft_input_schema *schema = game_input_schema(host);
  return schema ? schema->record_size : 0;
}

unsigned game_input_align(const game_host_t *host) {
  const ft_input_schema *schema = game_input_schema(host);
  return schema ? schema->record_align : 0;
}

int game_input_field_index(const game_host_t *host, const char *field_id) {
  const ft_input_schema *schema = game_input_schema(host);
  if (!schema || !field_id) return -1;
  for (uint32_t i = 0; i < schema->field_count; ++i) {
    if (schema->fields[i].id && strcmp(schema->fields[i].id, field_id) == 0) return (int)i;
  }
  return -1;
}

// --- call wrappers ----------------------------------------------------------

#define REQUIRE_GAME(ret)                       \
  if (!host || !host->instance) return ret;     \
  const ft_game_module *m = host->module;       \
  (void)m

ft_level *gh_level_load_path(game_host_t *host, const char *path) {
  REQUIRE_GAME(NULL);
  if (!m->level_load_path) return NULL;
  return m->level_load_path(host->instance, path, game_host_variant(host));
}

ft_level *gh_level_load_memory(game_host_t *host, const void *data, size_t size) {
  REQUIRE_GAME(NULL);
  if (!m->level_load_memory) return NULL;
  return m->level_load_memory(host->instance, data, size, game_host_variant(host));
}

void gh_level_destroy(game_host_t *host, ft_level *level) {
  REQUIRE_GAME();
  if (level && m->level_destroy) m->level_destroy(host->instance, level);
}

bool gh_level_info(game_host_t *host, const ft_level *level, ft_level_info *out) {
  REQUIRE_GAME(false);
  if (!level || !m->level_info || !out) return false;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  return m->level_info(host->instance, level, out);
}

size_t gh_level_serialize(game_host_t *host, const ft_level *level, void *out, size_t out_size) {
  REQUIRE_GAME(0);
  if (!level || !m->level_serialize) return 0;
  return m->level_serialize(host->instance, level, out, out_size);
}

ft_world *gh_world_create(game_host_t *host, const ft_level *level, int player_count, int world_index) {
  REQUIRE_GAME(NULL);
  ft_world_desc desc = {0};
  desc.struct_size = sizeof(desc);
  desc.level = level;
  desc.variant_id = game_host_variant(host);
  desc.player_count = game_clamp_player_count(host, player_count);
  desc.world_index = world_index;
  return m->world_create(host->instance, &desc);
}

void gh_world_destroy(game_host_t *host, ft_world *world) {
  REQUIRE_GAME();
  if (world) m->world_destroy(host->instance, world);
}

void gh_world_copy(game_host_t *host, ft_world *dst, const ft_world *src) {
  REQUIRE_GAME();
  if (dst && src) m->world_copy(host->instance, dst, src);
}

void gh_world_step(game_host_t *host, ft_world *world, const void *inputs, unsigned player_count) {
  REQUIRE_GAME();
  if (world) m->world_step(host->instance, world, inputs, player_count);
}

int gh_world_tick(game_host_t *host, const ft_world *world) {
  REQUIRE_GAME(0);
  return world ? m->world_tick(host->instance, world) : 0;
}

int gh_world_player_count(game_host_t *host, const ft_world *world) {
  REQUIRE_GAME(0);
  return world ? m->world_player_count(host->instance, world) : 0;
}

bool gh_world_player_view(game_host_t *host, const ft_world *world, int player, ft_player_view *out) {
  REQUIRE_GAME(false);
  if (!world || !out || !m->world_player_view) return false;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  return m->world_player_view(host->instance, world, player, out);
}

int gh_world_add_player(game_host_t *host, ft_world *world, int at_index, const ft_player_setup *setup) {
  REQUIRE_GAME(-1);
  if (!world || !m->world_add_player) return -1;
  if (!game_can_add_player(host, m->world_player_count(host->instance, world))) return -1;
  return m->world_add_player(host->instance, world, at_index, setup);
}

bool gh_world_remove_player(game_host_t *host, ft_world *world, int player) {
  REQUIRE_GAME(false);
  if (!world || !m->world_remove_player) return false;
  if (!game_can_remove_player(host, m->world_player_count(host->instance, world))) return false;
  return m->world_remove_player(host->instance, world, player);
}

size_t gh_world_serialize(game_host_t *host, const ft_world *world, void *out, size_t out_size) {
  REQUIRE_GAME(0);
  if (!world || !m->world_serialize) return 0;
  return m->world_serialize(host->instance, world, out, out_size);
}

bool gh_world_deserialize(game_host_t *host, ft_world *world, const void *data, size_t size) {
  REQUIRE_GAME(false);
  if (!world || !m->world_deserialize) return false;
  return m->world_deserialize(host->instance, world, data, size);
}

void gh_input_default(game_host_t *host, void *record) {
  if (!host || !host->instance || !record) return;
  if (host->module->input_default)
    host->module->input_default(host->instance, record);
  else
    memset(record, 0, game_input_size(host));
}

long long gh_input_get(game_host_t *host, const void *record, unsigned field) {
  REQUIRE_GAME(0);
  if (!record || !m->input_get) return 0;
  return m->input_get(host->instance, record, field);
}

void gh_input_set(game_host_t *host, void *record, unsigned field, long long value) {
  REQUIRE_GAME();
  if (record && m->input_set) m->input_set(host->instance, record, field, value);
}

float gh_input_get_float(game_host_t *host, const void *record, unsigned field) {
  REQUIRE_GAME(0.f);
  if (!record || !m->input_get_float) return 0.f;
  return m->input_get_float(host->instance, record, field);
}

void gh_input_set_float(game_host_t *host, void *record, unsigned field, float value) {
  REQUIRE_GAME();
  if (record && m->input_set_float) m->input_set_float(host->instance, record, field, value);
}

ft_vec2 gh_input_get_vec2(game_host_t *host, const void *record, unsigned field) {
  const ft_vec2 zero = {0.f, 0.f};
  REQUIRE_GAME(zero);
  if (!record || !m->input_get_vec2) return zero;
  return m->input_get_vec2(host->instance, record, field);
}

void gh_input_set_vec2(game_host_t *host, void *record, unsigned field, ft_vec2 value) {
  REQUIRE_GAME();
  if (record && m->input_set_vec2) m->input_set_vec2(host->instance, record, field, value);
}

void gh_linked_input_update(game_host_t *host, const ft_linked_input_frame *frame, void *inout_record) {
  REQUIRE_GAME();
  if (frame && inout_record && m->linked_input_update) m->linked_input_update(host->instance, frame, inout_record);
}

void gh_update(game_host_t *host, const ft_engine_state *state) {
  REQUIRE_GAME();
  if (m->update) m->update(host->instance, state);
}

void gh_render(game_host_t *host, const ft_render_frame *frame) {
  REQUIRE_GAME();
  if (m->render) m->render(host->instance, frame);
}

void gh_ui(game_host_t *host, const ft_ui_frame *frame) {
  REQUIRE_GAME();
  if (m->ui) m->ui(host->instance, frame);
}

bool gh_resources_create(game_host_t *host) {
  REQUIRE_GAME(false);
  if (!m->resources_create) return true;
  return m->resources_create(host->instance);
}

void gh_resources_destroy(game_host_t *host) {
  REQUIRE_GAME();
  if (m->resources_destroy) m->resources_destroy(host->instance);
}

unsigned gh_exporter_count(game_host_t *host) {
  REQUIRE_GAME(0);
  return m->exporter_count ? m->exporter_count(host->instance) : 0;
}

const ft_exporter_desc *gh_exporter_desc(game_host_t *host, unsigned index) {
  REQUIRE_GAME(NULL);
  return m->exporter_desc ? m->exporter_desc(host->instance, index) : NULL;
}

bool gh_export_run(game_host_t *host, unsigned index, const ft_export_request *request) {
  REQUIRE_GAME(false);
  return m->export_run ? m->export_run(host->instance, index, request) : false;
}

bool gh_entity_prop_get(game_host_t *host, const ft_world *world, unsigned entity_class, int entity, unsigned prop, ft_value *out) {
  REQUIRE_GAME(false);
  if (!world || !out || !m->entity_prop_get) return false;
  return m->entity_prop_get(host->instance, world, entity_class, entity, prop, out);
}

bool gh_entity_prop_set(game_host_t *host, ft_world *world, unsigned entity_class, int entity, unsigned prop, const ft_value *value) {
  REQUIRE_GAME(false);
  if (!world || !value || !m->entity_prop_set) return false;
  return m->entity_prop_set(host->instance, world, entity_class, entity, prop, value);
}

int gh_entity_count(game_host_t *host, const ft_world *world, unsigned entity_class) {
  REQUIRE_GAME(0);
  if (!world) return 0;
  if (entity_class == FT_ENTITY_CLASS_PLAYER) return m->world_player_count(host->instance, world);
  return m->entity_count ? m->entity_count(host->instance, world, entity_class) : 0;
}

const ft_entity_class *gh_entity_class(const game_host_t *host, unsigned index) {
  if (!host || !host->module) return NULL;
  if (index >= host->module->entity_class_count) return NULL;
  return &host->module->entity_classes[index];
}

unsigned gh_entity_class_count(const game_host_t *host) {
  if (!host || !host->module) return 0;
  return host->module->entity_class_count;
}

void gh_collect_events(game_host_t *host, const ft_world *previous, const ft_world *world,
                       void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  REQUIRE_GAME();
  if (m->collect_events && world && emit) m->collect_events(host->instance, previous, world, emit, user);
}

#undef REQUIRE_GAME

void game_host_print_listing(const game_host_t *host) {
  // Deliberately printf rather than the logger: this is the output of a
  // command, not a diagnostic, and it is what scripts will parse.
  printf("FrameTee game modules (ABI %u)\n", FT_GAME_ABI_VERSION);
  if (host->count == 0) {
    printf("  none found in '%s'\n", host->directory);
    return;
  }
  for (int i = 0; i < host->count; ++i) {
    const game_module_slot_t *slot = &host->slots[i];
    if (!slot->usable) {
      printf("  [unusable] %s: %s\n", slot->path, slot->error);
      continue;
    }
    const ft_game_constraints *c = &slot->module->constraints;
    printf("  %-24s %s v%s by %s\n", slot->id, slot->display_name, slot->module->info.version ? slot->module->info.version : "?",
           slot->module->info.author ? slot->module->info.author : "?");
    char max_players[16];
    if (c->max_players > 0)
      snprintf(max_players, sizeof(max_players), "%d", c->max_players);
    else
      snprintf(max_players, sizeof(max_players), "unbounded");
    printf("      players %d..%s, %d ticks/s, %s cast\n", c->min_players, max_players, c->ticks_per_second,
           (c->caps & FT_CAP_DYNAMIC_PLAYERS) ? "dynamic" : "fixed");
    printf("      inputs:");
    for (uint32_t f = 0; f < slot->module->input_schema->field_count; ++f) printf(" %s", slot->module->input_schema->fields[f].id);
    printf("\n");
    printf("      controls:");
    for (uint32_t action = 0; action < slot->module->input_schema->control_count; ++action)
      printf(" %s", slot->module->input_schema->controls[action].id);
    printf("\n");
  }
}

unsigned gh_setting_count(game_host_t *host) {
  if (!host || !host->instance) return 0;
  return host->module->setting_count ? host->module->setting_count(host->instance) : 0;
}

const ft_setting_desc *gh_setting_desc(game_host_t *host, unsigned index) {
  if (!host || !host->instance || !host->module->setting_desc) return NULL;
  return host->module->setting_desc(host->instance, index);
}

bool gh_setting_get(game_host_t *host, unsigned index, ft_value *out) {
  if (!host || !host->instance || !host->module->setting_get || !out) return false;
  return host->module->setting_get(host->instance, index, out);
}

bool gh_setting_set(game_host_t *host, unsigned index, const ft_value *value) {
  if (!host || !host->instance || !host->module->setting_set || !value) return false;
  return host->module->setting_set(host->instance, index, value);
}

int gh_setting_find(game_host_t *host, const char *id) {
  if (!id) return -1;
  const unsigned count = gh_setting_count(host);
  for (unsigned i = 0; i < count; ++i) {
    const ft_setting_desc *desc = gh_setting_desc(host, i);
    if (desc && desc->id && strcmp(desc->id, id) == 0) return (int)i;
  }
  return -1;
}

size_t gh_project_save(game_host_t *host, void *out, size_t out_size) {
  if (!host || !host->instance || !host->module->project_save) return 0;
  return host->module->project_save(host->instance, out, out_size);
}

bool gh_project_load(game_host_t *host, const void *data, size_t size) {
  if (!host || !host->instance) return false;
  if (!host->module->project_load) return size == 0;
  return host->module->project_load(host->instance, data, size);
}

// A game that declares no camera modes still gets one, so the engine never has
// to special-case "no modes at all".
static const ft_camera_mode g_default_camera_mode = {"free", "Free view", "Pan and zoom freely", FT_CAMERA_MODE_FREE};

// Flying through a volume is the engine's camera, not a game's: it asks the
// simulation for nothing and every 3D game wants it. So it is appended to
// whatever modes the game declared, which makes it selected, cycled and stored
// exactly like one of them instead of living on a hotkey of its own. Its index
// is one past the game's list and is never handed to camera_update — the
// engine flies the camera itself in this mode.
static const ft_camera_mode g_freecam_mode = {FT_CAMERA_MODE_FREECAM_ID, "Freecam",
                                              "Fly the camera: WASD to move, space and shift for up and down",
                                              FT_CAMERA_MODE_FREE};

// How many modes the game itself declares, counting the stand-in it gets when
// it declares none.
static unsigned game_own_camera_mode_count(const game_host_t *host) {
  const ft_game_constraints *c = constraints_of(host);
  return (!c || c->camera_mode_count == 0) ? 1u : c->camera_mode_count;
}

unsigned game_camera_mode_count(const game_host_t *host) {
  return game_own_camera_mode_count(host) + (game_is_3d(host) ? 1u : 0u);
}

const ft_camera_mode *game_camera_mode(const game_host_t *host, unsigned index) {
  const ft_game_constraints *c = constraints_of(host);
  const unsigned own = game_own_camera_mode_count(host);
  if (index >= own && game_is_3d(host)) return &g_freecam_mode;
  if (!c || c->camera_mode_count == 0) return &g_default_camera_mode;
  if (index >= own) return &c->camera_modes[0];
  return &c->camera_modes[index];
}

bool game_camera_mode_is_freecam(const game_host_t *host, unsigned index) {
  return game_camera_mode(host, index) == &g_freecam_mode;
}

bool gh_camera_update(game_host_t *host, const ft_camera_frame *frame, ft_camera *inout) {
  if (!host || !host->instance) return false;
  if (!host->module->camera_update || !frame || !inout) return false;
  return host->module->camera_update(host->instance, frame, inout);
}

unsigned gh_status_lines(game_host_t *host, const ft_world *world, int player, float alpha, char *out, unsigned max_lines,
                         unsigned line_size) {
  if (!host || !host->instance || !host->module->status_lines || !world || !out) return 0;
  return host->module->status_lines(host->instance, world, player, alpha, out, max_lines, line_size);
}

bool gh_player_label(game_host_t *host, const ft_world *world, int player, char *out, size_t out_size) {
  if (out && out_size > 0) out[0] = '\0';
  if (!host || !host->instance || !host->module->player_label || !world || !out) return false;
  return host->module->player_label(host->instance, world, player, out, out_size);
}

bool game_provides_splash(const game_host_t *host) {
  // A game that implements the UI hook at all is assumed to handle the splash
  // slot; there is no finer signal, and drawing nothing is a valid choice.
  return host && host->module && host->module->ui != NULL;
}
