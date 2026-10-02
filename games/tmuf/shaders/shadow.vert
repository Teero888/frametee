#version 450

// The car's shadow (tmuf_shadow.c): the caster drawn into the shadow
// picture's own coordinates, or a receiver drawn again over the frame with
// its coordinates in that picture, its fade along the light and whether it
// faces away from the sun.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uint in_normal; // 10:10:10 signed, world space (receivers)

layout(std140, binding = 0) uniform shadow_ubo {
  mat4 view_proj; // the caster: model -> the picture; receivers: the view's
  mat4 to_map;    // world -> the picture's clip space
  vec4 light_dir; // where the sun's light goes
  vec4 ramp;      // the fade's coordinate: dot(ramp.xyz, world) + ramp.w
  vec4 params;    // x: 1 for the caster, y: its grey
}
ubo;

layout(location = 0) out vec3 v_map; // the picture's clip x, y, w (the lamps' are in perspective)
layout(location = 1) out float v_ramp;
layout(location = 2) out float v_away;

void main() {
  vec4 p = vec4(in_pos, 1.0);
  gl_Position = ubo.view_proj * p;
  vec4 m = ubo.to_map * p;
  v_map = m.xyw;
  v_ramp = dot(ubo.ramp.xyz, in_pos) + ubo.ramp.w;
  ivec3 n = ivec3(int(in_normal << 22) >> 22, int(in_normal << 12) >> 22, int(in_normal << 2) >> 22);
  // faces turned from the sun are already dark: no more (1 - sat(3 N.-L))
  // (the blob: none, its oD0 is 0)
  v_away = ubo.params.z > 0.5 ? 0.0 : 1.0 - clamp(3.0 * dot(vec3(n) / 511.0, -ubo.light_dir.xyz), 0.0, 1.0);
}
