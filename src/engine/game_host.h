#ifndef ENGINE_GAME_HOST_H
#define ENGINE_GAME_HOST_H

// The engine's side of the game module ABI.
//
// The host discovers game modules, performs the ABI handshake, keeps exactly
// one of them active, and answers every "may I?" question the rest of the
// engine has to ask before touching a world. Nothing outside this file calls a
// module's function pointers directly: the gh_* wrappers below null-check the
// optional entries and apply the module's constraints, so an engine call site
// never has to care whether a game implements a given feature.

#include <frametee/game_abi.h>
#include <stdbool.h>
#include <stddef.h>

#define GAME_HOST_MAX_PATH 1024

typedef struct game_module_slot_t {
  char path[GAME_HOST_MAX_PATH];
  void *handle;                 // shared library handle, NULL for builtins
  const ft_game_module *module; // valid while the library stays loaded
  char id[FT_ID_MAX];
  char display_name[FT_NAME_MAX];
  char error[512]; // why the module was rejected, empty when usable
  bool usable;
} game_module_slot_t;

typedef struct game_host_t {
  game_module_slot_t *slots;
  int count;
  int capacity;

  int active; // index into slots, -1 when no game is active
  const ft_game_module *module;
  ft_game *instance;
  const ft_engine_api *engine_api;

  char directory[GAME_HOST_MAX_PATH];
  char variant_id[FT_ID_MAX];
  // Id of the active game, or "" when none. Held in the host rather than read
  // out of the module so callers can keep a pointer to it across activations
  // and unloads.
  char active_id[FT_ID_MAX];
} game_host_t;

// Lifecycle ------------------------------------------------------------------

void game_host_init(game_host_t *host, const ft_engine_api *engine_api);
// Scans `directory` for shared libraries and records every module that passes
// the handshake. Rejected libraries are kept in the list with an error string
// so the UI can explain the problem instead of silently ignoring them.
int game_host_discover(game_host_t *host, const char *directory);
bool game_host_activate_index(game_host_t *host, int index);
bool game_host_activate_id(game_host_t *host, const char *id);
void game_host_deactivate(game_host_t *host);
void game_host_shutdown(game_host_t *host);

int game_host_find_id(const game_host_t *host, const char *id);
static inline bool game_host_ready(const game_host_t *host) { return host && host->instance != NULL; }
const char *game_host_active_id(const game_host_t *host);
const char *game_host_active_version(const game_host_t *host);
// Prints every discovered module and what it allows, including the ones that
// were rejected and why. Backs the --list-games command.
void game_host_print_listing(const game_host_t *host);

// Selects the ruleset used for worlds created from now on. Passing NULL or an
// unknown id falls back to the module's first variant.
void game_host_set_variant(game_host_t *host, const char *variant_id);
const char *game_host_variant(const game_host_t *host);

// Constraints ----------------------------------------------------------------
//
// All of these are safe to call with no game active; they then report the most
// restrictive answer so the engine degrades to doing nothing rather than to
// doing something invalid.

bool game_has_cap(const game_host_t *host, uint32_t cap);
int game_min_players(const game_host_t *host);
int game_max_players(const game_host_t *host); // 0 means unbounded
int game_clamp_player_count(const game_host_t *host, int count);
bool game_can_add_player(const game_host_t *host, int current_count);
bool game_can_remove_player(const game_host_t *host, int current_count);
// True when the player count is pinned, e.g. a one-player-only game. The
// engine hides its add/remove affordances entirely in that case.
bool game_has_fixed_players(const game_host_t *host);
int game_ticks_per_second(const game_host_t *host);
// Whether the active game's world is a volume. Decides the viewport camera, the
// projection the renderer builds and whether depth testing is on.
bool game_is_3d(const game_host_t *host);
float game_units_per_tile(const game_host_t *host);
float game_default_camera_height(const game_host_t *host);

const ft_input_schema *game_input_schema(const game_host_t *host);
unsigned game_input_size(const game_host_t *host);
unsigned game_input_align(const game_host_t *host);
int game_input_field_index(const game_host_t *host, const char *field_id);

// Call wrappers --------------------------------------------------------------
//
// Thin, null-safe forwards to the active module. Everything returns a benign
// value when the feature is unavailable.

ft_level *gh_level_load_path(game_host_t *host, const char *path);
ft_level *gh_level_load_memory(game_host_t *host, const void *data, size_t size);
void gh_level_destroy(game_host_t *host, ft_level *level);
bool gh_level_info(game_host_t *host, const ft_level *level, ft_level_info *out);
// Bytes that reload the level, or 0 when the game cannot provide them.
size_t gh_level_serialize(game_host_t *host, const ft_level *level, void *out, size_t out_size);

// `world_index` is the editor world this belongs to; every copy of it shares one.
ft_world *gh_world_create(game_host_t *host, const ft_level *level, int player_count, int world_index);
void gh_world_destroy(game_host_t *host, ft_world *world);
void gh_world_copy(game_host_t *host, ft_world *dst, const ft_world *src);
void gh_world_step(game_host_t *host, ft_world *world, const void *inputs, unsigned player_count);
int gh_world_tick(game_host_t *host, const ft_world *world);
int gh_world_player_count(game_host_t *host, const ft_world *world);
bool gh_world_player_view(game_host_t *host, const ft_world *world, int player, ft_player_view *out);
int gh_world_add_player(game_host_t *host, ft_world *world, int at_index, const ft_player_setup *setup);
bool gh_world_remove_player(game_host_t *host, ft_world *world, int player);
size_t gh_world_serialize(game_host_t *host, const ft_world *world, void *out, size_t out_size);
bool gh_world_deserialize(game_host_t *host, ft_world *world, const void *data, size_t size);

void gh_input_default(game_host_t *host, void *record);
long long gh_input_get(game_host_t *host, const void *record, unsigned field);
void gh_input_set(game_host_t *host, void *record, unsigned field, long long value);
float gh_input_get_float(game_host_t *host, const void *record, unsigned field);
void gh_input_set_float(game_host_t *host, void *record, unsigned field, float value);
ft_vec2 gh_input_get_vec2(game_host_t *host, const void *record, unsigned field);
void gh_input_set_vec2(game_host_t *host, void *record, unsigned field, ft_vec2 value);
void gh_linked_input_update(game_host_t *host, const ft_linked_input_frame *frame, void *inout_record);

void gh_update(game_host_t *host, const ft_engine_state *state);
void gh_render(game_host_t *host, const ft_render_frame *frame);
void gh_ui(game_host_t *host, const ft_ui_frame *frame);
bool gh_resources_create(game_host_t *host);
void gh_resources_destroy(game_host_t *host);

unsigned gh_exporter_count(game_host_t *host);
const ft_exporter_desc *gh_exporter_desc(game_host_t *host, unsigned index);
bool gh_export_run(game_host_t *host, unsigned index, const ft_export_request *request);

bool gh_entity_prop_get(game_host_t *host, const ft_world *world, unsigned entity_class, int entity, unsigned prop, ft_value *out);
bool gh_entity_prop_set(game_host_t *host, ft_world *world, unsigned entity_class, int entity, unsigned prop, const ft_value *value);
int gh_entity_count(game_host_t *host, const ft_world *world, unsigned entity_class);
const ft_entity_class *gh_entity_class(const game_host_t *host, unsigned index);
unsigned gh_entity_class_count(const game_host_t *host);

// The game's own status readout for the viewport overlay. Returns how many
// lines were written into `out`, which must hold max_lines * line_size bytes.
unsigned gh_status_lines(game_host_t *host, const ft_world *world, int player, float alpha, char *out, unsigned max_lines,
                         unsigned line_size);

// A short line for a player in a list, e.g. a finish time. False when the game
// has nothing to say about that player.
bool gh_player_label(game_host_t *host, const ft_world *world, int player, char *out, size_t out_size);

// True when the active game draws its own start screen, so the editor knows
// whether to fall back to its own.
bool game_provides_splash(const game_host_t *host);

// Camera modes the active game offers. Falls back to a single free view when a
// game declares none, so the engine always has something to show.
unsigned game_camera_mode_count(const game_host_t *host);
const ft_camera_mode *game_camera_mode(const game_host_t *host, unsigned index);
bool gh_camera_update(game_host_t *host, const ft_camera_frame *frame, ft_camera *inout);

// Game settings the engine renders and persists on the game's behalf.
unsigned gh_setting_count(game_host_t *host);
const ft_setting_desc *gh_setting_desc(game_host_t *host, unsigned index);
bool gh_setting_get(game_host_t *host, unsigned index, ft_value *out);
bool gh_setting_set(game_host_t *host, unsigned index, const ft_value *value);
int gh_setting_find(game_host_t *host, const char *id);

// Opaque per-project data owned by the active game module.
size_t gh_project_save(game_host_t *host, void *out, size_t out_size);
bool gh_project_load(game_host_t *host, const void *data, size_t size);

void gh_collect_events(game_host_t *host, const ft_world *previous, const ft_world *world,
                       void (*emit)(void *user, const ft_timeline_event *event), void *user);

#endif // ENGINE_GAME_HOST_H
