#version 450

// Techno2's fence pixel shader: the lawn's colour through the blades'
// picture, cut out where the blades are not, fade with the angle and where no
// lawn is behind (the ground frame's alpha).
layout(std140, binding = 0) uniform fence_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 params;
}
ubo;
layout(binding = 1) uniform sampler2D map_ground; // the frame after the grounds
layout(binding = 2) uniform sampler2D map_fence;
layout(binding = 3) uniform sampler2D map_fade_xz;

layout(location = 0) in vec4 v_ground;
layout(location = 1) in vec2 v_fence;
layout(location = 2) in vec2 v_fade_xw;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 ground = texture(map_ground, v_ground.xy / v_ground.w * 0.5 + 0.5);
  vec4 fence = texture(map_fence, v_fence);
  vec3 color = clamp(ground.rgb * fence.rgb * 2.0, 0.0, 1.0);
  float fade = texture(map_fade_xz, vec2(v_fade_xw.x / v_fade_xw.y, 0.5)).x;
  float a = clamp(fence.a - fade, 0.0, 1.0) * clamp(ground.a * 2.0 - 1.0, 0.0, 1.0);
  if (a <= 0.0) discard; // the game's alpha test (greater than 0)
  out_color = vec4(color, a);
}
