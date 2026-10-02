#version 450

// The game's SSAO (OccZCmpPC33, env_light_spec.md §4.3): per pixel, 16
// points of a sphere around it (8 reflected by the pixel's random normal,
// at two radii), each occluded as far as the scene in front of it is,
// into 1 - sum / 16
layout(std140, binding = 0) uniform ssao_ubo {
  vec4 proj;  // P00, P11, zMin, zMax - zMin
  vec4 size;  // W, H, the image radius (Radius tan(fovY / 2)), -
}
ubo;
layout(binding = 1) uniform sampler2D depth;       // packed (aodepth.frag)
layout(binding = 2) uniform sampler2D rand_normal;  // 64 x 64, the game's (as stored, 0..1)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

const vec3 k_sphere[8] = vec3[8](vec3(-.79410452, .32892886, .5110811), vec3(.32892886, -.79410452, -.5110811),
                                 vec3(.79410452, .32892886, -.5110811), vec3(-.32892886, -.79410452, .5110811),
                                 vec3(-.79410452, -.32892886, -.5110811), vec3(.32892886, .79410452, .5110811),
                                 vec3(.79410452, -.32892886, .5110811), vec3(-.32892886, .79410452, -.5110811));

float depth_at(ivec2 p) {
  p = clamp(p, ivec2(0), ivec2(ubo.size.xy) - 1);
  return ubo.proj.z + ubo.proj.w * dot(texelFetch(depth, p, 0).rgb, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
}

// the camera-space point at a pixel (x left, y up, z the view depth)
vec3 point_at(ivec2 p) {
  float z = depth_at(p);
  vec2 t = (vec2(p) + 0.5) / ubo.size.xy;
  return vec3((1.0 - 2.0 * t.x) / ubo.proj.x * z, (1.0 - 2.0 * t.y) / ubo.proj.y * z, z);
}

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  vec3 p = point_at(px);
  // the face normal (the game's prepass: its position's screen derivatives),
  // from the nearer neighbours
  vec3 l = point_at(px - ivec2(1, 0)), r = point_at(px + ivec2(1, 0));
  vec3 u = point_at(px - ivec2(0, 1)), d = point_at(px + ivec2(0, 1));
  vec3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
  vec3 dy = abs(d.z - p.z) < abs(p.z - u.z) ? d - p : p - u;
  vec3 n = normalize(cross(dx, dy));
  if (dot(n, p) > 0.0) n = -n;
  vec3 rn = texelFetch(rand_normal, px % 64, 0).rgb;
  float radius = p.z * ubo.size.z * 1.25;
  float sum = 0.0;
  for (int q = 0; q < 2; q++) {
    float scale = q == 0 ? 1.0 : 0.5;
    for (int k = 0; k < 8; k++) {
      vec3 o = reflect(k_sphere[k], rn) * scale;
      if (dot(n, o) < 0.0) o = -o;
      vec3 s = p + o * radius;
      vec2 us = vec2(0.5 - 0.5 * ubo.proj.x * s.x / s.z, 0.5 - 0.5 * ubo.proj.y * s.y / s.z);
      float zs = depth_at(ivec2(floor(us * ubo.size.xy)));
      float t = max(0.0, (s.z - zs) / radius);
      sum += t / (0.4 + t * t * t);
    }
  }
  // with the pixel's packed depth beside it (aoblur.frag reads both at once)
  out_color = vec4(1.0 - sum / 16.0, texelFetch(depth, px, 0).rgb);
}
