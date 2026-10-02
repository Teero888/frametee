#version 450

// Grass tufts (Techno2's fence shader, TmForever's *.VHlsl): crossed quads
// whose picture's u runs along world x, drawn with the colour of the lawn
// under them (the frame as it was after the grounds: tg_frame_copy), faded by
// the angle they are seen at. The shader works in the fence's own frame
// (the visual's): its x and z axes come with each vertex.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;     // x: a random offset per quad, y: up the blades
layout(location = 2) in float in_base_y; // the ground the tufts stand on
layout(location = 3) in uint in_axis_x;  // the fence's x and z in the world, 10:10:10 signed
layout(location = 4) in uint in_axis_z;

layout(std140, binding = 0) uniform fence_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 params; // x: TexScaleU, y: SlideStartZ_Y1, z: SlideIntens, w: the level's far distance (GbxTreeMip_MinMaxZ.y)
}
ubo;

layout(location = 0) out vec4 v_ground; // where the lawn under it is on screen (clip space)
layout(location = 1) out vec2 v_fence;
layout(location = 2) out vec2 v_fade_xw;

vec3 unpack_normal(uint n) {
  ivec3 c = ivec3(int(n << 22) >> 22, int(n << 12) >> 22, int(n << 2) >> 22);
  return normalize(vec3(c));
}

void main() {
  vec3 ax = unpack_normal(in_axis_x), az = unpack_normal(in_axis_z);
  vec3 p = in_pos;
  vec3 eye_to_pos = p - ubo.eye.xyz;
  float ex = dot(eye_to_pos, ax), ez = dot(eye_to_pos, az);
  // only a wedge in front of the eye, which limits the fill
  float abs_z = abs(ez);
  float clamp_x = abs_z - ubo.params.w > 0.0 ? 0.0 : 0.5 * abs_z;
  p += ax * (clamp(ex, -clamp_x, clamp_x) - ex);
  v_fence = vec2(ubo.params.x * dot(p, ax) + in_uv.x, in_uv.y);
  // the ground under near vertices, the vertex itself further away
  vec3 on_xz = p;
  on_xz.y = in_base_y + (p.y - in_base_y) * clamp(abs_z - ubo.params.w * 0.5, 0.0, 1.0);
  v_ground = ubo.view_proj * vec4(on_xz, 1.0);
  // the feet slide away from the eye close by
  float start_z = ubo.params.y * abs(eye_to_pos.y);
  float slide = ubo.params.z * clamp(1.0 - abs_z / start_z, 0.0, 1.0);
  if (p.y - in_base_y < 0.01) p += az * (-slide * sign(ez));
  eye_to_pos = p - ubo.eye.xyz;
  ex = dot(eye_to_pos, ax), ez = dot(eye_to_pos, az);
  v_fade_xw = vec2(0.5 * (ez + ex * 2.0), ez);
  gl_Position = ubo.view_proj * vec4(p, 1.0);
}
