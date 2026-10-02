#version 450

// The lightmap bake's receivers (lightmap_bake_spec.md §2): every
// lightmapped surface drawn where it is in the lightmap atlas (its
// PreLightGen coordinates), with its world position and normal.
layout(std140, binding = 0) uniform bake_ubo {
  vec4 x_axis, y_axis, z_axis;
  vec4 centre;
  vec4 extent;
  vec4 params;
}
ubo;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_lm;
layout(location = 2) in uint in_normal; // 10:10:10 signed, world space

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;

void main() {
  ivec3 c = ivec3(int(in_normal << 22) >> 22, int(in_normal << 12) >> 22, int(in_normal << 2) >> 22);
  v_normal = vec3(c) / 511.0;
  v_world = in_pos;
  // the atlas: row 0 at lm.y 0
  gl_Position = vec4(in_lm * 2.0 - 1.0, 0.5, 1.0);
}
