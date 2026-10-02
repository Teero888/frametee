#version 450
// The module's frame over the viewport: its colour (the left half of the
// frame texture) and, for the engine's own 3D drawing (lines, markers) to
// hide behind the scene, its depth (the right half: the reversed depth's
// float bits, shaders/pack.frag) moved from the game's lens into the
// engine's: the point the game drew there, projected by the engine's
// view-projection.
layout(binding = 0) uniform present_ubo {
  mat4 game_to_world; // the inverse of the game's view-projection
  mat4 engine_view_proj;
}
ubo;
layout(binding = 1) uniform sampler2D frame_texture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
  const ivec2 size = textureSize(frame_texture, 0);
  const int width = size.x / 2;
  const ivec2 texel = ivec2(min(uv * vec2(width, size.y), vec2(width - 1, size.y - 1)));
  color = vec4(texelFetch(frame_texture, texel, 0).rgb, 1.0);
  const float d = uintBitsToFloat(packUnorm4x8(texelFetch(frame_texture, texel + ivec2(width, 0), 0)));
  if (!(d > 0.0)) {
    gl_FragDepth = 0.0; // nothing drawn there: the engine's far
    return;
  }
  vec4 world = ubo.game_to_world * vec4(uv * 2.0 - 1.0, d, 1.0);
  world /= world.w;
  const vec4 clip = ubo.engine_view_proj * world;
  gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
