// Reading the installed packs directly.
//
// ForeverValidator opens the packs to simulate a track, and along the way it
// decodes exactly as much of each material as the physics needs: which surface
// it is, and whether it is water. It never resolves a material to the picture
// painted on it, because nothing in a validator has any use for one.
//
// This file does that last step. It is the same pack reader, the same GBX
// reference tables and the same material decoder the validator uses (nothing
// here reimplements a format the submodule already knows) followed by a DDS
// decoder, which it does not have, because the textures are the one thing it
// never looks at.
//
// The chain, for one surface on screen:
//
//     block solid   .Solid.Gbx    external refs -> the materials it uses
//     material      .Material.Gbx external refs -> the textures it uses
//     texture       .Texture.gbx  external ref  -> the image
//     image         .dds          DXT1/DXT3/DXT5 -> pixels
//
// Every step but the last is a GBX reference table, which the validator already
// parses and hands over.

#include "tmnf_internal.h"

// ForeverValidator internals; see tmnf_vehicle_model.cpp for why this module
// reaches past the published headers and what that costs.
#include "engine/game/material_definition.h"
#include "format/archive/archive_class_ids.h"
#include "format/archive/tmnf_gbx_body_reader.h"
#include "format/materials/material_archive_decoder.h"
#include "format/pack/installed/byte_buffer.h"
#include "format/pack/installed/installed_pack_key_catalog.h"
#include "format/pack/installed/plug_file_pack.h"
#include "format/pack/installed/scene_descriptor_folder_paths.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace tmnf {
namespace {

std::vector<std::byte> ReadFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const auto size = static_cast<std::size_t>(file.tellg());
  file.seekg(0);
  std::vector<std::byte> bytes(size);
  file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
  return bytes;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Paths inside a pack are Windows-shaped and compared case insensitively, which
// matters: the same texture is spelled ".Texture.Gbx" in one reference and
// ".Texture.gbx" in the next.
std::string NormalizePath(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (const char c : path) out.push_back(c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// Where the installed game keeps the files its packs only point at.
//
// The pictures are not in the packs. A .pak holds the descriptors (the
// materials, the shaders, the bitmaps) and each bitmap names an image that
// lives on disk under GameData, which is why a copy of Packs on its own is
// enough to simulate a track and not enough to draw one.
//
// FrameTee keeps both directories in one fixed per-game data directory.
std::string FindGameData(const std::string &packs_dir) {
  const std::filesystem::path candidate = std::filesystem::path(packs_dir).parent_path() / "GameData";
  std::error_code error;
  if (std::filesystem::is_directory(candidate, error)) return candidate.string();
  return {};
}

} // namespace

// --- one opened pack ---------------------------------------------------------

struct Pack {
  std::string name;
  std::vector<std::byte> bytes;
  CPlugFilePack pack;
  // Every file in the pack by normalized plain path, so a reference resolves in
  // one lookup instead of a scan over two thousand descriptors.
  std::unordered_map<std::string, const CPlugFileFidContainer_SFileDesc *> by_path;

  Pack() = default;
  Pack(const Pack &) = delete;
  Pack &operator=(const Pack &) = delete;
};

PackSet::PackSet() = default;
PackSet::~PackSet() = default;

bool PackSet::Open(const std::string &packs_dir, const std::vector<std::string> &pack_names) {
  root_ = packs_dir;
  data_root_ = FindGameData(packs_dir);
  const std::vector<std::byte> packlist = ReadFile(packs_dir + "/packlist.dat");
  if (packlist.empty()) return false;
  keys_ = std::make_unique<InstalledPackKeyCatalog>();
  if (!keys_->LoadFromMemory(packlist.data(), packlist.size(), "")) {
    keys_.reset();
    return false;
  }
  for (const std::string &name : pack_names) OpenOne(name);
  return !packs_.empty();
}

void PackSet::Close() {
  material_paths_.reset();
  packs_.clear();
  keys_.reset();
}

bool PackSet::OpenOne(const std::string &name) {
  if (name.empty() || !keys_) return false;
  for (const auto &pack : packs_)
    if (pack->name == name) return true;

  auto pack = std::make_unique<Pack>();
  pack->name = name;
  pack->bytes = ReadFile(root_ + "/" + name + ".pak");
  if (pack->bytes.empty()) return false;
  if (!pack->pack.OpenFromMemory(pack->bytes.data(), pack->bytes.size(), *keys_, name.c_str())) return false;

  char path[1024];
  pack->by_path.reserve(pack->pack.files.size() * 2u);
  for (const CPlugFileFidContainer_SFileDesc &file : pack->pack.files) {
    if (pack->pack.FileDescPlainPath(&file, path, sizeof(path))) pack->by_path.emplace(NormalizePath(path), &file);
  }
  packs_.push_back(std::move(pack));
  return true;
}

// A file's bytes, whichever pack holds it. Content is spread across the packs
// with no rule a caller could apply: a stadium block's material sits in
// Stadium.pak and the cloud texture it reflects sits in Game.pak, so the
// lookup simply asks each of them.
//
// Most of a pack's entries are stored under a hash of their name rather than
// the name itself, so a reference that names a file has to be hashed the same
// way before it will be found. That is what the game does and it is what the
// second lookup below does.
bool PackSet::Read(std::string_view plain_path, std::vector<unsigned char> *out) const {
  if (plain_path.empty() || out == nullptr) return false;
  const std::string plain(plain_path);

  // Every way the packs are known to file something. A hash is taken over the
  // path below a known media folder, so which folder a file lives under decides
  // how it is named, and the rules for that are the validator's, not this
  // module's guesses.
  std::vector<std::string> hashed;
  const auto add_hash = [&](int (*hash)(const char *, char *, std::size_t)) {
    char buffer[1024];
    if (hash(plain.c_str(), buffer, sizeof(buffer))) hashed.emplace_back(buffer);
  };
  add_hash(&SceneDescriptorFolderPaths::HashMediaTexturePath);
  add_hash(&SceneDescriptorFolderPaths::HashMediaMaterialPath);
  add_hash(&SceneDescriptorFolderPaths::HashMediaSolidPath);
  add_hash(&SceneDescriptorFolderPaths::HashMediaShaderPath);
  {
    // The general rule, for anything the named folders do not cover.
    const std::size_t slash = plain.find_last_of('\\');
    char buffer[1024];
    if (slash != std::string::npos &&
        SceneDescriptorFolderPaths::HashFileNameDescriptorPathWithBase(
            plain.c_str(), plain.substr(0u, slash + 1u).c_str(), buffer, sizeof(buffer)))
      hashed.emplace_back(buffer);
  }

  for (const auto &pack : packs_) {
    // A file may also be stored under a name the pack itself maps the reference
    // to, which is a fourth routing and the only one that needs the pack.
    char selected[1024]{};
    std::vector<std::string> candidates;
    candidates.reserve(hashed.size() + 2u);
    candidates.push_back(plain);
    if (pack->pack.SelectedPathForPlainRef(plain.c_str(), selected, sizeof(selected))) candidates.emplace_back(selected);
    candidates.insert(candidates.end(), hashed.begin(), hashed.end());

    for (const std::string &candidate : candidates) {
      if (candidate.empty()) continue;
      // Some payloads only come out through the feedback-checked path and some
      // only through the plain one; which is which is a property of the file,
      // and either can report success while handing back nothing.
      ByteBuffer strict;
      if (pack->pack.ExtractPathWithStreamFeedbackStrict(candidate.c_str(), &strict) && !strict.Empty()) {
        out->assign(strict.Data(), strict.Data() + strict.Size());
        return true;
      }
      ByteBuffer plainBytes;
      if (pack->pack.ExtractPath(candidate.c_str(), &plainBytes) && !plainBytes.Empty()) {
        out->assign(plainBytes.Data(), plainBytes.Data() + plainBytes.Size());
        return true;
      }
    }
  }

  // Not everything the game reads is inside a pack. The images themselves are
  // not: an installed TrackMania keeps them as ordinary files under GameData,
  // and only the descriptors that point at them are packed. A path out of a
  // reference table is already the path on disk, once the slashes are turned
  // round.
  return ReadFromGameData(plain, out);
}

bool PackSet::ReadFromGameData(const std::string &plain_path, std::vector<unsigned char> *out) const {
  if (data_root_.empty()) return false;
  std::string relative = plain_path;
  std::replace(relative.begin(), relative.end(), '\\', '/');
  std::string full = data_root_ + "/" + relative;

  // The game was written for a filesystem that does not care about case, and it
  // shows: the same folder holds IslandBeach.dds beside IslandBoats.DDS, and a
  // reference names either spelling. So a miss is retried against the directory
  // listing, which is read once and kept.
  {
    std::error_code error;
    if (!std::filesystem::is_regular_file(full, error)) {
      const std::size_t slash = full.find_last_of('/');
      if (slash == std::string::npos) return false;
      const std::string directory = full.substr(0u, slash);
      const std::string wanted = Lowered(full.substr(slash + 1u));

      auto listing = directories_.find(directory);
      if (listing == directories_.end()) {
        std::unordered_map<std::string, std::string> entries;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
          entries.emplace(Lowered(it->path().filename().string()), it->path().string());
        listing = directories_.emplace(directory, std::move(entries)).first;
      }
      const auto found = listing->second.find(wanted);
      if (found == listing->second.end()) return false;
      full = found->second;
    }
  }

  std::ifstream file(full, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const auto size = static_cast<std::size_t>(file.tellg());
  if (size == 0u) return false;
  file.seekg(0);
  out->resize(size);
  file.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(size));
  return file.good() || file.gcount() == static_cast<std::streamsize>(size);
}

bool PackSet::References(std::string_view plain_path, GbxFile *out) const {
  if (out == nullptr) return false;
  out->references.clear();
  std::vector<unsigned char> bytes;
  if (!Read(plain_path, &bytes) || bytes.size() > UINT32_MAX) return false;

  u32 class_id = 0u;
  GbxBodyReferenceTable table;
  if (!GbxBodyOffsetReader::TryParseWithReferences(bytes.data(), static_cast<u32>(bytes.size()), &class_id, &table))
    return false;

  out->class_id = class_id;
  out->bytes = std::move(bytes);
  const std::string source(plain_path);
  for (const GbxBodyExternalReference &reference : table.externalReferences) {
    std::string resolved;
    if (!table.ResolvePlainPathForReference(source, reference, &resolved)) continue;
    // A reference with useFile clear is the file naming itself; that is how a
    // hashed pack entry says what it really is.
    out->references.push_back(GbxReference{std::move(resolved), reference.name, reference.useFile});
  }
  return true;
}

std::optional<std::uint8_t> PackSet::MaterialSurface(std::string_view plain_path) const {
  std::vector<unsigned char> bytes;
  if (!Read(plain_path, &bytes) || bytes.size() > UINT32_MAX) return std::nullopt;
  const std::optional<MaterialSurfaceDefinition> surface =
      DecodeMaterialArchive(bytes.data(), static_cast<u32>(bytes.size()));
  if (!surface || !surface->IsDefined()) return std::nullopt;
  return static_cast<std::uint8_t>(surface->MaterialId());
}

// Every material the packs name, under the name they name it by.
//
// A pack files almost everything under a hash of its path, so the file listing
// alone never says what a material is called, and the validator's material
// repository is keyed on the name, not the hash. The names are written down, but
// in the *referring* files: a solid says which materials it uses, a material
// says which others it derives from. So the vocabulary is the union of every
// material reference anywhere in the packs.
//
// Only the head of each file is read for this. A reference table sits at the
// front, and decompressing two thousand whole files to look at their first few
// hundred bytes would cost more than the rest of loading a track put together.
const std::vector<std::string> &PackSet::MaterialLogicalPaths() const {
  // Reading the head of every file in three packs takes a noticeable part of a
  // second, and the answer only changes when the packs do.
  if (material_paths_.has_value()) return *material_paths_;
  std::vector<std::string> out;
  std::unordered_map<std::string, bool> seen;
  char path[1024];
  for (const auto &pack : packs_) {
    for (const CPlugFileFidContainer_SFileDesc &file : pack->pack.files) {
      if (!pack->pack.FileDescPlainPath(&file, path, sizeof(path))) continue;
      ByteBuffer prefix;
      if (!pack->pack.ExtractReferenceTablePrefix(path, &prefix) || prefix.Empty() ||
          prefix.Size() > UINT32_MAX)
        continue;
      u32 class_id = 0u;
      GbxBodyReferenceTable table;
      if (!GbxBodyOffsetReader::TryParseWithReferences(prefix.Data(), static_cast<u32>(prefix.Size()), &class_id,
                                                       &table))
        continue;
      for (const GbxBodyExternalReference &reference : table.externalReferences) {
        // The repository only recognises this exact spelling, so a reference
        // written any other way could never be the one it holds.
        if (reference.name.find(".Material.Gbx") == std::string::npos) continue;
        std::string resolved;
        if (!table.ResolvePlainPathForReference(path, reference, &resolved)) continue;
        if (seen.emplace(resolved, true).second) out.push_back(std::move(resolved));
      }
    }
  }
  material_paths_ = std::move(out);
  return *material_paths_;
}

std::vector<std::string> PackSet::PathsOfClass(std::uint32_t class_id) const {
  std::vector<std::string> out;
  char path[1024];
  for (const auto &pack : packs_) {
    for (const CPlugFileFidContainer_SFileDesc &file : pack->pack.files) {
      if (file.classId != class_id) continue;
      if (pack->pack.FileDescPlainPath(&file, path, sizeof(path))) out.emplace_back(path);
    }
  }
  return out;
}

} // namespace tmnf
