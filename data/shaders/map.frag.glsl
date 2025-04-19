#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2D texSampler1;
layout(binding = 2) uniform sampler2D texSampler2;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 color1 = texture(texSampler1, fragTexCoord);
    vec4 color2 = texture(texSampler2, fragTexCoord);
    outColor = mix(color1, color2, 0.5) * vec4(fragColor, 1.0); // Example: blend textures
}
