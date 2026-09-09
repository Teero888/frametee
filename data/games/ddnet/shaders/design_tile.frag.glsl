#version 450
layout(binding=0) uniform Design {
  vec4 view; vec4 tint; vec4 clip; vec4 geometry; vec4 animation; vec4 params;
} u;
layout(binding=1) uniform sampler2DArray image;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=2) in vec2 screen;
layout(location=3) flat in uint tile;
layout(location=0) out vec4 output_color;
void main() {
  vec2 dx = dFdx(uv) * exp2(u.params.z), dy = dFdy(uv) * exp2(u.params.z);
  if(any(lessThan(screen, u.clip.xy)) || any(greaterThanEqual(screen, u.clip.zw))) discard;
  vec2 p = fract(uv);
  uint flags = tile >> 8;
  // DDNet rotates the UV corners after flipping them: the inverse mapping
  // from a pixel to the source image therefore rotates before flipping.
  if((flags & 8u) != 0u) { p = vec2(p.y, 1-p.x); dx = vec2(dx.y,-dx.x); dy = vec2(dy.y,-dy.x); }
  if((flags & 1u) != 0u) { p.x = 1-p.x; dx.x = -dx.x; dy.x = -dy.x; }
  if((flags & 2u) != 0u) { p.y = 1-p.y; dx.y = -dx.y; dy.y = -dy.y; }
  vec4 c = textureGrad(image, vec3(p, tile & 255u), dx, dy) * color;
  output_color = vec4(c.rgb * c.a, c.a);
}
