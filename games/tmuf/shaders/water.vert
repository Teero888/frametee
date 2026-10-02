#version 450

// Techno's sea (TmForever's *.VHlsl): two normal maps scrolling over world xz.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uint in_color; // BGRA

layout(std140, binding = 0) uniform water_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 params; // x: time (s), y: BumpScaleUV, z: BumpSpeedUV, w: RefracPertubPC3
  vec4 params2; // x: time of day, y: 1 with a reflection
  mat4 reflect_view_proj;
  mat4 refract_view_proj;
}
ubo;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_bump0;
layout(location = 2) out vec2 v_bump1;
layout(location = 3) out vec3 v_pos_to_eye;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  vec2 bump = in_pos.xz * ubo.params.y;
  float t = ubo.params.z * ubo.params.x;
  v_bump0 = bump - t * vec2(1.27389, -0.8975) * 0.5;
  v_bump1 = bump + t;
  v_pos_to_eye = ubo.eye.xyz - in_pos;
  v_color = unpackUnorm4x8(in_color).zyxw;
}
