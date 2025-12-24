#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform primitive_ubo {
  vec2 cam_pos;
  float zoom;
  float aspect;
  float max_map_size;
  mat4 proj;
  vec2 map_size;
  float lod_bias;
}
ubo;

void main() {
  vec2 norm = inPosition / ubo.map_size;
  vec2 rel = (norm - ubo.cam_pos) * (ubo.zoom * ubo.max_map_size);
  rel.y *= ubo.aspect;
  gl_Position = ubo.proj * vec4(rel, 0.0, 1.0);
  fragColor = inColor;
}
