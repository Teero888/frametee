#ifndef NET_EVENTS_H
#define NET_EVENTS_H

#include "user_interface.h"
#include <types.h>

void render_net_events_window(ui_handler_t *ui);
void net_events_add(timeline_state_t *ts, net_event_t event);
void net_events_remove(timeline_state_t *ts, int index);
void net_events_sort(timeline_state_t *ts);
void net_event_tooltip_draw(const net_event_t *ev);

bool timeline_has_net_event(const timeline_state_t *ts, int tick, net_event_type_t type);
struct CharacterCore;
void timeline_add_finish_events_for_character(timeline_state_t *ts, int tick, const struct CharacterCore *pChar, int track_index);
void timeline_generate_finish_events(timeline_state_t *ts);

#endif // NET_EVENTS_H
