#version 450

// A sun shadow map's caster (tmuf_pssm.c): its depth with the caster's
// bias, packed in the colour (24 bits); nothing outside the map's cell and its border; the alpha
// tested ones' holes left out
layout(std140, binding = 0) uniform pssm_ubo {
  vec4 rows[3];
  vec4 cell;
  vec4 params;
}
ubo;
layout(binding = 1) uniform sampler2D picture;

layout(location = 0) in vec3 v_tc;
layout(location = 1) in vec2 v_uv;

void main() {
  if (ubo.params.x > 0.0 && texture(picture, v_uv).a <= ubo.params.x) discard;
  // the caster's D3D bias (env_light_spec §3.4): DEPTHBIAS 1e-5 and
  // SLOPESCALEDEPTHBIAS 0.75 x the depth's steepest slope per texel
  // (its depth the rasterizer's: z + 1e-5 + 0.75 max(|dz/dx|, |dz/dy|), reversed)
}
