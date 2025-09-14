#version 450

// base quad position
layout(location = 0) in vec2 in_pos;        // [-0.5,0.5]

// per-instance data
layout(location = 1) in vec2 instance_pos;  // world pos in pixels (map space)
layout(location = 2) in float instance_scale;
layout(location = 3) in int instance_skin;
layout(location = 4) in int instance_eye;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out int frag_skin_index;
layout(location = 2) flat out int frag_eye;

layout(binding = 0) uniform primitive_ubo {
    vec2 cam_pos;
    float zoom;
    float aspect;
    float max_map_size;
    mat4 proj;
    vec2 map_size;
} ubo;

void main() {
    // pixel -> normalized map coords
    vec2 norm = (instance_pos + in_pos * instance_scale) / ubo.map_size;

    // same transformation as primitive.vert.glsl
    vec2 rel = norm - ubo.cam_pos;
    rel *= (ubo.zoom * ubo.max_map_size);
    rel.y *= ubo.aspect;

    // Project
    gl_Position = ubo.proj * vec4(rel, 0.0, 1.0);

    // Pass UV and indices
    frag_uv = in_pos * 0.5 + 0.5;
    frag_skin_index = instance_skin;
    frag_eye = instance_eye;
}
