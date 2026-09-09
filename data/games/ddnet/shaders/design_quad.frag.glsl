#version 450
layout(binding=0) uniform Design {
  vec4 view; vec4 tint; vec4 clip; vec4 geometry; vec4 animation; vec4 params;
} u;
layout(binding=1) uniform sampler2D image;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=2) in vec2 screen;
layout(location=3) flat in uint tile;
layout(location=0) out vec4 output_color;
void main() {
  vec2 dx = dFdx(uv) * exp2(u.params.z), dy = dFdy(uv) * exp2(u.params.z);
  if(any(lessThan(screen, u.clip.xy)) || any(greaterThanEqual(screen, u.clip.zw))) discard;
  vec4 c = textureGrad(image, uv, dx, dy) * color;
  output_color = vec4(c.rgb * c.a, c.a);
}
