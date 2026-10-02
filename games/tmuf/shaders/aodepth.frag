#version 450

// The depth prepass (tmuf_ssao.c): MapDepth = (view depth - zMin) / (zMax -
// zMin), packed in 24 bits; the alpha tested ones' holes left out
layout(std140, binding = 0) uniform ao_depth_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 forward;
  vec4 range;
}
ubo;
layout(binding = 1) uniform sampler2D picture;

layout(location = 0) in float v_depth;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
  if (ubo.forward.w > 0.0 && texture(picture, v_uv).a < ubo.forward.w) discard;
  float d = clamp(v_depth, 0.0, 0.99999);
  vec3 enc = fract(d * vec3(1.0, 255.0, 65025.0));
  enc.xy -= enc.yz / 255.0;
  out_color = vec4(enc, 1.0);
}
