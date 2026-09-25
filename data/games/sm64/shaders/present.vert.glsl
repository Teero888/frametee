#version 450
// The game's frame over the whole viewport. At depth 0, the far plane of the
// engine's reversed depth, so everything the engine draws in 3D is in front.
layout(location = 0) in vec2 in_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec2 uv;
void main() { gl_Position = vec4(in_pos, 0.0, 1.0); uv = in_uv; }
