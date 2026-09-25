#version 450
layout(location=0) in vec2 position;
layout(location=2) in vec2 uv;
layout(location=0) out vec2 texcoord;
void main() { gl_Position=vec4(position,0,1); texcoord=uv; }
