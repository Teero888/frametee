#version 450

// The track: vertices already in world space, one picture per draw. A blended
// ground (Blend1/Blend2 mixed by BlendI) samples its second and mask pictures
// at coordinates generated from the world position, as the game's fixed
// pipeline does (EGxUVGenerate), through each address's 2x3 transform.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_color; // BGRA, the mesh's vertex colour
layout(location = 3) in vec2 in_uv_occlusion;
layout(location = 4) in uint in_normal; // 10:10:10 signed, in world space (the track) or the part's frame (the car)
layout(location = 5) in vec2 in_uv3;    // Blend3's coordinates
layout(location = 6) in vec2 in_uv_prelight; // in the map's lightmap atlas
layout(location = 7) in uint in_prelight;    // COLOR0 (BGRA): the vertex-lit scenery's baked ambient light

layout(std140, binding = 0) uniform track_ubo {
  mat4 view_proj;
  float lod_bias;
  float opacity;
  float alpha_cutoff;
  float blend; // 1: Blend1/Blend2/BlendI
  vec4 gen;    // world axes the blend pictures' coordinates come from: x,y for Blend2, z,w for BlendI (0 x, 1 y, 2 z)
  vec4 t2a, t2b; // Blend2 transform (a b c d, tx ty)
  vec4 tia, tib; // BlendI transform
  vec4 color_scale; // multiplies the vertex colour (x), whether to use it (y), whether there is an occlusion map (z)
  vec4 stripe_gen;  // world axes of the Stripe picture's coordinates (x, y), whether there is one (z)
  vec4 tsa, tsb;    // its transform
  vec4 blend3;      // x: whether there is a third layer (Blend3); y: 1 for the start lights, w: their state's offset
  vec4 water;       // below the sea: x its height, y 1 when there is one, z time of day, w 1 / WaterDepthMax
  vec4 eye;
  vec4 horizon0, horizon1; // the sky's horizon clouds: their colours (horizon0.w: whether there are some)
  vec4 fog;       // GbxVisualToFogTnL: 1 near, 0 fully fogged
  vec4 fog_color; // w: 1 when the draw is fogged
  vec4 clouds_u, clouds_v; // GbxVPositionToTexCoord_MapClouds
  vec4 lawn;      // the Stadium's lawn: x its light (1 the lightmap, 2 its Lighting map), y the factor, z 1 with occlusion
  vec4 lightgen;
  vec4 light_dir, light_rgb; // the sun (the moon at night)
  vec4 scenery;   // x 1: vertex-lit scenery, y z: GbxPrelight_ScaleTrans, w 1: double sided
  vec4 light_dbl; // GbxLightDirRgbDblSided0
  vec4 extra;     // x: the second picture (TM_EXTRA_*), y: the scenery's family (TM_KIND_*)
  vec4 extra_a, extra_b; // its coordinates' transform (a b c d, tx ty)
  vec4 proj_pos, proj_x, proj_y, proj_z, proj; // the car's headlight (tm_track_uniforms)
  vec4 pssm_static[3];
  vec4 pssm_cascade[15];
  vec4 pssm_cell[5];
  vec4 pssm_split[5];
  vec4 pssm_view;
  vec4 ao, ao_mid_gray;
  mat4 to_world; // the vertices' frame (a car part's); [3][3] 0: they are in the world
}
ubo;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec2 frag_uv2;
layout(location = 2) out vec2 frag_uvi;
layout(location = 3) out vec4 frag_color;
layout(location = 4) out vec2 frag_uv_occlusion;
layout(location = 5) out vec2 frag_uv_stripe;
layout(location = 6) out vec3 frag_normal;
layout(location = 7) out vec2 frag_uv3;
layout(location = 8) out vec3 frag_pos;
layout(location = 9) out vec2 frag_uv_prelight;
layout(location = 10) out float frag_fog;
layout(location = 11) out vec2 frag_clouds;
layout(location = 12) out vec3 frag_c0; // the vertex-lit scenery's light in shadow (frag_color: in the sun)
layout(location = 13) out vec2 frag_uv_extra; // the second picture's

vec2 generated(vec2 axes, vec4 a, vec4 b) {
  vec2 p = vec2(in_pos[int(axes.x)], in_pos[int(axes.y)]);
  vec2 t = vec2(a.x * p.x + a.z * p.y + b.x, a.y * p.x + a.w * p.y + b.y);
  return t;
}

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  // the start lights' picture holds their three states one above the other:
  // a third of it, from the state's (blend3.w)
  frag_uv = ubo.blend3.y > 0.5 ? vec2(in_uv.x, in_uv.y / 3.0 + ubo.blend3.w) : in_uv;
  frag_uv2 = ubo.blend > 0.5 ? generated(ubo.gen.xy, ubo.t2a, ubo.t2b) : in_uv;
  frag_uvi = ubo.blend > 0.5 ? generated(ubo.gen.zw, ubo.tia, ubo.tib) : in_uv;
  frag_uv_occlusion = in_uv_occlusion;
  frag_uv3 = in_uv3;
  // the second picture: SelfIllum on the Diffuse's coordinates, the others
  // on their own (in the third layer's slot) through their transform
  vec4 ea = ubo.extra_a, eb = ubo.extra_b;
  frag_uv_extra = ubo.extra.x > 1.5 ? vec2(ea.x * in_uv3.x + ea.z * in_uv3.y + eb.x, ea.y * in_uv3.x + ea.w * in_uv3.y + eb.y)
                                    : in_uv;
  frag_pos = ubo.to_world[3][3] > 0.5 ? (ubo.to_world * vec4(in_pos, 1.0)).xyz : in_pos;
  frag_uv_prelight = in_uv_prelight;
  frag_fog = dot(vec4(in_pos, 1.0), ubo.fog);
  frag_clouds = vec2(dot(vec4(in_pos, 1.0), ubo.clouds_u), dot(vec4(in_pos, 1.0), ubo.clouds_v));
  ivec3 n = ivec3(int(in_normal << 22) >> 22, int(in_normal << 12) >> 22, int(in_normal << 2) >> 22);
  frag_normal = vec3(n) / 511.0;
  frag_uv_stripe = ubo.stripe_gen.z > 0.5 ? generated(ubo.stripe_gen.xy, ubo.tsa, ubo.tsb) : in_uv;
  vec4 c = unpackUnorm4x8(in_color).zyxw;
  frag_color = ubo.color_scale.y > 0.5 ? vec4(c.rgb * ubo.color_scale.x, 1.0) : vec4(1.0);
  frag_c0 = vec3(1.0);
  // (scenery.x 2: the tree crowns' sprites, their light in the colour)
  if (ubo.scenery.x > 0.5 && ubo.scenery.x < 1.5) {
    // DGbxGenCodePC3_PrelightCV: the mesh's colour (COLOR1) scaled into the
    // baked light (COLOR0), without the sun (C0) and with it (C1)
    vec4 c0 = unpackUnorm4x8(in_prelight).zyxw;
    vec3 mod_cv = clamp(ubo.scenery.y * c.rgb + ubo.scenery.z, 0.0, 1.0) * c0.a;
    vec3 sun = ubo.scenery.w > 0.5 ? ubo.light_dbl.rgb : ubo.light_rgb.rgb * max(dot(frag_normal, -ubo.light_dir.xyz), 0.0);
    frag_c0 = clamp(mod_cv * c0.rgb, 0.0, 1.0);
    frag_color = vec4(clamp(mod_cv * clamp(c0.rgb + sun, 0.0, 1.0), 0.0, 1.0), 1.0);
  }
}
