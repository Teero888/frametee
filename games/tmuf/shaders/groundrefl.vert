#version 450

// The lamps' fake ground reflection (tmuf_flares.c): quads in clip space,
// their picture across the screen as the game's projected coordinates
// ((u w, v w, w), D3DTTFF_PROJECTED) have it: not perspective-correct
layout(location = 0) in vec4 in_clip;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) noperspective out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
  gl_Position = in_clip;
  v_uv = in_uv;
  v_color = in_color;
}
