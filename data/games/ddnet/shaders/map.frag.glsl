#version 450
#extension GL_ARB_separate_shader_objects : enable

// The physics layers, drawn with the entities sheet over the whole screen.
// The layers are cut into 32x32-tile pages (dd_map.c). A page that holds one
// game tile and nothing else (every empty page, and the solid insides of a
// map) is only its entry in the page table; every other page has a slot in
// the atlas, two layers per set of slots:
//   0: game, front, tele, tune
//   1: speedup, switch, game flags | front flags << 4, switch flags
// so neither the size of a map nor its empty space decides how large a
// texture gets.
layout(binding = 1) uniform sampler2DArray tex_sampler1;
// rgb: the page's atlas slot (a = 1), or r: its game tile, g: that tile's flags (a = 0)
layout(binding = 2) uniform sampler2D pages;
layout(binding = 3) uniform sampler2DArray atlas;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform UniformBufferObject {
  // The tile at the centre of the screen (exact in a float) and where in it the
  // centre is: positions are taken relative to it, so a tile 100000 tiles out
  // keeps its precision.
  vec4 camera;
  vec4 scale; // xy: tiles from the centre to the edge of the screen, z: lod bias
  vec4 map;   // xy: size in tiles, z: atlas columns, w: slots per set
}
ubo;

const int PAGE = 32;

// The tile's corner of the sheet for a point of the tile, as DDNet's CalculateTexCoords puts it:
// it mirrors the texture coordinates of the corners and then hands each corner the one of the
// corner before it, so the tile is turned first and mirrored after. (The other way round, a tile
// with one mirror and a rotation, such as a mirrored stopper, ends up mirrored across the other axis.)
void apply_flags(uint flags, inout vec2 uv, inout vec2 dx, inout vec2 dy) {
  // ROTATE (90 CW): (u, v) -> (v, 1 - u), and the derivatives with it
  if ((flags & 8u) != 0u) {
    uv = vec2(uv.y, 1.0 - uv.x);
    dx = vec2(dx.y, -dx.x);
    dy = vec2(dy.y, -dy.x);
  }
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
}

uvec4 bytes(vec4 texel) { return uvec4(texel * 255.0 + 0.5); }

void main() {
  vec2 local = frag_tex_coord * ubo.scale.xy + ubo.camera.zw;
  vec2 cell = floor(local);
  vec2 within_tile_coord_normalized = local - cell;

  vec2 dx = dFdx(local);
  vec2 dy = dFdy(local);
  float bias_scale = exp2(ubo.scale.z);
  dx *= bias_scale;
  dy *= bias_scale;

  // Outside the map its edge tiles go on.
  ivec2 tile = clamp(ivec2(ubo.camera.xy) + ivec2(cell), ivec2(0), ivec2(ubo.map.xy) - 1);
  uvec4 page = bytes(texelFetch(pages, tile / PAGE, 0));
  uvec4 ids = uvec4(page.r, 0u, 0u, 0u);
  uvec4 more = uvec4(0u, 0u, page.g, 0u);
  if (page.a != 0u) {
    uint slot = page.r | (page.g << 8) | (page.b << 16);
    uint per_set = uint(ubo.map.w), columns = uint(ubo.map.z);
    uint index = slot % per_set;
    ivec2 texel = ivec2(index % columns, index / columns) * PAGE + tile % PAGE;
    int layer = int(slot / per_set) * 2;
    ids = bytes(texelFetch(atlas, ivec3(texel, layer), 0));
    more = bytes(texelFetch(atlas, ivec3(texel, layer + 1), 0));
  }

  uint game_tile_id = ids.r;
  uint front_tile_id = ids.g;
  uint tele_tile_id = ids.b;
  uint tune_tile_id = ids.a;
  uint speedup_tile_id = more.r;
  uint switch_tile_id = more.g;

  uint game_flags = more.b & 15u;
  uint front_flags = more.b >> 4;
  uint switch_flags = more.a;

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
