#ifndef UI_TIMELINE_TYPES_H
#define UI_TIMELINE_TYPES_H

#include <engine/game_host.h>
#include <engine/input_record.h>
#include <engine/prediction.h>
#include <stdbool.h>
#include <system/include_cimgui.h>
#include <types.h>
#include <user_interface/player_profile.h>

#define MAX_SNIPPETS_PER_PLAYER 64
#define MAX_SNIPPET_LAYERS 8
#define MAX_SNIPPET_INPUT_EFFECTS 16
#define MAX_TIMELINE_GROUP_NAME 64
#define MAX_TRACK_NAME 64

// Ring of periodic world snapshots, so scrubbing backwards does not have to
// re-simulate from tick zero. The worlds belong to the active game; the engine
// only ever creates, copies and destroys them.
struct physics_v_t {
  ft_world **data;
  uint32_t current_size;
  uint32_t max_size;
};

typedef struct input_effect_t {
  char type_id[FT_ID_MAX];
  bool enabled;
  uint32_t parameter_size;
  unsigned char *parameters;
  uint32_t runtime_size;
  unsigned char *runtime;
  bool runtime_ok;
} input_effect_t;

typedef struct input_effect_stage_cache_t {
  input_record_t *inputs;
  int input_count;
  unsigned char *runtime;
  uint32_t runtime_size;
  uint64_t key;
  bool valid;
} input_effect_stage_cache_t;

// A snippet is a window onto a source buffer, the way a clip is a window onto its media. Trimming
// an edge or splitting a snippet only moves the window: `inputs` keeps holding every tick that was
// ever recorded for it, so widening the window again brings the original inputs back.
//   inputs[source_offset .. source_offset + input_count)  is what plays, starting at start_tick
struct input_snippet_t {
  int id;
  int start_tick;
  int end_tick; // always start_tick + input_count
  bool is_active;
  int layer;
  input_record_t *inputs; // source buffer, may extend past the window on either side
  int input_count;        // length of the visible window
  int source_offset;      // index in `inputs` of the tick played at start_tick
  int source_count;       // total ticks held in `inputs`

  input_effect_t *effects;
  int effect_count;
  int effect_capacity;

  input_record_t *effect_inputs; // derived visible window, never serialized
  int effect_input_count;
  bool effect_cache_valid;
  input_effect_stage_cache_t *effect_stage_caches;
  int effect_stage_capacity;
};

// An override the user pinned on a track's starting state. The engine stores it
// as a game property id plus a value and applies it through the game's own
// property table, so it never has to know what "active weapon" means.
// Enough for every startable property a bundled game publishes (DDNet's is the
// longest, at 25) with room to spare. Each track carries this array by value and
// undo snapshots clone tracks, so it is sized to fit rather than generously.
#define MAX_STARTING_OVERRIDES 32
#define MAX_STARTING_STRING 256

typedef struct starting_override_t {
  char prop_id[32];
  ft_value value;
  // FT_VALUE_STRING cannot retain a module-owned pointer in project or undo
  // data. String overrides live inline and value.as.s is rebound after moves.
  char string_value[MAX_STARTING_STRING];
} starting_override_t;

struct starting_config_t {
  starting_override_t overrides[MAX_STARTING_OVERRIDES];
  int override_count;
  bool enabled;
};

struct player_track_t {
  input_snippet_t *snippets;
  int snippet_count;
  int snippet_capacity;

  // A temporary buffer for non-destructive recording
  input_snippet_t *recording_snippets;
  int recording_snippet_count;
  int recording_snippet_capacity;

  // The input state for this track for the current frame/tick
  input_record_t current_input;

  // Opaque to the editor: the active game writes it and reads it back.
  player_profile_t player_profile;
  starting_config_t starting_config;
  char name[MAX_TRACK_NAME];
  int group_index;
  // A linked track can be authored from another track during the same
  // recording pass. Which fields are copied comes from the active schema,
  // rather than from a fixed DDNet button list.
  bool is_linked;
  int linked_source_player;
  uint64_t linked_copy_fields;
  uint32_t linked_transform_flags;

  // Generic exporter selection; every exporter receives the same chosen track
  // set and interprets its own output format inside the game module.
  bool export_enabled;

  // Engine-owned trajectory selection. The game only supplies this player's
  // position through ft_player_view and advances scratch worlds on request.
  bool prediction_enabled;
};

struct dragged_snippet_info_t {
  int snippet_id;
  int track_offset;
  int layer_offset;
};

struct timeline_drag_state_t {
  bool active;
  int source_track_index;
  int dragged_snippet_id;
  int drag_offset_ticks;
  float drag_offset_y;
  ImVec2 initial_mouse_pos;
  dragged_snippet_info_t *drag_infos;
  int drag_info_count;
};

struct timeline_trim_state_t {
  bool active;
  int snippet_id;
  bool left_edge;        // which edge is being dragged
  int grab_offset_ticks; // mouse tick minus edge tick when the drag started
  int preview_tick;      // where the dragged edge currently sits, after snapping
};

struct snippet_id_vector_t {
  int *ids;
  int count;
  int capacity;
};

struct recording_snippet_vector_t {
  input_snippet_t **snippets;
  int count;
  int capacity;
};

// An event the active game reported at a tick. The engine stores and displays
// these; what they mean is the game's business, which is why the payload is a
// category and a line of text rather than the fixed set of DDNet message types
// this used to be.
struct timeline_event_t {
  int tick;
  int group_index;
  int player; // -1 for world-wide events
  char category[32];
  char message[256];
  float color[4];
  uint32_t data_size;
  unsigned char data[FT_TIMELINE_EVENT_DATA_MAX];
};

// A group owns an entirely separate physics history. Tracks remain in one flat array so existing
// timeline editing and plugin APIs can continue to address rows by index, while group_index maps a
// row into the correct independent world.
struct timeline_group_t {
  char name[MAX_TIMELINE_GROUP_NAME];
  float color[4];
  bool visible;
  bool export_enabled;
  bool prediction_enabled;
  int start_offset;

  physics_v_t vec;
  ft_world *initial_world;
  ft_world *previous_world;
  ft_world *prev_world_cached;
  ft_world *world_cached;
  int cached_tick;
  // Physics may be cached by inspectors, plugins or input effects while game
  // presentation is suppressed. Keep its visible high-water mark separate so
  // the render path knows when it must replay particles and other effects.
  int presentation_tick;
  // Reused scratch simulations, one for each prediction variant. These are
  // deliberately world handles rather than game data.
  ft_world *prediction_worlds[MAX_PREDICTION_LINES];
};

struct timeline_state {
  // View State
  float zoom;
  int view_start_tick;
  float track_height;
  // Screen-space Y of the top of the first track row, scrolling included.
  // Refreshed every frame by renderer_draw_tracks_area() so rendering and hit testing share one origin.
  float tracks_origin_y;

  // Playback & Recording State
  int current_tick;
  bool is_playing;
  int gui_playback_speed;
  int playback_speed;
  double last_update_time;
  bool auto_scroll_playhead;
  bool recording;
  bool is_reversing;
  bool linked_copy_input;

  // Data Model
  player_track_t *player_tracks;
  int player_track_count;
  int next_snippet_id;
  timeline_group_t **groups;
  int group_count;
  int active_group_index;
  int simulation_group_index;
  bool input_effects_dirty;
  bool input_effects_rebuilding;
  uint64_t input_effect_context_revision;

  prediction_settings_t prediction;

  // Timeline events reported by the active game.
  timeline_event_t *events;
  int event_count;
  int event_capacity;

  // Interaction State
  snippet_id_vector_t selected_snippets;
  int active_snippet_id; // The primary snippet for editing/context actions
  int selected_player_track_index;
  int context_menu_snippet_id;
  bool selection_box_active;
  ImVec2 selection_box_start;
  ImVec2 selection_box_end;
  timeline_drag_state_t drag_state;
  timeline_trim_state_t trim_state;
  bool is_header_dragging;
  // The playhead selected for a header drag. Overlapping handles use the renderer's back-to-front
  // order so hit testing selects the same handle the user sees in front.
  int header_drag_group_index;
  int header_drag_grab_offset_ticks;
  int header_drag_current_to_playhead_ticks;
  // snippet that was clicked while it was already part of a multi-selection. clicking one of
  // several selected snippets must keep the selection alive until release, so a group drag can
  // still start; if the click ends without a drag, the selection collapses onto this snippet.
  int pending_single_select_id;

  // Recording Targets
  recording_snippet_vector_t recording_snippets;

  // Back-pointer to parent UI handler
  ui_handler_t *ui;
};

#endif // UI_TIMELINE_TYPES_H
