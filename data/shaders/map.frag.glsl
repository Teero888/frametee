#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2D texSampler1;
layout(binding = 2) uniform sampler2D texSampler2;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
  vec2 map_data_size = vec2(textureSize(texSampler2, 0));
  vec2 EntitySize = vec2(textureSize(texSampler1, 0));

  float MaxEntitySize = max(EntitySize.x, EntitySize.y);

  uint Tile = uint(texture(texSampler2, fragTexCoord).r * 255.0);
  if (Tile == 0u) {
    outColor.a = 0.0;
    discard;
  }

  ivec2 TileCoord = ivec2(Tile % 16u, Tile / 16u);

  vec2 TileSize = vec2(1.0) / (map_data_size);

  vec2 Scale = (map_data_size * 64.0) / MaxEntitySize;

  vec2 TileTexOff = mod(fragTexCoord, TileSize) * Scale;

  vec2 NewTex = vec2(vec2(TileCoord) / 16.0) + TileTexOff;

  outColor = texture(texSampler1, NewTex);
}
