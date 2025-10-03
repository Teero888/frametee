#version 450

layout(binding = 1) uniform sampler2DArray tex_array;
layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_sprite_index;
layout(location = 2) in vec2 frag_uv_scale;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 final_uv = frag_uv * frag_uv_scale;
  vec4 col = texture(tex_array, vec3(final_uv, float(frag_sprite_index)));
  // pre-multiply alpha
  out_color = vec4(col.rgb * col.a, col.a);
}
