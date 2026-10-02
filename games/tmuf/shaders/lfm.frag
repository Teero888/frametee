#version 450

// the lightmap, fading to white 5 to 10 m under the car (LightFromMap_VisualToWhite)
layout(std140, binding = 0) uniform lfm_ubo {
  mat4 to_clip;
  vec4 depth_row;
  vec4 fade;
}
ubo;
layout(binding = 1) uniform sampler2D lightmap;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_depth;
layout(location = 0) out vec4 out_color;

void main() {
  float w = clamp((v_depth - ubo.fade.x) / ubo.fade.y, 0.0, 1.0);
  out_color = vec4(mix(texture(lightmap, v_uv).rgb, vec3(1.0), w), 1.0);
}
