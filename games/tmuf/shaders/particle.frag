#version 450

// The car's particles, as the game's fixed-function stages draw them (no
// light, no fog): the smoke, gravel and marks-like sprites MODULATE(picture,
// colour); the spray's stem the colour with alpha (layer 0 + layer 1) x the
// colour's; the splash foam the colour with alpha layer 0 x layer 1 x the
// colour's; Rally's falling leaves (tmuf_leaves.c) layer 0 x layer 1's colour
// x the colour. Alpha tested != 0.
layout(std140, binding = 0) uniform particle_ubo {
  mat4 view_proj;
  vec4 mode;
  vec4 uv0_m;
  vec4 uv0_t;
  vec4 uv1_m;
  vec4 uv1_t;
}
ubo;
layout(binding = 1) uniform sampler2D layer0;
layout(binding = 2) uniform sampler2D layer1;

layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 t0 = texture(layer0, v_uv0);
  vec4 c;
  int mode = int(ubo.mode.x + 0.5);
  if (mode == 1) {
    c = vec4(v_color.rgb, clamp(t0.a + texture(layer1, v_uv1).a, 0.0, 1.0) * v_color.a);
  } else if (mode == 2) {
    c = vec4(v_color.rgb, t0.a * texture(layer1, v_uv1).a * v_color.a);
  } else if (mode == 3) {
    c = t0 * vec4(texture(layer1, v_uv1).rgb, 1.0) * v_color; // Rally's leaves: MODULATE twice
  } else {
    c = t0 * v_color;
  }
  if (c.a < 0.5 / 255.0) discard;
  out_color = c;
}
