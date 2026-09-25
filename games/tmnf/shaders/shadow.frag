#version 450
layout(binding=1) uniform sampler2DArray diffuse_map;
layout(push_constant) uniform Material {
  vec4 animation;
  uvec4 flags;
  vec4 tint;
} material;
layout(location=2) in vec2 texcoord;
void main() {
  if ((material.flags.x & 2u) != 0u && texture(diffuse_map,vec3(texcoord,0)).a < 0.5)
    discard;
}
