#version 450
// A light sample at a lightmap texel (lightmap_bake_spec.md §5, §9), added to
// the sums so far (prev, the same texel: the game's ping-pong targets). The
// shadow test: lit when the texel's depth along the light, lowered by the
// bias, is before the shadow map's (point sampled; outside the map lit).
//   mode 1  the sky's or the directional light's sample (AccumLightDir):
//           x += weight x max(0, N.-d) x lit, y += weight
//   mode 2  the sun's (AccumIsLighted): z += weight x lit, w += weight
//   mode 4  a spot's sample (AccumLightSpot): x += weight x DoLightSpot x lit,
//           y += weight
//   mode 6  the ambient cube (SetAmbient) into the lightmap's sum, for the
//           receivers without a CubeAmbient layer (params.x 1)
// The spots' maps are perspective: the depth along their axis, the bias a
// factor of it.
layout(std140, binding = 0) uniform bake_ubo {
  vec4 x_axis, y_axis, z_axis;
  vec4 centre;
  vec4 extent;
  vec4 params;
  vec4 persp;
  vec4 spot_pos; // xyz: the spot's position (the sample's), w: 1 / (cos_inner - cos_outer)
  vec4 spot_dir; // xyz: where it shines, w: cos_outer
  vec4 spot_rgb; // w: 1 / radius^2
}
ubo;
layout(binding = 1) uniform sampler2D shadow;
layout(binding = 2) uniform sampler2D prev;
layout(binding = 3) uniform samplerCube ambient;
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 0) out vec4 out_sum;

float lit() {
  vec3 rel = v_world - ubo.centre.xyz;
  float x = dot(ubo.x_axis.xyz, rel), y = dot(ubo.y_axis.xyz, rel), z = dot(ubo.z_axis.xyz, rel);
  ivec2 size = textureSize(shadow, 0);
  vec2 ndc;
  if (ubo.persp.x > 0.5) {
    if (z <= ubo.persp.w) return 1.0;
    ndc = vec2((x / z - ubo.persp.y) / ubo.extent.x, (y / z - ubo.persp.z) / ubo.extent.y);
  } else {
    ndc = vec2(x / ubo.extent.x, y / ubo.extent.y);
  }
  ivec2 texel = clamp(ivec2(floor((ndc * ubo.extent.w * 0.5 + 0.5) * vec2(size))), ivec2(0), size - 1);
  float stored = texelFetch(shadow, texel, 0).r;
  if (ubo.persp.x > 0.5) return (1.0 - ubo.params.y) * z < stored ? 1.0 : 0.0;
  return z - ubo.params.y < stored ? 1.0 : 0.0;
}

// DoLightSpot (Lights.Phlsl.Inc.txt), without its colour
float spot() {
  vec3 l = ubo.spot_pos.xyz - v_world;
  float d2 = dot(l, l);
  vec3 ln = l * inversesqrt(max(d2, 1e-12));
  float att = max(0.0, 1.0 - d2 * ubo.spot_rgb.w);
  float ndl = max(0.0, dot(v_normal, ln));
  float cone = clamp((dot(ln, -ubo.spot_dir.xyz) - ubo.spot_dir.w) * ubo.spot_pos.w, 0.0, 1.0);
  return ndl * att * cone;
}

vec3 bits8(vec3 c) { return floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }

void main() {
  vec4 sum = texelFetch(prev, ivec2(gl_FragCoord.xy), 0);
  float w = ubo.params.z;
  int mode = int(ubo.params.w + 0.5);
  if (mode == 6) {
    vec3 c = ubo.params.x > 0.5 ? bits8(texture(ambient, v_normal).rgb) : vec3(0.0);
    out_sum = vec4(min(sum.rgb + c, vec3(1.0)), 1.0);
  } else if (mode == 4) {
    out_sum = sum + vec4(w * spot() * lit(), w, 0.0, 0.0);
  } else if (mode == 2) {
    out_sum = sum + vec4(0.0, 0.0, w * lit(), w);
  } else {
    out_sum = sum + vec4(w * max(dot(v_normal, -ubo.z_axis.xyz), 0.0) * lit(), w, 0.0, 0.0);
  }
}
