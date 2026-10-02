#version 450

// The depth prepass of the ambient occlusion (tmuf_ssao.c): the receivers'
// view depth, as the game's MapDepth holds it
invariant gl_Position;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(std140, binding = 0) uniform ao_depth_ubo {
  mat4 view_proj;
  vec4 eye;     // xyz
  vec4 forward; // xyz; w: the alpha test (0: none)
  vec4 range;   // x: zMin, y: 1 / (zMax - zMin)
}
ubo;

layout(location = 0) out float v_depth;
layout(location = 1) out vec2 v_uv;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  v_depth = (dot(in_pos - ubo.eye.xyz, ubo.forward.xyz) - ubo.range.x) * ubo.range.y;
  v_uv = in_uv;
}
