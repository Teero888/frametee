#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2DArray tex_sampler1;
layout(binding = 2) uniform sampler2D tex_sampler2;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform UniformBufferObject {
  vec3 transform;
  float aspect;
}
ubo;

void main() {
  vec2 map_data_size = vec2(textureSize(tex_sampler2, 0));

  vec2 tile_size = vec2(1.0) / map_data_size;

  vec2 tex_coord = frag_tex_coord;

  tex_coord.y *= ubo.aspect;

  tex_coord *= ubo.transform.z;

  tex_coord += ubo.transform.xy;

  if (tex_coord.x < 0.0)
    tex_coord.x = mod(tex_coord.x, tile_size.x);
  if (tex_coord.x > 1.0)
    tex_coord.x = (1.0 - tile_size.x) + mod(tex_coord.x, tile_size.x);
  if (tex_coord.y < 0.0)
    tex_coord.y = mod(tex_coord.y, tile_size.y);
  if (tex_coord.y > 1.0)
    tex_coord.y = (1.0 - tile_size.y) + mod(tex_coord.y, tile_size.y);

  uint tile_id = uint(texture(tex_sampler2, tex_coord).r * 255.0);

  if (tile_id == 0u)
    discard;

  ivec2 tile_atlas_coord = ivec2(tile_id % 16u, tile_id / 16u);

  vec2 within_tile_coord_normalized = mod(tex_coord, tile_size) / tile_size;

  if (tile_id > 0u) {
    const float entity_lod = 0.0;
    out_color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, tile_id), entity_lod);
  } else {
    out_color = vec4(0.0, 0.0, 0.0, 0.0);
  }
}
