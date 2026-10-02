// The sky's 3D clouds (Stadium): CFuncClouds' solids, laid out by the
// game's CSceneMobilClouds in a grid around the camera drifting with the
// wind (tmuf_weather_clouds_place), each piece a set of sprites facing the
// camera, drawn with the CloudsPC2 shaders:
//
//   occlusion  first, into a picture of its own: a 30 degree camera at the
//              eye looking toward the sun, cleared white, each sprite taking
//              away op * alpha * 0.044 of what passes (the transmittance),
//              then multiplied by CloudsMask.dds (light passes near the sun)
//   main       after the opaque scene, back to front, blended: the picture
//              by CloudsRgbMin..Max (lit from the sun through the piece's
//              box, in its height's layer), the sun's flare colour where the
//              occlusion picture lets light through
//
// The game lights the sprites in its vertex shader; with their quads built
// here on the CPU each frame anyway, their colour and opacity are too.

#include "tmuf_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLOUDS_FAR 50000.f   // the race camera's: the grid and the fade
#define OCC_SIZE 256u        // CloudsRenderOcc.Texture.Gbx
#define OCC_FOV_DEG 30.f
#define OCC_OPACITY 0.044f

typedef struct cloud_vertex {
  float pos[3];
  float uv[2];
  float color[4];
  float occ[4]; // the occlusion camera's clip x, y, in front of the eye (1/0), w
} cloud_vertex;

typedef struct clouds_uniforms {
  float view_proj[16];
  float flare[4];
  float params[4];
} clouds_uniforms;

struct tm_clouds {
  const tmuf_weather *weather;
  tg_target *occ;
  tg_texture picture; // the pieces' "Clouds" (Cumulus02.dds)
  tg_texture mask;    // CloudsMask.dds, over the occlusion picture
  tmuf_cloud_draw *draws;
  uint32_t draw_cap;
  // this frame's placement (the occlusion pass's, the main pass's again)
  float *mesh_reach; // per cloud mesh, how far its quads reach from its box centre (0: not yet)
  bool placed;
  uint32_t placed_ms, placed_count;
  float placed_eye[3];
  cloud_vertex *vertices;
  uint32_t vertex_cap;
  float occ_view_proj[16]; // this frame's occlusion camera (world -> clip)
};

typedef struct v3 {
  float x, y, z;
} v3;
static v3 sub(v3 a, v3 b) { return (v3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot3(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross3(v3 a, v3 b) { return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static v3 norm3(v3 a) {
  const float l = sqrtf(dot3(a, a));
  return l > 0.f ? (v3){a.x / l, a.y / l, a.z / l} : a;
}
// a direction into a location's frame (its rotation transposed)
static v3 into(const tmuf_iso4 *l, v3 d) {
  return (v3){l->r.m[0][0] * d.x + l->r.m[1][0] * d.y + l->r.m[2][0] * d.z,
              l->r.m[0][1] * d.x + l->r.m[1][1] * d.y + l->r.m[2][1] * d.z,
              l->r.m[0][2] * d.x + l->r.m[1][2] * d.y + l->r.m[2][2] * d.z};
}
static v3 out_of(const tmuf_iso4 *l, const float p[3]) {
  return (v3){l->r.m[0][0] * p[0] + l->r.m[0][1] * p[1] + l->r.m[0][2] * p[2] + l->t.x,
              l->r.m[1][0] * p[0] + l->r.m[1][1] * p[1] + l->r.m[1][2] * p[2] + l->t.y,
              l->r.m[2][0] * p[0] + l->r.m[2][1] * p[1] + l->r.m[2][2] * p[2] + l->t.z};
}


tm_clouds *tm_clouds_create(ft_game *game, const tmuf_track *track, tm_picture_table *table) {
  const tmuf_weather *w = tmuf_track_weather(track);
  if (!w || !w->sky_clouds.piece_count || !game->clouds_program) return NULL;
  const tmuf_visuals *v = &w->sky_clouds.visuals;
  tm_clouds *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->weather = w;
  // every piece has the same material (Clouds.Material.Gbx)
  for (uint32_t i = 0; i < v->instance_count && !c->picture; i++) {
    const uint32_t m = v->instances[i].material;
    if (m < v->material_count) {
      const uint32_t index = tm_map_index(game, table, tm_sampler(&v->materials[m], "Clouds"));
      if (index != UINT32_MAX) c->picture = table->textures[index];
    }
  }
  c->occ = tg_target_create(game->gpu, OCC_SIZE, OCC_SIZE);
  char path[1024];
  game->engine->resolve_data_path("GameData/Clouds/Media/Texture/Image/CloudsMask.dds", path, sizeof path);
  void *data = NULL;
  size_t size = 0;
  if (game->engine->read_file(path, &data, &size)) {
    tm_image image;
    if (tm_image_load(data, size, tg_supports_bc(game->gpu), &image)) {
      c->mask = tg_texture_create(game->gpu, &image);
      tm_image_free(&image);
    }
    game->engine->free_file_data(data);
  }
  if (!c->picture || !c->occ || !c->mask) {
    tm_clouds_destroy(game, c);
    return NULL;
  }
  return c;
}

void tm_clouds_destroy(ft_game *game, tm_clouds *c) {
  if (!c) return;
  tg_target_destroy(game->gpu, c->occ);
  if (c->mask) tg_texture_destroy(game->gpu, c->mask);
  free(c->draws);
  free(c->mesh_reach);
  free(c->vertices);
  free(c);
}

static bool place(tm_clouds *c, const ft_camera *cam, uint32_t time_ms, uint32_t *count) {
  const float eye[3] = {cam->eye.x, cam->eye.y, cam->eye.z};
  if (c->placed && c->placed_ms == time_ms && !memcmp(c->placed_eye, eye, sizeof eye)) {
    *count = c->placed_count;
    return true;
  }
  const uint32_t n = tmuf_weather_clouds_place(c->weather, eye, CLOUDS_FAR, time_ms, NULL, 0);
  if (n > c->draw_cap) {
    tmuf_cloud_draw *grown = realloc(c->draws, sizeof *grown * n);
    if (!grown) return false;
    c->draws = grown;
    c->draw_cap = n;
  }
  *count = tmuf_weather_clouds_place(c->weather, eye, CLOUDS_FAR, time_ms, c->draws, n);
  c->placed = true;
  c->placed_ms = time_ms;
  c->placed_count = *count;
  memcpy(c->placed_eye, eye, sizeof eye);
  return true;
}

// how far a cloud mesh's quads reach from its box centre, whichever way
// they face (each sprite's centre, then its quad's half diagonal)
static float mesh_reach(tm_clouds *c, uint32_t mesh_index) {
  const tmuf_weather_sky_clouds *sc = &c->weather->sky_clouds;
  if (!c->mesh_reach && !(c->mesh_reach = calloc(sc->visuals.mesh_count ? sc->visuals.mesh_count : 1, sizeof(float))))
    return 1e9f;
  if (c->mesh_reach[mesh_index] > 0.f) return c->mesh_reach[mesh_index];
  const tmuf_visual_mesh *mesh = &sc->visuals.meshes[mesh_index];
  const float *bb = mesh->bounds;
  static const float right[3] = {1.f, 0.f, 0.f}, up[3] = {0.f, 1.f, 0.f};
  float reach = 1e-3f;
  for (uint32_t k = 0; k < mesh->vertex_count; k++) {
    float corners[4][3], uv[4][2];
    tmuf_cloud_sprite_quad(mesh, k, right, up, corners, uv);
    float mid[3] = {0.f, 0.f, 0.f}, half = 0.f, d2 = 0.f;
    for (int i = 0; i < 4; i++)
      for (int a = 0; a < 3; a++)
        mid[a] += 0.25f * corners[i][a];
    for (int i = 0; i < 4; i++) {
      float e = 0.f;
      for (int a = 0; a < 3; a++)
        e += (corners[i][a] - mid[a]) * (corners[i][a] - mid[a]);
      half = e > half ? e : half;
    }
    for (int a = 0; a < 3; a++)
      d2 += (mid[a] - bb[a]) * (mid[a] - bb[a]);
    const float r = sqrtf(d2) + sqrtf(half);
    reach = r > reach ? r : reach;
  }
  c->mesh_reach[mesh_index] = reach;
  return reach;
}

// a piece's sphere: around its box centre, as far as its quads reach
static void piece_sphere(tm_clouds *c, uint32_t draw, v3 *centre, float *radius) {
  const tmuf_weather_sky_clouds *sc = &c->weather->sky_clouds;
  const tmuf_cloud_draw *d = &c->draws[draw];
  const tmuf_visual_instance *in = &sc->visuals.instances[sc->pieces[d->piece].instance];
  const float *bb = sc->visuals.meshes[in->mesh].bounds;
  *centre = out_of(&d->location, bb);
  float scale = 0.f;
  for (int col = 0; col < 3; col++) {
    const float l = sqrtf(d->location.r.m[0][col] * d->location.r.m[0][col] + d->location.r.m[1][col] * d->location.r.m[1][col] +
                          d->location.r.m[2][col] * d->location.r.m[2][col]);
    scale = l > scale ? l : scale;
  }
  *radius = 1.01f * scale * mesh_reach(c, in->mesh);
}

// room for this frame's quads
static bool reserve(tm_clouds *c, uint32_t draws) {
  const tmuf_weather_sky_clouds *sc = &c->weather->sky_clouds;
  uint32_t sprites = 0;
  for (uint32_t i = 0; i < draws; i++) {
    const tmuf_visual_instance *in = &sc->visuals.instances[sc->pieces[c->draws[i].piece].instance];
    sprites += sc->visuals.meshes[in->mesh].vertex_count;
  }
  if (sprites * 6u > c->vertex_cap) {
    cloud_vertex *grown = realloc(c->vertices, sizeof *grown * sprites * 6u);
    if (!grown) return false;
    c->vertices = grown;
    c->vertex_cap = sprites * 6u;
  }
  return true;
}

// the occlusion camera: at the eye, toward the sun, 30 degrees square
static void occ_camera(const tm_light *l, v3 eye, v3 *x, v3 *y, v3 *f, float *cot, float view_proj[16]) {
  *f = norm3((v3){-l->sun_dir[0], -l->sun_dir[1], -l->sun_dir[2]});
  *x = norm3(cross3(*f, (v3){0.f, 1.f, 0.f}));
  *y = cross3(*f, *x);
  *cot = 1.f / tanf(OCC_FOV_DEG * 0.5f * 3.14159265f / 180.f);
  // clip = (cot x.(p - eye), cot y.(p - eye), w / 2, f.(p - eye)): no depth test
  const v3 rx = {x->x * *cot, x->y * *cot, x->z * *cot}, ry = {y->x * *cot, y->y * *cot, y->z * *cot};
  const float m[16] = {rx.x, ry.x, 0.5f * f->x, f->x, rx.y, ry.y, 0.5f * f->y, f->y,
                       rx.z, ry.z, 0.5f * f->z, f->z, -dot3(rx, eye), -dot3(ry, eye), -0.5f * dot3(*f, eye),
                       -dot3(*f, eye)};
  memcpy(view_proj, m, sizeof m);
}

// the vertex shader's layer tables (In01LayerIntenss / In01LayerHeights)
static float layer_of(float y) {
  static const float heights[5] = {0.f, 0.136f, 0.232f, 0.384f, 1.f};
  static const float intens[5] = {0.2f, 0.3f, 0.4f, 0.7f, 1.f};
  const int k = y >= heights[3] ? 3 : y >= heights[2] ? 2 : y >= heights[1] ? 1 : 0;
  float w = (y - heights[k]) / (heights[k + 1] - heights[k]);
  w = w < 0.f ? 0.f : w > 1.f ? 1.f : w;
  return intens[k] + (intens[k + 1] - intens[k]) * w;
}

static float sat(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

typedef struct sorted {
  uint32_t draw;
  float dist;
} sorted;
static int far_first(const void *a, const void *b) {
  const float da = ((const sorted *)a)->dist, db = ((const sorted *)b)->dist;
  return da < db ? 1 : da > db ? -1 : 0;
}

// the quads of the pieces, facing a camera (right, up, forward: world), as
// seen from eye; lit (the main pass) or not (the occlusion pass)
static uint32_t build(tm_clouds *c, const tm_light *l, const sorted *order, uint32_t count, v3 eye, v3 right, v3 up,
                      v3 fwd) {
  const tmuf_weather_sky_clouds *sc = &c->weather->sky_clouds;
  const float *ovp = c->occ_view_proj;
  uint32_t n = 0;
  for (uint32_t k = 0; k < count; k++) {
    const tmuf_cloud_draw *d = &c->draws[order ? order[k].draw : k];
    const tmuf_visual_instance *in = &sc->visuals.instances[sc->pieces[d->piece].instance];
    const tmuf_visual_mesh *mesh = &sc->visuals.meshes[in->mesh];
    const tmuf_iso4 *loc = &d->location;
    const v3 r = norm3(into(loc, right)), u = norm3(into(loc, up)), sun = into(loc, (v3){l->sun_dir[0], l->sun_dir[1],
                                                                                           l->sun_dir[2]});
    const float *bb = mesh->bounds; // centre, half extents
    for (uint32_t s = 0; s < mesh->vertex_count; s++) {
      float corners[4][3], uv[4][2];
      tmuf_cloud_sprite_quad(mesh, s, (const float[3]){r.x, r.y, r.z}, (const float[3]){u.x, u.y, u.z}, corners, uv);
      cloud_vertex q[4];
      for (int i = 0; i < 4; i++) {
        const v3 p = out_of(loc, corners[i]);
        q[i].pos[0] = p.x, q[i].pos[1] = p.y, q[i].pos[2] = p.z;
        q[i].uv[0] = uv[i][0], q[i].uv[1] = uv[i][1];
        // where in the piece's box (0..1), and from its middle (-1..1)
        float b[3], m[3];
        for (int a = 0; a < 3; a++) {
          const float half = bb[3 + a] > 0.f ? bb[3 + a] : 1.f;
          b[a] = (corners[i][a] - (bb[a] - half)) / (2.f * half);
          m[a] = 2.f * b[a] - 1.f;
        }
        const float light = 0.5f + 0.5f * -(m[0] * sun.x + m[1] * sun.y + m[2] * sun.z);
        const float t = sat(light * layer_of(b[1]));
        for (int ch = 0; ch < 3; ch++)
          q[i].color[ch] = l->clouds_min[ch] + (l->clouds_max[ch] - l->clouds_min[ch]) * t;
        // fading out toward the far distance, and toward the box's edges
        float op = sat((CLOUDS_FAR - dot3(fwd, sub(p, eye))) / (0.1f * CLOUDS_FAR));
        const float edge = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        q[i].color[3] = sat(1.f + (op - 1.f) * (1.f + edge));
        // the occlusion camera's clip coordinates
        const float cx = ovp[0] * p.x + ovp[4] * p.y + ovp[8] * p.z + ovp[12];
        const float cy = ovp[1] * p.x + ovp[5] * p.y + ovp[9] * p.z + ovp[13];
        const float cw = ovp[3] * p.x + ovp[7] * p.y + ovp[11] * p.z + ovp[15];
        q[i].occ[0] = cx, q[i].occ[1] = cy, q[i].occ[2] = cw > 0.f ? 1.f : 0.f, q[i].occ[3] = cw;
      }
      static const int tri[6] = {0, 1, 2, 2, 1, 3};
      for (int i = 0; i < 6; i++)
        c->vertices[n++] = q[tri[i]];
    }
  }
  return n;
}

static void camera_axes(const ft_camera *cam, v3 *right, v3 *up, v3 *fwd) {
  *fwd = norm3((v3){cam->forward.x, cam->forward.y, cam->forward.z});
  *right = norm3(cross3(*fwd, (v3){cam->up.x, cam->up.y, cam->up.z}));
  *up = cross3(*right, *fwd);
}

void tm_clouds_occlusion(ft_game *game, tm_clouds *c, const tm_light *l, const ft_camera *cam, uint32_t time_ms) {
  uint32_t count = 0;
  if (!c || !place(c, cam, time_ms, &count) || !reserve(c, count)) return;
  const v3 eye = {cam->eye.x, cam->eye.y, cam->eye.z};
  v3 x, y, f;
  float cot;
  occ_camera(l, eye, &x, &y, &f, &cot, c->occ_view_proj);
  // the pieces the sun's square cone reaches (sideways by the cone's half
  // width at their depth and their radius / cos 15)
  sorted *order = malloc(sizeof *order * (count ? count : 1));
  if (!order) return;
  const float t = 1.f / cot, grow = sqrtf(1.f + t * t);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; i++) {
    v3 centre;
    float r;
    piece_sphere(c, i, &centre, &r);
    const v3 d = sub(centre, eye);
    const float z = dot3(f, d);
    if (z + r <= 0.f || fabsf(dot3(x, d)) > t * z + r * grow || fabsf(dot3(y, d)) > t * z + r * grow) continue;
    order[kept++] = (sorted){i, 0.f};
  }
  const uint32_t n = build(c, l, order, kept, eye, x, y, f);
  free(order);
  clouds_uniforms u = {.params = {1.f, OCC_OPACITY}};
  memcpy(u.view_proj, c->occ_view_proj, sizeof u.view_proj);
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  const tg_state state = {.blend_src = TG_BLEND_SRCALPHA, .blend_dst = TG_BLEND_INVSRCALPHA, .cull = TG_CULL_NONE};
  const tg_texture textures[2] = {c->picture, 0};
  const tg_sampler samplers[2] = {TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP};
  tg_target_begin(game->gpu, c->occ, white);
  tg_draw_dynamic(game->gpu, game->clouds_program, &state, c->vertices, n, textures, samplers, &u);
  // the mask over the whole picture, multiplied
  static const float corner[6][2] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {-1.f, 1.f}, {1.f, -1.f}, {1.f, 1.f}};
  cloud_vertex quad[6];
  memset(quad, 0, sizeof quad);
  for (int i = 0; i < 6; i++) {
    quad[i].pos[0] = corner[i][0], quad[i].pos[1] = corner[i][1], quad[i].pos[2] = 0.5f;
    quad[i].uv[0] = 0.5f + 0.5f * corner[i][0], quad[i].uv[1] = 0.5f + 0.5f * corner[i][1];
  }
  clouds_uniforms mu = {.view_proj = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
                        .params = {2.f}};
  const tg_state multiply = {.blend_src = TG_BLEND_DESTCOLOR, .blend_dst = TG_BLEND_ZERO, .cull = TG_CULL_NONE};
  const tg_texture mask[2] = {c->mask, 0};
  tg_draw_dynamic(game->gpu, game->clouds_program, &multiply, quad, 6, mask, samplers, &mu);
  tg_target_end(game->gpu);
}

void tm_clouds_render(ft_game *game, tm_clouds *c, const tm_light *l, const ft_camera *cam, uint32_t time_ms) {
  uint32_t count = 0;
  if (!c || !place(c, cam, time_ms, &count) || !reserve(c, count)) return;
  const tmuf_weather_sky_clouds *sc = &c->weather->sky_clouds;
  const v3 eye = {cam->eye.x, cam->eye.y, cam->eye.z};
  // the pieces in view (the frustum's sides), back to front by the distance
  // to the piece's box centre
  sorted *order = malloc(sizeof *order * (count ? count : 1));
  if (!order) return;
  float planes[4][4];
  for (int i = 0; i < 4; i++) {
    const int axis = i >> 1;
    const float sign = (i & 1) ? -1.f : 1.f, *vp = cam->view_proj;
    float len = 0.f;
    for (int k = 0; k < 4; k++) {
      planes[i][k] = vp[k * 4 + 3] + sign * vp[k * 4 + axis];
      if (k < 3) len += planes[i][k] * planes[i][k];
    }
    len = len > 0.f ? 1.f / sqrtf(len) : 0.f;
    for (int k = 0; k < 4; k++)
      planes[i][k] *= len;
  }
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; i++) {
    v3 centre;
    float r;
    piece_sphere(c, i, &centre, &r);
    bool in = true;
    for (int k = 0; k < 4 && in; k++)
      in = planes[k][0] * centre.x + planes[k][1] * centre.y + planes[k][2] * centre.z + planes[k][3] >= -r;
    if (!in) continue;
    const tmuf_visual_instance *pin = &sc->visuals.instances[sc->pieces[c->draws[i].piece].instance];
    const v3 bc = out_of(&c->draws[i].location, sc->visuals.meshes[pin->mesh].bounds);
    const v3 d = sub(bc, eye);
    order[kept++] = (sorted){i, sqrtf(dot3(d, d))};
  }
  qsort(order, kept, sizeof *order, far_first);
  v3 right, up, fwd;
  camera_axes(cam, &right, &up, &fwd);
  const uint32_t n = build(c, l, order, kept, eye, right, up, fwd);
  free(order);
  // LightDirRgbFlare: the sun's colour, whether its flare shows or not (the
  // clouds' draws in the A02, A06, A10 traces, the sun behind the loop in A06's)
  clouds_uniforms u = {.flare = {l->sun_rgb[0], l->sun_rgb[1], l->sun_rgb[2], 1.f}};
  memcpy(u.view_proj, cam->view_proj, sizeof u.view_proj);
  const tg_state state = {.blend_src = TG_BLEND_SRCALPHA,
                          .blend_dst = TG_BLEND_INVSRCALPHA,
                          .depth_test = true,
                          .cull = TG_CULL_NONE};
  const tg_texture textures[2] = {c->picture, tg_target_texture(c->occ)};
  const tg_sampler samplers[2] = {TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP};
  tg_draw_dynamic(game->gpu, game->clouds_program, &state, c->vertices, n, textures, samplers, &u);
}

tg_texture tm_clouds_occlusion_texture(const tm_clouds *c) { return c ? tg_target_texture(c->occ) : 0; }
