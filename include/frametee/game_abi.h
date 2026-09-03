/* FrameTee game module ABI.
 *
 * This header is the entire contract between the FrameTee engine and a game.
 * The engine knows nothing about any particular game: it owns the timeline,
 * snippets, undo/redo, projects, plugins, the window, the renderer and the
 * camera. A game owns its physics, its level format, how a world looks on
 * screen, and any panels, exporters or settings that only make sense for it.
 *
 * A game module is a shared library (.so/.dll/.dylib) exporting a single
 * symbol, ft_game_module_entry(). Nothing else about the library matters, so a
 * module can be written in any language able to produce a C-callable shared
 * library (C, C++, Rust, Zig, ...). Only the types below cross the boundary:
 * fixed-width integers, floats, pointers and UTF-8 strings. No engine header,
 * no ImGui type, no Vulkan type and no allocator is shared across it.
 *
 * Version rules
 * -------------
 *  - FT_GAME_ABI_VERSION is bumped whenever an existing field changes meaning,
 *    changes type, is added or is removed. The engine refuses modules built
 *    against any other ABI version.
 *  - Every struct that a module fills in starts with a `struct_size` field set
 *    to sizeof of the current struct. This pre-release API deliberately has no
 *    binary-compatibility promise: rebuild modules whenever the ABI changes.
 *  - Function pointers may be NULL unless documented as required. A NULL
 *    optional entry means "this game does not do that", and the engine adapts
 *    (hides UI, disables a feature) rather than failing to load.
 *
 * Threading
 * ---------
 * Unless a call is documented otherwise, the engine calls a module from the
 * thread that owns the frame loop. World simulation calls (world_step and
 * friends) may additionally be issued from worker threads, but never
 * concurrently on the same ft_world_t. Rendering and UI callbacks are always
 * main thread.
 *
 * Ownership
 * ---------
 * Every handle a module returns is freed by that same module through the
 * matching destroy call. Strings a module returns must stay valid until the
 * module is destroyed. Strings the engine passes in are only valid for the
 * duration of the call.
 */

#ifndef FRAMETEE_GAME_ABI_H
#define FRAMETEE_GAME_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Versioning and linkage
 * ------------------------------------------------------------------------- */

/* Bumped on any breaking change to the structures or calls below. */
#define FT_GAME_ABI_VERSION 17u

/* Reserved for describing revisions of one ABI in diagnostics. */
#define FT_GAME_ABI_REVISION 0u

#define FT_GAME_MODULE_ENTRY_NAME "ft_game_module_entry"

#if defined(_WIN32)
#define FT_GAME_EXPORT __declspec(dllexport)
#else
#define FT_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* Maximum length, including the terminator, of the identifiers that end up in
 * project files and on disk. Kept small and fixed so the engine can store them
 * inline in save files. */
#define FT_ID_MAX 32
#define FT_NAME_MAX 64

/* -------------------------------------------------------------------------
 * Plain data types
 * ------------------------------------------------------------------------- */

typedef struct ft_vec2 {
  float x, y;
} ft_vec2;

typedef struct ft_vec3 {
  float x, y, z;
} ft_vec3;

typedef struct ft_rect {
  float x, y, w, h;
} ft_rect;

typedef struct ft_color {
  float r, g, b, a;
} ft_color;

/* A tagged value, used everywhere the engine has to move a game-defined
 * quantity around without knowing what it is: entity inspectors, plugin and
 * script access, starting-state overrides. Strings are owned by the producer
 * and valid until the next call on the same object. */
typedef enum ft_value_kind {
  FT_VALUE_BOOL = 0,
  FT_VALUE_INT = 1,
  FT_VALUE_FLOAT = 2,
  FT_VALUE_VEC2 = 3,
  FT_VALUE_VEC3 = 5,
  FT_VALUE_STRING = 4,
} ft_value_kind;

typedef struct ft_value {
  ft_value_kind kind;
  union {
    bool b;
    int64_t i;
    double f;
    ft_vec2 v;
    ft_vec3 v3;
    const char *s;
  } as;
} ft_value;

/* Opaque handles owned by the game module. */
typedef struct ft_game ft_game;   /* one instance of a loaded game */
typedef struct ft_level ft_level; /* a loaded map/level/track */
typedef struct ft_world ft_world; /* one simulation state (a full snapshot) */

/* Opaque handles owned by the engine and handed to the module. */
typedef struct ft_texture ft_texture;
typedef struct ft_atlas ft_atlas;
typedef struct ft_pipeline ft_pipeline;
typedef struct ft_mesh ft_mesh;

/* -------------------------------------------------------------------------
 * Identity and capabilities
 * ------------------------------------------------------------------------- */

typedef struct ft_game_info {
  uint32_t struct_size;
  /* Stable machine identifier, e.g. "ddnet". Written into project files and
   * matched against a plugin's game_id, so it must never change once shipped.
   * Lowercase ASCII, digits, '_' and '-' only. */
  const char *id;
  const char *display_name; /* "DDNet" */
  /* Required Semantic Versioning 2.0.0 string (https://semver.org/), e.g.
   * "0.3.1" or "2.0.0-beta.1+git.42". Stored in project files. */
  const char *version;
  const char *author;
  /* Optional: URL shown in the game picker. */
  const char *url;
  /* Optional: a thumbnail for the game picker, as a path relative to this
   * game's data directory (data/games/<id>/). Roughly 16:9 looks best. The
   * editor falls back to a plain card when it is missing. */
  const char *thumbnail;
} ft_game_info;

/* Capability bits. Anything not advertised here is assumed unsupported, and
 * the engine hides or disables the corresponding editor features. */
enum ft_game_caps {
  /* Players can be added or removed after a world exists. Games with a fixed
   * cast (Trackmania, a Mario run) leave this off, and the engine then hides
   * add/remove track actions entirely. */
  FT_CAP_DYNAMIC_PLAYERS = 1u << 0,
  /* Inputs may be authored for additional tracks while another track is being
   * recorded. The engine supplies schema-driven copy/mirror controls and a
   * second binding bank; the game may add derived linked-input actions. */
  FT_CAP_LINKED_INPUTS = 1u << 1,
  /* (1u << 2) is unused. It once meant "supports multiple worlds", which was a
   * mistake: a timeline group is simply another instance of the same
   * simulation, so any game that can create one world can create several. Only
   * the cast inside a world is a game's business. */
  /* world_serialize/world_deserialize are implemented, so starting states can
   * be stored in project files. */
  FT_CAP_WORLD_SERIALIZE = 1u << 3,
  /* level_load_memory is implemented; the engine may hand over downloaded or
   * embedded level bytes instead of a path. */
  FT_CAP_LEVEL_FROM_MEMORY = 1u << 4,
  /* The game emits timeline events (chat, checkpoints, deaths...) the engine
   * shows on the timeline. */
  FT_CAP_TIMELINE_EVENTS = 1u << 5,
  /* The game provides at least one exporter (demo/replay/video). */
  FT_CAP_EXPORTERS = 1u << 6,
  /* The game draws its own level; the engine will call render_level. */
  FT_CAP_RENDERS_LEVEL = 1u << 7,
  /* The game can be stepped without any rendering resources present, so
   * headless scripting is allowed. */
  FT_CAP_HEADLESS = 1u << 8,
  /* The game places the engine's starting-state editor inside a panel of its
   * own, by calling starting_state_editor. The engine then does not open a
   * window for it; a game that leaves this off gets that window instead, so the
   * editor exists either way. */
  FT_CAP_HOSTS_STARTING_STATE = 1u << 9,
};

/* How the viewport may be pointed at the world. A racing game might offer
 * chase and trackside views, a platformer just a lock and a free view. The
 * engine owns panning, zooming and the projection; the game owns what modes
 * exist and where each one puts the camera. */
enum ft_camera_mode_flags {
  /* The user drives it: panning and dragging move the camera directly. */
  FT_CAMERA_MODE_FREE = 1u << 0,
  /* The game drives it. The engine drops back to the first free mode when the
   * user starts panning, so a drag always does something. */
  FT_CAMERA_MODE_DIRECTED = 1u << 1,
};

typedef struct ft_camera_mode {
  const char *id; /* stable, stored in config */
  const char *display_name;
  const char *description;
  uint32_t flags;
} ft_camera_mode;

/* Reserved for the engine. A 3D game's mode list is presented with the engine's
 * own freecam and top-down modes appended to it, so a game declaring either id
 * is rejected at load. The appended modes are never passed to camera_update:
 * the engine controls them itself. */
#define FT_CAMERA_MODE_FREECAM_ID "freecam"
#define FT_CAMERA_MODE_TOP_DOWN_ID "top_down"

/* A selectable ruleset within a game: DDNet's DDRace/FastCap, a console game's
 * region revisions, a speedrun category. Chosen before a level is loaded and
 * stored in the project. */
typedef struct ft_game_variant {
  const char *id; /* stable, stored in project files */
  const char *display_name;
} ft_game_variant;

/* What the game allows the engine to do. The engine treats these as hard
 * limits: it clamps its own UI and refuses operations that would violate them,
 * so a game never has to defend against impossible configurations. */
/* Whether a game's world is a plane or a volume. This is the one thing the
 * engine cannot infer: it decides how the viewport camera behaves, which
 * projection the renderer builds, and whether depth testing is on. A game that
 * leaves it zero is treated as 2D, which is what every game written before this
 * existed expects. */
typedef enum ft_dimensions {
  FT_DIMENSIONS_2D = 0,
  FT_DIMENSIONS_3D = 3,
} ft_dimensions;

typedef struct ft_game_constraints {
  uint32_t struct_size;

  uint32_t caps; /* bitmask of ft_game_caps */

  /* Plane or volume. See ft_dimensions. */
  ft_dimensions dimensions;

  /* Player-count envelope. Setting both to 1 pins the game to exactly one
   * player and the engine drops all multi-player affordances. max_players
   * may be 0 for "no fixed upper bound" on a dynamic game. A game without
   * FT_CAP_DYNAMIC_PLAYERS must set both values to the same positive cast
   * size, because the editor cannot add a missing player on its behalf. */
  int32_t min_players;
  int32_t max_players;

  /* Simulation rate in ticks per second. Drives playback speed, the timeline
   * ruler and every duration the engine shows. Must be > 0. */
  int32_t ticks_per_second;

  /* World units per level tile, and the default camera height in tiles. Used
   * only to pick sane camera defaults; a game free of tiles can set
   * units_per_tile to 1. */
  float units_per_tile;
  float default_camera_height;

  /* Optional ruleset list. May be NULL/0 for a game with a single ruleset. */
  const ft_game_variant *variants;
  uint32_t variant_count;

  /* Camera modes this game offers. NULL/0 leaves the engine with a single free
   * view, which is a reasonable default for a game that does not care. */
  const ft_camera_mode *camera_modes;
  uint32_t camera_mode_count;

  /* File-picker hints for levels, e.g. "map" / "DDNet map". Both may be NULL
   * when the game does not load levels from disk at all. */
  const char *level_extension;
  const char *level_filter_name;
} ft_game_constraints;

/* -------------------------------------------------------------------------
 * Input schema
 * ------------------------------------------------------------------------- */

/* An input record is an opaque, fixed-size, trivially copyable blob defined by
 * the game. The engine stores arrays of them in snippets, writes them to
 * project files and hands them back at simulation time, but only ever reads or
 * writes individual fields through the schema below. That is what lets one
 * timeline UI edit DDNet's direction/jump/hook and a driving game's steering
 * axis without knowing about either. */

typedef enum ft_input_kind {
  FT_INPUT_BOOL = 0,  /* held button: jump, fire, hook */
  FT_INPUT_INT = 1,   /* discrete value in [min_value, max_value] */
  FT_INPUT_ENUM = 2,  /* discrete value with labels, e.g. active weapon */
  FT_INPUT_FLOAT = 3, /* continuous value, read via input_get_float */
  FT_INPUT_VEC2 = 4,  /* aim/target/stick, read via input_get_vec2 */
} ft_input_kind;

enum ft_input_field_flags {
  /* Show a dedicated lane for this field on the timeline. Without it the field
   * is still editable in the snippet matrix, just not given a row. */
  FT_INPUT_FLAG_TIMELINE_LANE = 1u << 0,
  /* Edge-triggered: the value matters on the tick it changes rather than while
   * held. The engine draws these as marks instead of bars. */
  FT_INPUT_FLAG_LATCHED = 1u << 1,
  /* Negating this field mirrors the run horizontally/vertically. Used by the
   * engine's mirror tooling when copying input between players. */
  FT_INPUT_FLAG_MIRROR_X = 1u << 2,
  FT_INPUT_FLAG_MIRROR_Y = 1u << 3,
  /* Hidden from the generic editors; the game edits it in its own panel. */
  FT_INPUT_FLAG_INTERNAL = 1u << 4,
  /* Field is a one-shot request (kill, reset) rather than a state. */
  FT_INPUT_FLAG_TRIGGER = 1u << 5,
  /* While recording, fill this VEC2 from the editor's world-space cursor.
   * This is schema metadata rather than an engine convention around names
   * such as "target" or "aim". */
  FT_INPUT_FLAG_RECORDING_CURSOR = 1u << 6,
  /* Hide a stored field from generic editing and display UIs. Unlike INTERNAL,
   * this is presentation metadata and is excluded from project schema
   * compatibility, so a game may hide an existing field without invalidating
   * projects whose input record layout and semantics are unchanged. */
  FT_INPUT_FLAG_EDITOR_HIDDEN = 1u << 7,
};

typedef struct ft_input_field {
  const char *id; /* stable identifier, used in project files and scripts */
  const char *display_name;
  const char *description; /* tooltip, may be NULL */
  ft_input_kind kind;
  uint32_t flags; /* ft_input_field_flags */

  int32_t min_value, max_value, default_value; /* INT/ENUM */
  float min_float, max_float, default_float;   /* FLOAT/VEC2 components */

  /* ENUM only: `enum_count` labels, index 0 maps to min_value. */
  const char *const *enum_labels;
  uint32_t enum_count;

  /* Optional: a colour the timeline uses for this field's lane. */
  ft_color color;
} ft_input_field;

/* A key-bindable action exposed by a game. Controls deliberately refer to a
 * schema field by index: the engine can record them without knowing that a
 * button means "jump", "accelerate", "push the ball", or anything else.
 *
 * Held controls reset their target field to its schema default every frame,
 * then write `value` while down. Pressed controls only write on the edge and
 * therefore preserve values such as a selected weapon between ticks. A field
 * marked FT_INPUT_FLAG_TRIGGER latches that edge until one timeline tick
 * consumes it, then returns to its schema default.
 * Multiple FT_CONTROL_ADD controls may target one numeric field (e.g. -1/+1
 * steering).
 */
enum ft_input_control_flags {
  FT_CONTROL_PRESSED = 1u << 0,
  FT_CONTROL_ADD = 1u << 1,
};

typedef struct ft_input_control {
  const char *id; /* stable, used in the user's config */
  const char *display_name;
  const char *description;     /* tooltip, may be NULL */
  const char *category;        /* controls-window heading, may be NULL */
  /* One or more defaults separated by `|`, e.g. "A", "Ctrl+R" or
   * "UpArrow|W". Each becomes a binding on this same action. */
  const char *default_binding;
  uint32_t field;              /* index into ft_input_schema.fields */
  int32_t value;               /* value written through input_set */
  uint32_t flags;              /* ft_input_control_flags */
  /* Optional default for the linked-track binding bank. NULL leaves the
   * linked form unbound; users may still bind it in Controls. */
  const char *linked_default_binding;
} ft_input_control;

typedef struct ft_input_schema {
  uint32_t struct_size;
  /* Size and alignment of one input record. The engine allocates arrays of
   * this stride; it must be stable for the lifetime of the module. */
  uint32_t record_size;
  uint32_t record_align;
  const ft_input_field *fields;
  uint32_t field_count;
  /* The complete set of game-specific recording controls. A game may expose
   * no controls and rely entirely on snippet editing or its own UI. */
  const ft_input_control *controls;
  uint32_t control_count;
} ft_input_schema;

/* Game-defined helpers that only make sense while authoring a linked track.
 * Ordinary buttons and axes do not belong here: the engine exposes a linked
 * binding for every ft_input_control already. This table is for derived
 * operations such as DDNet's "aim at the source player". */
typedef struct ft_linked_action {
  const char *id;
  const char *display_name;
  const char *description;
  const char *default_binding;
} ft_linked_action;

enum ft_linked_transform_flags {
  FT_LINKED_MIRROR_X = 1u << 0,
  FT_LINKED_MIRROR_Y = 1u << 1,
};

typedef struct ft_linked_input_frame {
  uint32_t struct_size;
  const ft_world *world;
  int32_t source_player;
  int32_t target_player;
  const void *source_input;
  uint64_t actions_down;
  uint64_t actions_pressed;
} ft_linked_input_frame;

/* -------------------------------------------------------------------------
 * Game-owned input effects
 * -------------------------------------------------------------------------
 *
 * The editor owns only the ordered list attached to a snippet, its opaque
 * parameter bytes, undo/persistence and derived-input caches. Effect meaning,
 * defaults, evaluation and parameter UI all belong to the active game. This
 * deliberately allows two games to expose completely unrelated effect sets.
 */

#define FT_INPUT_EFFECT_PARAMETER_MAX 4096u
#define FT_INPUT_EFFECT_RUNTIME_MAX 4096u

typedef struct ft_input_effect_desc {
  uint32_t struct_size;
  const char *id; /* stable project identifier */
  const char *display_name;
  const char *description; /* optional, used by the game's editor UI */
  uint32_t parameter_size; /* opaque bytes persisted by the editor */
  uint32_t runtime_size;   /* opaque, zeroed bytes rebuilt at evaluation */
} ft_input_effect_desc;

/* Capabilities supplied by the editor while one effect is being evaluated.
 * Ticks are local to `world_index`; the editor translates world start offsets.
 * Returned worlds and inputs already include every earlier enabled effect in
 * timeline order, including earlier effects on this snippet. */
typedef struct ft_input_effect_frame {
  uint32_t struct_size;
  const ft_level *level;
  int32_t track_index; /* editor track index */
  int32_t world_index;
  int32_t player;     /* player index inside this world */
  int32_t start_tick; /* inclusive snippet window */
  int32_t end_tick;   /* exclusive snippet window */
  int32_t authored_end_tick;
  uint32_t player_count;
  uint32_t record_count;
  uint32_t record_stride;
  void *timeline_user;
  bool (*input_at_tick)(void *timeline_user, int32_t player, int32_t tick, void *out_record);
  const ft_world *(*world_at_tick)(void *timeline_user, int32_t tick);
  void (*reset_simulation)(void *timeline_user);
} ft_input_effect_frame;

/* Small, game-independent location record for the game's parameter editor.
 * The module adopts the engine's ImGui context and renders whatever controls
 * make sense for this effect. */
typedef struct ft_input_effect_ui_frame {
  uint32_t struct_size;
  int32_t track_index;
  int32_t world_index;
  int32_t player;
  int32_t start_tick;
  int32_t end_tick;
} ft_input_effect_ui_frame;

/* -------------------------------------------------------------------------
 * Levels and worlds
 * ------------------------------------------------------------------------- */

typedef struct ft_level_info {
  uint32_t struct_size;
  const char *name;
  /* Playfield bounds in world units. The engine uses them to clamp the camera
   * and to fit the view when a level loads. */
  ft_rect bounds;
  int32_t width_tiles, height_tiles; /* 0 when the game is not tile based */
  /* Where players start, used when the engine creates a fresh world. */
  ft_vec2 default_spawn;
} ft_level_info;

typedef struct ft_world_desc {
  uint32_t struct_size;
  const ft_level *level;
  const char *variant_id; /* NULL selects the first variant */
  int32_t player_count;   /* clamped by the engine to the constraints */
  /* Which of the editor's worlds this belongs to, matching
   * ft_render_frame.world_index. The engine creates many copies of the same
   * world for caching and rewinding, and they all carry the same index. A game
   * that keeps per-world presentation state (particles, sounds, trails) uses
   * it to route effects to the right one.
   *
   * -1 marks a scratch world: one the engine simulates to answer a question
   * rather than to show. A game must not raise effects from those, or a lookup
   * the user never sees would fill the screen with them. */
  int32_t world_index;
} ft_world_desc;

/* What the engine itself needs to know about a player to do its own job:
 * follow the camera, draw prediction lines, show a position readout. Anything
 * richer belongs in the property tables. */
enum ft_player_flags {
  FT_PLAYER_ALIVE = 1u << 0,
  FT_PLAYER_FINISHED = 1u << 1,
  FT_PLAYER_DISABLED = 1u << 2, /* frozen, stunned, otherwise not in control */
};

typedef struct ft_player_view {
  uint32_t struct_size;
  ft_vec2 position;
  ft_vec2 velocity;
  ft_vec2 aim; /* world-space aim/target, zero when the game has none */
  uint32_t flags;
  /* Tick this player's timed run began on, or -1 before it starts. Every game a
   * TAS tool cares about has this notion (crossing a start line, leaving the
   * gate, taking the first step) and the editor uses it to line up separate
   * attempts on a common origin. */
  int32_t run_start_tick;
} ft_player_view;

/* Everything the engine knows about who a player is.
 *
 * Which aspects of a player are worth customising is a question only a game can
 * answer: DDNet wants a nickname, a clan, a skin and two body colours, a racing
 * game wants a livery and a car, and neither list is one the editor should be
 * carrying. So the engine keeps exactly one identity of its own, the track's
 * name, which it needs for its lists and its undo entries, and treats the rest
 * as opaque bytes it stores on the game's behalf.
 *
 * That profile is written by the game through `set_player_profile`, normally
 * from a panel it draws itself, and comes back here and in every render frame.
 * The engine copies it into the project, snapshots it for undo and carries it
 * when a track is duplicated, without ever looking inside. */
typedef struct ft_player_setup {
  uint32_t struct_size;
  /* The editor's label for this track. Engine-owned; a game that wants a
   * nickname of its own keeps one in its profile. */
  const char *track_name;
  /* The game's own profile bytes, or NULL with `data_size` 0 when this track
   * has never been given one, which a game reads as "use your defaults". Valid
   * until the next engine call. */
  const void *data;
  uint32_t data_size;
  /* Index of the player this one mirrors, or -1. Only meaningful with
   * FT_CAP_LINKED_INPUTS. */
  int32_t linked_player;
} ft_player_setup;

/* Big enough for a nickname, a clan, a skin name and a palette, while keeping
 * per-track profiles fixed-size and trivially copyable for undo snapshots. A
 * game needing more than this wants module project data instead. */
#define FT_PLAYER_PROFILE_MAX 512u

/* -------------------------------------------------------------------------
 * Property reflection
 * ------------------------------------------------------------------------- */

enum ft_prop_flags {
  FT_PROP_WRITABLE = 1u << 0, /* the engine may offer an editor for it */
  FT_PROP_STARTING = 1u << 1, /* meaningful as a starting-state override */
  FT_PROP_SUMMARY = 1u << 2,  /* worth showing in the compact status bar */
  FT_PROP_READ_ONLY_UI = 1u << 3,
};

typedef struct ft_prop_desc {
  const char *id; /* stable; used by scripts, plugins and project files */
  const char *display_name;
  const char *group; /* optional heading, e.g. "Movement" */
  const char *unit;  /* optional suffix, e.g. "units/tick" */
  ft_value_kind kind;
  uint32_t flags;              /* ft_prop_flags */
  double min_value, max_value; /* editor clamp; equal means unbounded */
} ft_prop_desc;

/* Entity classes a game exposes for inspection. Players are always class 0. */
#define FT_ENTITY_CLASS_PLAYER 0u

typedef struct ft_entity_class {
  const char *id;
  const char *display_name;
  const ft_prop_desc *props;
  uint32_t prop_count;
} ft_entity_class;

/* -------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------
 *
 * Presentation options that belong to a game (whether to draw its weapons, how
 * long its trajectory preview runs) but that the user expects to find in the
 * editor's own settings menu. The game describes them; the engine renders the
 * controls, stores the values in its config file under the game's id, and hands
 * changes back. That way a module needs no UI toolkit of its own to be
 * configurable. */

typedef struct ft_setting_desc {
  const char *id; /* stable; used as the config key */
  const char *display_name;
  const char *description; /* tooltip, may be NULL */
  const char *group;       /* optional heading, e.g. "Rendering" */
  ft_value_kind kind;      /* BOOL, INT or FLOAT */
  double min_value, max_value;
} ft_setting_desc;

/* -------------------------------------------------------------------------
 * Timeline events
 * ------------------------------------------------------------------------- */

typedef struct ft_timeline_event {
  uint32_t struct_size;
  /* Index of the timeline world this event belongs to. A game leaves this at
   * -1 while reporting simulation events through collect_events; the engine
   * supplies the world that was being scanned. Authored events use an explicit
   * world index. */
  int32_t world_index;
  int32_t tick;
  int32_t player;       /* -1 for world-wide events */
  const char *category; /* "chat", "death", "checkpoint", ... */
  const char *text;
  ft_color color;
  /* Optional game-defined, pointer-free payload. The engine copies it into the
   * project and gives it back unchanged. This is what lets a DDNet module keep
   * editable protocol messages without teaching the engine their fields. */
  const void *data;
  uint32_t data_size;
} ft_timeline_event;

/* Large enough for rich protocol events while still keeping timeline records
 * fixed-size and trivially copyable for undo snapshots. */
#define FT_TIMELINE_EVENT_DATA_MAX 640u

/* -------------------------------------------------------------------------
 * Exporters
 * ------------------------------------------------------------------------- */

typedef struct ft_exporter_desc {
  const char *id;
  const char *display_name;
  const char *file_extension; /* without the dot */
  const char *filter_name;
} ft_exporter_desc;

typedef struct ft_export_request {
  uint32_t struct_size;
  const char *path;
  int32_t start_tick, end_tick;
  /* Players to include, as engine track indices mapped to world player
   * indices. NULL means every player. */
  const int32_t *players;
  uint32_t player_count;
  /* Called by the game to report progress in [0,1]; may be NULL. */
  void (*progress)(void *user, float fraction, const char *status);
  void *progress_user;
} ft_export_request;

/* A timeline may contain multiple independent worlds. Exporters query them
 * through the engine service table instead of reaching into editor structs. */
typedef struct ft_timeline_world_info {
  uint32_t struct_size;
  int32_t world_index;
  int32_t start_offset;
  uint32_t player_count;
  /* Engine-owned, valid until the timeline is mutated. */
  const char *name;
} ft_timeline_world_info;

typedef struct ft_directory_entry {
  const char *name;
  bool is_directory;
} ft_directory_entry;

/* Return false to stop visiting the directory early. */
typedef bool (*ft_directory_visitor)(void *user, const ft_directory_entry *entry);

/* -------------------------------------------------------------------------
 * Engine services offered to the module
 * ------------------------------------------------------------------------- */

typedef enum ft_log_level {
  FT_LOG_TRACE = 0,
  FT_LOG_INFO = 1,
  FT_LOG_WARN = 2,
  FT_LOG_ERROR = 3,
} ft_log_level;

typedef enum ft_texture_format {
  FT_TEXTURE_RGBA8 = 0,
  /* Two channels, eight bits each. Games use it for masks and weight maps that
   * would waste three quarters of an RGBA array, such as the grayscale sheets
   * behind recolourable characters. */
  FT_TEXTURE_RG8 = 1,
} ft_texture_format;

typedef struct ft_texture_desc {
  uint32_t struct_size;
  const void *pixels; /* may be NULL to allocate uninitialised */
  uint32_t width, height;
  uint32_t layers; /* 1 for a plain 2D texture, >1 for an array */
  ft_texture_format format;
  bool mipmaps;
  bool linear_filter;
} ft_texture_desc;

/* The graphics API the engine's device belongs to. A module compares this
 * against what its own renderer can adopt before touching any handle below. */
typedef enum ft_gpu_api {
  FT_GPU_API_NONE = 0,
  FT_GPU_API_VULKAN = 1,
} ft_gpu_api;

/* The engine's graphics device, handed over as opaque handles so this header
 * stays free of any graphics API's types. Every pointer is the corresponding
 * Vulkan handle when `api` is FT_GPU_API_VULKAN: VkInstance, VkPhysicalDevice,
 * VkDevice, VkQueue. A module casts them back itself.
 *
 * The queue is the one the engine records its own frame on. A module that
 * submits there is ordered against the engine's frame for free. */
typedef struct ft_gpu_device {
  uint32_t struct_size;
  ft_gpu_api api;
  void *instance;
  void *physical_device;
  void *device;
  void *queue;
  uint32_t queue_family_index;
  /* Vulkan API version the instance was created with, in VK_MAKE_VERSION form.
   * A renderer being handed the device needs it to build its own tables. */
  uint32_t api_version;
} ft_gpu_device;

/* The image behind an ft_texture, for a module rendering into engine memory. */
typedef struct ft_gpu_image {
  uint32_t struct_size;
  /* VkImage when the device reports FT_GPU_API_VULKAN. */
  void *image;
  /* VkFormat as a plain integer, so no Vulkan header is needed to read it. */
  uint64_t format;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  /* VkImageLayout the engine expects to find the image in when it samples it,
   * and therefore the layout the module must leave it in. The engine reports
   * what it will transition from rather than dictating a value mid-frame. */
  uint32_t layout;
} ft_gpu_image;

/* A sprite atlas: one texture plus a sprite table. The engine batches every
 * draw that shares an atlas, which is how a game gets thousands of particles or
 * tiles on screen without touching the graphics API itself. */
typedef struct ft_sprite_rect {
  uint32_t x, y, w, h;
} ft_sprite_rect;

typedef struct ft_atlas_desc {
  uint32_t struct_size;
  ft_texture *texture;
  const ft_sprite_rect *sprites;
  uint32_t sprite_count;
  uint32_t max_instances_per_frame;
} ft_atlas_desc;

typedef struct ft_sprite_draw {
  ft_vec2 pos;  /* world-space centre */
  ft_vec2 size; /* world-space size */
  float rotation;
  uint32_t sprite_index;
  ft_color color;
  ft_vec2 tiling; /* {1,1} for no repetition */
} ft_sprite_draw;

/* Escape hatch for effects the generic sprite path cannot express (DDNet's tee
 * skin compositing, a tile-map shader). The game ships SPIR-V and an instance
 * layout; the engine builds the pipeline and feeds it instances. */
typedef enum ft_vertex_format {
  FT_VERTEX_FLOAT1 = 0,
  FT_VERTEX_FLOAT2 = 1,
  FT_VERTEX_FLOAT3 = 2,
  FT_VERTEX_FLOAT4 = 3,
  FT_VERTEX_INT1 = 4,
  FT_VERTEX_UINT1 = 5,
} ft_vertex_format;

typedef struct ft_vertex_attr {
  uint32_t location;
  uint32_t offset;
  ft_vertex_format format;
} ft_vertex_attr;

/* The vertex the engine's mesh path stores. A module building geometry for
 * draw_mesh fills these; the layout is fixed so mesh_create needs no
 * description of it. */
typedef struct ft_vertex {
  ft_vec2 pos;
  float color[3];
  ft_vec2 uv;
} ft_vertex;

typedef struct ft_pipeline_desc {
  uint32_t struct_size;
  const void *vertex_spirv;
  size_t vertex_spirv_size;
  const void *fragment_spirv;
  size_t fragment_spirv_size;
  /* Per-instance attribute layout. */
  const ft_vertex_attr *instance_attrs;
  uint32_t instance_attr_count;
  /* Zero asks for a mesh pipeline instead of an instanced one: attributes are
   * read straight from the mesh's vertices and there is no per-instance data.
   * That is what a level pass wants, where one large mesh is drawn once. */
  uint32_t instance_stride;
  uint32_t max_instances_per_frame;
  uint32_t texture_count; /* combined image samplers bound at draw time */
  bool alpha_blend;
} ft_pipeline_desc;

/* Describes a small, immediate offscreen instance draw. It uses the exact
 * pipeline and textures supplied by the game, with preview coordinates centred
 * at {0,0}: a unit quad at scale 1 fills the target. This is intended for
 * shader-accurate thumbnails in game-owned UI, not normal viewport rendering.
 * Without a destination, the returned texture is newly owned by the caller;
 * with one, the same caller-owned destination handle is returned. */
typedef struct ft_texture_layer_update {
  ft_texture *texture;
  uint32_t layer;
  const void *pixels;
  uint32_t width;
  uint32_t height;
} ft_texture_layer_update;

typedef struct ft_instance_preview_desc {
  uint32_t struct_size;
  ft_pipeline *pipeline;
  ft_texture *const *textures;
  uint32_t texture_count;
  const void *instances;
  uint32_t instance_count;
  uint32_t width;
  uint32_t height;
  ft_color clear_color;
  /* Optional texture-array updates recorded immediately before the draw in
   * the same GPU submission. This avoids stalling once per streamed layer when
   * a preview needs freshly generated material. Pixel data only has to remain
   * valid for the duration of this call. */
  const ft_texture_layer_update *updates;
  uint32_t update_count;
  /* When non-NULL, renders the preview into this texture at the given pixel
   * offset and returns the destination instead of allocating a separate result
   * texture. Intended for thumbnail atlas pages. */
  ft_texture *destination;
  uint32_t destination_x;
  uint32_t destination_y;
} ft_instance_preview_desc;

typedef struct ft_camera {
  uint32_t struct_size;
  ft_vec2 position;
  float zoom;
  float aspect;
  /* Which camera mode is active, indexing constraints.camera_modes. A game
   * compares it against its own list to decide things like whether to draw a
   * crosshair. For a 3D game the engine appends its own freecam after the
   * declared modes, so this may be camera_mode_count: check the range before
   * using it as an index. */
  uint32_t mode;
  /* Size of the render target in pixels. A game needs it to turn screen
   * coordinates into world ones for its own culling, which is otherwise
   * impossible to do correctly from the outside. */
  ft_vec2 viewport;
  /* World-space rectangle currently visible, already computed for the common
   * case. Cull against this rather than deriving it. Meaningful for a 2D game;
   * a 3D one culls against the frustum implied by view_proj instead. */
  ft_rect visible;

  /* --- 3D ---
   * Filled for a game that declared FT_DIMENSIONS_3D, and left zeroed
   * otherwise. `position` and `zoom` above still track the camera's ground
   * position and distance, so an editor control that only understands the
   * plane keeps working. */
  ft_vec3 eye;    /* camera position in world space */
  ft_vec3 target; /* the point it looks at */
  ft_vec3 up;
  float fov_y; /* vertical field of view, radians */
  float near_z;
  float far_z;
  /* View-projection the engine renders with, column-major, ready to hand to a
   * shader. A game rendering with its own device needs exactly this to line its
   * frame up with the engine's own drawing. */
  float view_proj[16];
} ft_camera;

/* Read-only snapshot of engine state a module may want while drawing or
 * building UI. Handed to render/UI callbacks rather than fetched, so it stays
 * consistent for the whole call. */
typedef struct ft_engine_state {
  uint32_t struct_size;
  int32_t current_tick;
  bool playing;
  bool recording;
  bool headless;
  int32_t selected_player; /* -1 when nothing is selected */
  ft_camera camera;
  /* The texture LOD bias the user set, in mip levels: negative sharpens,
   * positive blurs. The engine applies it to everything it draws itself. A game
   * that samples through pipelines of its own has to apply it there, or its
   * level stops answering the setting the moment it takes over that drawing. */
  float lod_bias;
} ft_engine_state;

/* Services the engine exposes to a game module. Every pointer is non-NULL for
 * the lifetime of the module, except where noted for headless runs. */
typedef struct ft_engine_api {
  uint32_t struct_size;

  /* --- diagnostics --- */
  void (*log)(ft_log_level level, const char *category, const char *message);

  /* --- filesystem ---
   * Resolves a path relative to this game's data directory beside the running
   * executable (<executable>/data/games/<id>/...). Returns the number of bytes
   * needed; truncates when out_size is too small. */
  size_t (*resolve_data_path)(const char *relative, char *out, size_t out_size);
  /* Reads a whole file. The buffer must be released with free_file_data. */
  bool (*read_file)(const char *path, void **out_data, size_t *out_size);
  void (*free_file_data)(void *data);
  /* Resolves a path inside this game's cache directory, under the user's
   * config location rather than the install. Anything downloaded or generated
   * belongs here: levels fetched from the network, thumbnails, indexes. The
   * directory is created if it does not exist. */
  size_t (*resolve_cache_path)(const char *relative, char *out, size_t out_size);

  /* --- resources (NULL in headless runs) --- */
  ft_texture *(*texture_create)(const ft_texture_desc *desc);
  void (*texture_destroy)(ft_texture *texture);
  /* Replaces one layer of an array texture, so a game can stream appearances
   * (skins, liveries, character sheets) in and out of a texture array without
   * rebuilding it. `pixels` must hold width*height texels of the texture's
   * format. Returns false when the layer or size does not fit. */
  bool (*texture_update_layer)(ft_texture *texture, uint32_t layer, const void *pixels, uint32_t width, uint32_t height);
  ft_atlas *(*atlas_create)(const ft_atlas_desc *desc);
  void (*atlas_destroy)(ft_atlas *atlas);
  ft_pipeline *(*pipeline_create)(const ft_pipeline_desc *desc);
  void (*pipeline_destroy)(ft_pipeline *pipeline);
  ft_mesh *(*mesh_create)(const void *vertices, uint32_t vertex_count, uint32_t vertex_stride, const uint32_t *indices,
                          uint32_t index_count);
  void (*mesh_destroy)(ft_mesh *mesh);

  /* --- drawing (only valid inside render callbacks) ---
   * `z` orders draws back-to-front across the whole frame, so a game can
   * interleave its own layers with engine-drawn overlays. */
  void (*draw_sprites)(ft_atlas *atlas, float z, const ft_sprite_draw *draws, uint32_t count);
  void (*draw_line)(float z, ft_vec2 a, ft_vec2 b, ft_color color, float thickness);
  void (*draw_rect)(float z, ft_vec2 pos, ft_vec2 size, ft_color color);
  /* `segments` subdivides one quarter-circle corner. The radius is clamped to
   * half the shorter side, and a radius of zero draws a plain rect. */
  void (*draw_rect_rounded)(float z, ft_vec2 pos, ft_vec2 size, float radius, ft_color color, uint32_t segments);
  void (*draw_circle)(float z, ft_vec2 center, float radius, ft_color color, uint32_t segments);
  void (*draw_triangle)(float z, ft_vec2 a, ft_vec2 b, ft_vec2 c, ft_color color);
  void (*draw_text)(float z, ft_vec2 pos, float size, ft_color color, const char *text);

  /* --- drawing in three dimensions (only valid inside render callbacks) ---
   * The same primitives, positioned in the volume a 3D game lives in. These
   * are depth-tested against each other and against anything else drawn in 3D,
   * so there is no `z` ordering argument: depth comes from the geometry. A 2D
   * game never calls them, and a 3D game can still use the 2D calls above for
   * screen-space overlays. */
  void (*draw_line3)(ft_vec3 a, ft_vec3 b, ft_color color, float thickness);
  void (*draw_triangle3)(ft_vec3 a, ft_vec3 b, ft_vec3 c, ft_color color);
  /* The world's textures, as one array texture whose layers a game addresses
   * per triangle. Set it once per frame before drawing; passing NULL goes back
   * to untextured. The texture must have been created through texture_create
   * with `layers` > 0 and stays owned by the game. */
  void (*set_texture3)(ft_texture *texture);
  /* A textured triangle. `layer` selects a layer of the texture set above, and
   * `tint` multiplies what is sampled: white leaves the texture as authored.
   * Coordinates are taken as given and repeat outside [0,1], which is what a
   * road surface tiling along a track needs. Out-of-range layers, or no texture
   * set, draw the tint alone rather than sampling nothing.
   *
   * Textured and untextured 3D draws share one depth-tested stream, so they can
   * be interleaved freely and resolve against each other by geometry. */
  void (*draw_triangle3_textured)(ft_vec3 a, ft_vec3 b, ft_vec3 c, ft_vec2 uv_a, ft_vec2 uv_b, ft_vec2 uv_c,
                                  uint32_t layer, ft_color tint);
  /* An axis-aligned box given by its centre and full extents. Filled when
   * `wire` is false, twelve edges when true: the shape an editor wants for
   * bounds, triggers and hitboxes. */
  void (*draw_box3)(ft_vec3 center, ft_vec3 size, ft_color color, bool wire);
  void (*draw_instances)(ft_pipeline *pipeline, float z, ft_texture *const *textures, uint32_t texture_count,
                         const void *instances, uint32_t count);
  void (*draw_mesh)(ft_pipeline *pipeline, float z, ft_mesh *mesh, ft_texture *const *textures, uint32_t texture_count,
                    const void *uniforms, size_t uniform_size);

  /* --- camera and coordinates --- */
  void (*camera_get)(ft_camera *out);
  void (*camera_set)(const ft_camera *camera);
  ft_vec2 (*screen_to_world)(ft_vec2 screen);
  ft_vec2 (*world_to_screen)(ft_vec2 world);

  /* --- UI ---
   * The ImGui context the engine renders with, or NULL headless. A module that
   * wants panels links its own ImGui/cimgui and adopts this context. The
   * fonts are the engine's; igGetIO/igSetCurrentContext work as usual. */
  void *(*imgui_context)(void);
  /* ImGui allocator functions matching the engine's, so a module's ImGui
   * allocates from the same heap. Both may be NULL headless. */
  void (*imgui_allocators)(void **alloc_fn, void **free_fn, void **user_data);

  /* --- engine feedback --- */
  /* Asks the editor to open a level by path, exactly as if the user had picked
   * it. A game's start screen uses this once the user chooses something. */
  bool (*request_level)(const char *path);
  /* An ImGui texture handle for a texture the game created, so a module's own
   * panels can draw it. Zero when the texture cannot be shown. The value is an
   * ImTextureID; wrap it in an ImTextureRef to use it. Each successful call
   * owns one handle which must be released separately before its texture. */
  uint64_t (*imgui_texture_id)(ft_texture *texture);
  void (*imgui_texture_release)(uint64_t texture_id);
  /* Marks the project dirty after the game changed something the engine
   * stores (starting state overrides, player setups). */
  void (*mark_dirty)(void);
  /* Asks the engine to drop cached simulation from `tick` onwards, e.g. after
   * the game mutated a starting world behind the engine's back. */
  void (*invalidate_simulation)(int32_t tick);
  /* Current engine state outside a callback that already carries it. */
  void (*get_state)(ft_engine_state *out);
  /* Input the engine currently holds for a player at a tick, in the game's own
   * record format. Returns false when the player or tick is out of range. */
  bool (*get_player_input)(int32_t player, int32_t tick, void *out_record);
  /* Native file dialogs; return false when cancelled or headless. */
  bool (*save_file_dialog)(const char *filter_name, const char *filter_ext, const char *default_name, char *out_path,
                           size_t out_size);
  bool (*open_file_dialog)(const char *filter_name, const char *filter_ext, char *out_path, size_t out_size);

  /* Enumerates one directory synchronously. Returns the number of entries
   * delivered to `visitor`; zero also covers a directory that cannot be read. */
  uint32_t (*visit_directory)(const char *path, ft_directory_visitor visitor, void *user);

  /* Player numbers here are editor track indices, matching
   * ft_engine_state.selected_player. Strings and profile bytes in `out` remain
   * engine-owned and are valid until the next engine call. */
  bool (*get_player_setup)(int32_t player, ft_player_setup *out);
  /* Replaces one track's profile with `size` bytes of the game's own making,
   * at most FT_PLAYER_PROFILE_MAX, and marks the project dirty. Passing a size
   * of 0 clears it back to "no profile", which a game reads as its defaults. */
  bool (*set_player_profile)(int32_t player, const void *data, uint32_t size);

  /* Draws the editor for what a player starts as (every property the game
   * flagged FT_PROP_STARTING) at the current ImGui cursor, inside the caller's
   * own window. The editor is the engine's: it stores the overrides on the
   * track, saves them with the project and writes them into the starting world
   * through this game's property table.
   *
   * A game calls this to keep the editor next to its own controls. One that
   * does not gets the same editor in a window of its own, so the feature is
   * never missing. Returns false when there is nothing to draw. */
  bool (*starting_state_editor)(int32_t player);

  /* Read-only timeline access for exporters. World pairs are owned by the
   * engine and remain valid only until the next timeline query. `global_tick`
   * includes the world's start offset. */
  uint32_t (*timeline_world_count)(void);
  bool (*timeline_world_info)(uint32_t world_index, ft_timeline_world_info *out);
  bool (*timeline_world_pair)(uint32_t world_index, int32_t global_tick, const ft_world **out_previous, const ft_world **out_current);
  int32_t (*timeline_player_track)(uint32_t world_index, uint32_t local_player);

  /* Immediately renders instances through the game's existing pipeline into a
   * sampled 2D texture. Valid outside render callbacks, including game UI. */
  ft_texture *(*render_instances_preview)(const ft_instance_preview_desc *desc);

  /* --- games that bring their own renderer ---
   *
   * Everything above expresses a frame through the engine's renderer. A game
   * built on an engine that owns a renderer of its own (Bevy, raylib, anything
   * with its own device and pipelines) does not want that. These two calls let
   * such a module draw its frame itself and hand over the result.
   *
   * The engine's device, for a module whose renderer can adopt an existing one
   * rather than create its own. wgpu can (wgpu::hal), OpenGL cannot. NULL in
   * headless runs and whenever the engine has no graphics. */
  const ft_gpu_device *(*gpu_device)(void);
  /* The backing image of a texture the module created, so a module rendering on
   * the engine's device can target engine memory directly instead of copying
   * pixels through texture_update_layer. Returns false when the engine has no
   * graphics or the texture has no image.
   *
   * Calling this marks the texture externally rendered, which changes who owns
   * its contents: the engine stops assuming it left the image in a sampleable
   * layout and instead transitions it from `ft_gpu_image.layout` before each
   * frame that samples it. A module must therefore leave the image in the
   * layout it reported, every frame, and must submit its own work to the queue
   * in ft_gpu_device before returning from its render callback. Work submitted
   * to that queue is ordered ahead of the engine's own frame, which is what
   * makes a plain barrier sufficient and avoids any cross-device semaphore. */
  bool (*texture_gpu_image)(ft_texture *texture, ft_gpu_image *out);
  /* Draws a texture as a single quad, sampling it as it stands this frame.
   * This is how a module hands back a frame it rendered itself, and it is
   * deliberately not the atlas path: an atlas copies its sprites into a private
   * array once, which is right for a sprite sheet and wrong for an image whose
   * contents change every frame. `dst` is in world units; `tint` multiplies. */
  void (*draw_texture)(float z, ft_texture *texture, ft_rect dst, ft_color tint);

  /* --- authored timeline events ---
   * Games that have format-specific event payloads manage them through these
   * services. Returned strings and data remain engine-owned until the next
   * event mutation. add/update copy every pointed-to field immediately. */
  uint32_t (*timeline_event_count)(void);
  bool (*timeline_event_get)(uint32_t index, ft_timeline_event *out);
  bool (*timeline_event_add)(const ft_timeline_event *event);
  bool (*timeline_event_update)(uint32_t index, const ft_timeline_event *event);
  bool (*timeline_event_remove)(uint32_t index);
  int32_t (*timeline_active_world)(void);
  /* Inclusive global range covered by the authored timeline. */
  bool (*timeline_range)(int32_t *out_start_tick, int32_t *out_end_tick);
  /* True only when a world step is advancing presentation visible in the
   * viewport. Export/scanning queries step the same physics with this false so
   * they cannot fill or rewind particle, sound or trail state. */
  bool (*presentation_effects_enabled)(void);
} ft_engine_api;

/* The layer a 3D triangle names when it carries no texture. */
#define FT_LAYER_UNTEXTURED 0xFFFFFFFFu

/* -------------------------------------------------------------------------
 * Render and UI callback payloads
 * ------------------------------------------------------------------------- */

typedef enum ft_render_pass {
  FT_PASS_LEVEL_BACKGROUND = 0,
  FT_PASS_ENTITIES = 1,
  FT_PASS_LEVEL_FOREGROUND = 2,
  FT_PASS_OVERLAY = 3, /* HUD-ish world-space overlays, drawn above everything */
} ft_render_pass;

typedef struct ft_render_frame {
  uint32_t struct_size;
  ft_render_pass pass;
  const ft_level *level;
  /* World at the current tick, and the tick before it. `alpha` in [0,1]
   * interpolates between them so rendering stays smooth between ticks. */
  const ft_world *world;
  const ft_world *previous_world;
  float alpha;
  /* Local simulation tick of `world`. This can differ from
   * ft_engine_state.current_tick when the world has a timeline start offset. */
  int32_t tick;
  /* Per-player presentation data the engine owns, indexed by player. */
  const ft_player_setup *player_setups;
  uint32_t player_setup_count;
  ft_engine_state state;
  /* Opacity the engine wants applied, used for prediction ghosts. */
  float opacity;
  /* Colour the engine associates with this world, so a game's own markers and
   * trajectory lines match the timeline group they belong to. */
  ft_color accent;
  /* Index of the selected player *within this world*, or -1 when the selection
   * lives in another world. Distinct from ft_engine_state.selected_player,
   * which is the editor's own track numbering across every world. */
  int32_t selected_player;
  /* Index of this world among the editor's groups, or -1 for shared level
   * passes, and how many worlds there are. A
   * group is another instance of the same simulation, so every game may see
   * more than one. */
  int32_t world_index;
  int32_t world_count;
  /* True when this world owns the editor's focus: the one whose player is
   * selected, whose HUD is shown, and which the camera follows. */
  bool active;
} ft_render_frame;

/* Everything a game needs to place the camera for a frame. */
typedef struct ft_camera_frame {
  uint32_t struct_size;
  uint32_t mode; /* index into constraints.camera_modes */
  const ft_world *world;
  const ft_world *previous_world;
  /* Interpolation between the two worlds, matching what rendering uses. A
   * directed camera must apply it or it will visibly stutter against the
   * smoothly drawn world. */
  float alpha;
  int32_t player; /* selected player within this world, or -1 */
  bool recording;
} ft_camera_frame;

typedef enum ft_ui_slot {
  FT_UI_MAIN_MENU = 0, /* inside the engine's menu bar */
  /* The game's own windows, each opened with its own igBegin. Anything the
   * editor has no opinion about lives here, the player panel included: what a
   * player can be customised into is the game's business, so the game draws
   * that window itself rather than filling in fields the engine invented. */
  FT_UI_PANELS = 1,
  FT_UI_SETTINGS = 2,   /* inside the engine's settings window */
  FT_UI_PLAYER_ROW = 3, /* inline, next to a timeline track's controls */
  FT_UI_STATUS_BAR = 4, /* inline, in the engine's status bar */
  /* The start screen, after the user has picked this game. Whatever a run
   * begins with belongs here: a level browser, a track list, a category picker.
   * The engine draws the general half (games, recent projects) and hands the
   * rest of the panel to the game. */
  FT_UI_SPLASH = 5,
} ft_ui_slot;

typedef struct ft_ui_frame {
  uint32_t struct_size;
  ft_ui_slot slot;
  const ft_world *world;
  int32_t tick;
  /* Which player this frame is about, within `world`: the row's player for
   * FT_UI_PLAYER_ROW, and the selected one everywhere else. -1 when there is
   * none. `state.selected_player` is the same player in the editor's own track
   * numbering, which is what the player-profile calls take. */
  int32_t player;
  ft_engine_state state;
} ft_ui_frame;

/* Where a game's window wants to start out, the first time the editor ever sees
 * it. The editor places it and never again: from then on the window is the
 * user's to move, and its position comes back from the saved layout. */
typedef enum ft_dock_slot {
  FT_DOCK_FLOATING = 0, /* leave it where ImGui puts it */
  FT_DOCK_LEFT = 1,     /* beside the editor's player list */
  FT_DOCK_RIGHT = 2,    /* beside the snippet editor */
  FT_DOCK_BOTTOM = 3,   /* with the timeline */
  FT_DOCK_CENTER = 4,   /* with the viewport */
} ft_dock_slot;

typedef struct ft_panel_desc {
  /* Exactly the title the game passes to igBegin, including any "##id". */
  const char *window_title;
  ft_dock_slot dock;
} ft_panel_desc;

/* -------------------------------------------------------------------------
 * The module vtable
 * ------------------------------------------------------------------------- */

typedef struct ft_game_module {
  uint32_t struct_size;
  uint32_t abi_version; /* must equal FT_GAME_ABI_VERSION */
  uint32_t abi_revision;

  ft_game_info info;
  ft_game_constraints constraints;

  /* Input record layout. Required. */
  const ft_input_schema *input_schema;

  /* Entity classes for inspection; index 0 must be the player class. May be
   * NULL/0 for a game that exposes nothing. */
  const ft_entity_class *entity_classes;
  uint32_t entity_class_count;

  /* ---- lifecycle (required) ----
   * create() is called once after the module is selected. Returning NULL
   * aborts loading; the module should log why. `engine` stays valid until
   * destroy() returns. */
  ft_game *(*create)(const ft_engine_api *engine);
  void (*destroy)(ft_game *game);

  /* Called once per frame before rendering, whether or not anything is
   * playing. Optional. */
  void (*update)(ft_game *game, const ft_engine_state *state);

  /* ---- levels ---- */
  ft_level *(*level_load_path)(ft_game *game, const char *path, const char *variant_id);
  /* Required only with FT_CAP_LEVEL_FROM_MEMORY. */
  ft_level *(*level_load_memory)(ft_game *game, const void *data, size_t size, const char *variant_id);
  void (*level_destroy)(ft_game *game, ft_level *level);
  bool (*level_info)(ft_game *game, const ft_level *level, ft_level_info *out);
  /* Bytes that reload this exact level through level_load_memory. The engine
   * embeds them in project files so a project stays openable when the original
   * file is gone. Same size convention as world_serialize; leave NULL and the
   * engine stores only the level's path. */
  size_t (*level_serialize)(ft_game *game, const ft_level *level, void *out, size_t out_size);

  /* ---- worlds (all required) ---- */
  ft_world *(*world_create)(ft_game *game, const ft_world_desc *desc);
  void (*world_destroy)(ft_game *game, ft_world *world);
  /* Deep copy. dst and src always come from the same game and level. This is
   * on the hot path: the engine copies worlds constantly to cache and rewind,
   * so it should reuse dst's allocations. */
  void (*world_copy)(ft_game *game, ft_world *dst, const ft_world *src);
  /* Advances one tick. `inputs` points at player_count records of
   * input_schema->record_size, one per player, in player index order. */
  void (*world_step)(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count);
  int32_t (*world_tick)(ft_game *game, const ft_world *world);
  int32_t (*world_player_count)(ft_game *game, const ft_world *world);
  bool (*world_player_view)(ft_game *game, const ft_world *world, int32_t player, ft_player_view *out);

  /* ---- players (required with FT_CAP_DYNAMIC_PLAYERS) ----
   * Insert at `at_index`, or append when at_index < 0. Returns the new index
   * or -1. */
  int32_t (*world_add_player)(ft_game *game, ft_world *world, int32_t at_index, const ft_player_setup *setup);
  bool (*world_remove_player)(ft_game *game, ft_world *world, int32_t player);

  /* ---- serialization (required with FT_CAP_WORLD_SERIALIZE) ----
   * world_serialize returns the number of bytes written, or the number
   * required when `out` is NULL. Versioning inside the blob is the game's
   * business; the engine only stores it alongside the game id and version. */
  size_t (*world_serialize)(ft_game *game, const ft_world *world, void *out, size_t out_size);
  bool (*world_deserialize)(ft_game *game, ft_world *world, const void *data, size_t size);

  /* ---- input records ----
   * The engine never interprets a record's bytes, so a game with packed bits
   * or unions stays free to lay them out as it likes. */
  void (*input_default)(ft_game *game, void *record);
  int64_t (*input_get)(ft_game *game, const void *record, uint32_t field);
  void (*input_set)(ft_game *game, void *record, uint32_t field, int64_t value);
  ft_vec2 (*input_get_vec2)(ft_game *game, const void *record, uint32_t field);
  void (*input_set_vec2)(ft_game *game, void *record, uint32_t field, ft_vec2 value);
  /* Optional: a one-line summary of a record for tooltips and the timeline. */
  void (*input_describe)(ft_game *game, const void *record, char *out, size_t out_size);

  /* ---- property reflection (optional) ---- */
  bool (*entity_prop_get)(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                          ft_value *out);
  bool (*entity_prop_set)(ft_game *game, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop,
                          const ft_value *value);
  int32_t (*entity_count)(ft_game *game, const ft_world *world, uint32_t entity_class);

  /* ---- rendering (optional; skipped headless) ----
   * Called once per pass per visible world. A game that draws nothing in a
   * pass simply returns. */
  void (*render)(ft_game *game, const ft_render_frame *frame);
  /* Called when graphics resources must be (re)built, e.g. after the engine
   * recreated its device, and before they are torn down. */
  bool (*resources_create)(ft_game *game);
  void (*resources_destroy)(ft_game *game);

  /* ---- UI (optional; skipped headless) ---- */
  void (*ui)(ft_game *game, const ft_ui_frame *frame);
  /* Default placement for the windows this game opens in FT_UI_PANELS, applied
   * once per window and only while the saved layout has never heard of it. */
  const ft_panel_desc *panels;
  uint32_t panel_count;

  /* ---- timeline events (optional; needs FT_CAP_TIMELINE_EVENTS) ----
   * Called after the engine simulated `world` from the previous tick. The game
   * reports anything worth marking on the timeline by calling `emit` for each
   * event. */
  void (*collect_events)(ft_game *game, const ft_world *previous, const ft_world *world,
                         void (*emit)(void *user, const ft_timeline_event *event), void *user);

  /* ---- exporters (optional; needs FT_CAP_EXPORTERS) ---- */
  uint32_t (*exporter_count)(ft_game *game);
  const ft_exporter_desc *(*exporter_desc)(ft_game *game, uint32_t index);
  /* Exports the inclusive range [start_tick, end_tick]. The game pulls the
   * input and world data it needs through the engine API. */
  bool (*export_run)(ft_game *game, uint32_t index, const ft_export_request *request);

  /* ---- status readout (optional) ----
   * The lines a game wants shown in the editor's viewport overlay for the
   * selected player: positions, speeds, timers, whatever that game considers
   * worth watching frame by frame. The game formats them, so the wording and
   * precision are its own; the engine only draws them.
   *
   * Writes at most `max_lines` NUL-terminated strings of `line_size` bytes into
   * `out` and returns how many it wrote. */
  uint32_t (*status_lines)(ft_game *game, const ft_world *world, int32_t player, float alpha, char *out, uint32_t max_lines,
                           uint32_t line_size);

  /* ---- player label (optional) ----
   * A short line the editor shows next to a player in lists: a finish time, a
   * lap, a placing. Return false to show nothing. */
  bool (*player_label)(ft_game *game, const ft_world *world, int32_t player, char *out, size_t out_size);

  /* ---- camera (optional) ----
   * Positions the camera for the active mode. Return false to leave it exactly
   * as the user left it, which is what a free mode wants. Position is in world
   * units; zoom and aspect are the engine's and should normally be passed
   * through untouched. */
  bool (*camera_update)(ft_game *game, const ft_camera_frame *frame, ft_camera *inout);

  /* ---- settings (optional) ----
   * The engine draws a control per setting in its own menus and persists the
   * values, so a game gets configurable presentation without linking a UI. */
  uint32_t (*setting_count)(ft_game *game);
  const ft_setting_desc *(*setting_desc)(ft_game *game, uint32_t index);
  bool (*setting_get)(ft_game *game, uint32_t index, ft_value *out);
  bool (*setting_set)(ft_game *game, uint32_t index, const ft_value *value);

  /* ---- project data (optional) ----
   * Anything the game wants stored in the .tasp project beyond worlds and
   * inputs: its own settings, per-level metadata. Same size convention as
   * world_serialize. */
  size_t (*project_save)(ft_game *game, void *out, size_t out_size);
  bool (*project_load)(ft_game *game, const void *data, size_t size);

  /* ---- continuous input fields ----
   * FLOAT fields cannot travel through the integer accessors without losing
   * their fractional part. These callbacks are required when the input schema
   * contains FT_INPUT_FLOAT and are ignored for every other field kind. */
  float (*input_get_float)(ft_game *game, const void *record, uint32_t field);
  void (*input_set_float)(ft_game *game, void *record, uint32_t field, float value);

  /* ---- linked-track authoring (optional; needs FT_CAP_LINKED_INPUTS) ---- */
  const ft_linked_action *linked_actions;
  uint32_t linked_action_count;
  void (*linked_input_update)(ft_game *game, const ft_linked_input_frame *frame, void *inout_record);

  /* ---- game-owned input effects (optional) ----
   * Descriptors remain valid for the module lifetime. `input_effect_apply`
   * mutates `record_count` records in `inout_records`, separated by
   * `record_stride` bytes. It must be deterministic, have no external side
   * effects and not retain frame pointers: the editor may restore its output
   * and runtime bytes from a stage cache instead of calling it. The UI
   * callback returns true when it changed the parameter bytes. */
  uint32_t (*input_effect_count)(ft_game *game);
  const ft_input_effect_desc *(*input_effect_desc)(ft_game *game, uint32_t index);
  void (*input_effect_default)(ft_game *game, uint32_t index, void *parameters, uint32_t parameter_size);
  bool (*input_effect_apply)(ft_game *game, uint32_t index, const ft_input_effect_frame *frame,
                             const void *parameters, uint32_t parameter_size, void *runtime,
                             uint32_t runtime_size, void *inout_records);
  bool (*input_effect_ui)(ft_game *game, uint32_t index, const ft_input_effect_ui_frame *frame,
                          void *parameters, uint32_t parameter_size, const void *runtime,
                          uint32_t runtime_size);
} ft_game_module;

/* The one symbol a module must export.
 *
 * `engine_abi_version` is the engine's FT_GAME_ABI_VERSION. Return NULL when
 * the module cannot support it; the engine will report the mismatch. The
 * returned struct must remain valid and immutable for as long as the library
 * stays loaded, so it is normally a pointer to a static. */
typedef const ft_game_module *(*ft_game_module_entry_fn)(uint32_t engine_abi_version);

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FRAMETEE_GAME_ABI_H */
