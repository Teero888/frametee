// The car's particles (particle_spec.md) but the tyre marks (tmuf_marks.c):
// the asphalt smoke, the Stadium's dirt smoke and gravel, the water spray
// over water and the splash when the car lands in it.
//
//   emitters  the car's CSceneVehicleEmitters (tmuf_vehicle_emitter), each
//             tick switched on and placed as CSceneVehicle::VisualUpdateAsync
//             does (§3): the wheel ones by their wheel's ground material and
//             sliding, the spray over a Water surface within 50 m under the
//             car faster than 30 km/h, the splash at each water landing
//   birth     every type a ring of MaxParticleCount (all its emitters'), its
//             particles born by its BirthStepType (§4: every BirthMinDist
//             metres, every BirthPeriod, or a burst of SplashPartCount) with
//             the type's random spreads (§5)
//   life      each drawn at the frame's time from its birth values: the
//             friction's exponential drag toward rest and its weight, its
//             size, colour and transparency over its life (§6)
//   drawing   camera-facing sprites (the smoke back to front), the spray a
//             V-shaped ribbon of its particles (§7), its picture x colour
//             without light or fog, blended, after the car (§8)
//
// The game emits at rendered frames on its own random generator; here at
// ticks on one seeded by the tick, so a run looks the same each time it is
// drawn (re-run from three seconds before after a jump on the timeline).

#include "tmuf_internal.h"

#include "particle_frag_spv.h"
#include "particle_vert_spv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GROUPS 16u
#define MAX_EMITTERS 32u
#define QUALITY 2       // the car particles' quality at the game's highest settings
#define REPLAY_TICKS 300 // the longest life (2.5 s) and more
#define WATER 13u        // EPlugSurfaceMaterialId Water

typedef struct particle_vertex {
  float pos[3];
  float uv[2];
  uint32_t color; // RGBA8
} particle_vertex;

typedef struct particle_uniforms {
  float view_proj[16];
  float mode[4];
  float uv0[8], uv1[8];
} particle_uniforms;

typedef struct part {
  uint32_t birth, death; // ms
  float p0[3], v0[3];
  float x_axis[3];       // its birth rotation's x (the spray's across)
  float vz;              // its speed along its z at birth (the spray's rise)
  float size, size_x, weight, friction, roll0, roll_speed, alpha0, intensity;
  float color[3];
  int32_t cell; // atlas cell, -1 none
  int32_t prev; // the emitter's particle before it, -1 none
  bool restart; // its strip starts at it
  float u;      // along its strip
  uint8_t emitter;
} part;

typedef struct group {
  const tmuf_particle_type *type;
  const tmuf_visual_material *material;
  uint32_t pictures[2]; // its layers' pictures (UINT32_MAX none)
  int mode;             // 0 picture x colour, 1 the spray's stem, 2 the splash foam
  part *parts;
  uint32_t cap, next, count;
} group;

typedef struct emit_state { // an emitter's emission into one group
  uint32_t group;
  bool have, restart;
  uint32_t t;      // its last emission's time (ms)
  float l[3];      // and place
  int32_t newest;  // its newest particle, -1 none
} emit_state;

typedef struct emitter {
  const tmuf_vehicle_emitter *e;
  uint32_t state_count;
  emit_state states[2];
  // this tick's emit parameters (world)
  bool active;
  float rot[3][3], pos[3], speed[3], intensity;
} emitter;

struct tm_particles {
  const tm_car_model *tint_car; // whose LightFromMap colours the particles emitted now (tm_particles_update)
  const tmuf_track *track;
  const tmuf_vehicle_visuals *vv;
  tm_picture_table pictures;
  tg_program *program;
  group groups[MAX_GROUPS];
  uint32_t group_count;
  emitter emitters[MAX_EMITTERS];
  uint32_t emitter_count;
  int32_t tick; // the last tick run, -1 none
  uint32_t rng; // this tick's random generator (CRT rand's)
};

// --- randomness: the MSVC CRT's rand (the game's), seeded per tick -------------

static float rand01(tm_particles *s) {
  s->rng = s->rng * 214013u + 2531011u;
  return (float)((s->rng >> 16) & 0x7fffu) / 32767.f;
}
static float spread(tm_particles *s, float value, float variation) {
  return value + variation * (2.f * rand01(s) - 1.f);
}

// --- curves --------------------------------------------------------------------

// CFuncEnvelope::GetValue (amp 0 in every model)
static float curve_at(const tmuf_particle_curve *c, float t) {
  t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
  const float xs[4] = {0.f, c->t1, c->t2, 1.f};
  for (int i = 0; i < 3; i++)
    if (t <= xs[i + 1] || i == 2) {
      const float d = xs[i + 1] - xs[i];
      const float k = d > 1e-6f ? (t - xs[i]) / d : 1.f;
      return c->v[i] + (c->v[i + 1] - c->v[i]) * (k < 0.f ? 0.f : k > 1.f ? 1.f : k);
    }
  return c->v[3];
}

// CFuncColorGradient::GetValue
static void gradient_at(const tmuf_particle_gradient *g, float t, float out[3]) {
  t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
  const float xs[4] = {0.f, g->t1, g->t2, 1.f};
  int i = 0;
  while (i < 2 && t > xs[i + 1])
    i++;
  const float d = xs[i + 1] - xs[i];
  float k = d > 1e-6f ? (t - xs[i]) / d : 1.f;
  k = k < 0.f ? 0.f : k > 1.f ? 1.f : k;
  for (int c = 0; c < 3; c++)
    out[c] = g->c[i][c] + (g->c[i + 1][c] - g->c[i][c]) * k;
}

// --- set up ----------------------------------------------------------------------

static const tmuf_visual_texture *sampler_of(const tmuf_visual_material *m, const char *name) {
  for (uint32_t i = 0; m && i < m->texture_count; i++)
    if (m->textures[i].sampler && !strcmp(m->textures[i].sampler, name)) return &m->textures[i];
  return NULL;
}

static uint32_t group_of(tm_particles *s, ft_game *game, const tmuf_particle_type *t) {
  for (uint32_t i = 0; i < s->group_count; i++)
    if (s->groups[i].type == t) return i;
  if (s->group_count == MAX_GROUPS || !t->max_particle_count) return UINT32_MAX;
  group *g = &s->groups[s->group_count];
  memset(g, 0, sizeof *g);
  g->type = t;
  g->cap = t->max_particle_count;
  g->parts = calloc(g->cap, sizeof *g->parts);
  if (!g->parts) return UINT32_MAX;
  g->material = t->material < s->vv->visuals.material_count ? &s->vv->visuals.materials[t->material] : NULL;
  g->pictures[0] = g->pictures[1] = UINT32_MAX;
  const tmuf_visual_material *m = g->material;
  if (t->particle_type == TMUF_PARTICLES_MULTI_STATE && t->multi_state_render_mode == 5) {
    g->mode = 1; // Stem.Shader: its layers 0 and 1
    g->pictures[0] = tm_map_index(game, &s->pictures, sampler_of(m, "Layer 0"));
    g->pictures[1] = tm_map_index(game, &s->pictures, sampler_of(m, "Layer 1"));
  } else if (sampler_of(m, "Foam1")) {
    g->mode = 2; // WaterFoam.Shader: Foam1 x Foam2
    g->pictures[0] = tm_map_index(game, &s->pictures, sampler_of(m, "Foam1"));
    g->pictures[1] = tm_map_index(game, &s->pictures, sampler_of(m, "Foam2"));
  } else {
    g->mode = 0;
    g->pictures[0] = m && m->texture_count ? tm_map_index(game, &s->pictures, &m->textures[0]) : UINT32_MAX;
  }
  if (g->pictures[0] == UINT32_MAX) {
    free(g->parts);
    return UINT32_MAX;
  }
  return s->group_count++;
}

tg_program *tm_particle_program_create(ft_game *game) {
  const tg_attr attrs[] = {{0, offsetof(particle_vertex, pos), TG_FLOAT3},
                           {1, offsetof(particle_vertex, uv), TG_FLOAT2},
                           {2, offsetof(particle_vertex, color), TG_UINT1}};
  const tg_program_desc desc = {.vertex_spirv = k_particle_vert_spv,
                                .vertex_spirv_size = sizeof k_particle_vert_spv,
                                .fragment_spirv = k_particle_frag_spv,
                                .fragment_spirv_size = sizeof k_particle_frag_spv,
                                .vertex_stride = sizeof(particle_vertex),
                                .attrs = attrs,
                                .attr_count = 3,
                                .uniform_size = sizeof(particle_uniforms),
                                .texture_count = 2};
  return tg_program_create(game->gpu, &desc);
}

tm_particles *tm_particles_create(ft_game *game, ft_level *level) {
  const tmuf_vehicle_visuals *vv = tmuf_track_vehicle_visuals(level->track);
  if (!vv || !game->gpu) return NULL;
  tm_particles *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->track = level->track;
  s->vv = vv;
  s->tick = -1;
  tm_picture_table_init(game, &s->pictures, level->track);
  s->program = tm_particle_program_create(game);
  for (uint32_t i = 0; i < vv->emitter_count && s->emitter_count < MAX_EMITTERS; i++) {
    const tmuf_vehicle_emitter *e = &vv->emitters[i];
    const tmuf_particle_model *model = e->models[QUALITY];
    if (!model || e->kind == TMUF_EMITTER_OFF || e->kind == TMUF_EMITTER_LIGHT_TRAIL) continue;
    emitter *em = &s->emitters[s->emitter_count];
    memset(em, 0, sizeof *em);
    em->e = e;
    for (uint32_t k = 0; k < model->type_count && em->state_count < 2; k++) {
      const tmuf_particle_type *t = &model->types[k];
      // the marks are tmuf_marks.c's; the grass marks are not drawn yet
      if (t->particle_type == TMUF_PARTICLES_MULTI_STATE && t->multi_state_render_mode != 5) continue;
      if (t->particle_type == TMUF_PARTICLES_ONE) continue;
      const uint32_t g = group_of(s, game, t);
      if (g == UINT32_MAX) continue;
      emit_state *st = &em->states[em->state_count++];
      memset(st, 0, sizeof *st);
      st->group = g;
      st->newest = -1;
      st->restart = true;
    }
    if (em->state_count) s->emitter_count++;
  }
  return s;
}

void tm_particles_destroy(ft_game *game, tm_particles *s) {
  if (!s) return;
  for (uint32_t i = 0; i < s->group_count; i++)
    free(s->groups[i].parts);
  if (s->program) tg_program_destroy(game->gpu, s->program);
  tm_picture_table_free(game, &s->pictures);
  free(s);
}

static void reset(tm_particles *s) {
  for (uint32_t i = 0; i < s->group_count; i++)
    s->groups[i].next = s->groups[i].count = 0;
  for (uint32_t i = 0; i < s->emitter_count; i++) {
    emitter *em = &s->emitters[i];
    em->active = false;
    for (uint32_t k = 0; k < em->state_count; k++) {
      em->states[k].have = false;
      em->states[k].restart = true;
      em->states[k].newest = -1;
    }
  }
  s->tick = -1;
}

// --- the emitters each tick (CSceneVehicle::VisualUpdateAsync) --------------------

static void mat_mul(const float a[3][3], const float b[3][3], float out[3][3]) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}
static void to_world(const tm_pose *p, const float local[3], float out[3]) {
  for (int k = 0; k < 3; k++)
    out[k] = p->rotation[k][0] * local[0] + p->rotation[k][1] * local[1] + p->rotation[k][2] * local[2] +
             (&p->position.x)[k];
}

static bool wheel_sliding(const tmuf_world *a, const tmuf_world *b, uint32_t i) {
  return a->sim.car.wheels[i].contact && b->sim.car.wheels[i].contact && a->sim.car.wheels[i].slipping &&
         b->sim.car.wheels[i].slipping;
}

static void wheel_emitter(emitter *em, const tmuf_world *a, const tmuf_world *b, const tm_pose *pose) {
  const tmuf_vehicle_emitter *e = em->e;
  const tmuf_car *car = &b->sim.car;
  const bool burnout = car->geared.wheel_speed_override != 0;
  em->active = true;
  if (e->wheel != UINT32_MAX) {
    if (e->wheel >= car->wheel_count) {
      em->active = false;
      return;
    }
    // the material at the previous tick, with contact at both
    const bool contact = a->sim.car.wheels[e->wheel].contact && car->wheels[e->wheel].contact;
    const uint32_t material = contact ? a->sim.car.wheels[e->wheel].contact_material : 0xffffu;
    bool in = e->any_material;
    for (uint32_t k = 0; k < e->material_count && !in; k++)
      in = material == e->materials[k];
    bool all = true;
    for (uint32_t i = 0; i < car->wheel_count && all; i++)
      all = wheel_sliding(a, b, i);
    if (!in) em->active = false;
    else if (e->needs_all_sliding && !burnout && !all) em->active = false;
    else if (e->needs_sliding && !wheel_sliding(a, b, e->wheel) && !burnout) em->active = false;
  }
  if (!em->active) return;
  const float(*r)[3] = e->location.r.m;
  float local[3] = {e->location.t.x, e->location.t.y, e->location.t.z};
  if (e->wheel != UINT32_MAX) {
    const tmuf_car_wheel *w = &car->wheels[e->wheel];
    local[0] += w->cur_iso.t.x, local[1] += w->cur_iso.t.y - w->rolling_radius, local[2] += w->cur_iso.t.z;
  }
  const float vx = car->frame.side_speed, vz = car->frame.forward_speed;
  const float *p = e->params;
  em->intensity = p[0] + p[1] * fabsf(vx) + p[3] * fabsf(vz) + (p[4] != 0.f && burnout ? 1.f : 0.f);
  const float v[3] = {vx, 0.f, vz};
  float speed[3];
  for (int k = 0; k < 3; k++)
    speed[k] = p[5 + k] + p[8 + k] * v[k] + (burnout ? p[11 + k] * v[k] : 0.f);
  float rot[3][3];
  memcpy(rot, r, sizeof rot);
  mat_mul(pose->rotation, rot, em->rot);
  to_world(pose, local, em->pos);
  for (int k = 0; k < 3; k++)
    em->speed[k] = pose->rotation[k][0] * speed[0] + pose->rotation[k][1] * speed[1] + pose->rotation[k][2] * speed[2];
}

// the spray's speed curve (CSceneVehicleTuning +0x24, the same in every
// car's tuning): 0 up to 30 km/h, 1 from 100
static float spray_curve(float kmh) {
  return kmh <= 30.f ? 0.f : kmh >= 100.f ? 1.f : (kmh - 30.f) / 70.f;
}

static void water_emitter(tm_particles *s, emitter *em, const tmuf_world *b, const tm_pose *pose) {
  em->active = false;
  const tmuf_box *box = &b->sim.def.water_box;
  const float top[3] = {box->center.x, box->center.y + fabsf(box->half.y), box->center.z};
  float p0[3];
  to_world(pose, top, p0);
  const float seg[3] = {0.f, -50.f, 0.f};
  float t = 1.f;
  uint32_t material = UINT32_MAX;
  if (!tmuf_track_segment_hit(s->track, p0, seg, &t, &material) || material != WATER) return;
  const tmuf_vec3 lin = b->sim.body.state.lin;
  const float sp = sqrtf(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z);
  if (sp <= 1e-5f) return;
  const float k = spray_curve(sp * 3.6f);
  if (k <= 1e-5f) return;
  const float dir[3] = {lin.x / sp, lin.y / sp, lin.z / sp};
  float left[3] = {-dir[2], 0.f, dir[0]};
  const float l2 = left[0] * left[0] + left[2] * left[2];
  if (l2 <= 1e-10f) return;
  const float inv = 1.f / sqrtf(l2);
  left[0] *= inv, left[2] *= inv;
  // SetDOVandLeftV: z along the travel, x its left, y = z x x
  const float up[3] = {dir[1] * left[2] - dir[2] * left[1], dir[2] * left[0] - dir[0] * left[2],
                       dir[0] * left[1] - dir[1] * left[0]};
  for (int r = 0; r < 3; r++)
    em->rot[r][0] = left[r], em->rot[r][1] = up[r], em->rot[r][2] = dir[r];
  em->pos[0] = pose->position.x + 2.f * dir[0];
  em->pos[1] = b->sim.water.surface_height + 0.1f;
  em->pos[2] = pose->position.z + 2.f * dir[2];
  const float f = 1.f - t;
  em->intensity = (f < 0.f ? 0.f : f > 1.f ? 1.f : f) * k;
  em->speed[0] = em->speed[1] = em->speed[2] = 0.f;
  em->active = true;
}

// --- birth (PartInit, PartBirthParamsInit) -------------------------------------------

static void rot_y(float a, float m[3][3]) {
  const float c = cosf(a), s = sinf(a);
  const float r[3][3] = {{c, 0.f, s}, {0.f, 1.f, 0.f}, {-s, 0.f, c}};
  memcpy(m, r, sizeof r);
}
static void rot_x(float a, float m[3][3]) { // y toward z
  const float c = cosf(a), s = sinf(a);
  const float r[3][3] = {{1.f, 0.f, 0.f}, {0.f, c, s}, {0.f, -s, c}};
  memcpy(m, r, sizeof r);
}
static void rot_z(float a, float m[3][3]) {
  const float c = cosf(a), s = sinf(a);
  const float r[3][3] = {{c, -s, 0.f}, {s, c, 0.f}, {0.f, 0.f, 1.f}};
  memcpy(m, r, sizeof r);
}

static void emit_one(tm_particles *s, emitter *em, emit_state *st, uint32_t time, const float pos[3],
                     const float speed[3]) {
  group *g = &s->groups[st->group];
  const tmuf_particle_type *t = g->type;
  const int32_t slot = (int32_t)g->next;
  g->next = (g->next + 1) % g->cap;
  if (g->count < g->cap) g->count++;
  // the slot's old particle leaves its strip
  for (uint32_t i = 0; i < g->count; i++)
    if (g->parts[i].prev == slot) g->parts[i].prev = -1, g->parts[i].restart = true;
  for (uint32_t i = 0; i < s->emitter_count; i++)
    for (uint32_t k = 0; k < s->emitters[i].state_count; k++)
      if (s->emitters[i].states[k].group == st->group && s->emitters[i].states[k].newest == slot)
        s->emitters[i].states[k].newest = -1;
  part *p = &g->parts[slot];
  memset(p, 0, sizeof *p);
  const float life = spread(s, t->life, t->life_variation);
  p->birth = time;
  p->death = time + (uint32_t)lroundf(1000.f * (life > 0.f ? life : 0.f));
  p->weight = spread(s, t->weight, t->weight_variation);
  const float pitch = spread(s, t->pitch, t->pitch_variation) * 0.0174532925f;
  const float yaw = spread(s, t->yaw, t->yaw_variation) * 0.0174532925f;
  p->roll0 = spread(s, t->roll, t->roll_variation) * 0.0174532925f;
  p->size = spread(s, t->size, t->size_variation);
  p->size_x = p->size * (t->ratio_xy > 0.f ? t->ratio_xy : 1.f);
  p->roll_speed = spread(s, t->roll_speed, t->roll_speed_variation);
  float friction = spread(s, t->fluid_friction, t->fluid_friction_variation);
  if (t->fluid_friction_use_intensity)
    friction *= t->fluid_friction_intensity_base + (1.f - t->fluid_friction_intensity_base) * em->intensity;
  p->friction = friction;
  p->vz = spread(s, t->velocity, t->velocity_variation);
  float ry[3][3], rx[3][3], rz[3][3], ryx[3][3], r[3][3], w[3][3];
  rot_y(yaw, ry), rot_x(pitch, rx), rot_z(p->roll0, rz);
  mat_mul(ry, rx, ryx);
  mat_mul(ryx, rz, r);
  mat_mul(em->rot, r, w);
  for (int k = 0; k < 3; k++) {
    p->x_axis[k] = w[k][0];
    p->v0[k] = w[k][2] * p->vz + speed[k];
    p->p0[k] = pos[k];
  }
  p->intensity = em->intensity;
  if (t->color_gradient_use == 1 || !t->has_color_gradient) {
    p->color[0] = p->color[1] = p->color[2] = 1.f;
  } else {
    gradient_at(&t->color_gradient, rand01(s), p->color);
  }
  // EmitColor: the LightFromMap colour of the wheel's quadrant (the
  // Stadium's dirt smoke and gravel; particle_spec), kept from birth
  float tint[3];
  if (em->e->kind == TMUF_EMITTER_WHEEL && tm_car_lfm_tint(s->tint_car, pos, tint))
    for (int k = 0; k < 3; k++)
      p->color[k] *= tint[k];
  p->alpha0 = t->transparency;
  p->cell = -1;
  if (t->texture_atlas[0] == 1) p->cell = (int32_t)t->texture_atlas[3];
  else if (t->texture_atlas[0] == 2)
    p->cell = (int32_t)lroundf((float)(t->texture_atlas[1] * t->texture_atlas[2]) * rand01(s));
  // its strip
  p->emitter = (uint8_t)(em - s->emitters);
  p->restart = st->restart;
  p->prev = st->restart ? -1 : st->newest;
  if (p->prev >= 0) {
    const part *q = &g->parts[p->prev];
    const float d[3] = {pos[0] - q->p0[0], pos[1] - q->p0[1], pos[2] - q->p0[2]};
    p->u = q->u + sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * t->u_scale_dist;
  }
  st->newest = slot;
  st->have = true;
  st->restart = false;
  st->t = time;
  memcpy(st->l, pos, sizeof st->l);
}

// GroupEmitParticles for one emitter and group at `now`
static void emit(tm_particles *s, emitter *em, emit_state *st, uint32_t now) {
  const tmuf_particle_type *t = s->groups[st->group].type;
  if (!em->active) {
    st->restart = true;
    return;
  }
  switch (t->birth_step_type) {
  case 0: { // Active & FixedPeriod
    if (st->have && st->t < now && now < st->t + 5000u) {
      if (st->restart) {
        emit_one(s, em, st, now, em->pos, em->speed);
        break;
      }
      const uint32_t period = t->birth_period > 0.f ? (uint32_t)lroundf(t->birth_period * 1000.f) : 20u;
      const uint32_t t0 = st->t, n = (now - t0) / (period ? period : 1u);
      float l[3];
      memcpy(l, st->l, sizeof l);
      for (uint32_t k = 1; k <= n; k++) {
        const uint32_t tk = t0 + k * period;
        const float f = (float)(tk - t0) / (float)(now - t0);
        const float pos[3] = {l[0] + (em->pos[0] - l[0]) * f, l[1] + (em->pos[1] - l[1]) * f,
                              l[2] + (em->pos[2] - l[2]) * f};
        emit_one(s, em, st, tk, pos, em->speed);
      }
    } else {
      st->have = true;
      st->t = now;
      memcpy(st->l, em->pos, sizeof st->l);
    }
    break;
  }
  case 1: { // Active & MinDist
    const float d[3] = {em->pos[0] - st->l[0], em->pos[1] - st->l[1], em->pos[2] - st->l[2]};
    if (!st->have || st->restart || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > t->birth_min_dist * t->birth_min_dist)
      emit_one(s, em, st, now, em->pos, em->speed);
    break;
  }
  case 2: emit_one(s, em, st, now, em->pos, em->speed); break;
  case 4: { // Active & FixedDist
    if (!st->have || st->restart) {
      emit_one(s, em, st, now, em->pos, em->speed);
      break;
    }
    const float d3[3] = {em->pos[0] - st->l[0], em->pos[1] - st->l[1], em->pos[2] - st->l[2]};
    const float d = sqrtf(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
    if (d <= 1e-5f || t->birth_min_dist <= 0.f) break;
    const uint32_t n = (uint32_t)(d / t->birth_min_dist);
    if (!n) break;
    const float step[3] = {t->birth_min_dist * d3[0] / d, t->birth_min_dist * d3[1] / d, t->birth_min_dist * d3[2] / d};
    float l[3];
    memcpy(l, st->l, sizeof l);
    const uint32_t t0 = st->t;
    for (uint32_t k = 1; k <= n; k++) {
      // (sic: the step added once before the loop and once each time)
      const float pos[3] = {l[0] + (float)(k + 1) * step[0], l[1] + (float)(k + 1) * step[1],
                            l[2] + (float)(k + 1) * step[2]};
      emit_one(s, em, st, t0 + (now - t0) * k / n, pos, em->speed);
    }
    break;
  }
  case 3: { // the splash burst
    const float *sp = t->splash;
    const uint32_t count = t->splash_part_count;
    float h[3] = {0.f, 0.f, 0.f};
    if (fabsf(em->speed[1]) > 0.01f) h[0] = em->speed[0] / fabsf(em->speed[1]), h[2] = em->speed[2] / fabsf(em->speed[1]);
    for (uint32_t k = 0; k < count; k++) {
      const float a = 6.28318531f * ((float)k + rand01(s) - 0.5f) / (float)count;
      const float b = spread(s, sp[2], sp[3]) * 0.0174532925f;
      const float r = spread(s, sp[0], sp[1]);
      const float vel = spread(s, sp[6], sp[7]);
      const float d[3] = {sinf(b) * cosf(a) * vel, cosf(b) * vel, sinf(b) * sinf(a) * vel};
      const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      const float c = dl * (sp[4] + (sp[5] - sp[4]) * rand01(s));
      const float pos[3] = {em->pos[0] + r * cosf(a), em->pos[1], em->pos[2] + r * sinf(a)};
      const float speed[3] = {d[0] + c * h[0], d[1] + c * h[1], d[2] + c * h[2]};
      emit_one(s, em, st, now, pos, speed);
      // the burst's velocity is its own (the type's Velocity forced to 0)
      group *g = &s->groups[st->group];
      part *p = &g->parts[st->newest];
      memcpy(p->v0, speed, sizeof p->v0);
      p->vz = 0.f;
    }
    break;
  }
  default: break;
  }
}

static void run_tick(tm_particles *s, const ft_world *a, const ft_world *b, int32_t tick) {
  const uint32_t now = tick > 0 ? (uint32_t)tick * TMUF_TICK_MS : 0u;
  s->rng = (uint32_t)tick * 2654435761u ^ 0x5bd1e995u;
  const tm_pose pose = tm_pose_at(a, b, 1.f);
  for (uint32_t i = 0; i < s->emitter_count; i++) {
    emitter *em = &s->emitters[i];
    switch (em->e->kind) {
    case TMUF_EMITTER_WHEEL: wheel_emitter(em, &a->w, &b->w, &pose); break;
    case TMUF_EMITTER_WATER: water_emitter(s, em, &b->w, &pose); break;
    case TMUF_EMITTER_SPLASH:
      em->active = b->w.sim.car.water_splash_events > a->w.sim.car.water_splash_events;
      if (em->active) {
        em->pos[0] = pose.position.x;
        em->pos[1] = b->w.sim.water.surface_height + 0.1f;
        em->pos[2] = pose.position.z;
        memset(em->rot, 0, sizeof em->rot);
        em->rot[0][0] = em->rot[1][1] = em->rot[2][2] = 1.f;
        const tmuf_vec3 v = b->w.sim.car.water_splash_speed;
        em->speed[0] = v.x, em->speed[1] = v.y, em->speed[2] = v.z;
        em->intensity = 1.f;
      }
      break;
    default: em->active = false; break;
    }
    for (uint32_t k = 0; k < em->state_count; k++)
      emit(s, em, &em->states[k], now);
  }
}

// the timeline group the shown world is in (the first when none says so)
static uint32_t timeline_group(const ft_engine_api *api, const ft_render_frame *frame) {
  if (!api->timeline_world_pair || !api->timeline_world_count) return 0;
  for (uint32_t i = 0; i < api->timeline_world_count(); i++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(i, (int32_t)frame->tick, &a, &b) && b == frame->world) return i;
  }
  return 0;
}

void tm_particles_update(ft_game *game, tm_particles *s, const ft_render_frame *frame) {
  if (!s || !frame->world) return;
  const int32_t tick = (int32_t)frame->tick;
  if (tick == s->tick) return;
  const ft_engine_api *api = game->engine;
  if (!api->timeline_world_pair) return;
  // after a jump: the last seconds again (no particle lives longer)
  if (tick < s->tick || tick - s->tick > REPLAY_TICKS) {
    reset(s);
    s->tick = tick - REPLAY_TICKS;
  }
  const uint32_t group = timeline_group(api, frame);
  s->tint_car = game->tint_car;
  for (int32_t t = s->tick + 1 > 1 ? s->tick + 1 : 1; t <= tick; t++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(group, t, &a, &b) && a && b) run_tick(s, a, b, t);
  }
  s->tick = tick;
}

// --- a particle at a time (PartGetState) ---------------------------------------------

typedef struct state {
  float pos[3], sx, sy, roll, rgba[4], x;
} state;

static void state_at(const tmuf_particle_type *t, const part *p, float now, state *out) {
  const float age = (now - (float)p->birth) / 1000.f;
  const float span = (float)(p->death - p->birth);
  const float x = span > 0.f ? (now - (float)p->birth) / span : 1.f;
  const float f = t->has_intensity_filter ? curve_at(&t->intensity_filter, p->intensity) : p->intensity;
  const float k = p->friction, W = p->weight;
  if (k > 1e-5f) {
    const float e = (1.f - expf(-k * age)) / k;
    for (int c = 0; c < 3; c++)
      out->pos[c] = p->p0[c] + p->v0[c] * e;
    out->pos[1] += W / k * (age - e);
  } else {
    for (int c = 0; c < 3; c++)
      out->pos[c] = p->p0[c] + p->v0[c] * age;
    out->pos[1] += 0.5f * W * age * age;
  }
  const float sl = t->has_size_over_life ? curve_at(&t->size_over_life, x) : 1.f;
  out->sy = sl * p->size;
  out->sx = (t->size_use_size_x && t->has_size_x_over_life ? curve_at(&t->size_x_over_life, x) : sl) * p->size_x;
  if (t->size_use_intensity) out->sx *= f, out->sy *= f;
  out->roll = p->roll0 + p->roll_speed * age;
  float rgb[3] = {p->color[0], p->color[1], p->color[2]};
  if (t->color_gradient_use == 1 && t->has_color_gradient) {
    float g[3];
    gradient_at(&t->color_gradient, x, g);
    for (int c = 0; c < 3; c++)
      rgb[c] *= g[c];
  }
  if (t->color_use_intensity)
    for (int c = 0; c < 3; c++)
      rgb[c] *= f;
  float a = p->alpha0 * (t->transparency_use_intensity ? f : 1.f) *
            (t->has_transparency_over_life ? curve_at(&t->transparency_over_life, x) : 1.f);
  a = a < 0.f ? 0.f : a > 1.f ? 1.f : a;
  if (t->color_modulate_with_transparency)
    for (int c = 0; c < 3; c++)
      rgb[c] *= a;
  memcpy(out->rgba, rgb, sizeof rgb);
  out->rgba[3] = a;
  out->x = x;
}

static uint32_t pack_rgba(const float c[4]) {
  uint32_t out = 0;
  for (int k = 0; k < 4; k++) {
    const float v = c[k] < 0.f ? 0.f : c[k] > 1.f ? 1.f : c[k];
    out |= (uint32_t)lroundf(v * 255.f) << (8 * k);
  }
  return out;
}

// --- drawing -------------------------------------------------------------------------

typedef struct sorted {
  uint32_t index;
  float depth;
} sorted;

static int far_first(const void *pa, const void *pb) {
  const sorted *a = pa, *b = pb;
  return a->depth < b->depth ? 1 : a->depth > b->depth ? -1 : 0;
}

// a camera-facing sprite (CLoadGeomDynaSprite::LoadSprite): two triangles
static void sprite(particle_vertex *out, const group *g, const state *st, const part *p, const float right[3],
                   const float up[3], const float fwd[3]) {
  const tmuf_particle_type *t = g->type;
  float A[3], B[3];
  for (int k = 0; k < 3; k++)
    A[k] = 0.5f * st->sy * up[k], B[k] = 0.5f * st->sx * right[k];
  const bool atlas = t->texture_atlas[0] != 0;
  if (!atlas && st->roll != 0.f) { // turned about the view axis
    const float c = cosf(st->roll), s = sinf(st->roll);
    float A2[3], B2[3];
    for (int k = 0; k < 3; k++) {
      const float ca[3] = {fwd[1] * A[2] - fwd[2] * A[1], fwd[2] * A[0] - fwd[0] * A[2], fwd[0] * A[1] - fwd[1] * A[0]};
      const float cb[3] = {fwd[1] * B[2] - fwd[2] * B[1], fwd[2] * B[0] - fwd[0] * B[2], fwd[0] * B[1] - fwd[1] * B[0]};
      A2[k] = A[k] * c + ca[k] * s;
      B2[k] = B[k] * c + cb[k] * s;
    }
    memcpy(A, A2, sizeof A);
    memcpy(B, B2, sizeof B);
  }
  float centre[3];
  for (int k = 0; k < 3; k++)
    centre[k] = st->pos[k] - t->ref_pos[0] * B[k] - t->ref_pos[1] * A[k];
  float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
  if (atlas) {
    const uint32_t cols = t->texture_atlas[1] ? t->texture_atlas[1] : 1u, rows = t->texture_atlas[2] ? t->texture_atlas[2] : 1u;
    const uint32_t cell = p->cell >= 0 && (uint32_t)p->cell < cols * rows ? (uint32_t)p->cell : 0u;
    u0 = (float)(cell % cols) / (float)cols, u1 = (float)(cell % cols + 1u) / (float)cols;
    v0 = (float)(cell / cols) / (float)rows, v1 = (float)(cell / cols + 1u) / (float)rows;
  }
  const uint32_t color = pack_rgba(st->rgba);
  particle_vertex c[4];
  const float sa[4] = {-1.f, -1.f, 1.f, 1.f}, sb[4] = {-1.f, 1.f, -1.f, 1.f};
  const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 3; k++)
      c[i].pos[k] = centre[k] + sa[i] * A[k] + sb[i] * B[k];
    c[i].uv[0] = us[i], c[i].uv[1] = vs[i];
    c[i].color = color;
  }
  static const int tri[6] = {0, 1, 2, 2, 1, 3};
  for (int i = 0; i < 6; i++)
    out[i] = c[tri[i]];
}

// the spray's four vertices of a particle (GroupUpdateMultistateWaterSplash)
static void stem_verts(const part *p, const state *st, particle_vertex out[4]) {
  const float w = (2.f - p->intensity) * st->sx;
  const float h0 = st->x * p->vz - 0.5f * p->weight * st->x * st->x, h = h0 > 0.f ? h0 : 0.f;
  const float off[4] = {-w, -0.5f * w, 0.5f * w, w};
  const uint32_t color = pack_rgba(st->rgba);
  for (int j = 0; j < 4; j++) {
    for (int k = 0; k < 3; k++)
      out[j].pos[k] = p->p0[k] + off[j] * p->x_axis[k];
    if (j == 0 || j == 3) out[j].pos[1] += h;
    out[j].uv[0] = (float)j / 3.f, out[j].uv[1] = p->u;
    out[j].color = color;
  }
}

static uint32_t stem_link(particle_vertex *out, const particle_vertex a[4], const particle_vertex b[4]) {
  uint32_t n = 0;
  for (int j = 0; j < 3; j++) {
    const particle_vertex q[6] = {a[j], a[j + 1], b[j], b[j], a[j + 1], b[j + 1]};
    memcpy(out + n, q, sizeof q);
    n += 6;
  }
  return n;
}

void tm_particles_render(ft_game *game, tm_particles *s, const ft_render_frame *frame) {
  if (!s || !s->program || !frame->world) return;
  const ft_camera *cam = &frame->state.camera;
  const float alpha = frame->previous_world ? (frame->alpha < 0.f ? 0.f : frame->alpha > 1.f ? 1.f : frame->alpha) : 1.f;
  const float now = ((float)frame->tick - 1.f + alpha) * (float)TMUF_TICK_MS;
  const float fwd[3] = {cam->forward.x, cam->forward.y, cam->forward.z};
  const float cu[3] = {cam->up.x, cam->up.y, cam->up.z};
  float right[3] = {fwd[1] * cu[2] - fwd[2] * cu[1], fwd[2] * cu[0] - fwd[0] * cu[2], fwd[0] * cu[1] - fwd[1] * cu[0]};
  const float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (int k = 0; k < 3; k++)
    right[k] = rl > 0.f ? right[k] / rl : 0.f;
  const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                       right[0] * fwd[1] - right[1] * fwd[0]};
  const tg_state state_blend = {.blend_src = TG_BLEND_SRCALPHA, .blend_dst = TG_BLEND_INVSRCALPHA, .depth_test = true,
                                .cull = TG_CULL_NONE};
  const double seconds = (double)now / 1000.0;
  for (uint32_t gi = 0; gi < s->group_count; gi++) {
    const group *g = &s->groups[gi];
    if (!g->count) continue;
    particle_uniforms u;
    memset(&u, 0, sizeof u);
    memcpy(u.view_proj, cam->view_proj, sizeof u.view_proj);
    u.mode[0] = (float)g->mode;
    static const float ident[8] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    memcpy(u.uv0, ident, sizeof ident);
    memcpy(u.uv1, ident, sizeof ident);
    const tmuf_visual_material *m = g->material;
    if (g->mode == 1) {
      const tmuf_visual_texture *l0 = sampler_of(m, "Layer 0"), *l1 = sampler_of(m, "Layer 1");
      if (l0) tm_texture_uv(l0, seconds, u.uv0);
      if (l1) tm_texture_uv(l1, seconds, u.uv1);
    } else if (g->mode == 2) {
      const tmuf_visual_texture *f1 = sampler_of(m, "Foam1"), *f2 = sampler_of(m, "Foam2");
      if (f1) tm_texture_uv(f1, seconds, u.uv0);
      if (f2) tm_texture_uv(f2, seconds, u.uv1);
    }
    particle_vertex *verts = NULL;
    uint32_t n = 0;
    if (g->mode == 1) {
      // the spray: each emitter's chain from its newest particle
      verts = malloc(sizeof *verts * 18 * (g->count + s->emitter_count));
      if (!verts) continue;
      for (uint32_t ei = 0; ei < s->emitter_count; ei++) {
        const emitter *em = &s->emitters[ei];
        for (uint32_t k = 0; k < em->state_count; k++) {
          const emit_state *est = &em->states[k];
          if (est->group != gi || est->newest < 0) continue;
          int32_t i = est->newest;
          state st;
          const part *p = &g->parts[i];
          if (now < (float)p->birth || now >= (float)p->death) continue;
          particle_vertex cur[4], prev[4];
          state_at(g->type, p, now, &st);
          stem_verts(p, &st, cur);
          // the live link to the emitter, a point there
          if (em->active && !est->restart) {
            particle_vertex live[4];
            for (int j = 0; j < 4; j++) {
              memcpy(live[j].pos, em->pos, sizeof live[j].pos);
              live[j].uv[0] = (float)j / 3.f, live[j].uv[1] = p->u;
              live[j].color = cur[j].color;
            }
            n += stem_link(verts + n, live, cur);
          }
          for (int steps = 0; steps < (int)g->cap; steps++) {
            const int32_t q = g->parts[i].prev;
            if (q < 0 || g->parts[i].restart) break;
            const part *pq = &g->parts[q];
            if (now < (float)pq->birth || now >= (float)pq->death) break;
            state sq;
            state_at(g->type, pq, now, &sq);
            stem_verts(pq, &sq, prev);
            n += stem_link(verts + n, cur, prev);
            memcpy(cur, prev, sizeof cur);
            i = q;
          }
        }
      }
    } else {
      // sprites, in pool order or back to front
      sorted *order = malloc(sizeof *order * g->count);
      verts = malloc(sizeof *verts * 6 * g->count);
      if (!order || !verts) {
        free(order);
        free(verts);
        continue;
      }
      uint32_t live = 0;
      state *states = malloc(sizeof *states * g->count);
      if (!states) {
        free(order);
        free(verts);
        continue;
      }
      for (uint32_t i = 0; i < g->count; i++) {
        const part *p = &g->parts[i];
        if (now < (float)p->birth || now >= (float)p->death) continue;
        state_at(g->type, p, now, &states[i]);
        const float d[3] = {states[i].pos[0] - cam->eye.x, states[i].pos[1] - cam->eye.y, states[i].pos[2] - cam->eye.z};
        order[live++] = (sorted){i, d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]};
      }
      if (g->type->sort_sprites) qsort(order, live, sizeof *order, far_first);
      for (uint32_t k = 0; k < live; k++)
        sprite(verts + 6 * k, g, &states[order[k].index], &g->parts[order[k].index], right, up, fwd);
      n = 6 * live;
      free(states);
      free(order);
    }
    if (n) {
      const tg_texture textures[2] = {
          s->pictures.textures[g->pictures[0]],
          g->pictures[1] != UINT32_MAX ? s->pictures.textures[g->pictures[1]] : s->pictures.textures[g->pictures[0]]};
      const tg_sampler wrap = {1, 1, 3, 2}, clamp = {3, 3, 3, 2};
      const tg_sampler samplers[2] = {g->mode == 1 ? wrap : clamp, g->mode == 1 ? wrap : clamp};
      tg_draw_dynamic(game->gpu, s->program, &state_blend, verts, n, textures, samplers, &u);
    }
    free(verts);
  }
}
