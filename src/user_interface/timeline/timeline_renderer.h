#ifndef UI_TIMELINE_RENDERER_H
#define UI_TIMELINE_RENDERER_H

#include "timeline_types.h"

// Coordinate Conversion
int renderer_screen_x_to_tick(const timeline_state_t *ts, float screen_x, float timeline_start_x);
float renderer_tick_to_screen_x(const timeline_state_t *ts, int tick, float timeline_start_x);
float renderer_get_track_row_height(const timeline_state_t *ts);
float renderer_get_track_screen_y(const timeline_state_t *ts, int track_index);
int renderer_screen_y_to_track_index(const timeline_state_t *ts, float screen_y);
// Returns the frontmost rendered playhead handle at the position, or -1 when no handle was hit.
int renderer_hit_test_playhead_handle(const timeline_state_t *ts, ImRect header_bb, ImVec2 position);
// Returns the nearest rendered playhead to the horizontal position. Equal-distance ties select
// the playhead rendered in front.
int renderer_find_nearest_playhead(const timeline_state_t *ts, ImRect header_bb, float position_x);

// Main Rendering Functions
typedef enum transport_action_t {
  TRANSPORT_NONE,
  TRANSPORT_START,
  TRANSPORT_BACK,
  TRANSPORT_PLAY,
  TRANSPORT_FORWARD,
  TRANSPORT_END,
} transport_action_t;

// The play controls both the Timeline and the Camera tab start with, so the
// two read as one header over different clocks.
transport_action_t renderer_draw_transport(ui_handler_t *ui, bool playing);
void renderer_draw_controls(timeline_state_t *ts);
void renderer_draw_header(timeline_state_t *ts, ImDrawList *draw_list, ImRect header_bb);
void renderer_draw_playhead_line(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect);
void renderer_draw_playhead_handle(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect, ImRect header_bb);
void renderer_draw_tracks_area(timeline_state_t *ts, ImRect timeline_bb);
void renderer_draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb);
void renderer_draw_selection_box(timeline_state_t *ts, ImDrawList *overlay_draw_list);

#endif // UI_TIMELINE_RENDERER_H
