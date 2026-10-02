#version 450

// The caster's flat grey, or a receiver's multiplier: the fixed-function
// stages ADD(shadow, away) then ADD(ramp, that), saturated, where the blurred
// silhouette is (its alpha above 0).
layout(std140, binding = 0) uniform shadow_ubo {
  mat4 view_proj;
  mat4 to_map;
  vec4 light_dir;
  vec4 ramp;
  vec4 params;
}
ubo;
layout(binding = 1) uniform sampler2D shadow_map; // or the blob's picture
layout(binding = 2) uniform sampler2D fade; // the game's 512 x 1 ramp

layout(location = 0) in vec3 v_map;
layout(location = 1) in float v_ramp;
layout(location = 2) in float v_away;
layout(location = 0) out vec4 out_color;

void main() {
  if (ubo.params.x > 0.5) {
    out_color = vec4(vec3(ubo.params.y), 1.0);
    return;
  }
  if (v_map.z <= 0.0) discard; // (behind a lamp)
  vec4 s = texture(shadow_map, v_map.xy / v_map.z * 0.5 + 0.5);
  if (s.a <= 0.0) discard;
  vec3 c = clamp(clamp(s.rgb + v_away, 0.0, 1.0) + texture(fade, vec2(v_ramp, 0.5)).rgb, 0.0, 1.0);
  out_color = vec4(c, 1.0);
}
