#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2DArray tex_sampler1;
layout(binding = 2) uniform sampler2D tex1; // rgb = game, front, tele
layout(binding = 3) uniform sampler2D tex2; // rgb = tune, speedup, switch
layout(binding = 4) uniform sampler2D tex3; // rgb = game flags, front flags, switch flags

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform UniformBufferObject {
  vec3 transform;
  float aspect;
  float lod_bias;
}
ubo;

void apply_flags(uint flags, inout vec2 uv, inout vec2 dx, inout vec2 dy) {
  // XFLIP
  if ((flags & 1u) != 0u) {
    uv.x = 1.0 - uv.x;
    dx.x = -dx.x;
    dy.x = -dy.x;
  }
  // YFLIP
  if ((flags & 2u) != 0u) {
    uv.y = 1.0 - uv.y;
    dx.y = -dx.y;
    dy.y = -dy.y;
  }
  // ROTATE (90 CW)
  if ((flags & 8u) != 0u) {
    // uv transform: (u, v) -> (v, 1-u)
    float tmp = uv.x;
    uv.x = uv.y;
    uv.y = 1.0 - tmp;

    // derivative transform:
    // new_dx.x = d(v)/dx = dx.y
    // new_dx.y = d(1-u)/dx = -dx.x
    vec2 old_dx = dx;
    dx.x = old_dx.y;
    dx.y = -old_dx.x;

    vec2 old_dy = dy;
    dy.x = old_dy.y;
    dy.y = -old_dy.x;
  }
}

void main() {
  vec2 map_data_size = vec2(textureSize(tex1, 0));
  vec2 tile_size = vec2(1.0) / map_data_size;

  vec2 tex_coord = frag_tex_coord;
  tex_coord.y *= ubo.aspect;
  tex_coord *= ubo.transform.z;
  tex_coord += ubo.transform.xy;

  // Calculate derivatives before wrapping
  vec2 uv_scaled = tex_coord * map_data_size;
  vec2 dx = dFdx(uv_scaled);
  vec2 dy = dFdy(uv_scaled);
  float bias_scale = exp2(ubo.lod_bias);
  dx *= bias_scale;
  dy *= bias_scale;

  // handle texture coordinate wrapping
  if (tex_coord.x < 0.0) tex_coord.x = mod(tex_coord.x, tile_size.x);
  if (tex_coord.x > 1.0) tex_coord.x = (1.0 - tile_size.x) + mod(tex_coord.x, tile_size.x);
  if (tex_coord.y < 0.0) tex_coord.y = mod(tex_coord.y, tile_size.y);
  if (tex_coord.y > 1.0) tex_coord.y = (1.0 - tile_size.y) + mod(tex_coord.y, tile_size.y);

  // Read tile IDs
  uint game_tile_id = uint(texture(tex1, tex_coord).r * 255.0);
  uint front_tile_id = uint(texture(tex1, tex_coord).g * 255.0);
  uint tele_tile_id = uint(texture(tex1, tex_coord).b * 255.0);
  uint tune_tile_id = uint(texture(tex2, tex_coord).r * 255.0);
  uint speedup_tile_id = uint(texture(tex2, tex_coord).g * 255.0);
  uint switch_tile_id = uint(texture(tex2, tex_coord).b * 255.0);

  // Read flags
  uint game_flags = uint(texture(tex3, tex_coord).r * 255.0);
  uint front_flags = uint(texture(tex3, tex_coord).g * 255.0);
  uint switch_flags = uint(texture(tex3, tex_coord).b * 255.0);

  vec2 within_tile_coord_normalized = mod(tex_coord, tile_size) / tile_size;

  // initialize final color and alpha
  vec4 final_color = vec4(0.0, 0.0, 0.0, 0.0);

  vec2 uv;
  vec2 layer_dx;
  vec2 layer_dy;

  // blend layers from back to front: game -> front -> tele -> tune -> speedup -> switch
  if (game_tile_id > 0u) {
    uv = within_tile_coord_normalized;
    layer_dx = dx;
    layer_dy = dy;
    apply_flags(game_flags, uv, layer_dx, layer_dy);

    vec4 color = textureGrad(tex_sampler1, vec3(uv, game_tile_id), layer_dx, layer_dy);
    if ((game_flags & 4u) != 0u) color.a = 1.0; // Opaque

    final_color = vec4(color.rgb * color.a, color.a);
  }

  if (front_tile_id > 0u) {
    uv = within_tile_coord_normalized;
    layer_dx = dx;
    layer_dy = dy;
    apply_flags(front_flags, uv, layer_dx, layer_dy);

    vec4 color = textureGrad(tex_sampler1, vec3(uv, front_tile_id), layer_dx, layer_dy);
    if ((front_flags & 4u) != 0u) color.a = 1.0; // Opaque

    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }

  if (tele_tile_id > 0u) {
    vec4 color = textureGrad(tex_sampler1, vec3(within_tile_coord_normalized, tele_tile_id), dx, dy);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (tune_tile_id > 0u) {
    vec4 color = textureGrad(tex_sampler1, vec3(within_tile_coord_normalized, tune_tile_id), dx, dy);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (speedup_tile_id > 0u) {
    vec4 color = textureGrad(tex_sampler1, vec3(within_tile_coord_normalized, speedup_tile_id), dx, dy);
    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }
  if (switch_tile_id > 0u) {
    uv = within_tile_coord_normalized;
    layer_dx = dx;
    layer_dy = dy;
    apply_flags(switch_flags, uv, layer_dx, layer_dy);

    vec4 color = textureGrad(tex_sampler1, vec3(uv, switch_tile_id), layer_dx, layer_dy);
    if ((switch_flags & 4u) != 0u) color.a = 1.0; // Opaque

    vec3 blended_rgb = color.rgb * color.a + final_color.rgb * (1.0 - color.a);
    float blended_alpha = color.a + final_color.a * (1.0 - color.a);
    final_color = vec4(blended_rgb, blended_alpha);
  }

  out_color = final_color;
}