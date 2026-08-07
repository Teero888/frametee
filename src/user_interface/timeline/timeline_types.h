#ifndef UI_TIMELINE_TYPES_H
#define UI_TIMELINE_TYPES_H

#include <physics/physics.h>
#include <stdbool.h>
#include <system/include_cimgui.h>
#include <types.h>
#include <user_interface/player_info.h>

#define MAX_SNIPPETS_PER_PLAYER 64
#define MAX_SNIPPET_LAYERS 8

struct physics_v_t {
  SWorldCore *data;
  uint32_t current_size;
  uint32_t max_size;
};

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
  SPlayerInput *inputs; // source buffer, may extend past the window on either side
  int input_count;      // length of the visible window
  int source_offset;    // index in `inputs` of the tick played at start_tick
  int source_count;     // total ticks held in `inputs`
};

struct starting_config_t {
  vec2 position;
  vec2 velocity;
  int active_weapon;
  bool has_weapons[NUM_WEAPONS];
  bool enabled;
};

typedef enum {
  COPY_DIRECTION = 1 << 0,
  COPY_TARGET = 1 << 1,
  COPY_JUMP = 1 << 2,
  COPY_FIRE = 1 << 3,
  COPY_HOOK = 1 << 4,
  COPY_WEAPON = 1 << 5,
  COPY_MIRROR_X = 1 << 6,
  COPY_MIRROR_Y = 1 << 7,
  COPY_ALL = 0xFFFF & ~COPY_MIRROR_X & ~COPY_MIRROR_Y
} dummy_copy_flags_t;

struct player_track_t {
  input_snippet_t *snippets;
  int snippet_count;
  int snippet_capacity;

  // A temporary buffer for non-destructive recording
  input_snippet_t *recording_snippets;
  int recording_snippet_count;
  int recording_snippet_capacity;

  // The input state for this track for the current frame/tick
  SPlayerInput current_input;

  player_info_t player_info;
  starting_config_t starting_config;
  bool is_dummy;
  int dummy_copy_flags;
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

typedef enum { DUMMY_ACTION_COPY,
               DUMMY_ACTION_INPUTS,
               DUMMY_ACTION_COUNT } dummy_action_type_t;

typedef enum {
  NET_EVENT_CHAT,
  NET_EVENT_BROADCAST,
  NET_EVENT_KILLMSG,
  NET_EVENT_SOUND_GLOBAL,
  NET_EVENT_EMOTICON,
  NET_EVENT_VOTE_SET,
  NET_EVENT_VOTE_STATUS,
  NET_EVENT_DDRACE_TIME,
  NET_EVENT_RECORD,
  NET_EVENT_COUNT
} net_event_type_t;

struct net_event_t {
  int tick;
  net_event_type_t type;
  int team;
  int client_id;
  char message[256];

  // KillMsg
  int killer;
  int victim;
  int weapon;
  int mode_special;

  int sound_id;
  int emoticon;

  // Vote Set
  int vote_timeout;
  char reason[256]; // description is stored in message

  // Vote Status
  int vote_yes;
  int vote_no;
  int vote_pass;
  int vote_total;

  // DDRace Time
  int time;
  int check;
  int finish;

  // Record
  int server_time_best;
  int player_time_best;
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
  bool dummy_copy_input;
  dummy_action_type_t dummy_action_priority[DUMMY_ACTION_COUNT];

  // Data Model
  player_track_t *player_tracks;
  int player_track_count;
  int next_snippet_id;

  // Net Events
  net_event_t *net_events;
  int net_event_count;
  int net_event_capacity;
  int last_event_scan_tick;

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
  // snippet that was clicked while it was already part of a multi-selection. clicking one of
  // several selected snippets must keep the selection alive until release, so a group drag can
  // still start; if the click ends without a drag, the selection collapses onto this snippet.
  int pending_single_select_id;

  // Recording Targets
  recording_snippet_vector_t recording_snippets;

  // Physics Integration
  physics_v_t vec;
  SWorldCore previous_world;
  SWorldCore prev_world_cached;
  SWorldCore world_cached;
  int cached_tick;

  // Back-pointer to parent UI handler
  ui_handler_t *ui;
};

#endif // UI_TIMELINE_TYPES_H
