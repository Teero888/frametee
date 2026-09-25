#version 450
layout(binding=0, std140) uniform Presentation { vec4 scale; } presentation;
layout(binding=1) uniform sampler2D rendered_scene;
layout(location=0) in vec2 texcoord;
layout(location=0) out vec4 color;
void main() { color=texture(rendered_scene,texcoord*presentation.scale.xy); }
