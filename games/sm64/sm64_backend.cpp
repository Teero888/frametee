#include "sm64_bridge.h"
#include "sm64_vulkan.h"
#include "full_game/scene.h"
#include "fast3d/gfx_pc.h"
#include "fast3d/gfx_rendering_api.h"
#define SM64_PHYSICS_TYPES_DEFINED 1
#include <sm64_physics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxFrames = 10'000'000;
constexpr size_t kMovieHeaderSize = 0x400;
std::atomic<uint64_t> next_revision{1};

static inline uint32_t read_u32_be(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline int16_t read_s16_be(const uint8_t *p) {
  return int16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

[[maybe_unused]] static inline uint16_t read_u16_be(const uint8_t *p) {
  return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static bool decompress_mio0(const uint8_t *src, size_t src_size, std::vector<uint8_t> &dest) {
  if (!src || src_size < 16) return false;
  uint32_t magic = read_u32_be(src);
  if (magic != 0x4D494F30) return false; // "MIO0"
  uint32_t dest_size = read_u32_be(src + 4);
  uint32_t comp_offset = read_u32_be(src + 8);
  uint32_t uncomp_offset = read_u32_be(src + 12);
  if (comp_offset > src_size || uncomp_offset > src_size) return false;

  dest.clear();
  dest.reserve(dest_size);

  uint32_t flag_offset = 16;
  uint32_t flags = 0;
  int bit_idx = 0;

  while (dest.size() < dest_size) {
    if (bit_idx == 0) {
      if (flag_offset + 4 > src_size) return false;
      flags = read_u32_be(src + flag_offset);
      flag_offset += 4;
      bit_idx = 32;
    }
    bool bit = (flags >> 31) & 1;
    flags <<= 1;
    bit_idx--;

    if (bit) {
      if (uncomp_offset >= src_size) return false;
      dest.push_back(src[uncomp_offset++]);
    } else {
      if (comp_offset + 2 > src_size) return false;
      uint8_t b1 = src[comp_offset++];
      uint8_t b2 = src[comp_offset++];
      uint32_t length = (b1 >> 4) + 3;
      uint32_t disp = ((b1 & 0x0F) << 8) | b2;
      if (dest.size() < disp + 1) return false;
      size_t offset = dest.size() - (disp + 1);
      for (uint32_t i = 0; i < length && dest.size() < dest_size; ++i) {
        dest.push_back(dest[offset + i]);
      }
    }
  }
  return true;
}

void write_error(char *out, size_t size, const std::string &message) {
  if (!out || size == 0) return;
  const size_t n = std::min(size - 1, message.size());
  std::memcpy(out, message.data(), n);
  out[n] = '\0';
}

template <typename T, typename F>
T boundary(char *error, size_t error_size, T fallback, F &&fn) {
  try {
    return fn();
  } catch (const std::exception &exception) {
    write_error(error, error_size, exception.what());
  } catch (...) {
    write_error(error, error_size, "SM64 backend failed unexpectedly");
  }
  return fallback;
}

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }

std::vector<uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) fail("Cannot open " + path.string());
  const std::streamsize size = file.tellg();
  if (size < 0) fail("Cannot read " + path.string());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), size)) {
    fail("Cannot read " + path.string());
  }
  return bytes;
}

std::vector<uint8_t> normalized_rom(std::vector<uint8_t> bytes) {
  if (bytes.size() != 8 * 1024 * 1024) fail("Expected an 8 MiB vanilla SM64 ROM");
  if (std::equal(bytes.begin(), bytes.begin() + 4, std::array<uint8_t, 4>{0x80, 0x37, 0x12, 0x40}.begin())) return bytes;
  if (std::equal(bytes.begin(), bytes.begin() + 4, std::array<uint8_t, 4>{0x37, 0x80, 0x40, 0x12}.begin())) {
    for (size_t i = 0; i < bytes.size(); i += 2) std::swap(bytes[i], bytes[i + 1]);
    return bytes;
  }
  if (std::equal(bytes.begin(), bytes.begin() + 4, std::array<uint8_t, 4>{0x40, 0x12, 0x37, 0x80}.begin())) {
    for (size_t i = 0; i < bytes.size(); i += 4) std::reverse(bytes.begin() + i, bytes.begin() + i + 4);
    return bytes;
  }
  fail("Unrecognized N64 ROM byte order");
}

struct RomInfo {
  uint32_t crc = 0;
  uint8_t country = 0;
};

RomInfo inspect_rom(const std::vector<uint8_t> &rom) {
  const uint32_t crc = (uint32_t(rom[0x10]) << 24) | (uint32_t(rom[0x11]) << 16) |
                       (uint32_t(rom[0x12]) << 8) | uint32_t(rom[0x13]);
  const uint8_t country = rom[0x3e];
  if (!((crc == 0x635a2bff && country == 'E') || (crc == 0x4eaa3d0e && country == 'J') ||
        (crc == 0xa03cf036 && country == 'P'))) {
    fail("Only original US, JP and EU SM64 revisions are supported");
  }
  return {crc, country};
}

std::string json_string(std::string_view json, std::string_view key, bool required = true) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string_view::npos) {
    if (required) fail("SM64 setup is missing '" + std::string(key) + "'");
    return {};
  }
  size_t pos = json.find(':', key_pos + needle.size());
  if (pos == std::string_view::npos) fail("Invalid SM64 setup");
  while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {}
  if (pos >= json.size() || json[pos] != '\"') fail("Invalid string in SM64 setup");
  ++pos;
  std::string value;
  for (; pos < json.size(); ++pos) {
    const char c = json[pos];
    if (c == '\"') return value;
    if (c != '\\') {
      value.push_back(c);
      continue;
    }
    if (++pos == json.size()) fail("Invalid escape in SM64 setup");
    switch (json[pos]) {
      case '\"': value.push_back('\"'); break;
      case '\\': value.push_back('\\'); break;
      case '/': value.push_back('/'); break;
      case 'n': value.push_back('\n'); break;
      case 'r': value.push_back('\r'); break;
      case 't': value.push_back('\t'); break;
      default: fail("Unsupported escape in SM64 setup");
    }
  }
  fail("Unterminated string in SM64 setup");
}

uint32_t json_u32(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string_view::npos) return 0;
  size_t pos = json.find(':', key_pos + needle.size());
  if (pos == std::string_view::npos) fail("Invalid SM64 setup");
  while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {}
  uint64_t result = 0;
  const size_t start = pos;
  while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
    result = result * 10 + static_cast<uint32_t>(json[pos++] - '0');
    if (result > kMaxFrames) fail("Starting frame exceeds the SM64 frame limit");
  }
  if (pos == start) fail("Invalid starting frame in SM64 setup");
  return static_cast<uint32_t>(result);
}

std::filesystem::path resolve(const std::filesystem::path &base, const std::string &path) {
  if (path.empty()) return {};
  const std::filesystem::path p(path);
  if (p.is_absolute()) return p;
  return base / p;
}

std::string escape_json(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result.push_back(c); break;
    }
  }
  return result;
}

[[maybe_unused]] static std::vector<sm64_input> load_m64_prefix(const std::filesystem::path &path, uint32_t frames, RomInfo rom) {
  if (path.empty()) return std::vector<sm64_input>(frames);
  const auto bytes = read_bytes(path);
  if (bytes.size() < kMovieHeaderSize || std::memcmp(bytes.data(), "M64\x1a", 4) != 0) fail("Invalid M64 movie");
  const uint32_t crc = uint32_t(bytes[0xe4]) | (uint32_t(bytes[0xe5]) << 8) |
                       (uint32_t(bytes[0xe6]) << 16) | (uint32_t(bytes[0xe7]) << 24);
  if (crc != rom.crc || bytes[0xe8] != rom.country) fail("Movie and ROM versions differ");
  const size_t available = (bytes.size() - kMovieHeaderSize) / 4;
  if (frames > available) fail("Starting frame is beyond the movie");
  std::vector<sm64_input> inputs;
  inputs.reserve(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t *input = bytes.data() + kMovieHeaderSize + i * 4;
    inputs.push_back({uint16_t(uint16_t(input[0]) << 8 | input[1]), static_cast<int8_t>(input[2]), static_cast<int8_t>(input[3])});
  }
  return inputs;
}

void write_m64(const std::filesystem::path &path, const RomInfo &rom, const std::vector<sm64_input> &inputs) {
  std::error_code error;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) fail("Cannot write " + path.string());
  std::array<uint8_t, kMovieHeaderSize> header{};
  std::memcpy(header.data(), "M64\x1a", 4);
  header[4] = 3;
  header[12] = header[13] = header[14] = header[15] = 0xff;
  header[20] = 60;
  header[21] = 1;
  const uint32_t count = static_cast<uint32_t>(inputs.size());
  std::memcpy(header.data() + 24, &count, sizeof(count));
  header[28] = 2;
  header[32] = 1;
  std::memcpy(header.data() + 0xc4, "SUPER MARIO 64", 14);
  std::memcpy(header.data() + 0xe4, &rom.crc, sizeof(rom.crc));
  header[0xe8] = rom.country;
  std::memcpy(header.data() + 0x222, "FrameTee", 8);
  file.write(reinterpret_cast<const char *>(header.data()), header.size());
  for (const sm64_input input : inputs) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(input.buttons >> 8), static_cast<uint8_t>(input.buttons), static_cast<uint8_t>(input.stick_x), static_cast<uint8_t>(input.stick_y)};
    file.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
  }
  if (!file) fail("Cannot write " + path.string());
}

std::string get_level_title(uint32_t dest) {
  switch (dest) {
    case 9: return "Super Mario 64 - Bob-omb Battlefield";
    case 24: return "Super Mario 64 - Whomp's Fortress";
    case 12: return "Super Mario 64 - Jolly Roger Bay";
    case 5: return "Super Mario 64 - Cool, Cool Mountain";
    case 4: return "Super Mario 64 - Big Boo's Haunt";
    case 7: return "Super Mario 64 - Hazy Maze Cave";
    case 22: return "Super Mario 64 - Lethal Lava Land";
    case 8: return "Super Mario 64 - Shifting Sand Land";
    case 23: return "Super Mario 64 - Dire, Dire Docks";
    case 10: return "Super Mario 64 - Snowman's Land";
    case 11: return "Super Mario 64 - Wet-Dry World";
    case 36: return "Super Mario 64 - Tall, Tall Mountain";
    case 13: return "Super Mario 64 - Tiny-Huge Island";
    case 14: return "Super Mario 64 - Tick Tock Clock";
    case 15: return "Super Mario 64 - Rainbow Ride";
    case 16: return "Super Mario 64 - Castle Grounds";
    case 6: return "Super Mario 64 - Inside Castle";
    case 26: return "Super Mario 64 - Castle Courtyard";
    case 17: return "Super Mario 64 - Bowser in the Dark World";
    case 19: return "Super Mario 64 - Bowser in the Fire Sea";
    case 21: return "Super Mario 64 - Bowser in the Sky";
    case 30: return "Super Mario 64 - Bowser in the Dark World Battle";
    case 33: return "Super Mario 64 - Bowser in the Fire Sea Battle";
    case 34: return "Super Mario 64 - Bowser in the Sky Battle";
    case 27: return "Super Mario 64 - The Princess's Secret Slide";
    case 28: return "Super Mario 64 - Cavern of the Metal Cap";
    case 29: return "Super Mario 64 - Tower of the Wing Cap";
    case 18: return "Super Mario 64 - Vanish Cap Under the Moat";
    case 31: return "Super Mario 64 - Wing Mario Over the Rainbow";
    case 20: return "Super Mario 64 - The Secret Aquarium";
    default: return "Super Mario 64";
  }
}

// Software Renderer
struct RenderApi {
  bool (*z_is_from_0_to_1)();
  void (*unload_shader)(void *);
  void (*load_shader)(void *);
  void *(*create_and_load_new_shader)(uint32_t);
  void *(*lookup_shader)(uint32_t);
  void (*shader_get_info)(void *, uint8_t *, bool *);
  uint32_t (*new_texture)();
  void (*select_texture)(int, uint32_t);
  void (*upload_texture)(const uint8_t *, int, int);
  void (*set_sampler_parameters)(int, bool, uint32_t, uint32_t);
  void (*set_depth_test)(bool);
  void (*set_depth_mask)(bool);
  void (*set_zmode_decal)(bool);
  void (*set_viewport)(int, int, int, int);
  void (*set_scissor)(int, int, int, int);
  void (*set_use_alpha)(bool);
  void (*draw_triangles)(float *, size_t, size_t);
  void (*init)();
  void (*on_resize)();
  void (*start_frame)();
  void (*end_frame)();
  void (*finish_render)();
  void (*shutdown)();
};

struct Color {
  float r = 0, g = 0, b = 0, a = 1;
};

Color operator+(Color a, Color b) { return {a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a}; }
Color operator-(Color a, Color b) { return {a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a}; }
Color operator*(Color a, Color b) { return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a}; }
Color operator*(Color a, float b) { return {a.r * b, a.g * b, a.b * b, a.a * b}; }
Color clamp_color(Color c) {
  c.r = std::clamp(c.r, 0.0f, 1.0f);
  c.g = std::clamp(c.g, 0.0f, 1.0f);
  c.b = std::clamp(c.b, 0.0f, 1.0f);
  c.a = std::clamp(c.a, 0.0f, 1.0f);
  return c;
}

struct Texture {
  int width = 0, height = 0;
  bool linear = false;
  uint32_t cms = 0, cmt = 0;
  std::vector<uint8_t> pixels;
};

struct Shader {
  uint32_t id = 0;
  uint8_t input_count = 0;
  bool used_textures[2]{};
};

class SoftwareRenderer {
 public:
  void begin(uint8_t *pixels, uint32_t width, uint32_t height, bool editor_camera = false) {
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    editor_camera_ = editor_camera;
    selected_[0] = selected_[1] = 0;
    active_textures_[0] = active_textures_[1] = nullptr;
    upload_texture_ = 0;
    shader_ = nullptr;
    draw_calls_ = 0;
    drawn_triangles_ = 0;
    rasterized_triangles_ = 0;
    pixels_written_ = 0;
    depth_test_ = false;
    depth_mask_ = false;
    alpha_blend_ = false;
    zmode_decal_ = false;
    viewport_ = {0, 0, int(width), int(height)};
    scissor_ = viewport_;
    depth_.assign(size_t(width) * height, std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < size_t(width) * height; ++i) {
      pixels_[i * 4] = 68;
      pixels_[i * 4 + 1] = 132;
      pixels_[i * 4 + 2] = 181;
      pixels_[i * 4 + 3] = 255;
    }
  }

  void clear_cache() {
    textures_.clear();
    shaders_.clear();
    next_texture_ = 1;
    selected_[0] = selected_[1] = 0;
    active_textures_[0] = active_textures_[1] = nullptr;
    shader_ = nullptr;
  }

  static RenderApi api();

  void *lookup_shader(uint32_t id) {
    auto found = shaders_.find(id);
    if (found != shaders_.end()) return found->second.get();
    auto shader = std::make_unique<Shader>();
    shader->id = id;
    for (int cycle = 0; cycle < 2; ++cycle) {
      for (int part = 0; part < 4; ++part) {
        const uint8_t item = uint8_t(id >> (cycle * 12 + part * 3)) & 7;
        if (item >= 1 && item <= 4) shader->input_count = std::max(shader->input_count, item);
        if (item == 5 || item == 6) shader->used_textures[0] = true;
        if (item == 7) shader->used_textures[1] = true;
      }
    }
    void *result = shader.get();
    shaders_.emplace(id, std::move(shader));
    return result;
  }

  void shader_info(void *shader, uint8_t *inputs, bool *textures) const {
    const auto *s = static_cast<Shader *>(shader);
    *inputs = s->input_count;
    textures[0] = s->used_textures[0];
    textures[1] = s->used_textures[1];
  }

  uint32_t new_texture() { return next_texture_++; }
  void update_active_texture(int tile) {
    if (tile < 0 || tile >= 2) return;
    auto found = textures_.find(selected_[tile]);
    active_textures_[tile] = (found != textures_.end()) ? &found->second : nullptr;
  }
  void select_texture(int tile, uint32_t id) {
    if (tile < 0 || tile >= 2) return;
    selected_[tile] = id;
    upload_texture_ = id;
    update_active_texture(tile);
  }
  void upload_texture(const uint8_t *pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return;
    Texture &texture = textures_[upload_texture_];
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(pixels, pixels + size_t(width) * height * 4);
    update_active_texture(0);
    update_active_texture(1);
  }
  void sampler(int tile, bool linear, uint32_t cms, uint32_t cmt) {
    if (tile < 0 || tile > 1) return;
    Texture &texture = textures_[selected_[tile]];
    texture.linear = linear;
    texture.cms = cms;
    texture.cmt = cmt;
    update_active_texture(tile);
  }
  uint32_t draw_calls_ = 0;
  uint32_t drawn_triangles_ = 0;
  uint32_t rasterized_triangles_ = 0;
  uint32_t pixels_written_ = 0;
  void draw(float *buffer, size_t length, size_t triangles) {
    draw_calls_++;
    drawn_triangles_ += triangles;
    if (!buffer || !shader_ || triangles == 0 || length % (triangles * 3) != 0) {
      return;
    }
    const size_t stride = length / (triangles * 3);
    for (size_t triangle = 0; triangle < triangles; ++triangle) {
      draw_triangle(buffer + triangle * stride * 3, stride);
    }
  }

  void set_viewport(int x, int y, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    int top = int(height_) - y - h;
    if (top < 0) { h += top; top = 0; }
    if (top + h > int(height_)) h = int(height_) - top;
    if (x < 0) { w += x; x = 0; }
    if (x + w > int(width_)) w = int(width_) - x;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    viewport_ = {x, top, w, h};
  }
  void set_scissor(int x, int y, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    int top = int(height_) - y - h;
    if (top < 0) { h += top; top = 0; }
    if (top + h > int(height_)) h = int(height_) - top;
    if (x < 0) { w += x; x = 0; }
    if (x + w > int(width_)) w = int(width_) - x;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    scissor_ = {x, top, w, h};
  }
  void set_depth_test(bool value) { depth_test_ = value; }
  void set_depth_mask(bool value) { depth_mask_ = value; }
  void set_zmode_decal(bool value) { zmode_decal_ = value; }
  void set_alpha(bool value) { alpha_blend_ = value; }
  void set_shader(void *shader) { shader_ = static_cast<Shader *>(shader); }

 private:
  struct Rect { int x, y, w, h; };
  struct Vertex {
    const float *data = nullptr;
    float sx = 0, sy = 0, invw = 0, depth = 0;
  };

  static float fract(float value) { return value - std::floor(value); }
  static float wrap(float value, uint32_t mode) {
    if (mode & 2u) return std::clamp(value, 0.0f, 1.0f);
    const float integer = std::floor(value);
    const float fraction = fract(value);
    return (mode & 1u) && (static_cast<int64_t>(integer) & 1) ? 1.0f - fraction : fraction;
  }
  Color texture(uint32_t slot, float u, float v) const {
    const Texture *tex = active_textures_[slot];
    if (!tex || tex->width <= 0 || tex->height <= 0) return {};
    u = wrap(u, tex->cms);
    v = wrap(v, tex->cmt);
    const auto at = [&](int x, int y) {
      x = std::clamp(x, 0, tex->width - 1);
      y = std::clamp(y, 0, tex->height - 1);
      const uint8_t *p = tex->pixels.data() + (size_t(y) * tex->width + x) * 4;
      return Color{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    };
    const float fx = u * tex->width - 0.5f;
    const float fy = v * tex->height - 0.5f;
    if (!tex->linear) return at(int(std::floor(fx + 0.5f)), int(std::floor(fy + 0.5f)));
    const int x = int(std::floor(fx)), y = int(std::floor(fy));
    const float tx = fract(fx), ty = fract(fy);
    return at(x, y) * ((1 - tx) * (1 - ty)) + at(x + 1, y) * (tx * (1 - ty)) +
           at(x, y + 1) * ((1 - tx) * ty) + at(x + 1, y + 1) * (tx * ty);
  }

  static Color interpolate_weights(const Vertex vertices[3], int offset, int components, float w0, float w1, float w2) {
    const auto component = [&](int index) {
      return vertices[0].data[offset + index] * w0 +
             vertices[1].data[offset + index] * w1 +
             vertices[2].data[offset + index] * w2;
    };
    Color result{};
    if (components > 0) result.r = component(0);
    if (components > 1) result.g = component(1);
    if (components > 2) result.b = component(2);
    if (components > 3) result.a = component(3); else result.a = 1;
    return result;
  }

  Color shade(const Vertex vertices[3], size_t stride, float b0, float b1, float b2) const {
    const float denominator = b0 * vertices[0].invw + b1 * vertices[1].invw + b2 * vertices[2].invw;
    if (std::abs(denominator) < 1e-12f) return {};
    const float inv_denom = 1.0f / denominator;
    const float w0 = b0 * vertices[0].invw * inv_denom;
    const float w1 = b1 * vertices[1].invw * inv_denom;
    const float w2 = b2 * vertices[2].invw * inv_denom;

    if (!shader_) return {};
    const uint32_t id = shader_->id;
    const bool used_textures[2] = {shader_->used_textures[0], shader_->used_textures[1]};
    const uint8_t input_count = std::min<uint8_t>(shader_->input_count, 4);
    size_t offset = 4;
    Color texels[2]{};
    if (used_textures[0] || used_textures[1]) {
      if (offset + 2 <= stride) {
        const Color uv = interpolate_weights(vertices, int(offset), 2, w0, w1, w2);
        texels[0] = texture(0, uv.r, uv.g);
        texels[1] = texture(1, uv.r, uv.g);
      }
      offset += 2;
    }
    Color fog{};
    if (id & (1u << 25)) {
      if (offset + 4 <= stride) {
        fog = interpolate_weights(vertices, int(offset), 4, w0, w1, w2);
      }
      offset += 4;
    }
    std::array<Color, 4> inputs{};
    const bool alpha = (id & (1u << 24)) != 0;
    const size_t comp = alpha ? 4 : 3;
    for (uint8_t i = 0; i < input_count; ++i) {
      if (offset + comp <= stride) {
        inputs[i] = interpolate_weights(vertices, int(offset), int(comp), w0, w1, w2);
      }
      offset += comp;
    }
    if (offset > stride) return {};
    const auto item = [&](uint8_t code, bool alpha_only) -> Color {
      if (code == 0) return {};
      if (code >= 1 && code <= 4) {
        Color result = inputs[code - 1];
        if (alpha_only) result = {result.a, result.a, result.a, result.a};
        return result;
      }
      if (code == 5) return texels[0];
      if (code == 6) return {texels[0].a, texels[0].a, texels[0].a, texels[0].a};
      return texels[1];
    };
    const auto formula = [&](int cycle, bool alpha_only) {
      const uint8_t a = uint8_t(id >> (cycle * 12)) & 7;
      const uint8_t b = uint8_t(id >> (cycle * 12 + 3)) & 7;
      const uint8_t c = uint8_t(id >> (cycle * 12 + 6)) & 7;
      const uint8_t d = uint8_t(id >> (cycle * 12 + 9)) & 7;
      if (c == 0) return item(d, alpha_only);
      if (b == 0 && d == 0) return item(a, alpha_only) * item(c, alpha_only);
      if (b == d) return item(b, alpha_only) + (item(a, alpha_only) - item(b, alpha_only)) * item(c, alpha_only);
      return (item(a, alpha_only) - item(b, alpha_only)) * item(c, alpha_only) + item(d, alpha_only);
    };
    Color result = formula(0, false);
    if (alpha) {
      const Color alpha_value = formula(1, true);
      result.a = alpha_value.a;
    } else result.a = 1;
    if (id & (1u << 25)) {
      result.r = result.r * (1 - fog.a) + fog.r * fog.a;
      result.g = result.g * (1 - fog.a) + fog.g * fog.a;
      result.b = result.b * (1 - fog.a) + fog.b * fog.a;
    }
    return clamp_color(result);
  }

  void draw_triangle(const float *data, size_t stride) {
    const auto distance = [](const float *vertex, int plane) {
      switch (plane) {
        case 0: return vertex[0] + vertex[3];
        case 1: return vertex[3] - vertex[0];
        case 2: return vertex[1] + vertex[3];
        case 3: return vertex[3] - vertex[1];
        case 4: return vertex[2] + vertex[3];
        default: return vertex[3] - vertex[2];
      }
    };
    bool needs_clipping = false;
    for (int plane = 0; plane < 6; ++plane) {
      int inside = 0;
      for (int vertex = 0; vertex < 3; ++vertex) if (distance(data + size_t(vertex) * stride, plane) >= 0) ++inside;
      if (inside == 0) return;
      needs_clipping |= inside != 3;
    }
    if (!needs_clipping) {
      rasterize_triangle(data, stride);
      return;
    }
    std::vector<std::vector<float>> polygon;
    polygon.reserve(8);
    for (int vertex = 0; vertex < 3; ++vertex) polygon.emplace_back(data + size_t(vertex) * stride, data + size_t(vertex + 1) * stride);
    for (int plane = 0; plane < 6; ++plane) {
      std::vector<std::vector<float>> clipped;
      clipped.reserve(polygon.size() + 1);
      for (size_t current = 0; current < polygon.size(); ++current) {
        const auto &a = polygon[(current + polygon.size() - 1) % polygon.size()];
        const auto &b = polygon[current];
        const float da = distance(a.data(), plane), db = distance(b.data(), plane);
        const bool inside_a = da >= 0, inside_b = db >= 0;
        if (inside_a != inside_b) {
          const float t = da / (da - db);
          std::vector<float> edge(stride);
          for (size_t component = 0; component < stride; ++component) edge[component] = a[component] + (b[component] - a[component]) * t;
          clipped.push_back(std::move(edge));
        }
        if (inside_b) clipped.push_back(b);
      }
      polygon = std::move(clipped);
      if (polygon.size() < 3) return;
    }
    for (size_t vertex = 1; vertex + 1 < polygon.size(); ++vertex) {
      std::vector<float> triangle;
      triangle.reserve(stride * 3);
      triangle.insert(triangle.end(), polygon[0].begin(), polygon[0].end());
      triangle.insert(triangle.end(), polygon[vertex].begin(), polygon[vertex].end());
      triangle.insert(triangle.end(), polygon[vertex + 1].begin(), polygon[vertex + 1].end());
      rasterize_triangle(triangle.data(), stride);
    }
  }

  void rasterize_triangle(const float *data, size_t stride) {
    if (stride < 4) return;
    rasterized_triangles_++;
    Vertex vertices[3];
    for (int i = 0; i < 3; ++i) {
      vertices[i].data = data + size_t(i) * stride;
      const float w = vertices[i].data[3];
      if (std::abs(w) < 1e-8f) return;
      vertices[i].invw = 1.0f / w;
      vertices[i].sx = viewport_.x + (vertices[i].data[0] * vertices[i].invw * .5f + .5f) * viewport_.w;
      vertices[i].sy = viewport_.y + (1.0f - (vertices[i].data[1] * vertices[i].invw * .5f + .5f)) * viewport_.h;
      vertices[i].depth = vertices[i].data[2] * vertices[i].invw;
    }
    const float area = (vertices[1].sx - vertices[0].sx) * (vertices[2].sy - vertices[0].sy) -
                       (vertices[1].sy - vertices[0].sy) * (vertices[2].sx - vertices[0].sx);
    if (std::abs(area) < 1e-8f) return;
    const float inv_area = 1.0f / area;
    const int min_x = std::max({0, scissor_.x, int(std::floor(std::min({vertices[0].sx, vertices[1].sx, vertices[2].sx})))});
    const int max_x = std::min({int(width_) - 1, scissor_.x + scissor_.w - 1, int(std::ceil(std::max({vertices[0].sx, vertices[1].sx, vertices[2].sx})))});
    const int min_y = std::max({0, scissor_.y, int(std::floor(std::min({vertices[0].sy, vertices[1].sy, vertices[2].sy})))});
    const int max_y = std::min({int(height_) - 1, scissor_.y + scissor_.h - 1, int(std::ceil(std::max({vertices[0].sy, vertices[1].sy, vertices[2].sy})))});
    for (int y = min_y; y <= max_y; ++y) for (int x = min_x; x <= max_x; ++x) {
      const float px = x + .5f, py = y + .5f;
      const float b0 = ((vertices[1].sx - px) * (vertices[2].sy - py) - (vertices[1].sy - py) * (vertices[2].sx - px)) * inv_area;
      const float b1 = ((vertices[2].sx - px) * (vertices[0].sy - py) - (vertices[2].sy - py) * (vertices[0].sx - px)) * inv_area;
      const float b2 = 1 - b0 - b1;
      if (b0 < 0 || b1 < 0 || b2 < 0) continue;
      float depth = b0 * vertices[0].depth + b1 * vertices[1].depth + b2 * vertices[2].depth;
      if (zmode_decal_) depth -= 1e-5f;
      const size_t index = size_t(y) * width_ + x;
      if (depth_test_ && depth > depth_[index]) continue;
      Color color = shade(vertices, stride, b0, b1, b2);
      if ((shader_->id & (1u << 26)) && color.a <= .3f) continue;
      uint8_t *out = pixels_ + index * 4;
      if (alpha_blend_) {
        const float inverse = 1 - color.a;
        color.r = color.r * color.a + out[0] / 255.0f * inverse;
        color.g = color.g * color.a + out[1] / 255.0f * inverse;
        color.b = color.b * color.a + out[2] / 255.0f * inverse;
        color.a = color.a + out[3] / 255.0f * inverse;
      }
      color = clamp_color(color);
      out[0] = static_cast<uint8_t>(color.r * 255);
      out[1] = static_cast<uint8_t>(color.g * 255);
      out[2] = static_cast<uint8_t>(color.b * 255);
      out[3] = static_cast<uint8_t>(color.a * 255);
      if (depth_mask_) depth_[index] = depth;
      pixels_written_++;
    }
  }

  uint8_t *pixels_ = nullptr;
  uint32_t width_ = 0, height_ = 0, next_texture_ = 1;
  std::vector<float> depth_;
  std::unordered_map<uint32_t, Texture> textures_;
  const Texture *active_textures_[2]{};
  std::unordered_map<uint32_t, std::unique_ptr<Shader>> shaders_;
  uint32_t selected_[2]{};
  uint32_t upload_texture_ = 0;
  Shader *shader_ = nullptr;
  Rect viewport_{}, scissor_{};
  bool depth_test_ = false, depth_mask_ = false, zmode_decal_ = false, alpha_blend_ = false;
  bool editor_camera_ = false;
};

thread_local SoftwareRenderer *active_renderer = nullptr;
SoftwareRenderer &renderer() { return *active_renderer; }
bool rapi_z() { return false; }
void rapi_unload(void *) {}
void rapi_load(void *shader) {
  renderer().set_shader(shader);
}
void *rapi_create(uint32_t id) { return renderer().lookup_shader(id); }
void *rapi_lookup(uint32_t id) { return renderer().lookup_shader(id); }
void rapi_info(void *shader, uint8_t *inputs, bool *textures) { renderer().shader_info(shader, inputs, textures); }
uint32_t rapi_texture() { return renderer().new_texture(); }
void rapi_select(int tile, uint32_t id) { renderer().select_texture(tile, id); }
void rapi_upload(const uint8_t *data, int width, int height) { renderer().upload_texture(data, width, height); }
void rapi_sampler(int tile, bool linear, uint32_t cms, uint32_t cmt) { renderer().sampler(tile, linear, cms, cmt); }
void rapi_depth_test(bool enabled) { renderer().set_depth_test(enabled); }
void rapi_depth_mask(bool enabled) { renderer().set_depth_mask(enabled); }
void rapi_decal(bool enabled) { renderer().set_zmode_decal(enabled); }
void rapi_viewport(int x, int y, int w, int h) { renderer().set_viewport(x, y, w, h); }
void rapi_scissor(int x, int y, int w, int h) { renderer().set_scissor(x, y, w, h); }
void rapi_alpha(bool enabled) { renderer().set_alpha(enabled); }
void rapi_draw(float *buffer, size_t length, size_t triangles) { renderer().draw(buffer, length, triangles); }
void rapi_void() {}

RenderApi SoftwareRenderer::api() {
  return {rapi_z, rapi_unload, rapi_load, rapi_create, rapi_lookup, rapi_info, rapi_texture, rapi_select, rapi_upload,
          rapi_sampler, rapi_depth_test, rapi_depth_mask, rapi_decal, rapi_viewport, rapi_scissor, rapi_alpha, rapi_draw,
          rapi_void, rapi_void, rapi_void, rapi_void, rapi_void, rapi_void};
}

// Mario Skeleton and Bones Animation
struct OffsetSizePair { uint32_t offset; uint32_t size; };
struct Animation {
  int16_t flags; int16_t animYTransDivisor; int16_t startFrame; int16_t loopStart; int16_t loopEnd;
  int16_t unusedBoneCount; const int16_t *values; const uint16_t *index; uint32_t length;
};
struct MarioAnimsObj { uint32_t numEntries; const struct Animation *addrPlaceholder; struct OffsetSizePair entries[209]; };
extern "C" const struct MarioAnimsObj gMarioAnims;

static inline int32_t retrieve_anim_index(int32_t frame, const uint16_t **attributes) {
  int32_t result;
  if (frame < (*attributes)[0]) result = (*attributes)[1] + frame;
  else result = (*attributes)[1] + (*attributes)[0] - 1;
  *attributes += 2;
  return result;
}

struct MarioBoneDef { int parent; float base_trans[3]; uint32_t dl_addr; };
static const MarioBoneDef kMarioBones[20] = {
    {-1, {0, 0, 0}, 0},                   // 0: root
    {0,  {0, 0, 0}, 0x0400CC98},          // 1: butt
    {1,  {68, 0, 0}, 0x04010370},         // 2: torso
    {2,  {87, 0, 0}, 0x040119A0},         // 3: head
    {2,  {67, -10, 79}, 0},               // 4: L shoulder
    {4,  {0, 0, 0}, 0x0400D1D8},          // 5: L upper arm
    {5,  {65, 0, 0}, 0x0400D2F8},         // 6: L forearm
    {6,  {60, 0, 0}, 0x0400D8F0},         // 7: L hand
    {2,  {68, -10, -79}, 0},              // 8: R shoulder
    {8,  {0, 0, 0}, 0x0400DDE8},          // 9: R upper arm
    {9,  {65, 0, 0}, 0x0400DF08},         // 10: R forearm
    {10, {60, 0, 0}, 0x0400E458},         // 11: R hand
    {1,  {13, -8, 42}, 0},                // 12: L hip
    {12, {0, 0, 0}, 0x0400E7B0},          // 13: L thigh
    {13, {89, 0, 0}, 0x0400E918},         // 14: L shin
    {14, {67, 0, 0}, 0x0400ECA0},         // 15: L foot
    {1,  {13, -8, -42}, 0},               // 16: R hip
    {16, {0, 0, 0}, 0x0400EFB8},          // 17: R thigh
    {17, {89, 0, 0}, 0x0400F1D8},         // 18: R shin
    {18, {67, 0, 0}, 0x0400F4E8},         // 19: R foot
};

typedef float Mat4[4][4];
static void mat4_mul(Mat4 dest, const Mat4 a, const Mat4 b) {
  Mat4 tmp{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        tmp[i][j] += a[i][k] * b[k][j];
  memcpy(dest, tmp, sizeof(tmp));
}

static inline float sins(int16_t angle) { return std::sin(float(angle) * (3.14159265358979323846f / 32768.0f)); }
static inline float coss(int16_t angle) { return std::cos(float(angle) * (3.14159265358979323846f / 32768.0f)); }

static void mtxf_rotate_xyz_and_translate(Mat4 dest, const float b[3], const int16_t c[3]) {
  float sx = sins(c[0]), cx = coss(c[0]);
  float sy = sins(c[1]), cy = coss(c[1]);
  float sz = sins(c[2]), cz = coss(c[2]);

  dest[0][0] = cy * cz; dest[0][1] = cy * sz; dest[0][2] = -sy; dest[0][3] = 0;
  dest[1][0] = sx * sy * cz - cx * sz; dest[1][1] = sx * sy * sz + cx * cz; dest[1][2] = sx * cy; dest[1][3] = 0;
  dest[2][0] = cx * sy * cz + sx * sz; dest[2][1] = cx * sy * sz - sx * cz; dest[2][2] = cx * cy; dest[2][3] = 0;
  dest[3][0] = b[0]; dest[3][1] = b[1]; dest[3][2] = b[2]; dest[3][3] = 1;
}

static void render_mario_fast3d(const sm64_scene_mario &mario, const Mat4 V) {
  int animID = mario.anim_id >= 0 && mario.anim_id < 209 ? mario.anim_id : 0;
  int frame = mario.anim_frame >= 0 ? mario.anim_frame : 0;
  const auto &pair = gMarioAnims.entries[animID];
  const auto *anim = (const Animation *)((const uint8_t *)&gMarioAnims + pair.offset);
  const int16_t *vals = (const int16_t *)((const uint8_t *)anim + (uintptr_t)anim->values);
  const uint16_t *attr = (const uint16_t *)((const uint8_t *)anim + (uintptr_t)anim->index);

  float root_trans[3] = {
      (float)vals[retrieve_anim_index(frame, &attr)],
      (float)vals[retrieve_anim_index(frame, &attr)],
      (float)vals[retrieve_anim_index(frame, &attr)]
  };

  int16_t bone_rots[20][3]{};
  for (int b = 0; b < 20; ++b) {
    bone_rots[b][0] = vals[retrieve_anim_index(frame, &attr)];
    bone_rots[b][1] = vals[retrieve_anim_index(frame, &attr)];
    bone_rots[b][2] = vals[retrieve_anim_index(frame, &attr)];
  }

  Mat4 mario_world;
  mtxf_rotate_xyz_and_translate(mario_world, mario.pos, mario.angle);

  Mat4 bone_mats[20];
  for (int i = 0; i < 20; ++i) {
    const auto &b = kMarioBones[i];
    float trans[3] = {b.base_trans[0], b.base_trans[1], b.base_trans[2]};
    if (i == 0) {
      trans[0] += root_trans[0];
      trans[1] += root_trans[1];
      trans[2] += root_trans[2];
    }
    Mat4 local_m;
    mtxf_rotate_xyz_and_translate(local_m, trans, bone_rots[i]);
    if (b.parent == -1) {
      mat4_mul(bone_mats[i], local_m, mario_world);
    } else {
      mat4_mul(bone_mats[i], local_m, bone_mats[b.parent]);
    }
  }

  for (int i = 0; i < 20; ++i) {
    if (kMarioBones[i].dl_addr != 0) {
      Mat4 bone_view;
      mat4_mul(bone_view, bone_mats[i], V);
      gfx_set_modelview(bone_view);
      uint32_t off = kMarioBones[i].dl_addr & 0xFFFFFF;
      if (g_sm64_segments[4]) {
        gfx_run_dl(reinterpret_cast<Gfx *>(const_cast<uint8_t *>(g_sm64_segments[4] + off)));
      }
    }
  }
}

} // namespace

struct sm64_level {
  uint32_t level_id = 9;
  std::string level_name;
  std::string rom_path;
  std::vector<sm64_terrain_triangle> triangles;
  std::vector<sm64_terrain_region> regions;
  float spawn_x = 0.0f;
  float spawn_y = 0.0f;
  float spawn_z = 0.0f;
  int16_t spawn_yaw = 0;
  uint32_t area1_geo = 0;
  std::vector<uint32_t> display_lists;
  std::unordered_map<uint8_t, std::vector<uint8_t>> segments;
  RomInfo rom_info{};
};

struct sm64_world {
  std::shared_ptr<const sm64_level> level;
  sm64_sim_world *sim = nullptr;
  sm64_view view{};
  sm64_scene_mario pose{};
  uint64_t revision = next_revision.fetch_add(1, std::memory_order_relaxed);
  bool scratch = false;
  uint32_t edit_count = 0;
  std::vector<sm64_input> history;

  ~sm64_world() {
    if (sim) {
      sm64_world_destroy(sim);
      sim = nullptr;
    }
  }
};

namespace {

static std::mutex g_vk_mutex;
static sm64_vulkan *g_vk_renderers[2] = {nullptr, nullptr};
static unsigned g_render_slot = 0;

thread_local const sm64_world *g_current_render_world = nullptr;
thread_local const sm64_render_config *g_current_render_config = nullptr;

static void render_scene_fast3d(const sm64_world *world, const sm64_render_config *config) {
  if (!world || !world->level || !config) return;
  const auto &level = *world->level;

  // 1. Reset frame state and bind segments
  gfx_start_frame();
  for (const auto &[seg, data] : level.segments) {
    if (seg < 32 && !data.empty()) {
      g_sm64_segments[seg] = data.data();
    }
  }

  // 2. Set viewport dimensions and aspect ratio
  gfx_current_dimensions.width = config->width;
  gfx_current_dimensions.height = config->height;
  gfx_current_dimensions.aspect_ratio = float(config->width) / float(config->height);

  // 3. Camera matrices
  float eye[3], target[3], up[3];
  float fovy = 45.0f * (3.14159265358979323846f / 180.0f);
  float near_z = 100.0f, far_z = 20000.0f;

  if (config->mode == 0) {
    sm64_camera cam{};
    if (sm64_world_camera(world->sim, gfx_current_dimensions.aspect_ratio, &cam)) {
      eye[0] = cam.eye[0]; eye[1] = cam.eye[1]; eye[2] = cam.eye[2];
      target[0] = cam.target[0]; target[1] = cam.target[1]; target[2] = cam.target[2];
      up[0] = cam.up[0]; up[1] = cam.up[1]; up[2] = cam.up[2];
      fovy = cam.fov_y;
      near_z = cam.near_z;
      far_z = cam.far_z;
    } else {
      eye[0] = config->eye[0]; eye[1] = config->eye[1]; eye[2] = config->eye[2];
      target[0] = config->target[0]; target[1] = config->target[1]; target[2] = config->target[2];
      up[0] = config->up[0]; up[1] = config->up[1]; up[2] = config->up[2];
    }
  } else {
    eye[0] = config->eye[0]; eye[1] = config->eye[1]; eye[2] = config->eye[2];
    target[0] = config->target[0]; target[1] = config->target[1]; target[2] = config->target[2];
    up[0] = config->up[0]; up[1] = config->up[1]; up[2] = config->up[2];
  }

  float fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  float flen = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
  if (flen < 1e-4f) { fwd[0] = 0; fwd[1] = 0; fwd[2] = -1; flen = 1; }
  fwd[0] /= flen; fwd[1] /= flen; fwd[2] /= flen;

  float rgt[3] = {up[1]*fwd[2] - up[2]*fwd[1], up[2]*fwd[0] - up[0]*fwd[2], up[0]*fwd[1] - up[1]*fwd[0]};
  float rlen = std::sqrt(rgt[0]*rgt[0] + rgt[1]*rgt[1] + rgt[2]*rgt[2]);
  if (rlen < 1e-4f) { rgt[0] = 1; rgt[1] = 0; rgt[2] = 0; rlen = 1; }
  rgt[0] /= rlen; rgt[1] /= rlen; rgt[2] /= rlen;

  float u[3] = {fwd[1]*rgt[2] - fwd[2]*rgt[1], fwd[2]*rgt[0] - fwd[0]*rgt[2], fwd[0]*rgt[1] - fwd[1]*rgt[0]};

  Mat4 V = {
      {rgt[0], u[0], -fwd[0], 0.0f},
      {rgt[1], u[1], -fwd[1], 0.0f},
      {rgt[2], u[2], -fwd[2], 0.0f},
      {-(rgt[0]*eye[0] + rgt[1]*eye[1] + rgt[2]*eye[2]),
       -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]),
        (fwd[0]*eye[0] + fwd[1]*eye[1] + fwd[2]*eye[2]),
       1.0f}
  };

  float base_aspect = 4.0f / 3.0f;
  float tan_half = std::tan(fovy / 2.0f);
  Mat4 P = {
      {1.0f / (base_aspect * tan_half), 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f / tan_half, 0.0f, 0.0f},
      {0.0f, 0.0f, (near_z + far_z) / (near_z - far_z), -1.0f},
      {0.0f, 0.0f, (2.0f * near_z * far_z) / (near_z - far_z), 0.0f}
  };

  gfx_set_projection(P);
  gfx_set_modelview(V);

  // 4. Run terrain display lists
  for (uint32_t dl : level.display_lists) {
    uint8_t seg = (dl >> 24) & 0x1F;
    uint32_t off = dl & 0xFFFFFF;
    auto it = level.segments.find(seg);
    if (it != level.segments.end() && off < it->second.size()) {
      gfx_run_dl(reinterpret_cast<Gfx *>(const_cast<uint8_t *>(it->second.data() + off)));
    }
  }

  // 5. Render Mario (unless scene_only)
  if (!config->scene_only && world->pose.valid) {
    render_mario_fast3d(world->pose, V);
  }

  // 6. Render Ghosts
  if (config->ghosts && config->ghost_count > 0) {
    for (uint32_t g = 0; g < config->ghost_count; ++g) {
      if (config->ghosts[g].valid) {
        render_mario_fast3d(config->ghosts[g], V);
      }
    }
  }

  // 7. Flush Fast3D buffer
  gfx_flush();
}

} // namespace

extern "C" sm64_level *sm64_ft_open(const char *path, const char *, char *error, size_t error_size) {
  return boundary<sm64_level *>(error, error_size, nullptr, [&]() -> sm64_level * {
    if (!path) fail("Missing SM64 setup path");
    std::filesystem::path config_path(path);
    std::string config_str;
    std::string rom_path;
    uint32_t target_level = 0;
    uint32_t start_frame = 0;
    std::string movie_path;

    if (config_path.extension() == ".sm64") {
      const auto config_bytes = read_bytes(config_path);
      config_str.assign(config_bytes.begin(), config_bytes.end());
      const std::filesystem::path base = config_path.has_parent_path() ? config_path.parent_path() : ".";
      rom_path = resolve(base, json_string(config_str, "rom")).string();
      target_level = json_u32(config_str, "level");
      start_frame = json_u32(config_str, "start_frame");
      (void)start_frame;
      movie_path = json_string(config_str, "movie", false);
      if (!movie_path.empty()) movie_path = resolve(base, movie_path).string();
    } else {
      rom_path = path;
      target_level = 9; // Default to Bob-omb Battlefield
    }

    if (target_level == 0) target_level = 16; // Default to Castle Grounds

    auto rom_bytes = normalized_rom(read_bytes(rom_path));
    RomInfo rom_info = inspect_rom(rom_bytes);

    // Identify Segment 15 base and Segment 2 bounds based on region
    uint32_t seg15_base = 0x2ABCA0;
    uint32_t seg2_start = 0x108A40;
    uint32_t seg2_end = 0x114750;

    if (rom_info.country == 'J') {
      seg15_base = 0x2AA240;
      seg2_start = 0x1076D0;
      seg2_end = 0x112B50;
    } else if (rom_info.country == 'P') {
      seg15_base = 0x28CEE0;
      seg2_start = 0x0DE190;
      seg2_end = 0x0E49F0;
    }

    auto level = std::make_unique<sm64_level>();
    level->level_id = target_level;
    level->level_name = get_level_title(target_level);
    level->rom_path = std::filesystem::absolute(rom_path).string();
    level->rom_info = rom_info;

    // Segment 2: Textures & fonts
    decompress_mio0(rom_bytes.data() + seg2_start, seg2_end - seg2_start, level->segments[0x02]);

    // Segment 15 startup script decompression
    uint32_t off = seg15_base;
    while (off + 4 <= rom_bytes.size()) {
      uint8_t cmd = rom_bytes[off];
      uint8_t len = rom_bytes[off + 1];
      if (len == 0 || off + len > rom_bytes.size()) break;
      if (cmd == 0x18 || cmd == 0x1A) {
        uint8_t seg = rom_bytes[off + 3];
        uint32_t s_start = read_u32_be(rom_bytes.data() + off + 4);
        uint32_t s_end = read_u32_be(rom_bytes.data() + off + 8);
        if (s_start < s_end && s_end <= rom_bytes.size()) {
          decompress_mio0(rom_bytes.data() + s_start, s_end - s_start, level->segments[seg]);
        }
      } else if (cmd == 0x17) {
        uint8_t seg = rom_bytes[off + 3];
        uint32_t s_start = read_u32_be(rom_bytes.data() + off + 4);
        uint32_t s_end = read_u32_be(rom_bytes.data() + off + 8);
        if (s_start < s_end && s_end <= rom_bytes.size()) {
          level->segments[seg].assign(rom_bytes.begin() + s_start, rom_bytes.begin() + s_end);
        }
      } else if (cmd == 0x1D || cmd == 0x07 || cmd == 0x02) {
        break;
      }
      off += len;
    }

    // Locate target level in table
    static const uint32_t kLevelList[] = {
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 33, 34, 36
    };

    int list_index = 5; // Default Level 9
    for (size_t i = 0; i < std::size(kLevelList); ++i) {
      if (kLevelList[i] == target_level) {
        list_index = static_cast<int>(i);
        break;
      }
    }

    uint32_t target_seg = 0x15000000 | (0x03F4 + list_index * 0x14);
    uint32_t rom_entry_off = seg15_base + (target_seg & 0xFFFFFF);
    if (rom_entry_off + 16 > rom_bytes.size()) fail("Level script entry out of range");

    uint32_t rom_start = read_u32_be(rom_bytes.data() + rom_entry_off + 4);
    uint32_t rom_end = read_u32_be(rom_bytes.data() + rom_entry_off + 8);
    uint32_t entry_addr = read_u32_be(rom_bytes.data() + rom_entry_off + 12);
    uint32_t script_off = rom_start + (entry_addr & 0xFFFFFF);

    if (rom_start < rom_end && rom_end <= rom_bytes.size()) {
      level->segments[0x0E].assign(rom_bytes.begin() + rom_start, rom_bytes.begin() + rom_end);
    }

    uint32_t collision_addr = 0;

    // Parse level script recursively
    auto parse_script = [&](auto &self, uint32_t start_off, uint32_t end_off) -> void {
      uint32_t cur = start_off;
      while (cur + 4 <= end_off && cur + 4 <= rom_bytes.size()) {
        uint8_t cmd = rom_bytes[cur];
        uint8_t len = rom_bytes[cur + 1];
        if (len == 0 || cur + len > rom_bytes.size()) break;
        if (cmd == 0x18 || cmd == 0x1A) {
          uint8_t seg = rom_bytes[cur + 3];
          uint32_t s_start = read_u32_be(rom_bytes.data() + cur + 4);
          uint32_t s_end = read_u32_be(rom_bytes.data() + cur + 8);
          if (s_start < s_end && s_end <= rom_bytes.size()) {
            decompress_mio0(rom_bytes.data() + s_start, s_end - s_start, level->segments[seg]);
          }
        } else if (cmd == 0x17) {
          uint8_t seg = rom_bytes[cur + 3];
          uint32_t s_start = read_u32_be(rom_bytes.data() + cur + 4);
          uint32_t s_end = read_u32_be(rom_bytes.data() + cur + 8);
          if (s_start < s_end && s_end <= rom_bytes.size()) {
            level->segments[seg].assign(rom_bytes.begin() + s_start, rom_bytes.begin() + s_end);
          }
        } else if (cmd == 0x06 && len >= 8) {
          uint32_t target = read_u32_be(rom_bytes.data() + cur + 4);
          uint8_t t_seg = (target >> 24) & 0xFF;
          uint32_t t_off = target & 0xFFFFFF;
          if (t_seg == 0x15 && seg15_base + t_off < rom_bytes.size()) {
            self(self, seg15_base + t_off, rom_bytes.size());
          } else if (t_seg == 0x0E && rom_start + t_off < rom_end) {
            self(self, rom_start + t_off, rom_end);
          }
        } else if (cmd == 0x2B && len >= 12) {
          level->spawn_x = static_cast<float>(read_s16_be(rom_bytes.data() + cur + 6));
          level->spawn_y = static_cast<float>(read_s16_be(rom_bytes.data() + cur + 8));
          level->spawn_z = static_cast<float>(read_s16_be(rom_bytes.data() + cur + 10));
        } else if (cmd == 0x1F && len >= 8) {
          uint8_t area_id = rom_bytes[cur + 2];
          uint32_t geo_addr = read_u32_be(rom_bytes.data() + cur + 4);
          if (area_id == 1 || level->area1_geo == 0) {
            level->area1_geo = geo_addr;
          }
        } else if (cmd == 0x2E && len >= 8) {
          collision_addr = read_u32_be(rom_bytes.data() + cur + 4);
        } else if (cmd == 0x07 || cmd == 0x02) {
          break;
        }
        cur += len;
      }
    };

    parse_script(parse_script, script_off, rom_end);

    // Extract collision data
    if (collision_addr != 0) {
      uint8_t col_seg = (collision_addr >> 24) & 0x1F;
      uint32_t col_off = collision_addr & 0xFFFFFF;
      const auto it = level->segments.find(col_seg);
      if (it != level->segments.end() && col_off < it->second.size()) {
        const uint8_t *col_bytes = it->second.data();
        size_t col_len = it->second.size();
        size_t p = col_off;
        std::vector<std::array<int16_t, 3>> verts;
        while (p + 2 <= col_len) {
          int16_t cmd = read_s16_be(col_bytes + p);
          p += 2;
          if (cmd == 0x0040) { // TERRAIN_LOAD_VERTICES
            if (p + 2 > col_len) break;
            int16_t n = read_s16_be(col_bytes + p);
            p += 2;
            for (int i = 0; i < n && p + 6 <= col_len; ++i) {
              verts.push_back({read_s16_be(col_bytes + p), read_s16_be(col_bytes + p + 2), read_s16_be(col_bytes + p + 4)});
              p += 6;
            }
          } else if (cmd == 0x0041) { // TERRAIN_LOAD_CONTINUE
            continue;
          } else if (cmd == 0x0042) { // TERRAIN_LOAD_END
            break;
          } else if (cmd == 0x0043) { // TERRAIN_LOAD_OBJECTS
            if (p + 2 > col_len) break;
            int16_t n = read_s16_be(col_bytes + p);
            p += 2 + n * 8;
          } else if (cmd == 0x0044) { // TERRAIN_LOAD_ENVIRONMENT
            if (p + 2 > col_len) break;
            int16_t n = read_s16_be(col_bytes + p);
            p += 2;
            for (int i = 0; i < n && p + 12 <= col_len; ++i) {
              sm64_terrain_region reg{};
              reg.kind = read_s16_be(col_bytes + p);
              reg.low_x = read_s16_be(col_bytes + p + 2);
              reg.low_z = read_s16_be(col_bytes + p + 4);
              reg.high_x = read_s16_be(col_bytes + p + 6);
              reg.high_z = read_s16_be(col_bytes + p + 8);
              reg.height = read_s16_be(col_bytes + p + 10);
              level->regions.push_back(reg);
              p += 12;
            }
          } else { // Surface triangles
            if (p + 2 > col_len) break;
            int16_t n = read_s16_be(col_bytes + p);
            p += 2;
            bool has_force = (cmd == 0x0004 || cmd == 0x000e || cmd == 0x0024 ||
                              cmd == 0x0025 || cmd == 0x0026 || cmd == 0x0027 || cmd == 0x002c);
            for (int i = 0; i < n && p + 6 <= col_len; ++i) {
              int16_t i1 = read_s16_be(col_bytes + p);
              int16_t i2 = read_s16_be(col_bytes + p + 2);
              int16_t i3 = read_s16_be(col_bytes + p + 4);
              p += 6;
              int16_t force = 0;
              if (has_force && p + 2 <= col_len) {
                force = read_s16_be(col_bytes + p);
                p += 2;
              }
              if (size_t(i1) < verts.size() && size_t(i2) < verts.size() && size_t(i3) < verts.size()) {
                sm64_terrain_triangle tri{};
                tri.vertices[0][0] = verts[i1][0]; tri.vertices[0][1] = verts[i1][1]; tri.vertices[0][2] = verts[i1][2];
                tri.vertices[1][0] = verts[i2][0]; tri.vertices[1][1] = verts[i2][1]; tri.vertices[1][2] = verts[i2][2];
                tri.vertices[2][0] = verts[i3][0]; tri.vertices[2][1] = verts[i3][1]; tri.vertices[2][2] = verts[i3][2];
                tri.type = cmd;
                tri.force = force;
                tri.room = 0;
                tri.dynamic = false;
                level->triangles.push_back(tri);
              }
            }
          }
        }
      }
    }

    // Fallback plane if no collision
    if (level->triangles.empty()) {
      sm64_terrain_triangle t1{}, t2{};
      t1.vertices[0][0] = -8192; t1.vertices[0][1] = 0; t1.vertices[0][2] = -8192;
      t1.vertices[1][0] =  8192; t1.vertices[1][1] = 0; t1.vertices[1][2] = -8192;
      t1.vertices[2][0] =  8192; t1.vertices[2][1] = 0; t1.vertices[2][2] =  8192;
      level->triangles.push_back(t1);
      t2.vertices[0][0] = -8192; t2.vertices[0][1] = 0; t2.vertices[0][2] = -8192;
      t2.vertices[1][0] =  8192; t2.vertices[1][1] = 0; t2.vertices[1][2] =  8192;
      t2.vertices[2][0] = -8192; t2.vertices[2][1] = 0; t2.vertices[2][2] =  8192;
      level->triangles.push_back(t2);
    }

    // Extract terrain display lists by traversing Area 1 Geo layout
    auto parse_geo = [&](auto &self, uint32_t geo_addr) -> void {
      uint8_t seg = (geo_addr >> 24) & 0x1F;
      uint32_t off = geo_addr & 0xFFFFFF;
      auto it = level->segments.find(seg);
      if (it == level->segments.end()) return;
      const auto &data = it->second;

      uint32_t cur = off;
      while (cur + 4 <= data.size()) {
        uint8_t cmd = data[cur];
        if (cmd == 0x01) { // GEO_END
          break;
        } else if (cmd == 0x02) { // GEO_BRANCH / JUMP
          if (cur + 8 <= data.size()) {
            uint32_t target = read_u32_be(data.data() + cur + 4);
            self(self, target);
          }
          break;
        } else if (cmd == 0x00) { // GEO_BRANCH_AND_STORE / CALL
          if (cur + 8 <= data.size()) {
            uint32_t target = read_u32_be(data.data() + cur + 4);
            self(self, target);
          }
          cur += 8;
        } else if (cmd == 0x03) { // GEO_RETURN
          break;
        } else if (cmd == 0x15) { // GEO_DISPLAY_LIST
          if (cur + 8 <= data.size()) {
            uint32_t dl = read_u32_be(data.data() + cur + 4);
            if (dl != 0 && std::find(level->display_lists.begin(), level->display_lists.end(), dl) == level->display_lists.end()) {
              level->display_lists.push_back(dl);
            }
          }
          cur += 8;
        } else if (cmd == 0x04 || cmd == 0x05 || cmd == 0x0B || cmd == 0x0C || cmd == 0x17 || cmd == 0x20) {
          cur += 4;
        } else if (cmd == 0x08 || cmd == 0x13) {
          cur += 12;
        } else if (cmd == 0x0F) {
          cur += 20;
        } else if (cmd == 0x10 || cmd == 0x1F) {
          cur += 16;
        } else {
          cur += 8;
        }
      }
    };

    if (level->area1_geo != 0) {
      parse_geo(parse_geo, level->area1_geo);
    }

    // Fallback if no geo layout display lists were found
    if (level->display_lists.empty()) {
      for (size_t g = rom_start; g + 8 <= rom_end; g += 4) {
        if (rom_bytes[g] == 0x15 && rom_bytes[g + 1] >= 1 && rom_bytes[g + 1] <= 7 &&
            (rom_bytes[g + 4] == 0x07 || rom_bytes[g + 4] == 0x0E)) {
          uint32_t dl = read_u32_be(rom_bytes.data() + g + 4);
          if (std::find(level->display_lists.begin(), level->display_lists.end(), dl) == level->display_lists.end()) {
            level->display_lists.push_back(dl);
          }
        }
      }
    }

    return level.release();
  });
}

extern "C" void sm64_ft_level_free(sm64_level *level) {
  delete level;
}

extern "C" sm64_world *sm64_ft_world_new(const sm64_level *level, char *error, size_t error_size) {
  return boundary<sm64_world *>(error, error_size, nullptr, [&]() -> sm64_world * {
    if (!level) fail("Missing SM64 level");
    auto world = std::make_unique<sm64_world>();
    world->level = std::shared_ptr<const sm64_level>(level, [](const sm64_level *) {});

    world->sim = sm64_world_create(
        level->triangles.data(), level->triangles.size(),
        level->regions.data(), level->regions.size(),
        static_cast<int16_t>(level->level_id),
        level->spawn_x, level->spawn_y, level->spawn_z,
        level->spawn_yaw);

    if (!world->sim) fail("Failed to create native SM64 simulation world");

    sm64_sim_world_view(world->sim, &world->view);
    sm64_world_pose(world->sim, &world->pose);
    return world.release();
  });
}

extern "C" void sm64_ft_world_free(sm64_world *world) {
  delete world;
}

extern "C" void sm64_ft_world_set_scratch(sm64_world *world, bool scratch) {
  if (!world) return;
  world->scratch = scratch;
  if (world->sim) sm64_world_set_scratch(world->sim, scratch);
}

extern "C" void sm64_ft_copy(sm64_world *dst, const sm64_world *src) {
  if (!dst || !src || dst == src) return;
  if (dst->sim && src->sim) sm64_world_copy(dst->sim, src->sim);
  dst->level = src->level;
  dst->view = src->view;
  dst->pose = src->pose;
  dst->revision = src->revision;
  dst->edit_count = src->edit_count;
  dst->history = src->history;
}

extern "C" bool sm64_ft_step(sm64_world *world, sm64_input input, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !world->sim) fail("Missing SM64 world");
    if (!sm64_world_step(world->sim, input, error, error_size)) return false;
    sm64_sim_world_view(world->sim, &world->view);
    sm64_world_pose(world->sim, &world->pose);
    world->revision = sm64_world_revision(world->sim);
    world->edit_count = sm64_world_edit_count(world->sim);
    world->history.push_back(input);
    return true;
  });
}

extern "C" uint32_t sm64_ft_view(const sm64_world *world, sm64_view *out) {
  if (!world || !out) return 0;
  *out = world->view;
  return world->view.frame;
}

extern "C" bool sm64_ft_pose(const sm64_world *world, sm64_scene_mario *out) {
  if (!world || !out) return false;
  *out = world->pose;
  return out->valid;
}

extern "C" uint64_t sm64_ft_revision(const sm64_world *world) {
  return world ? world->revision : 0;
}

extern "C" bool sm64_ft_camera(const sm64_world *world, float aspect, sm64_camera *out) {
  if (!world || !world->sim || !out) return false;
  return sm64_world_camera(world->sim, aspect, out);
}

extern "C" bool sm64_ft_set(sm64_world *world, uint32_t property, const sm64_view *value, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !world->sim || !value) fail("Missing SM64 state edit");
    if (!sm64_world_set(world->sim, property, value, error, error_size)) return false;
    sm64_sim_world_view(world->sim, &world->view);
    sm64_world_pose(world->sim, &world->pose);
    world->revision = sm64_world_revision(world->sim);
    world->edit_count = sm64_world_edit_count(world->sim);
    return true;
  });
}

extern "C" size_t sm64_ft_save(const sm64_world *world, uint8_t *out, size_t size, char *error, size_t error_size) {
  return boundary<size_t>(error, error_size, 0, [&]() -> size_t {
    if (!world || !world->sim) fail("Missing SM64 world");
    return sm64_world_save(world->sim, out, size, error, error_size);
  });
}

extern "C" bool sm64_ft_load(sm64_world *world, const uint8_t *data, size_t size, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !world->sim || !data) fail("Missing SM64 saved state");
    if (!sm64_world_load(world->sim, data, size, error, error_size)) return false;
    sm64_sim_world_view(world->sim, &world->view);
    sm64_world_pose(world->sim, &world->pose);
    world->revision = sm64_world_revision(world->sim);
    world->edit_count = sm64_world_edit_count(world->sim);
    return true;
  });
}

extern "C" bool sm64_ft_export(const sm64_world *world, const char *path, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !world->level || !path) fail("Missing SM64 export arguments");
    if (world->edit_count > 0) fail("M64 cannot silently discard edits");
    write_m64(path, world->level->rom_info, world->history);
    return true;
  });
}

extern "C" sm64_physics *sm64_ft_physics_create(const sm64_world *source, char *error, size_t error_size) {
  if (!source || !source->sim) {
    if (error && error_size) std::snprintf(error, error_size, "Missing SM64 source world");
    return nullptr;
  }
  return sm64_world_physics_create(source->sim, error, error_size);
}

static bool is_valid_render_config(const sm64_render_config *config) {
  if (!config) return false;
  if (config->width == 0 || config->height == 0 || config->width > 4096 || config->height > 4096) return false;
  if (config->mode > 0) {
    for (int i = 0; i < 16; ++i) {
      if (!std::isfinite(config->view_proj[i])) return false;
    }
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(config->eye[i]) || !std::isfinite(config->target[i]) || !std::isfinite(config->up[i])) return false;
    }
    if (!std::isfinite(config->span)) return false;
  }
  return true;
}

extern "C" bool sm64_ft_render(const sm64_world *world, const sm64_render_config *config, uint8_t *pixels, size_t size, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !config || !pixels || !is_valid_render_config(config) || size < size_t(config->width) * config->height * 4) {
      fail("Invalid SM64 render target");
    }
    static thread_local SoftwareRenderer g_software_renderer;
    g_software_renderer.begin(pixels, config->width, config->height, config->mode > 0);
    active_renderer = &g_software_renderer;
    const RenderApi api = SoftwareRenderer::api();
    gfx_init(nullptr, reinterpret_cast<GfxRenderingAPI *>(const_cast<RenderApi *>(&api)), "sm64");
    render_scene_fast3d(world, config);
    active_renderer = nullptr;
    return true;
  });
}

extern "C" bool sm64_ft_render_gpu(const sm64_world *world, const sm64_render_config *config, const ft_gpu_device *gpu, const ft_gpu_image *target_image, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !config || !gpu || !target_image || !is_valid_render_config(config)) {
      fail("Invalid SM64 GPU render target");
    }
    std::lock_guard lock(g_vk_mutex);
    auto *&renderer = g_vk_renderers[g_render_slot++ % 2];
    if (!renderer) {
      renderer = sm64_vulkan_create(gpu, error, error_size);
      if (!renderer) fail("Failed to create SM64 Vulkan renderer");
    }

    g_current_render_world = world;
    g_current_render_config = config;

    return sm64_vulkan_render(renderer,
                              [](uint32_t, uint32_t) {
                                render_scene_fast3d(g_current_render_world, g_current_render_config);
                              },
                              [](void *api) {
                                gfx_init(nullptr, reinterpret_cast<GfxRenderingAPI *>(api), "sm64");
                              },
                              config->width,
                              config->height,
                              target_image,
                              config->mode > 0,
                              error,
                              error_size);
  });
}

extern "C" void sm64_ft_release_graphics(void) {
  std::lock_guard lock(g_vk_mutex);
  for (auto *&renderer : g_vk_renderers) {
    if (renderer) sm64_vulkan_destroy(renderer);
    renderer = nullptr;
  }
  gfx_clear_cache();
}

extern "C" const char *sm64_ft_level_name(const sm64_level *level) {
  return level ? level->level_name.c_str() : "Super Mario 64";
}

extern "C" bool sm64_ft_write_config(const char *path, const char *rom, const char *library, const char *movie, uint32_t frame, uint32_t level, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!path || !rom) fail("Missing SM64 setup value");
    const std::filesystem::path config_path(path);
    if (config_path.has_parent_path()) {
      std::error_code directory_error;
      std::filesystem::create_directories(config_path.parent_path(), directory_error);
      if (directory_error) fail("Cannot create SM64 setup directory: " + directory_error.message());
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) fail(std::string("Cannot write ") + path);
    file << "{\n  \"rom\": \"" << escape_json(rom) << "\",\n";
    if (library && library[0]) file << "  \"library\": \"" << escape_json(library) << "\",\n";
    if (movie && movie[0]) file << "  \"movie\": \"" << escape_json(movie) << "\",\n";
    if (level) file << "  \"level\": " << level << ",\n";
    file << "  \"start_frame\": " << frame << "\n}\n";
    if (!file) fail(std::string("Cannot write ") + path);
    return true;
  });
}
