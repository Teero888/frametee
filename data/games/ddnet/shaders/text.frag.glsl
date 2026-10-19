#version 450

// A glyph's coverage: the fill in red, the outline ring around it in green.
layout(binding = 1) uniform sampler2DArray glyphs;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_layer;
layout(location = 2) flat in vec4 frag_fill;
layout(location = 3) flat in vec4 frag_outline;
layout(location = 4) flat in float frag_lod_bias;

layout(location = 0) out vec4 out_color;

// DDNet's text shader: the fill over its outline in one quad, premultiplied.
// What blending leaves behind is exactly the outline drawn first and the fill
// over it, without a second quad per glyph.
void main() {
  vec2 coverage = texture(glyphs, vec3(frag_uv, float(frag_layer)), frag_lod_bias).rg;
  float fill = frag_fill.a * coverage.r;
  float outline = frag_outline.a * coverage.g * (1.0 - fill);
  out_color = vec4(frag_fill.rgb * fill + frag_outline.rgb * outline, fill + outline);
}
