#include "dd_profile.h"

#include "dd_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void dd_profile_default(dd_player_profile_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->version = DD_PROFILE_VERSION;
  snprintf(out->skin, sizeof(out->skin), "%s", "default");
  // The client's defaults, so an untouched tee looks like the one every player
  // starts with rather than a white blob.
  out->color_body = 65408u;
  out->color_feet = 65408u;
}

void dd_profile_decode(const void *data, uint32_t size, dd_player_profile_t *out) {
  if (!out) return;
  dd_profile_default(out);
  if (!data || size == 0) return;

  const dd_player_profile_t *stored = (const dd_player_profile_t *)data;
  if (size < sizeof(uint32_t) || stored->version != DD_PROFILE_VERSION || size != sizeof(*stored)) return;

  *out = *stored;
  // Whatever the bytes claimed, this build only ever hands out terminated
  // strings of its own sizes.
  out->name[sizeof(out->name) - 1] = '\0';
  out->clan[sizeof(out->clan) - 1] = '\0';
  out->skin[sizeof(out->skin) - 1] = '\0';
  if (!out->skin[0]) snprintf(out->skin, sizeof(out->skin), "%s", "default");
}

void dd_profile_from_setup(const ft_player_setup *setup, dd_player_profile_t *out) {
  if (!out) return;
  if (!setup) {
    dd_profile_default(out);
    return;
  }
  dd_profile_decode(setup->data, setup->data_size, out);
}

void dd_profile_for_track(ft_game *game, int32_t track, dd_player_profile_t *out) {
  if (!out) return;
  dd_profile_default(out);
  if (!game || track < 0) return;

  const ft_engine_api *engine = game->engine;
  ft_player_setup setup = {.struct_size = sizeof(setup)};
  if (!engine || !engine->get_player_setup || !engine->get_player_setup(track, &setup)) return;
  dd_profile_decode(setup.data, setup.data_size, out);
}

bool dd_profile_store(ft_game *game, int32_t track, const dd_player_profile_t *profile) {
  if (!game || !profile || track < 0) return false;
  const ft_engine_api *engine = game->engine;
  if (!engine || !engine->set_player_profile) return false;

  dd_player_profile_t stored = *profile;
  stored.version = DD_PROFILE_VERSION;
  memset(stored.padding, 0, sizeof(stored.padding));
  return engine->set_player_profile(track, &stored, (uint32_t)sizeof(stored));
}

void dd_profile_display_name(ft_game *game, int32_t track, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  dd_player_profile_t profile;
  dd_profile_for_track(game, track, &profile);
  if (profile.name[0]) {
    snprintf(out, out_size, "%s", profile.name);
    return;
  }
  // No nickname yet: the editor's own label for the track is the next best
  // thing to call this tee.
  const ft_engine_api *engine = game ? game->engine : NULL;
  ft_player_setup setup = {.struct_size = sizeof(setup)};
  if (engine && engine->get_player_setup && engine->get_player_setup(track, &setup) && setup.track_name && setup.track_name[0])
    snprintf(out, out_size, "%s", setup.track_name);
  else snprintf(out, out_size, "player");
}

// --- colours -----------------------------------------------------------------

static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

void dd_hsl_components_to_rgb(float h, float s, float l, float out_rgb[3]) {
  h -= floorf(h); // wrap
  s = clamp01(s);
  l = clamp01(l);

  const float c = (1.f - fabsf(2.f * l - 1.f)) * s;
  const float h1 = h * 6.f;
  const float x = c * (1.f - fabsf(fmodf(h1, 2.f) - 1.f));

  float r = 0.f, g = 0.f, b = 0.f;
  int sector = (int)floorf(h1) % 6;
  if (sector < 0) sector += 6;
  switch (sector) {
  case 0: r = c, g = x; break;
  case 1: r = x, g = c; break;
  case 2: g = c, b = x; break;
  case 3: g = x, b = c; break;
  case 4: r = x, b = c; break;
  default: r = c, b = x; break;
  }

  const float m = l - 0.5f * c;
  out_rgb[0] = clamp01(r + m);
  out_rgb[1] = clamp01(g + m);
  out_rgb[2] = clamp01(b + m);
}

void dd_hsl_unpack(uint32_t packed_hsl, float *out_h, float *out_s, float *out_l) {
  const float h = (float)((packed_hsl >> 16) & 0xFFu) / 255.f;
  const float s = (float)((packed_hsl >> 8) & 0xFFu) / 255.f;
  const float l = (float)(packed_hsl & 0xFFu) / 255.f;
  if (out_h) *out_h = h;
  if (out_s) *out_s = s;
  // The client never lets a tee go darker than half lightness, so the stored
  // byte spans the upper half of the range rather than all of it.
  if (out_l) *out_l = DD_DARKEST_LIGHTNESS + l * (1.f - DD_DARKEST_LIGHTNESS);
}

uint32_t dd_hsl_pack(float h, float s, float l) {
  float stored_l = (l - DD_DARKEST_LIGHTNESS) / (1.f - DD_DARKEST_LIGHTNESS);
  stored_l = clamp01(stored_l);
  const uint32_t hb = (uint32_t)roundf(clamp01(h) * 255.f);
  const uint32_t sb = (uint32_t)roundf(clamp01(s) * 255.f);
  const uint32_t lb = (uint32_t)roundf(stored_l * 255.f);
  return (hb << 16) | (sb << 8) | lb;
}

void dd_hsl_to_rgb(uint32_t packed_hsl, float out_rgb[3]) {
  float h, s, l;
  dd_hsl_unpack(packed_hsl, &h, &s, &l);
  dd_hsl_components_to_rgb(h, s, l, out_rgb);
}
