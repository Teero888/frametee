#version 450
// the casters' depth; alpha-tested ones cut where their picture's alpha is under 0.2
layout(std140, binding = 0) uniform bake_ubo {
  vec4 x_axis, y_axis, z_axis;
  vec4 centre;
  vec4 extent;
  vec4 params;
  vec4 persp;
  vec4 spot_pos, spot_dir, spot_rgb;
}
ubo;
layout(binding = 1) uniform sampler2D picture;
layout(location = 0) in float v_depth;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_depth;
void main() {
  if (ubo.params.x > 0.0 && texture(picture, v_uv).a < ubo.params.x) discard;
  out_depth = vec4(v_depth, 0.0, 0.0, 1.0);
}
