// The car's shadow from the sun, as the game draws it (Shadows = Complex,
// CHmsShadowGroup 0):
//
//   volume     an orthographic box along the sun's light (GbxLightDirDir0)
//              fitted to the car's box, widened by the blur's margin
//              (512 / 507)
//   caster     the car's parts, flat grey (1 - 0.4 ShadowCarIntensity), into
//              a 512 x 512 picture cleared white and transparent, a pixel
//              of border kept clear (the receivers clamp to it: no shadow)
//   blur       across then down, three bilinear taps (the game's weights and
//              offsets, its missing fifth tap included)
//   receivers  the static scenery around, drawn again multiplying the frame
//              by sat(sat(shadow + faces away from the sun) + fade), the
//              fade a ramp along the light over 5 maxHalf / ShadeSlope
//
// the lamps' (Island's Sunset trace): up to ShadowCountCarHuman - 1 more
// pictures, the car seen from a lamp in perspective, not blurred, its grey
// 1 - ShadowCarIntensity (1 - (d / radius[2])^2), their receivers
// sat(picture + fade) with no faces-away term (vs_7 / vs_8);
// and the fake blob shadow (§10): the car's ProjShad picture projected
// straight down on the same receivers, the same multiply, its fade along
// the projector's depth, no faces-away term
//
// The picture is laid out as the receivers sample it: u = 0.5 + 0.5 / 512 +
// ndc.x 255 / 512, v = 0.5 + 0.5 / 512 - ndc.y 255 / 512, so the caster is
// drawn straight into those coordinates.

#include "tmuf_internal.h"

#include "shadow_frag_spv.h"
#include "shadow_vert_spv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 512u
#define BLUR_TEXELS 5.f
#define SHADE_SLOPE 0.5f // group 0's
#define SHADOW_K 0.4f    // the rest of the caster's grey (the trace's 0.8 at ShadowCarIntensity 0.5)

// the car's box in its own frame (CHmsItem's, as the game fits the shadow to it)
static const float car_box_centre[3] = {0.f, 0.405f, 0.094f};
static const float car_box_half[3] = {1.07f, 0.615f, 2.05f};

typedef struct shadow_uniforms {
  float view_proj[16]; // the view's (receivers) or the caster's (world -> the picture's clip space)
  float to_map[16];    // world -> the picture's clip space
  float light_dir[4];  // GbxLightDirDir0 (world)
  float ramp[4];       // xyz: the ramp coordinate's gradient (world), w its offset
  float params[4];     // x: 1 for the caster, y its grey; z: 1 for the blob (no faces-away term)
} shadow_uniforms;

// one car's: the sun's picture, the blob, the lamps' (tm_shadow_select)
typedef struct shadow_car {
  tg_target *map, *temp;
  bool active; // cast this frame
  shadow_uniforms u;
  float centre[3], reach; // the volume, for picking receivers
  struct {
    bool active;
    shadow_uniforms u;
    tg_texture picture;
    float centre[3], reach;
  } blob;
  struct {
    bool active;
    tg_target *map;
    shadow_uniforms u;
    float centre[3], reach;
  } lamps[TM_SHADOW_LAMPS];
} shadow_car;

struct tm_shadow {
  tg_program *caster, *receiver_block, *receiver_track;
  tg_texture ramp;
  shadow_car cars[TM_SHADOW_CARS]; // [0] the active car's, then the other shown groups'
  int current;                     // the car the casting calls go to
};

// the car the calls go to
static shadow_car *current(tm_shadow *s) { return &s->cars[s->current]; }

bool tm_shadow_resources_create(ft_game *game) {
  if (!game->gpu) return true;
  tm_shadow *s = calloc(1, sizeof *s);
  if (!s) return false;
  game->shadow = s;
  // the caster: the car's vertices (tm_track_vertex), position only
  const tg_attr track_attrs[] = {{0, offsetof(tm_track_vertex, pos), TG_FLOAT3},
                                 {1, offsetof(tm_track_vertex, normal), TG_UINT1}};
  const tg_attr block_attrs[] = {{0, offsetof(tm_block_vertex, pos), TG_FLOAT3},
                                 {1, offsetof(tm_block_vertex, normal), TG_UINT1}};
  tg_program_desc d = {.vertex_spirv = k_shadow_vert_spv,
                       .vertex_spirv_size = sizeof k_shadow_vert_spv,
                       .fragment_spirv = k_shadow_frag_spv,
                       .fragment_spirv_size = sizeof k_shadow_frag_spv,
                       .vertex_stride = sizeof(tm_track_vertex),
                       .attrs = track_attrs,
                       .attr_count = 2,
                       .uniform_size = sizeof(shadow_uniforms),
                       .texture_count = 2};
  s->caster = tg_program_create(game->gpu, &d);
  s->receiver_track = tg_program_create(game->gpu, &d);
  d.vertex_stride = sizeof(tm_block_vertex);
  d.attrs = block_attrs;
  s->receiver_block = tg_program_create(game->gpu, &d);
  s->cars[0].map = tg_target_create(game->gpu, SIZE, SIZE);
  s->cars[0].temp = tg_target_create(game->gpu, SIZE, SIZE);
  // the fade along the light: texel 0 white and transparent, then
  // 1 - cos(pi/2 x/511) (the game's 512 x 1 ramp)
  uint8_t ramp[SIZE * 4];
  for (uint32_t x = 0; x < SIZE; x++) {
    const uint8_t g = x ? (uint8_t)floor(255.0 * (1.0 - cos(1.5707963267948966 * x / 511.0))) : 255;
    ramp[x * 4 + 0] = ramp[x * 4 + 1] = ramp[x * 4 + 2] = g;
    ramp[x * 4 + 3] = x ? 255 : 0;
  }
  const tg_image image = {TG_RGBA8, SIZE, 1, 1, 1, ramp, sizeof ramp};
  s->ramp = tg_texture_create(game->gpu, &image);
  return s->caster && s->receiver_track && s->receiver_block && s->cars[0].map && s->cars[0].temp && s->ramp;
}

void tm_shadow_resources_destroy(ft_game *game) {
  tm_shadow *s = game->shadow;
  if (!s) return;
  if (s->caster) tg_program_destroy(game->gpu, s->caster);
  if (s->receiver_track) tg_program_destroy(game->gpu, s->receiver_track);
  if (s->receiver_block) tg_program_destroy(game->gpu, s->receiver_block);
  for (int c = 0; c < TM_SHADOW_CARS; c++) {
    shadow_car *sc = &s->cars[c];
    if (sc->map) tg_target_destroy(game->gpu, sc->map);
    if (sc->temp) tg_target_destroy(game->gpu, sc->temp);
    for (int i = 0; i < TM_SHADOW_LAMPS; i++)
      if (sc->lamps[i].map) tg_target_destroy(game->gpu, sc->lamps[i].map);
  }
  if (s->ramp) tg_texture_destroy(game->gpu, s->ramp);
  free(s);
  game->shadow = NULL;
}

typedef struct v3 {
  float x, y, z;
} v3;
static float dot3(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross3(v3 a, v3 b) { return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static v3 norm3(v3 a) {
  const float l = sqrtf(dot3(a, a));
  return l > 0.f ? (v3){a.x / l, a.y / l, a.z / l} : a;
}

void tm_shadow_clear(ft_game *game) {
  tm_shadow *s = game->shadow;
  if (!s) return;
  for (int c = 0; c < TM_SHADOW_CARS; c++) {
    s->cars[c].active = s->cars[c].blob.active = false;
    for (int i = 0; i < TM_SHADOW_LAMPS; i++)
      s->cars[c].lamps[i].active = false;
  }
  s->current = 0;
}

bool tm_shadow_select(ft_game *game, int car) {
  tm_shadow *s = game->shadow;
  if (!s || car < 0 || car >= TM_SHADOW_CARS) return false;
  shadow_car *sc = &s->cars[car];
  if (!sc->map) sc->map = tg_target_create(game->gpu, SIZE, SIZE);
  if (!sc->temp) sc->temp = tg_target_create(game->gpu, SIZE, SIZE);
  if (!sc->map || !sc->temp) return false;
  s->current = car;
  return true;
}

// the car's box in the world: its centre and half extents (as the sun's)
static void car_box(const float car_r[3][3], const float car_t[3], float c[3], float h[3]) {
  for (int i = 0; i < 3; i++) {
    c[i] = car_t[i];
    h[i] = 0.f;
    for (int k = 0; k < 3; k++) {
      c[i] += car_r[i][k] * car_box_centre[k];
      h[i] += fabsf(car_r[i][k]) * car_box_half[k];
    }
  }
}

bool tm_shadow_lamp_begin(ft_game *game, int i, const float lamp[3], float grey, const float car_r[3][3],
                          const float car_t[3]) {
  tm_shadow *ts = game->shadow;
  if (!ts || i < 0 || i >= TM_SHADOW_LAMPS || !game->settings.shadows) return false;
  shadow_car *s = current(ts);
  s->lamps[i].active = false;
  float c[3], h[3];
  car_box(car_r, car_t, c, h);
  const float max_half = fmaxf(h[0], fmaxf(h[1], h[2]));
  const float r = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]) * (float)SIZE / ((float)SIZE - 2.f);
  const v3 P = {lamp[0], lamp[1], lamp[2]}, C = {c[0], c[1], c[2]};
  const v3 to = {C.x - P.x, C.y - P.y, C.z - P.z};
  const float d = sqrtf(dot3(to, to));
  if (d <= r * 1.05f) return false;
  // looking at the car's box: its bounding sphere fills the picture
  const v3 z = norm3(to);
  v3 up = {0.f, 1.f, 0.f};
  if (dot3(cross3(z, up), cross3(z, up)) < 1e-6f) up = (v3){1.f, 0.f, 0.f};
  const v3 x = norm3(cross3(z, up)), y = cross3(x, z);
  const float t = r / sqrtf(d * d - r * r), k = 255.f / 256.f / t;
  float *m = s->lamps[i].u.to_map;
  memset(&s->lamps[i].u, 0, sizeof s->lamps[i].u);
  // w = z.(P - lamp); x and y over w, a texel of border
  m[0] = k * x.x + z.x / 512.f, m[4] = k * x.y + z.y / 512.f, m[8] = k * x.z + z.z / 512.f;
  m[12] = -k * dot3(x, P) - dot3(z, P) / 512.f;
  m[1] = -k * y.x + z.x / 512.f, m[5] = -k * y.y + z.y / 512.f, m[9] = -k * y.z + z.z / 512.f;
  m[13] = k * dot3(y, P) - dot3(z, P) / 512.f;
  m[3] = z.x, m[7] = z.y, m[11] = z.z, m[15] = -dot3(z, P);
  m[2] = 0.5f * m[3], m[6] = 0.5f * m[7], m[10] = 0.5f * m[11], m[14] = 0.5f * m[15];
  // the fade along the lamp's axis from the car's box, as the sun's
  const float hz = fabsf(z.x) * h[0] + fabsf(z.y) * h[1] + fabsf(z.z) * h[2];
  const float kr = 0.2f * SHADE_SLOPE / max_half;
  shadow_uniforms *u = &s->lamps[i].u;
  u->ramp[0] = kr * z.x, u->ramp[1] = kr * z.y, u->ramp[2] = kr * z.z;
  u->ramp[3] = kr * (hz - dot3(z, C)) + 1.5f / 512.f;
  u->params[1] = grey;
  u->params[2] = 1.f;
  memcpy(s->lamps[i].centre, c, sizeof c);
  s->lamps[i].reach = r + 5.f * max_half / SHADE_SLOPE;
  if (!s->lamps[i].map && !(s->lamps[i].map = tg_target_create(game->gpu, SIZE, SIZE))) return false;
  static const float clear[4] = {1.f, 1.f, 1.f, 0.f};
  tg_target_begin(game->gpu, s->lamps[i].map, clear);
  s->lamps[i].active = true;
  return true;
}

void tm_shadow_lamp_end(ft_game *game, int i) {
  tm_shadow *s = game->shadow;
  if (s && i >= 0 && i < TM_SHADOW_LAMPS && current(s)->lamps[i].active) tg_target_end(game->gpu);
}

void tm_shadow_blob(ft_game *game, const tm_projector *p) {
  tm_shadow *ts = game->shadow;
  if (!ts || !game->settings.shadows || !p->picture) return;
  shadow_car *s = current(ts);
  // orthographic down its +Z: x and y to their extents (a texel of border
  // as the sun's picture: the trace's 0.249 / m for 2 m); the fade at
  // 0.5 / (far - near) per metre down (the trace's 0.8), from half a metre
  // over the car's origin (A01's trace: T1 = 2.0142 - 0.8 (y - 88), the car
  // at 90.0138: 0.4 + 1.5 / 512 there), the ramp's white texel 0 above
  const float *x = p->axes[0], *y = p->axes[1], *z = p->axes[2];
  const float xmax = fmaxf(fabsf(p->frustum[0]), fabsf(p->frustum[3])),
              ymax = fmaxf(fabsf(p->frustum[1]), fabsf(p->frustum[4]));
  const float near = p->frustum[2], far = p->frustum[5];
  if (xmax <= 0.f || ymax <= 0.f || far <= near) return;
  const float kx = 255.f / 256.f / xmax, ky = 255.f / 256.f / ymax;
  const float ox = x[0] * p->pos[0] + x[1] * p->pos[1] + x[2] * p->pos[2];
  const float oy = y[0] * p->pos[0] + y[1] * p->pos[1] + y[2] * p->pos[2];
  const float oz = z[0] * p->pos[0] + z[1] * p->pos[1] + z[2] * p->pos[2];
  shadow_uniforms *u = &s->blob.u;
  memset(u, 0, sizeof *u);
  float *m = u->to_map;
  m[0] = kx * x[0], m[4] = kx * x[1], m[8] = kx * x[2], m[12] = 1.f / 512.f - kx * ox;
  m[1] = -ky * y[0], m[5] = -ky * y[1], m[9] = -ky * y[2], m[13] = 1.f / 512.f + ky * oy;
  m[14] = 0.5f;
  m[15] = 1.f;
  const float k = 0.5f / (far - near);
  u->ramp[0] = k * z[0], u->ramp[1] = k * z[1], u->ramp[2] = k * z[2];
  u->ramp[3] = -k * (oz - 0.5f) + 1.5f / 512.f;
  u->params[2] = 1.f;
  s->blob.picture = p->picture;
  const float mid = 0.5f * (near + 2.f * far - near);
  for (int i = 0; i < 3; i++)
    s->blob.centre[i] = p->pos[i] + z[i] * mid;
  s->blob.reach = sqrtf(xmax * xmax + ymax * ymax) + (2.f * far - near);
  s->blob.active = true;
}

bool tm_shadow_begin(ft_game *game, const tm_light *light, const float car_r[3][3], const float car_t[3]) {
  tm_shadow *ts = game->shadow;
  if (!ts) return false;
  shadow_car *s = current(ts);
  s->active = false;
  if (!game->settings.shadows || light->shadow_car_intensity <= 0.f) return false;
  // the car's box in the world (its axis-aligned box)
  float c[3], h[3];
  for (int i = 0; i < 3; i++) {
    c[i] = car_t[i];
    h[i] = 0.f;
    for (int k = 0; k < 3; k++) {
      c[i] += car_r[i][k] * car_box_centre[k];
      h[i] += fabsf(car_r[i][k]) * car_box_half[k];
    }
  }
  const float max_half = fmaxf(h[0], fmaxf(h[1], h[2]));
  const float margin = (float)SIZE / ((float)SIZE - BLUR_TEXELS);
  for (int i = 0; i < 3; i++)
    h[i] *= margin;
  // the light's frame (GmMat3::SetDOV)
  const v3 L = norm3((v3){light->sun_dir[0], light->sun_dir[1], light->sun_dir[2]});
  v3 up = {0.f, 1.f, 0.f};
  if (dot3(cross3(L, up), cross3(L, up)) < 1e-6f) up = (v3){1.f, 0.f, 0.f};
  const v3 xs = norm3(cross3(L, up)), ys = cross3(xs, L);
  const v3 ha = {h[0], h[1], h[2]};
  const float hx = fabsf(xs.x) * ha.x + fabsf(xs.y) * ha.y + fabsf(xs.z) * ha.z;
  const float hy = fabsf(ys.x) * ha.x + fabsf(ys.y) * ha.y + fabsf(ys.z) * ha.z;
  const float hz = fabsf(L.x) * ha.x + fabsf(L.y) * ha.y + fabsf(L.z) * ha.z;
  // world -> the picture's clip space: 2u - 1, 2v - 1
  const float kx = 255.f / 256.f / hx, ky = 255.f / 256.f / hy;
  const v3 C = {c[0], c[1], c[2]};
  float m[16] = {0};
  m[0] = kx * xs.x, m[4] = kx * xs.y, m[8] = kx * xs.z, m[12] = 1.f / 512.f - kx * dot3(xs, C);
  m[1] = -ky * ys.x, m[5] = -ky * ys.y, m[9] = -ky * ys.z, m[13] = 1.f / 512.f + ky * dot3(ys, C);
  m[14] = 0.5f;
  m[15] = 1.f;
  memcpy(s->u.to_map, m, sizeof m);
  s->u.light_dir[0] = L.x, s->u.light_dir[1] = L.y, s->u.light_dir[2] = L.z, s->u.light_dir[3] = 0.f;
  // the fade: (L.(P - C) + hz) 0.2 ShadeSlope / maxHalf + 1.5 / 512
  const float k = 0.2f * SHADE_SLOPE / max_half;
  s->u.ramp[0] = k * L.x, s->u.ramp[1] = k * L.y, s->u.ramp[2] = k * L.z;
  s->u.ramp[3] = k * (hz - dot3(L, C)) + 1.5f / 512.f;
  s->u.params[1] = 1.f - SHADOW_K * light->shadow_car_intensity;
  memcpy(s->centre, c, sizeof c);
  s->reach = sqrtf(hx * hx + hy * hy + hz * hz) + 5.f * max_half / SHADE_SLOPE;
  static const float clear[4] = {1.f, 1.f, 1.f, 0.f};
  tg_target_begin(game->gpu, s->map, clear);
  s->active = true;
  return true;
}

static void cast_into(ft_game *game, shadow_uniforms *u, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                      const float model_r[3][3], const float model_t[3]);

void tm_shadow_cast(ft_game *game, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                    const float model_r[3][3], const float model_t[3]) {
  tm_shadow *ts = game->shadow;
  if (!ts || !current(ts)->active) return;
  cast_into(game, &current(ts)->u, mesh, first_index, index_count, model_r, model_t);
}

void tm_shadow_lamp_cast(ft_game *game, int i, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                         const float model_r[3][3], const float model_t[3]) {
  tm_shadow *ts = game->shadow;
  if (!ts || i < 0 || i >= TM_SHADOW_LAMPS || !current(ts)->lamps[i].active) return;
  cast_into(game, &current(ts)->lamps[i].u, mesh, first_index, index_count, model_r, model_t);
}

static void cast_into(ft_game *game, shadow_uniforms *su, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                      const float model_r[3][3], const float model_t[3]) {
  tm_shadow *s = game->shadow;
  // to_map * model
  const float model[16] = {model_r[0][0], model_r[1][0], model_r[2][0], 0.f, model_r[0][1], model_r[1][1],
                           model_r[2][1], 0.f,           model_r[0][2], model_r[1][2], model_r[2][2], 0.f,
                           model_t[0],    model_t[1],    model_t[2],    1.f};
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++)
        sum += su->to_map[k * 4 + row] * model[col * 4 + k];
      su->view_proj[col * 4 + row] = sum;
    }
  su->params[0] = 1.f;
  const tg_state state = {.cull = TG_CULL_NONE};
  tg_draw(game->gpu, s->caster, &state, mesh, first_index, index_count, NULL, NULL, su);
}

void tm_shadow_end(ft_game *game) {
  tm_shadow *ts = game->shadow;
  if (!ts || !current(ts)->active) return;
  shadow_car *s = current(ts);
  tg_target_end(game->gpu);
  // across into temp, then down back (texels from the output's centre)
  static const float taps[4] = {-1.37288f, 0.45679f, 0.f, 0.f};
  static const float weights[4] = {0.364197f, 0.5f, 0.135803f, 0.f};
  float offsets[4][2];
  for (int i = 0; i < 4; i++)
    offsets[i][0] = taps[i] / SIZE, offsets[i][1] = 0.f;
  tm_post_taps(game, s->temp, tg_target_texture(s->map), offsets, weights);
  for (int i = 0; i < 4; i++)
    offsets[i][0] = 0.f, offsets[i][1] = taps[i] / SIZE;
  tm_post_taps(game, s->map, tg_target_texture(s->temp), offsets, weights);
}

static bool reaches(const float a[3], float ra, const float b[3], float rb) {
  const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz <= (ra + rb) * (ra + rb);
}

static bool car_reaches(const shadow_car *s, const float centre[3], float radius) {
  for (int i = 0; i < TM_SHADOW_LAMPS; i++)
    if (s->lamps[i].active && reaches(centre, radius, s->lamps[i].centre, s->lamps[i].reach)) return true;
  return (s->active && reaches(centre, radius, s->centre, s->reach)) ||
         (s->blob.active && reaches(centre, radius, s->blob.centre, s->blob.reach));
}

bool tm_shadow_reaches(const ft_game *game, const float centre[3], float radius) {
  const tm_shadow *s = game->shadow;
  if (!s) return false;
  for (int c = 0; c < TM_SHADOW_CARS; c++)
    if (car_reaches(&s->cars[c], centre, radius)) return true;
  return false;
}

// the pixels a shadow can darken: its volume's box on screen (its receivers
// outside it are left as they are), all when a corner is behind the eye
static void scissor_to(ft_game *game, const float view_proj[16], const float centre[3], float reach) {
  const float w = (float)game->frame_width, h = (float)game->frame_height;
  float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
  for (int i = 0; i < 8; i++) {
    const float p[3] = {centre[0] + (i & 1 ? reach : -reach), centre[1] + (i & 2 ? reach : -reach),
                        centre[2] + (i & 4 ? reach : -reach)};
    float c[4];
    for (int r = 0; r < 4; r++)
      c[r] = view_proj[r] * p[0] + view_proj[4 + r] * p[1] + view_proj[8 + r] * p[2] + view_proj[12 + r];
    if (c[3] <= 1e-3f) {
      tg_scissor(game->gpu, 0, 0, 0, 0);
      return;
    }
    for (int k = 0; k < 2; k++) {
      const float v = c[k] / c[3];
      lo[k] = v < lo[k] ? v : lo[k];
      hi[k] = v > hi[k] ? v : hi[k];
    }
  }
  float x0 = floorf((lo[0] * 0.5f + 0.5f) * w) - 1.f, x1 = ceilf((hi[0] * 0.5f + 0.5f) * w) + 1.f;
  float y0 = floorf((lo[1] * 0.5f + 0.5f) * h) - 1.f, y1 = ceilf((hi[1] * 0.5f + 0.5f) * h) + 1.f;
  x0 = x0 < 0.f ? 0.f : x0, y0 = y0 < 0.f ? 0.f : y0;
  x1 = x1 > w ? w : x1, y1 = y1 > h ? h : y1;
  if (x1 <= x0 || y1 <= y0) x0 = y0 = 0.f, x1 = y1 = 1.f; // (off screen: one pixel, nothing drawn there)
  tg_scissor(game->gpu, (int32_t)x0, (int32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

void tm_shadow_receive(ft_game *game, bool block, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                       const float view_proj[16], const float centre[3], float radius) {
  tm_shadow *ts = game->shadow;
  if (!ts) return;
  // multiplying what is there, where it is (the game's LESSEQUAL, no depth
  // writes): each car's sun shadow, lamps' and blob
  const tg_state state = {.blend_src = TG_BLEND_DESTCOLOR,
                          .blend_dst = TG_BLEND_ZERO,
                          .depth_test = true,
                          .cull = TG_CULL_CW};
  const tg_sampler samplers[2] = {{3, 3, 2, 0}, {3, 3, 2, 0}};
  tg_program *program = block ? ts->receiver_block : ts->receiver_track;
  for (int c = 0; c < TM_SHADOW_CARS; c++) {
    shadow_car *s = &ts->cars[c];
    if (!car_reaches(s, centre, radius)) continue;
    if (s->active && reaches(centre, radius, s->centre, s->reach)) {
      memcpy(s->u.view_proj, view_proj, sizeof s->u.view_proj);
      s->u.params[0] = 0.f;
      const tg_texture textures[2] = {tg_target_texture(s->map), ts->ramp};
      scissor_to(game, view_proj, s->centre, s->reach);
      tg_draw(game->gpu, program, &state, mesh, first_index, index_count, textures, samplers, &s->u);
    }
    for (int i = 0; i < TM_SHADOW_LAMPS; i++) {
      if (!s->lamps[i].active || !reaches(centre, radius, s->lamps[i].centre, s->lamps[i].reach)) continue;
      memcpy(s->lamps[i].u.view_proj, view_proj, sizeof s->lamps[i].u.view_proj);
      s->lamps[i].u.params[0] = 0.f;
      const tg_texture textures[2] = {tg_target_texture(s->lamps[i].map), ts->ramp};
      scissor_to(game, view_proj, s->lamps[i].centre, s->lamps[i].reach);
      tg_draw(game->gpu, program, &state, mesh, first_index, index_count, textures, samplers, &s->lamps[i].u);
    }
    if (s->blob.active && reaches(centre, radius, s->blob.centre, s->blob.reach)) {
      memcpy(s->blob.u.view_proj, view_proj, sizeof s->blob.u.view_proj);
      const tg_texture textures[2] = {s->blob.picture, ts->ramp};
      scissor_to(game, view_proj, s->blob.centre, s->blob.reach);
      tg_draw(game->gpu, program, &state, mesh, first_index, index_count, textures, samplers, &s->blob.u);
    }
  }
  tg_scissor(game->gpu, 0, 0, 0, 0);
}
