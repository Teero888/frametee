#version 450
// The lightmap from the bake's sums (AccumNorm, lightmap_bake_spec.md §5.2,
// §9), its gutter (§6):
//   mode 0  AD: r = sat(sky^power) where the sky covers, g = sat(sun^power)
//           where the sun covers, a = covered
//   mode 1  one gutter pass: an uncovered texel takes its covered
//           4-neighbours' average
//   mode 3  RGB: the lightmap's sum (prev) plus the light's colour x sat(x)
//           where its samples cover (8 bits, as the game's blend)
//   mode 4  the lightmap's sum as it is
layout(std140, binding = 0) uniform finish_ubo {
  vec4 params; // x: mode, y: the sky's power, z: the sun's (mode 0); mode 3: yzw the light's colour
}
ubo;
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D prev;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 size = textureSize(source, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  int mode = int(ubo.params.x + 0.5);
  if (mode == 0) {
    vec4 acc = texelFetch(source, p, 0);
    float r = acc.y > 0.01 ? clamp(pow(max(acc.x, 0.0), ubo.params.y), 0.0, 1.0) : 0.0;
    float g = acc.w > 0.01 ? clamp(pow(max(acc.z, 0.0), ubo.params.z), 0.0, 1.0) : 0.0;
    out_color = vec4(r, g, 0.0, acc.y > 0.01 || acc.w > 0.01 ? 1.0 : 0.0);
    return;
  }
  if (mode == 3) {
    vec4 acc = texelFetch(source, p, 0), sum = texelFetch(prev, p, 0);
    if (acc.y > 0.01) {
      vec3 c = floor(clamp(ubo.params.yzw * clamp(acc.x, 0.0, 1.0), 0.0, 1.0) * 255.0 + 0.5) / 255.0;
      sum = vec4(min(sum.rgb + c, vec3(1.0)), 1.0);
    }
    out_color = sum;
    return;
  }
  if (mode == 4) {
    out_color = texelFetch(source, p, 0);
    return;
  }
  vec4 c = texelFetch(source, p, 0);
  if (c.a >= 0.9) {
    out_color = c;
    return;
  }
  vec4 n[4] = vec4[4](texelFetch(source, clamp(p + ivec2(1, 0), ivec2(0), size - 1), 0),
                      texelFetch(source, clamp(p - ivec2(1, 0), ivec2(0), size - 1), 0),
                      texelFetch(source, clamp(p + ivec2(0, 1), ivec2(0), size - 1), 0),
                      texelFetch(source, clamp(p - ivec2(0, 1), ivec2(0), size - 1), 0));
  float a = n[0].a + n[1].a + n[2].a + n[3].a;
  if (a > 0.9) {
    vec3 rgb = (n[0].rgb * n[0].a + n[1].rgb * n[1].a + n[2].rgb * n[2].a + n[3].rgb * n[3].a) / a;
    out_color = vec4(rgb, 1.0);
  } else {
    out_color = c;
  }
}
