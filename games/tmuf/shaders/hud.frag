#version 450
layout(std140, binding = 0) uniform hud_ubo {
  vec4 rect;
  vec4 uv;
  vec4 tint;
}
ubo;
layout(binding = 1) uniform sampler2D picture;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() { out_color = texture(picture, v_uv) * ubo.tint; }
