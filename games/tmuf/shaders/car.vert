#version 450

// The car's skin (TmForever's CarSkinMultiSpecPC3 vertex shader, vs_9):
// vertices in their part's frame; lit per vertex by the map's spot and ball
// lights (Light8), the ground's light from below (RgbLightUnderground) and
// the sun; where LightFromMap and the clouds are over it. Shaded in car.frag.
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_normal; // 10:10:10 signed, the part's frame

layout(std140, binding = 0) uniform car_ubo {
  mat4 view_proj;  // of the part
  mat4 to_world;   // the part's frame in the world
  vec4 eye;
  vec4 light_dir;  // where the sun's light goes, world
  vec4 light_rgb;  // GbxV2LightDirRgb0
  vec4 ambient;    // GbxLightAmbient
  vec4 clouds_u, clouds_v;
  vec4 fade;       // GbxGameUnder_BlendIn_Fade_FOut_FIn
  vec4 params;     // x: LightFromMap without its picture, y: dirt intensity, z: dirt gloss, w: 1 for the details
  vec4 balls_rgb[8];  // Light8Balls_Rgb_IRad2 (world)
  vec4 balls_pos[8];  // Light8Balls_Pos_Rad2
  vec4 spots_rgb[8];  // Light8Spots_Rgb_IRad2
  vec4 spots_pos[8];  // Light8Spots_Pos_ICosR
  vec4 spots_dir[8];  // Light8Spots_Dir_CosOt
  vec4 hemi_dir[8];   // HemiSpec's lights: toward them from the car's centre, k
  vec4 hemi_rgb[8];   //   their colours
  vec4 counts;        // balls, spots, HemiSpec lights, 1 when LightFromMap has its picture
  vec4 dov;           // from the eye to the car's centre (HemiSpec's frame's z)
  vec4 lfm_u, lfm_v;  // world -> LightFromMap's picture (VPositionTo01LightFromMap)
  vec4 light_from_map;
}
ubo;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_clouds;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec3 v_eye_to_pos;
layout(location = 4) out vec3 v_color1;
layout(location = 5) out vec2 v_lfm;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  ivec3 c = ivec3(int(in_normal << 22) >> 22, int(in_normal << 12) >> 22, int(in_normal << 2) >> 22);
  vec3 n = mat3(ubo.to_world) * (vec3(c) / 511.0); // not normalised, as the game's
  vec3 world = (ubo.to_world * vec4(in_pos, 1.0)).xyz;
  vec3 lights = vec3(0.0);
  for (int i = 0; i < int(ubo.counts.x); i++) {
    vec3 lv = ubo.balls_pos[i].xyz - world;
    float d2 = dot(lv, lv);
    vec3 l = lv * inversesqrt(max(d2, 1e-12));
    lights += ubo.balls_rgb[i].rgb * max(0.0, dot(n, l)) * max(0.0, 1.0 - d2 * ubo.balls_rgb[i].w);
  }
  for (int i = 0; i < int(ubo.counts.y); i++) {
    vec3 lv = ubo.spots_pos[i].xyz - world;
    float d2 = dot(lv, lv);
    vec3 l = lv * inversesqrt(max(d2, 1e-12));
    float spot = clamp((dot(l, -ubo.spots_dir[i].xyz) - ubo.spots_dir[i].w) * ubo.spots_pos[i].w, 0.0, 1.0);
    lights += ubo.spots_rgb[i].rgb * max(0.0, dot(n, l)) * max(0.0, 1.0 - d2 * ubo.spots_rgb[i].w) * spot;
  }
  // RgbLightUnderground by how much the normal faces up, the sun
  vec3 under = vec3(240.0, 211.0, 172.0) / 255.0 * clamp(n.y, 0.0, 1.0) * ubo.fade.x;
  vec3 dir = ubo.light_rgb.rgb * clamp(dot(n, -ubo.light_dir.xyz), 0.0, 1.0) * ubo.fade.z;
  v_color1 = clamp(lights + under + dir, 0.0, 1.0); // a colour interpolator: saturated
  v_normal = n;
  v_eye_to_pos = world - ubo.eye.xyz;
  v_clouds = vec2(dot(vec4(world, 1.0), ubo.clouds_u), dot(vec4(world, 1.0), ubo.clouds_v));
  v_lfm = vec2(dot(vec4(world, 1.0), ubo.lfm_u), dot(vec4(world, 1.0), ubo.lfm_v));
  v_uv = in_uv;
}
