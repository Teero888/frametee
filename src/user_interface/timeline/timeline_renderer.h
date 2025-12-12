#ifndef UI_TIMELINE_RENDERER_H
#define UI_TIMELINE_RENDERER_H

#include "timeline_types.h"

// Coordinate Conversion
int renderer_screen_x_to_tick(const timeline_state_t *ts, float screen_x, float timeline_start_x);
float renderer_tick_to_screen_x(const timeline_state_t *ts, int tick, float timeline_start_x);
float renderer_get_track_screen_y(const timeline_state_t *ts, ImRect timeline_bb, int track_index, float scroll_y);
int renderer_screen_y_to_track_index(const timeline_state_t *ts, ImRect timeline_bb, float screen_y, float scroll_y);

// Main Rendering Functions
void renderer_draw_controls(timeline_state_t *ts);
void renderer_draw_header(timeline_state_t *ts, ImDrawList *draw_list, ImRect header_bb);
void renderer_draw_playhead_line(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect);
void renderer_draw_playhead_handle(timeline_state_t *ts, ImDrawList *draw_list, ImRect timeline_rect, ImRect header_bb);
void renderer_draw_tracks_area(timeline_state_t *ts, ImRect timeline_bb);
void renderer_draw_drag_preview(timeline_state_t *ts, ImDrawList *overlay_draw_list, ImRect timeline_bb, float tracks_area_scroll_y);
void renderer_draw_selection_box(timeline_state_t *ts, ImDrawList *overlay_draw_list);

#endif // UI_TIMELINE_RENDERER_H
