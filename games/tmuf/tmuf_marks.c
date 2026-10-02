// The wheels' marks (skid_spec.md): the car's wheel emitters whose particle
// type is a flat ribbon (MultiState QuadCenterLeft: the asphalt and dirt
// marks) lay a particle every BirthMinDist while their conditions hold (the
// wheel on one of their ground materials, sliding if they need it); the
// particles of an emitter make a strip, 2 Size wide across the car's speed,
// its texture repeated every 1 / UScaleDist metres. Each particle type keeps
// a ring of MaxParticleCount, shared by the emitters using it, the oldest
// overwritten. The game emits at rendered frames; here at ticks (its
// particles are within one frame of travel of these). The marks add up
// from the start of the run: the ticks up to the one shown are run once, in
// order; after a jump back, again from the newest checkpoint before it (the
// marks as they were, every half second).
//
// The Stadium's grass marks (GrassMarks: GrassMark #1 and #2, born
// together) are the same strips without the live link to the wheel: a decal
// on the ground (GrassMarkGround), and the tufts flattened over them for
// 30 s (GrassMarkFenceIntens: its alpha into the frame's with MIN before the
// tufts read it, tm_marks_flatten).

#include "tmuf_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RINGS 8u
#define MAX_EMITTERS 32u
#define CHECKPOINT_TICKS 50
#define MAX_CHECKPOINTS 240u // two minutes back

typedef struct mark {
  float pos[3], left[3];
  float u, half_width;
  uint32_t birth_ms, death_ms;
  int32_t prev; // the particle before it on its strip, -1 for none
  bool brk;     // its strip starts at it
  uint32_t color; // BGRA: its birth colour (the LightFromMap under its wheel, else white)
} mark;

enum { RIBBON, GRASS_GROUND, GRASS_FLATTEN };

typedef struct ring {
  const tmuf_particle_type *type;
  int kind;         // RIBBON, GRASS_*
  uint32_t picture; // index into the pictures, UINT32_MAX for none
  uint32_t tile;    // the grass decal's MapBlendXZ, UINT32_MAX for none
  mark *parts;
  uint32_t cap, next, count;
} ring;

typedef struct emitter {
  const tmuf_vehicle_emitter *e;
  uint32_t ring;
  int32_t last; // its newest particle, -1 for none
  float last_pos[3]; // where it emitted last (the origin before)
  bool brk;
  bool active; // at the tick run last
} emitter;

// the marks at a tick: the rings' particles, where each ring and emitter was
typedef struct checkpoint {
  int32_t tick;
  mark *parts; // the rings' particles one after the other
  uint32_t next[MAX_RINGS], count[MAX_RINGS];
  emitter emitters[MAX_EMITTERS];
} checkpoint;

struct tm_marks {
  const tmuf_track *track;
  const tmuf_vehicle_visuals *vv;
  tm_picture_table pictures;
  ring rings[MAX_RINGS];
  uint32_t ring_count;
  emitter emitters[MAX_EMITTERS];
  uint32_t emitter_count;
  int32_t tick; // the last tick run, -1 for none
  const tm_car_model *tint_car; // whose LightFromMap colours the marks laid now (tm_marks_update)
  checkpoint checkpoints[MAX_CHECKPOINTS]; // by tick, a ring (the newest on top)
  uint32_t checkpoint_top, checkpoint_count;
  uint32_t part_total; // the rings' capacities together
};

// the particle quality the game's settings give at their highest
enum { QUALITY = 2 };

tm_marks *tm_marks_create(ft_game *game, ft_level *level) {
  const tmuf_vehicle_visuals *vv = tmuf_track_vehicle_visuals(level->track);
  if (!vv || !game->gpu) return NULL;
  tm_marks *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  m->track = level->track;
  m->vv = vv;
  m->tick = -1;
  tm_picture_table_init(game, &m->pictures, level->track);
  for (uint32_t i = 0; i < vv->emitter_count && m->emitter_count < MAX_EMITTERS; i++) {
    const tmuf_vehicle_emitter *e = &vv->emitters[i];
    const tmuf_particle_model *model = e->models[QUALITY];
    if (e->kind != TMUF_EMITTER_WHEEL || e->wheel == UINT32_MAX || !model) continue;
    uint32_t grass = 0; // the grass types seen: #1 the tufts', #2 the ground's
    for (uint32_t k = 0; k < model->type_count && m->emitter_count < MAX_EMITTERS; k++) {
      const tmuf_particle_type *t = &model->types[k];
      if (t->particle_type != TMUF_PARTICLES_MULTI_STATE || !t->max_particle_count ||
          (t->multi_state_render_mode != TMUF_PARTICLES_QUAD_CENTER_LEFT &&
           t->multi_state_render_mode != TMUF_PARTICLES_GRASS_MARKS))
        continue;
      const tmuf_visual_material *mat = t->material < vv->visuals.material_count ? &vv->visuals.materials[t->material] : NULL;
      const int kind = t->multi_state_render_mode == TMUF_PARTICLES_QUAD_CENTER_LEFT ? RIBBON
                       : grass++ == 0 && !(mat && mat->texture_count >= 2) ? GRASS_FLATTEN
                                                                           : GRASS_GROUND;
      uint32_t r = 0;
      while (r < m->ring_count && m->rings[r].type != t)
        r++;
      if (r == m->ring_count) {
        if (m->ring_count == MAX_RINGS) continue;
        ring *g = &m->rings[m->ring_count];
        g->type = t;
        g->cap = t->max_particle_count;
        g->parts = calloc(g->cap, sizeof *g->parts);
        if (!g->parts) continue;
        m->part_total += g->cap;
        g->kind = kind;
        g->picture = g->tile = UINT32_MAX;
        if (kind == GRASS_GROUND) {
          // MapIntens across the mark, MapBlendXZ the grass over the world
          const tmuf_visual_texture *in = tm_sampler(mat, "Intens"), *xz = tm_sampler(mat, "BlendXZ");
          if (!in && mat && mat->texture_count) in = &mat->textures[0];
          if (!xz && mat && mat->texture_count >= 2) xz = &mat->textures[1];
          if (in) g->picture = tm_map_index(game, &m->pictures, in);
          if (xz) g->tile = tm_map_index(game, &m->pictures, xz);
        } else if (mat && mat->texture_count) {
          g->picture = tm_map_index(game, &m->pictures, &mat->textures[0]);
        }
        m->ring_count++;
      }
      emitter *em = &m->emitters[m->emitter_count++];
      memset(em, 0, sizeof *em);
      em->e = e;
      em->ring = r;
      em->last = -1;
      em->brk = true;
    }
  }
  return m;
}

void tm_marks_destroy(ft_game *game, tm_marks *m) {
  if (!m) return;
  for (uint32_t r = 0; r < m->ring_count; r++)
    free(m->rings[r].parts);
  for (uint32_t i = 0; i < MAX_CHECKPOINTS; i++)
    free(m->checkpoints[i].parts);
  tm_picture_table_free(game, &m->pictures);
  free(m);
}

static void reset(tm_marks *m) {
  for (uint32_t r = 0; r < m->ring_count; r++) {
    m->rings[r].next = m->rings[r].count = 0;
  }
  for (uint32_t i = 0; i < m->emitter_count; i++) {
    m->emitters[i].last = -1;
    m->emitters[i].brk = true;
    m->emitters[i].active = false;
    memset(m->emitters[i].last_pos, 0, sizeof m->emitters[i].last_pos);
  }
  m->tick = -1;
}

static checkpoint *checkpoint_newest(tm_marks *m) {
  return m->checkpoint_count ? &m->checkpoints[(m->checkpoint_top + MAX_CHECKPOINTS - 1u) % MAX_CHECKPOINTS] : NULL;
}

static void checkpoint_save(tm_marks *m) {
  while (m->checkpoint_count && checkpoint_newest(m)->tick >= m->tick)
    m->checkpoint_top = (m->checkpoint_top + MAX_CHECKPOINTS - 1u) % MAX_CHECKPOINTS, m->checkpoint_count--;
  checkpoint *c = &m->checkpoints[m->checkpoint_top];
  if (!c->parts) c->parts = malloc(sizeof *c->parts * (m->part_total ? m->part_total : 1u));
  if (!c->parts) return;
  uint32_t at = 0;
  for (uint32_t r = 0; r < m->ring_count; r++) {
    const ring *g = &m->rings[r];
    memcpy(c->parts + at, g->parts, sizeof *g->parts * g->count);
    at += g->cap;
    c->next[r] = g->next, c->count[r] = g->count;
  }
  memcpy(c->emitters, m->emitters, sizeof c->emitters);
  c->tick = m->tick;
  m->checkpoint_top = (m->checkpoint_top + 1u) % MAX_CHECKPOINTS;
  if (m->checkpoint_count < MAX_CHECKPOINTS) m->checkpoint_count++;
}

// back to the newest checkpoint at or before tick (the ones after dropped),
// else to the start
static void checkpoint_restore(tm_marks *m, int32_t tick) {
  while (m->checkpoint_count && checkpoint_newest(m)->tick > tick)
    m->checkpoint_top = (m->checkpoint_top + MAX_CHECKPOINTS - 1u) % MAX_CHECKPOINTS, m->checkpoint_count--;
  const checkpoint *c = checkpoint_newest(m);
  if (!c) {
    reset(m);
    return;
  }
  uint32_t at = 0;
  for (uint32_t r = 0; r < m->ring_count; r++) {
    ring *g = &m->rings[r];
    memcpy(g->parts, c->parts + at, sizeof *g->parts * c->count[r]);
    at += g->cap;
    g->next = c->next[r], g->count = c->count[r];
  }
  memcpy(m->emitters, c->emitters, sizeof m->emitters);
  m->tick = c->tick;
}

// a wheel as the game's async state blends two ticks: in contact in both,
// sliding in both, on the earlier one's material
typedef struct wheel_state {
  bool contact, sliding;
  uint32_t material; // UINT32_MAX without contact
} wheel_state;

static wheel_state wheel_of(const tmuf_world *a, const tmuf_world *b, uint32_t i) {
  wheel_state w = {false, false, UINT32_MAX};
  if (i >= b->sim.car.wheel_count) return w;
  const tmuf_car_wheel *wa = &a->sim.car.wheels[i], *wb = &b->sim.car.wheels[i];
  w.contact = wa->contact && wb->contact;
  w.sliding = w.contact && wa->slipping && wb->slipping;
  w.material = w.contact ? wa->contact_material : UINT32_MAX;
  return w;
}

// CSceneVehicle::VisualUpdateAsync: whether an emitter emits
static bool active(const tmuf_vehicle_emitter *e, const tmuf_world *a, const tmuf_world *b) {
  const wheel_state w = wheel_of(a, b, e->wheel);
  if (!e->any_material) {
    bool in = false;
    for (uint32_t k = 0; k < e->material_count && !in; k++)
      in = w.material == e->materials[k];
    if (!in) return false;
  }
  const bool burnout = b->sim.car.geared.wheel_speed_override != 0;
  bool all = true;
  for (uint32_t i = 0; i < b->sim.car.wheel_count && all; i++)
    all = wheel_of(a, b, i).sliding;
  return (!e->needs_all_sliding || burnout || all) && (!e->needs_sliding || w.sliding || burnout);
}

// where an emitter is (the world point and the strip's left), with the car
// posed as `pose`: the wheel's lowest point, raised by the emitter's offset,
// its left across the car's horizontal speed
static void place(const tmuf_vehicle_emitter *e, const tmuf_world *w, const tm_pose *pose, float pos[3],
                  float left[3]) {
  const tmuf_car_wheel *wh = &w->sim.car.wheels[e->wheel];
  const float local[3] = {wh->cur_iso.t.x + e->location.t.x, wh->cur_iso.t.y - wh->rolling_radius + e->location.t.y,
                          wh->cur_iso.t.z + e->location.t.z};
  const float(*r)[3] = pose->rotation;
  const float p[3] = {pose->position.x, pose->position.y, pose->position.z};
  for (int k = 0; k < 3; k++)
    pos[k] = r[k][0] * local[0] + r[k][1] * local[1] + r[k][2] * local[2] + p[k];
  // the speed in the car's frame (x side, z forward)
  const tmuf_vec3 v = w->sim.body.state.lin;
  const float(*m)[3] = w->sim.body.state.rot.m;
  const float side = v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0];
  const float forward = v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2];
  float l[3] = {e->location.r.m[0][0], e->location.r.m[1][0], e->location.r.m[2][0]};
  if (e->orient_to_speed && side * side + forward * forward > 0.01f) {
    // SetDOV((side, 0, forward), up): its x, cross(up, dir)
    const float n = sqrtf(side * side + forward * forward);
    l[0] = forward / n, l[1] = 0.f, l[2] = -side / n;
  }
  for (int k = 0; k < 3; k++)
    left[k] = r[k][0] * l[0] + r[k][1] * l[1] + r[k][2] * l[2];
}

static float dist2(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

// CMotionManagerParticles::EmitOnePart: the ring's next slot
static void emit(tm_marks *m, emitter *em, uint32_t ms, const float pos[3], const float left[3]) {
  ring *g = &m->rings[em->ring];
  const tmuf_particle_type *t = g->type;
  const int32_t slot = (int32_t)g->next;
  g->next = (g->next + 1) % g->cap;
  if (g->count < g->cap) g->count++;
  // the old particle's links cut
  for (uint32_t i = 0; i < g->count; i++)
    if (g->parts[i].prev == slot) g->parts[i].prev = -1, g->parts[i].brk = true;
  for (uint32_t i = 0; i < m->emitter_count; i++)
    if (m->emitters[i].ring == em->ring && m->emitters[i].last == slot) m->emitters[i].last = -1;
  mark *p = &g->parts[slot];
  memcpy(p->pos, pos, sizeof p->pos);
  memcpy(p->left, left, sizeof p->left);
  p->half_width = t->size * (t->ratio_xy > 0.f ? t->ratio_xy : 1.f);
  p->birth_ms = ms;
  const double death = (double)ms + (double)t->life * 1000.0;
  p->death_ms = death > 4e9 ? UINT32_MAX : (uint32_t)death;
  p->prev = em->last;
  p->brk = em->brk;
  // EmitColor: the LightFromMap colour of the wheel's quadrant (Stadium;
  // skid_spec §7.3), white without one
  p->color = 0xffffffffu;
  float rgb[3];
  if (tm_car_lfm_tint(m->tint_car, pos, rgb)) {
    uint32_t c = 0xff000000u;
    for (int k = 0; k < 3; k++) {
      const float x = rgb[k] < 0.f ? 0.f : rgb[k] > 1.f ? 1.f : rgb[k];
      c |= (uint32_t)lrintf(x * 255.f) << (16 - 8 * k); // B G R A: r at 16, g at 8, b at 0
    }
    p->color = c;
  }
  p->u = em->last >= 0 ? g->parts[em->last].u + sqrtf(dist2(pos, g->parts[em->last].pos)) * t->u_scale_dist : 0.f;
  em->last = slot;
  memcpy(em->last_pos, pos, sizeof em->last_pos);
  em->brk = false;
}

// one tick of the emitters: b the world at it, a the one before
static void run_tick(tm_marks *m, const ft_world *a, const ft_world *b, int32_t tick) {
  const uint32_t ms = tick > 0 ? (uint32_t)tick * TMUF_TICK_MS : 0u;
  const tm_pose pose = tm_pose_at(a, b, 1.f);
  for (uint32_t i = 0; i < m->emitter_count; i++) {
    emitter *em = &m->emitters[i];
    em->active = em->e->wheel < b->w.sim.car.wheel_count && active(em->e, &a->w, &b->w);
    if (!em->active) {
      em->brk = true;
      continue;
    }
    float pos[3], left[3];
    place(em->e, &b->w, &pose, pos, left);
    const float min = m->rings[em->ring].type->birth_min_dist;
    if (dist2(pos, em->last_pos) > min * min) emit(m, em, ms, pos, left);
  }
}

// the timeline group the shown world is in (the first when none says so)
static uint32_t group_of(const ft_engine_api *api, const ft_render_frame *frame) {
  if (!api->timeline_world_pair || !api->timeline_world_count) return 0;
  for (uint32_t i = 0; i < api->timeline_world_count(); i++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(i, (int32_t)frame->tick, &a, &b) && b == frame->world) return i;
  }
  return 0;
}

void tm_marks_update(ft_game *game, tm_marks *m, const ft_render_frame *frame) {
  if (!m || !frame->world) return;
  const int32_t tick = (int32_t)frame->tick;
  if (tick < m->tick) checkpoint_restore(m, tick);
  if (tick == m->tick) return;
  const ft_engine_api *api = game->engine;
  if (!api->timeline_world_pair) return;
  const uint32_t group = group_of(api, frame);
  m->tint_car = game->tint_car;
  for (int32_t t = m->tick + 1 > 1 ? m->tick + 1 : 1; t <= tick; t++) {
    const ft_world *a = NULL, *b = NULL;
    if (api->timeline_world_pair(group, t, &a, &b) && a && b) run_tick(m, a, b, t);
    m->tick = t;
    if (t % CHECKPOINT_TICKS == 0) checkpoint_save(m);
  }
  m->tick = tick;
}

// the quad between particles a (older) and b, in the strip's texture
static uint32_t quad(tm_track_vertex *out, const mark *a, const float bpos[3], const float bleft_in[3], float bhalf,
                     float bu, uint32_t bcolor) {
  float bleft[3] = {bleft_in[0], bleft_in[1], bleft_in[2]};
  // no bow tie where the direction turns over
  if (a->left[0] * bleft[0] + a->left[1] * bleft[1] + a->left[2] * bleft[2] < 0.f)
    bleft[0] = -bleft[0], bleft[1] = -bleft[1], bleft[2] = -bleft[2];
  tm_track_vertex v[4];
  memset(v, 0, sizeof v);
  for (int k = 0; k < 3; k++) {
    v[0].pos[k] = a->pos[k] + a->half_width * a->left[k];
    v[1].pos[k] = a->pos[k] - a->half_width * a->left[k];
    v[2].pos[k] = bpos[k] + bhalf * bleft[k];
    v[3].pos[k] = bpos[k] - bhalf * bleft[k];
  }
  v[0].uv[0] = 0.f, v[0].uv[1] = a->u;
  v[1].uv[0] = 1.f, v[1].uv[1] = a->u;
  v[2].uv[0] = 0.f, v[2].uv[1] = bu;
  v[3].uv[0] = 1.f, v[3].uv[1] = bu;
  for (int i = 0; i < 4; i++) {
    v[i].color = i < 2 ? a->color : bcolor;
    v[i].normal = 511u << 10; // up
    v[i].prelight = 0xffffffffu;
  }
  static const int tri[6] = {0, 1, 2, 2, 1, 3};
  for (int i = 0; i < 6; i++)
    out[i] = v[tri[i]];
  return 6;
}

// a ring's strips as triangles (each emitter's from its newest particle
// back while they live, with the live link to the wheel for the ribbons);
// the vertex count, *out to free
static uint32_t strips(tm_marks *m, uint32_t r, const ft_render_frame *frame, tm_track_vertex **out) {
  const ring *g = &m->rings[r];
  *out = NULL;
  if (!g->count || g->picture == UINT32_MAX) return 0;
  const uint32_t now = frame->tick > 0 ? (uint32_t)frame->tick * TMUF_TICK_MS : 0u;
  const tm_pose pose = tm_pose_at(frame->previous_world, frame->world, frame->alpha);
  // at most every particle's quad and each emitter's live one
  tm_track_vertex *verts = malloc(sizeof *verts * 6 * (g->count + m->emitter_count));
  if (!verts) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < m->emitter_count; i++) {
    const emitter *em = &m->emitters[i];
    if (em->ring != r || em->last < 0) continue;
    // the live link: from the newest particle to the wheel now
    if (g->kind == RIBBON && em->active && !em->brk && frame->previous_world) {
      float pos[3], left[3];
      place(em->e, &frame->world->w, &pose, pos, left);
      const mark *last = &g->parts[em->last];
      const float u2 = last->u + sqrtf(dist2(pos, last->pos)) * g->type->u_scale_dist;
      n += quad(verts + n, last, pos, left, last->half_width, u2, last->color);
    }
    // back along the strip while its particles live
    int32_t newer = em->last;
    for (int32_t k = g->parts[newer].prev, steps = 0; k >= 0 && steps < (int32_t)g->cap; steps++) {
      const mark *a = &g->parts[k], *b = &g->parts[newer];
      if (now < a->birth_ms || now > a->death_ms) break;
      if (!b->brk) n += quad(verts + n, a, b->pos, b->left, b->half_width, b->u, b->color);
      newer = k;
      k = a->prev;
    }
  }
  *out = verts;
  return n;
}

static void base_uniforms(const ft_render_frame *frame, tm_track_uniforms *u) {
  memset(u, 0, sizeof *u);
  memcpy(u->view_proj, frame->state.camera.view_proj, sizeof u->view_proj);
  u->lod_bias = frame->state.lod_bias;
  u->opacity = 1.f;
  u->color_scale[0] = u->color_scale[1] = 1.f;
}

void tm_marks_render(ft_game *game, tm_marks *m, const ft_render_frame *frame) {
  if (!m || !frame->world || !m->ring_count) return;
  tm_track_uniforms u;
  base_uniforms(frame, &u);
  u.stripe_gen[3] = 1.f;         // blended: the picture's alpha out
  u.alpha_cutoff = 0.5f / 255.f; // (alpha > 0)
  // fixed function: no light, no fog; blended over the ground, no depth written
  const tg_state state = {.blend_src = TG_BLEND_SRCALPHA, .blend_dst = TG_BLEND_INVSRCALPHA, .depth_test = true,
                          .cull = TG_CULL_NONE};
  const tg_sampler samplers[TM_TRACK_TEXTURES] = {{3, 1, 3, 2}, TG_SAMPLER_WRAP};
  for (uint32_t r = 0; r < m->ring_count; r++) {
    const ring *g = &m->rings[r];
    if (g->kind == GRASS_FLATTEN || (g->kind == GRASS_GROUND && (!game->grass_program || g->tile == UINT32_MAX)))
      continue;
    tm_track_vertex *verts;
    const uint32_t n = strips(m, r, frame, &verts);
    if (n) {
      tg_texture textures[TM_TRACK_TEXTURES] = {m->pictures.textures[g->picture]};
      if (g->kind == GRASS_GROUND) {
        textures[1] = m->pictures.textures[g->tile];
        u.blend = 0.f;
        tg_draw_dynamic(game->gpu, game->grass_program, &state, verts, n, textures, samplers, &u);
      } else {
        tg_draw_dynamic(game->gpu, game->track_program, &state, verts, n, textures, samplers, &u);
      }
    }
    free(verts);
  }
}

void tm_marks_flatten(ft_game *game, tm_marks *m, const ft_render_frame *frame) {
  if (!m || !frame->world || !game->grass_program) return;
  tm_track_uniforms u;
  base_uniforms(frame, &u);
  u.blend = 1.f;
  // the frame's alpha (the ground's 1) down to GrassMarkFenceIntens's
  const tg_state state = {.depth_test = true, .cull = TG_CULL_NONE, .blend_min = true, .alpha_only = true};
  const tg_sampler samplers[TM_TRACK_TEXTURES] = {{3, 1, 3, 2}};
  for (uint32_t r = 0; r < m->ring_count; r++) {
    if (m->rings[r].kind != GRASS_FLATTEN) continue;
    tm_track_vertex *verts;
    const uint32_t n = strips(m, r, frame, &verts);
    if (n) {
      const tg_texture textures[TM_TRACK_TEXTURES] = {m->pictures.textures[m->rings[r].picture]};
      tg_draw_dynamic(game->gpu, game->grass_program, &state, verts, n, textures, samplers, &u);
    }
    free(verts);
  }
}
