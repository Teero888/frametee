#version 450

// CarSkinMultiSpecPC3 (the variant the game compiles for these settings):
//   gloss = sat(2 a), metal = sat(2 a - 1) of the skin's alpha
//   LDiffus = Color1 LightFromMap, LHdr = ambient (0.5 + 0.5 LightFromMap) + LDiffus (1 - metal)
//   colour  = skin (1 - LFresnel) LHdr + skin metal HemiSpec
//           + fresnel gloss EnvCubic + gloss 2 HemiSpec HemiSpec.a, times 2 clouds
// HemiSpec (the game's sphere map of the lights' highlights, drawn each
// frame) is evaluated here for the reflection: sum of colour pow(R.L, 7),
// alpha sum of k pow(R.L, 500) over the sun and the map's lights near the
// car (the game's ExpL / ExpA). LightFromMap: the lightmap under the car
// (tmuf_car.c draws it), else a constant.
layout(std140, binding = 0) uniform car_ubo {
  mat4 view_proj;
  mat4 to_world;
  vec4 eye;
  vec4 light_dir;
  vec4 light_rgb;
  vec4 ambient;
  vec4 clouds_u, clouds_v;
  vec4 fade;
  vec4 params;
  vec4 balls_rgb[8];
  vec4 balls_pos[8];
  vec4 spots_rgb[8];
  vec4 spots_pos[8];
  vec4 spots_dir[8];
  vec4 hemi_dir[8];
  vec4 hemi_rgb[8];
  vec4 counts;
  vec4 dov;
  vec4 lfm_u, lfm_v;
  vec4 light_from_map; // GbxLightFromMap_ScaleAD_TransDA: y 0 when LightFromMap holds colours
  vec4 water; // drawn into the sea's refraction: x its height, y 1, z time of day, w 1 / WaterDepthMax
}
ubo;
layout(binding = 1) uniform sampler2D map_clouds;
layout(binding = 2) uniform sampler2D map_diffuse_gloss;
layout(binding = 3) uniform samplerCube map_env_cubic;
layout(binding = 4) uniform sampler2D map_fresnel;
layout(binding = 5) uniform sampler2D map_dirt_marks;
layout(binding = 6) uniform sampler2D map_self_illum;
layout(binding = 7) uniform sampler2D map_light_from_map;
layout(binding = 8) uniform sampler2D map_water_fog; // the sea's fog (track.frag)

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_clouds;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec3 v_eye_to_pos;
layout(location = 4) in vec3 v_color1;
layout(location = 5) in vec2 v_lfm;
layout(location = 0) out vec4 out_color;

void shade();

// the sea's refraction (tmuf_scene.c): what is under the water, in its fog
// by how far the view goes through the water (as track.frag)
void main() {
  shade();
  if (ubo.water.y < 0.5) return;
  vec3 pos = ubo.eye.xyz + v_eye_to_pos;
  if (pos.y > ubo.water.x) discard;
  float dist = length(v_eye_to_pos);
  float depth = ubo.water.x - pos.y;
  float drop = -v_eye_to_pos.y;
  float way = dist * depth / max(depth, drop);
  vec4 fog = texture(map_water_fog, vec2(ubo.water.z, 1.0 - way * ubo.water.w));
  out_color = vec4(mix(out_color.rgb, fog.rgb, fog.a), 1.0);
}

void shade() {
  vec3 n = v_normal;
  vec3 r = normalize(reflect(v_eye_to_pos, n));
  vec4 skin = texture(map_diffuse_gloss, v_uv);
  vec4 dirt = texture(map_dirt_marks, v_uv);
  float dirt_intens = dirt.a * ubo.params.y;
  vec3 diffuse = mix(skin.rgb, dirt.rgb, dirt_intens);
  float gloss_a = mix(skin.a, ubo.params.z, dirt_intens);
  float metal = clamp(2.0 * gloss_a - 1.0, 0.0, 1.0);
  float gloss = clamp(2.0 * gloss_a, 0.0, 1.0);
  vec4 env = texture(map_env_cubic, r) * ubo.fade.y;
  // the fresnel's u: the reflection along the view of the car (HemiSpec's
  // frame's z)
  float fresnel = texture(map_fresnel, vec2(dot(r, ubo.dov.xyz))).r * gloss;
  vec4 hemi = vec4(0.0);
  for (int i = 0; i < int(ubo.counts.z); i++) {
    float d = clamp(dot(r, ubo.hemi_dir[i].xyz), 0.0, 1.0);
    hemi.rgb += ubo.hemi_rgb[i].rgb * pow(d, 7.0);
    hemi.a += clamp(ubo.hemi_dir[i].w, 0.0, 1.0) * pow(d, 500.0);
  }
  hemi = min(hemi, vec4(1.0));
  vec3 lfm3 = ubo.counts.w > 0.5 ? texture(map_light_from_map, v_lfm).rgb : vec3(ubo.params.x);
  // the glass (params.w 2: multiplying the frame by its Translucent through
  // its Fresnel; 3: adding its reflections)
  if (ubo.params.w > 1.5) {
    if (ubo.params.w < 2.5) {
      float c = dot(normalize(n), -normalize(v_eye_to_pos));
      float f = texture(map_fresnel, vec2(c)).r;
      out_color = vec4(skin.rgb * f * texture(map_clouds, v_clouds).rgb * 2.0, 1.0);
    } else {
      float f = texture(map_fresnel, vec2(dot(r, ubo.dov.xyz))).r;
      out_color = vec4(texture(map_env_cubic, r).rgb * f * ubo.fade.y + hemi.rgb * hemi.a, 1.0);
    }
    return;
  }
  if (ubo.light_from_map.y != 0.0) {
    // Day, Sunrise (the PSSM variants): LightFromMap's sky light (r) scales
    // the ambient and the reflections, the sun lights where the light mask
    // (GbxShadow0) lets it through; LightFromMap's sun visibility (g) stands
    // in for the mask, the screen's ambient occlusion for 1
    vec2 lg = ubo.counts.w > 0.5 ? texture(map_light_from_map, v_lfm).rg : vec2(1.0);
    float lfm_amb = lg.r * ubo.light_from_map.x + ubo.light_from_map.w;
    float vis = lg.g, mask = clamp(0.85 * vis + 0.15, 0.0, 1.0);
    vec3 clouds = texture(map_clouds, v_clouds).rgb;
    if (ubo.params.w > 0.5) {
      float gloss_d = mix(skin.a, ubo.params.z, dirt_intens);
      vec3 c = diffuse * (ubo.ambient.rgb * lfm_amb + mask * v_color1) + mask * hemi.rgb * gloss_d;
      out_color = vec4(c * clouds * 2.0 + texture(map_self_illum, v_uv).rgb, 1.0);
      return;
    }
    vec4 env_l = env * lfm_amb;
    float l_fresnel = fresnel * (1.0 - v_color1.x * mask) * env_l.a;
    vec3 l_hdr = ubo.ambient.rgb * lfm_amb + (1.0 - metal) * mask * v_color1;
    vec3 color = diffuse * (1.0 - l_fresnel) * l_hdr + diffuse * metal * hemi.rgb * mask * mask;
    color += fresnel * env_l.rgb;
    color += 2.0 * gloss * hemi.rgb * hemi.a * mask * vis;
    out_color = vec4(color * 2.0 * clouds, 1.0);
    return;
  }
  if (ubo.params.w > 0.5) {
    // the car's details (CarDetails): no reflections, their own lights
    // (SelfIllum) added
    float gloss_d = mix(skin.a, ubo.params.z, dirt_intens);
    vec3 c = diffuse * (v_color1 * lfm3 + ubo.ambient.rgb) + hemi.rgb * gloss_d;
    out_color = vec4(c * texture(map_clouds, v_clouds).rgb * 2.0 + texture(map_self_illum, v_uv).rgb, 1.0);
    return;
  }
  vec3 l_diffus = v_color1 * lfm3;
  float l_fresnel = fresnel * (1.0 - l_diffus.r) * env.a;
  vec3 l_hdr = ubo.ambient.rgb * (0.5 + 0.5 * clamp(lfm3, 0.0, 1.0)) + l_diffus * (1.0 - metal);
  vec3 color = diffuse * (1.0 - l_fresnel) * l_hdr + diffuse * metal * hemi.rgb;
  color += fresnel * env.rgb;
  color += gloss * 2.0 * hemi.rgb * hemi.a;
  color *= 2.0 * texture(map_clouds, v_clouds).rgb;
  out_color = vec4(color, 1.0);
}
