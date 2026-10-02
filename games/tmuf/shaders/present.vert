#version 450
// The module's frame (tmuf_gpu.h) over the whole viewport, at depth 0: the
// far plane of the engine's reversed depth, so what the engine draws in 3D
// stays in front.
layout(location = 0) in vec2 in_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec2 uv;
void main() {
  gl_Position = vec4(in_pos, 0.0, 1.0);
  uv = in_uv;
}
