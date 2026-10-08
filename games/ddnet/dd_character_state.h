#ifndef DD_CHARACTER_STATE_H
#define DD_CHARACTER_STATE_H

#include "include/ddnet/ddnet_game.h"

#include <string.h>

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

// The tee of the world's player `player` into `out` (zeros for a player without one).
static inline void dd_character_state_write(void *out, const ft_world *world, int player) {
  dd_character_state_v2 s;
  memset(&s, 0, sizeof(s));
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  if (chr) {
    const ddnet_character_core_t *c = &chr->core;
    s.pos_x = chr->pos.x;
    s.pos_y = chr->pos.y;
    s.vel_x = c->vel.x;
    s.vel_y = c->vel.y;
    s.active_weapon = c->active_weapon;
    s.freeze_time = chr->freeze_time;
    s.jumps = c->jumps;
    s.jumped_total = c->jumped_total;
    s.health = chr->health;
    s.armor = chr->armor;
    for (int w = 0; w < DDNET_NUM_WEAPONS; ++w)
      s.weapons_got[w] = c->weapons[w].got;
    s.deep_frozen = c->deep_frozen;
    s.live_frozen = c->live_frozen;
    s.endless_jump = c->endless_jump;
    s.endless_hook = c->endless_hook;
    s.jetpack = c->jetpack;
    s.solo = c->solo;
    s.has_telegun_gun = c->has_telegun_gun;
    s.has_telegun_grenade = c->has_telegun_grenade;
    s.has_telegun_laser = c->has_telegun_laser;
    s.collision_disabled = c->collision_disabled;
    s.hook_hit_disabled = c->hook_hit_disabled;
    s.hammer_hit_disabled = c->hammer_hit_disabled;
    s.shotgun_hit_disabled = c->shotgun_hit_disabled;
    s.grenade_hit_disabled = c->grenade_hit_disabled;
    s.laser_hit_disabled = c->laser_hit_disabled;
  }
  memcpy(out, &s, sizeof(s));
}

// A stored tee over the tee of the world's player `player`.
static inline void dd_character_state_read(ft_world *world, int player, const void *in) {
  dd_character_state_v2 s;
  memcpy(&s, in, sizeof(s));
  ddnet_character_t *chr = ddnet_player_character_mut(world, player);
  if (!chr) return;
  ddnet_character_core_t *c = &chr->core;
  chr->pos = (ddnet_vec2_t){s.pos_x, s.pos_y};
  chr->prev_pos = chr->pos;
  c->pos = chr->pos;
  c->vel = (ddnet_vec2_t){s.vel_x, s.vel_y};
  c->active_weapon = s.active_weapon >= 0 && s.active_weapon < DDNET_NUM_WEAPONS ? s.active_weapon : DDNET_WEAPON_GUN;
  chr->freeze_time = s.freeze_time;
  c->jumps = s.jumps;
  c->jumped_total = s.jumped_total;
  chr->health = s.health;
  chr->armor = s.armor;
  for (int w = 0; w < DDNET_NUM_WEAPONS; ++w)
    c->weapons[w].got = s.weapons_got[w] != 0;
  c->deep_frozen = s.deep_frozen;
  c->live_frozen = s.live_frozen;
  c->endless_jump = s.endless_jump;
  c->endless_hook = s.endless_hook;
  c->jetpack = s.jetpack;
  c->solo = s.solo;
  c->has_telegun_gun = s.has_telegun_gun;
  c->has_telegun_grenade = s.has_telegun_grenade;
  c->has_telegun_laser = s.has_telegun_laser;
  c->collision_disabled = s.collision_disabled;
  c->hook_hit_disabled = s.hook_hit_disabled;
  c->hammer_hit_disabled = s.hammer_hit_disabled;
  c->shotgun_hit_disabled = s.shotgun_hit_disabled;
  c->grenade_hit_disabled = s.grenade_hit_disabled;
  c->laser_hit_disabled = s.laser_hit_disabled;
  const int client_id = ddnet_player_client(world, player);
  world->core.players[client_id].is_solo = c->solo;
  ddnet_character_changed(&world->core, client_id);
}

#endif // DD_CHARACTER_STATE_H
