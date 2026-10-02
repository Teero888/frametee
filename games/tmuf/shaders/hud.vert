#version 450
// A picture on screen (tmuf_hud.c): a rectangle in clip space, a part of
// its texture.
layout(std140, binding = 0) uniform hud_ubo {
  vec4 rect; // clip space: x0 y0 x1 y1
  vec4 uv;   // u0 v0 u1 v1
  vec4 tint;
}
ubo;
layout(location = 0) out vec2 v_uv;
// two triangles
const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
void main() {
  int c = corners[gl_VertexIndex % 6];
  vec2 corner = vec2(c & 1, (c >> 1) & 1);
  gl_Position = vec4(mix(ubo.rect.xy, ubo.rect.zw, corner), 0.0, 1.0);
  v_uv = mix(ubo.uv.xy, ubo.uv.zw, corner);
}
