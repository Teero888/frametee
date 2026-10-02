#version 450

// Techno's sea pixel shader (SeaTransPC3): what is under the water (drawn
// apart, in the water's fog: track.frag) seen through the waves, and the
// reflection (the scenery drawn from the mirrored camera: tmuf_scene.c) by
// the fresnel. Without the reflection map (the game has none on cards
// without 2x multisampling) it is black: the sea darkens with the angle.
layout(std140, binding = 0) uniform water_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 params;
  vec4 params2; // x: time of day (the fog table's column), y: 1 with a reflection
  mat4 reflect_view_proj; // the mirrored camera's
  mat4 refract_view_proj; // the refraction's: the view's, its tangents widened 1.05
}
ubo;
layout(binding = 1) uniform sampler2D map_normal;  // signed, offset by 0.5
layout(binding = 2) uniform sampler2D map_fresnel;
layout(binding = 3) uniform sampler2D map_refrac;  // what is under the water (its own target, mipmapped)
layout(binding = 4) uniform sampler2D map_water_fog; // the sea's fog (track.frag)
layout(binding = 5) uniform sampler2D map_reflect;   // the reflection

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_bump0;
layout(location = 2) in vec2 v_bump1;
layout(location = 3) in vec3 v_pos_to_eye;
layout(location = 0) out vec4 out_color;

void main() {
  // (128 + 127 n: tmuf_scene.c's sea_normal_index)
  vec3 n = (texture(map_normal, v_bump0).xyz + texture(map_normal, v_bump1).xyz) * (255.0 / 127.0) - 2.0 * 128.0 / 127.0;
  n = normalize(n);
  vec3 pos_to_eye = normalize(v_pos_to_eye);
  vec3 refrac = -pos_to_eye;
  refrac.xz += n.xz * ubo.params.w;
  // a direction projects to where it points on screen (the eye's own
  // position projects to the vanishing point)
  vec4 clip = ubo.refract_view_proj * vec4(refrac, 0.0);
  // what is under the water, else the refraction's clear colour
  vec3 under = texture(map_refrac, clip.xy / clip.w * 0.5 + 0.5).rgb;
  float eye_dot_n = dot(pos_to_eye, n);
  float fresnel = texture(map_fresnel, vec2(eye_dot_n)).r;
  vec3 reflect_color = vec3(0.0);
  if (ubo.params2.y > 0.5) {
    // the reflected direction projected by the mirrored camera (the sign
    // cancels in the division), as the refraction's
    vec3 r = pos_to_eye - 2.0 * dot(pos_to_eye, n) * n;
    vec4 c = ubo.reflect_view_proj * vec4(r, 0.0);
    float bias = min(3e-6 * dot(v_pos_to_eye, v_pos_to_eye), 2.7);
    reflect_color = texture(map_reflect, c.xy / c.w * 0.5 + 0.5, bias).rgb;
  }
  out_color = vec4(mix(under, reflect_color, fresnel) * v_color.rgb, 0.0);
}
