#include <engine/int_math.h>
#include "timeline_events.h"
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>
#include "timeline/timeline_model.h"

static int compare_timeline_events(const void *a, const void *b) {
  const timeline_event_t *ev_a = (const timeline_event_t *)a;
  const timeline_event_t *ev_b = (const timeline_event_t *)b;
  return ev_a->tick - ev_b->tick;
}

void timeline_events_sort(timeline_state_t *ts) {
  if (ts->event_count > 1) {
    qsort(ts->events, ts->event_count, sizeof(timeline_event_t), compare_timeline_events);
  }
}

void timeline_events_add(timeline_state_t *ts, timeline_event_t event) {
  if (ts->event_count >= ts->event_capacity) {
    const int new_capacity = ts->event_capacity == 0 ? 8 : ts->event_capacity * 2;
    timeline_event_t *events = realloc(ts->events, sizeof(*events) * (size_t)new_capacity);
    if (!events) return;
    ts->events = events;
    ts->event_capacity = new_capacity;
  }
  ts->events[ts->event_count++] = event;
  timeline_events_sort(ts);
}

void timeline_events_remove(timeline_state_t *ts, int index) {
  if (index < 0 || index >= ts->event_count) return;
  if (index < ts->event_count - 1) {
    memmove(&ts->events[index], &ts->events[index + 1], (ts->event_count - index - 1) * sizeof(timeline_event_t));
  }
  ts->event_count--;
}


// A plain list of whatever the active game reported. Editing the payload of an
// event is a game concern and now lives in the game's own panels; the engine
// only shows what happened and lets the user jump to it or drop it.
void render_timeline_events_window(ui_handler_t *ui) {
  if (!ui->show_timeline_events_window) return;
  timeline_state_t *ts = &ui->timeline;

  igSetNextWindowSize((ImVec2){560, 420}, ImGuiCond_FirstUseEver);
  if (igBegin("Timeline Events", &ui->show_timeline_events_window, 0)) {
    game_host_t *host = &ui->gfx_handler->game_host;
    if (!game_has_cap(host, FT_CAP_TIMELINE_EVENTS)) {
      igTextDisabled("%s does not report timeline events.", ui->plugin_context.active_game_id);
      igEnd();
      return;
    }

    if (igButton("Rescan", (ImVec2){0, 0})) timeline_rescan_events(ts);
    igSameLine(0, 8.f);
    if (igButton("Clear", (ImVec2){0, 0})) {
      ts->event_count = 0;
      ui_mark_unsaved(ui);
    }
    igSeparator();

    if (ts->event_count == 0) {
      igTextDisabled("No events recorded yet.");
      igEnd();
      return;
    }

    if (igBeginTable("EventsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, (ImVec2){0, 0}, 0.f)) {
      igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 70.f, 0);
      igTableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110.f, 0);
      igTableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
      igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.f, 0);
      igTableHeadersRow();

      int pending_remove = -1;
      for (int i = 0; i < ts->event_count; ++i) {
        const timeline_event_t *ev = &ts->events[i];
        igTableNextRow(0, 0.f);
        igPushID_Int(i);

        igTableSetColumnIndex(0);
        char label[32];
        snprintf(label, sizeof(label), "%d", ev->tick);
        if (igSelectable_Bool(label, false, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){0, 0})) {
          ts->current_tick = ev->tick + ts->groups[ev->group_index >= 0 && ev->group_index < ts->group_count ? ev->group_index : 0]->start_offset;
        }

        igTableSetColumnIndex(1);
        igTextColored((ImVec4){ev->color[0], ev->color[1], ev->color[2], ev->color[3] > 0.f ? ev->color[3] : 1.f}, "%s", ev->category);

        igTableSetColumnIndex(2);
        igTextWrapped("%s", ev->message);

        igTableSetColumnIndex(3);
        if (igSmallButton("Remove")) pending_remove = i;
        igPopID();
      }
      igEndTable();

      if (pending_remove >= 0) {
        timeline_events_remove(ts, pending_remove);
        ui_mark_unsaved(ui);
      }
    }
  }
  igEnd();
}

void timeline_event_tooltip_content(const timeline_event_t *ev) {
  igTextColored((ImVec4){ev->color[0], ev->color[1], ev->color[2], ev->color[3] > 0.f ? ev->color[3] : 1.f}, "%s", ev->category);
  if (ev->message[0]) igTextWrapped("%s", ev->message);
  igTextDisabled("tick %d", ev->tick);
}

// Walks the timeline once and records everything the active game reports along
// the way. What counts as an event — a finish, a checkpoint, a death — is the
// game's decision; the engine only stores and shows them.
static void collect_emit(void *user, const ft_timeline_event *event) {
  if (!user || !event || event->struct_size != sizeof(*event)) return;
  timeline_state_t *ts = user;
  timeline_event_t ev = {0};
  ev.tick = event->tick;
  ev.group_index = ts->simulation_group_index;
  snprintf(ev.category, sizeof(ev.category), "%s", event->category ? event->category : "event");
  snprintf(ev.message, sizeof(ev.message), "%s", event->text ? event->text : "");
  ev.player = event->player;
  ev.color[0] = event->color.r;
  ev.color[1] = event->color.g;
  ev.color[2] = event->color.b;
  ev.color[3] = event->color.a;

  // Games may report the same event on every re-simulation, so identical
  // entries are folded rather than piling up.
  for (int i = 0; i < ts->event_count; ++i) {
    const timeline_event_t *existing = &ts->events[i];
    if (existing->tick == ev.tick && existing->group_index == ev.group_index && existing->player == ev.player &&
        strcmp(existing->category, ev.category) == 0)
      return;
  }
  timeline_events_add(ts, ev);
}

void timeline_rescan_events(timeline_state_t *ts) {
  game_host_t *host = ts->ui ? &ts->ui->gfx_handler->game_host : NULL;
  if (!host || !game_has_cap(host, FT_CAP_TIMELINE_EVENTS)) return;

  ts->event_count = 0;
  const int max_ticks = model_get_max_timeline_tick(ts);
  if (max_ticks <= 0) {
    ts->ui->has_unsaved_changes = true;
    return;
  }

  const int previous_simulation_group = ts->simulation_group_index;
  for (int group_index = 0; group_index < ts->group_count; ++group_index) {
    ts->simulation_group_index = group_index;
    const int local_max = imax(0, max_ticks - ts->groups[group_index]->start_offset);

    const ft_world *previous = NULL;
    for (int tick = 1; tick <= local_max; ++tick) {
      const ft_world *world = model_group_world_at_tick(ts, group_index, tick + ts->groups[group_index]->start_offset);
      if (!world) break;
      gh_collect_events(host, previous, world, collect_emit, ts);
      previous = world;
    }
  }
  ts->simulation_group_index = previous_simulation_group;
  ts->ui->has_unsaved_changes = true;
}
