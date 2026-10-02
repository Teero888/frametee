#version 450

// The Stadium's grass marks (skid_spec.md §8), on track.vert's strips:
//   blend 0  the ground decal (GrassMarkGroundPC3): MapIntens across the
//            mark times the grass tile at 0.7 x the world's x, z; lit by the
//            vertex colour (white) and the ambient, at most 1; the clouds'
//            shadows are left out (their picture averages a half: x 2 = 1)
//   blend 1  the flattened tufts: GrassMarkFenceIntens's alpha alone, drawn
//            into the frame's alpha with MIN before the tufts read it
layout(std140, binding = 0) uniform track_ubo {
  mat4 view_proj;
  float lod_bias;
  float opacity;
  float alpha_cutoff;
  float blend;
}
ubo;
layout(binding = 1) uniform sampler2D intens; // MapIntens (u clamp, v wrap), or GrassMarkFenceIntens
layout(binding = 2) uniform sampler2D tile;   // MapBlendXZ, wrap

layout(location = 0) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;
layout(location = 8) in vec3 frag_pos;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 i = texture(intens, frag_uv, ubo.lod_bias);
  if (ubo.blend > 0.5) {
    out_color = vec4(0.0, 0.0, 0.0, i.a);
    return;
  }
  vec4 c = i * texture(tile, 0.7 * frag_pos.xz, ubo.lod_bias);
  if (c.a <= 0.0) discard;
  vec3 light = clamp(frag_color.rgb + vec3(0.212), 0.0, 1.0);
  out_color = vec4(c.rgb * light, c.a);
}
