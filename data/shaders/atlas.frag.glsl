#version 450

layout(binding = 1) uniform sampler2DArray atlas_texture_array;
layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in int frag_layer_index;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = texture(atlas_texture_array, vec3(frag_uv, float(frag_layer_index)));
	// out_color = vec4(frag_uv, 0.0,1.0);
}
