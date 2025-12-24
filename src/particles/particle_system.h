#ifndef PARTICLE_SYSTEM_H
#define PARTICLE_SYSTEM_H

#include <cglm/cglm.h>
#include <ddnet_map_loader.h>
#include <renderer/renderer.h>
#include <stdbool.h>

#define MAX_PARTICLES (1024 * 8)
#define MAX_FLOW_EVENTS 64

typedef enum { GROUP_PROJECTILE_TRAIL = 0,
               GROUP_TRAIL_EXTRA,
               GROUP_EXPLOSIONS,
               GROUP_EXTRA,
               GROUP_GENERAL,
               NUM_PARTICLE_GROUPS } particle_group_t;

typedef struct {
  double spawn_time;
  vec2 start_pos;
  vec2 start_vel;

  float life_span;
  float start_size;
  float end_size;

  float rot;
  float rot_speed;

  float gravity;
  float friction;
  float flow_affected;

  vec4 color;

  bool use_alpha_fading;
  float start_alpha;
  float end_alpha;

  int sprite_index;
  bool collides;

  int group;
  uint32_t seed;
  int creation_tick;
} particle_t;

typedef struct {
  double time;
  vec2 pos;
  float strength;
  bool active;
  int creation_tick;
} flow_event_t;

typedef struct {
  particle_t particles[MAX_PARTICLES];
  int next_index;

  flow_event_t flow_events[MAX_FLOW_EVENTS];
  int next_flow_index;

  double current_time;
  int last_simulated_tick;
  uint32_t rng_seed;
} particle_system_t;

void particle_system_init(particle_system_t *ps);
void particle_system_update(particle_system_t *ps, float dt, map_data_t *map);
void particle_system_render(particle_system_t *ps, gfx_handler_t *gfx, int layer);

void particle_system_prune_by_time(particle_system_t *ps, double min_time);
void particle_spawn(particle_system_t *ps, int group, particle_t *p_template, float time_passed);

// Effects
void particles_create_explosion(particle_system_t *ps, vec2 pos);
void particles_create_smoke(particle_system_t *ps, vec2 pos, vec2 vel, float alpha, float time_passed);
void particles_create_skid_trail(particle_system_t *ps, vec2 pos, vec2 vel, int direction, float alpha);
void particles_create_bullet_trail(particle_system_t *ps, vec2 pos, float alpha, float time_passed);
void particles_create_player_death(particle_system_t *ps, vec2 pos, vec4 blood_color);
void particles_create_star(particle_system_t *ps, vec2 pos);
void particles_create_hammer_hit(particle_system_t *ps, vec2 pos, float alpha);
void particles_create_air_jump(particle_system_t *ps, vec2 pos, float alpha);
void particles_create_player_spawn(particle_system_t *ps, vec2 pos, float alpha);
void particles_create_confetti(particle_system_t *ps, vec2 pos, float alpha);
void particles_create_damage_ind(particle_system_t *ps, vec2 pos, vec2 dir, float alpha);
void particles_create_powerup_shine(particle_system_t *ps, vec2 pos, vec2 size, float alpha);
void particles_create_freezing_flakes(particle_system_t *ps, vec2 pos, vec2 size, float alpha);
void particles_create_sparkle(particle_system_t *ps, vec2 pos, float alpha);

#endif // PARTICLE_SYSTEM_H
