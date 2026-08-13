#include "dd_internal.h"

#include <ddnet_physics/collision.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Deterministic PRNG for re-simulation loop
static float deterministic_frand(uint32_t *seed) {
  *seed = (*seed ^ 61) ^ (*seed >> 16);
  *seed *= 9;
  *seed = *seed ^ (*seed >> 4);
  *seed *= 0x27d4eb2d;
  *seed = *seed ^ (*seed >> 15);
  return (float)(*seed & 0xFFFFFF) / 16777216.0f;
}

static float ps_frand01(dd_particle_system_t *ps) { return deterministic_frand(&ps->rng_seed); }
static float ps_frand_range(dd_particle_system_t *ps, float min, float max) { return min + ps_frand01(ps) * (max - min); }

static void random_direction(dd_particle_system_t *ps, vec2 out) {
  float angle = ps_frand01(ps) * 2.0f * M_PI;
  out[0] = cosf(angle);
  out[1] = sinf(angle);
}

static void mix_colors(vec4 c1, vec4 c2, float t, vec4 out) { glm_vec4_lerp(c1, c2, t, out); }

void dd_particles_init(dd_particle_system_t *ps) {
  memset(ps, 0, sizeof(dd_particle_system_t));
  ps->particles = calloc(DD_MAX_PARTICLES, sizeof(dd_particle_t));
  ps->active_count = 0;
  ps->next_flow_index = 0;
  ps->last_simulated_tick = -1;
}

void dd_particles_reset(dd_particle_system_t *ps) {
  ps->active_count = 0;
  memset(ps->flow_events, 0, sizeof(ps->flow_events));
  ps->next_flow_index = 0;
  ps->current_time = 0.0;
  ps->last_simulated_tick = -1;
  ps->rng_seed = 0;
}

void dd_particles_cleanup(dd_particle_system_t *ps) {
  if (ps->particles) {
    free(ps->particles);
    ps->particles = NULL;
  }
}

void dd_particles_prune_by_time(dd_particle_system_t *ps, double min_time) {
  int target_tick = (int)(min_time * 50.0 + 0.1);

  // Compact particles
  int valid_count = 0;
  for (int i = 0; i < ps->active_count; ++i) {
    if (ps->particles[i].life_span > 0.0001f && ps->particles[i].creation_tick <= target_tick) {
      if (i != valid_count) {
        ps->particles[valid_count] = ps->particles[i];
      }
      valid_count++;
    }
  }
  ps->active_count = valid_count;

  // Compact flow events
  int valid_flow = 0;
  for (int i = 0; i < DD_MAX_FLOW_EVENTS; ++i) {
    if (ps->flow_events[i].active && ps->flow_events[i].creation_tick <= target_tick) {
      if (i != valid_flow) {
        ps->flow_events[valid_flow] = ps->flow_events[i];
      }
      valid_flow++;
    }
  }
  if (valid_flow < DD_MAX_FLOW_EVENTS) {
    memset(&ps->flow_events[valid_flow], 0, (DD_MAX_FLOW_EVENTS - valid_flow) * sizeof(dd_flow_event_t));
  }
  ps->next_flow_index = valid_flow % DD_MAX_FLOW_EVENTS;

  if (target_tick < ps->last_simulated_tick) {
    ps->last_simulated_tick = target_tick;
  }
}

void dd_particle_spawn(dd_particle_system_t *ps, int group, dd_particle_t *p_template, float time_passed) {
  int current_tick = (int)(ps->current_time * 50.0 + 0.1);
  if (current_tick <= ps->last_simulated_tick) return;

  if (ps->active_count >= DD_MAX_PARTICLES) return;

  int id = ps->active_count++;
  dd_particle_t *p = &ps->particles[id];
  *p = *p_template;

  p->spawn_time = ps->current_time - (double)time_passed;
  p->group = group;
  // Initialize deterministic seed for this particle
  p->seed = ps->rng_seed;
  p->current_seed = p->seed;
  p->creation_tick = current_tick;
  ps_frand01(ps); // Advance the generator

  glm_vec2_copy(p_template->start_pos, p->start_pos);
  glm_vec2_copy(p_template->start_vel, p->start_vel);

  glm_vec2_copy(p->start_pos, p->current_pos);
  glm_vec2_copy(p->start_vel, p->current_vel);
  glm_vec2_copy(p->start_pos, p->prev_pos);
  p->last_sim_time = p->spawn_time;
  p->prev_sim_time = p->spawn_time;
}

static void flow_add(dd_particle_system_t *ps, vec2 pos, float strength) {
  int id = ps->next_flow_index;
  ps->next_flow_index = (ps->next_flow_index + 1) % DD_MAX_FLOW_EVENTS;
  ps->flow_events[id].active = true;
  ps->flow_events[id].time = ps->current_time;
  ps->flow_events[id].strength = strength;
  ps->flow_events[id].creation_tick = (int)(ps->current_time * 50.0 + 0.1);
  glm_vec2_copy(pos, ps->flow_events[id].pos);
}

static float g_flow_decay_table[76];
static bool g_flow_decay_inited = false;

static void init_flow_decay_table(void) {
  if (g_flow_decay_inited) return;
  for (int i = 0; i <= 75; i++) {
    g_flow_decay_table[i] = powf(0.85f, (float)i);
  }
  g_flow_decay_inited = true;
}

static void flow_get(dd_particle_system_t *ps, double sim_time, vec2 pos, vec2 out_vel) {
  out_vel[0] = 0;
  out_vel[1] = 0;
  if (!g_flow_decay_inited) init_flow_decay_table();

  for (int i = 0; i < DD_MAX_FLOW_EVENTS; ++i) {
    if (!ps->flow_events[i].active) continue;
    double age = sim_time - ps->flow_events[i].time;
    if (age < 0 || age > 1.5) continue; // Decays fully after 1.5s

    int tick_age = (int)(age * 50.0);
    if (tick_age < 0 || tick_age > 75) continue;

    float decay = g_flow_decay_table[tick_age];
    if (decay < 0.01f) continue;

    float dist = glm_vec2_distance(pos, ps->flow_events[i].pos);
    if (dist > 128.0f || dist < 0.1f) continue;

    float dist_factor = 1.0f - (dist / 128.0f);
    vec2 dir;
    glm_vec2_sub(pos, ps->flow_events[i].pos, dir);
    glm_vec2_normalize(dir);

    float force = ps->flow_events[i].strength * decay * dist_factor;
    out_vel[0] += dir[0] * force;
    out_vel[1] += dir[1] * force;
  }
}

static bool point_is_solid(const map_data_t *map, float x, float y) {
  const unsigned char *tiles = map->game_layer.data;
  if (!tiles) return false;

  int nx = (int)roundf(x) / 32;
  int ny = (int)roundf(y) / 32;
  nx = nx < 0 ? 0 : (nx > map->width - 1 ? map->width - 1 : nx);
  ny = ny < 0 ? 0 : (ny > map->height - 1 ? map->height - 1 : ny);

  unsigned char tile = tiles[ny * map->width + nx];
  return tile == TILE_SOLID || tile == TILE_NOHOOK;
}

static void move_point(const map_data_t *map, vec2 *inout_pos, vec2 *inout_vel, float elasticity) {
  if (!map || !map->game_layer.data) {
    glm_vec2_add(*inout_pos, *inout_vel, *inout_pos);
    return;
  }

  vec2 pos = {(*inout_pos)[0], (*inout_pos)[1]};
  vec2 vel = {(*inout_vel)[0], (*inout_vel)[1]};

  if (!point_is_solid(map, pos[0] + vel[0], pos[1] + vel[1])) {
    glm_vec2_add(pos, vel, *inout_pos);
    return;
  }

  // Each axis is tested on its own, so a step into a corner reflects both instead of guessing one.
  int affected = 0;
  if (point_is_solid(map, pos[0] + vel[0], pos[1])) {
    (*inout_vel)[0] *= -elasticity;
    affected++;
  }
  if (point_is_solid(map, pos[0], pos[1] + vel[1])) {
    (*inout_vel)[1] *= -elasticity;
    affected++;
  }
  if (affected == 0) {
    (*inout_vel)[0] *= -elasticity;
    (*inout_vel)[1] *= -elasticity;
  }
}

static void particle_simulate_step(dd_particle_system_t *ps, dd_particle_t *p, vec2 pos, vec2 vel, uint32_t *seed, double sim_time, float dt, const map_data_t *map) {
  vel[1] += p->gravity * dt;

  if (p->flow_affected > 0.0f) {
    vec2 flow_vel;
    flow_get(ps, sim_time, pos, flow_vel);
    vel[0] += flow_vel[0] * p->flow_affected * dt;
    vel[1] += flow_vel[1] * p->flow_affected * dt;
  }

  if (p->friction > 0.0f) {
    glm_vec2_scale(vel, powf(p->friction, dt / 0.05f), vel);
  }

  vec2 move;
  glm_vec2_scale(vel, dt, move);
  if (p->collides && map) {
    float elasticity = 0.1f + 0.9f * deterministic_frand(seed);
    move_point(map, (vec2 *)pos, &move, elasticity);
    if (dt > 0.0001f) {
      glm_vec2_scale(move, 1.0f / dt, vel);
    }
  } else {
    glm_vec2_add(pos, move, pos);
  }
}

void dd_particles_update_sim(dd_particle_system_t *ps, const map_data_t *map) {
  const double step = 0.02;
  double sim_target = ps->current_time;

  // Survivors are compacted forwards, keeping their relative order. Swapping the last particle into
  // a dead one's slot instead would reshuffle the array every time a particle expired, and since
  // particles are drawn in array order that made a surviving particle jump in front of or behind
  // its neighbours on the tick any other particle died.
  int write = 0;

  for (int i = 0; i < ps->active_count; ++i) {
    dd_particle_t *p = &ps->particles[i];

    // Check life
    double age = sim_target - p->spawn_time;
    if (age > p->life_span || age < -0.001) continue;

    // Incremental Simulation / Rewind Handling. The retained pair straddles the render time, so a
    // rewind is when even the older of the two sits ahead of it.
    if (p->prev_sim_time > sim_target + 0.001) {
      glm_vec2_copy(p->start_pos, p->current_pos);
      glm_vec2_copy(p->start_vel, p->current_vel);
      glm_vec2_copy(p->start_pos, p->prev_pos);
      p->last_sim_time = p->spawn_time;
      p->prev_sim_time = p->spawn_time;
      p->current_seed = p->seed;
    }

    // Step until the simulation has passed the render time, keeping the state before it. Stopping
    // short and extrapolating instead made every particle jump at each step boundary: a step also
    // changes the velocity that would have been extrapolated with, by gravity, friction and bounces.
    while (p->last_sim_time < sim_target) {
      glm_vec2_copy(p->current_pos, p->prev_pos);
      p->prev_sim_time = p->last_sim_time;

      particle_simulate_step(ps, p, p->current_pos, p->current_vel, &p->current_seed, p->last_sim_time, (float)step, map);
      p->last_sim_time += step;
    }

    if (write != i) ps->particles[write] = *p;
    write++;
  }

  ps->active_count = write;
}
void dd_particles_update(dd_particle_system_t *ps, float dt, const map_data_t *map) {
  (void)ps;
  (void)dt;
  (void)map;
}

static const int g_group_layer[NUM_PARTICLE_GROUPS] = {
    0, // GROUP_PROJECTILE_TRAIL
    0, // GROUP_TRAIL_EXTRA
    1, // GROUP_EXPLOSIONS
    1, // GROUP_EXTRA
    1  // GROUP_GENERAL
};

void dd_particles_render(dd_particle_system_t *ps, ft_game *game, int layer) {
  if (ps->active_count == 0 || !game->gfx.ready) return;
  const ft_engine_api *engine = game->engine;

  // Frustum culling bounds in pixel coordinates. The engine converts screen to
  // world for us; the rest of this file thinks in DDNet pixels.
  ft_camera camera;
  engine->camera_get(&camera);

  const float margin = 200.0f; // pixel margin for large particles
  const float cam_min_x = camera.visible.x * PX_PER_TILE - margin;
  const float cam_max_x = (camera.visible.x + camera.visible.w) * PX_PER_TILE + margin;
  const float cam_min_y = camera.visible.y * PX_PER_TILE - margin;
  const float cam_max_y = (camera.visible.y + camera.visible.h) * PX_PER_TILE + margin;

  float z_layer = layer ? DD_Z_PARTICLES_FRONT : DD_Z_PARTICLES_BACK;

  for (int i = 0; i < ps->active_count; ++i) {
    dd_particle_t *p = &ps->particles[i];

    if (g_group_layer[p->group] != layer) continue;

    double age = ps->current_time - p->spawn_time;
    if (age < 0 || age > p->life_span) continue;

    vec2 pos;
    float rot;
    float size;
    vec4 col;
    if (p->sprite_index == GAMESKIN_STAR_0) {
      const float remaining_life = p->life_span - (float)age;
      const float move_mix = fminf(fmaxf((remaining_life - 0.60f) / 0.15f, 0.0f), 1.0f);
      pos[0] = p->start_pos[0] + p->start_vel[0] * 75.0f * (1.0f - move_mix);
      pos[1] = p->start_pos[1] + p->start_vel[1] * 75.0f * (1.0f - move_mix);
      rot = p->rot + remaining_life * 2.0f;
      size = p->start_size;
      glm_vec4_copy(p->color, col);
      col[3] *= fminf(fmaxf(remaining_life / 0.1f, 0.0f), 1.0f);
    } else {
      // Interpolate between the two simulated states straddling the render time. Never extrapolate
      // past the newer one: that is what pushed particles into walls a step before the collision
      // response ran, and what made them lurch whenever a step was taken.
      double span = p->last_sim_time - p->prev_sim_time;
      float alpha = span > 1e-9 ? (float)((ps->current_time - p->prev_sim_time) / span) : 1.0f;
      alpha = fminf(fmaxf(alpha, 0.0f), 1.0f);
      pos[0] = p->prev_pos[0] + (p->current_pos[0] - p->prev_pos[0]) * alpha;
      pos[1] = p->prev_pos[1] + (p->current_pos[1] - p->prev_pos[1]) * alpha;

      rot = p->rot + p->rot_speed * (float)age;
      float life_frac = (float)age / p->life_span;
      size = p->start_size * (1.0f - life_frac) + p->end_size * life_frac;
      glm_vec4_copy(p->color, col);
      if (p->use_alpha_fading) col[3] = p->start_alpha * (1.0f - life_frac) + p->end_alpha * life_frac;
    }

    // Camera frustum culling
    if (pos[0] < cam_min_x || pos[0] > cam_max_x || pos[1] < cam_min_y || pos[1] > cam_max_y) {
      continue;
    }

    // The sprite index carries which sheet it came from as an offset.
    int atlas_type = (p->sprite_index < PARTICLE_SPRITE_OFFSET) ? 1 : (p->sprite_index < EXTRA_SPRITE_OFFSET ? 2 : 3);
    ft_atlas *target = (atlas_type == 1) ? game->gfx.gameskin : (atlas_type == 2 ? game->gfx.particles : game->gfx.extras);
    int render_sprite_idx = p->sprite_index - (atlas_type == 1 ? 0 : (atlas_type == 2 ? PARTICLE_SPRITE_OFFSET : EXTRA_SPRITE_OFFSET));

    dd_draw_sprite(game, target, z_layer, (vec2){pos[0] / 32.f, pos[1] / 32.f}, (vec2){size / 32.f, size / 32.f}, rot,
                   (uint32_t)render_sprite_idx, col);
  }
}


void dd_particles_create_explosion(dd_particle_system_t *ps, vec2 pos) {
  flow_add(ps, pos, 5000.0f);
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = 0.4f;
  p.start_size = 150.0f;
  p.end_size = 0.0f;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.sprite_index = PARTICLE_EXPL01 + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, 1}, p.color);
  dd_particle_spawn(ps, GROUP_EXPLOSIONS, &p, 0);

  for (int i = 0; i < 24; ++i) {
    memset(&p, 0, sizeof(p));
    glm_vec2_copy(pos, p.start_pos);
    vec2 dir;
    random_direction(ps, dir);
    float speed = ps_frand_range(ps, 1.0f, 1.2f) * 1000.0f;
    p.start_vel[0] = dir[0] * speed;
    p.start_vel[1] = dir[1] * speed;
    p.life_span = ps_frand_range(ps, 0.5f, 0.9f);
    p.start_size = ps_frand_range(ps, 32.0f, 40.0f);
    p.end_size = 0.0f;
    p.gravity = ps_frand_range(ps, -800.0f, 0.0f);
    p.friction = 0.4f;
    p.sprite_index = PARTICLE_SMOKE + PARTICLE_SPRITE_OFFSET;
    p.collides = true;
    p.flow_affected = 1.0f;
    mix_colors((vec4){0.75, 0.75, 0.75, 1}, (vec4){0.5, 0.5, 0.5, 1}, ps_frand01(ps), p.color);
    dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void dd_particles_create_smoke(dd_particle_system_t *ps, vec2 pos, vec2 vel, float alpha, float time_passed) {
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  vec2 dir;
  random_direction(ps, dir);
  p.start_vel[0] = vel[0] + dir[0] * 50.0f;
  p.start_vel[1] = vel[1] + dir[1] * 50.0f;
  p.life_span = ps_frand_range(ps, 0.5f, 1.0f);
  p.start_size = ps_frand_range(ps, 12, 20);
  p.end_size = 0;
  p.friction = 0.7f;
  p.gravity = ps_frand_range(ps, -500, 0);
  p.flow_affected = 0.0f;
  p.sprite_index = PARTICLE_SMOKE + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_PROJECTILE_TRAIL, &p, time_passed);
}

void dd_particles_create_skid_trail(dd_particle_system_t *ps, vec2 pos, vec2 vel, int direction, float alpha) {
  dd_particle_t p = {0};
  p.sprite_index = PARTICLE_SMOKE + PARTICLE_SPRITE_OFFSET;
  p.start_pos[0] = pos[0] + (-direction * 6);
  p.start_pos[1] = pos[1] + 12;
  vec2 rdir;
  random_direction(ps, rdir);
  float v_len = glm_vec2_norm(vel);
  p.start_vel[0] = (-direction * 100 * v_len) + rdir[0] * 50;
  p.start_vel[1] = -50 + rdir[1] * 50;
  p.life_span = ps_frand_range(ps, 0.5, 1);
  p.start_size = ps_frand_range(ps, 24, 36);
  p.end_size = 0;
  p.friction = 0.7f;
  p.gravity = ps_frand_range(ps, -500, 0);
  glm_vec4_copy((vec4){0.75, 0.75, 0.75, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
}

void dd_particles_create_bullet_trail(dd_particle_system_t *ps, vec2 pos, float alpha, float time_passed) {
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = ps_frand_range(ps, 0.25, 0.5);
  p.start_size = 8;
  p.end_size = 0;
  p.friction = 0.7;
  p.sprite_index = PARTICLE_BALL + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_PROJECTILE_TRAIL, &p, time_passed);
}

void dd_particles_create_player_death(dd_particle_system_t *ps, vec2 pos, vec4 blood_color) {
  for (int i = 0; i < 64; ++i) {
    dd_particle_t p = {0};
    glm_vec2_copy(pos, p.start_pos);
    vec2 dir;
    random_direction(ps, dir);
    float speed = ps_frand_range(ps, 0.1, 1.1) * 900;
    p.start_vel[0] = dir[0] * speed;
    p.start_vel[1] = dir[1] * speed;
    p.life_span = ps_frand_range(ps, 0.3, 0.6);
    p.start_size = ps_frand_range(ps, 24, 40);
    p.end_size = 0;
    p.gravity = 800;
    p.friction = 0.8;
    p.rot = ps_frand01(ps) * 2 * M_PI;
    p.rot_speed = ps_frand_range(ps, -0.5, 0.5) * M_PI;
    p.sprite_index = (PARTICLE_SPLAT01 + (int)(ps_frand01(ps) * 3)) + PARTICLE_SPRITE_OFFSET;
    p.collides = true;
    float t = ps_frand_range(ps, 0.75, 1);
    p.color[0] = blood_color[0] * t;
    p.color[1] = blood_color[1] * t;
    p.color[2] = blood_color[2] * t;
    p.color[3] = 0.75f * blood_color[3];
    dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void dd_particles_create_confetti(dd_particle_system_t *ps, vec2 pos, float alpha) {
  vec4 cols[] = {{1, 0.4, 0.4, 1}, {0.4, 1, 0.4, 1}, {0.4, 0.4, 1, 1}, {1, 1, 0.4, 1}, {0.4, 1, 1, 1}, {1, 0.4, 1, 1}};
  for (int i = 0; i < 64; ++i) {
    dd_particle_t p = {0};
    glm_vec2_copy(pos, p.start_pos);
    p.sprite_index = (PARTICLE_SPLAT01 + (int)(ps_frand01(ps) * 3)) + PARTICLE_SPRITE_OFFSET;
    float a = -0.5 * M_PI + ps_frand_range(ps, -0.8, 0.8);
    vec2 d = {cosf(a), sinf(a)};
    p.start_vel[0] = d[0] * ps_frand_range(ps, 500, 2000);
    p.start_vel[1] = d[1] * ps_frand_range(ps, 500, 2000);
    p.life_span = ps_frand_range(ps, 0.8, 1.2);
    p.start_size = ps_frand_range(ps, 12, 24);
    p.end_size = 0;
    p.rot = ps_frand01(ps) * 2 * M_PI;
    p.rot_speed = ps_frand_range(ps, -0.5, 0.5) * M_PI;
    p.gravity = -700;
    p.friction = 0.6;
    glm_vec4_copy(cols[(int)(ps_frand01(ps) * 6)], p.color);
    p.color[3] = 0.75f * alpha;
    dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void dd_particles_create_star(dd_particle_system_t *ps, vec2 pos) {
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.start_vel[1] = -200;
  p.life_span = 1;
  p.start_size = 32;
  p.end_size = 32;
  p.sprite_index = GAMESKIN_STAR_1;
  glm_vec4_copy((vec4){1, 1, 1, 1}, p.color);
  dd_particle_spawn(ps, GROUP_EXTRA, &p, 0);
}

void dd_particles_create_hammer_hit(dd_particle_system_t *ps, vec2 pos, float alpha) {
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = 0.3f;
  p.start_size = 120;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.sprite_index = PARTICLE_HIT01 + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_EXPLOSIONS, &p, 0);
}

void dd_particles_create_air_jump(dd_particle_system_t *ps, vec2 pos, float alpha) {
  vec2 off = {-6, 16};
  for (int i = 0; i < 2; ++i) {
    dd_particle_t p = {0};
    p.start_pos[0] = pos[0] + off[0];
    p.start_pos[1] = pos[1] + off[1];
    p.start_vel[1] = -200;
    p.life_span = 0.5;
    p.start_size = 48;
    p.end_size = 0;
    p.gravity = 500;
    p.friction = 0.7;
    p.rot = ps_frand01(ps) * 2 * M_PI;
    p.rot_speed = 2 * M_PI;
    p.sprite_index = PARTICLE_AIRJUMP + PARTICLE_SPRITE_OFFSET;
    glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
    dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
    off[0] = 6.0f;
  }
}

void dd_particles_create_player_spawn(dd_particle_system_t *ps, vec2 pos, float alpha) {
  for (int i = 0; i < 32; ++i) {
    dd_particle_t p = {0};
    glm_vec2_copy(pos, p.start_pos);
    vec2 d;
    random_direction(ps, d);
    float s = powf(ps_frand01(ps), 3) * 600;
    p.start_vel[0] = d[0] * s;
    p.start_vel[1] = d[1] * s;
    p.life_span = ps_frand_range(ps, 0.3, 0.6);
    p.start_size = ps_frand_range(ps, 64, 96);
    p.end_size = 0;
    p.gravity = ps_frand_range(ps, -400, 0);
    p.friction = 0.7;
    p.rot = ps_frand01(ps) * 2 * M_PI;
    p.sprite_index = PARTICLE_SHELL + PARTICLE_SPRITE_OFFSET;
    glm_vec4_copy((vec4){181.f / 255.f, 80.f / 255.f, 203.f / 255.f, alpha}, p.color);
    dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void dd_particles_create_powerup_shine(dd_particle_system_t *ps, vec2 pos, vec2 size, float alpha) {
  dd_particle_t p = {0};
  p.sprite_index = PARTICLE_SLICE + PARTICLE_SPRITE_OFFSET;
  p.start_pos[0] = pos[0] + ps_frand_range(ps, -0.5, 0.5) * size[0];
  p.start_pos[1] = pos[1] + ps_frand_range(ps, -0.5, 0.5) * size[1];
  p.life_span = 0.5;
  p.start_size = 16;
  p.end_size = 0;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.rot_speed = 2 * M_PI;
  p.gravity = 500;
  p.friction = 0.9;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
}

void dd_particles_create_freezing_flakes(dd_particle_system_t *ps, vec2 pos, vec2 size, float alpha) {
  dd_particle_t p = {0};
  p.sprite_index = EXTRA_SNOWFLAKE + EXTRA_SPRITE_OFFSET;
  p.start_pos[0] = pos[0] + ps_frand_range(ps, -0.5, 0.5) * size[0];
  p.start_pos[1] = pos[1] + ps_frand_range(ps, -0.5, 0.5) * size[1];
  p.life_span = 1.5;
  p.start_size = ps_frand_range(ps, 8, 24);
  p.end_size = p.start_size * 0.5f;
  p.use_alpha_fading = true;
  p.start_alpha = alpha;
  p.end_alpha = 0.0;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.rot_speed = M_PI;
  p.gravity = ps_frand_range(ps, 0, 250);
  p.friction = 0.9;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_EXTRA, &p, 0);
}

void dd_particles_create_sparkle(dd_particle_system_t *ps, vec2 pos, float alpha) {
  dd_particle_t p = {0};
  p.sprite_index = EXTRA_SPARKLE + EXTRA_SPRITE_OFFSET;
  vec2 d;
  random_direction(ps, d);
  float dist = ps_frand01(ps) * 40;
  p.start_pos[0] = pos[0] + d[0] * dist;
  p.start_pos[1] = pos[1] + d[1] * dist;
  p.life_span = 0.5;
  p.start_size = 0;
  p.end_size = ps_frand_range(ps, 20, 30);
  p.use_alpha_fading = true;
  p.start_alpha = alpha;
  p.end_alpha = fminf(0.2f, alpha);
  glm_vec4_copy((vec4){1, 1, 1, 1}, p.color);
  dd_particle_spawn(ps, GROUP_TRAIL_EXTRA, &p, 0);
}

void dd_particles_create_damage_ind(dd_particle_system_t *ps, vec2 pos, vec2 dir, float alpha) {
  dd_particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  glm_vec2_negate_to(dir, p.start_vel);
  p.life_span = 0.75f;
  p.start_size = 48.0f / sqrtf(2.0f);
  p.end_size = p.start_size;
  p.rot = -ps_frand_range(ps, 0.0f, 2.0f * M_PI);
  p.sprite_index = GAMESKIN_STAR_0;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  dd_particle_spawn(ps, GROUP_GENERAL, &p, 0);
}

// --- engine glue -------------------------------------------------------------

dd_particle_system_t *dd_particles_for(ft_game *game, int world_index) {
  if (world_index < 0) return NULL;
  if (world_index >= game->particle_count) {
    dd_particle_system_t *grown = realloc(game->particles, (size_t)(world_index + 1) * sizeof(*grown));
    if (!grown) return NULL;
    game->particles = grown;
    for (int i = game->particle_count; i <= world_index; ++i) dd_particles_init(&game->particles[i]);
    game->particle_count = world_index + 1;
  }
  return &game->particles[world_index];
}

// The physics raises effects through these while a tick runs. Routing them by
// world index is what keeps two timeline groups from sharing one set of sparks.
typedef struct {
  ft_game *game;
  int world_index;
} dd_effect_sink_t;

static dd_effect_sink_t g_effect_sinks[64];

static void on_particle(mvec2 pos, int type, int cid, void *user_data) {
  const dd_effect_sink_t *sink = user_data;
  if (!sink) return;
  dd_particle_system_t *ps = dd_particles_for(sink->game, sink->world_index);
  if (!ps) return;
  (void)cid;

  vec2 p = {vgetx(pos), vgety(pos)};
  const vec2 zero_vel = {0, -1};

  switch (type) {
  case PARTICLE_TYPE_SMOKE: dd_particles_create_smoke(ps, p, (float *)zero_vel, 1.0f, 0.0f); break;
  case PARTICLE_TYPE_PLAYER_SPAWN: dd_particles_create_player_spawn(ps, p, 1.0f); break;
  case PARTICLE_TYPE_PLAYER_DEATH: dd_particles_create_player_death(ps, p, (vec4){1, 1, 1, 1}); break;
  case PARTICLE_TYPE_AIR_JUMP: dd_particles_create_air_jump(ps, p, 1.0f); break;
  case PARTICLE_TYPE_BULLET_TRAIL: dd_particles_create_bullet_trail(ps, p, 1.0f, 0.0f); break;
  case PARTICLE_TYPE_BULLET_STARS: dd_particles_create_star(ps, p); break;
  case PARTICLE_TYPE_EXPLOSION: dd_particles_create_explosion(ps, p); break;
  case PARTICLE_TYPE_HAMMER_HIT: dd_particles_create_hammer_hit(ps, p, 1.0f); break;
  case PARTICLE_TYPE_CONFETTI: dd_particles_create_confetti(ps, p, 1.0f); break;
  default: break;
  }
}

static void on_damage_indicator(mvec2 pos, float angle, int amount, int cid, void *user_data) {
  const dd_effect_sink_t *sink = user_data;
  (void)cid;
  if (!sink) return;
  dd_particle_system_t *ps = dd_particles_for(sink->game, sink->world_index);
  if (!ps) return;

  vec2 p = {vgetx(pos), vgety(pos)};
  const float pi = 3.14159265358979323846f;
  const float center = 3.0f * pi / 2.0f + angle;
  const float start = center - pi / 3.0f;
  const float end = center + pi / 3.0f;
  for (int i = 0; i < amount; ++i) {
    const float indicator_angle = start + (end - start) * (float)(i + 1) / (float)(amount + 1);
    vec2 dir = {cosf(indicator_angle), sinf(indicator_angle)};
    dd_particles_create_damage_ind(ps, p, dir, 1.0f);
  }
}

bool dd_particles_bind(ft_game *game, ft_world *world) {
  if (!world) return false;
  const int index = world->index;
  if (index < 0 || index >= (int)(sizeof(g_effect_sinks) / sizeof(g_effect_sinks[0]))) {
    world->core.particle = NULL;
    world->core.damage_indicator = NULL;
    return false;
  }

  dd_particle_system_t *ps = dd_particles_for(game, index);
  if (!ps || game->headless || !game->settings.render_particles) {
    world->core.particle = NULL;
    world->core.damage_indicator = NULL;
    return false;
  }

  // Re-simulating a tick that already ran means the editor rewound or replayed.
  // Drop everything newer than this moment and resume from here: without this
  // the high-water mark stays ahead forever and nothing ever spawns again.
  if (world->core.m_GameTick <= ps->last_simulated_tick) {
    const double time = (double)world->core.m_GameTick / (double)GAME_TICK_SPEED;
    dd_particles_prune_by_time(ps, time);
    ps->current_time = time;
    ps->last_simulated_tick = world->core.m_GameTick - 1;
  }

  g_effect_sinks[index].game = game;
  g_effect_sinks[index].world_index = index;
  ps->rng_seed = (uint32_t)world->core.m_GameTick;
  ps->current_time = (double)world->core.m_GameTick / (double)GAME_TICK_SPEED;

  world->core.user_data = &g_effect_sinks[index];
  world->core.particle = on_particle;
  world->core.damage_indicator = on_damage_indicator;
  return true;
}

// Closes out a tick's effects. The high-water mark only moves once the tick has
// actually run: spawning refuses anything at or below it, so raising it first
// silently rejected every particle the tick produced.
void dd_particles_finish(ft_game *game, ft_world *world, int tick_before, bool bound) {
  world->core.particle = NULL;
  world->core.damage_indicator = NULL;
  if (!bound) return;

  dd_particle_system_t *ps = dd_particles_for(game, world->index);
  if (ps) ps->last_simulated_tick = tick_before;
}

void dd_particles_advance(ft_game *game, int world_index, const ft_level *level, int tick, float alpha) {
  dd_particle_system_t *ps = dd_particles_for(game, world_index);
  if (!ps || !level) return;

  // The simulation runs one step ahead of the rendered time; rendering
  // interpolates between the two, so this only has to set where "now" is.
  const double particle_time = ((double)tick - 1.0 + (double)alpha) / (double)GAME_TICK_SPEED;
  ps->current_time = particle_time < 0.0 ? 0.0 : particle_time;
  dd_particles_update_sim(ps, &level->collision.m_MapData);
}
