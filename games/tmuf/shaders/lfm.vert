#version 450

// LightFromMap (tmuf_car.c): the static scenery under the car seen from
// above along the car's down axis, orthographically, in the map's lightmap;
// the nearest surface below the car's top wins (depth: 1 at the top).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv_prelight; // in the lightmap atlas

layout(std140, binding = 0) uniform lfm_ubo {
  mat4 to_clip;   // world -> the view (z: 1 at the car's top .. 0 at its depth)
  vec4 depth_row; // world -> depth below the car's origin
  vec4 fade;      // white by sat((depth - x) / y)
}
ubo;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_depth;

void main() {
  gl_Position = ubo.to_clip * vec4(in_pos, 1.0);
  v_uv = in_uv_prelight;
  v_depth = dot(vec4(in_pos, 1.0), ubo.depth_row);
}
