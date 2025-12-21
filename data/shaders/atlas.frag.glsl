#version 450

layout(binding = 1) uniform sampler2DArray tex_array;
layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_sprite_index;
layout(location = 2) flat in vec4 frag_uv_limits;
layout(location = 4) flat in float frag_lod_bias;
layout(location = 5) in vec4 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 local_uv = fract(frag_uv);
  vec2 final_uv = frag_uv_limits.xy + local_uv * frag_uv_limits.zw;

  float bias_scale = exp2(frag_lod_bias);
  vec2 ddx = dFdx(frag_uv) * frag_uv_limits.zw * bias_scale;
  vec2 ddy = dFdy(frag_uv) * frag_uv_limits.zw * bias_scale;

  vec4 col = textureGrad(tex_array, vec3(final_uv, float(frag_sprite_index)), ddx, ddy);
  col *= frag_color;
  out_color = vec4(col.rgb * col.a, col.a);
}
