#version 450

// The main view's depth prepass (tmuf_scene.c): the opaque scenery's depth
// alone, from its own vertex shader (invariant), as the game's prepass
// writes the main Z buffer its main pass reuses (env_light_spec §4.1).
void main() {}
