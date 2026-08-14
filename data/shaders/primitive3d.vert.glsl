#version 450

// Primitives a 3D game draws: positions are already in world space, and the
// engine's view-projection is the only transform applied. Depth comes from the
// geometry rather than from a sort key, which is what separates this from the
// 2D primitive path.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(std140, binding = 0) uniform primitive_ubo {
  vec2 cam_pos;
  float zoom;
  float aspect;
  float max_map_size;
  mat4 proj;
  vec2 map_size;
  float lod_bias;
  float pad;
  mat4 view_proj;
}
ubo;

layout(location = 0) out vec4 frag_color;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  frag_color = in_color;
}
