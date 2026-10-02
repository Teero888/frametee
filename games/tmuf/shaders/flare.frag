#version 450
// the flare's picture by its colour, added (ONE / ONE)
layout(binding = 1) uniform sampler2D picture;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(texture(picture, v_uv).rgb * v_color.rgb, 1.0); }
