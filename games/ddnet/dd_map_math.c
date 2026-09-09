/* C99 adaptation of DDNet's RenderEvalEnvelope and MapScreenToWorld.
 * See data/games/ddnet/mapres/LICENSE.txt for the upstream notice. */
#include "dd_map_math.h"
#include <math.h>

static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi
                                                                                  : v; }
static double bezier(double a, double b, double c, double d, double t) {
  double s = 1.0 - t;
  return s * s * s * a + 3.0 * s * s * t * b + 3.0 * s * t * t * c + t * t * t * d;
}

void dd_map_envelope(const map_data_t *map, int index, double time_ms, int channels, float result[4]) {
  if (index < 0 || index >= map->num_envelopes || !isfinite(time_ms)) return;
  const map_envelope_t *e = &map->envelopes[index];
  if (e->channels < channels) channels = e->channels;
  if (channels > 4) channels = 4;
  if (channels <= 0 || e->num_points <= 0 || e->start_point < 0 || e->start_point >= map->num_env_points ||
      e->num_points > map->num_env_points - e->start_point)
    return;
  const map_env_point_t *points = map->env_points + e->start_point;
  const map_env_point_t *last = points + e->num_points - 1;
  if (e->num_points == 1) {
    for (int c = 0; c < channels; ++c)
      result[c] = points->values[c] / 1024.f;
    return;
  }
  /* fmod deliberately preserves negative offsets, matching C++ remainder. */
  double time = last->time > 0 ? fmod(time_ms, last->time) : 0;
  int lo = 0, hi = e->num_points - 1;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (points[mid].time <= time) lo = mid;
    else hi = mid - 1;
  }
  if (time < points[lo].time || lo == e->num_points - 1) {
    for (int c = 0; c < channels; ++c)
      result[c] = last->values[c] / 1024.f;
    return;
  }
  const map_env_point_t *a = points + lo, *b = a + 1;
  double delta = (double)b->time - a->time;
  double t = delta > 0 ? (time - a->time) / delta : 0;
  switch (a->curve) {
  case 0:
    t = 0;
    break;
  case 2:
    t = t * t * t;
    break;
  case 3:
    t = 1 - (1 - t) * (1 - t) * (1 - t);
    break;
  case 4:
    t = t * t * (3 - 2 * t);
    break;
  default:
    break;
  }
  for (int c = 0; c < channels; ++c) {
    if (a->curve == 5 && map->env_bezier && delta > 0) {
      double x1 = clamp((double)a->time + a->out_dx[c], a->time, b->time);
      double x2 = clamp((double)b->time + b->in_dx[c], a->time, b->time);
      /* Monotone cubic inversion with a bounded error. Handles vertical and
       * coincident tangents without the unstable cubic-formula special cases. */
      double left = 0, right = 1, u = t;
      for (int i = 0; i < 28; ++i) {
        if (bezier(a->time, x1, x2, b->time, u) < time) left = u;
        else right = u;
        u = (left + right) * 0.5;
      }
      result[c] =
          (float)(bezier(a->values[c], (double)a->values[c] + a->out_dy[c], (double)b->values[c] + b->in_dy[c], b->values[c], u) / 1024.0);
    } else result[c] = (float)((a->values[c] + ((double)b->values[c] - a->values[c]) * t) / 1024.0);
  }
}

void dd_map_group_view(const map_group_t *g, const float camera[4], float view[4]) {
  float w = camera[2] - camera[0], h = camera[3] - camera[1];
  float aspect = w / h;
  /* DDNet's zoom=1 view; convert its pixels to the editor's 32px tiles. */
  float base_h = sqrtf(1150.f * 1000.f / aspect), base_w = base_h * aspect;
  if (base_w > 1500.f) {
    base_w = 1500.f;
    base_h = base_w / aspect;
  }
  if (base_h > 1050.f) {
    base_h = 1050.f;
    base_w = base_h * aspect;
  }
  float parallax_zoom = (float)clamp(g->parallax_x > g->parallax_y ? g->parallax_x : g->parallax_y, 0, 100) / 100.f;
  float width = base_w / 32.f + (w - base_w / 32.f) * parallax_zoom;
  float height = base_h / 32.f + (h - base_h / 32.f) * parallax_zoom;
  float cx = (camera[0] + camera[2]) * 0.5f * g->parallax_x / 100.f + g->offset_x / 32.f;
  float cy = (camera[1] + camera[3]) * 0.5f * g->parallax_y / 100.f + g->offset_y / 32.f;
  view[0] = cx - width * 0.5f;
  view[1] = cy - height * 0.5f;
  view[2] = cx + width * 0.5f;
  view[3] = cy + height * 0.5f;
}
