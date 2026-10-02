#version 450

// The main view's depth prepass (tmuf_scene.c): the position alone, as
// track.vert and block.vert compute it (invariant: the same depth, which
// their colour pass then tests equal), from their uniforms' first member.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;

layout(std140, binding = 0) uniform depth_ubo {
  mat4 view_proj;
}
ubo;

void main() { gl_Position = ubo.view_proj * vec4(in_pos, 1.0); }
