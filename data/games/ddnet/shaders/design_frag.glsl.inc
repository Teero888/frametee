// The map design's fragment stage, built once per variant by the .frag.glsl
// files that include it, so each draws with only the work its layer needs:
//   TILE    a tile layer, sampling the layer's 256-tile array (else a quad's image)
//   BORDER  with TILE: a border batch, one tile stretched over many and repeated
// A group that clips is drawn with a scissor, which costs the pixels nothing.
layout(binding=0) uniform Design {
  vec4 view; vec4 tint; vec4 clip; vec4 geometry; vec4 animation; vec4 params;
} u;
#if TILE
layout(binding=1) uniform sampler2DArray image;
#else
layout(binding=1) uniform sampler2D image;
#endif
layout(location=0) noperspective in vec2 uv;
#if !TILE
layout(location=1) noperspective in vec4 color;
#endif
#if TILE
layout(location=3) flat in uint tile;
layout(location=4) noperspective in vec2 tile_uv;
#endif
layout(location=0) out vec4 output_color;

#if TILE
// One tile per quad: its corners already hold the flipped image.
vec4 sample_tile() { return texture(image, vec3(tile_uv, tile & 255u), u.params.z); }

// A border stretches one tile over many, repeating it, and the repeat breaks
// the derivatives the sampler would take from the coordinates.
vec4 sample_border() {
  vec2 dx = dFdx(uv) * exp2(u.params.z), dy = dFdy(uv) * exp2(u.params.z);
  vec2 p = fract(uv);
  uint flags = tile >> 8;
  // DDNet rotates the UV corners after flipping them: the inverse mapping
  // from a pixel to the source image therefore rotates before flipping.
  if((flags & 8u) != 0u) { p = vec2(p.y, 1-p.x); dx = vec2(dx.y,-dx.x); dy = vec2(dy.y,-dy.x); }
  if((flags & 1u) != 0u) { p.x = 1-p.x; dx.x = -dx.x; dy.x = -dy.x; }
  if((flags & 2u) != 0u) { p.y = 1-p.y; dx.y = -dx.y; dy.y = -dy.y; }
  return textureGrad(image, vec3(p, tile & 255u), dx, dy);
}
#endif

void main() {
  // A tile's colour is its layer's, the same over the whole draw; a quad's
  // corners have their own. Blending is straight (the pipeline multiplies by
  // alpha), so the colour goes out as it is.
#if TILE && BORDER
  output_color = sample_border() * u.tint;
#elif TILE
  output_color = sample_tile() * u.tint;
#else
  output_color = texture(image, uv, u.params.z) * color;
#endif
}
