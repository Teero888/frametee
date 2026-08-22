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
  // it can possibly be worth on screen. Stadium's advertising hoardings are
  // genuinely 2048, so that is the line rather than anything below it.
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

// --- what a texture's alpha channel is for -----------------------------------

// TrackMania puts two completely different things in the alpha of a diffuse
// texture, and which one it is decides whether sampling it as opacity produces
// a fence or a hole in a wall.
//
// A cut-out — a chain-link fence, a railing, a tree card, a sign with a shaped
// edge — has alpha that is almost entirely on or off. Everything else is using
// the channel to carry specular strength or gloss, which varies smoothly and
// means nothing as an opacity: sample that and a car body turns to glass.
//
// The shader would say which, but a TrackMania material references its shader as
// an embedded node rather than a file, so there is no shader name to look up.
// The distribution of the channel says it just as well.
enum class AlphaUse : std::uint8_t {
  Opaque, // nothing there: the channel is unused or solid
  Cutout, // an opacity, and the surface has to be drawn with it
  Gloss,  // a material property that happens to live in this channel
};

AlphaUse ClassifyAlpha(const std::vector<std::uint8_t> &rgba) {
  const std::size_t texels = rgba.size() / 4u;
  if (texels == 0u) return AlphaUse::Opaque;

  std::size_t clear = 0u, solid = 0u;
  for (std::size_t i = 0; i < texels; ++i) {
    const std::uint8_t a = rgba[i * 4u + 3u];
    if (a < 16u) ++clear;
    else if (a > 240u) ++solid;
  }
  if (clear == 0u) return solid == texels ? AlphaUse::Opaque : AlphaUse::Gloss;

  // Some of it is see-through. That is only an opacity if the rest is not a
  // ramp: a twentieth of the picture in between is the soft edge a cut-out has,
  // and more than that is a gradient nobody meant as transparency.
  const std::size_t between = texels - clear - solid;
  return between * 20u <= texels ? AlphaUse::Cutout : AlphaUse::Gloss;
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

// --- how a material is drawn -------------------------------------------------

// TrackMania's shaders, and what each one means on screen.
//
// A material is a texture plus a shader, and it is the shader that says whether
// the surface is transparent, two-sided, unlit, textured from the world, or not
// meant to be drawn at all. Shaders are shared across every environment — the
// same "TDiff PX2 Trans 2Sided" is behind a stadium fence and an island railing
// — so this table is short and covers all of them.
//
// The classifications are taken from GbxTools3D, a viewer for these same
// environments, which has already established which shader means what:
// https://github.com/BigBang1112/gbx-tools-3d. The names are matched with the
// extension stripped and separators normalised, so a reference written
// "Techno\Media\Material\Sky.Shader.Gbx" finds "techno/media/material/sky".
struct ShaderStyleEntry {
  const char *name;
  MaterialStyle style;
};

const ShaderStyleEntry kShaderStyles[] = {
    // Terrain: textured by where it is rather than by authored coordinates.
    {"techno/media/material/pdiff pdiff pa px2", MaterialStyle{.world_uv = true}},
    {"techno2/media/material/pdiff pdiff pa px2 grass2", MaterialStyle{.world_uv = true}},
    {"techno2/media/material/pdiff pdiff pa tocc px2 grass", MaterialStyle{.world_uv = true}},
    {"techno2/media/material/pdiff pdiff pa tocc px2 grass nolightv", MaterialStyle{.world_uv = true}},
    {"techno/media/material/pdiff fresnel px2", MaterialStyle{.world_uv = true}},
    {"techno/media/material/pdisp pdiff px2", MaterialStyle{.world_uv = true}},
    {"techno2/media/material/soilgen21", MaterialStyle{.world_uv = true}},
    {"techno3/media/material/tech3 block pdiff_spec_norm", MaterialStyle{.world_uv = true}},
    {"techno3/media/material/tech3 block pdiff_spec_norm grassx2", MaterialStyle{.world_uv = true}},
    {"techno3/media/material/tech3 warp pyapxzdiff", MaterialStyle{.world_uv = true}},
    {"techno3/media/material/tech3 warp_pyadiff_to_pdiffpgrassx2", MaterialStyle{.world_uv = true}},

    // The sky, drawn as authored from inside the shell it is painted on.
    {"techno/media/material/sky", MaterialStyle{.unlit = true, .double_sided = true}},
    {"sky/media/material/skyday", MaterialStyle{.unlit = true, .double_sided = true}},
    {"island/media/material/islandsky", MaterialStyle{.unlit = true, .double_sided = true}},
    {"techno3/media/material/sky/tech3 sky", MaterialStyle{.unlit = true, .double_sided = true}},

    // Alpha-cut surfaces: fences, railings, foliage cards, glass.
    {"techno/media/material/tdiff px2 trans", MaterialStyle{.double_sided = true, .transparent = true}},
    {"techno/media/material/tdiff px2 trans 2sided", MaterialStyle{.double_sided = true, .transparent = true}},
    {"techno/media/material/tdiff px2 trans normy pc3only", MaterialStyle{.double_sided = true, .transparent = true}},
    {"techno/media/material/tdiffg px2 cspec fcout trans", MaterialStyle{.transparent = true}},
    {"techno/media/material/tdiffg px2 cspecl trans", MaterialStyle{.transparent = true}},
    {"techno2/media/material/tdiff_spec_nrm tocc cspecsoft", MaterialStyle{.transparent = true}},
    {"techno2/media/material/tdiff_spec_nrm tocc cspecsoft nolightv", MaterialStyle{.transparent = true}},
    {"techno2/media/material/tdiff_spec_nrm tocc cspecsoft trans", MaterialStyle{.transparent = true}},
    {"techno3/media/material/tech3 block tdiffa_spec_norm", MaterialStyle{.transparent = true}},
    {"techno3/media/material/tech3_block_tdiffablend_specnorm_cubeout", MaterialStyle{.transparent = true}},
    {"island/media/material/modelalpha1sidedlight", MaterialStyle{.transparent = true}},
    {"island/media/material/modelalpha2sidednolight",
     MaterialStyle{.double_sided = true, .transparent = true}},
    {"island/media/material/islandwindowsmip", MaterialStyle{.transparent = true}},
    {"vehicles/media/material/sportcarglass", MaterialStyle{.transparent = true}},

    // Light that adds to what is behind it.
    {"techno/media/material/tadd", MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"techno/media/material/tadd zbias", MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"techno/media/material/tadd night", MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"techno/media/material/tadd night zbias",
     MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"techno2/media/material/tselfi add", MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"island/media/material/modellightvolume",
     MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"alpine/media/material/alpinesignsselfillum",
     MaterialStyle{.unlit = true, .transparent = true, .additive = true}},
    {"island/media/material/islandbeachfoam", MaterialStyle{.unlit = true, .transparent = true, .additive = true}},

    // Never drawn. These are collision and depth stand-ins that live in the
    // same geometry as the surfaces around them, which is why leaving them in
    // gives a track a layer of flat panels fighting it for the depth buffer.
    {"techno2/media/material/vdep fence", MaterialStyle{.invisible = true}},
    {"techno/media/material/shadowskirt", MaterialStyle{.invisible = true}},

    {"techno/media/material/sea", MaterialStyle{.transparent = true, .water = true}},
    {"techno/media/material/seamultiy", MaterialStyle{.transparent = true, .water = true}},
    {"techno3/media/material/tech3 sea", MaterialStyle{.transparent = true, .water = true}},
};

// A resolved plain path, as the table spells it: lowercased, forward slashes,
// and everything from the first extension dot onwards removed.
std::string ShaderKey(std::string_view path) {
  std::string key = Lower(path);
  for (char &c : key)
    if (c == '\\') c = '/';
  const std::size_t dot = key.find('.');
  if (dot != std::string::npos) key.resize(dot);
  return key;
}

const MaterialStyle *StyleForKey(const std::string &key) {
  for (const ShaderStyleEntry &entry : kShaderStyles)
    if (key == entry.name) return &entry.style;
  return nullptr;
}

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

// The image a `.Texture.gbx` descriptor points at. A texture file is itself
// only a reference to a picture plus the sampling state the engine here has no
// use for.
std::optional<std::string> ImageOfTexture(const PackSet &packs, const std::string &texture_path) {
  GbxFile texture;
  if (!packs.References(texture_path, &texture)) return std::nullopt;
  for (const GbxReference &image : texture.references) {
    const std::string lower = Lower(image.name);
    if (lower.find(".dds") != std::string::npos || lower.find(".tga") != std::string::npos) return image.path;
  }
  return std::nullopt;
}

std::optional<std::string> TextureLibrary::DiffuseImagePath(const PackSet &packs, const std::string &material_path) {
  // What the material actually says. A sampler named "Diffuse" is the surface's
  // own picture, and nothing about a file name is as reliable as that.
  std::vector<MaterialTextureSlot> slots;
  if (ReadMaterialTextures(packs, material_path, &slots)) {
    if (const std::optional<std::string> diffuse = DiffuseTextureOf(slots)) {
      if (std::optional<std::string> image = ImageOfTexture(packs, *diffuse)) return image;
    }
  }

  // Falling back to reading the names. Some materials bind their pictures
  // through routes this module does not decode, and a track drawn from a good
  // guess is worth more than one drawn in flat colours.
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

  for (const TextureCandidate &candidate : candidates) {
    if (std::optional<std::string> image = ImageOfTexture(packs, candidate.path)) return image;
  }
  return std::nullopt;
}

MaterialStyle TextureLibrary::Style(const PackSet &packs, const std::string &material_path) {
  const auto cached = style_by_material_.find(material_path);
  if (cached != style_by_material_.end()) return cached->second;

  MaterialStyle style;
  GbxFile material;
  if (packs.References(material_path, &material)) {
    // A material is a texture plus the thing that says how the surface behaves,
    // and environments disagree about what that thing is called. Techno and
    // Island name a .Shader.Gbx; Stadium names another .Material.Gbx and
    // inherits from it -- StadiumWarpSpotsGlow is "TAdd Night ZBias", which is
    // the additive shader the table already knows under that name. Looking only
    // for the first spelling left every Stadium surface on the default opaque
    // style: no additive glows, no world-space grass, no cut-out signs.
    //
    // Textures are always .Texture.Gbx, so the two kinds of reference cannot be
    // confused with a material's pictures.
    for (const GbxReference &reference : material.references) {
      const std::string lower = Lower(reference.name);
      const bool is_base = lower.find(".shader.gbx") != std::string::npos ||
                           lower.find(".material.gbx") != std::string::npos;
      if (!is_base) continue;
      // A material that names itself says nothing about itself.
      if (Lower(reference.path) == Lower(material_path)) continue;
      if (const MaterialStyle *found = StyleForKey(ShaderKey(reference.path))) {
        style = *found;
        break;
      }
    }
  }
  if (const MaterialStyle *found = StyleForKey(ShaderKey(material_path))) style = *found;

  // A picture whose alpha is a cut-out says what the missing shader would have:
  // this is a fence, a railing, a shaped sign — a sheet, drawn with its opacity
  // and looked at from both sides.
  if (const std::optional<std::uint32_t> layer = Layer(packs, material_path)) {
    if (layers_[*layer].alpha_used) {
      style.transparent = true;
      style.double_sided = true;
    }
  }

  style_by_material_.emplace(material_path, style);
  return style;
}

namespace {

// A signal lamp's glow is not one picture but three, laid out corner to corner
// along the texture's diagonal: green, then amber, then red, each in its own
// third. The game slides the sampled window down that diagonal to light one
// colour at a time, and the mesh it does that on carries UVs spanning the whole
// sheet -- so drawing the sheet as-is puts all three lamps on the quad at once,
// which is what the start gantry has been showing.
//
// There is no race clock here to slide the window with, so the frame the track
// is left sitting in is the one that gets drawn: green, the same one the game
// leaves lit once a run is under way.
bool CropDiagonalLampFrame(const std::string &image, std::uint32_t *width, std::uint32_t *height,
                           std::vector<std::uint8_t> *rgba) {
  constexpr std::uint32_t kFrames = 3u;
  const std::string lower = Lower(image);
  if (lower.find("stadiumstartsignglow") == std::string::npos) return false;
  if (*width != *height || *width < kFrames * 4u) return false;

  // The sheet does not divide evenly (the stadium's is 64 across), so the frame
  // is the floor of a third: a hair inside the cell, which keeps the next lamp
  // out rather than letting a sliver of it bleed in.
  const std::uint32_t side = *width / kFrames;
  std::vector<std::uint8_t> lit(static_cast<std::size_t>(side) * side * 4u);
  for (std::uint32_t y = 0; y < side; ++y)
    std::memcpy(&lit[static_cast<std::size_t>(y) * side * 4u],
                &(*rgba)[static_cast<std::size_t>(y) * *width * 4u],
                static_cast<std::size_t>(side) * 4u);
  *rgba = std::move(lit);
  *width = side;
  *height = side;
  return true;
}

}  // namespace

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

  CropDiagonalLampFrame(*image, &width, &height, &rgba);

  if (layers_.size() >= kMaxTextureLayers) {
    by_material_.emplace(material_path, std::nullopt);
    return std::nullopt;
  }

  // Kept at the size it was authored. What page it ends up on is decided once,
  // when the whole track's textures are known; see Upload.
  const std::uint32_t layer = static_cast<std::uint32_t>(layers_.size());
  const bool cutout = ClassifyAlpha(rgba) == AlphaUse::Cutout;
  layers_.push_back(Page{std::move(rgba), width, height, cutout});
  by_image_.emplace(*image, layer);
  by_material_.emplace(material_path, layer);
  return layer;
}

std::optional<std::uint32_t> TextureLibrary::SkyLayer(const PackSet &packs, const std::string &environment,
                                                      const std::string &mood) {
  if (environment.empty() || mood.empty()) return std::nullopt;
  // Not a file, so it cannot collide with one: the pack paths this is keyed
  // beside all start with an environment name.
  const std::string key = "@sky\\" + environment + "\\" + mood;
  if (const auto shared = by_image_.find(key); shared != by_image_.end()) return shared->second;

  struct Image {
    std::uint32_t width = 0u, height = 0u;
    std::vector<std::uint8_t> rgba;
    bool ok = false;
  };
  const auto load = [&](const char *name) {
    Image image;
    std::vector<unsigned char> bytes;
    const std::string path = environment + "\\Media\\Moods\\" + mood + "\\" + name;
    image.ok = packs.Read(path, &bytes) && DecodeDds(bytes, &image.width, &image.height, &image.rgba);
    return image;
  };

  const Image ceiling = load("SkyCeiling.dds");
  const Image panoramic = load("SkyPanoramic.dds");
  if (!ceiling.ok && !panoramic.ok) {
    by_image_.emplace(key, std::nullopt);
    return std::nullopt;
  }
  if (layers_.size() >= kMaxTextureLayers) return std::nullopt;

  // Both are mapped the same way on the dome — the coordinate runs from the
  // pole at zero to the horizon at one — so compositing them is a straight
  // per-pixel blend at whichever of the two is the larger.
  const std::uint32_t width = std::max(ceiling.width, panoramic.width);
  const std::uint32_t height = std::max(ceiling.height, panoramic.height);
  Page page;
  page.width = width;
  page.height = height;
  page.alpha_used = false;
  page.rgba.assign(static_cast<std::size_t>(width) * height * 4u, 255u);

  const auto sample = [&](const Image &image, std::uint32_t x, std::uint32_t y, std::uint8_t out[4]) {
    const std::uint32_t sx = std::min(image.width - 1u, static_cast<std::uint32_t>(
                                                            static_cast<std::uint64_t>(x) * image.width / width));
    const std::uint32_t sy = std::min(image.height - 1u, static_cast<std::uint32_t>(
                                                             static_cast<std::uint64_t>(y) * image.height / height));
    std::memcpy(out, &image.rgba[(static_cast<std::size_t>(sy) * image.width + sx) * 4u], 4);
  };

  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t *dst = &page.rgba[(static_cast<std::size_t>(y) * width + x) * 4u];
      std::uint8_t base[4] = {128u, 150u, 200u, 255u};
      if (ceiling.ok) sample(ceiling, x, y, base);
      std::memcpy(dst, base, 3);
      if (panoramic.ok) {
        // The panorama is transparent at the pole and opaque at the horizon,
        // which is exactly the sunset band lying over the ceiling behind it.
        std::uint8_t over[4];
        sample(panoramic, x, y, over);
        const std::uint32_t a = over[3];
        for (int c = 0; c < 3; ++c)
          dst[c] = static_cast<std::uint8_t>((over[c] * a + dst[c] * (255u - a)) / 255u);
      }
      dst[3] = 255u;
    }
  }

  const std::uint32_t layer = static_cast<std::uint32_t>(layers_.size());
  layers_.push_back(std::move(page));
  by_image_.emplace(key, layer);
  return layer;
}

void TextureLibrary::Clear() {
  layers_.clear();
  by_material_.clear();
  by_image_.clear();
  style_by_material_.clear();
  uploaded_ = 0u;
  page_size_ = 0u;
}

// The one size every layer of the array is stored at.
//
// The array cannot hold layers of different sizes, so this is a single choice
// made for the whole track. It is the largest side any of its textures were
// actually authored at, because anything smaller throws away detail that is
// there — the stadium's road and its hoardings are 1024 and 2048, and squashing
// them to a fixed 512 is what made every surface read as a blur a few metres
// ahead of the car. It is then halved until the array fits the memory budget,
// which is what keeps an environment with a hundred and fifty textures from
// asking for a gigabyte of them.
std::uint32_t TextureLibrary::ChoosePageSize() const {
  std::uint32_t largest = kMinTexturePageSize;
  for (const Page &page : layers_) largest = std::max({largest, page.width, page.height});

  // Round up to a power of two: a mip chain is exact on one and lopsided on
  // anything else, and every one of the game's own textures is one already.
  std::uint32_t size = kMinTexturePageSize;
  while (size < largest && size < kMaxTexturePageSize) size *= 2u;

  const std::size_t bytes_per_page = 4u; // RGBA8
  while (size > kMinTexturePageSize &&
         static_cast<std::size_t>(size) * size * bytes_per_page * layers_.size() > kTextureMemoryBudget) {
    size /= 2u;
  }
  return size;
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

  page_size_ = ChoosePageSize();

  ft_texture_desc desc{};
  desc.struct_size = sizeof(desc);
  desc.pixels = nullptr;
  desc.width = page_size_;
  desc.height = page_size_;
  desc.layers = static_cast<std::uint32_t>(layers_.size());
  desc.format = FT_TEXTURE_RGBA8;
  desc.mipmaps = true;
  desc.linear_filter = true;
  texture_ = api->texture_create(&desc);
  if (texture_ == nullptr) return false;

  // A texture that is not square is stretched onto a square page rather than
  // letterboxed. Coordinates are normalised, so the picture comes out the shape
  // the surface asks for either way; what a stretch costs is resolution along
  // the shorter side, and what letterboxing would cost is bleeding at the seam
  // of every tiled surface on the track.
  std::vector<std::uint8_t> scratch;
  std::size_t rescaled = 0u;
  for (std::size_t i = 0; i < layers_.size(); ++i) {
    Page &page = layers_[i];
    // Alpha is only an opacity where a material says so. Everywhere else the
    // game is using the channel to carry specular strength, and plenty of
    // ordinary surfaces store a zero in it — sampling that as opacity is what
    // made the car, its wheels and a good deal of the stadium see-through.
    if (!page.alpha_used) {
      for (std::size_t p = 3u; p < page.rgba.size(); p += 4u) page.rgba[p] = 255u;
    }

    const std::uint8_t *pixels = page.rgba.data();
    if (page.width != page_size_ || page.height != page_size_) {
      Resample(page.rgba, page.width, page.height, &scratch, page_size_, page_size_);
      pixels = scratch.data();
      ++rescaled;
    }
    api->texture_update_layer(texture_, static_cast<std::uint32_t>(i), pixels, page_size_, page_size_);
  }
  uploaded_ = layers_.size();
  Log(game, FT_LOG_INFO, "Loaded %zu track textures at %ux%u (%zu resampled).", layers_.size(), page_size_, page_size_,
      rescaled);
  return true;
}

void TextureLibrary::Destroy(ft_game *game) {
  if (texture_ != nullptr && game->engine->texture_destroy) game->engine->texture_destroy(texture_);
  texture_ = nullptr;
  uploaded_ = 0u;
}

} // namespace tmnf
