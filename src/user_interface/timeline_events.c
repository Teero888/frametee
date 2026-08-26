#include "timeline_events.h"
#include "timeline/timeline_model.h"
#include <engine/engine_api.h>
#include <engine/int_math.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

static int compare_timeline_events(const void *a, const void *b) {
  const timeline_event_t *ev_a = (const timeline_event_t *)a;
  const timeline_event_t *ev_b = (const timeline_event_t *)b;
  if (ev_a->tick < ev_b->tick) return -1;
  if (ev_a->tick > ev_b->tick) return 1;
  if (ev_a->group_index < ev_b->group_index) return -1;
  if (ev_a->group_index > ev_b->group_index) return 1;
  return 0;
}

bool timeline_event_from_abi(timeline_event_t *out, const ft_timeline_event *event, int fallback_group_index) {
  if (!out || !event || event->struct_size != sizeof(*event) || event->data_size > FT_TIMELINE_EVENT_DATA_MAX ||
      (event->data_size > 0 && !event->data))
    return false;

  memset(out, 0, sizeof(*out));
  out->tick = event->tick;
  out->group_index = event->world_index >= 0 ? event->world_index : fallback_group_index;
  out->player = event->player;
  snprintf(out->category, sizeof(out->category), "%s", event->category ? event->category : "event");
  snprintf(out->message, sizeof(out->message), "%s", event->text ? event->text : "");
  out->color[0] = event->color.r;
  out->color[1] = event->color.g;
  out->color[2] = event->color.b;
  out->color[3] = event->color.a;
  out->data_size = event->data_size;
  if (event->data_size > 0) memcpy(out->data, event->data, event->data_size);
  return true;
}

void timeline_event_to_abi(const timeline_event_t *event, ft_timeline_event *out) {
  if (!event || !out) return;
  *out = (ft_timeline_event){.struct_size = sizeof(*out),
                             .world_index = event->group_index,
                             .tick = event->tick,
                             .player = event->player,
                             .category = event->category,
                             .text = event->message,
                             .color = {event->color[0], event->color[1], event->color[2], event->color[3]},
                             .data = event->data_size > 0 ? event->data : NULL,
                             .data_size = event->data_size};
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
// the way. What counts as an event (a finish, a checkpoint, a death) is the
// game's decision; the engine only stores and shows them.
static void collect_emit(void *user, const ft_timeline_event *event) {
  if (!user || !event || event->struct_size != sizeof(*event)) return;
  timeline_state_t *ts = user;
  timeline_event_t ev;
  ft_timeline_event reported = *event;
  // collect_events describes the world passed to the callback; a zero-filled
  // event from older source code must not accidentally pin everything to group
  // zero now that authored events can carry an explicit world index.
  reported.world_index = -1;
  if (!timeline_event_from_abi(&ev, &reported, ts->simulation_group_index)) return;

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

  // A rescan replaces observations derived from simulation, not authored
  // game payloads such as DDNet chat and race messages.
  int authored = 0;
  for (int i = 0; i < ts->event_count; ++i) {
    if (ts->events[i].data_size == 0) continue;
    if (authored != i) ts->events[authored] = ts->events[i];
    ++authored;
  }
  ts->event_count = authored;
  const int max_ticks = model_get_max_timeline_tick(ts);
  if (max_ticks <= 0) {
    ts->ui->has_unsaved_changes = true;
    return;
  }

  const int previous_simulation_group = ts->simulation_group_index;
  const bool previous_effects = engine_api_set_presentation_effects(false);
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
  engine_api_set_presentation_effects(previous_effects);
  ts->simulation_group_index = previous_simulation_group;
  ts->ui->has_unsaved_changes = true;
}
