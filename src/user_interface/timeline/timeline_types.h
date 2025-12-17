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

struct input_snippet_t {
  int id;
  int start_tick;
  int end_tick;
  bool is_active;
  int layer;
  SPlayerInput *inputs;
  int input_count;
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

typedef enum { DUMMY_ACTION_COPY, DUMMY_ACTION_INPUTS, DUMMY_ACTION_COUNT } dummy_action_type_t;

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

  // Interaction State
  snippet_id_vector_t selected_snippets;
  int active_snippet_id; // The primary snippet for editing/context actions
  int selected_player_track_index;
  int context_menu_snippet_id;
  bool selection_box_active;
  ImVec2 selection_box_start;
  ImVec2 selection_box_end;
  timeline_drag_state_t drag_state;
  bool is_header_dragging;

  // Recording Targets
  recording_snippet_vector_t recording_snippets;

  // Physics Integration
  physics_v_t vec;
  SWorldCore previous_world;

  // Back-pointer to parent UI handler
  ui_handler_t *ui;
};

#endif // UI_TIMELINE_TYPES_H
