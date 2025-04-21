#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1) uniform sampler2D texSampler1;
layout(binding = 2) uniform sampler2D texSampler2;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform UniformBufferObject {
  vec3 transform;
  float aspect;
}
ubo;

void main() {
  vec2 map_data_size = vec2(textureSize(texSampler2, 0));
  vec2 EntitySize = vec2(textureSize(texSampler1, 0));
  float MaxEntitySize = max(EntitySize.x, EntitySize.y);

  vec4 colorSum = vec4(0.0);
  int sampleCount = 0;
  vec2 tex_coord = fragTexCoord;
  tex_coord.y *= ubo.aspect;
  tex_coord *= ubo.transform.z;
  tex_coord += ubo.transform.xy;
  tex_coord = tex_coord * 0.5 + 0.5;

  // if (uint(texture(texSampler2, tex_coord).r * 255.0) == 0u) {
  //   outColor.a = 0.0;
  //   discard;
  // }

  int numDiscards = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      // im doing * 64 here right now but we could do * 128 to get 1 pixel neighbor
      vec2 off_texcoord = tex_coord + (vec2(dx, dy) / (map_data_size * 128));
      vec2 TileSize = vec2(1.0) / (map_data_size);
	  // repeating edges of the map
      off_texcoord =
          mix(mix(off_texcoord, mod(off_texcoord, TileSize), step(off_texcoord, vec2(0.0))),
              vec2(1.0) - mod(off_texcoord, TileSize), max(vec2(0.0), sign(off_texcoord - vec2(1.0))));

      off_texcoord = clamp(off_texcoord, 0.0, 1.0);
      uint Tile = uint(texture(texSampler2, off_texcoord).r * 255.0);
      if (Tile == 0u) {
        ++numDiscards;
        continue;
      }

      ivec2 TileCoord = ivec2(Tile % 16u, Tile / 16u);

      vec2 Scale = (map_data_size * 64.0) / MaxEntitySize;

      vec2 TileTexOff = mod(off_texcoord, TileSize) * Scale;

      vec2 NewTex = vec2(vec2(TileCoord) / 16.0) + TileTexOff;

      colorSum += texture(texSampler1, NewTex);
      sampleCount++;
    }
  }
  // if (numDiscards >= 9) {
  // outColor = vec4(1.0, 0.0, 0.0, 1.0);
  // } else {
  outColor = colorSum / float(sampleCount);
  // }
}
