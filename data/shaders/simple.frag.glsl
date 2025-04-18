#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(binding = 1) uniform sampler2D texSampler; // Matches descriptor set layout

layout(location = 0) out vec4 outColor;

void main() {
  // Combine vertex color and texture color
  outColor = texture(texSampler, fragTexCoord) * vec4(fragColor, 1.0);
  // Or just texture: outColor = texture(texSampler, fragTexCoord);
  // Or just vertex color: outColor = vec4(fragColor, 1.0);
}
