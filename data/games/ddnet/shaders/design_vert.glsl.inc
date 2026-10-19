// The map design's vertex stage, built once per kind of layer by the .vert.glsl
// files that include it:
//   QUADS  a quad layer's quads, several to a draw (DDNet's grouped quads): each
//          moved and tinted by its own entry in `quads`; else a tile layer
layout(location=0) in vec2 position;
layout(location=1) in uint packed_color;
layout(location=2) in vec2 auxiliary; // quad pivot, or packed tile index/flags
layout(location=3) in vec2 texcoord;
#define GROUP_QUADS 128
layout(binding=0) uniform Design {
  vec4 view;
  vec4 tint;
  vec4 clip;
  vec4 geometry;
  vec4 animation;
  vec4 params;
#if QUADS
  vec4 group;                  // x: the draw's first quad in the mesh
  vec4 quads[2 * GROUP_QUADS]; // per quad: offset.xy, cos, sin; then its tint
#endif
} u;
// (a 2D draw has no perspective to correct for)
layout(location=0) noperspective out vec2 uv;
#if QUADS
layout(location=1) noperspective out vec4 color;
#else
layout(location=3) flat out uint tile;
// A tile's own corner in its image, flipped and rotated as the tile asks: the
// same transform at every corner, so the fragments in between need none.
layout(location=4) noperspective out vec2 tile_uv;
#endif
void main() {
#if QUADS
  // (four vertices to a quad, numbered through the whole mesh)
  int k = gl_VertexIndex / 4 - int(u.group.x);
  vec4 animation = u.quads[2 * k];
  vec2 p = auxiliary + mat2(animation.z, animation.w, -animation.w, animation.z) * (position - auxiliary) + animation.xy;
  gl_Position = vec4((p - u.view.xy) * u.view.zw, 0, 1);
  uv = texcoord;
  color = unpackUnorm4x8(packed_color) * u.quads[2 * k + 1];
#else
  vec2 p = position * u.geometry.zw + u.geometry.xy;
  gl_Position = vec4((p - u.view.xy) * u.view.zw, 0, 1);
  uv = texcoord * u.geometry.zw;
  tile = uint(auxiliary.x);
  // DDNet rotates the UV corners after flipping them: the inverse mapping
  // from a pixel to the source image therefore rotates before flipping.
  vec2 t = texcoord;
  uint flags = tile >> 8;
  if((flags & 8u) != 0u) t = vec2(t.y, 1 - t.x);
  if((flags & 1u) != 0u) t.x = 1 - t.x;
  if((flags & 2u) != 0u) t.y = 1 - t.y;
  tile_uv = t;
#endif
}
