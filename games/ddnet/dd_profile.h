#ifndef DD_PROFILE_H
#define DD_PROFILE_H

#include <frametee/game_abi.h>
#include <stdbool.h>
#include <stdint.h>

// Bumped whenever the layout below changes. Older projects hand back bytes with
// an older version, which decode into whatever this build understands.
#define DD_PROFILE_VERSION 1u

typedef struct dd_player_profile_t {
  uint32_t version;
  // Sized as the protocol sizes them, so a name that fits here fits a demo.
  char name[16];
  char clan[12];
  char skin[64];
  uint8_t use_custom_color;
  uint8_t padding[3];
  uint32_t color_body; // 0xHHSSLL, the client's own packing
  uint32_t color_feet;
} dd_player_profile_t;

// The tee a track starts as, before anyone has touched its panel.
void dd_profile_default(dd_player_profile_t *out);

void dd_profile_decode(const void *data, uint32_t size, dd_player_profile_t *out);

// The profile of one editor track, by track index (ft_engine_state numbering).
void dd_profile_for_track(ft_game *game, int32_t track, dd_player_profile_t *out);
// The profile carried by a render frame's player setup.
void dd_profile_from_setup(const ft_player_setup *setup, dd_player_profile_t *out);
// Writes a profile back to the editor, which stores and saves it.
bool dd_profile_store(ft_game *game, int32_t track, const dd_player_profile_t *profile);

// The nickname to show for a track, falling back to the editor's track name and
// then to a placeholder, so a caller always has something to print.
void dd_profile_display_name(ft_game *game, int32_t track, char *out, size_t out_size);

// DDNet packs tee colours as hue/saturation/lightness bytes and remaps the
// lightness into the upper half of the range, which is why a "black" tee is
// grey. Both halves of that live here.
void dd_hsl_to_rgb(uint32_t packed_hsl, float out_rgb[3]);
uint32_t dd_hsl_pack(float h, float s, float l);
void dd_hsl_unpack(uint32_t packed_hsl, float *out_h, float *out_s, float *out_l);
void dd_hsl_components_to_rgb(float h, float s, float l, float out_rgb[3]);

// Lightness below this is never reachable: the client's own clamp.
#define DD_DARKEST_LIGHTNESS 0.5f

#endif // DD_PROFILE_H
