#include "dd_internal.h"
#include "dd_profile.h"

#include "dd_imgui.h"
#include <float.h>
#include <frametee/icons.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(dd_event_payload_t) <= FT_TIMELINE_EVENT_DATA_MAX, "DDNet event payload exceeds the ABI limit");

static const char *const event_names[DD_EVENT_COUNT] = {
    "Chat", "Broadcast", "KillMsg", "SoundGlobal", "Emoticon", "VoteSet", "VoteStatus", "DDRaceTime", "Record"};

static const char *const sound_names[] = {
    "Gun Fire", "Shotgun Fire", "Grenade Fire", "Hammer Fire", "Hammer Hit",
    "Ninja Fire", "Grenade Explode", "Ninja Hit", "Laser Fire", "Laser Bounce",
    "Weapon Switch", "Player Pain Short", "Player Pain Long", "Body Land", "Player Airjump",
    "Player Jump", "Player Die", "Player Spawn", "Player Skid", "Tee Cry",
    "Hook Loop", "Hook Attach Ground", "Hook Attach Player", "Hook NoAttach", "Pickup Health",
    "Pickup Armor", "Pickup Grenade", "Pickup Shotgun", "Pickup Ninja", "Weapon Spawn",
    "Weapon NoAmmo", "Hit", "Chat Server", "Chat Client", "Chat Highlight",
    "CTF Drop", "CTF Return", "CTF Grab PL", "CTF Grab EN", "CTF Capture",
    "Menu"};
static const char *const weapon_names[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
static const char *const emote_names[] = {"oop", "exclamation", "hearts", "drop", "dotdot", "music", "sorry", "ghost",
                                          "sushi", "splattee", "deviltee", "zomg", "zzz", "wtf", "eyes", "question"};
static const char *const team_names[] = {"All", "Spectators", "Red", "Blue", "Whisper Send", "Whisper Receive"};

static bool event_api_ready(const ft_game *game) {
  return game && game->engine->timeline_event_count && game->engine->timeline_event_get && game->engine->timeline_event_add &&
         game->engine->timeline_event_update && game->engine->timeline_event_remove;
}

bool dd_event_decode(const ft_timeline_event *event, dd_event_payload_t *out) {
  if (!event || !out || event->data_size != sizeof(*out) || !event->data) return false;
  memcpy(out, event->data, sizeof(*out));
  out->message[sizeof(out->message) - 1] = '\0';
  out->reason[sizeof(out->reason) - 1] = '\0';
  return out->magic == DD_EVENT_PAYLOAD_MAGIC && out->type >= 0 && out->type < DD_EVENT_COUNT;
}

static void event_summary(const dd_event_payload_t *payload, char *out, size_t out_size) {
  switch ((dd_event_type_t)payload->type) {
  case DD_EVENT_CHAT:
    snprintf(out, out_size, "%s", payload->message);
    break;
  case DD_EVENT_BROADCAST:
    snprintf(out, out_size, "%s", payload->message);
    break;
  case DD_EVENT_KILLMSG:
    snprintf(out, out_size, "killer %d, victim %d", payload->killer, payload->victim);
    break;
  case DD_EVENT_SOUND_GLOBAL:
    snprintf(out, out_size, "sound %d", payload->sound_id);
    break;
  case DD_EVENT_EMOTICON:
    snprintf(out, out_size, "client %d, emote %d", payload->client_id, payload->emoticon);
    break;
  case DD_EVENT_VOTE_SET:
    snprintf(out, out_size, "%s", payload->message);
    break;
  case DD_EVENT_VOTE_STATUS:
    snprintf(out, out_size, "yes %d, no %d, pass %d, total %d", payload->vote_yes, payload->vote_no, payload->vote_pass,
             payload->vote_total);
    break;
  case DD_EVENT_DDRACE_TIME:
    snprintf(out, out_size, "time %.2fs", (double)payload->time / 100.0);
    break;
  case DD_EVENT_RECORD:
    snprintf(out, out_size, "server %d, player %d", payload->server_time_best, payload->player_time_best);
    break;
  default:
    out[0] = '\0';
    break;
  }
}

static ft_color event_color(dd_event_type_t type) {
  switch (type) {
  case DD_EVENT_CHAT:
    return (ft_color){0.45f, 0.8f, 1.f, 1.f};
  case DD_EVENT_DDRACE_TIME:
  case DD_EVENT_RECORD:
    return (ft_color){0.35f, 1.f, 0.5f, 1.f};
  case DD_EVENT_KILLMSG:
    return (ft_color){1.f, 0.4f, 0.35f, 1.f};
  default:
    return (ft_color){0.9f, 0.75f, 0.3f, 1.f};
  }
}

static void make_event(int world_index, int tick, const dd_event_payload_t *payload, ft_timeline_event *out, char *summary,
                       size_t summary_size) {
  event_summary(payload, summary, summary_size);
  int player = -1;
  if (payload->type == DD_EVENT_CHAT || payload->type == DD_EVENT_EMOTICON) player = payload->client_id;
  else if (payload->type == DD_EVENT_KILLMSG) player = payload->victim;
  *out = (ft_timeline_event){.struct_size = sizeof(*out),
                             .world_index = world_index,
                             .tick = tick,
                             .player = player,
                             .category = event_names[payload->type],
                             .text = summary,
                             .color = event_color((dd_event_type_t)payload->type),
                             .data = payload,
                             .data_size = sizeof(*payload)};
}

static bool add_event(ft_game *game, int world_index, int tick, const dd_event_payload_t *payload) {
  char summary[256];
  ft_timeline_event event;
  make_event(world_index, tick, payload, &event, summary, sizeof(summary));
  return game->engine->timeline_event_add(&event);
}

static bool update_event(ft_game *game, uint32_t index, int world_index, int tick, const dd_event_payload_t *payload) {
  char summary[256];
  ft_timeline_event event;
  make_event(world_index, tick, payload, &event, summary, sizeof(summary));
  return game->engine->timeline_event_update(index, &event);
}

static bool has_event(ft_game *game, int world_index, int tick, dd_event_type_t type) {
  const uint32_t count = game->engine->timeline_event_count();
  for (uint32_t i = 0; i < count; ++i) {
    ft_timeline_event event = {.struct_size = sizeof(event)};
    dd_event_payload_t payload;
    if (game->engine->timeline_event_get(i, &event) && event.world_index == world_index && event.tick == tick &&
        dd_event_decode(&event, &payload) && payload.type == (int32_t)type)
      return true;
  }
  return false;
}

static const char *world_name(ft_game *game, int world_index, char *fallback, size_t fallback_size) {
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (game->engine->timeline_world_info && game->engine->timeline_world_info((uint32_t)world_index, &info) && info.name && info.name[0])
    return info.name;
  snprintf(fallback, fallback_size, "Group %d", world_index + 1);
  return fallback;
}

static int current_local_tick(ft_game *game, const ft_ui_frame *frame, int world_index) {
  ft_timeline_world_info info = {.struct_size = sizeof(info)};
  if (game->engine->timeline_world_info && game->engine->timeline_world_info((uint32_t)world_index, &info))
    return frame->state.current_tick - info.start_offset;
  return frame->state.current_tick;
}

static void generate_finish_events(ft_game *game, int world_index, int local_tick, int local_player, const SCharacterCore *character) {
  if (!character || character->m_RaceTime <= 0.f) return;
  const int track = game->engine->timeline_player_track((uint32_t)world_index, (uint32_t)local_player);
  char name[32];
  dd_profile_display_name(game, track, name, sizeof(name));

  if (!has_event(game, world_index, local_tick, DD_EVENT_CHAT)) {
    dd_event_payload_t payload = {.magic = DD_EVENT_PAYLOAD_MAGIC, .type = DD_EVENT_CHAT, .team = 0, .client_id = -1};
    const int minutes = (int)character->m_RaceTime / 60;
    const float seconds = fmodf(character->m_RaceTime, 60.f);
    snprintf(payload.message, sizeof(payload.message), "%s finished in: %d minute(s) %.3f second(s)", name, minutes, seconds);
    add_event(game, world_index, local_tick, &payload);
  }
  if (!has_event(game, world_index, local_tick, DD_EVENT_DDRACE_TIME)) {
    dd_event_payload_t payload = {.magic = DD_EVENT_PAYLOAD_MAGIC,
                                  .type = DD_EVENT_DDRACE_TIME,
                                  .time = (int)lroundf(character->m_RaceTime * 100.f),
                                  .finish = 1};
    add_event(game, world_index, local_tick, &payload);
  }
  if (!has_event(game, world_index, local_tick, DD_EVENT_RECORD)) {
    const int time = (int)lroundf(character->m_RaceTime * 100.f);
    dd_event_payload_t payload = {
        .magic = DD_EVENT_PAYLOAD_MAGIC, .type = DD_EVENT_RECORD, .server_time_best = time, .player_time_best = time};
    add_event(game, world_index, local_tick, &payload);
  }
}

static void scan_finish_tick(ft_game *game, int global_tick) {
  if (global_tick <= 0 || !game->engine->timeline_world_count || !game->engine->timeline_world_pair) return;
  const uint32_t worlds = game->engine->timeline_world_count();
  for (uint32_t world_index = 0; world_index < worlds; ++world_index) {
    ft_timeline_world_info info = {.struct_size = sizeof(info)};
    if (!game->engine->timeline_world_info(world_index, &info) || global_tick < info.start_offset) continue;
    const ft_world *previous = NULL;
    const ft_world *current = NULL;
    if (!game->engine->timeline_world_pair(world_index, global_tick, &previous, &current) || !previous || !current) continue;
    const int players = current->core.m_NumCharacters < previous->core.m_NumCharacters ? current->core.m_NumCharacters
                                                                                       : previous->core.m_NumCharacters;
    for (int player = 0; player < players; ++player) {
      const SCharacterCore *before = &previous->core.m_pCharacters[player];
      const SCharacterCore *after = &current->core.m_pCharacters[player];
      if (before->m_FinishTick < 0 && after->m_FinishTick >= 0)
        generate_finish_events(game, (int)world_index, current->core.m_GameTick, player, after);
    }
  }
}

static bool payload_input_int(const char *label, const char *id, int *value) {
  igTextDisabled("%s", label);
  igSetNextItemWidth(-FLT_MIN);
  return igInputInt(id, value, 1, 1, 0);
}

static bool payload_input_text(const char *label, const char *id, char *value, size_t value_size) {
  igTextDisabled("%s", label);
  igSetNextItemWidth(-FLT_MIN);
  return igInputText(id, value, value_size, 0, NULL, NULL);
}

static bool payload_combo(const char *label, const char *id, int *value, const char *const items[], int item_count,
                          int popup_items) {
  igTextDisabled("%s", label);
  igSetNextItemWidth(-FLT_MIN);
  return igCombo_Str_arr(id, value, items, item_count, popup_items);
}

static bool edit_payload(dd_event_payload_t *payload) {
  bool changed = false;
  switch ((dd_event_type_t)payload->type) {
  case DD_EVENT_CHAT: {
    changed |= payload_input_int("Client ID", "##client_id", &payload->client_id);
    int team_index = payload->team + 2;
    if (payload_combo("Team", "##team", &team_index, team_names, (int)(sizeof(team_names) / sizeof(team_names[0])), 0)) {
      payload->team = team_index - 2;
      changed = true;
    }
    changed |= payload_input_text("Message", "##message", payload->message, sizeof(payload->message));
    break;
  }
  case DD_EVENT_BROADCAST:
    changed |= payload_input_text("Message", "##message", payload->message, sizeof(payload->message));
    break;
  case DD_EVENT_KILLMSG:
    changed |= payload_input_int("Killer", "##killer", &payload->killer);
    changed |= payload_input_int("Victim", "##victim", &payload->victim);
    changed |= payload_combo("Weapon", "##weapon", &payload->weapon, weapon_names,
                             (int)(sizeof(weapon_names) / sizeof(weapon_names[0])), 0);
    changed |= payload_input_int("Mode", "##mode", &payload->mode_special);
    break;
  case DD_EVENT_SOUND_GLOBAL:
    changed |= payload_combo("Sound", "##sound", &payload->sound_id, sound_names,
                             (int)(sizeof(sound_names) / sizeof(sound_names[0])), 20);
    break;
  case DD_EVENT_EMOTICON:
    changed |= payload_input_int("Client ID", "##client_id", &payload->client_id);
    changed |= payload_combo("Emoticon", "##emoticon", &payload->emoticon, emote_names,
                             (int)(sizeof(emote_names) / sizeof(emote_names[0])), 0);
    break;
  case DD_EVENT_VOTE_SET:
    changed |= payload_input_int("Timeout", "##timeout", &payload->vote_timeout);
    changed |= payload_input_text("Description", "##description", payload->message, sizeof(payload->message));
    changed |= payload_input_text("Reason", "##reason", payload->reason, sizeof(payload->reason));
    break;
  case DD_EVENT_VOTE_STATUS:
    changed |= payload_input_int("Yes", "##yes", &payload->vote_yes);
    changed |= payload_input_int("No", "##no", &payload->vote_no);
    changed |= payload_input_int("Pass", "##pass", &payload->vote_pass);
    changed |= payload_input_int("Total", "##total", &payload->vote_total);
    break;
  case DD_EVENT_DDRACE_TIME:
    changed |= payload_input_int("Time", "##time", &payload->time);
    changed |= payload_input_int("Check", "##check", &payload->check);
    changed |= payload_input_int("Finish", "##finish", &payload->finish);
    break;
  case DD_EVENT_RECORD:
    changed |= payload_input_int("Server Best", "##server_best", &payload->server_time_best);
    changed |= payload_input_int("Player Best", "##player_best", &payload->player_time_best);
    break;
  default:
    break;
  }
  return changed;
}

typedef struct dd_event_editor_t {
  bool active;
  int original_world;
  int original_tick;
  dd_event_payload_t original_payload;
  int world;
  int tick;
  dd_event_payload_t payload;
} dd_event_editor_t;

static int find_editor_event(ft_game *game, const dd_event_editor_t *editor) {
  if (!editor->active) return -1;
  const uint32_t count = game->engine->timeline_event_count();
  for (uint32_t index = 0; index < count; ++index) {
    ft_timeline_event event = {.struct_size = sizeof(event)};
    dd_event_payload_t payload;
    if (game->engine->timeline_event_get(index, &event) && event.world_index == editor->original_world &&
        event.tick == editor->original_tick && dd_event_decode(&event, &payload) &&
        memcmp(&payload, &editor->original_payload, sizeof(payload)) == 0)
      return (int)index;
  }
  return -1;
}

static void select_editor_event(dd_event_editor_t *editor, const ft_timeline_event *event,
                                const dd_event_payload_t *payload) {
  editor->active = true;
  editor->original_world = editor->world = event->world_index;
  editor->original_tick = editor->tick = event->tick;
  editor->original_payload = editor->payload = *payload;
}

// ImGui carries the text baseline of a cell's *last* line over to the next cell
// of the same row (see the baseline propagation FIXME in imgui_tables.cpp), so a
// cell that ends on a framed widget pushes the next cell's label down by
// FramePadding.y. These field cells stack a label over a widget, so every one of
// them has to start from a clean baseline for the columns to line up.
static void field_column(void) {
  igTableNextColumn();
  igGetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.f;
}

static bool event_world_combo(ft_game *game, const char *id, int *world, int world_count) {
  char fallback[32];
  const char *selected = world_name(game, *world, fallback, sizeof(fallback));
  bool changed = false;
  if (igBeginCombo(id, selected, 0)) {
    for (int candidate = 0; candidate < world_count; ++candidate) {
      char candidate_fallback[32];
      const char *candidate_name = world_name(game, candidate, candidate_fallback, sizeof(candidate_fallback));
      if (igSelectable_Bool(candidate_name, candidate == *world, 0, (ImVec2){0.f, 0.f})) {
        *world = candidate;
        changed = true;
      }
    }
    igEndCombo();
  }
  return changed;
}

static bool event_row_selectable(const char *label, bool selected) {
  const ImVec4 transparent = {0.f, 0.f, 0.f, 0.f};
  igPushStyleColor_Vec4(ImGuiCol_Header, transparent);
  igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, transparent);
  igPushStyleColor_Vec4(ImGuiCol_HeaderActive, transparent);
  const bool clicked = igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){0.f, 0.f});
  const bool hovered = igIsItemHovered(0);
  const bool active = igIsItemActive();
  igPopStyleColor(3);

  if (active) igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_HeaderActive, 1.f), -1);
  else if (hovered)
    igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_HeaderHovered, 1.f), -1);
  else if (selected)
    igTableSetBgColor(ImGuiTableBgTarget_RowBg0, igGetColorU32_Col(ImGuiCol_Header, 1.f), -1);
  return clicked;
}

// The panel lists this game's events, not every timeline event, so a clipper row
// is not a timeline index. Membership is decided by the payload size alone,
// which costs nothing to test; the full decode is left to the rows on screen.
static bool event_row_fetch(ft_game *game, uint32_t index, ft_timeline_event *event) {
  *event = (ft_timeline_event){.struct_size = sizeof(*event)};
  return game->engine->timeline_event_get(index, event) && event->data_size == sizeof(dd_event_payload_t);
}

static int event_row_count(ft_game *game, uint32_t count) {
  int rows = 0;
  for (uint32_t index = 0; index < count; ++index) {
    ft_timeline_event event;
    if (event_row_fetch(game, index, &event)) ++rows;
  }
  return rows;
}

static void render_event_list(ft_game *game, dd_event_editor_t *editor, float height, float dpi) {
  if (!igBeginChild_Str("DDNetEventList", (ImVec2){0.f, height}, true, ImGuiWindowFlags_None)) {
    igEndChild();
    return;
  }

  const uint32_t count = game->engine->timeline_event_count();
  const int row_count = event_row_count(game, count);
  if (row_count == 0) {
    igSpacing();
    igTextDisabled(ICON_FA_LIST "  No authored DDNet events.");
    igEndChild();
    return;
  }

  const int selected_index = find_editor_event(game, editor);
  const bool compact = igGetContentRegionAvail().x < 430.f * dpi;
  const int columns = compact ? 3 : 4;
  const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
  if (igBeginTable("DDNetEventRows", columns, flags, (ImVec2){0.f, 0.f}, 0.f)) {
    if (!compact) igTableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 100.f * dpi, 0);
    igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 62.f * dpi, 0);
    igTableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 112.f * dpi, 0);
    igTableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
    igTableHeadersRow();

    ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
    ImGuiListClipper_Begin(clipper, row_count, -1.f);
    while (ImGuiListClipper_Step(clipper)) {
      int row = 0;
      for (uint32_t index = 0; index < count; ++index) {
        ft_timeline_event event;
        dd_event_payload_t payload;
        if (!event_row_fetch(game, index, &event)) continue;
        const int current_row = row++;
        if (current_row < clipper->DisplayStart) continue;
        if (current_row >= clipper->DisplayEnd) break;

        igPushID_Int((int)index);
        igTableNextRow(0, 0.f);
        // A payload that passed the size test but not the decode still owns its
        // row: dropping it here would desync every row height below it.
        if (!dd_event_decode(&event, &payload)) {
          igTableSetColumnIndex(0);
          igTextDisabled("--");
          igPopID();
          continue;
        }
        const bool selected = selected_index == (int)index;
        bool clicked;
        int column = 0;
        if (!compact) {
          igTableSetColumnIndex(column++);
          char fallback[32];
          const char *name = world_name(game, event.world_index, fallback, sizeof(fallback));
          clicked = event_row_selectable(name, selected);
        } else {
          igTableSetColumnIndex(column++);
          char tick_label[32];
          snprintf(tick_label, sizeof(tick_label), "%d", event.tick);
          clicked = event_row_selectable(tick_label, selected);
        }

        if (!compact) {
          igTableSetColumnIndex(column++);
          igText("%d", event.tick);
        }
        igTableSetColumnIndex(column++);
        igTextColored((ImVec4){event.color.r, event.color.g, event.color.b,
                               event.color.a > 0.f ? event.color.a : 1.f},
                      "%s", event_names[payload.type]);
        igTableSetColumnIndex(column);
        igTextUnformatted(event.text ? event.text : "", NULL);
        if (clicked) select_editor_event(editor, &event, &payload);
        igPopID();
      }
    }
    ImGuiListClipper_End(clipper);
    ImGuiListClipper_destroy(clipper);
    igEndTable();
  }
  igEndChild();
}

static void render_event_inspector(ft_game *game, dd_event_editor_t *editor, int world_count, float height, float dpi) {
  if (!igBeginChild_Str("DDNetEventInspector", (ImVec2){0.f, height}, true, ImGuiWindowFlags_None)) {
    igEndChild();
    return;
  }

  const int selected_index = find_editor_event(game, editor);
  if (!editor->active || selected_index < 0) {
    editor->active = false;
    igSpacing();
    igTextDisabled(ICON_FA_PEN_TO_SQUARE "  Select an event to inspect or edit it.");
    igEndChild();
    return;
  }

  igSeparatorText("Selected event");
  const int field_columns = igGetContentRegionAvail().x >= 360.f * dpi ? 2 : 1;
  if (igBeginTable("SelectedEventBasics", field_columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0.f, 0.f}, 0.f)) {
    field_column();
    igTextDisabled("Group");
    igSetNextItemWidth(-FLT_MIN);
    event_world_combo(game, "##selected_world", &editor->world, world_count);
    field_column();
    igTextDisabled("Tick");
    igSetNextItemWidth(-FLT_MIN);
    igDragInt("##selected_tick", &editor->tick, 1.f, 0, 0, "%d", 0);
    igEndTable();
  }

  igTextDisabled("Type");
  igSetNextItemWidth(-FLT_MIN);
  int type = editor->payload.type;
  if (igCombo_Str_arr("##selected_type", &type, event_names, DD_EVENT_COUNT, 0)) editor->payload.type = type;

  igSpacing();
  igPushID_Str("selected_payload");
  igPushItemWidth(-FLT_MIN);
  edit_payload(&editor->payload);
  igPopItemWidth();
  igPopID();

  const bool dirty = editor->world != editor->original_world || editor->tick != editor->original_tick ||
                     memcmp(&editor->payload, &editor->original_payload, sizeof(editor->payload)) != 0;
  igSeparator();
  if (!dirty) igBeginDisabled(true);
  if (igButton(ICON_FA_FLOPPY_DISK " Save", (ImVec2){0.f, 0.f})) {
    if (update_event(game, (uint32_t)selected_index, editor->world, editor->tick, &editor->payload)) {
      editor->original_world = editor->world;
      editor->original_tick = editor->tick;
      editor->original_payload = editor->payload;
    }
  }
  igSameLine(0.f, 6.f * dpi);
  if (igButton(ICON_FA_ROTATE_LEFT " Revert", (ImVec2){0.f, 0.f})) {
    editor->world = editor->original_world;
    editor->tick = editor->original_tick;
    editor->payload = editor->original_payload;
  }
  if (!dirty) igEndDisabled();
  igSameLine(0.f, 12.f * dpi);
  if (igButton(ICON_FA_TRASH " Delete", (ImVec2){0.f, 0.f})) {
    if (game->engine->timeline_event_remove((uint32_t)selected_index)) editor->active = false;
  }
  igEndChild();
}

void dd_events_render(ft_game *game, const ft_ui_frame *frame) {
  if (!event_api_ready(game) || !frame) return;
  if (game->auto_finish_events && frame->state.recording) scan_finish_tick(game, frame->state.current_tick);
  if (!game->show_events) return;

  igSetNextWindowSize((ImVec2){820.f, 620.f}, ImGuiCond_FirstUseEver);
  if (!igBegin("DDNet Timeline Events", &game->show_events, 0)) {
    igEnd();
    return;
  }

  static int new_type = DD_EVENT_CHAT;
  static int new_tick = 0;
  static int new_world = 0;
  static dd_event_payload_t draft;
  static dd_event_editor_t editor;
  if (draft.magic != DD_EVENT_PAYLOAD_MAGIC)
    draft = (dd_event_payload_t){.magic = DD_EVENT_PAYLOAD_MAGIC, .type = DD_EVENT_CHAT, .team = -2, .vote_timeout = 30};

  const int world_count = game->engine->timeline_world_count ? (int)game->engine->timeline_world_count() : 0;
  if (new_world < 0 || new_world >= world_count)
    new_world = game->engine->timeline_active_world ? game->engine->timeline_active_world() : 0;
  if (new_world < 0 || new_world >= world_count) new_world = 0;

  const float dpi = igGetFontSize() > 0.f ? igGetFontSize() / 19.f : 1.f;
  const float window_width = igGetContentRegionAvail().x;
  const bool automation_inline = window_width >= 560.f * dpi;
  igCheckbox("Auto-generate finish events while recording", &game->auto_finish_events);
  if (automation_inline) igSameLine(0.f, 10.f * dpi);
  if (igButton(ICON_FA_FLAG_CHECKERED " Generate finish events", (ImVec2){0.f, 0.f}) && game->engine->timeline_range) {
    int32_t first = 0, last = 0;
    if (game->engine->timeline_range(&first, &last))
      for (int tick = first + 1; tick <= last; ++tick)
        scan_finish_tick(game, tick);
  }
  igSeparator();

  if (igCollapsingHeader_TreeNodeFlags("Create event", ImGuiTreeNodeFlags_DefaultOpen)) {
    const int basics_columns = window_width >= 590.f * dpi ? 3 : (window_width >= 360.f * dpi ? 2 : 1);
    if (igBeginTable("NewEventBasics", basics_columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0.f, 0.f}, 0.f)) {
      field_column();
      igTextDisabled("Group");
      igSetNextItemWidth(-FLT_MIN);
      event_world_combo(game, "##new_world", &new_world, world_count);
      field_column();
      igTextDisabled("Tick");
      const char *current_tick_label = ICON_FA_LOCATION_CROSSHAIRS " Current";
      const ImGuiStyle *style = igGetStyle();
      const ImVec2 current_tick_text = igCalcTextSize(current_tick_label, NULL, false, -1.f);
      const float current_tick_width = current_tick_text.x + style->FramePadding.x * 2.f;
      const float tick_input_width =
          fmaxf(70.f * dpi, igGetContentRegionAvail().x - current_tick_width - style->ItemSpacing.x);
      igSetNextItemWidth(tick_input_width);
      igDragInt("##new_tick", &new_tick, 1.f, 0, 0, "%d", 0);
      igSameLine(0.f, style->ItemSpacing.x);
      if (igButton(current_tick_label, (ImVec2){current_tick_width, igGetFrameHeight()}))
        new_tick = current_local_tick(game, frame, new_world);
      field_column();
      igTextDisabled("Type");
      igSetNextItemWidth(-FLT_MIN);
      if (igCombo_Str_arr("##new_type", &new_type, event_names, DD_EVENT_COUNT, 0)) draft.type = new_type;
      else draft.type = new_type;
      igEndTable();
    }

    igPushID_Str("new_event");
    igPushItemWidth(-FLT_MIN);
    edit_payload(&draft);
    igPopItemWidth();
    igPopID();
    if (igButton(ICON_FA_PLUS " Add event", (ImVec2){130.f * dpi, 0.f})) add_event(game, new_world, new_tick, &draft);
  }

  igSeparatorText("Authored events");
  const uint32_t event_count = game->engine->timeline_event_count();
  igTextDisabled("%u event%s", event_count, event_count == 1 ? "" : "s");
  const ImVec2 remaining = igGetContentRegionAvail();
  const bool split = remaining.x >= 700.f * dpi;
  if (split && igBeginTable("DDNetEventWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                            (ImVec2){0.f, 0.f}, 0.f)) {
    igTableSetupColumn("Events", ImGuiTableColumnFlags_WidthStretch, .58f, 0);
    igTableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, .42f, 0);
    igTableNextRow(0, 0.f);
    igTableSetColumnIndex(0);
    render_event_list(game, &editor, 0.f, dpi);
    igTableSetColumnIndex(1);
    render_event_inspector(game, &editor, world_count, 0.f, dpi);
    igEndTable();
  } else if (!split) {
    const float list_height = fmaxf(150.f * dpi, remaining.y * .45f);
    render_event_list(game, &editor, list_height, dpi);
    igSpacing();
    render_event_inspector(game, &editor, world_count, 0.f, dpi);
  }
  igEnd();
}
