#include "particle_system.h"
#include "renderer/renderer.h"
#include <ddnet_physics/collision.h>
#include <logger/logger.h>
#include <math.h>
#include <renderer/graphics_backend.h>
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

static float ps_frand01(particle_system_t *ps) { return deterministic_frand(&ps->rng_seed); }
static float ps_frand_range(particle_system_t *ps, float min, float max) { return min + ps_frand01(ps) * (max - min); }

static void random_direction(particle_system_t *ps, vec2 out) {
  float angle = ps_frand01(ps) * 2.0f * M_PI;
  out[0] = cosf(angle);
  out[1] = sinf(angle);
}

static void mix_colors(vec4 c1, vec4 c2, float t, vec4 out) { glm_vec4_lerp(c1, c2, t, out); }

void particle_system_init(particle_system_t *ps) {
  memset(ps, 0, sizeof(particle_system_t));
  ps->next_index = 0;
  ps->next_flow_index = 0;
  ps->last_simulated_tick = -1;
}

void particle_system_prune_by_time(particle_system_t *ps, double min_time) {
  int target_tick = (int)(min_time * 50.0 + 0.1);

  // Compact particles
  int valid_count = 0;
  for (int i = 0; i < MAX_PARTICLES; ++i) {
    if (ps->particles[i].life_span > 0.0001f && ps->particles[i].creation_tick <= target_tick) {
      if (i != valid_count) {
        ps->particles[valid_count] = ps->particles[i];
      }
      valid_count++;
    }
  }
  // Clear the rest
  if (valid_count < MAX_PARTICLES) {
    memset(&ps->particles[valid_count], 0, (MAX_PARTICLES - valid_count) * sizeof(particle_t));
  }
  ps->next_index = valid_count % MAX_PARTICLES;

  // Compact flow events
  int valid_flow = 0;
  for (int i = 0; i < MAX_FLOW_EVENTS; ++i) {
    if (ps->flow_events[i].active && ps->flow_events[i].creation_tick <= target_tick) {
      if (i != valid_flow) {
        ps->flow_events[valid_flow] = ps->flow_events[i];
      }
      valid_flow++;
    }
  }
  if (valid_flow < MAX_FLOW_EVENTS) {
    memset(&ps->flow_events[valid_flow], 0, (MAX_FLOW_EVENTS - valid_flow) * sizeof(flow_event_t));
  }
  ps->next_flow_index = valid_flow % MAX_FLOW_EVENTS;

  if (target_tick < ps->last_simulated_tick) {
    ps->last_simulated_tick = target_tick;
  }
}

void particle_spawn(particle_system_t *ps, int group, particle_t *p_template, float time_passed) {
  int current_tick = (int)(ps->current_time * 50.0 + 0.1);
  if (current_tick <= ps->last_simulated_tick) return;

  int id = ps->next_index;
  ps->next_index = (ps->next_index + 1) % MAX_PARTICLES;

  particle_t *p = &ps->particles[id];
  *p = *p_template;

  p->spawn_time = ps->current_time - (double)time_passed;
  p->group = group;
  // Initialize deterministic seed for this particle
  p->seed = ps->rng_seed;
  p->creation_tick = current_tick;
  ps_frand01(ps); // Advance the generator

  glm_vec2_copy(p_template->start_pos, p->start_pos);
  glm_vec2_copy(p_template->start_vel, p->start_vel);
}

static void flow_add(particle_system_t *ps, vec2 pos, float strength) {
  int id = ps->next_flow_index;
  ps->next_flow_index = (ps->next_flow_index + 1) % MAX_FLOW_EVENTS;
  ps->flow_events[id].active = true;
  ps->flow_events[id].time = ps->current_time;
  ps->flow_events[id].strength = strength;
  ps->flow_events[id].creation_tick = (int)(ps->current_time * 50.0 + 0.1);
  glm_vec2_copy(pos, ps->flow_events[id].pos);
}

static void flow_get(particle_system_t *ps, double sim_time, vec2 pos, vec2 out_vel) {
  out_vel[0] = 0;
  out_vel[1] = 0;
  for (int i = 0; i < MAX_FLOW_EVENTS; ++i) {
    if (!ps->flow_events[i].active) continue;
    double age = sim_time - ps->flow_events[i].time;
    if (age < 0 || age > 1.5) continue; // Decays fully after 1.5s

    // Match reference: 0.85 decay per tick (50Hz)
    float decay = powf(0.85f, (float)(age * 50.0));
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

static void move_point(map_data_t *map, vec2 *inout_pos, vec2 *inout_vel, float elasticity) {
  if (!map) {
    glm_vec2_add(*inout_pos, *inout_vel, *inout_pos);
    return;
  }
  vec2 pos = {(*inout_pos)[0], (*inout_pos)[1]};
  vec2 vel = {(*inout_vel)[0], (*inout_vel)[1]};
  vec2 next_pos;
  glm_vec2_add(pos, vel, next_pos);
  int width = map->width, height = map->height;
  int tx = (int)(next_pos[0] / 32.0f), ty = (int)(next_pos[1] / 32.0f);
  bool collision = (tx < 0 || tx >= width || ty < 0 || ty >= height)
                       ? true
                       : (map->game_layer.data[ty * width + tx] == 1 || map->game_layer.data[ty * width + tx] == 3);

  if (collision) {
    int curr_tx = (int)(pos[0] / 32.0f), curr_ty = (int)(pos[1] / 32.0f);
    bool hit_x = false;
    int check_tx = (int)((pos[0] + vel[0]) / 32.0f);
    if (check_tx != curr_tx) {
      if (check_tx >= 0 && check_tx < width && curr_ty >= 0 && curr_ty < height) {
        if (map->game_layer.data[curr_ty * width + check_tx] == 1 || map->game_layer.data[curr_ty * width + check_tx] == 3) hit_x = true;
      } else hit_x = true;
    }
    if (hit_x) {
      vel[0] *= -elasticity;
    } else {
      vel[1] *= -elasticity;
    }
    glm_vec2_scale(vel, 0.5f, vel);
    (*inout_vel)[0] = vel[0];
    (*inout_vel)[1] = vel[1];
  }
  glm_vec2_add(pos, vel, *inout_pos);
}

void particle_system_update(particle_system_t *ps, float dt, map_data_t *map) {
  (void)ps;
  (void)dt;
  (void)map;
}

void particle_system_render(particle_system_t *ps, gfx_handler_t *gfx, int layer) {
  int groups_back[] = {GROUP_PROJECTILE_TRAIL, GROUP_TRAIL_EXTRA};
  int groups_front[] = {GROUP_EXPLOSIONS, GROUP_EXTRA, GROUP_GENERAL};
  int *groups = layer == 0 ? groups_back : groups_front;
  int count = layer == 0 ? 2 : 3;
  int current_atlas_type = -1;
  atlas_renderer_t *current_ar = NULL;

  for (int i = 0; i < MAX_PARTICLES; ++i) {
    particle_t *p = &ps->particles[i];
    if (p->life_span <= 0.0001f) continue;
    if (p->spawn_time > ps->current_time) continue;
    bool group_match = false;
    for (int g = 0; g < count; ++g)
      if (p->group == groups[g]) {
        group_match = true;
        break;
      }
    if (!group_match) continue;

    double age = ps->current_time - p->spawn_time;
    if (age > p->life_span) continue;

    uint32_t sim_seed = p->seed;

    vec2 pos, vel, prev_pos;
    glm_vec2_copy(p->start_pos, pos);
    glm_vec2_copy(p->start_vel, vel);
    glm_vec2_copy(pos, prev_pos);

    float rot = p->rot + p->rot_speed * (float)age;
    double sim_time = 0.0;
    const double step = 0.02;

    while (sim_time < age) {
      glm_vec2_copy(pos, prev_pos);
      float dt = (float)step;

      vel[1] += p->gravity * dt;

      if (p->flow_affected > 0.0f) {
        vec2 flow_vel;
        flow_get(ps, p->spawn_time + sim_time, pos, flow_vel);
        vel[0] += flow_vel[0] * p->flow_affected * dt;
        vel[1] += flow_vel[1] * p->flow_affected * dt;
      }

      if (p->friction > 0.0f) {
        glm_vec2_scale(vel, powf(p->friction, dt / 0.05f), vel);
      }

      vec2 move;
      glm_vec2_scale(vel, dt, move);
      if (p->collides && gfx->map_data) {
        float elasticity = 0.1f + 0.9f * deterministic_frand(&sim_seed);
        move_point(gfx->map_data, &pos, &move, elasticity);
        if (dt > 0.0001f) {
          glm_vec2_scale(move, 1.0f / dt, vel);
        }
      } else {
        glm_vec2_add(pos, move, pos);
      }

      sim_time += step;
    }

    float t = (float)((age - (sim_time - step)) / step);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    glm_vec2_lerp(prev_pos, pos, t, pos);

    int atlas_type = (p->sprite_index < PARTICLE_SPRITE_OFFSET) ? 1 : (p->sprite_index < EXTRA_SPRITE_OFFSET ? 2 : 3);
    atlas_renderer_t *target_ar =
        (atlas_type == 1) ? &gfx->renderer.gameskin_renderer : (atlas_type == 2 ? &gfx->renderer.particle_renderer : &gfx->renderer.extras_renderer);
    int render_sprite_idx = p->sprite_index - (atlas_type == 1 ? 0 : (atlas_type == 2 ? PARTICLE_SPRITE_OFFSET : EXTRA_SPRITE_OFFSET));

    if (atlas_type != current_atlas_type) {
      current_atlas_type = atlas_type;
      current_ar = target_ar;
    }
    float life_frac = (float)age / p->life_span;
    float size = p->start_size * (1.0f - life_frac) + p->end_size * life_frac;
    vec4 col;
    glm_vec4_copy(p->color, col);
    if (p->use_alpha_fading) col[3] = p->start_alpha * (1.0f - life_frac) + p->end_alpha * life_frac;
    renderer_submit_atlas(gfx, current_ar, layer ? Z_LAYER_PARTICLES_FRONT : Z_LAYER_PARTICLES_BACK, (vec2){pos[0] / 32.f, pos[1] / 32.f}, (vec2){size / 32.f, size / 32.f}, rot, render_sprite_idx, false, col, false);
  }
}

void particles_create_explosion(particle_system_t *ps, vec2 pos) {
  flow_add(ps, pos, 5000.0f);
  particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = 0.4f;
  p.start_size = 150.0f;
  p.end_size = 0.0f;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.sprite_index = PARTICLE_EXPL01 + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, 1}, p.color);
  particle_spawn(ps, GROUP_EXPLOSIONS, &p, 0);

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
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void particles_create_smoke(particle_system_t *ps, vec2 pos, vec2 vel, float alpha, float time_passed) {
  particle_t p = {0};
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
  particle_spawn(ps, GROUP_PROJECTILE_TRAIL, &p, time_passed);
}

void particles_create_skid_trail(particle_system_t *ps, vec2 pos, vec2 vel, int direction, float alpha) {
  particle_t p = {0};
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
  particle_spawn(ps, GROUP_GENERAL, &p, 0);
}

void particles_create_bullet_trail(particle_system_t *ps, vec2 pos, float alpha, float time_passed) {
  particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = ps_frand_range(ps, 0.25, 0.5);
  p.start_size = 8;
  p.end_size = 0;
  p.friction = 0.7;
  p.sprite_index = PARTICLE_BALL + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  particle_spawn(ps, GROUP_PROJECTILE_TRAIL, &p, time_passed);
}

void particles_create_player_death(particle_system_t *ps, vec2 pos, vec4 blood_color) {
  for (int i = 0; i < 64; ++i) {
    particle_t p = {0};
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
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void particles_create_confetti(particle_system_t *ps, vec2 pos, float alpha) {
  vec4 cols[] = {{1, 0.4, 0.4, 1}, {0.4, 1, 0.4, 1}, {0.4, 0.4, 1, 1}, {1, 1, 0.4, 1}, {0.4, 1, 1, 1}, {1, 0.4, 1, 1}};
  for (int i = 0; i < 64; ++i) {
    particle_t p = {0};
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
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void particles_create_star(particle_system_t *ps, vec2 pos) {
  particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.start_vel[1] = -200;
  p.life_span = 1;
  p.start_size = 32;
  p.end_size = 32;
  p.sprite_index = GAMESKIN_STAR_1;
  glm_vec4_copy((vec4){1, 1, 1, 1}, p.color);
  particle_spawn(ps, GROUP_EXTRA, &p, 0);
}

void particles_create_hammer_hit(particle_system_t *ps, vec2 pos, float alpha) {
  particle_t p = {0};
  glm_vec2_copy(pos, p.start_pos);
  p.life_span = 0.3f;
  p.start_size = 120;
  p.rot = ps_frand01(ps) * 2 * M_PI;
  p.sprite_index = PARTICLE_HIT01 + PARTICLE_SPRITE_OFFSET;
  glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
  particle_spawn(ps, GROUP_EXPLOSIONS, &p, 0);
}

void particles_create_air_jump(particle_system_t *ps, vec2 pos, float alpha) {
  vec2 off = {-6, 16};
  for (int i = 0; i < 2; ++i) {
    particle_t p = {0};
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
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
    off[0] = 6.0f;
  }
}

void particles_create_player_spawn(particle_system_t *ps, vec2 pos, float alpha) {
  for (int i = 0; i < 32; ++i) {
    particle_t p = {0};
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
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}

void particles_create_powerup_shine(particle_system_t *ps, vec2 pos, vec2 size, float alpha) {
  particle_t p = {0};
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
  particle_spawn(ps, GROUP_GENERAL, &p, 0);
}

void particles_create_freezing_flakes(particle_system_t *ps, vec2 pos, vec2 size, float alpha) {
  particle_t p = {0};
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
  particle_spawn(ps, GROUP_EXTRA, &p, 0);
}

void particles_create_sparkle(particle_system_t *ps, vec2 pos, float alpha) {
  particle_t p = {0};
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
  particle_spawn(ps, GROUP_TRAIL_EXTRA, &p, 0);
}

void particles_create_damage_ind(particle_system_t *ps, vec2 pos, vec2 dir, float alpha) {
  (void)dir;
  for (int i = 0; i < 6; ++i) {
    particle_t p = {0};
    glm_vec2_copy(pos, p.start_pos);
    vec2 rd;
    random_direction(ps, rd);
    float s = 300 + ps_frand01(ps) * 300;
    p.start_vel[0] = rd[0] * s;
    p.start_vel[1] = rd[1] * s;
    p.life_span = 0.5 + ps_frand01(ps) * 0.3;
    p.start_size = 32 + ps_frand01(ps) * 16;
    p.end_size = 0;
    p.gravity = 500;
    p.friction = 0.8;
    p.sprite_index = GAMESKIN_STAR_1;
    glm_vec4_copy((vec4){1, 1, 1, alpha}, p.color);
    particle_spawn(ps, GROUP_GENERAL, &p, 0);
  }
}
