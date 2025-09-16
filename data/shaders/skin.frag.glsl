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
vec4 sample_skin(part_info_t part, vec2 frag_uv, int skin_index) {
  if (frag_uv.x < part.place_offset.x || frag_uv.x > part.place_offset.x + part.place_size.x ||
      frag_uv.y < part.place_offset.y || frag_uv.y > part.place_offset.y + part.place_size.y) {
    return vec4(0.0);
  }
  vec2 local_uv = (frag_uv - part.place_offset) / part.place_size;
  vec2 atlas_full = vec2(256.0, 128.0);
  vec2 uv = vec2(part.atlas_offset) / atlas_full + local_uv * (vec2(part.atlas_size) / atlas_full);
  vec4 c = texture(skins, vec3(uv, float(skin_index)));
  c.rgb *= c.a;
  return c;
}

vec4 blend_pma(vec4 dst, vec4 src) {
  return vec4(src.rgb + dst.rgb * (1.0 - src.a), src.a + dst.a * (1.0 - src.a));
}

vec2 apply_anim(vec2 uv, vec3 anim, part_info_t part) {
  vec2 offs = vec2(-anim.x / 64.0, -anim.y / 64.0);
  vec2 center = part.place_offset + part.place_size * 0.5 - offs;
  uv -= center;
  uv *= rot(anim.z * 2.0 * 3.14159265);
  uv += center;
  return uv + offs;
}

void main() {
  part_info_t foot = part_info_t(ivec2(192, 32), ivec2(64, 32), vec2(0.0, 0.25), vec2(1.0, 0.5));
  part_info_t foot_shadow = part_info_t(ivec2(192, 64), ivec2(64, 32), vec2(0.0, 0.25), vec2(1.0, 0.5));

  part_info_t body = part_info_t(ivec2(0, 0), ivec2(96, 96), vec2(0.0), vec2(1.0, 1.0));
  part_info_t body_shadow = part_info_t(ivec2(96, 0), ivec2(96, 96), vec2(0.0), vec2(1.0, 1.0));

  // dead eyes would be ivec2(192,96) but they are unused
  ivec2 eye_offsets[6] = ivec2[6](ivec2(64, 96),  // normal eyes
                                  ivec2(96, 96),  // angry
                                  ivec2(128, 96), // hurt
                                  ivec2(160, 96), // happy
                                  ivec2(64, 96),  // blink
                                  ivec2(224, 96)  // suprise
  );

  vec2 offset = vec2(frag_dir.x * 0.125f, -0.05f + frag_dir.y * 0.10f);
  // on ddnet it's 0.075f but it doesn't look exactly the same here.
  vec2 eye = vec2(0.07f - 0.010f * abs(frag_dir.x), 0.0);

  vec2 size = vec2(0.4);
  // blink
  if (frag_eye - 6 == 4)
    size.y = 0.15;
  part_info_t eye_right =
      part_info_t(eye_offsets[frag_eye - 6], ivec2(32, 32), vec2(0.5) - size * 0.5 + eye + offset, size);
  part_info_t eye_left =
      part_info_t(eye_offsets[frag_eye - 6], ivec2(32, 32), vec2(0.5) - size * 0.5 - eye + offset, size);

  // vec4 final_color = vec4(frag_uv.rg, 0.0, 1.0);
  vec4 final_color = vec4(0.0);

  vec2 uv_body = apply_anim(frag_uv, frag_body, body);
  vec2 uv_back = apply_anim(frag_uv, frag_back, foot);
  vec2 uv_front = apply_anim(frag_uv, frag_front, foot);

  final_color = blend_pma(final_color, sample_skin(foot_shadow, uv_back, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(foot_shadow, uv_front, frag_skin_index));

  final_color = blend_pma(final_color, sample_skin(foot, uv_back, frag_skin_index));

  final_color = blend_pma(final_color, sample_skin(body_shadow, uv_body, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(body, uv_body, frag_skin_index));

  final_color = blend_pma(final_color, sample_skin(eye_right, uv_body, frag_skin_index));
  final_color = blend_pma(final_color, sample_skin(eye_left, uv_body, frag_skin_index));

  final_color = blend_pma(final_color, sample_skin(foot, uv_front, frag_skin_index));

  out_color = final_color;
}
