#version 450
// Another group's Mario, drawn alone on a transparent frame (sm64_set_draw_mario_only),
// over this one's: tinted toward the group's colour, at its opacity.
layout(binding = 0) uniform Ghost {
  vec4 tint;
  float opacity;
}
ghost;
layout(binding = 1) uniform sampler2D ghost_texture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
  vec4 c = texture(ghost_texture, uv);
  color = vec4(mix(c.rgb, ghost.tint.rgb, 0.35), c.a * ghost.opacity);
}
