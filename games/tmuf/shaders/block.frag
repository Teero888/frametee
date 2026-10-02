#version 450

// Techno2's PC3 block pixel shaders (TmForever's *.PHlsl, as the game
// compiles them; ubo.params.w picks one). In the map's lightmap:
//
//   LDiffus  = (CubeAmbient(normal) + PreLightGen) * Occlusion + Lighting
//   colour   = Diffuse * LDiffus * Clouds * 2
//            + Fresnel * ReflectSoft * Specular        * sat(0.1 + 2.5 PreLightGen)
//            + ReflectSoft.a * lit * Specular.a * Sun * Specular.rgb * sat(0.2 + 2.5 PreLightGen)
//
// Lighting (rgb) and Occlusion (a) are the block's baked map ("...L.dds"),
// PreLightGen the map's lightmap (the sun and the sky, shadowed), then the
// fixed-function fog over it.
layout(std140, binding = 0) uniform block_ubo {
  mat4 view_proj;
  vec4 eye;
  vec4 light_dir;
  vec4 light_rgb;
  vec4 clouds_u;
  vec4 clouds_v;
  vec4 fog;
  vec4 fog_color;
  vec4 params; // x: alpha cutoff, y: 1 with a PreLightGen map, z: 1 when Lighting is an occlusion map,
               // w: the game's variant (0: in the map's lightmap, 1: its baked light alone, 2: no maps)
  vec4 lightgen; // GbxLightGenP_ScaleAD_TransDA: y 0 when the lightmap holds colours
  vec4 water;    // the water's height, y 1 drawing its refraction, -1 its reflection, the time of day,
                 // 1 / WaterDepthMax (as track.frag)
  vec4 pssm_static[3]; // the sun's shadow maps (as track.frag's), pssm_view.w 1 to use them
  vec4 pssm_cascade[15];
  vec4 pssm_cell[5];
  vec4 pssm_split[5];
  vec4 pssm_view;
}
ubo;
layout(binding = 1) uniform sampler2D map_diffuse;
layout(binding = 2) uniform sampler2D map_specular;
layout(binding = 3) uniform sampler2D map_normal;
layout(binding = 4) uniform sampler2D map_lighting;
layout(binding = 5) uniform samplerCube map_cube_ambient;
layout(binding = 6) uniform samplerCube map_reflect_soft;
layout(binding = 7) uniform sampler2D map_fresnel;
layout(binding = 8) uniform sampler2D map_clouds;
layout(binding = 9) uniform sampler2D map_prelight;
layout(binding = 10) uniform sampler2D water_fog; // the water's fog: time of day across, the way through it down
layout(binding = 11) uniform sampler2D pssm_static; // the sun's static shadow map (depth packed in rgb)
layout(binding = 12) uniform sampler2D pssm_atlas;  // its cascades, 3 x 2 cells

layout(location = 0) in vec4 v_normal_low_lit;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_uv_lighting;
layout(location = 3) in vec3 v_tgt_x;
layout(location = 4) in vec3 v_tgt_y;
layout(location = 5) in vec3 v_tgt_z;
layout(location = 6) in vec3 v_eye_to_pos;
layout(location = 7) in vec2 v_clouds;
layout(location = 8) in vec2 v_prelight;
layout(location = 9) in float v_fog;
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

void main() {
  // the water's reflection keeps what is above the water (0.1 below it)
  vec3 world = ubo.eye.xyz + v_eye_to_pos;
  if (ubo.water.y < -0.5 && world.y < ubo.water.x - 0.1) discard;
  if (ubo.water.y > 0.5 && world.y > ubo.water.x) discard;
  vec4 diffuse = texture(map_diffuse, v_uv);
  if (diffuse.a < ubo.params.x) discard;
  vec4 specular = texture(map_specular, v_uv);
  vec3 clouds = texture(map_clouds, v_clouds).rgb;
  const int mode = int(ubo.params.w + 0.5);
  vec3 normal_low = v_normal_low_lit.xyz * 2.0 - 1.0;
  float is_lit = v_normal_low_lit.w;
  vec3 eye_to_pos = normalize(v_eye_to_pos);
  vec3 color;
  if (mode == 2) {
    // no maps of its own: the sun and the ambient cube (Techno2's PC3
    // "dynamic" block shader)
    vec3 n = normalize(vec3(v_tgt_x.z, v_tgt_y.z, v_tgt_z.z));
    vec3 r = reflect(eye_to_pos, n);
    float front = clamp((dot(r, normal_low) + 0.3) * 4.0, 0.0, 1.0);
    vec3 fresnel = texture(map_fresnel, vec2(dot(-eye_to_pos, n), specular.a)).rgb * front;
    vec3 soft = specular.rgb * texture(map_reflect_soft, r).rgb * fresnel;
    float soft_intens = dot(soft, vec3(0.577350259));
    vec3 light = clamp(ubo.light_rgb.rgb * max(dot(n, -ubo.light_dir.xyz), 0.0) * is_lit +
                           texture(map_cube_ambient, n).rgb, 0.0, 1.0);
    color = diffuse.rgb * light * clouds * 2.0 * (1.0 - 0.5 * soft_intens) + soft;
    float spec_intens = pow(max(dot(r, -ubo.light_dir.xyz), 0.0), 20.0) * is_lit * specular.a;
    color = color * (1.0 - 0.3 * spec_intens) + specular.rgb * spec_intens * ubo.light_rgb.rgb;
  } else {
    vec3 normal_in_tgt = texture(map_normal, v_uv).agb * 2.0 - 254.0 / 255.0;
    vec4 lighting_occ = texture(map_lighting, v_uv_lighting);
    vec3 lighting = lighting_occ.rgb;
    float occlusion = lighting_occ.a;
    if (ubo.params.z > 0.5) { // an occlusion map in its place (no baked light)
      lighting = vec3(0.0);
      occlusion = lighting_occ.r;
    }
    vec3 n = normalize(vec3(dot(normal_in_tgt, v_tgt_x), dot(normal_in_tgt, v_tgt_y), dot(normal_in_tgt, v_tgt_z)));
    vec3 r = reflect(eye_to_pos, n);
    vec4 reflect_soft_spec = texture(map_reflect_soft, r);
    vec3 fresnel = texture(map_fresnel, vec2(dot(-eye_to_pos, n), specular.a)).rgb;
    // no fresnel where the normal map reflects backwards (against the low normal)
    fresnel *= clamp((dot(r, normal_low) + 0.3) * 4.0, 0.0, 1.0);
    vec3 l_specular_soft = fresnel * reflect_soft_spec.rgb * specular.rgb;
    float spec_intens = reflect_soft_spec.a * is_lit * specular.a;
    vec3 l_specular = spec_intens * ubo.light_rgb.rgb * specular.rgb;
    if (mode == 1) {
      // not in the map's lightmap: its baked light alone, doubled
      color = diffuse.rgb * 2.0 * lighting * clouds * 2.0 + l_specular_soft + l_specular;
    } else {
      vec3 ambient = texture(map_cube_ambient, n).rgb;
      if (ubo.lightgen.y != 0.0) {
        // Day, Sunrise: the lightmap holds the sky's light (r) and the sun's
        // visibility (g), the sun lit per pixel where the light mask
        // (GbxShadow0: the sun's shadow maps) lets it through; without them
        // the lightmap's visibility stands in for the mask. Outside the
        // lightmap (the decoration: the game's shaders without PreLightGen,
        // A06's trace) the ambient and the sun whole
        vec2 lm = ubo.params.y > 0.5 ? texture(map_prelight, v_prelight).rg : vec2(1.0);
        vec2 lg = ubo.params.y > 0.5 ? lm * ubo.lightgen.xy + ubo.lightgen.wz : vec2(1.0);
        float mask = ubo.pssm_view.w > 0.5 ? sun_shadow(world) : lm.y;
        vec3 sun = ubo.light_rgb.rgb * max(dot(n, -ubo.light_dir.xyz), 0.0) * clamp(0.85 * mask + 0.15, 0.0, 1.0);
        vec3 l_diffus = clamp(ambient * lg.x * occlusion + sun * is_lit, 0.0, 1.0);
        vec3 soft = l_specular_soft * lg.x;
        float soft_intens = dot(soft, vec3(0.577350259));
        float dir = specular.a * reflect_soft_spec.a * is_lit * mask * lg.y;
        color = diffuse.rgb * l_diffus * clouds * 2.0 * (1.0 - 0.5 * soft_intens) + soft;
        color = color * (1.0 - 0.3 * dir) + specular.rgb * dir * ubo.light_rgb.rgb;
      } else {
        vec3 prelight = ubo.params.y > 0.5 ? texture(map_prelight, v_prelight).rgb : vec3(0.0);
        vec3 l_diffus = (ambient + prelight) * occlusion + lighting;
        color = diffuse.rgb * l_diffus * clouds * 2.0;
        color += l_specular_soft * clamp(0.1 + 2.5 * prelight, 0.0, 1.0);
        color += l_specular * clamp(0.2 + 2.5 * prelight, 0.0, 1.0);
      }
    }
  }
  // the water's refraction: what is under the water, in its fog by how far
  // the view goes through the water (track.frag)
  if (ubo.water.y > 0.5) {
    float dist = length(v_eye_to_pos);
    float depth = ubo.water.x - world.y;
    float drop = -v_eye_to_pos.y;
    float way = dist * depth / max(depth, drop);
    vec4 fog = texture(water_fog, vec2(ubo.water.z, 1.0 - way * ubo.water.w));
    out_color = vec4(mix(color, fog.rgb, fog.a), 1.0);
    return;
  }
  float f = clamp(v_fog, 0.0, 1.0);
  out_color = vec4(mix(ubo.fog_color.rgb, color, f), diffuse.a);
}
