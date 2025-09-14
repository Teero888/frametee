#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform PrimitiveUBO {
    vec2 camPos;      // camera center, in normalized map coords
    float zoom;       // camera zoom
    float aspect;     // window_ratio / map_ratio
    float maxMapSize; // fmax(width, height) * 0.001
    mat4 proj;        // orthographic [-1,1]
    vec2 mapSize;     // {width, height}, passed for normalization
} ubo;

void main() {
    vec2 norm = inPosition / ubo.mapSize;
    vec2 rel = norm - ubo.camPos;
    rel *= (ubo.zoom * ubo.maxMapSize);
    rel.y *= ubo.aspect;
    gl_Position = ubo.proj * vec4(rel, 0.0, 1.0);
    fragColor = inColor;
}
