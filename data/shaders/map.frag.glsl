#version 330 core
out vec4 FragColor;

in vec3 ourColor;
in vec2 TexCoord;

uniform sampler2D Entities;
uniform sampler2D MapData;

void main() {
  vec2 MapDataSize = vec2(textureSize(MapData, 0));

  vec2 EntitySize = vec2(textureSize(Entities, 0));
  float MaxEntitySize = max(EntitySize.x, EntitySize.y);

  uint Tile = uint(texture(MapData, TexCoord).r * 255.0);
  if (Tile == 0u) {
    FragColor.a = 0.0;
    discard;
  }

  ivec2 TileCoord = ivec2(Tile % 16u, Tile / 16u);

  vec2 TileSize = vec2(1.0) / (MapDataSize);

  vec2 Scale = (MapDataSize * 64.0) / MaxEntitySize;

  vec2 TileTexOff = mod(TexCoord, TileSize) * Scale;

  vec2 NewTex = vec2(vec2(TileCoord) / 16.0) + TileTexOff;

  FragColor = texture(Entities, NewTex);
}
