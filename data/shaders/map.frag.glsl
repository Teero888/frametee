#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2DArray tex_sampler1;
layout(binding = 2) uniform sampler2D tex1; // rgb = game, front, tele
layout(binding = 3) uniform sampler2D tex2; // rgb = tune, speedup, switch

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform UniformBufferObject {
  vec3 transform;
  float aspect;
  float lod;
}
ubo;

void main() {
  vec2 map_data_size = vec2(textureSize(tex1, 0));
  vec2 tile_size = vec2(1.0) / map_data_size;

  vec2 tex_coord = frag_tex_coord;
  tex_coord.y *= ubo.aspect;
  tex_coord *= ubo.transform.z;
  tex_coord += ubo.transform.xy;

  // handle texture coordinate wrapping
  if (tex_coord.x < 0.0)
    tex_coord.x = mod(tex_coord.x, tile_size.x);
  if (tex_coord.x > 1.0)
    tex_coord.x = (1.0 - tile_size.x) + mod(tex_coord.x, tile_size.x);
  if (tex_coord.y < 0.0)
    tex_coord.y = mod(tex_coord.y, tile_size.y);
  if (tex_coord.y > 1.0)
    tex_coord.y = (1.0 - tile_size.y) + mod(tex_coord.y, tile_size.y);

  uint game_tile_id = uint(texture(tex1, tex_coord).r * 255.0);
  uint front_tile_id = uint(texture(tex1, tex_coord).g * 255.0);
  uint tele_tile_id = uint(texture(tex1, tex_coord).b * 255.0);
  uint tune_tile_id = uint(texture(tex2, tex_coord).r * 255.0);
  uint speedup_tile_id = uint(texture(tex2, tex_coord).g * 255.0);
  uint switch_tile_id = uint(texture(tex2, tex_coord).b * 255.0);

  vec2 within_tile_coord_normalized = mod(tex_coord, tile_size) / tile_size;

  // initialize final color and alpha
  vec4 final_color = vec4(0.0, 0.0, 0.0, 0.0);

  // blend layers from back to front: game -> front -> tele -> tune -> speedup -> switch
  if (game_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, game_tile_id), ubo.lod);
    final_color = vec4(color.rgb * color.a, color.a);
  }

  if (front_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, front_tile_id), ubo.lod);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }

  if (tele_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, tele_tile_id), ubo.lod);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (tune_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, tune_tile_id), ubo.lod);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (speedup_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, speedup_tile_id), ubo.lod);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (switch_tile_id > 0u) {
    vec4 color = textureLod(tex_sampler1, vec3(within_tile_coord_normalized, switch_tile_id), ubo.lod);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }

  out_color = final_color;
  if (final_color.a > 0.0) {
    out_color.rgb /= final_color.a;
    out_color.a = clamp(final_color.a, 0.0, 1.0);
  } else {
    discard;
  }
}
