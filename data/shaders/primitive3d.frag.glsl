#version 450

// The 3D path is unlit: a fragment is its vertex colour times its texel, and
// nothing else. Depth is what resolves the world; shading is left to whatever
// the game baked into the two.
layout(binding = 1) uniform sampler2DArray tex_array;

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in float frag_layer;
layout(location = 0) out vec4 out_color;

void main() {
  bool additive = frag_color.a <= 0.0;

  vec4 col = frag_color;
  if (frag_layer >= 0.0) {
    col *= texture(tex_array, vec3(frag_uv, frag_layer));
  }

  if (additive) {
    if (max(max(col.r, col.g), col.b) < 0.02) discard;
    out_color = vec4(col.rgb, 0.0);
    return;
  }

  if (col.a < 0.02) discard;

  out_color = vec4(col.rgb * col.a, col.a);
}
