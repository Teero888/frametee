#version 450

// The game's post effects (CSceneFxBloom, TmForever's own shaders), drawn
// over a whole target (blit.vert):
//
//   0 high:      MapCopy * pow(sat(mean(rgb) + 0.001), HighExponent)
//   1 taps:      sum of weight_i * MapSrc(uv + offset_i), 4 taps (the
//                quarter-size copy and the two blurs)
//   2 composite: MapCopy + MapBloom * sat((1 - pow(mean(copy), HdrExp)) * RemapS + RemapT)
layout(std140, binding = 0) uniform post_ubo {
  vec4 params;     // x: the mode above, y: HighExponent, z: HdrExp, w: HdrRemapS
  vec4 params2;    // x: HdrRemapT
  vec4 offsets[4]; // xy: tap i (1), or the bloom's (2), in uv
  vec4 weights;
}
ubo;
layout(binding = 1) uniform sampler2D map_src;   // MapCopy
layout(binding = 2) uniform sampler2D map_bloom; // MapBloom
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
  const int mode = int(ubo.params.x + 0.5);
  if (mode == 0) {
    vec4 c = texture(map_src, uv);
    float w = pow(clamp(dot(c.rgb, vec3(0.333333343)) + 0.001, 0.0, 1.0), ubo.params.y);
    out_color = c * w;
  } else if (mode == 1) {
    vec4 c = vec4(0.0);
    for (int i = 0; i < 4; i++) c += texture(map_src, uv + ubo.offsets[i].xy) * ubo.weights[i];
    out_color = c;
  } else {
    vec4 c = texture(map_src, uv);
    vec3 bloom = texture(map_bloom, uv + ubo.offsets[0].xy).rgb;
    float f = clamp((1.0 - pow(dot(c.rgb, vec3(0.333333343)), ubo.params.z)) * ubo.params.w + ubo.params2.x, 0.0, 1.0);
    out_color = vec4(c.rgb + bloom * f, c.a);
  }
}
