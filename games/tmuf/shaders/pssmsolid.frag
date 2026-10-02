#version 450

// The sun's shadow maps' opaque casters (tmuf_pssm.c): their depth alone,
// the rasterizer's (pssm.vert's, with the slope's bias); no discard, so the
// depth is tested before the shader. The map's border is the scissor's.
void main() {}
