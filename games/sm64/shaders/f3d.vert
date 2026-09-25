#version 450
// A vertex of f3d/f3d.h: OpenGL's clip space to Vulkan's (Y down, Z from 0).
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_uv;
layout(location = 2) in vec4 in_shade;
layout(location = 3) in float in_fog;

layout(location = 0) out vec4 uv;
layout(location = 1) out vec4 shade;
layout(location = 2) out float fog;

void main() {
    gl_Position = vec4(in_position.x, -in_position.y, (in_position.z + in_position.w) * 0.5, in_position.w);
    uv = in_uv;
    shade = in_shade;
    fog = in_fog;
}
