// LOVE YOU TATER
// TODO: The mipmap edges bleed, but its not really visible under normal coditions, maybe fix that in the future

#version 450

// original colours, rgba premultiplied
layout(binding = 1) uniform sampler2DArray skins;
// Colorable skin
layout(binding = 2) uniform sampler2DArray skins_gray;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_skin_index;
layout(location = 2) flat in int frag_eye;
layout(location = 3) flat in vec3 frag_body;
layout(location = 4) flat in vec3 frag_back;
layout(location = 5) flat in vec3 frag_front;
layout(location = 6) flat in vec3 frag_attach;
layout(location = 7) flat in vec2 frag_dir;
layout(location = 8) flat in vec3 frag_col_body;
layout(location = 9) flat in vec3 frag_col_feet;
layout(location = 10) flat in int frag_col_custom;
layout(location = 11) flat in int frag_mode;
layout(location = 12) flat in float frag_lod_bias;
// Whole-instance fade. Every colour below is premultiplied, so scaling rgb and
// alpha together is the correct way to make one instance translucent.
layout(location = 13) flat in float frag_alpha;

layout(location = 0) out vec4 out_color;

const vec2 ATLAS_SIZE = vec2(512.0, 352.0);

struct part_info_t {
  ivec2 atlas_offset; // px
  ivec2 atlas_size;   // px
  vec2 place_offset;  // 0..1
  vec2 place_size;    // 0..1
};

mat2 rot(float a) {
  float s = sin(a), c = cos(a);
  return mat2(c, s, -s, c);
}

vec4 blend_pma(vec4 dst, vec4 src) { return vec4(src.rgb + dst.rgb * (1.0 - src.a), src.a + dst.a * (1.0 - src.a)); }

vec2 apply_anim(vec2 uv, vec3 anim, part_info_t part) {
  vec2 offs = vec2(-anim.x / 64.0, -anim.y / 64.0);
  vec2 center = part.place_offset + part.place_size * 0.5 - offs;
  uv -= center;
  uv *= rot(anim.z * 2.0 * 3.14159265);
  uv += center;
  return uv + offs;
}

vec4 get_part_color(part_info_t part, vec2 frag_uv, int skin_index, bool use_body_color, bool mirror) {
  vec2 local_uv = (frag_uv - part.place_offset) / part.place_size;
  if (mirror) {
    local_uv.x = 1.0 - local_uv.x;
  }

  vec2 uv_size = vec2(part.atlas_size) / ATLAS_SIZE;
  vec2 uv_unclamped = (vec2(part.atlas_offset) / ATLAS_SIZE) + local_uv * uv_size;

  float bias_scale = exp2(frag_lod_bias);
  vec2 ddx = dFdx(uv_unclamped) * bias_scale;
  vec2 ddy = dFdy(uv_unclamped) * bias_scale;

  if (frag_uv.x <= part.place_offset.x || frag_uv.x >= part.place_offset.x + part.place_size.x ||
      frag_uv.y <= part.place_offset.y || frag_uv.y >= part.place_offset.y + part.place_size.y) {
    return vec4(0.0);
  }

  if (frag_col_custom != 0) {
    vec2 src = textureGrad(skins_gray, vec3(uv_unclamped, float(skin_index)), ddx, ddy).rg;
    if (src.g == 0.0) return vec4(0.0);
    vec3 tint = use_body_color ? frag_col_body : frag_col_feet;
    // src.r is already weight * alpha, so tint * src.r is premultiplied too
    return vec4(tint * src.r, src.g);
  }

  vec4 src = textureGrad(skins, vec3(uv_unclamped, float(skin_index)), ddx, ddy);
  if (src.a == 0.0) return vec4(0.0);
  if (!use_body_color) src.rgb *= frag_col_feet.r;
  return src;
}

void main() {
  if (frag_mode == 1) {
    part_info_t hand = part_info_t(ivec2(280, 208), ivec2(64, 64), vec2(0.0), vec2(1.0));
    part_info_t hand_shadow = part_info_t(ivec2(352, 208), ivec2(64, 64), vec2(0.0), vec2(1.0));
    vec4 c = vec4(0.0);
    c = blend_pma(c, get_part_color(hand_shadow, frag_uv, frag_skin_index, true, false));
    c = blend_pma(c, get_part_color(hand, frag_uv, frag_skin_index, true, false));
    out_color = c * frag_alpha;
    return;
  }

  part_info_t foot = part_info_t(ivec2(8, 208), ivec2(128, 64), vec2(0.0, 0.25), vec2(1.0, 0.5));
  part_info_t foot_shadow = part_info_t(ivec2(144, 208), ivec2(128, 64), vec2(0.0, 0.25), vec2(1.0, 0.5));

  part_info_t body = part_info_t(ivec2(8, 8), ivec2(192, 192), vec2(0.0), vec2(1.0, 1.0));
  part_info_t body_shadow = part_info_t(ivec2(208, 8), ivec2(192, 192), vec2(0.0), vec2(1.0, 1.0));

  ivec2 eye_offsets[6] = ivec2[6](ivec2(8, 280), ivec2(80, 280), ivec2(152, 280), ivec2(224, 280), ivec2(8, 280), ivec2(368, 280));

  vec2 offset = vec2(frag_dir.x * 0.125f, -0.05f + frag_dir.y * 0.10f);
  vec2 eye_h_offset = vec2(0.075f - 0.010f * abs(frag_dir.x), 0.0);

  vec2 size = vec2(0.4);
  if (frag_eye - 6 == 4) size.y = 0.15;

  part_info_t eye_right = part_info_t(eye_offsets[frag_eye - 6], ivec2(64, 64), vec2(0.5) - size * 0.5 + eye_h_offset + offset, size);
  part_info_t eye_left = part_info_t(eye_offsets[frag_eye - 6], ivec2(64, 64), vec2(0.5) - size * 0.5 - eye_h_offset + offset, size);

  vec4 final_color = vec4(0.0);

  vec2 uv_body = apply_anim(frag_uv, frag_body, body);
  vec2 uv_back = apply_anim(frag_uv, frag_back, foot);
  vec2 uv_front = apply_anim(frag_uv, frag_front, foot);

  final_color = blend_pma(final_color, get_part_color(foot_shadow, uv_back, frag_skin_index, false, false));
  final_color = blend_pma(final_color, get_part_color(body_shadow, uv_body, frag_skin_index, true, false));
  final_color = blend_pma(final_color, get_part_color(foot_shadow, uv_front, frag_skin_index, false, false));
  final_color = blend_pma(final_color, get_part_color(foot, uv_back, frag_skin_index, false, false));
  final_color = blend_pma(final_color, get_part_color(body, uv_body, frag_skin_index, true, false));
  final_color = blend_pma(final_color, get_part_color(eye_left, uv_body, frag_skin_index, true, false));
  final_color = blend_pma(final_color, get_part_color(eye_right, uv_body, frag_skin_index, true, true));
  final_color = blend_pma(final_color, get_part_color(foot, uv_front, frag_skin_index, false, false));

  out_color = final_color * frag_alpha;
}
