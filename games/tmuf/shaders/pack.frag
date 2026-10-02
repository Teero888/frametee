#version 450
// The frame's depth, handed to the engine beside its colour (tmuf_gpu.c's
// depth export): the reversed depth's float bits in the four bytes of an
// RGBA8 texel, read back exactly (shaders/present.frag).
#ifdef MULTISAMPLED
layout(binding = 1) uniform sampler2DMS depth;
#else
layout(binding = 1) uniform sampler2D depth;
#endif
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
#ifdef MULTISAMPLED
  const float d = texelFetch(depth, ivec2(gl_FragCoord.xy) - ivec2(textureSize(depth).x, 0), 0).r;
#else
  const float d = texelFetch(depth, ivec2(gl_FragCoord.xy) - ivec2(textureSize(depth, 0).x, 0), 0).r;
#endif
  color = unpackUnorm4x8(floatBitsToUint(d));
}
