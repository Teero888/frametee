// Synthetic format regressions, plus optional checks against installed assets.
// Usage: tmnf_render_assets [data/games/tmnf]
#include "tmnf_internal.h"
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tmnf {
bool GpuUploadTextures(ft_game *) { return false; }
void Log(const ft_game *, ft_log_level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}
} // namespace tmnf

namespace {
using Bytes = std::vector<unsigned char>;
void Require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void Put(Bytes &b, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    b.at(offset + i) = static_cast<unsigned char>(value >> (i * 8));
}
void Word(Bytes &b, std::uint32_t value) {
  const auto offset = b.size();
  b.resize(offset + 4);
  Put(b, offset, value);
}
void String(Bytes &b, const std::string &s) {
  Word(b, static_cast<std::uint32_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}
Bytes Dds(unsigned width, unsigned height, unsigned bits, std::array<unsigned, 4> masks, unsigned flags = 0x41) {
  Bytes b(128);
  Put(b, 0, 0x20534444);
  Put(b, 4, 124);
  Put(b, 8, 0x1007);
  Put(b, 12, height);
  Put(b, 16, width);
  Put(b, 76, 32);
  Put(b, 80, flags);
  Put(b, 88, bits);
  for (unsigned i = 0; i < 4; ++i)
    Put(b, 92 + i * 4, masks[i]);
  return b;
}
void TestDds() {
  std::uint32_t w = 0, h = 0;
  Bytes rgba;
  auto b = Dds(1, 1, 32, {0xff, 0xff00, 0xff0000, 0xff000000});
  b.insert(b.end(), {17, 34, 51, 68});
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && w == 1 && h == 1 && rgba == Bytes({17, 34, 51, 68}),
          "RGBA channel masks");
  b = Dds(1, 2, 24, {0xff0000, 0xff00, 0xff, 0}, 0x40);
  Put(b, 8, 0x100f);
  Put(b, 20, 4);
  b.insert(b.end(), {3, 2, 1, 0xee, 6, 5, 4, 0xee});
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba == Bytes({1, 2, 3, 255, 4, 5, 6, 255}), "DDS padded row pitch");
  b = Dds(1, 1, 16, {0xf800, 0x7e0, 0x1f, 0}, 0x40);
  b.insert(b.end(), {0, 0xf8});
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba == Bytes({255, 0, 0, 255}), "RGB565 expansion");
  b = Dds(1, 1, 8, {0xff, 0, 0, 0}, 0x20000);
  b.push_back(93);
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba == Bytes({93, 93, 93, 255}), "DDS luminance");
  b = Dds(4, 4, 0, {}, 4);
  Put(b, 84, 0x31545844);
  b.resize(136);
  Put(b, 132, 0xffffffff);
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba[3] == 0 && rgba[63] == 0, "DXT1 punchthrough alpha");
  b = Dds(1, 1, 0, {}, 4);
  Put(b, 84, 0x33545844);
  b.resize(144);
  b[128] = 0x88;
  b[136] = 0xff;
  b[137] = 0xff;
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba == Bytes({255, 255, 255, 136}),
          "DXT3 explicit alpha and partial block");
  Put(b, 84, 0x35545844);
  b[128] = 0;
  b[129] = 255;
  b[130] = 6;
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba[3] == 0, "DXT5 zero alpha endpoint");
  b[130] = 7;
  Require(tmnf::DecodeDds(b, &w, &h, &rgba) && rgba[3] == 255, "DXT5 full alpha endpoint");
  b.pop_back();
  Require(!tmnf::DecodeDds(b, &w, &h, &rgba), "Truncated DXT block accepted");
  Put(b, 16, 0xffffffff);
  Require(!tmnf::DecodeDds(b, &w, &h, &rgba), "Oversized DDS accepted");
  Require(!tmnf::DecodeDds({}, &w, &h, &rgba) && !tmnf::DecodeDds(b, nullptr, &h, &rgba),
          "Invalid DDS request accepted");
}
Bytes Gbx(std::uint32_t class_id, bool external = false) {
  Bytes b = {'G', 'B', 'X', 6, 0, 'B', 'U', 'U', 'R'};
  Word(b, class_id);
  Word(b, 0);
  Word(b, 2);
  Word(b, external ? 1 : 0);
  return b;
}
void Write(const std::filesystem::path &path, const Bytes &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  Require(bool(out), "Could not write synthetic fixture");
}
Bytes Material(unsigned flags) {
  auto b = Gbx(0x09079000);
  for (unsigned word :
       {0x09079007u, 1u, 0x0903a000u, 0x0903a004u, 2u, 0x12345678u, 0x87654321u, 0x0903a006u, 0u, 0x0903a00au})
    Word(b, word);
  // Both constant arrays are nonempty; render flags follow the second array.
  for (int stage = 0; stage < 2; ++stage) {
    Word(b, 1);
    if (stage == 0) Word(b, 3);
    Word(b, 0x40000000);
    String(b, stage == 0 ? "VertexConstant" : "PixelConstant");
    for (unsigned word : {4u, 1u, 1u, 0u, 0u, 0u, 0u})
      Word(b, word);
  }
  for (unsigned word : {0x0903a00du, 0u, 0x7f00u, flags, 0u, 0xfacade01u, 0xfacade01u})
    Word(b, word);
  return b;
}
struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("tmnf-render-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
void TestMaterials() {
  TempDirectory tmp;
  const auto data = tmp.path / "GameData";
  Write(data / "MixedCase" / "Surface.Material.Gbx", Material(0));
  tmnf::PackSet packs;
  packs.Open((tmp.path / "Packs").string(), {});
  for (unsigned flags : {0u, 0x80u, 0x100u, 0x480u}) {
    Write(data / "MixedCase" / "Surface.Material.Gbx", Material(flags));
    tmnf::MaterialStyle style;
    style.transparent = true;
    std::vector<tmnf::MaterialTextureSlot> slots;
    Require(tmnf::ReadMaterialTextures(packs, "mixedcase/surface.material.gbx", &slots, &style),
            "Material GPU constants/chunk parsing");
    Require(style.transparent == bool(flags & 0x100) && style.alpha_test == bool(flags & 0x80) &&
                style.double_sided == bool(flags & 0x400),
            "Material alpha/culling requirements");
  }
  auto b = Gbx(0x09005000, true);
  Word(b, 7);
  Word(b, 0);
  Word(b, 1);
  String(b, "StadiumStartSign.Material.Gbx");
  Word(b, 1);
  Word(b, 1);
  Word(b, 0);
  Word(b, 0xfacade01);
  Write(data / "hashed.gbx", b);
  tmnf::GbxFile file;
  Require(packs.References("hashed.gbx", &file) && file.references.size() == 1 && file.references[0].node_index == 1 &&
              file.references[0].path == "StadiumStartSign.Material.Gbx",
          "Unresolvable hashed-solid references were discarded");
  std::vector<tmnf::MaterialTextureSlot> slots = {{"Grass", "", 0, "blades"}, {"GDiffuse", "", 0, "ground"}};
  Require(tmnf::DiffuseTextureOf(slots) == "ground", "Grass cards selected as ground diffuse");
}
void TestInstalled(const std::string &root) {
  tmnf::PackSet packs;
  Require(packs.Open(root + "/Packs", {"Stadium", "Game", "Resource"}), "Cannot open installed TMNF packs");
  tmnf::TextureLibrary textures;
  textures.SetMood("Stadium", "Sunset");
  auto road = textures.Material(packs, "Stadium\\Media\\Material\\StadiumRoadRace.Material.Gbx");
  Require(road.diffuse != tmnf::kNoTextureLayer && road.normal != tmnf::kNoTextureLayer && !road.style.transparent &&
              !road.style.alpha_test,
          "Start/finish road material maps and opacity");
  auto light = textures.Material(packs, "Stadium\\Media\\Material\\StadiumStartSignGlow.Material.Gbx");
  Require(light.style.additive && light.animation.kind == tmnf::TextureAnimationKind::StartLights &&
              light.animation.frame_count == 3,
          "Start light states/additive material");
  const auto &lamp = textures.Pages().at(light.diffuse);
  const auto offset = (std::size_t(lamp.height / 2) * lamp.width + lamp.width / 6) * 4;
  Require(lamp.rgba[offset + 1] > lamp.rgba[offset] * 1.5f && lamp.rgba[offset + 1] > 60,
          "Green lamp is not centered across housing");
#ifdef TMNF_HAS_FFMPEG
  const auto left = textures.DirectionSignLayer(packs, true), right = textures.DirectionSignLayer(packs, false);
  Require(left && right && left != right, "Left/right boards use identical video");
  Require(textures.Animation(*left).frame_count > 1 && textures.Animation(*right).frame_count > 1,
          "Bink boards did not decode all frames");
#endif
  textures.SetMood("Stadium", "Day");
  Require(std::fabs(textures.MoodTime(packs) - .65f) < .0001f, "Day decoration time");
  const auto sun = textures.MoodColor(packs, "LightSun", {});
  Require(sun.r > .99f && sun.g > .99f && sun.b > .99f, "Day light gradient sampled at wrong time");
  textures.SetMood("Stadium", "Night");
  Require(std::fabs(textures.MoodTime(packs) - .15f) < .0001f, "Night decoration time");
  const auto night = textures.MoodColor(packs, "LightSun", {1, 1, 1, 1});
  Require(night.r == 0 && night.g == 0 && night.b == 0, "Night reused daylight color");
  std::puts("Installed start/finish materials, start lights, animated boards, and mood lighting passed.");
}
} // namespace

int main(int argc, char **argv) {
  try {
    TestDds();
    TestMaterials();
    std::puts("DDS, material flags, and hashed reference regressions passed.");
    if (argc > 1) TestInstalled(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
