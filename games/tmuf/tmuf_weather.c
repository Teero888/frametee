// The light of the map's time of day, as the game's weather gives its
// shaders (CMotionManagerWeathers::UpdateAsync, tmuf_track_weather): the
// weather's pictures are ramps over the day, sampled once at the mood's
// time (the clock does not run in a race).

#include "tmuf_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A01's sunset, as the game had it: for maps without a weather
static const tm_light fallback = {
    .sun_dir = {-0.991445f, -0.0839005f, 0.0999888f},
    .sun_rgb = {1.f, 0.560784f, 0.223529f},
    .sun_rgb_double_sided = {1.f, 0.560784f, 0.223529f},
    .ambient = {0.584314f, 0.517647f, 0.603922f, 0.431372f},
    .clouds_min = {0.717647f, 0.760784f, 0.815686f},
    .clouds_max = {0.882353f, 0.784314f, 0.615686f},
    .fog_rgb = {0.717647f, 0.396078f, 0.407843f},
    .fog_start = 100.f,
    .fog_end = 4000.f,
    .day_time = {0.729167f, 0.75f},
    .spec_intensity = 0.8f,
    .spec_power = 20.f,
    .shadow_car_intensity = 0.5f,
    .clouds_scale = {0.0005f, 0.0005f},
    .clouds_speed = {1.f, 1.f},
    .clouds_period = 342.f,
};

// a picture in the order the game keeps it (TGA rows as stored, DDS
// flipped), RGBA
static bool read_picture(ft_game *game, const tmuf_weather_file *f, uint8_t **rgba, uint32_t *w, uint32_t *h) {
  if (!f->file && !f->pack_file) return false;
  const ft_engine_api *api = game->engine;
  void *engine_data = NULL;
  uint8_t *data = NULL;
  size_t size = 0;
  if (f->file) {
    if (!api->read_file(f->file, &engine_data, &size)) return false;
    data = engine_data;
  } else {
    data = tmuf_packs_read(game->packs, f->pack_file, &size);
  }
  if (!data) return false;
  const bool dds = size >= 4 && memcmp(data, "DDS ", 4) == 0;
  const bool ok = tm_image_decode(data, size, rgba, w, h);
  if (engine_data) api->free_file_data(engine_data);
  else tmuf_free(data);
  if (ok && dds) {
    const size_t row = (size_t)*w * 4;
    uint8_t *tmp = malloc(row);
    for (uint32_t y = 0; tmp && y < *h / 2; y++) {
      memcpy(tmp, *rgba + y * row, row);
      memcpy(*rgba + y * row, *rgba + (size_t)(*h - 1 - y) * row, row);
      memcpy(*rgba + (size_t)(*h - 1 - y) * row, tmp, row);
    }
    free(tmp);
  }
  return ok;
}

// a picture's colour at the time of day (bytes, as the game truncates them)
static bool sample(ft_game *game, const tmuf_weather_file *f, float remapped, uint8_t out[4]) {
  uint8_t *rgba = NULL;
  uint32_t w = 0, h = 0;
  if (!read_picture(game, f, &rgba, &w, &h)) return false;
  tmuf_picture_sample(rgba, w, h, 4, remapped, 0.f, out);
  free(rgba);
  return true;
}

static void to_float(const uint8_t in[4], float out[3]) {
  for (int k = 0; k < 3; k++)
    out[k] = (float)in[k] * 0.0039215688f;
}

void tm_light_of(ft_game *game, const tmuf_track *track, tm_light *out) {
  *out = fallback;
  const tmuf_weather *w = track ? tmuf_track_weather(track) : NULL;
  if (!w) return;
  const tmuf_day_time *t = &w->start;
  const float r = t->remapped;
  out->day_time[0] = t->time;
  out->shadow_car_intensity = w->mood.shadow_car_intensity;
  // Stadium maps by day (IsNight: the start time outside 0.25 .. 0.75, so
  // Sunset is night) bake two-channel lightmaps, (sky, sun), which the
  // shaders read with GbxLightGenP_ScaleAD_TransDA: from the mood's
  // PackLightMapMood (AmbMin, AmbMax, DirScale): (AmbMax - AmbMin, DirScale,
  // 0, AmbMin) (CHmsPackLightMapMood::UpdateFileGpuGlobalConstants).
  // MoodSettings holds (0.5, 1.1, 1.2); Sunrise's LightMapSettings (0.344,
  // 1.0, 2.0). LightFromMap's from its ambient and sun ranges.
  const char *env = tmuf_track_environment(track);
  if (!w->is_night && env && !strcmp(env, "Stadium")) {
    float amb_min = 0.5f, amb_max = 1.1f, dir_scale = 1.2f;
    if (w->mood.folder && strstr(w->mood.folder, "Sunrise")) amb_min = 0.344f, amb_max = 1.f, dir_scale = 2.f;
    const float gen[4] = {amb_max - amb_min, dir_scale, 0.f, amb_min}, lfm[4] = {1.f, 1.18483f, 0.14218f, 0.5f};
    memcpy(out->lightgen, gen, sizeof gen);
    memcpy(out->light_from_map, lfm, sizeof lfm);
  }
  out->day_time[1] = r;
  uint8_t sun[4] = {255, 255, 255, 255}, moon[4] = {0, 0, 0, 255}, c[4];
  sample(game, &w->light_sun, r, sun);
  const bool has_moon = sample(game, &w->light_moon, r, moon);
  out->moon = tmuf_day_time_light(t, sun, has_moon ? moon : (const uint8_t[3]){0, 0, 0}, out->sun_dir) != 0;
  to_float(out->moon ? moon : sun, out->sun_rgb);
  // the double-sided light: its own picture, else the sun's
  if (sample(game, &w->light_double_sided, r, c)) to_float(c, out->sun_rgb_double_sided);
  else memcpy(out->sun_rgb_double_sided, out->sun_rgb, sizeof out->sun_rgb);
  if (sample(game, &w->light_ambient, r, c)) {
    to_float(c, out->ambient);
    out->ambient[3] = 1.f - (out->ambient[0] + out->ambient[1] + out->ambient[2]) / 3.f;
  }
  if (sample(game, &w->clouds_min, r, c)) to_float(c, out->clouds_min);
  if (sample(game, &w->clouds_max, r, c)) to_float(c, out->clouds_max);
  tmuf_weather_fog fog;
  tmuf_weather_at(w, t, &fog, &out->spec_intensity, &out->spec_power);
  memcpy(out->fog_rgb, fog.rgb, sizeof fog.rgb);
  if (sample(game, &w->fog_color, r, c)) to_float(c, out->fog_rgb);
  out->fog_start = fog.start;
  out->fog_end = fog.end;
  if (w->has_clouds_layer) {
    memcpy(out->clouds_scale, w->clouds_scale, sizeof out->clouds_scale);
    memcpy(out->clouds_speed, w->clouds_speed, sizeof out->clouds_speed);
    memcpy(out->clouds_offset, w->clouds_offset, sizeof out->clouds_offset);
    out->clouds_period = w->clouds_period > 0.f ? w->clouds_period : 342.f;
  }
}

void tm_light_clouds(const tm_light *l, double seconds, float u[4], float v[4]) {
  // EGxUVGenerate Hack1Vertex, then the clouds layer's scale and scroll
  const double ph = l->clouds_period > 0.f ? seconds / l->clouds_period - floor(seconds / l->clouds_period) : 0.0;
  const float s2 = 0.70710678f, s6 = 0.40824829f;
  u[0] = 0.f;
  u[1] = -l->clouds_scale[0] * s2;
  u[2] = l->clouds_scale[0] * s2;
  u[3] = l->clouds_offset[0] + (float)ph * l->clouds_speed[0];
  v[0] = 2.f * l->clouds_scale[1] * s6;
  v[1] = l->clouds_scale[1] * s6;
  v[2] = -l->clouds_scale[1] * s6;
  v[3] = l->clouds_offset[1] + (float)ph * l->clouds_speed[1];
}
