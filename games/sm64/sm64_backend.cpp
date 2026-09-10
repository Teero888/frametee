#include "sm64_bridge.h"
#include "sm64_vulkan.h"
#include "full_game/scene.h"

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

#ifdef _WIN32
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#endif

namespace {

constexpr size_t kMaxFrames = 10'000'000;
constexpr size_t kMovieHeaderSize = 0x400;
// Small immutable pages keep nearby search branches from duplicating large
// mostly unchanged ranges. Native display-list memory is excluded below.
constexpr size_t kSnapshotChunkSize = 4 * 1024;
std::atomic<uint64_t> next_revision{1};

struct Pad {
  uint16_t buttons;
  int8_t stick_x;
  int8_t stick_y;
  int8_t ext_stick_x;
  int8_t ext_stick_y;
  uint8_t error;
};
static_assert(sizeof(Pad) == 8);

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
// SM64EX currently has 23 callbacks. Older full-game libsm64 builds omit the
// final shutdown callback and safely consume this common prefix.
static_assert(sizeof(RenderApi) == 23 * sizeof(void *));

struct Segment {
  uint8_t *address = nullptr;
  size_t size = 0;
};

struct Snapshot {
  // A timeline state owns a small table of immutable pages. Adjacent states
  // point at the same chunks until the game changes one, so a long TAS does
  // not retain one complete .data/.bss image for every frame.
  std::vector<std::vector<std::shared_ptr<const std::vector<uint8_t>>>> chunks;
};

struct Runtime {
#ifdef _WIN32
  HMODULE handle = nullptr;
#else
  void *handle = nullptr;
#endif
  std::filesystem::path path;
  void (*init)() = nullptr;
  void (*update)() = nullptr;
  void (*set_render_api)(RenderApi *) = nullptr;
  void (*render_display_list)(uint32_t, uint32_t) = nullptr;
  bool (*get_scene_camera)(sm64_scene_camera *) = nullptr;
  void (*prepare_scene)(const sm64_scene_config *) = nullptr;
  bool (*get_mario_pose)(sm64_scene_mario *) = nullptr;
  bool (*edit_mario)(uint32_t, const uint32_t *) = nullptr;
  Pad *pads = nullptr;
  void **mario_state = nullptr;
  std::vector<Segment> segments;
  std::shared_ptr<const Snapshot> power_on;
  // Keep the comparison baseline alive even when its world is destroyed or
  // overwritten by a branch copy.
  mutable std::shared_ptr<const Snapshot> active_snapshot;
  ::sm64_world *active_world = nullptr;
  // One native game image may back several setup files. Its globals can only
  // represent one activated state at a time.
  sm64_vulkan *vk_renderers[2]{};
  unsigned render_slot = 0;
  std::mutex mutex;

  ~Runtime() {
    for (auto *renderer : vk_renderers) if (renderer) sm64_vulkan_destroy(renderer);
#ifdef _WIN32
    if (handle) FreeLibrary(handle);
#else
    if (handle) dlclose(handle);
#endif
  }
};

struct InputHistory {
  std::vector<std::shared_ptr<std::vector<sm64_input>>> chunks;
  size_t size = 0;

  void push(sm64_input input) {
    if (chunks.empty() || chunks.back()->size() == 256) {
      chunks.push_back(std::make_shared<std::vector<sm64_input>>());
      chunks.back()->reserve(256);
    } else if (!chunks.back().unique()) {
      chunks.back() = std::make_shared<std::vector<sm64_input>>(*chunks.back());
    }
    chunks.back()->push_back(input);
    ++size;
  }

  std::vector<sm64_input> flatten() const {
    std::vector<sm64_input> result;
    result.reserve(size);
    for (const auto &chunk : chunks) result.insert(result.end(), chunk->begin(), chunk->end());
    return result;
  }
};

struct StateEdit {
  uint32_t frame = 0, property = 0;
  std::array<uint32_t, 3> value{};
};
constexpr size_t kMaxEdits = 1'000'000;

struct RomInfo {
  uint32_t crc = 0;
  uint8_t country = 0;
};

struct Session {
  std::shared_ptr<Runtime> runtime;
  std::shared_ptr<const Snapshot> initial;
  std::vector<sm64_input> prefix;
  RomInfo rom;
  std::string rom_path, movie_path;
  uint32_t start_frame = 0;
  uint64_t fingerprint = 0;
  std::string level_name = "Super Mario 64";
  uint32_t level_id = 16;
};

const char *get_level_title(uint32_t id) {
  switch (id) {
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
    case 30: return "Super Mario 64 - Bowser in the Dark World (Boss)";
    case 33: return "Super Mario 64 - Bowser in the Fire Sea (Boss)";
    case 34: return "Super Mario 64 - Bowser in the Sky (Boss)";
    case 27: return "Super Mario 64 - Princess's Secret Slide";
    case 28: return "Super Mario 64 - Cavern of the Metal Cap";
    case 29: return "Super Mario 64 - Tower of the Wing Cap";
    case 18: return "Super Mario 64 - Vanish Cap Under the Moat";
    case 31: return "Super Mario 64 - Wing Mario Over the Rainbow";
    case 20: return "Super Mario 64 - The Secret Aquarium";
    default: return "Super Mario 64";
  }
}

} // namespace

struct sm64_level {
  std::shared_ptr<Session> session;
};

struct sm64_world {
  std::shared_ptr<Session> session;
  std::shared_ptr<const Snapshot> state;
  InputHistory history;
  std::shared_ptr<std::vector<StateEdit>> edits = std::make_shared<std::vector<StateEdit>>();
  sm64_view view{};
  sm64_scene_camera camera{};
  sm64_scene_mario pose{};
  uint64_t revision = next_revision.fetch_add(1, std::memory_order_relaxed);
  bool dirty = false;
  bool scratch = false;
};

namespace {

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

uint64_t hash_bytes(uint64_t seed, const std::vector<uint8_t> &bytes) {
  constexpr uint64_t prime = 1099511628211ull;
  for (uint8_t byte : bytes) {
    seed ^= byte;
    seed *= prime;
  }
  return seed;
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

std::filesystem::path resolve(const std::filesystem::path &base, const std::string &value) {
  const std::filesystem::path path(value);
  return path.is_absolute() ? path : base / path;
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

std::vector<sm64_input> load_m64_prefix(const std::filesystem::path &path, uint32_t frames, RomInfo rom) {
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

std::shared_ptr<Snapshot> capture(const Runtime &runtime, const Snapshot *previous = nullptr) {
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->chunks.reserve(runtime.segments.size());
  for (size_t segment_index = 0; segment_index < runtime.segments.size(); ++segment_index) {
    const Segment &segment = runtime.segments[segment_index];
    auto &chunks = snapshot->chunks.emplace_back();
    const size_t num_chunks = (segment.size + kSnapshotChunkSize - 1) / kSnapshotChunkSize;
    chunks.reserve(num_chunks);
    const auto *old_chunks = previous && segment_index < previous->chunks.size() ? &previous->chunks[segment_index] : nullptr;
    for (size_t offset = 0; offset < segment.size; offset += kSnapshotChunkSize) {
      const size_t chunk_size = std::min(kSnapshotChunkSize, segment.size - offset);
      if (old_chunks && chunks.size() < old_chunks->size()) {
        const auto &old = (*old_chunks)[chunks.size()];
        if (old && old->size() == chunk_size && std::memcmp(segment.address + offset, old->data(), chunk_size) == 0) {
          chunks.push_back(old);
          continue;
        }
      }
      chunks.push_back(std::make_shared<const std::vector<uint8_t>>(segment.address + offset, segment.address + offset + chunk_size));
    }
  }
  return snapshot;
}

void restore(const Runtime &runtime, const std::shared_ptr<const Snapshot> &state) {
  const Snapshot &snapshot = *state;
  if (snapshot.chunks.size() != runtime.segments.size()) fail("SM64 snapshot has incompatible memory sections");
  if (runtime.active_snapshot == state) return;
  const Snapshot *current = runtime.active_snapshot.get();
  for (size_t i = 0; i < runtime.segments.size(); ++i) {
    const Segment &segment = runtime.segments[i];
    const auto &chunks = snapshot.chunks[i];
    const auto *cur_chunks = current && i < current->chunks.size() ? &current->chunks[i] : nullptr;
    size_t offset = 0;
    for (size_t c = 0; c < chunks.size(); ++c) {
      const auto &chunk = chunks[c];
      if (!chunk || chunk->empty() || offset + chunk->size() > segment.size) fail("SM64 snapshot has incompatible memory size");
      if (!cur_chunks || c >= cur_chunks->size() || (*cur_chunks)[c] != chunk) {
        if (std::memcmp(segment.address + offset, chunk->data(), chunk->size()) != 0)
          std::memcpy(segment.address + offset, chunk->data(), chunk->size());
      }
      offset += chunk->size();
    }
    if (offset != segment.size) fail("SM64 snapshot has incompatible memory size");
  }
  runtime.active_snapshot = state;
}

#ifndef _WIN32
std::vector<Segment> elf_segments(const std::filesystem::path &path, void *handle) {
  const auto file = read_bytes(path);
  if (file.size() < sizeof(Elf64_Ehdr)) fail("SM64 backend is not a 64-bit ELF shared library");
  const auto *header = reinterpret_cast<const Elf64_Ehdr *>(file.data());
  if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 || header->e_ident[EI_CLASS] != ELFCLASS64) {
    fail("SM64 backend is not a 64-bit ELF shared library");
  }
  if (header->e_shoff == 0 || header->e_shentsize != sizeof(Elf64_Shdr) ||
      header->e_shnum == 0 || header->e_shstrndx >= header->e_shnum ||
      header->e_shoff + size_t(header->e_shnum) * sizeof(Elf64_Shdr) > file.size()) {
    fail("SM64 backend has no readable ELF section table");
  }
  const auto *sections = reinterpret_cast<const Elf64_Shdr *>(file.data() + header->e_shoff);
  const Elf64_Shdr &names_section = sections[header->e_shstrndx];
  if (names_section.sh_offset + names_section.sh_size > file.size()) fail("SM64 backend has invalid ELF section names");
  const char *names = reinterpret_cast<const char *>(file.data() + names_section.sh_offset);
  link_map *map = nullptr;
  if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || !map) fail("Cannot locate loaded SM64 library");
  std::vector<Segment> result;
  for (uint16_t i = 0; i < header->e_shnum; ++i) {
    const Elf64_Shdr &section = sections[i];
    if (!(section.sh_flags & SHF_ALLOC) || !(section.sh_flags & SHF_WRITE) || section.sh_size == 0 || section.sh_name >= names_section.sh_size) continue;
    const std::string_view name(names + section.sh_name);
    if (name != ".data" && name != ".bss") continue;
    result.push_back({reinterpret_cast<uint8_t *>(map->l_addr + section.sh_addr), static_cast<size_t>(section.sh_size)});
  }
  if (result.empty()) fail("SM64 backend has no mutable .data/.bss sections");
  return result;
}
#else
std::vector<Segment> pe_segments(HMODULE module) {
  const auto *base = reinterpret_cast<const uint8_t *>(module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) fail("SM64 backend is not a Windows DLL");
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) fail("SM64 backend is not a 64-bit Windows DLL");
  const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  std::vector<Segment> result;
  for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    char raw_name[9]{};
    std::memcpy(raw_name, section->Name, 8);
    const std::string_view name(raw_name);
    if (name != ".data" && name != ".bss") continue;
    const size_t size = std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData);
    if (size) result.push_back({const_cast<uint8_t *>(base) + section->VirtualAddress, size});
  }
  if (result.empty()) fail("SM64 backend has no mutable .data/.bss sections");
  return result;
}
#endif

void *symbol(const Runtime &runtime, const char *name) {
#ifdef _WIN32
  return reinterpret_cast<void *>(GetProcAddress(runtime.handle, name));
#else
  return dlsym(runtime.handle, name);
#endif
}

std::mutex runtimes_mutex;
std::map<std::filesystem::path, std::weak_ptr<Runtime>> runtimes;

std::shared_ptr<Runtime> open_runtime(const std::filesystem::path &requested_path) {
  const std::filesystem::path path = std::filesystem::absolute(requested_path).lexically_normal();
  std::lock_guard lock(runtimes_mutex);
  if (const auto found = runtimes.find(path); found != runtimes.end()) {
    if (auto runtime = found->second.lock()) return runtime;
  }
  auto runtime = std::make_shared<Runtime>();
  runtime->path = path;
#ifdef _WIN32
  runtime->handle = LoadLibraryW(path.wstring().c_str());
  if (!runtime->handle) fail("Cannot load SM64 library: Windows error " + std::to_string(GetLastError()));
#else
  runtime->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!runtime->handle) fail(std::string("Cannot load SM64 library: ") + dlerror());
#endif
  runtime->init = reinterpret_cast<void (*)()>(symbol(*runtime, "sm64_init"));
  runtime->update = reinterpret_cast<void (*)()>(symbol(*runtime, "sm64_update"));
  runtime->set_render_api = reinterpret_cast<void (*)(RenderApi *)>(symbol(*runtime, "sm64_set_render_api"));
  runtime->render_display_list = reinterpret_cast<void (*)(uint32_t, uint32_t)>(symbol(*runtime, "sm64_render_display_list"));
  runtime->get_scene_camera = reinterpret_cast<bool (*)(sm64_scene_camera *)>(symbol(*runtime, "sm64_get_scene_camera"));
  runtime->prepare_scene = reinterpret_cast<void (*)(const sm64_scene_config *)>(symbol(*runtime, "sm64_prepare_scene"));
  runtime->edit_mario = reinterpret_cast<bool (*)(uint32_t, const uint32_t *)>(symbol(*runtime, "sm64_edit_mario"));
  runtime->get_mario_pose = reinterpret_cast<bool (*)(sm64_scene_mario *)>(symbol(*runtime, "sm64_get_mario_pose"));
  runtime->pads = reinterpret_cast<Pad *>(symbol(*runtime, "gControllerPads"));
  runtime->mario_state = reinterpret_cast<void **>(symbol(*runtime, "gMarioState"));
  if (!runtime->init || !runtime->update || !runtime->set_render_api || !runtime->render_display_list || !runtime->pads || !runtime->mario_state) {
    fail("The selected library is not FrameTee's full-game libsm64 backend. Build the native full-game library for this platform.");
  }
#ifdef _WIN32
  runtime->segments = pe_segments(runtime->handle);
#else
  runtime->segments = elf_segments(path, runtime->handle);
#endif
  // Current runtimes regenerate display lists on every render. Exclude their
  // large transient pool from all simulation snapshots (legacy images retain it).
  auto render_memory = reinterpret_cast<void (*)(void **, size_t *)>(symbol(*runtime, "sm64_render_memory"));
  if (render_memory) {
    void *address = nullptr; size_t size = 0;
    render_memory(&address, &size);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address), end = begin + size;
    std::vector<Segment> simulation;
    for (auto segment : runtime->segments) {
      const uintptr_t first = reinterpret_cast<uintptr_t>(segment.address), last = first + segment.size;
      if (end <= first || begin >= last) simulation.push_back(segment);
      else {
        if (begin > first) simulation.push_back({segment.address, begin - first});
        if (end < last) simulation.push_back({reinterpret_cast<uint8_t *>(end), last - end});
      }
    }
    runtime->segments = std::move(simulation);
  }
  runtime->init();
  runtime->power_on = capture(*runtime);
  runtime->active_snapshot = runtime->power_on;
  runtimes[path] = runtime;
  return runtime;
}

sm64_view inspect(Runtime &runtime, uint32_t frame) {
  sm64_view result{};
  result.frame = frame;
  if (!runtime.mario_state || !*runtime.mario_state) return result;
  const auto *mario = static_cast<const uint8_t *>(*runtime.mario_state);
  std::memcpy(&result.action, mario + 0x0c, sizeof(result.action));
  std::memcpy(result.pos, mario + 0x3c, sizeof(result.pos));
  std::memcpy(result.vel, mario + 0x48, sizeof(result.vel));
  int16_t health = 0;
  std::memcpy(&health, mario + 0xea, sizeof(health));
  result.health = health;
  result.valid = result.action != 0;
  return result;
}

bool valid_edit(const StateEdit &edit) {
  if (edit.property < 2) {
    float values[3];
    std::memcpy(values, edit.value.data(), sizeof(values));
    for (float value : values) if (!std::isfinite(value) || std::abs(value) > 1'000'000.f) return false;
    return true;
  }
  return edit.property == 3 && edit.value[0] <= 0x880 && edit.value[1] == 0 && edit.value[2] == 0;
}

void apply_edit(Runtime &runtime, const StateEdit &edit) {
  if (!valid_edit(edit)) fail("Invalid SM64 state edit");
  if (!inspect(runtime, 0).valid) fail("Mario is not active");
  if (runtime.edit_mario) {
    if (!runtime.edit_mario(edit.property, edit.value.data())) fail("Native SM64 state edit failed");
    runtime.active_snapshot = nullptr;
    return;
  }
  auto *mario = static_cast<uint8_t *>(*runtime.mario_state);
  if (edit.property < 2) {
    std::memcpy(mario + (edit.property == 0 ? 0x3c : 0x48), edit.value.data(), 12);
  } else {
    const int16_t health = static_cast<int16_t>(edit.value[0]);
    std::memcpy(mario + 0xea, &health, sizeof(health));
  }
  runtime.active_snapshot = nullptr;
}

void set_input(Runtime &runtime, sm64_input input) {
  runtime.pads[0].buttons = input.buttons;
  runtime.pads[0].stick_x = input.stick_x;
  runtime.pads[0].stick_y = input.stick_y;
}

void replay(Runtime &runtime, const std::shared_ptr<const Snapshot> &start, const std::vector<sm64_input> &inputs) {
  restore(runtime, start);
  runtime.active_snapshot = nullptr;
  for (const sm64_input input : inputs) {
    set_input(runtime, input);
    runtime.update();
  }
  runtime.active_snapshot = nullptr;
}

void sync_active_world(Runtime &runtime, sm64_world *target) {
  if (runtime.active_world == target) return;
  if (runtime.active_world && runtime.active_world->dirty) {
    {
      runtime.active_world->state = capture(runtime, runtime.active_world->state.get());
      runtime.active_snapshot = runtime.active_world->state;
    }
    runtime.active_world->dirty = false;
  }
  if (target && target->state) {
    restore(runtime, target->state);
  }
  runtime.active_world = target;
}

// Fast3D caches hold pointers owned by the renderer, and rendering also writes
// dimensions read by the game. Neither may enter a simulation snapshot. Restore
// the exact pre-render state on every exit, including backend failures.
struct RenderStateGuard {
  Runtime &runtime;
  std::shared_ptr<const Snapshot> state;
  explicit RenderStateGuard(sm64_world &world) : runtime(*world.session->runtime) {
    sync_active_world(runtime, &world);
    if (world.dirty) {
      {
        world.state = capture(runtime, world.state.get());
      }
      world.dirty = false;
    }
    state = world.state;
    runtime.active_snapshot = nullptr;
  }
  ~RenderStateGuard() { restore(runtime, state); }
};

void multiply_matrix(float out[16], const float a[16], const float b[16]) {
  float result[16]{};
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k) result[col*4+row] += a[k*4+row] * b[col*4+k];
  std::memcpy(out, result, sizeof(result));
}

bool normalize_vector(float v[3]) {
  const float length = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
  if (!std::isfinite(length) || length < 1.e-6f) return false;
  for (int i = 0; i < 3; ++i) v[i] /= length;
  return true;
}

// Must be inside RenderStateGuard: scene traversal allocates native display
// lists and updates render-only object/animation bookkeeping.
void prepare_scene(Runtime &runtime, const sm64_render_config &config) {
  if (!config.mode) {
    if (runtime.prepare_scene) {
      sm64_scene_config scene{};
      scene.ghosts = config.ghosts; scene.ghost_count = config.ghost_count; scene.scene_only = config.scene_only;
      scene.width = config.width; scene.height = config.height;
      runtime.prepare_scene(&scene);
    }
    return;
  }
  if (!runtime.prepare_scene) fail("This SM64 runtime has no editor camera support; rebuild or reinstall the runtime");
  if (config.mode > 2) fail("Invalid SM64 camera mode");
  for (float value : config.view_proj) if (!std::isfinite(value)) fail("Invalid SM64 camera matrix");
  sm64_scene_config scene{};
  scene.ghosts = config.ghosts; scene.ghost_count = config.ghost_count; scene.scene_only = config.scene_only;
  scene.mode = config.mode; scene.width = config.width; scene.height = config.height;
  float back[3], right[3], up[3];
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(config.eye[i]) || !std::isfinite(config.target[i])) fail("Invalid SM64 camera position");
    back[i] = config.eye[i] - config.target[i];
  }
  if (!normalize_vector(back)) fail("SM64 camera eye and target coincide");
  for (int i = 0; i < 3; ++i)
    right[i] = config.up[(i+1)%3]*back[(i+2)%3] - config.up[(i+2)%3]*back[(i+1)%3];
  if (!normalize_vector(right)) fail("Invalid SM64 camera up vector");
  for (int i = 0; i < 3; ++i) up[i] = back[(i+1)%3]*right[(i+2)%3] - back[(i+2)%3]*right[(i+1)%3];
  float inverse_view[16]{};
  for (int i = 0; i < 3; ++i) {
    scene.view[i*4] = inverse_view[i] = right[i];
    scene.view[i*4+1] = inverse_view[4+i] = up[i];
    scene.view[i*4+2] = inverse_view[8+i] = back[i];
    scene.view[12] -= right[i]*config.eye[i];
    scene.view[13] -= up[i]*config.eye[i];
    scene.view[14] -= back[i]*config.eye[i];
    inverse_view[12+i] = config.eye[i];
  }
  inverse_view[15] = scene.view[15] = 1.f;
  multiply_matrix(scene.projection, config.view_proj, inverse_view);
  const float aspect_compensation = (float(config.width)/config.height) / (4.f/3.f);
  for (int col = 0; col < 4; ++col) {
    // Undo engine Vulkan Y/reversed Z; Fast3D applies its own aspect correction.
    scene.projection[col*4] *= aspect_compensation;
    scene.projection[col*4+1] *= -1.f;
    scene.projection[col*4+2] = scene.projection[col*4+3] - 2.f*scene.projection[col*4+2];
  }
  std::memcpy(scene.eye, config.eye, sizeof(scene.eye));
  std::memcpy(scene.target, config.target, sizeof(scene.target));
  runtime.prepare_scene(&scene);
}

void update_world(sm64_world &world, sm64_input input) {
  if (world.history.size >= kMaxFrames) fail("Movie frame limit reached");
  Runtime &runtime = *world.session->runtime;
  sync_active_world(runtime, &world);
  set_input(runtime, input);
  runtime.update();
  runtime.active_snapshot = nullptr;
  world.dirty = true;
  world.history.push(input);
  world.view = inspect(runtime, static_cast<uint32_t>(world.history.size));
  if (runtime.get_scene_camera) runtime.get_scene_camera(&world.camera);
  if (runtime.get_mario_pose) runtime.get_mario_pose(&world.pose);
  world.revision = next_revision.fetch_add(1, std::memory_order_relaxed);
}

std::vector<uint8_t> encode_state(const sm64_world &world) {
  const auto inputs = world.history.flatten();
  std::vector<uint8_t> result;
  result.reserve(24 + inputs.size() * 4 + world.edits->size() * 20);
  result.insert(result.end(), {'F', 'T', 'S', 'M', '6', '4', '0', '3'});
  const uint64_t fingerprint = world.session->fingerprint;
  for (int i = 0; i < 8; ++i) result.push_back(static_cast<uint8_t>(fingerprint >> (i * 8)));
  const uint32_t count = static_cast<uint32_t>(inputs.size());
  for (int i = 0; i < 4; ++i) result.push_back(static_cast<uint8_t>(count >> (i * 8)));
  const uint32_t edit_count = static_cast<uint32_t>(world.edits->size());
  for (int i = 0; i < 4; ++i) result.push_back(static_cast<uint8_t>(edit_count >> (i * 8)));
  for (sm64_input input : inputs) {
    result.push_back(static_cast<uint8_t>(input.buttons));
    result.push_back(static_cast<uint8_t>(input.buttons >> 8));
    result.push_back(static_cast<uint8_t>(input.stick_x));
    result.push_back(static_cast<uint8_t>(input.stick_y));
  }
  for (const StateEdit &edit : *world.edits) {
    const uint32_t values[] = {edit.frame, edit.property, edit.value[0], edit.value[1], edit.value[2]};
    for (uint32_t value : values)
      for (int i = 0; i < 4; ++i) result.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
  return result;
}

struct SavedState {
  std::vector<sm64_input> inputs;
  std::shared_ptr<std::vector<StateEdit>> edits = std::make_shared<std::vector<StateEdit>>();
};

SavedState decode_state(const sm64_world &world, const uint8_t *data, size_t size) {
  if (!data || size < 20 || (std::memcmp(data, "FTSM6402", 8) != 0 && std::memcmp(data, "FTSM6403", 8) != 0)) fail("Invalid SM64 state");
  const bool version3 = data[7] == '3';
  const size_t header_size = version3 ? 24 : 20;
  if (size < header_size) fail("Invalid SM64 state length");
  uint64_t fingerprint = 0;
  for (int i = 0; i < 8; ++i) fingerprint |= uint64_t(data[8 + i]) << (i * 8);
  if (fingerprint != world.session->fingerprint) fail("State belongs to a different ROM, backend, or starting movie");
  uint32_t count = 0;
  for (int i = 0; i < 4; ++i) count |= uint32_t(data[16 + i]) << (i * 8);
  uint32_t edit_count = 0;
  if (version3) for (int i = 0; i < 4; ++i) edit_count |= uint32_t(data[20 + i]) << (i * 8);
  if (count > kMaxFrames || edit_count > kMaxEdits || size != header_size + size_t(count) * 4 + size_t(edit_count) * 20) fail("Invalid SM64 state length");
  SavedState result;
  result.inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t *input = data + header_size + i * 4;
    result.inputs.push_back({uint16_t(input[0] | uint16_t(input[1]) << 8), static_cast<int8_t>(input[2]), static_cast<int8_t>(input[3])});
  }
  result.edits->reserve(edit_count);
  for (size_t i = 0; i < edit_count; ++i) {
    const uint8_t *bytes = data + header_size + size_t(count) * 4 + i * 20;
    uint32_t values[5]{};
    for (int v = 0; v < 5; ++v)
      for (int b = 0; b < 4; ++b) values[v] |= uint32_t(bytes[v * 4 + b]) << (b * 8);
    StateEdit edit{values[0], values[1], {values[2], values[3], values[4]}};
    if (edit.frame > count || (!result.edits->empty() && edit.frame < result.edits->back().frame) || !valid_edit(edit))
      fail("Invalid SM64 state edit journal");
    result.edits->push_back(edit);
  }
  return result;
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
    textures_.clear();
    selected_[0] = selected_[1] = 0;
    active_textures_[0] = active_textures_[1] = nullptr;
    upload_texture_ = 0;
    shaders_.clear();
    shader_ = nullptr;
    next_texture_ = 1;
    // Match gfx_init's zeroed state cache. Otherwise the first background
    // writes depth without a callback ever disabling it, hiding the scene.
    depth_test_ = false;
    depth_mask_ = false;
    alpha_blend_ = false;
    zmode_decal_ = false;
    viewport_ = {0, 0, int(width), int(height)};
    scissor_ = viewport_;
    depth_.assign(size_t(width) * height, std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < size_t(width) * height; ++i) {
      pixels_[i * 4] = 0;
      pixels_[i * 4 + 1] = 0;
      pixels_[i * 4 + 2] = 0;
      pixels_[i * 4 + 3] = 255;
    }
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
  void draw(float *buffer, size_t length, size_t triangles) {
    if (!buffer || !shader_ || triangles == 0 || length % (triangles * 3) != 0) return;
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

    const uint32_t id = shader_->id;
    const bool used_textures[2] = {shader_->used_textures[0], shader_->used_textures[1]};
    const uint8_t input_count = shader_->input_count;
    size_t offset = 4;
    Color texels[2]{};
    if (used_textures[0] || used_textures[1]) {
      const Color uv = interpolate_weights(vertices, int(offset), 2, w0, w1, w2);
      texels[0] = texture(0, uv.r, uv.g);
      texels[1] = texture(1, uv.r, uv.g);
      offset += 2;
    }
    Color fog{};
    if (id & (1u << 25)) {
      fog = interpolate_weights(vertices, int(offset), 4, w0, w1, w2);
      offset += 4;
    }
    std::array<Color, 4> inputs{};
    const bool alpha = (id & (1u << 24)) != 0;
    for (uint8_t i = 0; i < input_count; ++i) {
      inputs[i] = interpolate_weights(vertices, int(offset), alpha ? 4 : 3, w0, w1, w2);
      offset += alpha ? 4 : 3;
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
    // gfx_pc hands the rendering API homogeneous clip coordinates. Hardware
    // APIs clip those coordinates automatically; do the same before CPU
    // rasterization so geometry crossing the screen or near plane remains
    // well formed.
    const auto distance = [](const float *vertex, int plane) {
      switch (plane) {
        case 0: return vertex[0] + vertex[3]; // left
        case 1: return vertex[3] - vertex[0]; // right
        case 2: return vertex[1] + vertex[3]; // bottom
        case 3: return vertex[3] - vertex[1]; // top
        case 4: return vertex[2] + vertex[3]; // near (OpenGL depth)
        default: return vertex[3] - vertex[2]; // far
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
void rapi_load(void *shader) { renderer().set_shader(shader); }
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

std::string escape_json(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    if (c == '\\' || c == '\"') result.push_back('\\');
    result.push_back(c);
  }
  return result;
}

} // namespace

extern "C" sm64_level *sm64_ft_open(const char *path, const char *, char *error, size_t error_size) {
  return boundary<sm64_level *>(error, error_size, nullptr, [&] {
    if (!path) fail("Missing SM64 setup path");
    const std::filesystem::path config_path(path);
    const auto config_bytes = read_bytes(config_path);
    const std::string config(config_bytes.begin(), config_bytes.end());
    const std::filesystem::path base = config_path.has_parent_path() ? config_path.parent_path() : ".";
    const auto rom_bytes = normalized_rom(read_bytes(resolve(base, json_string(config, "rom"))));
    const RomInfo rom = inspect_rom(rom_bytes);
    const std::filesystem::path library_path = resolve(base, json_string(config, "library"));
    const uint32_t start_frame = json_u32(config, "start_frame");
    const uint32_t target_level = json_u32(config, "level");
    const std::string movie = json_string(config, "movie", false);

    auto session = std::make_shared<Session>();
    session->runtime = open_runtime(library_path);
    session->rom = rom;
    session->rom_path = std::filesystem::absolute(resolve(base, json_string(config, "rom"))).string();
    session->movie_path = movie.empty() ? "" : std::filesystem::absolute(resolve(base, movie)).string();
    session->start_frame = start_frame;
    Runtime &runtime = *session->runtime;
    std::lock_guard lock(runtime.mutex);
    sync_active_world(runtime, nullptr);

    uint32_t dest = target_level != 0 ? target_level : 16;
    session->level_id = dest;
    session->level_name = get_level_title(dest);

    std::vector<sm64_input> prefix;
    if (!movie.empty()) {
      prefix = load_m64_prefix(resolve(base, movie), start_frame, rom);
      replay(runtime, runtime.power_on, prefix);
      session->initial = capture(runtime, runtime.power_on.get());
    } else {
      restore(runtime, runtime.power_on);
      runtime.active_snapshot = nullptr;
      auto *skip_intro = reinterpret_cast<bool *>(symbol(runtime, "configSkipIntro"));
      if (skip_intro) *skip_intro = true;
      auto startup_update = reinterpret_cast<void (*)()>(symbol(runtime, "sm64_update_startup"));

      prefix.reserve(200 + 120);
      for (uint32_t frame = 1; frame < 199; ++frame) {
        sm64_input in{};
        // Skip-intro still leaves the title/file-select state machine active.
        // Feed the same deterministic Start/A sequence used by the original
        // host path so the full game loop reaches playable Castle Grounds.
        if (frame >= 110 && frame <= 150 && (frame % 2 == 0)) in.buttons = 0x1000;
        else if (frame >= 160 && frame <= 175 && (frame % 2 == 0)) in.buttons = 0x8000;
        set_input(runtime, in);
        (startup_update ? startup_update : runtime.update)();
        prefix.push_back(in);
      }

      if (dest != 16) {
        auto initiate_warp = reinterpret_cast<void (*)(int16_t, int16_t, int16_t, int32_t)>(symbol(runtime, "initiate_warp"));
        auto fade_into_special_warp = reinterpret_cast<void (*)(uint32_t, uint32_t)>(symbol(runtime, "fade_into_special_warp"));
        if (initiate_warp && fade_into_special_warp) {
          initiate_warp(static_cast<int16_t>(dest), 1, 0x0A, 0);
          fade_into_special_warp(0, 0);
          for (uint32_t step = 1; step <= 116; ++step) {
            sm64_input in{};
            if (step % 2 == 0) in.buttons = 0x8000;
            set_input(runtime, in);
            (startup_update ? startup_update : runtime.update)();
            prefix.push_back(in);
          }
        }
      }
      if (!inspect(runtime, 0).valid) fail("SM64 automatic startup did not enter gameplay");
      session->initial = capture(runtime, runtime.power_on.get());
    }

    session->prefix = std::move(prefix);
    uint64_t fingerprint = 1469598103934665603ull;
    fingerprint = hash_bytes(fingerprint, rom_bytes);
    fingerprint = hash_bytes(fingerprint, read_bytes(library_path));
    fingerprint ^= dest;
    fingerprint *= 1099511628211ull;
    for (const sm64_input input : session->prefix) {
      fingerprint ^= input.buttons;
      fingerprint *= 1099511628211ull;
      fingerprint ^= static_cast<uint8_t>(input.stick_x);
      fingerprint *= 1099511628211ull;
      fingerprint ^= static_cast<uint8_t>(input.stick_y);
      fingerprint *= 1099511628211ull;
    }
    session->fingerprint = fingerprint;
    return new sm64_level{std::move(session)};
  });
}

extern "C" void sm64_ft_level_free(sm64_level *level) { delete level; }

extern "C" sm64_world *sm64_ft_world_new(const sm64_level *level, char *error, size_t error_size) {
  return boundary<sm64_world *>(error, error_size, nullptr, [&] {
    if (!level) fail("Missing SM64 level");
    auto world = std::make_unique<sm64_world>();
    world->session = level->session;
    std::lock_guard lock(world->session->runtime->mutex);
    Runtime &runtime = *world->session->runtime;
    sync_active_world(runtime, nullptr);
    world->state = world->session->initial;
    restore(runtime, world->state);
    world->view = inspect(runtime, 0);
    if (runtime.get_scene_camera) runtime.get_scene_camera(&world->camera);
    if (runtime.get_mario_pose) runtime.get_mario_pose(&world->pose);
    world->dirty = false;
    runtime.active_world = world.get();
    return world.release();
  });
}

extern "C" void sm64_ft_world_free(sm64_world *world) {
  if (!world) return;
  if (world->session && world->session->runtime) {
    std::lock_guard lock(world->session->runtime->mutex);
    if (world->session->runtime->active_world == world) {
      world->session->runtime->active_world = nullptr;
    }
  }
  delete world;
}

extern "C" void sm64_ft_world_set_scratch(sm64_world *world, bool scratch) {
  if (world) world->scratch = scratch;
}

extern "C" void sm64_ft_copy(sm64_world *destination, const sm64_world *source) {
  if (!destination || !source || destination == source) return;
  std::lock_guard lock(source->session->runtime->mutex);
  Runtime &runtime = *source->session->runtime;
  if (source->dirty && runtime.active_world == source) {
    {
      const_cast<sm64_world *>(source)->state = capture(runtime, source->state.get());
      runtime.active_snapshot = source->state;
    }
    const_cast<sm64_world *>(source)->dirty = false;
  }
  destination->session = source->session;
  destination->state = source->state;
  destination->history = source->history;
  destination->edits = source->edits;
  destination->view = source->view;
  destination->camera = source->camera;
  destination->pose = source->pose;
  destination->revision = source->revision;
  destination->dirty = false;
  if (runtime.active_world == destination) {
    runtime.active_world = nullptr;
  }
}

extern "C" bool sm64_ft_step(sm64_world *world, sm64_input input, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world) fail("Missing SM64 world");
    std::lock_guard lock(world->session->runtime->mutex);
    update_world(*world, input);
    return true;
  });
}

extern "C" uint32_t sm64_ft_view(const sm64_world *world, sm64_view *out) {
  if (!world || !out) return 0;
  *out = world->view;
  return static_cast<uint32_t>(world->history.size);
}

extern "C" uint64_t sm64_ft_revision(const sm64_world *world) { return world ? world->revision : 0; }

extern "C" bool sm64_ft_camera(const sm64_world *world, float aspect, sm64_camera *out) {
  if (!world || !out || !world->camera.valid || !std::isfinite(aspect) || aspect <= 0.f) return false;
  const auto &camera = world->camera;
  *out = {};
  std::memcpy(out->eye, camera.eye, sizeof(out->eye));
  std::memcpy(out->target, camera.target, sizeof(out->target));
  multiply_matrix(out->view_proj, camera.projection, camera.view);
  for (int col = 0; col < 4; ++col) {
    out->view_proj[col*4] *= (4.f/3.f)/aspect;
    out->view_proj[col*4+1] *= -1.f;
    out->view_proj[col*4+2] = (out->view_proj[col*4+3] - out->view_proj[col*4+2]) * .5f;
  }
  for (int i = 0; i < 3; ++i) out->up[i] = -out->view_proj[i*4+1];
  if (!normalize_vector(out->up)) return false;
  const auto *p = camera.projection;
  out->fov_y = 2.f*std::atan(1.f/std::sqrt(p[1]*p[1] + p[5]*p[5]));
  out->near_z = p[14]/(p[10]-1.f);
  out->far_z = p[14]/(p[10]+1.f);
  for (float value : out->view_proj) if (!std::isfinite(value)) return false;
  return true;
}

extern "C" bool sm64_ft_set(sm64_world *world, uint32_t property, const sm64_view *value, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !value) fail("Missing SM64 state edit");
    std::lock_guard lock(world->session->runtime->mutex);
    StateEdit edit{static_cast<uint32_t>(world->history.size), property, {}};
    if (property < 2) std::memcpy(edit.value.data(), property == 0 ? value->pos : value->vel, 12);
    else edit.value[0] = static_cast<uint32_t>(value->health);
    if (!valid_edit(edit)) fail("Invalid SM64 property value");
    if (world->edits->size() >= kMaxEdits) fail("SM64 state edit limit reached");
    Runtime &runtime = *world->session->runtime;
    sync_active_world(runtime, world);
    if (!world->edits.unique()) world->edits = std::make_shared<std::vector<StateEdit>>(*world->edits);
    if (world->edits->size() == world->edits->capacity())
      world->edits->reserve(std::max<size_t>(8, world->edits->size() * 2));
    apply_edit(runtime, edit);
    world->edits->push_back(edit);
    world->dirty = true;
    world->view = inspect(runtime, static_cast<uint32_t>(world->history.size));
    if (runtime.get_mario_pose) runtime.get_mario_pose(&world->pose);
    world->revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    return true;
  });
}

extern "C" size_t sm64_ft_save(const sm64_world *world, uint8_t *out, size_t size, char *error, size_t error_size) {
  return boundary<size_t>(error, error_size, 0, [&] {
    if (!world) fail("Missing SM64 world");
    const auto data = encode_state(*world);
    if (!out) return data.size();
    if (size < data.size()) fail("State buffer is too small");
    std::memcpy(out, data.data(), data.size());
    return data.size();
  });
}

extern "C" bool sm64_ft_load(sm64_world *world, const uint8_t *data, size_t size, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world) fail("Missing SM64 world");
    auto saved = decode_state(*world, data, size);
    std::lock_guard lock(world->session->runtime->mutex);
    Runtime &runtime = *world->session->runtime;
    sync_active_world(runtime, nullptr);
    restore(runtime, world->session->initial);
    runtime.active_snapshot = nullptr;
    size_t edit_index = 0;
    for (size_t frame = 0; frame <= saved.inputs.size(); ++frame) {
      while (edit_index < saved.edits->size() && (*saved.edits)[edit_index].frame == frame)
        apply_edit(runtime, (*saved.edits)[edit_index++]);
      if (frame < saved.inputs.size()) {
        set_input(runtime, saved.inputs[frame]);
        runtime.update();
      }
    }
    world->state = capture(runtime, world->state.get());
    world->dirty = false;
    runtime.active_snapshot = world->state;
    runtime.active_world = world;
    world->history = {};
    for (const sm64_input input : saved.inputs) world->history.push(input);
    world->edits = std::move(saved.edits);
    world->view = inspect(runtime, static_cast<uint32_t>(saved.inputs.size()));
    if (runtime.get_scene_camera) runtime.get_scene_camera(&world->camera);
    if (runtime.get_mario_pose) runtime.get_mario_pose(&world->pose);
    world->revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    return true;
  });
}

extern "C" bool sm64_ft_export(const sm64_world *world, const char *path, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !path) fail("Missing SM64 movie path");
    if (!world->edits->empty()) fail("M64 controller movies cannot represent state overrides; save a FrameTee project to preserve them");
    std::vector<sm64_input> inputs = world->session->prefix;
    const auto suffix = world->history.flatten();
    inputs.insert(inputs.end(), suffix.begin(), suffix.end());
    write_m64(path, world->session->rom, inputs);
    return true;
  });
}

extern "C" bool sm64_ft_render(const sm64_world *world, const sm64_render_config *config, uint8_t *pixels, size_t size, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !config || !pixels || config->width == 0 || config->height == 0 ||
        config->width > 4096 || config->height > 4096 || size < size_t(config->width) * config->height * 4) {
      fail("Invalid SM64 render target");
    }
    std::lock_guard lock(world->session->runtime->mutex);
    Runtime &runtime = *world->session->runtime;
    RenderStateGuard state_guard(*const_cast<sm64_world *>(world));
    prepare_scene(runtime, *config);
    SoftwareRenderer software;
    software.begin(pixels, config->width, config->height, config->mode > 0);
    active_renderer = &software;
    const RenderApi api = SoftwareRenderer::api();
    try {
      runtime.set_render_api(const_cast<RenderApi *>(&api));
      runtime.render_display_list(config->width, config->height);
    } catch (...) {
      active_renderer = nullptr;
      throw;
    }
    active_renderer = nullptr;
    return true;
  });
}

extern "C" bool sm64_ft_render_gpu(const sm64_world *world, const sm64_render_config *config, const ft_gpu_device *gpu, const ft_gpu_image *target_image, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!world || !config || !gpu || !target_image || config->width == 0 || config->height == 0) {
      fail("Invalid SM64 GPU render target");
    }
    std::lock_guard lock(world->session->runtime->mutex);
    Runtime &runtime = *world->session->runtime;
    RenderStateGuard state_guard(*const_cast<sm64_world *>(world));
    prepare_scene(runtime, *config);

    auto *&renderer = runtime.vk_renderers[runtime.render_slot++ % 2];
    if (!renderer) {
      renderer = sm64_vulkan_create(gpu, error, error_size);
      if (!renderer) fail("Failed to create SM64 Vulkan renderer");
    }

    return sm64_vulkan_render(renderer,
                              runtime.render_display_list,
                              reinterpret_cast<void (*)(void *)>(runtime.set_render_api),
                              config->width,
                              config->height,
                              target_image,
                              config->mode > 0,
                              error,
                              error_size);
  });
}

extern "C" void sm64_ft_release_graphics(void) {
  std::lock_guard registry_lock(runtimes_mutex);
  for (auto &[path, weak] : runtimes) {
    if (auto runtime = weak.lock()) {
      std::lock_guard runtime_lock(runtime->mutex);
      for (auto *&renderer : runtime->vk_renderers) {
        if (renderer) sm64_vulkan_destroy(renderer);
        renderer = nullptr;
      }
    }
  }
}

extern "C" const char *sm64_ft_level_name(const sm64_level *level) {
  if (!level || !level->session) return "Super Mario 64";
  return level->session->level_name.c_str();
}

extern "C" bool sm64_ft_write_config(const char *path, const char *rom, const char *library, const char *movie, uint32_t frame, uint32_t level, char *error, size_t error_size) {
  return boundary<bool>(error, error_size, false, [&] {
    if (!path || !rom || !library || !movie) fail("Missing SM64 setup value");
    const std::filesystem::path config_path(path);
    if (config_path.has_parent_path()) {
      std::error_code directory_error;
      std::filesystem::create_directories(config_path.parent_path(), directory_error);
      if (directory_error) fail("Cannot create SM64 setup directory: " + directory_error.message());
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) fail(std::string("Cannot write ") + path);
    file << "{\n  \"rom\": \"" << escape_json(rom) << "\",\n"
         << "  \"library\": \"" << escape_json(library) << "\",\n";
    if (movie[0]) file << "  \"movie\": \"" << escape_json(movie) << "\",\n";
    if (level) file << "  \"level\": " << level << ",\n";
    file << "  \"start_frame\": " << frame << "\n}\n";
    if (!file) fail(std::string("Cannot write ") + path);
    return true;
  });
}


namespace {
struct PhysicsOwner {
  sm64_physics api{};
  std::filesystem::path directory;
  std::shared_ptr<Runtime> runtime;
  std::weak_ptr<const Snapshot> last_checkpoint;
  ~PhysicsOwner() {
    runtime.reset(); // Unload the image before removing it (also on Windows).
    if (!directory.empty()) { std::error_code ec; std::filesystem::remove_all(directory, ec); }
  }
};
}
struct sm64_checkpoint {
  std::shared_ptr<Runtime> runtime;
  std::shared_ptr<const Snapshot> state;
};

extern "C" sm64_physics *sm64_ft_physics_create(const sm64_world *source, char *error, size_t error_size) {
  return boundary<sm64_physics *>(error, error_size, nullptr, [&]() -> sm64_physics * {
    if (!source) fail("Missing SM64 source world");
    std::shared_ptr<Session> session;
    std::vector<sm64_input> inputs;
    std::vector<StateEdit> edits;
    {
      std::lock_guard lock(source->session->runtime->mutex);
      session = source->session;
      inputs = source->history.flatten();
      edits = *source->edits;
    }
    auto owner = std::make_unique<PhysicsOwner>();
    const auto root = std::filesystem::temp_directory_path();
    for (;;) {
      const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
      auto candidate = root / ("frametee-sm64-worker-" + std::to_string(id) + "-" + std::to_string(next_revision.fetch_add(1)));
      if (std::filesystem::create_directory(candidate)) { owner->directory = candidate; break; }
    }
    const auto library = owner->directory / session->runtime->path.filename();
    std::filesystem::copy_file(session->runtime->path, library);
    const auto config = owner->directory / "worker.sm64";
    if (!sm64_ft_write_config(config.string().c_str(), session->rom_path.c_str(), library.string().c_str(),
                             session->movie_path.c_str(), session->start_frame, session->level_id, error, error_size))
      return nullptr;
    std::unique_ptr<sm64_level> level(sm64_ft_open(config.string().c_str(), owner->directory.string().c_str(), error, error_size));
    if (!level) return nullptr;
    owner->runtime = level->session->runtime;
    Runtime &runtime = *owner->runtime;
    auto bind = reinterpret_cast<const sm64_view *(*)(uint32_t)>(symbol(runtime, "sm64_physics_bind"));
    auto tick = reinterpret_cast<void (*)(sm64_input)>(symbol(runtime, "sm64_physics_step"));
    if (!bind || !tick) fail("Rebuild the SM64 runtime for isolated physics support");
    restore(runtime, level->session->initial);
    runtime.active_snapshot = nullptr;
    size_t edit_index = 0;
    for (size_t frame = 0; frame <= inputs.size(); ++frame) {
      while (edit_index < edits.size() && edits[edit_index].frame == frame) apply_edit(runtime, edits[edit_index++]);
      if (frame < inputs.size()) { set_input(runtime, inputs[frame]); runtime.update(); }
    }
    auto &api = owner->api;
    api.step = tick;
    api.view = bind(static_cast<uint32_t>(inputs.size()));
    api.mario = *runtime.mario_state;
    api.owner = owner.get();
    api.capture = [](sm64_physics *sim, char *error, size_t size) -> sm64_checkpoint * {
      return boundary<sm64_checkpoint *>(error, size, nullptr, [&] {
        auto &owner = *static_cast<PhysicsOwner *>(sim->owner);
        auto previous = owner.last_checkpoint.lock();
        auto state = capture(*owner.runtime, previous.get());
        owner.last_checkpoint = state;
        return new sm64_checkpoint{owner.runtime, std::move(state)};
      });
    };
    api.restore = [](sm64_physics *sim, const sm64_checkpoint *state, char *error, size_t size) {
      return boundary<bool>(error, size, false, [&] {
        auto &owner = *static_cast<PhysicsOwner *>(sim->owner);
        if (!state || state->runtime != owner.runtime) fail("SM64 checkpoint belongs to a different physics instance");
        owner.runtime->active_snapshot = nullptr; // Direct ticks bypass snapshot bookkeeping.
        restore(*owner.runtime, state->state);
        owner.last_checkpoint = state->state;
        sim->mario = *owner.runtime->mario_state;
        return true;
      });
    };
    api.free_checkpoint = [](sm64_checkpoint *state) { delete state; };
    api.destroy = [](sm64_physics *sim) { if (sim) delete static_cast<PhysicsOwner *>(sim->owner); };
    owner.release();
    return &api;
  });
}

extern "C" bool sm64_ft_pose(const sm64_world *world, sm64_scene_mario *out) {
  if (!world || !out) return false;
  *out = world->pose; return out->valid;
}
