#version 450

// base quad vertex
layout(location = 0) in vec2 in_pos;

// per-instance
layout(location = 1) in vec2 instance_pos;
layout(location = 2) in float instance_scale;
layout(location = 3) in int instance_skin;
layout(location = 4) in int instance_eye;
layout(location = 5) in vec3 anim_body;
layout(location = 6) in vec3 anim_back;
layout(location = 7) in vec3 anim_front;
layout(location = 8) in vec3 anim_attach;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out int frag_skin_index;
layout(location = 2) flat out int frag_eye;
layout(location = 3) flat out vec3 frag_body;
layout(location = 4) flat out vec3 frag_back;
layout(location = 5) flat out vec3 frag_front;
layout(location = 6) flat out vec3 frag_attach;

layout(binding = 0) uniform primitive_ubo {
    vec2 cam_pos;
    float zoom;
    float aspect;
    float max_map_size;
    mat4 proj;
    vec2 map_size;
} ubo;

void main() {
    vec2 norm = (instance_pos + in_pos * instance_scale) / ubo.map_size;
    vec2 rel = norm - ubo.cam_pos;
    rel *= (ubo.zoom * ubo.max_map_size);
    rel.y *= ubo.aspect;

    gl_Position = ubo.proj * vec4(rel,0.0,1.0);

    frag_uv = in_pos*0.5+0.5;
    frag_skin_index = instance_skin;
    frag_eye = instance_eye;
    frag_body = anim_body;
    frag_back = anim_back;
    frag_front = anim_front;
    frag_attach = anim_attach;
}
