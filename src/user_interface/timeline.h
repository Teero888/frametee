#ifndef UI_TIMELINE_H
#define UI_TIMELINE_H

#include <cimgui.h>
#include <stdbool.h>

#define MAX_SNIPPETS_PER_PLAYER 64

typedef struct {
  int id;
  int start_tick;
  int end_tick;
} input_snippet_t;

typedef struct {
  input_snippet_t *snippets; // Dynamic array of snippets
  int snippet_count;
} player_track_t;

// State for managing snippet dragging
typedef struct {
  bool active;
  int source_track_index;
  int source_snippet_index; // Index within the source track's snippet array
  int dragged_snippet_id;
  int drag_offset_ticks;    // Offset from snippet start to mouse click point in ticks
  ImVec2 initial_mouse_pos; // Mouse position when drag started
} timeline_drag_state_t;

typedef struct {
  int current_tick;
  float zoom;          // Pixels per tick
  int view_start_tick; // The tick at the left edge of the timeline view
  float track_height;
  player_track_t *player_tracks; // Dynamic array of tracks
  int player_track_count;
  int selected_snippet_id;         // ID of the currently selected snippet (-1 if none)
  int selected_player_track_index; // Index of the track containing the selected snippet (-1 if none)
  timeline_drag_state_t drag_state;
  int next_snippet_id;
  bool is_header_dragging;
  bool is_playing;
  int playback_speed;
  double last_update_time;
} timeline_state_t;

void render_timeline(timeline_state_t *ts);
void timeline_init(timeline_state_t *ts);

#endif
