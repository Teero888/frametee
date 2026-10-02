#version 450

// A sun shadow map's caster (tmuf_pssm.c): the scenery's world positions
// into the map's cell of its target, the depth along the sun's light
// (reversed: the nearest to the sun kept)
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(std140, binding = 0) uniform pssm_ubo {
  vec4 rows[3]; // world -> the map's coordinates u, v and depth (dot(xyz, P) + w)
  vec4 cell;    // where the map is in the target: x0, y0, width, height (0..1)
  vec4 params;  // x: the alpha test (0: none), y: the border kept clear (in the map's coordinates)
}
ubo;

layout(location = 0) out vec3 v_tc;
layout(location = 1) out vec2 v_uv;

void main() {
  vec4 p = vec4(in_pos, 1.0);
  v_tc = vec3(dot(ubo.rows[0], p), dot(ubo.rows[1], p), dot(ubo.rows[2], p));
  v_uv = in_uv;
  vec2 t = ubo.cell.xy + v_tc.xy * ubo.cell.zw;
  // the depth with the constant part of the bias (the slope's is the
  // rasterizer's: tm_pssm_cast), the map's range [-1, 16] in [1, 0] (reversed;
  // the receivers undo it: pssm_pcf): not clamped per vertex, so a long
  // triangle beyond the far end keeps its plane where it is in range
  gl_Position = vec4(t * 2.0 - 1.0, 1.0 - (v_tc.z + 1e-5 + 1.0) / 18.0, 1.0);
}
