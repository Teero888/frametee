#version 450

layout(binding = 1) uniform sampler2D atlas_texture;
layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 col = texture(atlas_texture, frag_uv);
  
  // Pre-multiply alpha
  out_color = vec4(col.rgb * col.a, col.a);
}
