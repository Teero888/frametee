#ifndef UI_TIMELINE_H
#define UI_TIMELINE_H

#include "../physics/physics.h"
#include "../renderer/renderer.h"
#include "player_info.h"
#include "undo_redo.h"
#include <cimgui.h>
#include <stdbool.h>

#define MAX_SNIPPETS_PER_PLAYER 64

typedef struct {
  SWorldCore *data;
  uint32_t current_size;
  uint32_t max_size;
} physics_v_t;

typedef struct {
  int id;
  int start_tick;
  int end_tick;

  SPlayerInput *inputs; // Array of player inputs, one per tick of duration
  int input_count;      // Duration in ticks
} input_snippet_t;

typedef struct {
  input_snippet_t *snippets; // Dynamic array of snippets
  int snippet_count;
  player_info_t player_info;
} player_track_t;

// state for managing snippet dragging
typedef struct {
  bool active;
  int source_track_index;
  int source_snippet_index; // Index within the source track's snippet array
  int dragged_snippet_id;
  int drag_offset_ticks; // Offset from snippet start to mouse click point in ticks
  float drag_offset_y;
  ImVec2 initial_mouse_pos; // Mouse position when drag started
} timeline_drag_state_t;

typedef struct {
  int *ids;     // Pointer to the dynamically allocated array of IDs
  int count;    // Number of IDs currently in the array
  int capacity; // Allocated size of the array
} snippet_id_vector_t;

typedef struct {
  input_snippet_t **snippets; // An array of POINTERS to snippets
  int count;
  int capacity;
} recording_snippet_vector_t;

typedef struct ui_handler ui_handler_t;
struct timeline_state {
  int current_tick;
  float zoom;          // Pixels per tick
  int view_start_tick; // The tick at the left edge of the timeline view
  float track_height;
  player_track_t *player_tracks; // Dynamic array of tracks
  int player_track_count;
  int selected_snippet_id;         // ID of the currently selected snippet (-1 if none)
  int selected_player_track_index; // Index of the track containing the selected snippet (-1 if none)
  // Multi-selection support
  snippet_id_vector_t selected_snippets;
  bool selection_box_active;  // Are we currently dragging a selection rectangle?
  ImVec2 selection_box_start; // Mouse start pos for selection box
  ImVec2 selection_box_end;   // Current mouse pos for selection box

  timeline_drag_state_t drag_state;
  int next_snippet_id;
  bool is_header_dragging;
  bool is_playing;
  int gui_playback_speed;
  int playback_speed;
  double last_update_time;
  bool auto_scroll_playhead;
  bool recording;
  recording_snippet_vector_t recording_snippets;
  SPlayerInput recording_input;

  physics_v_t vec;
  SWorldCore previous_world;

  ui_handler_t *ui;
};

undo_command_t *do_add_snippet(ui_handler_t *ui);
undo_command_t *do_split_selected_snippets(ui_handler_t *ui);
undo_command_t *do_delete_selected_snippets(ui_handler_t *ui);
undo_command_t *do_merge_selected_snippets(ui_handler_t *ui);
undo_command_t *do_remove_player_track(ui_handler_t *ui, int index);

input_snippet_t *find_snippet_by_id(player_track_t *track, int snippet_id);
void free_snippet_inputs(input_snippet_t *snippet);
player_track_t *add_new_track(timeline_state_t *ts, ph_t *ph, int num);
SPlayerInput get_input(const timeline_state_t *ts, int track_index, int tick);
input_snippet_t create_empty_snippet(timeline_state_t *ts, int start_tick, int duration);
void timeline_update_inputs(timeline_state_t *ts, gfx_handler_t *gfx);
int get_max_timeline_tick(timeline_state_t *ts);
void recalc_ts(timeline_state_t *ts, int tick);
void process_global_shortcuts(ui_handler_t *ui);
void add_snippet_to_track(player_track_t *track, const input_snippet_t *snippet);

void render_timeline(ui_handler_t *ui);
void timeline_init(timeline_state_t *ts);
void timeline_cleanup(timeline_state_t *ts);

void v_init(physics_v_t *t);
void v_push(physics_v_t *t, SWorldCore *world);
void v_destroy(physics_v_t *t);

undo_command_t *create_edit_inputs_command(input_snippet_t *snippet, const int *indices, int count,
                                           const SPlayerInput *before_states,
                                           const SPlayerInput *after_states);

undo_command_t *timeline_api_create_track(ui_handler_t *ui, const player_info_t *info,
                                          int *out_track_index);
undo_command_t *timeline_api_create_snippet(ui_handler_t *ui, int track_index, int start_tick,
                                            int duration, int *out_snippet_id);
undo_command_t *timeline_api_set_snippet_inputs(ui_handler_t *ui, int snippet_id, int tick_offset,
                                                int count, const SPlayerInput *new_inputs);

#endif
