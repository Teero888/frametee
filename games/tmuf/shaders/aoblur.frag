#version 450

// The game's depth-aware blur (BlurHV_DepthTest, env_light_spec.md §4.4):
// n point taps along one axis, gaussian weights; beyond the first ones a
// tap counts as much as its depth follows the centre's slope; then the
// power
layout(std140, binding = 0) uniform blur_ubo {
  vec4 axis;       // xy: one texel along the axis (in texels: (1, 0) or (0, 1)), z: the power, w: taps a side
  vec4 weights[3]; // the weights of the centre and of the taps 1..11
  vec4 depth;      // x: DepthDeltaScale
}
ubo;
layout(binding = 1) uniform sampler2D color; // r: the occlusion, gba: the depth, packed (ssao.frag)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  ivec2 hi = textureSize(color, 0) - 1;
  ivec2 step_ = ivec2(ubo.axis.xy);
  vec4 c0 = texelFetch(color, px, 0);
  float d0 = dot(c0.gba, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
  // (the fine derivatives: the slope along the axis, per texel)
  float slope = dot(vec2(dFdxFine(d0), dFdyFine(d0)), ubo.axis.xy);
  float w0 = ubo.weights[0].x;
  float sum = w0 * c0.r;
  float ws = w0;
  int n = int(ubo.axis.w);
  for (int i = 1; i < n; i++) {
    float w = ubo.weights[i / 4][i % 4];
    for (int s = -1; s <= 1; s += 2) {
      vec4 t = texelFetch(color, clamp(px + s * i * step_, ivec2(0), hi), 0);
      float wd = w;
      if (i >= 2)
        wd *= 1.0 - clamp(abs(dot(t.gba, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0)) - (d0 + float(s * i) * slope)) * ubo.depth.x,
                          0.0, 1.0);
      sum += wd * t.r;
      ws += wd;
    }
  }
  // (the depth passed on for the other axis)
  out_color = vec4(pow(sum / ws * 1.001, ubo.axis.z), c0.gba);
}
