#version 450

// The sky's 3D clouds (tmuf_clouds.c): camera-facing sprite quads built on
// the CPU each frame in world space, lit there as the game's CloudsPC2
// vertex shader lights them (their colour and opacity per vertex).
invariant gl_Position;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color; // CloudsRgbMin..Max by the light, and the opacity
layout(location = 3) in vec4 in_occ;   // the light-occlusion camera: clip x, y, 1 in front of the eye, w

layout(std140, binding = 0) uniform clouds_ubo {
  mat4 view_proj; // the view's, or the occlusion camera's (the occlusion pass)
  vec4 flare;     // LightDirRgbFlare
  vec4 params;    // x: 1 for the occlusion pass, y: LightOcc opacity
}
ubo;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_occ;

void main() {
  gl_Position = ubo.view_proj * vec4(in_pos, 1.0);
  v_uv = in_uv;
  v_color = in_color;
  v_occ = in_occ;
}
