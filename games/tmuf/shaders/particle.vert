#version 450

// The car's particles (tmuf_particles.c): world-space vertices built on the
// CPU every frame, as the game's dynamic sprites and strips (vs_19/vs_21/
// vs_51: the camera's transform, the vertex colour, the layers' uv).
layout(std140, binding = 0) uniform particle_ubo {
  mat4 view_proj;
  vec4 mode;    // x: 0 picture x colour, 1 the spray's stem, 2 the splash foam, 3 the leaves
  vec4 uv0_m;   // the first layer's uv transform (a b c d), then its translation
  vec4 uv0_t;
  vec4 uv1_m;   // the second layer's
  vec4 uv1_t;
}
ubo;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_color;

layout(location = 0) out vec2 v_uv0;
layout(location = 1) out vec2 v_uv1;
layout(location = 2) out vec4 v_color;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  // (a b c d, tx ty as track.vert's: u' = a u + c v + tx, v' = b u + d v + ty)
  v_uv0 = vec2(ubo.uv0_m.x * in_uv.x + ubo.uv0_m.z * in_uv.y, ubo.uv0_m.y * in_uv.x + ubo.uv0_m.w * in_uv.y) + ubo.uv0_t.xy;
  v_uv1 = vec2(ubo.uv1_m.x * in_uv.x + ubo.uv1_m.z * in_uv.y, ubo.uv1_m.y * in_uv.x + ubo.uv1_m.w * in_uv.y) + ubo.uv1_t.xy;
  v_color = unpackUnorm4x8(in_color);
}
