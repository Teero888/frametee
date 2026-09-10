#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec2 uv;
void main() { gl_Position = vec4(in_pos, 0.999, 1.0); uv = in_uv; }
