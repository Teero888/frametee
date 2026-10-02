#version 450

// Textures, the vertex colour, a blended ground's mix and the block's baked
// occlusion map: no dynamic lighting yet.
// The engine blends premultiplied alpha.
layout(std140, binding = 0) uniform track_ubo {
  mat4 view_proj;
  float lod_bias;
  float opacity;
  float alpha_cutoff; // texels below it are holes (0: opaque)
  float blend;
  vec4 gen;
  vec4 t2a, t2b;
  vec4 tia, tib;
  vec4 color_scale;
  vec4 stripe_gen;
  vec4 tsa, tsb;
  vec4 blend3; // x: whether there is a third layer (Blend3, over the others by its alpha)
  vec4 water;  // x: the sea's height, y: 1 when drawing its refraction, -1 its reflection, z: time of day,
               // w: 1 / WaterDepthMax
  vec4 eye;
  vec4 horizon0, horizon1; // the sky's horizon clouds (Sky\\...\\CloudsHorizon*.dds, the occlusion's slot and
                           // coordinates): their colours by the clouds' alpha; horizon0.w 1 when there are some
  vec4 fog;                // GbxVisualToFogTnL (track.vert)
  vec4 fog_color;          // w: 1 when the draw is fogged (the game's opaque ones)
  vec4 clouds_u, clouds_v; // (track.vert)
  vec4 lawn; // the Stadium's lawn: x its light (1 the lightmap, 2 its Lighting map in the occlusion's slot),
             // y the factor, z 1 when its occlusion multiplies the lightmap
  vec4 lightgen; // GbxLightGenP_ScaleAD_TransDA: y 0 when the lightmap holds colours
  vec4 light_dir, light_rgb; // the sun
  vec4 scenery;   // x 1: vertex-lit scenery (track.vert)
  vec4 light_dbl;
  vec4 extra; // x: the second picture: 1 SelfIllum, 2 a Glow added, 3 a sequencer over the Glow, 4 the sky's
              // ceiling under the panorama; y: the scenery's family: 0 X2, 1 SelfI, 2 TSelfI, 3 SelfI X2
  vec4 extra_a, extra_b;
  vec4 proj_pos, proj_x, proj_y, proj_z, proj; // the car's headlight (tm_track_uniforms)
  vec4 pssm_static[3];  // the sun's shadow maps (tm_track_uniforms)
  vec4 pssm_cascade[15];
  vec4 pssm_cell[5];
  vec4 pssm_split[5];
  vec4 pssm_view;
  vec4 ao, ao_mid_gray; // the ambient occlusion (tm_track_uniforms)
  mat4 to_world;
  vec4 spec_cube; // x 1: the specular lobe around the sun (SpecularCubeL); y: the Spec FCOut family (1 opaque, 2 the glass);
                  // z 1: the frame's alpha the gloss (the lamps' fake ground reflection), w 1: this draw's
                  // (CSpecL_Pixel)
}
ubo;
layout(binding = 1) uniform sampler2D picture;
layout(binding = 2) uniform sampler2D picture2;
layout(binding = 3) uniform sampler2D mask;
layout(binding = 4) uniform sampler2D occlusion;
layout(binding = 5) uniform sampler2D stripe;
layout(binding = 6) uniform sampler2D blend3;
layout(binding = 7) uniform sampler2D water_fog; // the sea's fog: time of day across, the way through the water down
layout(binding = 8) uniform sampler2D prelight;  // the map's lightmap atlas (PreLightGen)
layout(binding = 9) uniform sampler2D fresnel;   // the lawn's Fresnel, by how steeply it is seen
layout(binding = 10) uniform sampler2D clouds;   // the clouds' shadows
layout(binding = 11) uniform sampler2D extra;    // the second picture (ubo.extra)
layout(binding = 12) uniform sampler2D projector; // the headlight's picture (CarLights)
layout(binding = 13) uniform sampler2D pssm_static; // the sun's static shadow map (depth packed in rgb)
layout(binding = 14) uniform sampler2D pssm_atlas;  // its cascades, 3 x 2 cells
layout(binding = 15) uniform sampler2D ao_map;      // the ambient occlusion at the view's pixels (tmuf_ssao.c)
layout(binding = 16) uniform samplerCube env_cubic; // the Spec FCOut family's EnvCubic

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec2 frag_uv2;
layout(location = 2) in vec2 frag_uvi;
layout(location = 3) in vec4 frag_color;
layout(location = 4) in vec2 frag_uv_occlusion;
layout(location = 5) in vec2 frag_uv_stripe;
layout(location = 6) in vec3 frag_normal;
layout(location = 7) in vec2 frag_uv3;
layout(location = 8) in vec3 frag_pos;
layout(location = 9) in vec2 frag_uv_prelight;
layout(location = 10) in float frag_fog;
layout(location = 11) in vec2 frag_clouds;
layout(location = 12) in vec3 frag_c0; // the vertex-lit scenery's light in shadow
layout(location = 13) in vec2 frag_uv_extra;
layout(location = 0) out vec4 out_color;

// lit (1) or not at uv (in the texture) for the depth z: the 2x2 texels
// around, each compared, filtered bilinearly (the hardware PCF); z clamped
// to 1 as D16's compare does: beyond the far plane lit by the cleared border
// (Bay's skyline past the static map)
float pssm_pcf(sampler2D map, vec2 uv, float z) {
  z = min(z, 1.0);
  vec2 size = vec2(textureSize(map, 0));
  vec2 t = uv * size - 0.5;
  vec2 f = fract(t);
  // the 2 x 2 texels at once (clamped at the edges by the sampler): x (0, 1),
  // y (1, 1), z (1, 0), w (0, 0); the depths stored reversed, [-1, 16] in
  // [1, 0] (pssm.vert; cleared: 16, lit)
  vec4 d = (1.0 - textureGather(map, (floor(t) + 1.0) / size)) * 18.0 - 1.0;
  vec4 s = step(vec4(z), d);
  return mix(mix(s.w, s.z, f.x), mix(s.x, s.y, f.x), f.y);
}

// the static map's shadow alone (the scenery's glass: ShadowImageSpaceDisable)
float static_shadow(vec3 pos) {
  vec4 p = vec4(pos, 1.0);
  return pssm_pcf(pssm_static, vec2(dot(ubo.pssm_static[0], p), dot(ubo.pssm_static[1], p)), dot(ubo.pssm_static[2], p));
}

// The sun's shadow here (the game's light mask rgb, env_light_spec §3.6):
// the static map, then each cascade from far to near by its weight, i.e.
// static (1 - w0) .. (1 - w4) + the sum of w_j pcf_j (1 - w_k) over the
// nearer k: summed here from near to far, the farther ones left out once a
// cascade covers the point fully (most of the view: 4 taps, not 24)
float sun_shadow(vec3 pos) {
  vec4 p = vec4(pos, 1.0);
  float z = dot(pos - ubo.eye.xyz, ubo.pssm_view.xyz);
  float s = 0.0, rest = 1.0;
  for (int j = 4; j >= 0 && rest > 0.0; j--) {
    if (ubo.pssm_split[j].z < 0.5) continue;
    float w = clamp((ubo.pssm_split[j].x - z) * ubo.pssm_split[j].y, 0.0, 1.0);
    if (w <= 0.0) continue;
    vec3 c = vec3(dot(ubo.pssm_cascade[3 * j], p), dot(ubo.pssm_cascade[3 * j + 1], p), dot(ubo.pssm_cascade[3 * j + 2], p));
    vec2 uv = ubo.pssm_cell[j].xy + clamp(c.xy, 0.0, 1.0) * ubo.pssm_cell[j].zw;
    s += rest * w * pssm_pcf(pssm_atlas, uv, c.z);
    rest *= 1.0 - w;
  }
  if (rest > 0.0) {
    vec3 tc = vec3(dot(ubo.pssm_static[0], p), dot(ubo.pssm_static[1], p), dot(ubo.pssm_static[2], p));
    s += rest * pssm_pcf(pssm_static, tc.xy, tc.z);
  }
  return s;
}

// The car's headlight on this point (night_island_spec.md §5): the game's
// deferred projector buffer (8 bits, the light over DeferedScale), times
// DeferedScale, at most 1; added to the scenery's light
vec3 headlight(vec3 pos, vec3 normal) {
  vec3 lv = ubo.proj_pos.xyz - pos;
  float d2 = dot(lv, lv);
  float atten = clamp(1.0 - d2 * ubo.proj_pos.w, 0.0, 1.0);
  vec3 q = pos - ubo.proj_pos.xyz;
  float w = dot(q, ubo.proj_z.xyz);
  if (w <= 0.0) return vec3(0.0);
  vec2 uv = 0.5 + 0.5 * (vec2(dot(q, ubo.proj_x.xyz), dot(q, ubo.proj_y.xyz)) / w - vec2(ubo.proj_x.w, ubo.proj_y.w)) *
                      ubo.proj.xy;
  // only inside its frustum (the game draws the frustum's volume): the
  // picture clamped would light everything below the cone with its bottom row
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
  vec3 tex = texture(projector, uv).rgb;
  // lit receivers by sqrt(N.L) (N as interpolated), alpha-tested ones
  // without it, at a quarter (ProjectorBaseRgb)
  vec3 buf = ubo.proj.w < 1.5 ? clamp(sqrt(clamp(dot(normal, lv * inversesqrt(d2)), 0.0, 1.0)) * atten * tex, 0.0, 1.0)
                              : clamp(tex * atten * 0.25, 0.0, 1.0);
  buf = floor(buf * 255.0 + 0.5) / 255.0;
  return clamp(ubo.proj.z * buf, 0.0, 1.0);
}

void main() {
  // the sea's reflection keeps what is above the water (its clip plane,
  // 0.1 below the surface)
  if (ubo.water.y < -0.5 && frag_pos.y < ubo.water.x - 0.1) discard;
  vec4 texel = texture(picture, frag_uv, ubo.lod_bias);
  int extra_mode = int(ubo.extra.x + 0.5);
  vec4 second = extra_mode > 0 ? texture(extra, frag_uv_extra, ubo.lod_bias) : vec4(0.0);
  // the sky (Island's, fixed function): its ceiling (the stars at night)
  // under the panorama by the panorama's alpha
  if (extra_mode == 4) texel = vec4(mix(second.rgb, texel.rgb, texel.a), 1.0);
  // a ground's Borders over its Grass by their alpha (Island's beaches: the
  // grass's edge painted on the beach's own coordinates)
  if (extra_mode == 5) texel.rgb = mix(texel.rgb, second.rgb, second.a);
  if (ubo.blend > 0.5) {
    vec4 second = texture(picture2, frag_uv2, ubo.lod_bias);
    float m = texture(mask, frag_uvi, ubo.lod_bias).r;
    texel = mix(texel, second, m);
  }
  // a third layer over it by its alpha: a ground's Blend3, an advert's Mask
  if (ubo.blend3.x > 0.5) {
    vec4 third = texture(blend3, frag_uv3, ubo.lod_bias);
    texel.rgb = mix(texel.rgb, third.rgb, third.a);
  }
  // the Stadium's lawn (Techno2's grass shaders): its mix, lit by the map's
  // lightmap (x8) or its own Lighting map (x16), the clouds' shadows, the
  // mown stripes and a Fresnel by how steeply it is seen
  if (ubo.lawn.x > 2.5) {
    // the Stadium's dirt (Techno2's soil shaders): at night the lightmap's
    // colour; by day its sky light on a blue sky and the sun on the normal
    // where it is lit (the light mask; the lightmap's sun visibility stands
    // in for it); times the occlusion, the clouds' shadows, doubled
    vec4 lm = texture(prelight, frag_uv_prelight);
    vec3 light = lm.rgb;
    if (ubo.lightgen.y != 0.0) {
      float sky = lm.r * ubo.lightgen.x + ubo.lightgen.w;
      float sun = max(dot(normalize(frag_normal), -ubo.light_dir.xyz), 0.0);
      float mask = ubo.pssm_view.w > 0.5 ? sun_shadow(frag_pos) : lm.g;
      light = clamp(sky * vec3(0.52, 0.6, 0.7) + mask * sun * ubo.light_rgb.rgb, 0.0, 1.0);
    }
    float occ = ubo.color_scale.z > 0.5 ? texture(occlusion, frag_uv_occlusion, ubo.lod_bias).r : 1.0;
    vec3 c = texel.rgb * light * occ * texture(clouds, frag_clouds).rgb * 2.0;
    float f = ubo.fog_color.w > 0.5 ? clamp(frag_fog, 0.0, 1.0) : 1.0;
    if (ubo.water.y > 0.5) {
      out_color = vec4(c, 1.0); // (under the sea: not the Stadium's)
      return;
    }
    out_color = vec4(mix(ubo.fog_color.rgb, c, f) * ubo.opacity, ubo.opacity);
    return;
  }
  if (ubo.lawn.x > 0.5) {
    vec3 light = ubo.lawn.x < 1.5 ? texture(prelight, frag_uv_prelight).rgb
                                  : texture(occlusion, frag_uv_occlusion, ubo.lod_bias).rgb;
    if (ubo.lawn.x < 1.5 && ubo.lightgen.y != 0.0) {
      // Day, Sunrise (Techno2's grass with GbxShadow0): the lightmap's sky
      // light on a blue sky and the sun on the lawn's normal where it is lit
      // (the light mask; the lightmap's sun visibility stands in for it)
      vec2 lg = texture(prelight, frag_uv_prelight).rg;
      float sky = lg.r * ubo.lightgen.x + ubo.lightgen.w;
      float mask = ubo.pssm_view.w > 0.5 ? sun_shadow(frag_pos) : lg.g;
      float sun = clamp(mask * 0.85 + 0.15, 0.0, 1.0) * max(dot(normalize(frag_normal), -ubo.light_dir.xyz), 0.0);
      light = clamp(sky * vec3(0.52, 0.6, 0.7) + sun * ubo.light_rgb.rgb, 0.0, 1.0);
    }
    if (ubo.lawn.z > 0.5) light *= texture(occlusion, frag_uv_occlusion, ubo.lod_bias).r;
    vec3 st = ubo.stripe_gen.z > 0.5 ? texture(stripe, frag_uv_stripe, ubo.lod_bias).rgb : vec3(1.0);
    vec3 to_eye = ubo.eye.xyz - frag_pos;
    float steep = to_eye.y / length(to_eye);
    vec3 c = texel.rgb * light * texture(clouds, frag_clouds).rgb * st * texture(fresnel, vec2(steep)).rgb * ubo.lawn.y;
    float f = ubo.fog_color.w > 0.5 ? clamp(frag_fog, 0.0, 1.0) : 1.0;
    out_color = vec4(mix(ubo.fog_color.rgb, c, f) * ubo.opacity, ubo.opacity);
    return;
  }
  // a lawn's mown stripes: the detail pictures modulate it, doubled
  if (ubo.stripe_gen.z > 0.5) texel.rgb = texture(stripe, frag_uv_stripe, ubo.lod_bias).rgb * texel.rgb * 2.0;
  if (texel.a < ubo.alpha_cutoff) discard;
  // the sky's horizon clouds over its gradient (Sky's shader)
  if (ubo.horizon0.w > 0.5) {
    vec4 clouds = texture(occlusion, frag_uv_occlusion, ubo.lod_bias);
    vec3 c = mix(ubo.horizon0.rgb, ubo.horizon1.rgb, clouds.a) * clouds.rgb;
    texel.rgb = mix(texel.rgb, c, clouds.a);
  }
  float occ = ubo.color_scale.z > 0.5 ? texture(occlusion, frag_uv_occlusion, ubo.lod_bias).r : 1.0;
  vec3 light = vec3(occ);
  // lit draws (color_scale.w): the mood's ambient (tia) and sun (t2b) from the
  // sun's direction (t2a), in the draw's own frame
  if (ubo.color_scale.w > 0.5) {
    float ndl = max(dot(normalize(frag_normal), ubo.t2a.xyz), 0.0);
    light = occ * (ubo.tia.rgb + ubo.t2b.rgb * ndl);
  }
  float a = ubo.opacity;
  vec3 color = texel.rgb * frag_color.rgb * light;
  if (ubo.scenery.x > 1.5) {
    // the tree crowns' sprites (Rally, the trace's ps_21): from the
    // ambient towards min(ambient + sun, 1.25) by k = sat(N.-L + 0.5) (the
    // vertex colour, tmuf_scene.c), times the clouds' shadows, doubled
    vec3 lit = mix(ubo.tia.rgb, min(ubo.tia.rgb + ubo.t2b.rgb, vec3(1.25)), frag_color.r);
    color = texel.rgb * lit * texture(clouds, frag_clouds).rgb * 2.0;
  } else if (ubo.scenery.x > 0.5) {
    // the vertex-lit scenery (e.g. Techno's blended grounds): its light
    // between the shadow's and the sun's by the light mask (not drawn yet:
    // in the sun), and by its family: times the clouds' shadows, doubled
    // (X2); plus its SelfIllum (SelfI, without them; SelfI X2, inside);
    // unlit (TSelfI)
    // (the glass and the sea's refraction: the static map alone)
    float mask = ubo.pssm_view.w > 0.5 ? (ubo.spec_cube.y > 1.5 || ubo.water.y > 0.5 ? static_shadow(frag_pos) : sun_shadow(frag_pos)) : 1.0;
    // the ambient part darkened by the occlusion (MidGray at one half), the
    // sun's by the shadow down to 15 %
    vec3 ao = vec3(1.0);
    if (ubo.ao.x > 0.5) {
      float a = texture(ao_map, gl_FragCoord.xy * ubo.ao.yz).r;
      ao = mix(vec3(clamp(2.0 * a - 1.0, 0.0, 1.0)), vec3(clamp(2.0 * a, 0.0, 1.0)), ubo.ao_mid_gray.rgb);
    }
    vec3 lit = frag_c0 * ao + clamp(0.15 + 0.85 * mask, 0.0, 1.0) * (frag_color.rgb - frag_c0);
    // (proj.w 3: the projector buffer is the shadow mask, tmuf_scene.c)
    vec3 proj = ubo.proj.w > 2.5 ? vec3(clamp(ubo.proj.z * mask, 0.0, 1.0))
                : ubo.proj.w > 0.5 ? headlight(frag_pos, frag_normal) : vec3(0.0);
    lit += proj;
    vec3 cl = texture(clouds, frag_clouds).rgb;
    int kind = int(ubo.extra.y + 0.5);
    vec3 self_illum = extra_mode == 1 ? second.rgb : vec3(0.0);
    if (kind == 1) color = texel.rgb * lit + self_illum;
    else if (kind == 2) color = texel.rgb * cl * 2.0 * (1.0 + proj);
    else if (kind == 3) color = (texel.rgb * lit + self_illum) * cl * 2.0;
    else color = texel.rgb * lit * cl * 2.0;
    // the Spec FCOut family (Bay's far city, its trace's ps_21): the light
    // in the sun (no shadow, no AO) on the picture plus its SelfIllum,
    // towards the EnvCubic reflected by (Fresnel x (1 + light.r) / 2 + its
    // blue / 4) x the picture's alpha, times the clouds' shadows, doubled;
    // plus the sun's lobe (its generated 1D Specular: u^20 at u = R.-L)
    // times the sun's colour and the picture's alpha
    if (ubo.spec_cube.y > 0.5 && ubo.spec_cube.y < 1.5) {
      vec3 v = normalize(frag_pos - ubo.eye.xyz);
      vec3 n = frag_normal;
      vec3 r = reflect(v, n);
      float f = texture(fresnel, vec2(1.0 - dot(-v, n))).r * (frag_color.r + 1.0);
      vec4 env = texture(env_cubic, r);
      float k = (0.5 * f + 0.25 * env.b) * texel.a;
      color = mix(frag_color.rgb * texel.rgb + self_illum, env.rgb, k) * cl * 2.0;
      color += pow(clamp(dot(r, -ubo.light_dir.xyz), 0.0, 1.0), 20.0) * ubo.light_rgb.rgb * texel.a;
    }
    // its glass ("Trans", the trace's ps_29): the light between the shadow's
    // and the sun's by the static map's shadow (no AO) on the picture,
    // towards the EnvCubic by Fresnel x (1 - the picture's alpha) x the
    // cube's alpha, times the clouds' shadows, doubled; plus the
    // SpecularCubeL lobe times the sun's colour, 1 - the picture's alpha
    // and the shadow; blended by Fresnel x the cube's alpha + the picture's
    if (ubo.spec_cube.y > 1.5) {
      vec3 v = normalize(frag_pos - ubo.eye.xyz);
      vec3 n = frag_normal;
      vec3 r = reflect(v, n);
      float f = texture(fresnel, vec2(1.0 - dot(-v, n))).r;
      vec4 env = texture(env_cubic, r);
      vec3 sun_lit = frag_c0 + clamp(0.15 + 0.85 * mask, 0.0, 1.0) * (frag_color.rgb - frag_c0);
      color = mix(sun_lit * texel.rgb, env.rgb, f * (1.0 - texel.a) * env.a) * cl * 2.0;
      float lobe = 0.996 * pow(clamp(dot(normalize(r), -ubo.light_dir.xyz), 0.0, 1.0), 20.0);
      color += lobe * ubo.light_rgb.rgb * (1.0 - texel.a) * mask;
      texel.a = clamp(f * env.a + texel.a, 0.0, 1.0);
    }
    // the sun's specular lobe (Bay's buildings, the "CSpecL" shaders): the
    // generated SpecularCubeL, 254/255 max(0, cos)^20 about the sun (the
    // traces' L8 cube, its +Z face), by the view reflected off the normal,
    // times the sun's colour, the picture's alpha (its gloss), the shadow
    if (ubo.spec_cube.x > 0.5) {
      vec3 r = reflect(normalize(frag_pos - ubo.eye.xyz), normalize(frag_normal));
      float lobe = 0.996 * pow(max(dot(normalize(r), -normalize(ubo.light_dir.xyz)), 0.0), 20.0);
      color += lobe * ubo.light_rgb.rgb * texel.a * mask;
    }
  }
  // a Glow added (the start lights: 2 D vcol clouds + Glow)
  if (extra_mode == 2) color += second.rgb;
  // the chasing light signs: Glow vcol Sequencer, added
  if (extra_mode == 3) color *= second.rgb;
  // a blended material (stripe_gen.w): its picture's alpha, for the blend
  // factors the material has (glows: added)
  if (ubo.stripe_gen.w > 0.5) {
    out_color = vec4(color, texel.a * a);
    return;
  }
  // the sea's refraction (water.y 1: drawn into it, water.frag): only what is
  // under the water, in its fog by how far the view goes through the water
  // (TmForever's sea fog shader)
  if (ubo.water.y > 0.5) {
    if (frag_pos.y > ubo.water.x) discard;
    vec3 eye_to_pos = frag_pos - ubo.eye.xyz;
    float dist = length(eye_to_pos);
    float depth = ubo.water.x - frag_pos.y;
    float drop = -eye_to_pos.y;
    float way = dist * depth / max(depth, drop);
    vec4 fog = texture(water_fog, vec2(ubo.water.z, 1.0 - way * ubo.water.w));
    out_color = vec4(mix(color, fog.rgb, fog.a), 1.0);
    return;
  }
  // the fixed-function fog over the opaque ones
  if (ubo.fog_color.w > 0.5) color = mix(ubo.fog_color.rgb, color, clamp(frag_fog, 0.0, 1.0));
  // with the lamps' fake ground reflection (spec_cube.z) the frame's alpha
  // is the gloss it is added by: CSpecL_Pixel's picture alpha times
  // LightReflect (1), the game's other shaders 0 (Bay's and Coast's Sunset
  // traces)
  if (ubo.spec_cube.z > 0.5) {
    bool gloss = ubo.spec_cube.w > 0.5 && ubo.scenery.x > 0.5 && ubo.scenery.x < 1.5;
    out_color = vec4(color * a, gloss ? texel.a : 0.0);
    return;
  }
  out_color = vec4(color * a, a);
}
