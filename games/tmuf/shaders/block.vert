#version 450

// A block's surface as the game draws it with a normal map (Techno2's
// PC3 block shader, TmForever's *.VHlsl): world-space vertices here, so the
// game's GbxVisualToWorld is the identity. See block.frag.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;          // Diffuse, Specular, Normal
layout(location = 2) in vec2 in_uv_lighting; // Lighting (baked light, occlusion in alpha)
layout(location = 3) in vec2 in_uv_prelight; // PreLightGen (the map's lightmap)
layout(location = 4) in uint in_normal;      // 10:10:10 signed, world space
layout(location = 5) in uint in_tangent;
layout(location = 6) in uint in_binormal;

layout(std140, binding = 0) uniform block_ubo {
  mat4 view_proj;
  vec4 eye;        // GbxEyeInVisual
  vec4 light_dir;  // GbxLightDirDir0: where the sun's light goes
  vec4 light_rgb;  // GbxLightDirRgb0
  vec4 clouds_u;   // GbxVPositionToTexCoord_MapClouds
  vec4 clouds_v;
  vec4 fog;        // GbxVisualToFogTnL: 1 near, 0 fully fogged
  vec4 fog_color;
  vec4 params;     // x: alpha cutoff, y: 1 when there is a PreLightGen map
}
ubo;

layout(location = 0) out vec4 v_normal_low_lit; // NormalLow_IsLight
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_uv_lighting;
layout(location = 3) out vec3 v_tgt_x;          // TgtToWorldX..Z
layout(location = 4) out vec3 v_tgt_y;
layout(location = 5) out vec3 v_tgt_z;
layout(location = 6) out vec3 v_eye_to_pos;
layout(location = 7) out vec2 v_clouds;
layout(location = 8) out vec2 v_prelight;
layout(location = 9) out float v_fog;

vec3 unpack_normal(uint n) {
  ivec3 c = ivec3(int(n << 22) >> 22, int(n << 12) >> 22, int(n << 2) >> 22);
  return vec3(c) / 511.0;
}

void main() {
  vec4 pos = vec4(in_pos, 1.0);
  gl_Position = ubo.view_proj * pos;
  vec3 n = unpack_normal(in_normal), t = unpack_normal(in_tangent), b = unpack_normal(in_binormal);
  v_tgt_x = vec3(t.x, b.x, n.x);
  v_tgt_y = vec3(t.y, b.y, n.y);
  v_tgt_z = vec3(t.z, b.z, n.z);
  // the game passes the low normal as a colour: 0..1, 8 bits
  v_normal_low_lit.xyz = floor(clamp(n * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
  v_normal_low_lit.w = dot(n, -ubo.light_dir.xyz) > 0.0 ? 1.0 : 0.0;
  v_eye_to_pos = in_pos - ubo.eye.xyz;
  v_clouds = vec2(dot(pos, ubo.clouds_u), dot(pos, ubo.clouds_v));
  v_prelight = in_uv_prelight;
  v_fog = dot(pos, ubo.fog);
  v_uv = in_uv;
  v_uv_lighting = in_uv_lighting;
}
