#ifndef NET_EVENTS_H
#define NET_EVENTS_H

#include "user_interface.h"
#include <types.h>

void render_net_events_window(ui_handler_t *ui);
void net_events_add(timeline_state_t *ts, net_event_t event);
void net_events_remove(timeline_state_t *ts, int index);
void net_events_sort(timeline_state_t *ts);

#endif // NET_EVENTS_H
