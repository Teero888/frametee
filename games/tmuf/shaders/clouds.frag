#version 450

// CloudsPC2's pixel shaders: the clouds' picture by the vertex colour, the
// sun's flare colour where light passes through the clouds toward the sun
// (the occlusion picture), or, drawing that picture, the transmittance and
// then its mask.
layout(std140, binding = 0) uniform clouds_ubo {
  mat4 view_proj;
  vec4 flare;
  vec4 params;
}
ubo;
layout(binding = 1) uniform sampler2D clouds;
layout(binding = 2) uniform sampler2D occlusion; // white, darkened by the clouds toward the sun

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_occ;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 t = texture(clouds, v_uv);
  // the occlusion picture's last draw: multiplied by CloudsMask (in clouds)
  if (ubo.params.x > 1.5) {
    out_color = t;
    return;
  }
  if (ubo.params.x > 0.5) {
    out_color = vec4(0.0, 0.0, 0.0, v_color.a * t.a * ubo.params.y);
    return;
  }
  vec4 c = clamp(v_color * t, 0.0, 1.0);
  float passing = 0.0;
  if (v_occ.w > 0.0)
    passing = clamp(texture(occlusion, v_occ.xy / v_occ.w * 0.5 + 0.5).r * v_occ.z, 0.0, 1.0);
  out_color = vec4(mix(c.rgb, ubo.flare.rgb, passing), c.a);
}
