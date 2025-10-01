#version 450

// Base quad vertex (-1 to 1)
layout(location = 0) in vec2 in_pos;

// Per-instance data
layout(location = 1) in vec2 instance_pos;       // World position
layout(location = 2) in vec2 instance_size;      // World size
layout(location = 3) in float instance_rotation; // Rotation in radians
layout(location = 4) in int instance_layer_index;  // Which layer to use
layout(location = 5) in vec2 instance_uv_scale;    // UV scale for sub-texture
 
// UBO for camera transforms
layout(binding = 0) uniform primitive_ubo {
    vec2 cam_pos;
    float zoom;
    float aspect;
    float max_map_size;
    mat4 proj;
    vec2 map_size;
} ubo;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out int frag_layer_index;

mat2 rot(float a) {
  float s = sin(a);
  float c = cos(a);
  return mat2(c, -s, s, c);
}

void main() {
    // Apply rotation and scale to the base quad vertices
    vec2 transformed_pos = rot(instance_rotation) * (in_pos * instance_size * 0.5);

    // Calculate final world position and normalize it to map coordinates [0,1]
    vec2 world_pos = instance_pos + transformed_pos;
    vec2 norm = world_pos / ubo.map_size;

    // Convert to camera-relative NDC coordinates for projection
    vec2 rel = (norm - ubo.cam_pos) * (ubo.zoom * ubo.max_map_size);
    rel.y *= ubo.aspect;

    // Set final clip-space position
    gl_Position = ubo.proj * vec4(rel, 0.0, 1.0);

    // Pass UVs and layer index to the fragment shader
    // UVs are the full [0,1] range since each sprite is a full texture layer
    frag_uv = (in_pos * 0.5 + 0.5) * instance_uv_scale;
    frag_layer_index = instance_layer_index;
}
