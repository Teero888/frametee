#include "net_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

static int compare_net_events(const void *a, const void *b) {
  const net_event_t *ev_a = (const net_event_t *)a;
  const net_event_t *ev_b = (const net_event_t *)b;
  return ev_a->tick - ev_b->tick;
}

void net_events_sort(timeline_state_t *ts) {
  if (ts->net_event_count > 1) {
    qsort(ts->net_events, ts->net_event_count, sizeof(net_event_t), compare_net_events);
  }
}

void net_events_add(timeline_state_t *ts, net_event_t event) {
  if (ts->net_event_count >= ts->net_event_capacity) {
    ts->net_event_capacity = ts->net_event_capacity == 0 ? 8 : ts->net_event_capacity * 2;
    ts->net_events = realloc(ts->net_events, sizeof(net_event_t) * ts->net_event_capacity);
  }
  ts->net_events[ts->net_event_count++] = event;
  net_events_sort(ts);
}

void net_events_remove(timeline_state_t *ts, int index) {
  if (index < 0 || index >= ts->net_event_count) return;
  if (index < ts->net_event_count - 1) {
    memmove(&ts->net_events[index], &ts->net_events[index + 1], (ts->net_event_count - index - 1) * sizeof(net_event_t));
  }
  ts->net_event_count--;
}

static const char *sound_names[] = {"Gun Fire",
                                    "Shotgun Fire",
                                    "Grenade Fire",
                                    "Hammer Fire",
                                    "Hammer Hit",
                                    "Ninja Fire",
                                    "Grenade Explode",
                                    "Ninja Hit",
                                    "Laser Fire",
                                    "Laser Bounce",
                                    "Weapon Switch",
                                    "Player Pain Short",
                                    "Player Pain Long",
                                    "Body Land",
                                    "Player Airjump",
                                    "Player Jump",
                                    "Player Die",
                                    "Player Spawn",
                                    "Player Skid",
                                    "Tee Cry",
                                    "Hook Loop",
                                    "Hook Attach Ground",
                                    "Hook Attach Player",
                                    "Hook NoAttach",
                                    "Pickup Health",
                                    "Pickup Armor",
                                    "Pickup Grenade",
                                    "Pickup Shotgun",
                                    "Pickup Ninja",
                                    "Weapon Spawn",
                                    "Weapon NoAmmo",
                                    "Hit",
                                    "Chat Server",
                                    "Chat Client",
                                    "Chat Highlight",
                                    "CTF Drop",
                                    "CTF Return",
                                    "CTF Grab PL",
                                    "CTF Grab EN",
                                    "CTF Capture",
                                    "Menu"};
static const int sound_count = sizeof(sound_names) / sizeof(sound_names[0]);

static const char *weapon_names[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
static const int weapon_count = sizeof(weapon_names) / sizeof(weapon_names[0]);

static const char *emote_names[] = {"Normal", "Pain", "Happy", "Surprise", "Angry", "Blink"};
static const int emote_count = sizeof(emote_names) / sizeof(emote_names[0]);

static const char *team_names[] = {"All", "Spectators", "Red", "Blue", "Whisper Send", "Whisper Receive"};
static const int team_count = sizeof(team_names) / sizeof(team_names[0]);

// Helper to handle team conversion
// UI Index: 0=All(-2), 1=Spec(-1), 2=Red(0), 3=Blue(1)
static int team_idx_to_val(int idx) { return idx - 2; }
static int team_val_to_idx(int val) { return val + 2; }

void render_net_events_window(ui_handler_t *ui) {
  if (!ui->show_net_events_window) return;

  timeline_state_t *ts = &ui->timeline;

  if (igBegin("Net Events", &ui->show_net_events_window, 0)) {
    static int new_type = NET_EVENT_CHAT;
    static int new_tick = 0;
    static char new_message[256] = "";
    static int new_team_idx = 0; // Default to All
    static int new_client_id = 0;

    if (igButton("Set to Current Tick", (ImVec2){0, 0})) {
      new_tick = ts->current_tick;
    }
    igSameLine(0, 5);
    igDragInt("Tick", &new_tick, 1.0f, 0, 0, "%d", 0);

    igCombo_Str("Type", &new_type, "Chat\0Broadcast\0KillMsg\0SoundGlobal\0Emoticon\0VoteSet\0VoteStatusDDRaceTime\0Record\0\0", 0);

    // Common message field for types that use it
    bool uses_message = (new_type == NET_EVENT_CHAT || new_type == NET_EVENT_BROADCAST || new_type == NET_EVENT_VOTE_SET);

    if (uses_message) {
      const char *label = "Message";
      if (new_type == NET_EVENT_VOTE_SET) label = "Vote Desc";
      igInputText(label, new_message, sizeof(new_message), 0, NULL, NULL);
    }

    // Static variables for specific fields
    static int new_killer = 0;
    static int new_victim = 0;
    static int new_weapon = 0;
    static int new_mode_special = 0;
    static int new_sound_id = 0;
    static int new_emoticon = 0;

    // Vote specific
    static int new_vote_timeout = 30;
    static char new_vote_reason[256] = "";
    static int new_vote_yes = 0;
    static int new_vote_no = 0;
    static int new_vote_pass = 0;
    static int new_vote_total = 0;

    // Time/Record specific
    static int new_time = 0;
    static int new_check = 0;
    static int new_finish = 0;
    static int new_server_best = 0;
    static int new_player_best = 0;

    if (new_type == NET_EVENT_CHAT) {
      igInputInt("Client ID", &new_client_id, 1, 1, 0);
      igCombo_Str_arr("Team", &new_team_idx, team_names, team_count, 0);
    } else if (new_type == NET_EVENT_KILLMSG) {
      igInputInt("Killer ID", &new_killer, 1, 1, 0);
      igInputInt("Victim ID", &new_victim, 1, 1, 0);
      igCombo_Str_arr("Weapon", &new_weapon, weapon_names, weapon_count, 0);
      igInputInt("Mode Special", &new_mode_special, 1, 1, 0);
    } else if (new_type == NET_EVENT_SOUND_GLOBAL) {
      igCombo_Str_arr("Sound ID", &new_sound_id, sound_names, sound_count, 20);
    } else if (new_type == NET_EVENT_EMOTICON) {
      igInputInt("Client ID", &new_client_id, 1, 1, 0);
      igCombo_Str_arr("Emoticon ID", &new_emoticon, emote_names, emote_count, 0);
    } else if (new_type == NET_EVENT_VOTE_SET) {
      igInputInt("Timeout", &new_vote_timeout, 1, 1, 0);
      igInputText("Reason", new_vote_reason, sizeof(new_vote_reason), 0, NULL, NULL);
    } else if (new_type == NET_EVENT_VOTE_STATUS) {
      igInputInt("Yes", &new_vote_yes, 1, 1, 0);
      igInputInt("No", &new_vote_no, 1, 1, 0);
      igInputInt("Pass", &new_vote_pass, 1, 1, 0);
      igInputInt("Total", &new_vote_total, 1, 1, 0);
    } else if (new_type == NET_EVENT_DDRACE_TIME) {
      igInputInt("Time", &new_time, 0, 0, 0);
      igInputInt("Check", &new_check, 0, 0, 0);
      igInputInt("Finish", &new_finish, 0, 0, 0);
    } else if (new_type == NET_EVENT_RECORD) {
      igInputInt("Server Best", &new_server_best, 0, 0, 0);
      igInputInt("Player Best", &new_player_best, 0, 0, 0);
    }

    if (igButton("Add Event", (ImVec2){0, 0})) {
      net_event_t ev = {0};
      ev.tick = new_tick;
      ev.type = (net_event_type_t)new_type;

      if (uses_message) {
        strncpy(ev.message, new_message, sizeof(ev.message) - 1);
      }

      if (new_type == NET_EVENT_CHAT) {
        ev.team = team_idx_to_val(new_team_idx);
        ev.client_id = new_client_id;
      } else if (new_type == NET_EVENT_KILLMSG) {
        ev.killer = new_killer;
        ev.victim = new_victim;
        ev.weapon = new_weapon;
        ev.mode_special = new_mode_special;
      } else if (new_type == NET_EVENT_SOUND_GLOBAL) {
        ev.sound_id = new_sound_id;
      } else if (new_type == NET_EVENT_EMOTICON) {
        ev.client_id = new_client_id;
        ev.emoticon = new_emoticon;
      } else if (new_type == NET_EVENT_VOTE_SET) {
        ev.vote_timeout = new_vote_timeout;
        strncpy(ev.reason, new_vote_reason, sizeof(ev.reason) - 1);
      } else if (new_type == NET_EVENT_VOTE_STATUS) {
        ev.vote_yes = new_vote_yes;
        ev.vote_no = new_vote_no;
        ev.vote_pass = new_vote_pass;
        ev.vote_total = new_vote_total;
      } else if (new_type == NET_EVENT_DDRACE_TIME) {
        ev.time = new_time;
        ev.check = new_check;
        ev.finish = new_finish;
      } else if (new_type == NET_EVENT_RECORD) {
        ev.server_time_best = new_server_best;
        ev.player_time_best = new_player_best;
      }

      net_events_add(ts, ev);
    }

    igSeparator();

    if (igBeginTable("EventsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, (ImVec2){0, 0}, 0.0f)) {
      igTableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 60.0f, 0);
      igTableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
      igTableSetupColumn("Message/Info", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
      igTableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 200.0f, 0);
      igTableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 50.0f, 0);
      igTableHeadersRow();

      bool sort_needed = false;

      for (int i = 0; i < ts->net_event_count; ++i) {
        net_event_t *ev = &ts->net_events[i];
        igPushID_Int(i);
        igTableNextRow(0, 0.0f);

        igTableSetColumnIndex(0);
        igPushItemWidth(-FLT_MIN);
        if (igDragInt("##tick", &ev->tick, 1.0f, 0, 0, "%d", 0)) {
          // just updated value
        }
        if (igIsItemDeactivatedAfterEdit()) sort_needed = true;
        igPopItemWidth();

        igTableSetColumnIndex(1);
        igPushItemWidth(-FLT_MIN);
        int type_idx = (int)ev->type;
        if (igCombo_Str("##type", &type_idx, "Chat\0Broadcast\0KillMsg\0SoundGlobal\0Emoticon\0VoteSet\0VoteStatus\0DDRaceTime\0Record\0\0", 0)) {
          ev->type = (net_event_type_t)type_idx;
        }
        igPopItemWidth();

        igTableSetColumnIndex(2);
        if (ev->type == NET_EVENT_CHAT || ev->type == NET_EVENT_BROADCAST || ev->type == NET_EVENT_VOTE_SET) {
          igPushItemWidth(-FLT_MIN);
          igInputText("##msg", ev->message, sizeof(ev->message), 0, NULL, NULL);
          igPopItemWidth();
        } else if (ev->type == NET_EVENT_VOTE_STATUS) {
          igText("Status: Y:%d N:%d P:%d T:%d", ev->vote_yes, ev->vote_no, ev->vote_pass, ev->vote_total);
        } else if (ev->type == NET_EVENT_DDRACE_TIME) {
          igText("Time: %d", ev->time);
        } else if (ev->type == NET_EVENT_RECORD) {
          igText("Rec: S:%d P:%d", ev->server_time_best, ev->player_time_best);
        } else {
          igTextDisabled("-");
        }

        igTableSetColumnIndex(3);
        if (ev->type == NET_EVENT_CHAT) {
          igPushItemWidth(40);
          igInputInt("##cid", &ev->client_id, 0, 0, 0);
          igSameLine(0, 2);
          int team_idx = team_val_to_idx(ev->team);
          igPushItemWidth(80);
          if (igCombo_Str_arr("##team", &team_idx, team_names, team_count, 0)) {
            ev->team = team_idx_to_val(team_idx);
          }
          igPopItemWidth();
          igPopItemWidth();
          if (igIsItemHovered(0)) igSetTooltip("Client ID / Team");
        } else if (ev->type == NET_EVENT_KILLMSG) {
          igPushItemWidth(30);
          igInputInt("##k", &ev->killer, 0, 0, 0);
          igSameLine(0, 2);
          igInputInt("##v", &ev->victim, 0, 0, 0);
          igSameLine(0, 2);
          igPushItemWidth(80);
          igCombo_Str_arr("##w", &ev->weapon, weapon_names, weapon_count, 0);
          igPopItemWidth();
          igSameLine(0, 2);
          igInputInt("##m", &ev->mode_special, 0, 0, 0);
          igPopItemWidth();
        } else if (ev->type == NET_EVENT_SOUND_GLOBAL) {
          igPushItemWidth(150);
          igCombo_Str_arr("##snd", &ev->sound_id, sound_names, sound_count, 20);
          igPopItemWidth();
        } else if (ev->type == NET_EVENT_EMOTICON) {
          igPushItemWidth(40);
          igInputInt("##cid", &ev->client_id, 0, 0, 0);
          igSameLine(0, 2);
          igPushItemWidth(100);
          igCombo_Str_arr("##emo", &ev->emoticon, emote_names, emote_count, 0);
          igPopItemWidth();
          igPopItemWidth();
        } else if (ev->type == NET_EVENT_VOTE_SET) {
          igPushItemWidth(40);
          igInputInt("##tm", &ev->vote_timeout, 0, 0, 0);
          igSameLine(0, 2);
          igPushItemWidth(80);
          igInputText("##rsn", ev->reason, sizeof(ev->reason), 0, NULL, NULL);
          igPopItemWidth();
          igPopItemWidth();
        } else if (ev->type == NET_EVENT_VOTE_STATUS) {
          // Allow editing details
          igPushItemWidth(30);
          igInputInt("##y", &ev->vote_yes, 0, 0, 0);
          igSameLine(0, 2);
          igInputInt("##n", &ev->vote_no, 0, 0, 0);
          igSameLine(0, 2);
          igInputInt("##p", &ev->vote_pass, 0, 0, 0);
          igSameLine(0, 2);
          igInputInt("##t", &ev->vote_total, 0, 0, 0);
          igPopItemWidth();
        } else {
          igTextDisabled("-");
        }

        igTableSetColumnIndex(4);
        if (igButton("Del", (ImVec2){0, 0})) {
          net_events_remove(ts, i);
          i--;
        }
        igPopID();
      }
      igEndTable();

      if (sort_needed) net_events_sort(ts);
    }
  }
  igEnd();
}