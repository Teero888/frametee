// LOVE YOU TATER

#version 450

layout(binding = 1) uniform sampler2DArray skins;

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
layout(location = 11) flat in int frag_col_gs;
layout(location = 12) flat in float frag_lod_bias;

layout(location = 0) out vec4 out_color;

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
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec4 blend_pma(vec4 dst, vec4 src) { return vec4(src.rgb + dst.rgb * (1.0 - src.a), src.a + dst.a * (1.0 - src.a)); }

vec2 apply_anim(vec2 uv, vec3 anim, part_info_t part) {
  vec2 offs = vec2(-anim.x / 64.0, -anim.y / 64.0);
  vec2 center = part.place_offset + part.place_size * 0.5 - offs;
  uv -= center;
  uv *= rot(anim.z * 2.0 * 3.14159265);
  uv += center;
  return uv + offs;
}

vec4 get_part_color(part_info_t part, vec2 frag_uv, int skin_index, bool use_body_color, bool apply_gs_contrast, bool mirror) {
  vec2 local_uv = (frag_uv - part.place_offset) / part.place_size;
  if (mirror) {
    local_uv.x = 1.0 - local_uv.x;
  }

  vec2 atlas_full = vec2(512.0, 512.0);
  vec2 uv_size = vec2(part.atlas_size) / atlas_full;

  vec2 uv_unclamped = (vec2(part.atlas_offset) / atlas_full) + local_uv * uv_size;

  // calculate derivatives on the unclamped UVs to maintain correct Mip Level selection
  vec2 ddx = dFdx(uv_unclamped);
  vec2 ddy = dFdy(uv_unclamped);

  // This check has to be after the derivatives calculation to avoid artifacts at the edges
  vec4 src = textureGrad(skins, vec3(uv_unclamped, float(skin_index)), ddx, ddy);
  if (frag_uv.x <= part.place_offset.x || frag_uv.x >= part.place_offset.x + part.place_size.x || frag_uv.y <= part.place_offset.y ||
      frag_uv.y >= part.place_offset.y + part.place_size.y) {
    return vec4(0.0);
  }
  if (src.a == 0.0) return vec4(0.0);

  // Un-premultiply
  vec3 straight_rgb = src.rgb / src.a;

  if (frag_col_custom != 0) {
    float lum = clamp(luminance(straight_rgb), 0.0, 1.0);
    int v = int(lum * 255.0 + 0.5);
    
    if (apply_gs_contrast) {
      if (v <= frag_col_gs) {
        v = int(float(v) / float(max(1, frag_col_gs)) * 192.0 + 0.5);
      } else {
        v = int(float(v - frag_col_gs) / float(max(1, 255 - frag_col_gs)) * (255 - 192) + 192.0 + 0.5);
      }
    }

    vec3 tint = use_body_color ? frag_col_body : frag_col_feet;
    return vec4(tint * (float(v) / 255.0) * src.a, src.a);
  } else {
    if (!use_body_color) {
      src.rgb *= frag_col_feet.r;
    }
    return src;
  }
}
void main() {
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

  final_color = blend_pma(final_color, get_part_color(foot_shadow, uv_back, frag_skin_index, false, false, false));
  final_color = blend_pma(final_color, get_part_color(body_shadow, uv_body, frag_skin_index, true, false, false));
  final_color = blend_pma(final_color, get_part_color(foot_shadow, uv_front, frag_skin_index, false, false, false));
  final_color = blend_pma(final_color, get_part_color(foot, uv_back, frag_skin_index, false, false, false));
  final_color = blend_pma(final_color, get_part_color(body, uv_body, frag_skin_index, true, true, false));
  final_color = blend_pma(final_color, get_part_color(eye_left, uv_body, frag_skin_index, true, false, false));
  final_color = blend_pma(final_color, get_part_color(eye_right, uv_body, frag_skin_index, true, false, true));
  final_color = blend_pma(final_color, get_part_color(foot, uv_front, frag_skin_index, false, false, false));

  out_color = final_color;
}
