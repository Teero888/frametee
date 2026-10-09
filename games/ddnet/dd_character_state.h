#ifndef DD_CHARACTER_STATE_H
#define DD_CHARACTER_STATE_H

#include "include/ddnet/ddnet_game.h"

#include <ddnet_map_loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- what a player is in its team ----------------------------------------------
//
// The setters a starting state is made with, as its properties and as a stored
// world. They describe how a world starts, not what happened before it: putting
// a tee into a team that others are in already keeps the switches the team has,
// whoever was put into it first.

// The switches of a team, one bit each: set where a switch is deactivated.
typedef struct {
  uint8_t bits[32];
} dd_switch_set;

static inline bool dd_switch_set_has(const dd_switch_set *set, int number) {
  return number >= 0 && number < 256 && (set->bits[number >> 3] >> (number & 7)) & 1;
}

static inline void dd_switch_set_add(dd_switch_set *set, int number) {
  if (number >= 0 && number < 256) set->bits[number >> 3] |= (uint8_t)(1u << (number & 7));
}

// The deactivated switches of a team.
static inline dd_switch_set dd_team_switches_off(const ddnet_world_t *world, int team) {
  dd_switch_set set;
  memset(&set, 0, sizeof(set));
  for (int number = 1; number < world->num_switchers && number < 256; ++number)
    if (!ddnet_world_switch(world, number, team).status) dd_switch_set_add(&set, number);
  return set;
}

// Deactivates the switches of the team of a connected client that are in `off` and activates the
// others, without timers.
static inline void dd_team_set_switches_off(ddnet_world_t *world, int client_id, const dd_switch_set *off) {
  if (world->num_switchers <= 1) return;
  const int row = world->players[client_id].team_row;
  if (row < 0 || row >= world->num_team_rows) return;
  ddnet_switch_state_t *states = &world->switch_states[row * world->num_switchers];
  for (int number = 1; number < world->num_switchers && number < 256; ++number) {
    const bool status = !dd_switch_set_has(off, number);
    states[number] = (ddnet_switch_state_t){.status = status, .type = status ? TILE_SWITCHOPEN : TILE_SWITCHCLOSE};
  }
  world->teams[row].switches_touched = true;
  ddnet_world_changed(world);
}

// Puts a connected client into a team.
static inline void dd_player_set_team(ddnet_world_t *world, int client_id, int team) {
  if (team < 0 || team >= DDNET_NUM_TEAMS || world->players[client_id].team == team) return;
  bool joined = false;
  for (int i = 0; i < world->num_clients && !joined; ++i)
    joined = i != client_id && world->players[i].active && world->players[i].team == team;
  const dd_switch_set off = dd_team_switches_off(world, team);
  ddnet_player_set_team(world, client_id, team);
  if (joined) dd_team_set_switches_off(world, client_id, &off);
}

// Whether the race of a tee is running, and the ticks since it started.
static inline bool dd_player_race_started(const ddnet_character_t *chr) { return chr->race_state == DDNET_RACE_STARTED; }
static inline int dd_player_race_ticks(const ddnet_world_t *world, const ddnet_character_t *chr) {
  return dd_player_race_started(chr) ? world->tick - chr->start_time : 0;
}

// Starts the race of a tee, as if it went through the start line, or stops it. A team that is not
// team 0 has started when one of its tees has.
static inline void dd_player_set_race_started(ddnet_world_t *world, int client_id, ddnet_character_t *chr, bool started) {
  ddnet_player_t *player = &world->players[client_id];
  if (started != dd_player_race_started(chr)) {
    chr->race_state = started ? DDNET_RACE_STARTED : DDNET_RACE_NONE;
    chr->start_time = world->tick;
  }
  player->tee_started = started;
  const int row = player->team_row;
  if (player->team == DDNET_TEAM_FLOCK || row < 0 || row >= world->num_team_rows) return;
  ddnet_team_t *team = &world->teams[row];
  if (started) {
    if (team->state < DDNET_TEAMSTATE_STARTED) team->state = DDNET_TEAMSTATE_STARTED;
    team->kill_tick = -1;
    return;
  }
  for (int i = 0; i < world->num_clients; ++i)
    if (world->players[i].active && world->players[i].team == player->team && world->players[i].tee_started) return;
  if (team->state == DDNET_TEAMSTATE_STARTED) team->state = DDNET_TEAMSTATE_OPEN;
}

// The switches of a set as text, for a property: numbers and ranges ("2 5-7"). False when it does
// not fit.
static inline bool dd_switch_set_format(const dd_switch_set *set, char *out, size_t out_size) {
  size_t used = 0;
  out[0] = '\0';
  for (int number = 0; number < 256; ++number) {
    if (!dd_switch_set_has(set, number)) continue;
    int last = number;
    while (last + 1 < 256 && dd_switch_set_has(set, last + 1)) ++last;
    const int written = last > number ? snprintf(out + used, out_size - used, used ? " %d-%d" : "%d-%d", number, last)
                                      : snprintf(out + used, out_size - used, used ? " %d" : "%d", number);
    if (written < 0 || (size_t)written >= out_size - used) return false;
    used += (size_t)written;
    number = last;
  }
  return true;
}

// Reads what dd_switch_set_format writes (spaces or commas between the parts). False for anything else.
static inline bool dd_switch_set_parse(const char *text, dd_switch_set *out) {
  memset(out, 0, sizeof(*out));
  const char *p = text ? text : "";
  while (*p) {
    if (*p == ' ' || *p == ',') {
      ++p;
      continue;
    }
    char *end;
    const long first = strtol(p, &end, 10);
    if (end == p || first < 1 || first > 255) return false;
    long last = first;
    p = end;
    if (*p == '-') {
      last = strtol(p + 1, &end, 10);
      if (end == p + 1 || last < first || last > 255) return false;
      p = end;
    }
    for (long number = first; number <= last; ++number)
      dd_switch_set_add(out, (int)number);
  }
  return true;
}

// --- the stored tee --------------------------------------------------------------

// A tee as a project stores it for a starting state: what the editor can set a
// tee up with (the properties it offers as starting overrides), at the spawn of
// a new world. Fixed sizes and no padding, read and written with memcpy.
typedef struct {
  float pos_x, pos_y;
  float vel_x, vel_y;
  int32_t active_weapon;
  int32_t freeze_time;
  int32_t jumps;
  int32_t jumped_total;
  int32_t health;
  int32_t armor;
  uint8_t weapons_got[DDNET_NUM_WEAPONS];
  uint8_t deep_frozen, live_frozen;
  uint8_t endless_jump, endless_hook, jetpack, solo;
  uint8_t has_telegun_gun, has_telegun_grenade, has_telegun_laser;
  uint8_t collision_disabled, hook_hit_disabled;
  uint8_t hammer_hit_disabled, shotgun_hit_disabled, grenade_hit_disabled, laser_hit_disabled;
  uint8_t padding[3];
} dd_character_state_v2;

// Version 3: the tee, whether the player is in the game and what it is in its team.
typedef struct {
  dd_character_state_v2 tee;
  uint8_t team;
  uint8_t team_locked;
  uint8_t race_started;
  uint8_t connected;
  int32_t race_ticks; // since the race started
  dd_switch_set switches_off;
} dd_character_state_v3;

// The tee of the world's player `player` into `out` (zeros for a player without one).
static inline void dd_character_state_write(void *out, const ft_world *world, int player) {
  dd_character_state_v3 s;
  memset(&s, 0, sizeof(s));
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  s.connected = world->core.players[ddnet_player_client(world, player)].active;
  if (chr) {
    const ddnet_character_core_t *c = &chr->core;
    s.tee.pos_x = chr->pos.x;
    s.tee.pos_y = chr->pos.y;
    s.tee.vel_x = c->vel.x;
    s.tee.vel_y = c->vel.y;
    s.tee.active_weapon = c->active_weapon;
    s.tee.freeze_time = chr->freeze_time;
    s.tee.jumps = c->jumps;
    s.tee.jumped_total = c->jumped_total;
    s.tee.health = chr->health;
    s.tee.armor = chr->armor;
    for (int w = 0; w < DDNET_NUM_WEAPONS; ++w)
      s.tee.weapons_got[w] = c->weapons[w].got;
    s.tee.deep_frozen = c->deep_frozen;
    s.tee.live_frozen = c->live_frozen;
    s.tee.endless_jump = c->endless_jump;
    s.tee.endless_hook = c->endless_hook;
    s.tee.jetpack = c->jetpack;
    s.tee.solo = c->solo;
    s.tee.has_telegun_gun = c->has_telegun_gun;
    s.tee.has_telegun_grenade = c->has_telegun_grenade;
    s.tee.has_telegun_laser = c->has_telegun_laser;
    s.tee.collision_disabled = c->collision_disabled;
    s.tee.hook_hit_disabled = c->hook_hit_disabled;
    s.tee.hammer_hit_disabled = c->hammer_hit_disabled;
    s.tee.shotgun_hit_disabled = c->shotgun_hit_disabled;
    s.tee.grenade_hit_disabled = c->grenade_hit_disabled;
    s.tee.laser_hit_disabled = c->laser_hit_disabled;
    const int team = world->core.players[ddnet_player_client(world, player)].team;
    s.team = (uint8_t)team;
    s.team_locked = ddnet_world_team(&world->core, team).locked;
    s.race_started = dd_player_race_started(chr);
    s.race_ticks = dd_player_race_ticks(&world->core, chr);
    s.switches_off = dd_team_switches_off(&world->core, team);
  }
  memcpy(out, &s, sizeof(s));
}

// A stored tee over the tee of the world's player `player`. `version` 2 has no team.
static inline void dd_character_state_read(ft_world *world, int player, const void *in, uint32_t version) {
  dd_character_state_v3 s;
  memset(&s, 0, sizeof(s));
  s.connected = 1;
  memcpy(&s, in, version >= 3 ? sizeof(s) : sizeof(s.tee));
  const int client_id = ddnet_player_client(world, player);
  if (!s.connected) {
    ddnet_player_leave(&world->core, client_id);
    return;
  }
  ddnet_character_t *chr = ddnet_player_character_mut(world, player);
  if (!chr) return;
  ddnet_character_core_t *c = &chr->core;
  chr->pos = (ddnet_vec2_t){s.tee.pos_x, s.tee.pos_y};
  chr->prev_pos = chr->pos;
  c->pos = chr->pos;
  c->vel = (ddnet_vec2_t){s.tee.vel_x, s.tee.vel_y};
  c->active_weapon =
      s.tee.active_weapon >= 0 && s.tee.active_weapon < DDNET_NUM_WEAPONS ? s.tee.active_weapon : DDNET_WEAPON_GUN;
  chr->freeze_time = s.tee.freeze_time;
  c->jumps = s.tee.jumps;
  c->jumped_total = s.tee.jumped_total;
  chr->health = s.tee.health;
  chr->armor = s.tee.armor;
  for (int w = 0; w < DDNET_NUM_WEAPONS; ++w)
    c->weapons[w].got = s.tee.weapons_got[w] != 0;
  c->deep_frozen = s.tee.deep_frozen;
  c->live_frozen = s.tee.live_frozen;
  c->endless_jump = s.tee.endless_jump;
  c->endless_hook = s.tee.endless_hook;
  c->jetpack = s.tee.jetpack;
  c->solo = s.tee.solo;
  c->has_telegun_gun = s.tee.has_telegun_gun;
  c->has_telegun_grenade = s.tee.has_telegun_grenade;
  c->has_telegun_laser = s.tee.has_telegun_laser;
  c->collision_disabled = s.tee.collision_disabled;
  c->hook_hit_disabled = s.tee.hook_hit_disabled;
  c->hammer_hit_disabled = s.tee.hammer_hit_disabled;
  c->shotgun_hit_disabled = s.tee.shotgun_hit_disabled;
  c->grenade_hit_disabled = s.tee.grenade_hit_disabled;
  c->laser_hit_disabled = s.tee.laser_hit_disabled;
  world->core.players[client_id].is_solo = c->solo;
  if (version >= 3) {
    dd_player_set_team(&world->core, client_id, s.team);
    const int team = world->core.players[client_id].team;
    if (team != DDNET_TEAM_FLOCK && ddnet_world_team(&world->core, team).locked != (s.team_locked != 0))
      ddnet_world_lock_team(&world->core, team, s.team_locked != 0);
    dd_player_set_race_started(&world->core, client_id, chr, s.race_started != 0);
    if (s.race_started) chr->start_time = world->core.tick - s.race_ticks;
    dd_team_set_switches_off(&world->core, client_id, &s.switches_off);
  }
  ddnet_character_changed(&world->core, client_id);
}

#endif // DD_CHARACTER_STATE_H
