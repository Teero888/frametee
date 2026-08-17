// Turning the game's textures into something the engine can sample.
//
// TrackMania ships its images as DXT-compressed DDS inside the packs. The
// engine's 3D path samples one texture array, so every texture a track needs
// becomes one layer of it: same size, same format, uncompressed RGBA. That
// costs more memory than keeping them compressed and is worth it here, because
// it means the whole world goes out in a single draw and a triangle picks its
// texture with an integer.
//
// The decoder below handles DXT1, DXT3 and DXT5, which is everything the
// stadium, island, bay, coast, alpine, speed and rally packs use for surfaces.
// Anything else is skipped and the surface falls back to a flat colour.

#include "tmnf_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmnf {
namespace {

// --- DDS ---------------------------------------------------------------------

constexpr std::uint32_t kDdsMagic = 0x20534444u; // "DDS "
constexpr std::size_t kDdsHeaderSize = 128u;     // magic + 124-byte header

std::uint32_t ReadU32(const unsigned char *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct DdsInfo {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t fourcc = 0u;
  std::uint32_t rgb_bit_count = 0u;
  bool has_alpha_channel = false;
  const unsigned char *data = nullptr;
  std::size_t data_size = 0u;
};

bool ParseDds(const std::vector<unsigned char> &bytes, DdsInfo *out) {
  if (bytes.size() < kDdsHeaderSize || ReadU32(bytes.data()) != kDdsMagic) return false;
  const unsigned char *h = bytes.data() + 4;
  out->height = ReadU32(h + 8);
  out->width = ReadU32(h + 12);
  const unsigned char *pf = h + 72; // DDS_PIXELFORMAT
  const std::uint32_t flags = ReadU32(pf + 4);
  out->fourcc = ReadU32(pf + 8);
  out->rgb_bit_count = ReadU32(pf + 12);
  out->has_alpha_channel = (flags & 0x1u) != 0u; // DDPF_ALPHAPIXELS
  if ((flags & 0x4u) == 0u) out->fourcc = 0u;    // DDPF_FOURCC clear: uncompressed
  out->data = bytes.data() + kDdsHeaderSize;
  out->data_size = bytes.size() - kDdsHeaderSize;
  return out->width != 0u && out->height != 0u;
}

// One 4x4 DXT colour block. `opaque_third` selects the DXT1 rule where the
// fourth colour is transparent black rather than a third interpolant.
void DecodeColorBlock(const unsigned char *block, bool allow_punchthrough, std::uint8_t out[16][4]) {
  const std::uint16_t c0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
  const std::uint16_t c1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
  const auto expand = [](std::uint16_t c, std::uint8_t rgb[3]) {
    const std::uint32_t r = (c >> 11) & 0x1Fu, g = (c >> 5) & 0x3Fu, b = c & 0x1Fu;
    rgb[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    rgb[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    rgb[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
  };

  std::uint8_t palette[4][4] = {};
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255u;
  const bool punchthrough = allow_punchthrough && c0 <= c1;
  for (int i = 0; i < 3; ++i) {
    if (punchthrough) {
      palette[2][i] = static_cast<std::uint8_t>((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0u;
    } else {
      palette[2][i] = static_cast<std::uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = static_cast<std::uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
    }
  }
  palette[2][3] = 255u;
  palette[3][3] = punchthrough ? 0u : 255u;

  const std::uint32_t bits = ReadU32(block + 4);
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], palette[(bits >> (2 * i)) & 3u], 4);
}

void DecodeDxt3Alpha(const unsigned char *block, std::uint8_t out[16][4]) {
  for (int i = 0; i < 16; ++i) {
    const std::uint8_t nibble = (block[i / 2] >> ((i & 1) ? 4 : 0)) & 0x0Fu;
    out[i][3] = static_cast<std::uint8_t>(nibble * 17u);
  }
}

void DecodeDxt5Alpha(const unsigned char *block, std::uint8_t out[16][4]) {
  std::uint8_t alpha[8];
  alpha[0] = block[0];
  alpha[1] = block[1];
  if (alpha[0] > alpha[1]) {
    for (int i = 0; i < 6; ++i)
      alpha[2 + i] = static_cast<std::uint8_t>(((6 - i) * alpha[0] + (1 + i) * alpha[1]) / 7);
  } else {
    for (int i = 0; i < 4; ++i)
      alpha[2 + i] = static_cast<std::uint8_t>(((4 - i) * alpha[0] + (1 + i) * alpha[1]) / 5);
    alpha[6] = 0u;
    alpha[7] = 255u;
  }
  std::uint64_t bits = 0u;
  for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i][3] = alpha[(bits >> (3 * i)) & 7u];
}

// The top mip of a DDS, as RGBA8. Only the top level is read: the engine builds
// its own mip chain when the layer is uploaded.
bool DecodeDds(const std::vector<unsigned char> &bytes, std::uint32_t *width, std::uint32_t *height,
               std::vector<std::uint8_t> *rgba) {
  DdsInfo dds;
  if (!ParseDds(bytes, &dds)) return false;
  // Something the size of a whole level is not a surface texture; it is a
  // lightmap atlas or a cube map face strip, and unpacking one costs more than
  // it can possibly be worth on screen.
  if (dds.width > 2048u || dds.height > 2048u) return false;

  *width = dds.width;
  *height = dds.height;
  rgba->assign(static_cast<std::size_t>(dds.width) * dds.height * 4u, 0u);

  const std::uint32_t blocks_x = (dds.width + 3u) / 4u;
  const std::uint32_t blocks_y = (dds.height + 3u) / 4u;

  const bool dxt1 = dds.fourcc == 0x31545844u; // "DXT1"
  const bool dxt3 = dds.fourcc == 0x33545844u; // "DXT3"
  const bool dxt5 = dds.fourcc == 0x35545844u; // "DXT5"

  if (dxt1 || dxt3 || dxt5) {
    const std::size_t block_bytes = dxt1 ? 8u : 16u;
    if (dds.data_size < static_cast<std::size_t>(blocks_x) * blocks_y * block_bytes) return false;
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
      for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
        const unsigned char *block = dds.data + (static_cast<std::size_t>(by) * blocks_x + bx) * block_bytes;
        std::uint8_t texels[16][4];
        DecodeColorBlock(dxt1 ? block : block + 8, dxt1, texels);
        if (dxt3) DecodeDxt3Alpha(block, texels);
        if (dxt5) DecodeDxt5Alpha(block, texels);
        for (int i = 0; i < 16; ++i) {
          const std::uint32_t x = bx * 4u + static_cast<std::uint32_t>(i % 4);
          const std::uint32_t y = by * 4u + static_cast<std::uint32_t>(i / 4);
          if (x >= dds.width || y >= dds.height) continue;
          std::memcpy(&(*rgba)[(static_cast<std::size_t>(y) * dds.width + x) * 4u], texels[i], 4);
        }
      }
    }
    return true;
  }

  // Uncompressed. TrackMania stores these as BGRA or BGR.
  if (dds.rgb_bit_count == 32u || dds.rgb_bit_count == 24u) {
    const std::size_t stride = dds.rgb_bit_count / 8u;
    if (dds.data_size < static_cast<std::size_t>(dds.width) * dds.height * stride) return false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(dds.width) * dds.height; ++i) {
      const unsigned char *src = dds.data + i * stride;
      std::uint8_t *dst = &(*rgba)[i * 4u];
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = stride == 4u && dds.has_alpha_channel ? src[3] : 255u;
    }
    return true;
  }
  return false;
}

// --- resampling --------------------------------------------------------------

// Box filter onto the array's page size. Every layer of a texture array is the
// same size, and the game's textures are not, so one of the two has to give.
void Resample(const std::vector<std::uint8_t> &src, std::uint32_t sw, std::uint32_t sh, std::vector<std::uint8_t> *dst,
              std::uint32_t dw, std::uint32_t dh) {
  dst->assign(static_cast<std::size_t>(dw) * dh * 4u, 0u);
  for (std::uint32_t y = 0; y < dh; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * sh / dh);
    const std::uint32_t y1 = std::max(y0 + 1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * sh / dh));
    for (std::uint32_t x = 0; x < dw; ++x) {
      const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * sw / dw);
      const std::uint32_t x1 =
          std::max(x0 + 1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * sw / dw));
      std::uint32_t sum[4] = {0, 0, 0, 0};
      std::uint32_t count = 0u;
      for (std::uint32_t sy = y0; sy < y1 && sy < sh; ++sy) {
        for (std::uint32_t sx = x0; sx < x1 && sx < sw; ++sx) {
          const std::uint8_t *p = &src[(static_cast<std::size_t>(sy) * sw + sx) * 4u];
          for (int c = 0; c < 4; ++c) sum[c] += p[c];
          ++count;
        }
      }
      std::uint8_t *out = &(*dst)[(static_cast<std::size_t>(y) * dw + x) * 4u];
      for (int c = 0; c < 4; ++c) out[c] = count ? static_cast<std::uint8_t>(sum[c] / count) : 0u;
    }
  }
}

// --- picking a material's diffuse texture ------------------------------------

// A material references everything it samples: the picture on the surface, and
// beside it the environment map it reflects, the cloud layer in that
// reflection, a specular ramp, a normal map, a baked lightmap. Only the first is
// the surface's own appearance.
//
// Some are named outright — "DefaultEnvCubic", "SpecularCube". The rest are
// named by a single trailing letter, which is TrackMania's convention: D for the
// picture, N for the normals, S for specular, L for the baked light.
//
// That letter cannot be read on its own, because plenty of names simply end in
// one: IslandBeach, IslandTransition, StadiumGrass. What marks a suffix is
// having a sibling — either the same name without the letter, or another name
// with the same stem and a different letter. Judging a name in isolation is what
// left half of Island untextured, and judging it against its siblings is what
// this does instead.
enum TextureRole {
  TEXTURE_ROLE_REJECT = 0, // never the surface: normals, specular, environment
  TEXTURE_ROLE_LIGHT,      // a baked lightmap — the right shape, the wrong colour
  TEXTURE_ROLE_PLAIN,      // no suffix, so the picture itself
  TEXTURE_ROLE_DIFFUSE,    // says so
};

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// A texture named for what it is rather than by a suffix. These are never a
// surface's own appearance whatever else is around them.
bool IsNamedSupport(const std::string &lower_name) {
  static const char *const kReject[] = {
      "envmap",   "envcubic", "cube",  "cloud",  "specular", "hemispec", "fresnel", "reflec",
      "selfillum", "shadow",  "normal", "bump",  "noise",    "sprite",   "flare",   "distort",
      "ramp",     "gloss",    "damage",
  };
  for (const char *pattern : kReject)
    if (lower_name.find(pattern) != std::string::npos) return true;
  return false;
}

TextureRole RoleOfSuffix(char letter) {
  switch (letter) {
  case 'd': return TEXTURE_ROLE_DIFFUSE;
  case 'l': return TEXTURE_ROLE_LIGHT;
  case 'n':
  case 's':
  case 'i':
  case 'o':
  case 'h': return TEXTURE_ROLE_REJECT;
  default: return TEXTURE_ROLE_PLAIN;
  }
}

struct TextureCandidate {
  std::string path;
  std::string base; // lowercased name without any extension
  TextureRole role = TEXTURE_ROLE_PLAIN;
};

// Every texture a file names, with the ones named outright as support dropped.
void CollectTextures(const GbxFile &file, std::vector<TextureCandidate> *out) {
  for (const GbxReference &reference : file.references) {
    const std::string lower = Lower(reference.name);
    if (lower.find(".texture.gbx") == std::string::npos) continue;
    if (IsNamedSupport(lower)) continue;
    const std::size_t dot = lower.find('.');
    out->push_back(TextureCandidate{reference.path, dot == std::string::npos ? lower : lower.substr(0u, dot),
                                    TEXTURE_ROLE_PLAIN});
  }
}

// Decides each candidate's role by what it sits beside. A trailing letter is a
// suffix when some sibling shares the stem under it, and is part of the word
// otherwise.
void ScoreCandidates(std::vector<TextureCandidate> *candidates) {
  std::unordered_map<std::string, int> stems;
  std::unordered_map<std::string, int> names;
  for (const TextureCandidate &candidate : *candidates) {
    ++names[candidate.base];
    if (candidate.base.size() >= 2u) {
      const char last = candidate.base.back();
      if (last >= 'a' && last <= 'z') ++stems[candidate.base.substr(0u, candidate.base.size() - 1u)];
    }
  }

  for (TextureCandidate &candidate : *candidates) {
    if (candidate.base.size() < 2u) continue;
    const char last = candidate.base.back();
    if (last < 'a' || last > 'z') continue;
    const std::string stem = candidate.base.substr(0u, candidate.base.size() - 1u);
    const bool has_sibling = names.count(stem) != 0u || stems[stem] > 1;
    if (has_sibling) candidate.role = RoleOfSuffix(last);
  }
}

} // namespace

// --- the library -------------------------------------------------------------

TextureLibrary::TextureLibrary() = default;
TextureLibrary::~TextureLibrary() = default;

std::optional<std::string> TextureLibrary::DiffuseImagePath(const PackSet &packs, const std::string &material_path) {
  GbxFile material;
  if (!packs.References(material_path, &material)) return std::nullopt;

  std::vector<TextureCandidate> candidates;
  CollectTextures(material, &candidates);

  // A material names its textures directly or, just as often, only names the
  // shaders that sample them. Both spellings are in use across the same
  // environment — the stadium car names its own, the sport car names shaders —
  // so when the material itself says nothing, the shaders it uses are asked.
  if (candidates.empty()) {
    for (const GbxReference &reference : material.references) {
      if (Lower(reference.name).find(".shader.gbx") == std::string::npos) continue;
      GbxFile shader;
      if (!packs.References(reference.path, &shader)) continue;
      CollectTextures(shader, &candidates);
    }
  }
  if (candidates.empty()) return std::nullopt;

  ScoreCandidates(&candidates);
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                  [](const TextureCandidate &c) { return c.role == TEXTURE_ROLE_REJECT; }),
                   candidates.end());
  if (candidates.empty()) return std::nullopt;

  // A file usually names its own picture before the maps that modify it, so the
  // order it was written in breaks ties between equally likely candidates.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const TextureCandidate &a, const TextureCandidate &b) { return a.role > b.role; });

  // A texture file is itself only a reference to an image, plus the sampling
  // state the engine here has no use for.
  for (const TextureCandidate &candidate : candidates) {
    GbxFile texture;
    if (!packs.References(candidate.path, &texture)) continue;
    for (const GbxReference &image : texture.references) {
      const std::string lower = Lower(image.name);
      if (lower.find(".dds") != std::string::npos || lower.find(".tga") != std::string::npos) return image.path;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> TextureLibrary::Layer(const PackSet &packs, const std::string &material_path) {
  const auto cached = by_material_.find(material_path);
  if (cached != by_material_.end()) return cached->second;

  const std::optional<std::string> image = DiffuseImagePath(packs, material_path);
  if (!image) {
    by_material_.emplace(material_path, std::nullopt);
    return std::nullopt;
  }

  // Two materials very often paint the same picture, so layers are keyed on the
  // image rather than on the material that asked for it.
  const auto shared = by_image_.find(*image);
  if (shared != by_image_.end()) {
    by_material_.emplace(material_path, shared->second);
    return shared->second;
  }

  std::vector<unsigned char> bytes;
  std::uint32_t width = 0u, height = 0u;
  std::vector<std::uint8_t> rgba;
  if (!packs.Read(*image, &bytes) || !DecodeDds(bytes, &width, &height, &rgba)) {
    by_image_.emplace(*image, std::nullopt);
    by_material_.emplace(material_path, std::nullopt);
    return std::nullopt;
  }

  if (layers_.size() >= kMaxTextureLayers) {
    by_material_.emplace(material_path, std::nullopt);
    return std::nullopt;
  }

  std::vector<std::uint8_t> page;
  if (width == kTexturePageSize && height == kTexturePageSize) {
    page = std::move(rgba);
  } else {
    Resample(rgba, width, height, &page, kTexturePageSize, kTexturePageSize);
  }

  const std::uint32_t layer = static_cast<std::uint32_t>(layers_.size());
  layers_.push_back(std::move(page));
  by_image_.emplace(*image, layer);
  by_material_.emplace(material_path, layer);
  return layer;
}

void TextureLibrary::Clear() {
  layers_.clear();
  by_material_.clear();
  by_image_.clear();
  uploaded_ = 0u;
}

bool TextureLibrary::Upload(ft_game *game) {
  const ft_engine_api *api = game->engine;
  if (game->headless || layers_.empty() || !api->texture_create || !api->texture_update_layer) return false;
  if (texture_ != nullptr && uploaded_ >= layers_.size()) return true;

  // The array has to be created at its final size, so any layer discovered
  // after the upload would need a new one. Everything is decoded at load, so
  // this happens once.
  if (texture_ != nullptr) {
    api->texture_destroy(texture_);
    texture_ = nullptr;
  }

  ft_texture_desc desc{};
  desc.struct_size = sizeof(desc);
  desc.pixels = nullptr;
  desc.width = kTexturePageSize;
  desc.height = kTexturePageSize;
  desc.layers = static_cast<std::uint32_t>(layers_.size());
  desc.format = FT_TEXTURE_RGBA8;
  desc.mipmaps = true;
  desc.linear_filter = true;
  texture_ = api->texture_create(&desc);
  if (texture_ == nullptr) return false;

  for (std::size_t i = 0; i < layers_.size(); ++i) {
    api->texture_update_layer(texture_, static_cast<std::uint32_t>(i), layers_[i].data(), kTexturePageSize,
                              kTexturePageSize);
  }
  uploaded_ = layers_.size();
  Log(game, FT_LOG_INFO, "Loaded %zu track textures at %ux%u.", layers_.size(), kTexturePageSize, kTexturePageSize);
  return true;
}

void TextureLibrary::Destroy(ft_game *game) {
  if (texture_ != nullptr && game->engine->texture_destroy) game->engine->texture_destroy(texture_);
  texture_ = nullptr;
  uploaded_ = 0u;
}

} // namespace tmnf
