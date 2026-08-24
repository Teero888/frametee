#ifndef TIMELINE_EVENTS_H
#define TIMELINE_EVENTS_H

#include "user_interface.h"
#include <types.h>

void render_timeline_events_window(ui_handler_t *ui);
void timeline_events_add(timeline_state_t *ts, timeline_event_t event);
void timeline_events_remove(timeline_state_t *ts, int index);
void timeline_events_sort(timeline_state_t *ts);
void timeline_event_tooltip_content(const timeline_event_t *ev);
bool timeline_event_from_abi(timeline_event_t *out, const ft_timeline_event *event, int fallback_group_index);
void timeline_event_to_abi(const timeline_event_t *event, ft_timeline_event *out);

// Re-collects every event the active game reports across the whole timeline.
void timeline_rescan_events(timeline_state_t *ts);

#endif // TIMELINE_EVENTS_H
