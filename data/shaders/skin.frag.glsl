#version 450

layout(binding = 1) uniform sampler2DArray skins;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_skin_index;
layout(location = 2) flat in int frag_eye;
layout(location = 3) flat in vec3 frag_body;
layout(location = 4) flat in vec3 frag_back;
layout(location = 5) flat in vec3 frag_front;
layout(location = 6) flat in vec3 frag_attach;

layout(location = 0) out vec4 out_color;

struct part_info_t {
  ivec2 atlas_offset; // offset in atlas (px)
  ivec2 atlas_size;   // size in atlas (px)
  vec2 place_offset;  // where to put it in quad space (0–1)
  vec2 place_size;    // size on quad (0–1)
};

// sample from atlas while applying placement
vec4 sample_skin(part_info_t part, vec2 frag_uv, int skin_index) {
  // check inside placement zone
  if (frag_uv.x < part.place_offset.x || frag_uv.x > part.place_offset.x + part.place_size.x ||
      frag_uv.y < part.place_offset.y || frag_uv.y > part.place_offset.y + part.place_size.y) {
    return vec4(0.0);
  }

  // local UV within the part
  vec2 local_uv = (frag_uv - part.place_offset) / part.place_size;

  // convert to atlas UVs
  vec2 atlas_full = vec2(256.0, 128.0);
  vec2 uv = vec2(part.atlas_offset) / atlas_full + local_uv * (vec2(part.atlas_size) / atlas_full);

  vec4 c = texture(skins, vec3(uv, float(skin_index)));

  // premultiply
  c.rgb *= c.a;
  return c;
}

// blending
vec4 blend_pma(vec4 dst, vec4 src) {
  return vec4(src.rgb + dst.rgb * (1.0 - src.a), src.a + dst.a * (1.0 - src.a));
}

vec2 rotate(vec2 p, float angle) {
  float c = cos(angle), s = sin(angle);
  return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}
vec2 apply_anim(vec2 uv, vec3 anim) {
  vec2 offs = anim.xy / 32.0;
  uv -= vec2(0.5);
  uv = rotate(uv, anim.z);
  uv += vec2(0.5) + offs;
  return uv;
}

void main() {
  // hardcoded part placements
  part_info_t body = part_info_t(ivec2(0, 0), ivec2(96, 96), vec2(0.0, -0.0625 /* 4/64 */), vec2(1.0, 1.0));
  part_info_t body_shadow = part_info_t(ivec2(96, 0), ivec2(96, 96), body.place_offset, body.place_size);

  // part_info_t hand = part_info_t(
  //     ivec2(192,0), ivec2(32,32),
  //     vec2(0.70,0.45), vec2(0.15,0.15)
  // );
  // part_info_t hand_shadow = part_info_t(
  //     ivec2(224,0), ivec2(32,32),
  //     hand.place_offset, hand.place_size
  // );

  part_info_t foot_right = part_info_t(ivec2(192, 32), ivec2(64, 32),
                                       vec2(0.109375 /* 7/64 */, 0.40625 /* 13/32 */), vec2(1.0, 0.5));

  part_info_t foot_shadow_right = foot_right;
  foot_shadow_right.atlas_offset = ivec2(192, 64);
  // mirrored
  part_info_t foot_left = foot_right;
  foot_left.place_offset.x *= -1.0;
  part_info_t foot_shadow_left = foot_shadow_right;
  foot_shadow_left.place_offset.x *= -1.0;

  // Eyes: select from frag_eye (atlas slots 6–11)
  ivec2 eye_offsets[6] = ivec2[6](ivec2(64, 96),  // normal eyes
                                  ivec2(96, 96),  // angry
                                  ivec2(128, 96), // hurt
                                  ivec2(160, 96), // happy
                                  ivec2(192, 96), // dead
                                  ivec2(224, 96)  // open
  );

  part_info_t eye_right =
      part_info_t(eye_offsets[frag_eye - 6], ivec2(32, 32), vec2(0.40, 0.58), vec2(0.20, 0.20));
  part_info_t eye_left = eye_right;
  eye_left.place_offset.x *= -1.0;

  // final color
  vec2 local = frag_uv;
  vec4 final_color = vec4(0.0);

  // Apply per-part animation to UVs before sampling
  vec2 uv_body = apply_anim(frag_uv, frag_body);
  vec2 uv_back = apply_anim(frag_uv, frag_back);
  vec2 uv_front = apply_anim(frag_uv, frag_front);

  final_color = blend_pma(final_color, sample_skin(body_shadow, uv_body, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(body, uv_body, frag_skin_index));

  final_color = blend_pma(final_color, sample_skin(foot_shadow_left, uv_back, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(foot_shadow_right, uv_back, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(foot_left, uv_back, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(foot_right, uv_back, frag_skin_index));

  // Eyes remain directly placed (not animated, except selecting type)
  final_color = blend_pma(final_color, sample_skin(eye_left, frag_uv, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(eye_right, frag_uv, frag_skin_index));

  out_color = final_color;
  // out_color = vec4(final_color.rgb, 1.0);
}
