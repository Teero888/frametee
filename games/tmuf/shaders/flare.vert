#version 450

// A light's flare (tmuf_flares.c): a quad on screen, as bright as the light
// is visible: its point (moved toward the camera by the flare's bias) tested
// against the frame's depth by an occlusion query, as the game's (the count
// read back a frame or two later: in_test.x 1 when it passed); the sun's
// quad, drawn, also hidden where the clouds' light occlusion at its middle
// covers it (the game's ps cnd: its intensity, which the clouds' glow takes,
// is the query's alone)
layout(location = 0) in vec4 in_clip;    // the quad's corner
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_test;    // x: the query passed; w > 0: the light was tested
layout(location = 4) in float in_clouds; // 1: masked by the clouds (the sun)

layout(std140, binding = 0) uniform flare_ubo {
  vec4 size; // the frame's width, height
}
ubo;
layout(binding = 3) uniform sampler2D clouds_occlusion;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
  gl_Position = in_clip;
  v_uv = in_uv;
  float visible = in_test.w > 0.0 ? in_test.x : 1.0;
  if (in_clouds > 0.5 && in_test.w <= 0.0) visible *= texture(clouds_occlusion, vec2(0.5)).b > 0.5 ? 1.0 : 0.0;
  v_color = in_color * visible;
}
