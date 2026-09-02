// Reading which picture a material binds to which sampler.
//
// A TrackMania material names several textures and only one of them is the
// surface's own appearance: beside it sit a normal map, a specular ramp, a
// baked occlusion pass, an environment cube. Which is which is not a property
// of the file names (plenty of them are called things like StadiumWarpO), it
// is the *sampler* each one is bound to. "Diffuse" is the picture. So the
// material has to be read rather than guessed at, and this reads it.
//
// The traversal mirrors ForeverValidator's own material archive decoder, which
// walks the same chunks to find a surface id and throws the rest away. Nothing
// there is patched: this is a second pass over the same bytes that keeps what
// the first one discards. The two therefore have to agree about the format
// exactly, and where the shape of a chunk looks arbitrary here it is because it
// is arbitrary there; see
// ForeverValidator/src/format/materials/material_archive_decoder.cpp.
//
// The roles are TrackMania's own vocabulary; the order they are preferred in
// follows GbxTools3D, a viewer for these same environments:
// https://github.com/BigBang1112/gbx-tools-3d

#include "tmnf_internal.h"

#include "format/archive/archive_class_ids.h"
#include "format/archive/archive_node_reference.h"
#include "format/archive/mw_id_archive_codec.h"
#include "format/archive/tmnf_archive_ids.h"
#include "format/archive/tmnf_gbx_body_reader.h"
#include "format/materials/material_archive_schema.h"
#include "format/static_solid/static_solid_archive_shader_chunk_ids.h"

#include <algorithm>
#include <cstring>

namespace tmnf {
namespace {

constexpr u32 kChunkIntArray = 0x0903a004u;
constexpr u32 kChunkBitmaps = 0x0903a006u;
constexpr u32 kChunkGpuFx = 0x0903a00au;
constexpr u32 kChunkBitmapEnable = 0x0903a00cu;
constexpr u32 kChunkFlags = 0x0903a00du;
constexpr u32 kChunkFloats = 0x0903a00fu;
constexpr u32 kChunkLegacySkip = 0x0903a011u;
constexpr u32 kMaxArrayCount = 0x100000u;
constexpr u32 kMaxStringBytes = 0x10000u;

u32 ReadWordAt(const unsigned char *bytes) {
  return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8u) | (static_cast<u32>(bytes[2]) << 16u) |
         (static_cast<u32>(bytes[3]) << 24u);
}

enum class IdEncoding : u32 { Unknown = 0u, TextTagged = 1u, InlineNames = 2u, SharedNames = 3u };

class Cursor;

// A material archive writes identifiers in one of three ways and only says
// which the first time one appears. Shared names are written once and referred
// to by index afterwards, so reading any of them means keeping all of them.
class IdReader {
public:
  bool Read(Cursor &cursor, std::string *out);

private:
  IdEncoding mode_ = IdEncoding::Unknown;
  std::vector<std::string> shared_;
};

class Cursor {
public:
  Cursor(const unsigned char *bytes, u32 byte_count, const GbxBodyReferenceTable &references)
      : bytes_(bytes), byte_count_(byte_count), offset_(references.bodyOffset), references_(references) {
    internal_seen_.assign(static_cast<std::size_t>(references.nodeCount) + 1u, 0u);
  }

  u32 Remaining() const { return offset_ <= byte_count_ ? byte_count_ - offset_ : 0u; }
  u32 PeekWord() const { return Remaining() >= 4u ? ReadWordAt(bytes_ + offset_) : 0u; }

  bool ReadWord(u32 &value) {
    if (Remaining() < 4u) return false;
    value = ReadWordAt(bytes_ + offset_);
    offset_ += 4u;
    return true;
  }
  bool ReadByte(unsigned char &value) {
    if (Remaining() == 0u) return false;
    value = bytes_[offset_++];
    return true;
  }
  bool SkipWord() {
    u32 ignored = 0u;
    return ReadWord(ignored);
  }
  bool SkipBytes(u32 count) {
    if (count > Remaining()) return false;
    offset_ += count;
    return true;
  }
  bool ReadString(std::string *out) {
    u32 count = 0u;
    if (!ReadWord(count) || count > kMaxStringBytes || count > Remaining()) return false;
    if (out != nullptr) out->assign(reinterpret_cast<const char *>(bytes_ + offset_), count);
    offset_ += count;
    return true;
  }

  const GbxBodyExternalReference *ExternalReference(u32 node_index) const {
    for (const GbxBodyExternalReference &reference : references_.externalReferences)
      if (reference.nodeIndex == node_index) return &reference;
    return nullptr;
  }

  // The node reference itself, and, when it points outside the file, which
  // file it points at, which is the whole reason this pass exists.
  bool ReadNodeReference(const GbxBodyExternalReference **external = nullptr, bool *is_null = nullptr);
  bool ReadFidReference(const GbxBodyExternalReference **external = nullptr);
  bool SkipDeviceSets(u32 chunk);
  bool SkipSizedBlock();
  bool ReadShaderApply();
  bool ReadBitmapApply(const GbxBodyExternalReference **texture);
  bool ReadMaterialCustom();

  std::vector<MaterialTextureSlot> slots;
  // A material that is only a shader wrapper names the shader it defers to.
  std::vector<const GbxBodyExternalReference *> shader_files;

private:
  const unsigned char *bytes_ = nullptr;
  u32 byte_count_ = 0u;
  u32 offset_ = 0u;
  const GbxBodyReferenceTable &references_;
  std::vector<unsigned char> internal_seen_;
};

bool IdReader::Read(Cursor &cursor, std::string *out) {
  if (out != nullptr) out->clear();
  u32 word = 0u;
  if (!cursor.ReadWord(word)) return false;
  if (mode_ == IdEncoding::Unknown && word >= 1u && word <= 3u) {
    mode_ = static_cast<IdEncoding>(word);
    if (!cursor.ReadWord(word)) return false;
  }
  if (mode_ == IdEncoding::TextTagged) {
    if (word == 0u) return true;
    return word <= 3u && cursor.ReadString(out);
  }

  const TmnfFormat::ArchiveIdentifierWord id = TmnfFormat::CMwIdArchiveCodec::ParseWord(word);
  if (!id.IsNamed()) return true;
  if (mode_ == IdEncoding::SharedNames && id.payload != 0u) {
    if (id.payload > shared_.size()) return false;
    if (out != nullptr) *out = shared_[id.payload - 1u];
    return true;
  }
  if (mode_ == IdEncoding::Unknown && id.payload != 0u) return false;

  std::string name;
  if (!cursor.ReadString(&name)) return false;
  if (mode_ == IdEncoding::SharedNames) {
    if (shared_.size() >= kMaxArrayCount) return false;
    shared_.push_back(name);
  }
  if (out != nullptr) *out = std::move(name);
  return true;
}

bool Cursor::ReadNodeReference(const GbxBodyExternalReference **external, bool *is_null) {
  if (external != nullptr) *external = nullptr;
  u32 node_index = 0u;
  if (!ReadWord(node_index)) return false;
  const bool null_reference = node_index == ArchiveNodeReference::InvalidIndex;
  if (is_null != nullptr) *is_null = null_reference;
  if (null_reference) return true;
  if (node_index == 0u || node_index > references_.nodeCount) return false;

  if (const GbxBodyExternalReference *reference = ExternalReference(node_index)) {
    if (external != nullptr) *external = reference;
    return true;
  }
  if (node_index >= internal_seen_.size()) return false;
  // A node written once and referred to again carries no second body.
  if (internal_seen_[node_index] != 0u) return true;

  u32 class_id = 0u;
  if (!ReadWord(class_id)) return false;
  internal_seen_[node_index] = 1u;
  switch (class_id) {
  case TMNF_CLASS_CPlugMaterialCustom: return ReadMaterialCustom();
  case TMNF_CLASS_CPlugShaderApply: return ReadShaderApply();
  case TMNF_CLASS_CPlugBitmapApply: return ReadBitmapApply(nullptr);
  default: return false;
  }
}

bool Cursor::ReadFidReference(const GbxBodyExternalReference **external) {
  if (external != nullptr) *external = nullptr;
  u32 node_index = 0u;
  if (!ReadWord(node_index)) return false;
  if (node_index == ArchiveNodeReference::InvalidIndex) return true;
  if (node_index == 0u || node_index == ArchiveNodeReference::DeferredIndex || node_index > references_.nodeCount)
    return false;
  const GbxBodyExternalReference *reference = ExternalReference(node_index);
  if (reference == nullptr) return false;
  if (external != nullptr) *external = reference;
  return true;
}

bool Cursor::SkipSizedBlock() {
  u32 marker = 0u, size = 0u;
  if (!ReadWord(marker) || marker != MaterialArchiveSkipBlockMarker || !ReadWord(size) || size > Remaining())
    return false;
  offset_ += size;
  return true;
}

bool Cursor::ReadShaderApply() {
  for (;;) {
    u32 chunk = 0u;
    if (!ReadWord(chunk)) return false;
    if (chunk == CMwNodArchiveFacadeSentinel) return true;
    switch (chunk) {
    case ArchiveChunkIdValue(CPlugShaderArchiveChunkId::LegacyCustomShader): {
      u32 count = 0u;
      if (!ReadNodeReference()) return false;
      u32 array_count = 0u;
      if (!ReadWord(array_count) || array_count > Remaining() / 4u) return false;
      for (u32 i = 0; i < array_count; ++i)
        if (!ReadNodeReference()) return false;
      if (!ReadNodeReference() || !ReadWord(count) || count > Remaining() / 4u) return false;
      for (u32 i = 0; i < count; ++i)
        if (!ReadNodeReference()) return false;
      break;
    }
    case ArchiveChunkIdValue(CPlugShaderArchiveChunkId::RequirementsWithNat16):
      if (!SkipBytes(12u) || !ReadNodeReference() || !SkipBytes(2u)) return false;
      break;
    case ArchiveChunkIdValue(CPlugShaderGenericArchiveChunkId::Material):
      if (!SkipBytes(0x58u)) return false;
      break;
    case ArchiveChunkIdValue(CPlugShaderApplyArchiveChunkId::TextureApplyRefs): {
      // The shader's own bitmap bindings. This is the route a vehicle's
      // materials take: they carry no custom material of their own and name
      // their picture only through the shader they apply.
      u32 count = 0u;
      if (!ReadWord(count) || count > Remaining() / 4u) return false;
      for (u32 i = 0; i < count; ++i) {
        const GbxBodyExternalReference *texture = nullptr;
        if (!ReadNodeReference(&texture)) return false;
        if (texture != nullptr) slots.push_back(MaterialTextureSlot{std::string(), texture->name, texture->nodeIndex});
      }
      break;
    }
    case ArchiveChunkIdValue(CPlugShaderApplyArchiveChunkId::ApplyField):
      if (!SkipBytes(4u)) return false;
      break;
    case ArchiveChunkIdValue(CPlugShaderApplyArchiveChunkId::ApplyFields):
      if (!SkipBytes(8u)) return false;
      break;
    default: return false;
    }
  }
}

bool Cursor::ReadBitmapApply(const GbxBodyExternalReference **texture) {
  IdReader ids;
  u32 chunk = 0u;
  unsigned char has_transform = 0u;
  if (!ReadWord(chunk) || chunk != ArchiveChunkIdValue(CPlugBitmapSamplerArchiveChunkId::Bitmap)) return false;
  if (!ids.Read(*this, nullptr)) return false;
  // The bitmap itself, which is the file the sampler is pointed at.
  if (!ReadNodeReference(texture)) return false;
  if (!SkipBytes(8u)) return false;
  if (!ReadWord(chunk) || chunk != ArchiveChunkIdValue(CPlugBitmapAddressArchiveChunkId::Transform)) return false;
  if (!SkipBytes(4u) || !ReadNodeReference() || !ReadByte(has_transform) || has_transform > 1u) return false;
  if (has_transform != 0u && !SkipBytes(24u)) return false;
  if (!ReadWord(chunk) || chunk != ArchiveChunkIdValue(CPlugBitmapAddressArchiveChunkId::BumpEnvScale)) return false;
  if (!SkipBytes(4u)) return false;
  if (!ReadWord(chunk) || chunk != ArchiveChunkIdValue(CPlugBitmapApplyArchiveChunkId::ApplyFlags)) return false;
  if (!SkipBytes(4u)) return false;
  return ReadWord(chunk) && chunk == CMwNodArchiveFacadeSentinel;
}

bool Cursor::ReadMaterialCustom() {
  IdReader ids;
  for (;;) {
    u32 chunk = 0u;
    if (!ReadWord(chunk)) return false;
    if (chunk == CMwNodArchiveFacadeSentinel) return true;
    switch (chunk) {
    case kChunkIntArray: {
      u32 count = 0u;
      if (!ReadWord(count) || count > kMaxArrayCount || count > Remaining() / 4u) return false;
      for (u32 i = 0; i < count; ++i)
        if (!ids.Read(*this, nullptr) || !SkipWord()) return false;
      break;
    }
    case kChunkBitmaps: {
      // What this whole file is for: a sampler name and the bitmap bound to it.
      u32 count = 0u;
      if (!ReadWord(count) || count > kMaxArrayCount) return false;
      for (u32 i = 0; i < count; ++i) {
        std::string sampler;
        if (!ids.Read(*this, &sampler) || !SkipWord()) return false;
        const GbxBodyExternalReference *texture = nullptr;
        if (!ReadNodeReference(&texture)) return false;
        if (texture != nullptr)
          slots.push_back(MaterialTextureSlot{std::move(sampler), texture->name, texture->nodeIndex});
      }
      break;
    }
    case kChunkGpuFx: {
      u32 count = 0u;
      if (!ReadWord(count) || count > kMaxArrayCount) return false;
      for (u32 i = 0; i < count; ++i) {
        u32 components = 0u, registers = 0u, enabled = 0u;
        if (!ids.Read(*this, nullptr) || !ReadWord(components) || !ReadWord(registers) || !ReadWord(enabled) ||
            enabled > 1u || components > kMaxArrayCount || registers > kMaxArrayCount ||
            (components != 0u && registers > 0xFFFFFFFFu / components) || components * registers > 0xFFFFFFFFu / 4u ||
            !SkipBytes(components * registers * 4u))
          return false;
      }
      break;
    }
    case kChunkBitmapEnable: {
      u32 count = 0u;
      if (!ReadWord(count) || count > kMaxArrayCount) return false;
      for (u32 i = 0; i < count; ++i) {
        u32 enabled = 0u;
        if (!ids.Read(*this, nullptr) || !ReadWord(enabled) || enabled > 1u) return false;
      }
      break;
    }
    case kChunkFlags: {
      u32 flags = 0u;
      if (!ReadWord(flags) || !SkipBytes(12u) || ((flags & 1u) != 0u && !SkipBytes(4u))) return false;
      break;
    }
    case kChunkFloats:
      if (!SkipSizedBlock()) return false;
      break;
    case kChunkLegacySkip: {
      u32 marker = 0u, byte_count = 0u;
      if (!ReadWord(marker) || marker != MaterialArchiveSkipBlockMarker || !ReadWord(byte_count) || byte_count != 4u ||
          !SkipBytes(byte_count))
        return false;
      break;
    }
    default: return false;
    }
  }
}

bool Cursor::SkipDeviceSets(u32 chunk) {
  u32 count = 0u;
  if (!ReadWord(count) || count > 0x10000000u) return false;
  for (u32 i = 0; i < count; ++i) {
    u32 shader_is_fid = 0u;
    if (!SkipWord() || !ReadWord(shader_is_fid) || shader_is_fid > 1u) return false;
    if (shader_is_fid != 0u) {
      const GbxBodyExternalReference *shader = nullptr;
      if (!ReadFidReference(&shader)) return false;
      if (shader != nullptr) shader_files.push_back(shader);
    } else if (!ReadNodeReference()) {
      return false;
    }
    if (MaterialDeviceSetHasShaderRefs(chunk) && (!ReadFidReference() || !ReadFidReference())) return false;
  }
  return !MaterialDeviceSetHasFormats(chunk) || [&] {
    u32 formats = 0u;
    if (!ReadWord(formats) || formats > Remaining() / 4u) return false;
    return SkipBytes(formats * 4u);
  }();
}

// The order a surface's own picture is looked for in. A material binds several
// samplers and any of them may be missing, so this is a preference rather than
// a lookup: the first one present wins.
//
// "Grass" is deliberately not in here. It is not a ground picture; it is the
// close-up blade sheet a terrain material binds beside its diffuse, and because
// it sorted ahead of GDiffuse/PxzDiffuse/BaseColor it was winning on stadium
// terrain and tiling blades across the whole field instead of the ground.
const char *const kDiffuseSamplers[] = {
    "Diffuse", "Blend1",   "Panorama",   "Advert",    "Glow",      "Soil",
    "Grass",   "Foam 1",   "GDiffuse",   "PxzDiffuse", "PyDiffuse", "BaseColor",
    "PxzBaseColor",
};

} // namespace

bool ReadMaterialTextures(const PackSet &packs, const std::string &material_path,
                          std::vector<MaterialTextureSlot> *out) {
  if (out == nullptr) return false;
  out->clear();

  std::vector<unsigned char> bytes;
  if (!packs.Read(material_path, &bytes) || bytes.empty() || bytes.size() > 0xFFFFFFFFu) return false;

  u32 class_id = 0u;
  GbxBodyReferenceTable references;
  if (!GbxBodyOffsetReader::TryParseWithReferences(bytes.data(), static_cast<u32>(bytes.size()), &class_id,
                                                   &references) ||
      class_id != TMNF_CLASS_CPlugMaterial) {
    return false;
  }

  Cursor cursor(bytes.data(), static_cast<u32>(bytes.size()), references);
  bool complete = false;
  for (;;) {
    u32 chunk = 0u;
    if (!cursor.ReadWord(chunk)) break;
    if (chunk == CMwNodArchiveFacadeSentinel) {
      complete = true;
      break;
    }
    if (IsMaterialNodeReferenceChunk(chunk)) {
      if (!cursor.ReadNodeReference()) break;
      continue;
    }
    if (IsMaterialDeviceSetChunk(chunk)) {
      bool model_is_null = false;
      if (!cursor.ReadNodeReference(nullptr, &model_is_null)) break;
      if (model_is_null && !cursor.SkipDeviceSets(chunk)) break;
      continue;
    }
    if (IsMaterialSurfaceFlagsChunk(chunk)) {
      if (!cursor.SkipWord()) break;
      continue;
    }
    if (chunk == MaterialArchiveChunkValue(MaterialArchiveChunk::FourByteLegacyPayload)) {
      if (!cursor.SkipWord()) break;
      continue;
    }
    if (cursor.Remaining() >= 8u && cursor.PeekWord() == MaterialArchiveSkipBlockMarker) {
      if (!cursor.SkipSizedBlock()) break;
      continue;
    }
    break;
  }

  // A partial read is still worth having: the bitmap chunk comes early and a
  // chunk further on that this does not recognise costs the shader list, not
  // the pictures.
  for (MaterialTextureSlot &slot : cursor.slots) {
    for (const GbxBodyExternalReference &reference : references.externalReferences) {
      if (reference.nodeIndex != slot.node_index) continue;
      std::string resolved;
      if (references.ResolvePlainPathForReference(material_path, reference, &resolved)) slot.path = std::move(resolved);
      break;
    }
  }
  cursor.slots.erase(std::remove_if(cursor.slots.begin(), cursor.slots.end(),
                                    [](const MaterialTextureSlot &slot) { return slot.path.empty(); }),
                     cursor.slots.end());

  // A material that only wraps a shader has its pictures over there. The car's
  // do exactly this.
  if (cursor.slots.empty()) {
    for (const GbxBodyExternalReference *shader : cursor.shader_files) {
      std::string resolved;
      if (!references.ResolvePlainPathForReference(material_path, *shader, &resolved)) continue;
      if (resolved == material_path) continue;
      std::vector<MaterialTextureSlot> nested;
      if (ReadMaterialTextures(packs, resolved, &nested) && !nested.empty()) {
        *out = std::move(nested);
        return true;
      }
    }
  }

  *out = std::move(cursor.slots);
  return complete || !out->empty();
}

std::optional<std::string> DiffuseTextureOf(const std::vector<MaterialTextureSlot> &slots) {
  for (const char *wanted : kDiffuseSamplers) {
    for (const MaterialTextureSlot &slot : slots) {
      if (slot.sampler == wanted) return slot.path;
    }
  }
  return std::nullopt;
}

} // namespace tmnf
