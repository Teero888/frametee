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
  vec4 col = frag_color;
  // A negative page is how a triangle says it has no texture, which is what
  // every 2D-turned-3D marker, line and box uses.
  if (frag_layer >= 0.0) {
    // Authored world textures tile across a surface, so the coordinate is taken
    // as given and wrapped by the sampler rather than clamped into a sub-rect.
    col *= texture(tex_array, vec3(frag_uv, frag_layer));
  }

  // Everything here is drawn in one unsorted pass that writes depth, so a
  // fully transparent texel would otherwise punch a hole through whatever
  // stands behind it. Cutting those fragments out is also what a fence, a
  // railing or a tree card actually wants: they are alpha-tested surfaces, not
  // blended ones.
  if (col.a < 0.02) discard;

  out_color = vec4(col.rgb * col.a, col.a);
}
