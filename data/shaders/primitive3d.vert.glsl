#version 450

// Primitives a 3D game draws: positions are already in world space, and the
// engine's view-projection is the only transform applied. Depth comes from the
// geometry rather than from a sort key, which is what separates this from the
// 2D primitive path.
//
// `in_layer` selects a page of the bound texture array, or is negative when the
// triangle carries no texture. Textured and untextured geometry share the one
// stream so the depth buffer resolves them against each other.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_layer;

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
layout(location = 1) out vec2 frag_uv;
layout(location = 2) flat out float frag_layer;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  frag_color = in_color;
  frag_uv = in_uv;
  frag_layer = in_layer;
}
