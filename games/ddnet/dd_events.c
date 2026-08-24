#include "dd_internal.h"

#include "dd_imgui.h"
#include <float.h>
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
  ft_player_setup setup = {.struct_size = sizeof(setup)};
  const char *name = "nameless tee";
  if (track >= 0 && game->engine->get_player_setup(track, &setup) && setup.name && setup.name[0]) name = setup.name;

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

static bool edit_payload(dd_event_payload_t *payload) {
  bool changed = false;
  switch ((dd_event_type_t)payload->type) {
  case DD_EVENT_CHAT: {
    changed |= igInputInt("Client ID", &payload->client_id, 1, 1, 0);
    int team_index = payload->team + 2;
    if (igCombo_Str_arr("Team", &team_index, team_names, (int)(sizeof(team_names) / sizeof(team_names[0])), 0)) {
      payload->team = team_index - 2;
      changed = true;
    }
    changed |= igInputText("Message", payload->message, sizeof(payload->message), 0, NULL, NULL);
    break;
  }
  case DD_EVENT_BROADCAST:
    changed |= igInputText("Message", payload->message, sizeof(payload->message), 0, NULL, NULL);
    break;
  case DD_EVENT_KILLMSG:
    changed |= igInputInt("Killer", &payload->killer, 1, 1, 0);
    changed |= igInputInt("Victim", &payload->victim, 1, 1, 0);
    changed |= igCombo_Str_arr("Weapon", &payload->weapon, weapon_names, (int)(sizeof(weapon_names) / sizeof(weapon_names[0])), 0);
    changed |= igInputInt("Mode", &payload->mode_special, 1, 1, 0);
    break;
  case DD_EVENT_SOUND_GLOBAL:
    changed |= igCombo_Str_arr("Sound", &payload->sound_id, sound_names, (int)(sizeof(sound_names) / sizeof(sound_names[0])), 20);
    break;
  case DD_EVENT_EMOTICON:
    changed |= igInputInt("Client ID", &payload->client_id, 1, 1, 0);
    changed |= igCombo_Str_arr("Emoticon", &payload->emoticon, emote_names, (int)(sizeof(emote_names) / sizeof(emote_names[0])), 0);
    break;
  case DD_EVENT_VOTE_SET:
    changed |= igInputInt("Timeout", &payload->vote_timeout, 1, 1, 0);
    changed |= igInputText("Description", payload->message, sizeof(payload->message), 0, NULL, NULL);
    changed |= igInputText("Reason", payload->reason, sizeof(payload->reason), 0, NULL, NULL);
    break;
  case DD_EVENT_VOTE_STATUS:
    changed |= igInputInt("Yes", &payload->vote_yes, 1, 1, 0);
    changed |= igInputInt("No", &payload->vote_no, 1, 1, 0);
    changed |= igInputInt("Pass", &payload->vote_pass, 1, 1, 0);
    changed |= igInputInt("Total", &payload->vote_total, 1, 1, 0);
    break;
  case DD_EVENT_DDRACE_TIME:
    changed |= igInputInt("Time", &payload->time, 1, 1, 0);
    changed |= igInputInt("Check", &payload->check, 1, 1, 0);
    changed |= igInputInt("Finish", &payload->finish, 1, 1, 0);
    break;
  case DD_EVENT_RECORD:
    changed |= igInputInt("Server Best", &payload->server_time_best, 1, 1, 0);
    changed |= igInputInt("Player Best", &payload->player_time_best, 1, 1, 0);
    break;
  default:
    break;
  }
  return changed;
}

void dd_events_render(ft_game *game, const ft_ui_frame *frame) {
  if (!event_api_ready(game) || !frame) return;
  if (game->auto_finish_events && frame->state.recording) scan_finish_tick(game, frame->state.current_tick);
  if (!game->show_events) return;

  igSetNextWindowSize((ImVec2){760.f, 560.f}, ImGuiCond_FirstUseEver);
  if (!igBegin("DDNet Timeline Events", &game->show_events, 0)) {
    igEnd();
    return;
  }

  static int new_type = DD_EVENT_CHAT;
  static int new_tick = 0;
  static int new_world = 0;
  static dd_event_payload_t draft;
  if (draft.magic != DD_EVENT_PAYLOAD_MAGIC)
    draft = (dd_event_payload_t){.magic = DD_EVENT_PAYLOAD_MAGIC, .type = DD_EVENT_CHAT, .team = -2, .vote_timeout = 30};

  const int world_count = game->engine->timeline_world_count ? (int)game->engine->timeline_world_count() : 0;
  if (new_world < 0 || new_world >= world_count)
    new_world = game->engine->timeline_active_world ? game->engine->timeline_active_world() : 0;
  if (new_world < 0 || new_world >= world_count) new_world = 0;

  igCheckbox("Auto-generate finish events while recording", &game->auto_finish_events);
  igSameLine(0, 10.f);
  if (igButton("Generate Finish Events", (ImVec2){0, 0}) && game->engine->timeline_range) {
    int32_t first = 0, last = 0;
    if (game->engine->timeline_range(&first, &last))
      for (int tick = first + 1; tick <= last; ++tick)
        scan_finish_tick(game, tick);
  }
  igSeparator();

  char world_fallback[32];
  const char *selected_world_name = world_name(game, new_world, world_fallback, sizeof(world_fallback));
  if (igBeginCombo("Group", selected_world_name, 0)) {
    for (int world = 0; world < world_count; ++world) {
      char fallback[32];
      const char *name = world_name(game, world, fallback, sizeof(fallback));
      if (igSelectable_Bool(name, world == new_world, 0, (ImVec2){0, 0})) new_world = world;
    }
    igEndCombo();
  }
  if (igButton("Set to Current Tick", (ImVec2){0, 0})) new_tick = current_local_tick(game, frame, new_world);
  igSameLine(0, 6.f);
  igSetNextItemWidth(130.f);
  igDragInt("Tick", &new_tick, 1.f, 0, 0, "%d", 0);
  igSameLine(0, 12.f);
  igSetNextItemWidth(160.f);
  if (igCombo_Str_arr("Type", &new_type, event_names, DD_EVENT_COUNT, 0)) draft.type = new_type;
  else draft.type = new_type;

  igPushID_Str("new_event");
  edit_payload(&draft);
  igPopID();
  if (igButton("Add Event", (ImVec2){110.f, 0.f})) add_event(game, new_world, new_tick, &draft);
  igSeparator();

  if (igBeginTable("DDNetEvents", 5,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                   (ImVec2){0, 0}, 0.f)) {
    igTableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 110.f, 0);
    igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 70.f, 0);
    igTableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 115.f, 0);
    igTableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
    igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 55.f, 0);
    igTableHeadersRow();

    const uint32_t count = game->engine->timeline_event_count();
    for (uint32_t index = 0; index < count; ++index) {
      ft_timeline_event event = {.struct_size = sizeof(event)};
      dd_event_payload_t payload;
      if (!game->engine->timeline_event_get(index, &event) || !dd_event_decode(&event, &payload)) continue;
      igPushID_Int((int)index);
      igTableNextRow(0, 0.f);
      bool changed = false;
      int world = event.world_index;
      int tick = event.tick;

      igTableSetColumnIndex(0);
      char fallback[32];
      const char *name = world_name(game, world, fallback, sizeof(fallback));
      if (igBeginCombo("##world", name, 0)) {
        for (int candidate = 0; candidate < world_count; ++candidate) {
          char candidate_fallback[32];
          const char *candidate_name = world_name(game, candidate, candidate_fallback, sizeof(candidate_fallback));
          if (igSelectable_Bool(candidate_name, candidate == world, 0, (ImVec2){0, 0})) {
            world = candidate;
            changed = true;
          }
        }
        igEndCombo();
      }

      igTableSetColumnIndex(1);
      igSetNextItemWidth(-FLT_MIN);
      changed |= igDragInt("##tick", &tick, 1.f, 0, 0, "%d", 0);

      igTableSetColumnIndex(2);
      int type = payload.type;
      igSetNextItemWidth(-FLT_MIN);
      if (igCombo_Str_arr("##type", &type, event_names, DD_EVENT_COUNT, 0)) {
        payload.type = type;
        changed = true;
      }

      igTableSetColumnIndex(3);
      igPushID_Str("details");
      changed |= edit_payload(&payload);
      igPopID();

      igTableSetColumnIndex(4);
      const bool remove = igSmallButton("Delete");
      if (remove) game->engine->timeline_event_remove(index);
      else if (changed) update_event(game, index, world, tick, &payload);
      igPopID();
      if (remove || changed) break; // mutation may re-sort the engine's array
    }
    igEndTable();
  }
  igEnd();
}
