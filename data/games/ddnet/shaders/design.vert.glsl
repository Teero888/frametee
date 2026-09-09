#version 450
layout(location=0) in vec2 position;
layout(location=1) in uint packed_color;
layout(location=2) in vec2 auxiliary; // quad pivot, or packed tile index/flags
layout(location=3) in vec2 texcoord;
layout(binding=0) uniform Design {
  vec4 view;
  vec4 tint;
  vec4 clip;
  vec4 geometry;
  vec4 animation;
  vec4 params;
} u;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;
layout(location=2) out vec2 screen;
layout(location=3) flat out uint tile;
void main() {
  bool tiles = u.params.w != 0.0;
  vec2 p = position;
  if(!tiles) {
    vec2 d = p - auxiliary;
    p = auxiliary + mat2(u.params.x, u.params.y, -u.params.y, u.params.x) * d + u.animation.xy;
  }
  p = p * u.geometry.zw + u.geometry.xy;
  screen = (p - u.view.xy) * u.view.zw;
  gl_Position = vec4(screen, 0, 1);
  uv = texcoord * (tiles ? u.geometry.zw : vec2(1));
  color = unpackUnorm4x8(packed_color) * u.tint;
  tile = tiles ? uint(auxiliary.x) : 0u;
}
