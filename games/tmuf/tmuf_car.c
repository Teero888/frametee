// The car: the game's own model, moved as the game moves it.
//
// tmuf_physics hands over the vehicle's trees (parts) with their meshes and
// the rig CSceneVehicleStruct lays over them (tmuf_track_vehicle_visuals).
// Every frame each rigged part gets the location the game gives it, from the
// car's state (CSceneVehicleCar::VehicleUpdateAsync, CSceneVehicle::
// VisualUpdateAsync):
//
//   wheels  spin about their axle and steer (SVisualWheel "rolling"), hubs
//           follow the suspension, guards steer and follow it too;
//   body    pitches up to pi/64 while a turbo runs;
//   head    of the pilot tilts with the two feedback springs;
//   arms    are aimed from the hub at the body and stretched to reach it
//           (SVisualArm), cardans also turn with their wheel.
//
// The meshes are uploaded once, in their parts' frames; a frame is one draw
// per mesh with its part's matrix.

#include "tmuf_internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- isometries (GmIso4: world = r * p + t, r[row][col]) --------------------

#define LFM_SIZE 64u // a cell of the game's LightFromMap picture

typedef struct iso {
  float r[3][3];
  float t[3];
} iso;

static iso iso_identity(void) { return (iso){{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}}; }

static iso iso_of(const tmuf_iso4 *l) {
  iso o;
  memcpy(o.r, l->r.m, sizeof o.r);
  o.t[0] = l->t.x, o.t[1] = l->t.y, o.t[2] = l->t.z;
  return o;
}

static void mat_mul(float out[3][3], const float a[3][3], const float b[3][3]) {
  float r[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
  memcpy(out, r, sizeof r);
}

static void point(const iso *a, const float p[3], float out[3]) {
  float r[3];
  for (int i = 0; i < 3; i++)
    r[i] = a->r[i][0] * p[0] + a->r[i][1] * p[1] + a->r[i][2] * p[2] + a->t[i];
  memcpy(out, r, sizeof r);
}

// a after b: (a . b)(p) = a(b(p))
static iso compose(const iso *a, const iso *b) {
  iso o;
  mat_mul(o.r, a->r, b->r);
  point(a, b->t, o.t);
  return o;
}

static void point_inverse(const iso *a, const float p[3], float out[3]) {
  const float d[3] = {p[0] - a->t[0], p[1] - a->t[1], p[2] - a->t[2]};
  for (int i = 0; i < 3; i++)
    out[i] = a->r[0][i] * d[0] + a->r[1][i] * d[1] + a->r[2][i] * d[2];
}

// GmMat3::RotateX / RotateY / RotateZ: m = R(angle) * m
static void rotate_x(float m[3][3], float angle) {
  const float s = sinf(angle), c = cosf(angle);
  const float rx[3][3] = {{1, 0, 0}, {0, c, -s}, {0, s, c}};
  mat_mul(m, rx, m);
}

static void rotate_y(float m[3][3], float angle) {
  const float s = sinf(angle), c = cosf(angle);
  const float ry[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
  mat_mul(m, ry, m);
}

static void rotate_z(float m[3][3], float angle) {
  const float s = sinf(angle), c = cosf(angle);
  const float rz[3][3] = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
  mat_mul(m, rz, m);
}

// --- the model on the GPU ------------------------------------------------------

typedef struct car_draw {
  uint32_t part;
  uint32_t first_index, index_count;
  uint32_t texture; // UINT32_MAX: white
  float alpha_cutoff;
  // the skin shader (CarSkinMultiSpecPC3): its maps, texture indices
  bool skin;
  bool paint; // its picture is the car's paint, which a skin pack replaces
  uint32_t dirt, env, fresnel, clouds, self_illum;
  bool details; // CarDetails: no reflections, lights of their own
  bool glass;   // CarGlass: the frame multiplied by its Translucent, then its reflections added
  // an additive glow coloured by its material's emissive colour (the brake
  // discs, IA4's trace vs_42: GbxMaterialEmissiveRGB1, black when not
  // braking): not drawn (how braking lights them is not decoded)
  bool emissive_glow;
} car_draw;

// SVisualArm's anchors, measured on the rest pose (CSceneVehicle::
// VehicleInitFromSolid): the arm's origin in the frame of `from`, the far end
// of its mesh along its z in the frame of `to`, and the length between them.
typedef struct car_arm {
  bool valid;
  float from_point[3], to_point[3];
  float rest_length;
} car_arm;

struct tm_car_model {
  const tmuf_vehicle_visuals *v;
  const tmuf_vehicle_visual_level *level; // the one drawn, NULL: every part
  tg_mesh *mesh;
  tm_picture_table pictures;
  uint32_t draw_count;
  car_draw *draws;
  bool *shown;      // per part
  car_arm *arms;    // per arm of the level
  iso *local, *world; // per part, scratch for a frame
  tm_light light; // the map's time of day (tmuf_weather.c)
  // the map's lights (tmuf_track_lights) and the car's LightFromMap: the
  // lightmap under it this frame, and where the car's surfaces sample it
  const tmuf_light *lights;
  uint32_t light_count;
  tg_target *lfm;
  bool lfm_drawn;
  float lfm_u[4], lfm_v[4];
  // the cell read back after the frame, 2 x 2 (ObjectGrabColors: the
  // marks' and particles' colours), rows by v
  bool lfm_read;
  float lfm_rgb[2][2][3];
  // skin packs' paint (Diffuse.dds of the zip), loaded as they are worn
  struct {
    char path[256];
    tg_texture texture;
  } skins[16];
  uint32_t skin_count;
};

const char *tm_profile_skin(const ft_player_setup *setups, uint32_t count, int32_t player) {
  if (!setups || player < 0 || (uint32_t)player >= count) return "";
  const ft_player_setup *s = &setups[player];
  if (!s->data || s->data_size < offsetof(tm_profile, name) || memcmp(s->data, TM_PROFILE_MAGIC, 4) != 0) return "";
  const tm_profile *p = s->data;
  return memchr(p->skin, 0, sizeof p->skin) ? p->skin : "";
}

const char *tm_profile_name(const ft_player_setup *setups, uint32_t count, int32_t player) {
  if (!setups || player < 0 || (uint32_t)player >= count) return "";
  const ft_player_setup *s = &setups[player];
  if (!s->data || s->data_size < sizeof(tm_profile) || memcmp(s->data, TM_PROFILE_MAGIC, 4) != 0) return "";
  const tm_profile *p = s->data;
  return memchr(p->name, 0, sizeof p->name) ? p->name : "";
}

uint32_t tm_skin_roots(ft_game *game, char roots[TM_SKIN_ROOTS][1024]) {
  uint32_t n = 0;
  game->engine->resolve_data_path("GameData", roots[n++], sizeof roots[0]);
  game->engine->resolve_data_path("Documents", roots[n++], sizeof roots[0]);
  const char *home = getenv("HOME"), *user = getenv("USER");
  if (home && home[0]) {
    snprintf(roots[n++], sizeof roots[0], "%s/Documents/TrackMania", home);
    if (user && user[0]) snprintf(roots[n++], sizeof roots[0], "%s/.wine/drive_c/users/%s/Documents/TrackMania", home, user);
  }
  return n;
}

// The paint of skin pack `path` (as the game stores it: tm_skin_roots), 0
// when it cannot be had.
static tg_texture skin_paint(ft_game *game, tm_car_model *car, const char *path) {
  if (!path[0]) return 0;
  for (uint32_t i = 0; i < car->skin_count; i++)
    if (strcmp(car->skins[i].path, path) == 0) return car->skins[i].texture;
  if (car->skin_count == sizeof car->skins / sizeof car->skins[0]) return 0;
  char relative[300], file[1400], roots[TM_SKIN_ROOTS][1024];
  snprintf(relative, sizeof relative, "%s", path);
  for (char *c = relative; *c; c++)
    if (*c == '\\') *c = '/';
  tg_texture texture = 0;
  void *zip = NULL;
  size_t zip_size = 0;
  bool found = false;
  const uint32_t root_count = tm_skin_roots(game, roots);
  for (uint32_t r = 0; r < root_count && !found; r++) {
    snprintf(file, sizeof file, "%s/%s", roots[r], relative);
    found = game->engine->read_file(file, &zip, &zip_size);
  }
  if (found) {
    size_t size = 0;
    uint8_t *dds = tmuf_zip_extract(zip, zip_size, "Diffuse.dds", &size);
    tm_image image;
    if (dds && tm_image_load(dds, size, tg_supports_bc(game->gpu), &image)) {
      texture = tg_texture_create(game->gpu, &image);
      tm_image_free(&image);
    }
    tmuf_free(dds);
    game->engine->free_file_data(zip);
  }
  if (!texture && found) tm_log(game, FT_LOG_WARN, "Cannot read the skin %s", path);
  if (!found) tm_log(game, FT_LOG_WARN, "The skin %s is not on this computer: the default one is shown", path);
  snprintf(car->skins[car->skin_count].path, sizeof car->skins[0].path, "%s", path);
  car->skins[car->skin_count++].texture = texture;
  return texture;
}

// The part's group of detail: the root of a level it hangs below, if any.
static uint32_t level_root_of(const tmuf_vehicle_visuals *v, uint32_t part) {
  for (uint32_t p = part; p != TMUF_VEHICLE_NO_PART; p = v->parts[p].parent)
    for (uint32_t k = 0; k < v->level_count; k++)
      if (v->levels[k].root == p) return p;
  return TMUF_VEHICLE_NO_PART;
}

static void measure_arms(tm_car_model *car) {
  const tmuf_vehicle_visuals *v = car->v;
  const tmuf_vehicle_visual_level *l = car->level;
  for (uint32_t i = 0; i < l->arm_count; i++) {
    const tmuf_vehicle_visual_arm *a = &l->arms[i];
    car_arm *out = &car->arms[i];
    out->valid = false;
    if (a->arm == TMUF_VEHICLE_NO_PART || a->from == TMUF_VEHICLE_NO_PART || a->to == TMUF_VEHICLE_NO_PART) continue;
    // the arm's own mesh gives its length
    const tmuf_visual_mesh *mesh = NULL;
    for (uint32_t k = 0; k < v->visuals.instance_count && !mesh; k++)
      if (v->visuals.instances[k].block == a->arm) mesh = &v->visuals.meshes[v->visuals.instances[k].mesh];
    if (!mesh) continue;
    const iso arm = iso_of(&v->parts[a->arm].location);
    const iso from = iso_of(&v->parts[a->from].location), to = iso_of(&v->parts[a->to].location);
    const float end_local[3] = {0.f, 0.f, mesh->bounds[2] + mesh->bounds[5]};
    float end[3];
    point(&arm, end_local, end);
    const float d[3] = {end[0] - arm.t[0], end[1] - arm.t[1], end[2] - arm.t[2]};
    out->rest_length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    point_inverse(&to, end, out->to_point);
    point_inverse(&from, arm.t, out->from_point);
    out->valid = out->rest_length > 0.f;
  }
}

tm_car_model *tm_car_create(ft_game *game, ft_level *level) {
  const tmuf_vehicle_visuals *v = tmuf_track_vehicle_visuals(level->track);
  if (!v || !game->gpu || !game->track_program || !v->visuals.instance_count) return NULL;
  tm_car_model *car = calloc(1, sizeof *car);
  if (!car) return NULL;
  tg_upload_begin(game->gpu);
  car->v = v;
  tm_picture_table_init(game, &car->pictures, level->track);
  car->light = level->light;
  car->light_count = tmuf_track_lights(level->track, &car->lights);
  if (v->lighting.has_light_from_map) car->lfm = tg_target_create(game->gpu, LFM_SIZE, LFM_SIZE);
  // the player's car shows its most detailed level
  for (uint32_t k = 0; k < v->level_count; k++)
    if (!car->level || v->levels[k].quality > car->level->quality) car->level = &v->levels[k];
  const uint32_t parts = v->part_count ? v->part_count : 1;
  car->shown = malloc(sizeof *car->shown * parts);
  car->local = malloc(sizeof *car->local * parts);
  car->world = malloc(sizeof *car->world * parts);
  car->arms = calloc(car->level && car->level->arm_count ? car->level->arm_count : 1, sizeof *car->arms);
  car->draws = malloc(sizeof *car->draws * v->visuals.instance_count);
  uint64_t vertex_total = 0, index_total = 0;
  for (uint32_t i = 0; i < v->visuals.instance_count; i++) {
    const tmuf_visual_mesh *m = &v->visuals.meshes[v->visuals.instances[i].mesh];
    vertex_total += m->vertex_count;
    index_total += m->index_count;
  }
  tm_track_vertex *vertices = malloc(sizeof *vertices * (vertex_total ? vertex_total : 1));
  uint32_t *indices = malloc(sizeof *indices * (index_total ? index_total : 1));
  if (!car->shown || !car->local || !car->world || !car->arms || !car->draws || !vertices || !indices) {
    free(vertices), free(indices);
    tg_upload_end(game->gpu);
    tm_car_destroy(game, car);
    return NULL;
  }
  for (uint32_t p = 0; p < v->part_count; p++) {
    const uint32_t root = level_root_of(v, p);
    car->shown[p] = !car->level || root == TMUF_VEHICLE_NO_PART || root == car->level->root;
  }
  uint32_t vcount = 0, icount = 0;
  for (uint32_t i = 0; i < v->visuals.instance_count; i++) {
    const tmuf_visual_instance *in = &v->visuals.instances[i];
    const tmuf_visual_mesh *m = &v->visuals.meshes[in->mesh];
    if (!m->index_count || in->block >= v->part_count || !car->shown[in->block]) continue;
    const tmuf_visual_material *mat = in->material != UINT32_MAX ? &v->visuals.materials[in->material] : NULL;
    car_draw *d = &car->draws[car->draw_count++];
    d->part = in->block;
    d->first_index = icount;
    d->index_count = m->index_count;
    d->texture = tm_picture_index(game, &car->pictures, mat);
    d->alpha_cutoff = tm_alpha_cutoff(mat);
    d->details = mat && tm_sampler(mat, "Diffuse_Gloss") && !tm_sampler(mat, "EnvCubic") && tm_sampler(mat, "SelfIllum");
    d->skin = mat && tm_sampler(mat, "Diffuse_Gloss") && (tm_sampler(mat, "EnvCubic") || d->details);
    d->glass = mat && tm_sampler(mat, "Translucent") && tm_sampler(mat, "EnvCubic");
    if (d->glass) d->skin = true;
    d->emissive_glow = mat && !d->skin && mat->has_render_state && mat->alpha_blend &&
                       mat->blend_src == TMUF_BLEND_ONE && mat->blend_dst == TMUF_BLEND_ONE;
    const tmuf_visual_texture *picture_texture = tm_picture_texture(mat);
    const char *picture_file = picture_texture ? (picture_texture->file ? picture_texture->file : picture_texture->pack_file) : "";
    d->paint = d->skin && picture_file && strstr(picture_file, "Skin.");
    if (d->skin) {
      d->dirt = tm_map_index(game, &car->pictures, tm_sampler(mat, "DirtMarks"));
      d->env = tm_map_index(game, &car->pictures, tm_sampler(mat, "EnvCubic"));
      d->fresnel = tm_map_index(game, &car->pictures, tm_sampler(mat, "Fresnel"));
      d->clouds = tm_map_index(game, &car->pictures, tm_sampler(mat, "Clouds"));
      d->self_illum = tm_map_index(game, &car->pictures, tm_sampler(mat, "SelfIllum"));
      if (d->glass) d->texture = tm_map_index(game, &car->pictures, tm_sampler(mat, "Translucent"));
    }
    const iso at = iso_of(&in->location);
    const tmuf_visual_texture *picture = tm_picture_texture(mat), *occlusion = tm_occlusion_texture(mat);
    for (uint32_t k = 0; k < m->vertex_count; k++) {
      float pos[3];
      memcpy(pos, m->vertices + (size_t)k * m->vertex_stride, sizeof pos);
      tm_track_vertex *o = &vertices[vcount + k];
      point(&at, pos, o->pos);
      tm_texcoord(picture, m, k, o->pos, o->uv);
      o->color = tm_vertex_color(m, k);
      tm_texcoord(occlusion, m, k, o->pos, o->uv_occlusion);
      o->normal = tm_vertex_normal(m, k);
    }
    for (uint32_t k = 0; k < m->index_count; k++)
      indices[icount + k] = vcount + m->indices[k];
    vcount += m->vertex_count;
    icount += m->index_count;
  }
  car->mesh = tg_mesh_create(game->gpu, vertices, (size_t)vcount * sizeof(tm_track_vertex), indices, icount);
  free(vertices);
  free(indices);
  tg_upload_end(game->gpu);
  if (!car->mesh) {
    tm_car_destroy(game, car);
    return NULL;
  }
  if (car->level) measure_arms(car);
  tm_log(game, FT_LOG_INFO, "Car %s: %u triangles in %u draws", tmuf_track_vehicle(level->track), icount / 3,
         car->draw_count);
  return car;
}

void tm_car_destroy(ft_game *game, tm_car_model *car) {
  if (!car) return;
  tg_target_destroy(game->gpu, car->lfm);
  if (car->mesh) tg_mesh_destroy(game->gpu, car->mesh);
  tm_picture_table_free(game, &car->pictures);
  for (uint32_t i = 0; i < car->skin_count; i++)
    if (car->skins[i].texture) tg_texture_destroy(game->gpu, car->skins[i].texture);
  free(car->draws);
  free(car->shown);
  free(car->arms);
  free(car->local);
  free(car->world);
  free(car);
}

// --- the car's state between two ticks -------------------------------------------

tm_pose tm_pose_at(const ft_world *previous, const ft_world *world, float alpha) {
  const tmuf_dyna_state *b = &world->w.sim.body.state;
  const tmuf_dyna_state *a = previous ? &previous->w.sim.body.state : b;
  if (alpha < 0.f) alpha = 0.f;
  if (alpha > 1.f) alpha = 1.f;
  tm_pose pose;
  pose.position = (ft_vec3){a->pos.x + (b->pos.x - a->pos.x) * alpha, a->pos.y + (b->pos.y - a->pos.y) * alpha,
                            a->pos.z + (b->pos.z - a->pos.z) * alpha};
  pose.velocity = (ft_vec3){b->lin.x, b->lin.y, b->lin.z};
  // the two orientations slerped, the short way round, as the game
  // interpolates the car's location (GetInterpolatedLocation; a normalised
  // lerp when they are all but equal)
  float qa[4] = {a->quat.w, a->quat.x, a->quat.y, a->quat.z}, qb[4] = {b->quat.w, b->quat.x, b->quat.y, b->quat.z};
  float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
  if (dot < 0.f) {
    dot = -dot;
    for (int k = 0; k < 4; k++) qb[k] = -qb[k];
  }
  float wa = 1.f - alpha, wb = alpha;
  if (dot < 0.9999f) {
    const float ang = acosf(dot), s = sinf(ang);
    wa = sinf((1.f - alpha) * ang) / s;
    wb = sinf(alpha * ang) / s;
  }
  float q[4], n = 0.f;
  for (int k = 0; k < 4; k++) {
    q[k] = qa[k] * wa + qb[k] * wb;
    n += q[k] * q[k];
  }
  n = n > 0.f ? 1.f / sqrtf(n) : 1.f;
  const float w = q[0] * n, x = q[1] * n, y = q[2] * n, z = q[3] * n;
  // as the game builds its rotation from the quaternion (GmMat3::Set)
  pose.rotation[0][0] = 1.f - 2.f * (y * y + z * z);
  pose.rotation[1][0] = 2.f * (x * y + z * w);
  pose.rotation[2][0] = 2.f * (x * z - y * w);
  pose.rotation[0][1] = 2.f * (x * y - z * w);
  pose.rotation[1][1] = 1.f - 2.f * (x * x + z * z);
  pose.rotation[2][1] = 2.f * (y * z + x * w);
  pose.rotation[0][2] = 2.f * (x * z + y * w);
  pose.rotation[1][2] = 2.f * (y * z - x * w);
  pose.rotation[2][2] = 1.f - 2.f * (x * x + y * y);
  return pose;
}

#define TWO_PI 6.28318530717958647692f

// SSimulationWheel::SState::SetBlend: the spin angle blended the short way
// round its period (OrderWindowedValues), the steering linearly
static float blend_angle(float a, float b, float alpha) {
  if (b - a > TWO_PI * 0.5f) a += TWO_PI;
  else if (a - b > TWO_PI * 0.5f) b += TWO_PI;
  return a + (b - a) * alpha;
}

static float lerp(float a, float b, float alpha) { return a * (1.f - alpha) + b * alpha; }

// The body's pitch while a turbo runs, over its progress t in 0..1: up in
// the first 0.3, held, down from 0.7 (CSceneVehicleCar::VehicleUpdateAsync).
static float turbo_pitch(float t) {
  float a;
  if (t < 0.3f) a = sinf(t * 1.5707963705062866f / 0.30000001192092896f);
  else if (t < 0.7f) a = 1.f;
  else if (t < 1.f) a = 1.f - sinf((t - 0.699999988079071f) * 1.5707963705062866f / 0.30000001192092896f);
  else a = 0.f;
  return a * 0.04908738657832146f;
}

// the game shakes the pitch by up to 0.005 each frame; a hash of the time
// keeps a picture reproducible
static float turbo_shake(uint32_t tick, float alpha) {
  uint32_t h = tick * 2654435761u ^ (uint32_t)(alpha * 1024.f) * 40503u;
  h ^= h >> 15, h *= 2246822519u, h ^= h >> 13;
  return ((float)(h & 0xffffu) / 65535.f * 2.f - 1.f) * 0.004999999888241291f;
}

static void animate(tm_car_model *car, const ft_world *previous, const ft_world *world, float alpha) {
  const tmuf_vehicle_visuals *v = car->v;
  const tmuf_car *c = &world->w.sim.car;
  const tmuf_car *pc = previous ? &previous->w.sim.car : c;
  for (uint32_t p = 0; p < v->part_count; p++)
    car->local[p] = iso_of(&v->parts[p].location);
  const tmuf_vehicle_visual_level *l = car->level;
  if (!l) return;

  // the body pitches with a turbo
  if (l->body != TMUF_VEHICLE_NO_PART && c->turbo.type != 0) {
    float x = turbo_pitch(lerp(pc->turbo.type != 0 ? pc->turbo.progress : c->turbo.progress, c->turbo.progress,
                               alpha));
    x += turbo_shake(world->w.tick, alpha);
    iso tilt = iso_identity();
    rotate_x(tilt.r, -x);
    const iso base = car->local[l->body];
    car->local[l->body] = compose(&base, &tilt);
  }

  // wheels
  float spin[TMUF_CAR_MAX_WHEELS] = {0}, steer[TMUF_CAR_MAX_WHEELS][3][3];
  for (uint32_t i = 0; i < c->wheel_count && i < TMUF_CAR_MAX_WHEELS; i++) {
    const tmuf_car_wheel *w = &c->wheels[i], *pw = &pc->wheels[i];
    spin[i] = blend_angle(pw->spin_angle, w->spin_angle, alpha);
    // ComputeAsyncState: the wheel's current rotation, turned by its steering
    memcpy(steer[i], w->cur_iso.r.m, sizeof steer[i]);
    rotate_y(steer[i], lerp(pw->steer_angle, w->steer_angle, alpha));
  }
  for (uint32_t i = 0; i < l->wheel_count; i++) {
    const tmuf_vehicle_visual_wheel *vw = &l->wheels[i];
    if (vw->wheel >= c->wheel_count || vw->wheel >= TMUF_CAR_MAX_WHEELS) continue;
    const tmuf_car_wheel *w = &c->wheels[vw->wheel], *pw = &pc->wheels[vw->wheel];
    // (ComputeAsyncState: the damper between the two ticks)
    const float bounce = lerp(pw->cur_iso.t.y - pw->rest_iso.t.y, w->cur_iso.t.y - w->rest_iso.t.y, alpha);
    if (vw->rolling != TMUF_VEHICLE_NO_PART) {
      iso m = iso_identity();
      rotate_x(m.r, spin[vw->wheel]);
      if (vw->steers) mat_mul(m.r, steer[vw->wheel], m.r);
      const iso base = car->local[vw->rolling];
      car->local[vw->rolling] = compose(&base, &m);
      car->local[vw->rolling].t[1] += bounce;
    }
    if (vw->bouncing != TMUF_VEHICLE_NO_PART) car->local[vw->bouncing].t[1] += bounce;
    if (vw->steering != TMUF_VEHICLE_NO_PART) {
      iso m = iso_identity();
      if (vw->steers) memcpy(m.r, steer[vw->wheel], sizeof m.r);
      const iso base = car->local[vw->steering];
      car->local[vw->steering] = compose(&base, &m);
      car->local[vw->steering].t[1] += bounce;
    }
  }

  // the pilot's head: rolled by the side spring, pitched by the forward one
  if (l->pilot_head != TMUF_VEHICLE_NO_PART) {
    iso *h = &car->local[l->pilot_head];
    rotate_z(h->r, lerp(pc->feedback.side.value, c->feedback.side.value, alpha));
    rotate_x(h->r, lerp(pc->feedback.forward.value, c->feedback.forward.value, alpha));
  }

  // arms, from where the hubs and the body now are
  for (uint32_t i = 0; i < l->arm_count; i++) {
    const tmuf_vehicle_visual_arm *a = &l->arms[i];
    const car_arm *m = &car->arms[i];
    if (!m->valid) continue;
    float from[3], to[3];
    point(&car->local[a->from], m->from_point, from);
    point(&car->local[a->to], m->to_point, to);
    float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < 9.999999747378752e-06f) continue;
    for (int k = 0; k < 3; k++)
      d[k] /= length;
    // GmMat3::SetDOVandUpV(d, up): x = up x d, y = d x x, z = d
    float x[3] = {d[2], 0.f, -d[0]};
    const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (xl > 0.f) x[0] /= xl, x[2] /= xl;
    const float y[3] = {d[1] * x[2] - d[2] * x[1], d[2] * x[0] - d[0] * x[2], d[0] * x[1] - d[1] * x[0]};
    iso o;
    for (int r = 0; r < 3; r++) {
      o.r[r][0] = x[r];
      o.r[r][1] = y[r];
      o.r[r][2] = d[r] * (length / m->rest_length); // stretched to reach
    }
    memcpy(o.t, from, sizeof o.t);
    if (a->rolls && a->wheel < c->wheel_count && a->wheel < TMUF_CAR_MAX_WHEELS) {
      float turn[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      rotate_z(turn, spin[a->wheel]);
      mat_mul(turn, steer[a->wheel], turn);
      mat_mul(o.r, o.r, turn);
    }
    car->local[a->arm] = o;
  }
}

// the car's box in the world: centre, half extents (axis-aligned)
static void world_box(const tm_car_model *car, const iso *root, float c[3], float h[3]) {
  const float *box = car->v->lighting.box;
  for (int i = 0; i < 3; i++) {
    c[i] = root->t[i];
    h[i] = 0.f;
    for (int k = 0; k < 3; k++) {
      c[i] += root->r[i][k] * box[k];
      h[i] += fabsf(root->r[i][k]) * box[3 + k];
    }
  }
}

// the parts where they are this frame (car->world); the car's own frame
static iso pose_parts(tm_car_model *car, const ft_render_frame *frame) {
  const tmuf_vehicle_visuals *v = car->v;
  animate(car, frame->previous_world, frame->world, frame->alpha);
  const tm_pose pose = tm_pose_at(frame->previous_world, frame->world, frame->alpha);
  iso root;
  memcpy(root.r, pose.rotation, sizeof root.r);
  root.t[0] = pose.position.x, root.t[1] = pose.position.y, root.t[2] = pose.position.z;
  // parents come before their children
  for (uint32_t p = 0; p < v->part_count; p++) {
    const uint32_t parent = v->parts[p].parent;
    const iso *up = parent < p ? &car->world[parent] : &root;
    car->world[p] = compose(up, &car->local[p]);
  }
  return root;
}

// every part of the level drawn (the glass too) casts the shadow
void tm_car_shadow(ft_game *game, tm_car_model *car, const ft_render_frame *frame) {
  if (!car->mesh || !frame->world) return;
  const iso root = pose_parts(car, frame);
  // in the Night state the game's car shadow comes from the lamps alone, in
  // perspective (IA4's trace), not from the moon; at Sunset the sun casts it
  // too (Island's Sunset)
  const tmuf_weather *w = game->level ? tmuf_track_weather(game->level->track) : NULL;
  if (!(w && w->start.state == TMUF_DAY_NIGHT) && tm_shadow_begin(game, &car->light, root.r, root.t)) {
    for (uint32_t i = 0; i < car->draw_count; i++) {
      const car_draw *d = &car->draws[i];
      const iso *m = &car->world[d->part];
      tm_shadow_cast(game, car->mesh, d->first_index, d->index_count, m->r, m->t);
    }
    tm_shadow_end(game);
  }
  // and the lamps' (ShadowCountCarHuman: the sun's and up to three more,
  // Island's Sunset trace), as Shadow_CreateVolumes picks them: the lights
  // with flag bit 2 (TMUF_LIGHT_FLAG_RADIUS2) whose radius [2] box meets the
  // car's, nearest for that radius (d / radius[2], d from the car box's
  // centre), a spot only when the centre is inside its second outer cone
  // (+0xa4; Bay's Sunset: 930, 86 and 100 over nearer spots aimed away;
  // IA4's Night: 509 alone of its three). Island, Coast and Bay only,
  // the collection's shadow_90 0 (Desert's Sunset trace has the sun's alone,
  // A01's too)
  const tmuf_scenery_light *scenery = game->level ? tmuf_track_scenery_light(game->level->track) : NULL;
  const uint32_t count = w && scenery && scenery->vertex_lighting == 1 && scenery->shadow_90 == 0
                             ? w->mood.shadow_count_car_human
                             : 0;
  const uint32_t lamps = count > 1 ? (count - 1 < TM_SHADOW_LAMPS ? count - 1 : TM_SHADOW_LAMPS) : 0;
  struct {
    uint32_t light;
    float rank;
  } best[TM_SHADOW_LAMPS];
  uint32_t n = 0;
  float c[3], h[3];
  world_box(car, &root, c, h);
  for (uint32_t i = 0; i < car->light_count && lamps; i++) {
    const tmuf_light *l = &car->lights[i];
    if ((l->kind != TMUF_LIGHT_BALL && l->kind != TMUF_LIGHT_SPOT) || !(l->flags & TMUF_LIGHT_FLAG_RADIUS2) ||
        (l->night_only && !(w && w->is_night)) || l->radius[2] <= 0.f)
      continue;
    const float dx = c[0] - l->position[0], dy = c[1] - l->position[1], dz = c[2] - l->position[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (fabsf(dx) > l->radius[2] + h[0] || fabsf(dy) > l->radius[2] + h[1] || fabsf(dz) > l->radius[2] + h[2])
      continue;
    if (l->kind == TMUF_LIGHT_SPOT && d > 0.f &&
        (dx * l->direction[0] + dy * l->direction[1] + dz * l->direction[2]) / d < l->cos_outer2)
      continue;
    const float rank = d / l->radius[2];
    uint32_t at = n < lamps ? n++ : lamps;
    while (at > 0 && best[at - 1].rank > rank) {
      if (at < lamps) best[at] = best[at - 1];
      at--;
    }
    if (at < lamps) best[at].light = i, best[at].rank = rank;
  }
  for (uint32_t k = 0; k < n; k++) {
    const tmuf_light *l = &car->lights[best[k].light];
    const float f = 1.f - best[k].rank * best[k].rank;
    const float grey = 1.f - car->light.shadow_car_intensity * (f > 0.f ? f : 0.f);
    if (!tm_shadow_lamp_begin(game, (int)k, l->position, grey, root.r, root.t)) continue;
    for (uint32_t i = 0; i < car->draw_count; i++) {
      const car_draw *d = &car->draws[i];
      const iso *m = &car->world[d->part];
      tm_shadow_lamp_cast(game, (int)k, car->mesh, d->first_index, d->index_count, m->r, m->t);
    }
    tm_shadow_lamp_end(game, (int)k);
  }
}

// the car into one of the sun's shadow maps (the cascades: the game's car
// darkens the light mask behind it at a low sun, A06), its parts where
// tm_car_shadow placed them this frame
void tm_car_pssm_cast(ft_game *game, tm_car_model *car, int map) {
  if (!car->mesh || !car->draw_count || !tm_pssm_reaches(game, map, car->world[0].t, 4.f)) return;
  for (uint32_t i = 0; i < car->draw_count; i++) {
    const car_draw *d = &car->draws[i];
    const iso *m = &car->world[d->part];
    tm_pssm_cast_part(game, map, car->mesh, d->first_index, d->index_count, m->r, m->t);
  }
}

// LightFromMap (CPlugBitmapRenderLightFromMap, ComputeCamera_DovObjectY):
// the static scenery under the car in the map's lightmap, orthographically
// from the car's origin down its -Y over its box's footprint, the surface
// nearest the car's top winning, white where there is none and fading to
// white from halfway down its depth
void tm_car_light_from_map(ft_game *game, tm_car_model *car, const tm_scene *scene, const ft_render_frame *frame) {
  car->lfm_drawn = false;
  const tmuf_vehicle_lighting *lg = &car->v->lighting;
  if (!car->lfm || !car->mesh || !frame->world || !lg->has_light_from_map) return;
  const iso root = pose_parts(car, frame);
  const float *box = lg->box; // centre, half extents in the car's frame
  float axis[3][3]; // the car's x, y, z in the world (the rotation's columns)
  for (int a = 0; a < 3; a++)
    for (int k = 0; k < 3; k++)
      axis[a][k] = root.r[k][a];
  const float *o = root.t;
  const float sx = 1.f / (lg->lfm_footprint * box[3]), sz = 1.f / (lg->lfm_footprint * box[5]);
  const float dmin = -box[1] - box[4] + 2.f * box[4] * lg->lfm_top, dmax = -box[1] + box[4] + lg->lfm_depth;
  const float xo = axis[0][0] * o[0] + axis[0][1] * o[1] + axis[0][2] * o[2];
  const float yo = axis[1][0] * o[0] + axis[1][1] * o[1] + axis[1][2] * o[2];
  const float zo = axis[2][0] * o[0] + axis[2][1] * o[1] + axis[2][2] * o[2];
  // uniforms: to_clip (column-major), depth_row, fade
  float u[24] = {0};
  const float kz = 1.f / (dmax - dmin);
  for (int k = 0; k < 3; k++) {
    u[k * 4 + 0] = sx * axis[0][k];
    u[k * 4 + 1] = sz * axis[2][k];
    u[k * 4 + 2] = kz * axis[1][k]; // z = 1 - (d - dmin) / (dmax - dmin), d = -(y - yo)
  }
  u[12] = -sx * (xo + box[0]);
  u[13] = -sz * (zo + box[2]);
  u[14] = 1.f + kz * (dmin - yo);
  u[15] = 1.f;
  for (int k = 0; k < 3; k++)
    u[16 + k] = -axis[1][k];
  u[19] = yo;
  u[20] = box[4] - box[1] + lg->lfm_white * lg->lfm_depth;
  u[21] = (1.f - lg->lfm_white) * lg->lfm_depth;
  // where the car samples it: the clip's x, y to 0..1
  for (int k = 0; k < 3; k++) {
    car->lfm_u[k] = 0.5f * u[k * 4 + 0];
    car->lfm_v[k] = 0.5f * u[k * 4 + 1];
  }
  car->lfm_u[3] = 0.5f * u[12] + 0.5f;
  car->lfm_v[3] = 0.5f * u[13] + 0.5f;
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  const float reach = sqrtf(box[3] * box[3] + box[5] * box[5]) * lg->lfm_footprint + box[4] + lg->lfm_depth;
  tg_target_begin(game->gpu, car->lfm, white);
  car->lfm_drawn = tm_scene_light_from_map(game, scene, o, reach, u);
  tg_target_end(game->gpu);
}

// The lights the car's shaders get (CHmsZoneVPacker::AddInteractLights):
// Light8 for its vertices (DIFFUSE lights whose sphere meets the car's box;
// spots only when their cone reaches the box's sphere), HemiSpec's lights
// for its reflections (the sun, and SPECULAR lights within their specular
// radius, weighed by distance and cone)
static void car_lights(const tm_car_model *car, const iso *root, const ft_camera *cam, tm_car_uniforms *cu) {
  float c[3], h[3];
  world_box(car, root, c, h);
  const float *bh = car->v->lighting.box + 3;
  const float sphere = sqrtf(bh[0] * bh[0] + bh[1] * bh[1] + bh[2] * bh[2]);
  uint32_t balls = 0, spots = 0, hemi = 0;
  // the sun first
  const tm_light *l = &car->light;
  for (int k = 0; k < 3; k++) {
    cu->hemi_dir[0][k] = -l->sun_dir[k];
    const float v = l->sun_rgb[k] * l->spec_intensity;
    cu->hemi_rgb[0][k] = v > 1.f ? 1.f : v;
  }
  cu->hemi_dir[0][3] = 1.f;
  hemi = 1;
  for (uint32_t i = 0; i < car->light_count; i++) {
    const tmuf_light *L = &car->lights[i];
    const bool spot = L->kind == TMUF_LIGHT_SPOT, ball = L->kind == TMUF_LIGHT_BALL || L->kind == TMUF_LIGHT_POINT;
    if (!spot && !ball) continue;
    const float d[3] = {L->position[0] - c[0], L->position[1] - c[1], L->position[2] - c[2]};
    const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], dl = sqrtf(d2);
    if ((L->flags & TMUF_LIGHT_FLAG_DIFFUSE) && L->radius[0] > 0.f) {
      bool meets = true;
      for (int k = 0; k < 3 && meets; k++)
        meets = fabsf(d[k]) <= L->radius[0] + h[k];
      if (meets && spot && dl > sphere) {
        // the angle from the axis to the car's centre, less the sphere's
        const float cosang = -(d[0] * L->direction[0] + d[1] * L->direction[1] + d[2] * L->direction[2]) / dl;
        const float ang = acosf(cosang > 1.f ? 1.f : cosang < -1.f ? -1.f : cosang) - asinf(sphere / dl);
        meets = ang <= L->angle_outer * 0.5f * 3.14159265f / 180.f;
      }
      const float irad2 = 1.f / (L->radius[0] * L->radius[0]);
      if (meets && ball && balls < 8) {
        memcpy(cu->balls_rgb[balls], L->diffuse_rgb, 3 * sizeof(float));
        cu->balls_rgb[balls][3] = irad2;
        memcpy(cu->balls_pos[balls], L->position, 3 * sizeof(float));
        cu->balls_pos[balls][3] = L->radius[0] * L->radius[0];
        balls++;
      } else if (meets && spot && spots < 8) {
        memcpy(cu->spots_rgb[spots], L->diffuse_rgb, 3 * sizeof(float));
        cu->spots_rgb[spots][3] = irad2;
        memcpy(cu->spots_pos[spots], L->position, 3 * sizeof(float));
        const float span = L->cos_inner - L->cos_outer;
        cu->spots_pos[spots][3] = span > 1e-6f ? 1.f / span : 1e6f;
        memcpy(cu->spots_dir[spots], L->direction, 3 * sizeof(float));
        cu->spots_dir[spots][3] = L->cos_outer;
        spots++;
      }
    }
    if ((L->flags & TMUF_LIGHT_FLAG_SPECULAR) && hemi < 8 && L->radius[1] > 0.f) {
      const float r2 = L->radius[1] * L->radius[1];
      if (d2 < 0.001f || d2 > r2) continue;
      float k = 1.f - d2 / r2;
      if (spot) {
        const float cosv = -(d[0] * L->direction[0] + d[1] * L->direction[1] + d[2] * L->direction[2]) / dl;
        if (cosv < L->cos_outer) continue;
        float f = 1.f;
        if (cosv < L->cos_inner && L->cos_inner > L->cos_outer)
          f = powf((cosv - L->cos_outer) / (L->cos_inner - L->cos_outer), L->falloff);
        if (f < 0.001f) continue;
        k *= f;
      }
      for (int j = 0; j < 3; j++) {
        cu->hemi_dir[hemi][j] = d[j] / dl;
        const float v = L->specular_rgb[j] * k;
        cu->hemi_rgb[hemi][j] = v > 1.f ? 1.f : v;
      }
      cu->hemi_dir[hemi][3] = k;
      hemi++;
    }
  }
  cu->counts[0] = (float)balls, cu->counts[1] = (float)spots, cu->counts[2] = (float)hemi;
  const float e[3] = {c[0] - cam->eye.x, c[1] - cam->eye.y, c[2] - cam->eye.z};
  const float el = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  for (int k = 0; k < 3; k++)
    cu->dov[k] = el > 0.f ? e[k] / el : 0.f;
}

uint32_t tm_car_lights(const tm_car_model *car, tm_car_light *out, uint32_t cap) {
  const tmuf_vehicle_visuals *v = car->v;
  uint32_t n = 0;
  for (uint32_t i = 0; i < v->light_count && n < cap; i++) {
    const tmuf_light *l = &v->lights[i];
    if (l->block >= v->part_count || !car->shown[l->block]) continue;
    const iso *m = &car->world[l->block];
    tm_car_light *o = &out[n++];
    o->light = l;
    point(m, l->position, o->position);
    for (int k = 0; k < 3; k++)
      o->direction[k] = m->r[k][0] * l->direction[0] + m->r[k][1] * l->direction[1] + m->r[k][2] * l->direction[2];
  }
  return n;
}

// the shown part's projector: the headlight (night only) or the fake
// shadow (ProjShad, not night-only)
static bool car_projector(ft_game *game, tm_car_model *car, bool night, tm_projector *out) {
  const tmuf_vehicle_visuals *v = car->v;
  for (uint32_t i = 0; i < v->light_count; i++) {
    const tmuf_light *l = &v->lights[i];
    // (the lists hold night-only lights at night only)
    if (l->kind != TMUF_LIGHT_FRUSTUM || !l->night_only != !night || l->block >= v->part_count ||
        !car->shown[l->block] || (!l->projector_file && !l->projector_pack_file))
      continue;
    const iso *m = &car->world[l->block];
    point(m, (const float[3]){l->location.t.x, l->location.t.y, l->location.t.z}, out->pos);
    for (int axis = 0; axis < 3; axis++)
      for (int k = 0; k < 3; k++)
        out->axes[axis][k] = m->r[k][0] * l->location.r.m[0][axis] + m->r[k][1] * l->location.r.m[1][axis] +
                             m->r[k][2] * l->location.r.m[2][axis];
    memcpy(out->frustum, l->frustum, sizeof out->frustum);
    // GmFrustum::GetFarZ
    if (l->frustum_flag) out->frustum[5] += out->frustum[2];
    out->intensity = l->intensity;
    memcpy(out->rgb, l->rgb, sizeof out->rgb);
    const tmuf_visual_texture t = {.file = l->projector_file, .pack_file = l->projector_pack_file};
    const uint32_t index = tm_map_index(game, &car->pictures, &t);
    out->picture = index != UINT32_MAX ? car->pictures.textures[index] : 0;
    return out->picture != 0;
  }
  return false;
}

bool tm_car_projector(ft_game *game, tm_car_model *car, tm_projector *out) {
  return car_projector(game, car, true, out);
}

// the fake blob shadow (shadow_spec §10): the car's ProjShad, where its
// parts stand this frame (after tm_car_shadow)
bool tm_car_blob(ft_game *game, tm_car_model *car, tm_projector *out) {
  return car->mesh && car_projector(game, car, false, out);
}

// the sea's refraction the car is drawn into, NULL for the view
typedef struct car_refraction {
  float water_y, day_time;
  tg_texture fog;
} car_refraction;

static void car_render(ft_game *game, tm_car_model *car, const ft_render_frame *frame, const car_refraction *refr);

void tm_car_render(ft_game *game, tm_car_model *car, const ft_render_frame *frame) { car_render(game, car, frame, NULL); }

// The car's depth alone (into the depth handed to the engine): its parts
// but the glass and the glows, posed as tm_car_render poses them
void tm_car_render_depth(ft_game *game, tm_car_model *car, const ft_render_frame *frame) {
  if (!car || !car->mesh || !game->track_depth_program) return;
  pose_parts(car, frame);
  const float *vp = frame->state.camera.view_proj;
  const tg_state state = {.depth_test = true, .depth_write = true, .no_color_write = true, .cull = TG_CULL_NONE};
  for (uint32_t i = 0; i < car->draw_count; i++) {
    const car_draw *d = &car->draws[i];
    if (d->glass || d->emissive_glow) continue;
    const iso *m = &car->world[d->part];
    const float model[16] = {m->r[0][0], m->r[1][0], m->r[2][0], 0.f, m->r[0][1], m->r[1][1], m->r[2][1], 0.f,
                             m->r[0][2], m->r[1][2], m->r[2][2], 0.f, m->t[0],    m->t[1],    m->t[2],    1.f};
    float view_proj[16];
    for (int col = 0; col < 4; col++)
      for (int row = 0; row < 4; row++)
        view_proj[col * 4 + row] = vp[0 * 4 + row] * model[col * 4 + 0] + vp[1 * 4 + row] * model[col * 4 + 1] +
                                   vp[2 * 4 + row] * model[col * 4 + 2] + vp[3 * 4 + row] * model[col * 4 + 3];
    tg_draw(game->gpu, game->track_depth_program, &state, car->mesh, d->first_index, d->index_count, NULL, NULL,
            view_proj);
  }
}

void tm_car_render_refraction(ft_game *game, tm_car_model *car, const ft_render_frame *frame, float water_y,
                              float day_time, tg_texture water_fog) {
  const car_refraction r = {water_y, day_time, water_fog};
  car_render(game, car, frame, &r);
}

static void car_render(ft_game *game, tm_car_model *car, const ft_render_frame *frame, const car_refraction *refr) {
  if (!car->mesh) return;
  const iso root = pose_parts(car, frame);
  const float *vp = frame->state.camera.view_proj; // column-major
  tm_track_uniforms u;
  memset(&u, 0, sizeof u);
  u.lod_bias = frame->state.lod_bias;
  u.opacity = frame->opacity;
  u.color_scale[0] = 1.f;
  u.color_scale[1] = 1.f;
  u.color_scale[3] = 1.f; // lit
  // the map's light (tmuf_weather.c): toward the sun for the lit draws
  const tm_light *l = &car->light;
  const float sun_dir[3] = {-l->sun_dir[0], -l->sun_dir[1], -l->sun_dir[2]};
  memcpy(u.blend2 + 4, l->sun_rgb, sizeof l->sun_rgb);
  memcpy(u.blend_mask, l->ambient, sizeof l->sun_rgb);
  // the skin's light: the weather's sun, ambient and clouds; LightFromMap as
  // it is in the open
  tm_car_uniforms cu;
  memset(&cu, 0, sizeof cu);
  const ft_camera *cam = &frame->state.camera;
  cu.eye[0] = cam->eye.x, cu.eye[1] = cam->eye.y, cu.eye[2] = cam->eye.z;
  memcpy(cu.light_dir, l->sun_dir, sizeof l->sun_dir);
  memcpy(cu.light_rgb, l->sun_rgb, sizeof l->sun_rgb);
  memcpy(cu.ambient, l->ambient, sizeof l->ambient);
  tm_light_clouds(l, frame->world ? (double)frame->world->w.tick * TMUF_TICK_MS / 1000.0 : 0.0, cu.clouds_u,
                  cu.clouds_v);
  const float fade[4] = {0.f, 1.f, 1.f, 0.f};
  memcpy(cu.fade, fade, sizeof fade);
  const float params[4] = {0.6f, 0.f, 0.f, 0.f};
  memcpy(cu.params, params, sizeof params);
  car_lights(car, &root, cam, &cu);
  cu.counts[3] = car->lfm_drawn ? 1.f : 0.f;
  // into the sea's refraction: what is under the water, in its fog
  if (refr) {
    const float water[4] = {refr->water_y, 1.f, refr->day_time, 1.f / 30.f}; // WaterDepthMax
    memcpy(u.water, water, sizeof water);
    memcpy(cu.water, water, sizeof water);
    u.eye[0] = cam->eye.x, u.eye[1] = cam->eye.y, u.eye[2] = cam->eye.z;
  }
  memcpy(cu.light_from_map, l->light_from_map, sizeof cu.light_from_map);
  memcpy(cu.lfm_u, car->lfm_u, sizeof cu.lfm_u);
  memcpy(cu.lfm_v, car->lfm_v, sizeof cu.lfm_v);
  // the world's player's skin pack
  // (TM_TEST_SKIN: one for pictures of the module, tools/refshot)
  const char *test_skin = getenv("TM_TEST_SKIN");
  const tg_texture paint = skin_paint(
      game, car, test_skin ? test_skin : tm_profile_skin(frame->player_setups, frame->player_setup_count, 0));
  // the glass after the rest, in two passes (CarGlass)
  for (uint32_t pass = 0; pass < 3; pass++)
  for (uint32_t i = 0; i < car->draw_count; i++) {
    const car_draw *d = &car->draws[i];
    if ((pass == 0) == d->glass || d->emissive_glow) continue;
    const iso *m = &car->world[d->part];
    // view_proj * model, both column-major
    const float model[16] = {m->r[0][0], m->r[1][0], m->r[2][0], 0.f, m->r[0][1], m->r[1][1], m->r[2][1], 0.f,
                             m->r[0][2], m->r[1][2], m->r[2][2], 0.f, m->t[0],    m->t[1],    m->t[2],    1.f};
    for (int col = 0; col < 4; col++)
      for (int row = 0; row < 4; row++)
        u.view_proj[col * 4 + row] = vp[0 * 4 + row] * model[col * 4 + 0] + vp[1 * 4 + row] * model[col * 4 + 1] +
                                     vp[2 * 4 + row] * model[col * 4 + 2] + vp[3 * 4 + row] * model[col * 4 + 3];
    u.alpha_cutoff = d->alpha_cutoff;
    memcpy(u.to_world, model, sizeof model);
    // the sun's direction in the part's frame (the normals are the mesh's own)
    for (int k = 0; k < 3; k++)
      u.blend2[k] = m->r[0][k] * sun_dir[0] + m->r[1][k] * sun_dir[1] + m->r[2][k] * sun_dir[2];
    const tg_texture textures[TM_TRACK_TEXTURES] = {d->texture != UINT32_MAX ? car->pictures.textures[d->texture] : 0, 0, 0, 0, 0, 0,
                                     refr ? refr->fog : 0};
    const tg_sampler samplers[TM_TRACK_TEXTURES] = {TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP,
                                     TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP,
                                     TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_CLAMP,
                                     {3, 3, 1, 0},    {3, 3, 1, 0},    {3, 3, 1, 0}};
    const tg_state opaque = {.depth_test = true, .depth_write = true, .cull = TG_CULL_CW};
    if (d->skin) {
      memcpy(cu.view_proj, u.view_proj, sizeof cu.view_proj);
      memcpy(cu.to_world, model, sizeof model);
      const uint32_t maps[6] = {d->clouds, d->texture, d->env, d->fresnel, d->dirt, d->self_illum};
      tg_texture skin_textures[8];
      for (int k = 0; k < 6; k++)
        skin_textures[k] = maps[k] != UINT32_MAX ? car->pictures.textures[maps[k]] : 0;
      if (d->paint && paint) skin_textures[1] = paint;
      // no self-illumination: black, not the white of an empty slot
      if (!d->details) skin_textures[5] = 0;
      skin_textures[6] = car->lfm_drawn ? tg_target_texture(car->lfm) : 0;
      skin_textures[7] = refr ? refr->fog : 0;
      cu.params[3] = d->glass ? (float)(pass + 1) : d->details ? 1.f : 0.f;
      const tg_sampler skin_samplers[8] = {TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP, TG_SAMPLER_WRAP, TG_SAMPLER_CLAMP,
                                           TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP, {3, 3, 2, 0},    TG_SAMPLER_CLAMP};
      const tg_state glass = {.blend_src = pass == 1 ? TG_BLEND_DESTCOLOR : TG_BLEND_ONE,
                              .blend_dst = pass == 1 ? TG_BLEND_ZERO : TG_BLEND_ONE,
                              .depth_test = true,
                              .cull = TG_CULL_CW};
      tg_draw(game->gpu, game->car_program, d->glass ? &glass : &opaque, car->mesh, d->first_index, d->index_count,
              skin_textures, skin_samplers, &cu);
      continue;
    }
    tg_draw(game->gpu, game->track_program, &opaque, car->mesh, d->first_index, d->index_count, textures, samplers,
            &u);
  }
}

// After the frame: the car's LightFromMap cell, reduced to 2 x 2 (the
// game's "LightFromMap_Download" target, CPlugBitmapRenderLightFromMap::
// ObjectGrabColors; skid_spec §7.3)
void tm_car_lfm_read(ft_game *game, tm_car_model *car) {
  if (!car || !car->lfm) return;
  if (!car->lfm_drawn) {
    car->lfm_read = false;
    return;
  }
  // this frame's cell, back in a frame or two; the newest back now
  tg_target_read_queue(game->gpu, car->lfm, TM_READ_LFM);
  const uint32_t w = tg_target_width(car->lfm), h = tg_target_height(car->lfm);
  uint8_t *px = malloc((size_t)w * h * 4);
  if (!px) return;
  if (tg_target_read_result(game->gpu, TM_READ_LFM, px, w * h * 4u, NULL)) {
    double sum[2][2][3] = {{{0}}};
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++)
        for (int k = 0; k < 3; k++)
          sum[y * 2 / h][x * 2 / w][k] += px[((size_t)y * w + x) * 4 + (size_t)k];
    const double n = (double)(w / 2) * (double)(h / 2) * 255.0;
    for (int r = 0; r < 2; r++)
      for (int c = 0; c < 2; c++)
        for (int k = 0; k < 3; k++)
          car->lfm_rgb[r][c][k] = (float)(sum[r][c][k] / n);
    car->lfm_read = true;
  }
  free(px);
}

// the LightFromMap colour under a world point near the car (its quadrant of
// the cell, as the game picks one per wheel); false without a cell
bool tm_car_lfm_tint(const tm_car_model *car, const float p[3], float rgb[3]) {
  if (!car || !car->lfm_read) return false;
  const float u = car->lfm_u[0] * p[0] + car->lfm_u[1] * p[1] + car->lfm_u[2] * p[2] + car->lfm_u[3];
  const float v = car->lfm_v[0] * p[0] + car->lfm_v[1] * p[1] + car->lfm_v[2] * p[2] + car->lfm_v[3];
  memcpy(rgb, car->lfm_rgb[v >= 0.5f][u >= 0.5f], sizeof(float) * 3);
  return true;
}
