#version 450
// the glow by the light's colour, alpha 0; blended DESTALPHA / ONE: added by
// the gloss the ground left in the frame's alpha
layout(binding = 1) uniform sampler2D picture;
layout(location = 0) noperspective in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(texture(picture, v_uv).r * v_color.rgb, 0.0); }
