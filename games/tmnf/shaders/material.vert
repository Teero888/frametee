#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec2 uv1;
layout(location=4) in vec4 color;
layout(binding=0, std140) uniform Frame {
  mat4 view_projection;
  mat4 reflection_projection;
  mat4 shadow_projection;
  vec4 eye_time;
  vec4 sun_direction;
  vec4 sun_color;
  vec4 ambient;
  vec4 fog_color;
  vec4 clip_plane;
  vec4 viewport;
} frame;
layout(location=0) out vec3 world_position;
layout(location=1) out vec3 world_normal;
layout(location=2) out vec2 texcoord;
layout(location=3) out vec2 occlusion_coord;
layout(location=4) out vec4 vertex_color;
void main() {
  gl_Position = frame.view_projection * vec4(position, 1);
  world_position = position;
  world_normal = normal;
  texcoord = uv;
  occlusion_coord = uv1;
  vertex_color = color;
}
