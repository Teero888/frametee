#version 450

// base quad vertex (-1 to 1)
layout(location = 0) in vec2 in_pos;

// per glyph (text_instance_t in dd_text.c)
layout(location = 1) in vec2 instance_pos;  // world-space centre
layout(location = 2) in vec2 instance_size; // world-space size
layout(location = 3) in vec2 instance_uv;   // how much of its layer the glyph's sprite covers
layout(location = 4) in int instance_layer;
layout(location = 5) in vec4 instance_fill;
layout(location = 6) in vec4 instance_outline;

layout(binding = 0) uniform primitive_ubo {
  vec2 cam_pos;
  float zoom;
  float aspect;
  float max_map_size;
  mat4 proj;
  vec2 map_size;
  float lod_bias;
}
ubo;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out int frag_layer;
layout(location = 2) flat out vec4 frag_fill;
layout(location = 3) flat out vec4 frag_outline;
layout(location = 4) flat out float frag_lod_bias;

void main() {
  vec2 norm = (instance_pos + in_pos * instance_size * 0.5) / ubo.map_size;
  vec2 rel = (norm - ubo.cam_pos) * (ubo.zoom * ubo.max_map_size);
  rel.y *= ubo.aspect;
  gl_Position = ubo.proj * vec4(rel, 0.0, 1.0);

  // the sprite sits in its layer's top-left corner
  frag_uv = (in_pos * 0.5 + 0.5) * instance_uv;
  frag_layer = instance_layer;
  frag_fill = instance_fill;
  frag_outline = instance_outline;
  frag_lod_bias = ubo.lod_bias;
}
