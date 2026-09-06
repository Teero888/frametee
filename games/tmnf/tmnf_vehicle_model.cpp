// The car, as TrackMania authored it.
//
// ForeverValidator decodes the vehicle for physics only: its public API hands
// back wheel definitions and collision ellipsoids, and never touches the visual
// half of the same file. The model is there in the pack though, and the
// validator already contains everything needed to reach it: a pack reader, a
// solid-archive decoder and a scene-tree assembler, all of which it uses to
// build the track's meshes.
//
// So this file decodes that solid the way the track's own meshes are decoded,
// and reads the visual half of it where the validator's vehicle loader reads
// the collision half. It is the only place in this module that reaches past
// ForeverValidator's published headers into its internals; the submodule itself
// is untouched. That coupling is deliberate and contained here: if an upstream
// change breaks it, the car falls back to the modelled one and nothing else in
// the module notices.

#include "tmnf_internal.h"

// ForeverValidator internals. Everything below this line is private to the
// validator and carries no compatibility promise.
#include "engine/core/gm_types.h"
#include "engine/core/mw_id.h"
#include "engine/rendering/plug_material.h"
#include "engine/rendering/plug_tree.h"
#include "engine/rendering/plug_visual.h"
#include "format/pack/installed/installed_pack_key_catalog.h"
#include "format/pack/installed/plug_file_pack.h"
#include "format/pack/installed_vehicle_asset_graph.h"
#include "format/static_solid/static_scene_archive_loader.h"
#include "format/static_solid/static_scene_archive_models.h"
#include "format/static_solid/static_solid_archive_assembler.h"
#include "format/static_solid/static_solid_archive_id.h"
#include "format/pack/block_info_catalog/installed_pack_asset_repository.h"
#include "format/static_solid/static_solid_archive_definitions.h"
#include "format/archive/archive_class_ids.h"
#include "format/static_solid/static_solid_archive_graph.h"
#include "format/static_solid/static_solid_archive_graph_writer.h"
#include "format/static_solid/static_solid_descriptor_dependency_queue.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmnf {
namespace {

std::vector<std::byte> ReadWholeFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const auto size = static_cast<std::size_t>(file.tellg());
  file.seekg(0);
  std::vector<std::byte> bytes(size);
  file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
  return bytes;
}

// A tree node carries its authored name in its own id, which is how the
// validator finds the collision surfaces. The visuals are named the same way,
// so the four wheels can be told apart from the body and turned with the
// steering.
const char *TreeName(const CPlugTree &tree) {
  const CMwId &id = tree.PlugId();
  return id.IsLocalName() != 0 ? id.GetString() : nullptr;
}

// The bodywork's own livery, by the material that paints it. Every car in the
// game names it "<Car>Skin", and it is the one a livery archive supplies.
bool NamesTheLivery(const std::string &material_path) {
  std::string lower = material_path;
  for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower.find("skin") != std::string::npos;
}

// The turning parts. Names are prefixed with the detail level they belong to
// ("1FLWheel", "2FLWheel") and only the wheel itself turns: the suspension arms
// and the hub around it are anchored to the chassis and would swing out of the
// bodywork if they were turned about the wheel's own centre.
//
// Front left, front right, rear right, rear left: the order the simulation
// reports wheel contact in.
std::uint8_t PartForName(const char *name) {
  if (name == nullptr) return VEHICLE_PART_BODY;
  const std::string_view value(name);

  // Every moving piece of a corner is named for it: "1FLArmTop", "2RRWheel".
  // The detail level in front of the corner is why these are searched for
  // rather than compared against.
  static constexpr std::string_view kCorners[kVehicleCorners] = {"FL", "FR", "RR", "RL"};
  for (int corner = 0; corner < static_cast<int>(kVehicleCorners); ++corner) {
    const std::size_t at = value.find(kCorners[corner]);
    if (at == std::string_view::npos) continue;
    const std::string_view piece = value.substr(at + 2u);

    if (piece.rfind("Wheel", 0u) == 0u) return static_cast<std::uint8_t>(corner);
    // The upright and the guard over it are carried by the wheel.
    if (piece.rfind("Hub", 0u) == 0u || piece.rfind("Guard", 0u) == 0u)
      return static_cast<std::uint8_t>(VEHICLE_PART_CARRIER_FL + corner);
    // The rest are hinged to the chassis. "Dir" steers a front corner and
    // "Cardan" drives a rear one, but both swing the same way.
    if (piece.rfind("ArmTop", 0u) == 0u) return VehicleLinkPart(corner, VEHICLE_LINK_ARM_TOP);
    if (piece.rfind("ArmBot", 0u) == 0u) return VehicleLinkPart(corner, VEHICLE_LINK_ARM_BOTTOM);
    if (piece.rfind("ArmDir", 0u) == 0u || piece.rfind("Cardan", 0u) == 0u)
      return VehicleLinkPart(corner, VEHICLE_LINK_ARM_STEER);
    if (piece.rfind("Susp", 0u) == 0u) return VehicleLinkPart(corner, VEHICLE_LINK_SPRING);
    break;
  }
  return VEHICLE_PART_BODY;
}

// The vehicle is authored once per detail level, and the levels hang off the
// root as sibling subtrees named by their number, finest first. Drawing all of
// them would draw four cars inside each other.
std::optional<unsigned long> DetailLevel(const char *name) {
  if (name == nullptr || *name == '\0') return std::nullopt;
  unsigned long level = 0u;
  for (const char *c = name; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') return std::nullopt;
    level = level * 10u + static_cast<unsigned long>(*c - '0');
  }
  return level;
}

// The root's detail levels, finest first. Empty when the root is not a set of
// detail levels, in which case it is the model itself.
std::vector<CPlugTree *> DetailLevels(const CPlugTree &root) {
  std::vector<std::pair<unsigned long, CPlugTree *>> levels;
  for (unsigned long i = 0; i < root.GetChildCount(); ++i) {
    CPlugTree *child = root.GetChild(i);
    if (child == nullptr) continue;
    if (const std::optional<unsigned long> level = DetailLevel(TreeName(*child))) levels.emplace_back(*level, child);
  }
  if (levels.size() < 2u) return {};
  std::sort(levels.begin(), levels.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<CPlugTree *> ordered;
  ordered.reserve(levels.size());
  for (const auto &level : levels) ordered.push_back(level.second);
  return ordered;
}

// The surfaces the physics uses. They sit in the same tree as the visuals and
// are not meant to be seen.
bool IsCollisionOnlyName(const char *name) {
  if (name == nullptr) return false;
  const std::string_view value(name);
  return value.find("Surf") != std::string_view::npos || value.rfind("Sphere", 0u) == 0u;
}

ft_vec3 ToVec(const GmVec3 &v) { return ft_vec3{v.x, v.y, v.z}; }

ft_vec3 TransformPoint(const GmIso4 &iso, const GmVec3 &p) {
  GmVec3 out;
  out.SetMult(p, iso);
  return ToVec(out);
}

ft_vec3 TransformDirection(const GmIso4 &iso, const GmVec3 &d) {
  GmVec3 out;
  out.SetMult(d, iso.rotation);
  return ToVec(out);
}

// What a face is painted with before lighting.
//
// The car's real appearance is in its textures, and the module has no textured
// path to put them through, so there is nothing to sample. The materials do not
// help either: the validator only attaches them when a pack-wide material
// repository is installed on the load session, which it does for the map and
// not for a solid decoded on its own, so every node here comes back with a
// null material, and asking it for a surface id would be asking nothing.
//
// So the tyres are black, and the rest is left white for the caller to paint
// with the editor's colour for the world. A flat body reads better than a flat
// grey one, and it tells the ghosts apart, which is what the colour is for.
ft_color BaseColorFor(const CPlugMaterial *material, std::uint8_t part) {
  constexpr ft_color kTyre{0.09f, 0.09f, 0.10f, 1.f};
  if (material != nullptr) {
    bool known = false;
    const ft_color color = SurfaceColor(static_cast<std::uint8_t>(material->SurfaceMaterialId()), &known);
    if (known) return color;
  }
  return IsWheelPart(part) ? kTyre : ft_color{1.f, 1.f, 1.f, 1.f};
}

// The vehicle's materials, attached to the archive graph by hand.
//
// A track's materials are linked for us: the load session is given a material
// repository and the validator resolves each material node against it. A solid
// decoded on its own gets no repository (there is nowhere to hand one in), so
// nothing ever classifies its material nodes, no definitions are made, and every
// tree comes back with no material at all. That is why the car had no picture on
// it while the track around it did.
//
// The two halves of the answer are both kept. The solid's reference table names
// the materials it uses, in order; the tree links record which material node
// each part of the car draws with, in the same order. Pairing them off is what
// the validator's own linker does with the transient node graph, and the counts
// are required to agree first: an off-by-one here would not fail, it would paint
// the windscreen with the tyre.
void NameVehicleMaterials(InstalledPackAssetRepository &assets, const PackSet &packs,
                          const std::string &solid_path, StaticSolidArchiveLoadSession &archive,
                          StaticSolidArchiveId payload) {
  GbxFile solid;
  if (!packs.References(solid_path, &solid)) return;

  std::vector<std::string> material_paths;
  for (const GbxReference &reference : solid.references) {
    if (reference.name.find(".Material.") == std::string::npos) continue;
    material_paths.push_back(reference.path);
  }
  if (material_paths.empty()) return;

  // The nodes the trees actually draw with, first seen first. A vehicle names
  // its materials through its shaders rather than directly, which is the
  // fallback the tree assembler itself takes, so either kind counts.
  std::vector<CGameCtnReplayStaticSolidArchiveNodeIdentity> material_nodes;
  archive.ArchiveGraph().TreeGraph().ForEachTreeSourceLink(
      [&](const CGameCtnReplayStaticSolidArchiveTreeSourceLink &link) {
        const auto node = link.HasMaterialNode() ? link.Material() : link.Shader();
        if ((!link.HasMaterialNode() && !link.HasShaderNode()) || !node.MatchesPayload(payload)) return 1;
        for (const auto &seen : material_nodes)
          if (seen.Matches(node)) return 1;
        material_nodes.push_back(node);
        return 1;
      });
  if (material_nodes.size() != material_paths.size()) return;

  CGameCtnReplayStaticSolidArchiveGraphWriter writer(&archive.MutableArchiveGraph(), payload);
  CGameCtnReplayStaticSolidArchiveSurfaceGraph &graph = archive.MutableArchiveGraph().SurfaceGraph();
  for (std::size_t i = 0; i < material_nodes.size(); ++i) {
    std::optional<ResolvedMaterialDefinition> resolved = assets.ResolveMaterialPath(material_paths[i]);
    if (!resolved) continue;
    resolved->material.render.SetMaterialPaths(material_paths[i], std::string());
    // The node has to be declared a material as well as defined as one: the
    // assembler counts material nodes before it allocates any.
    if (!writer.AppendNode(material_nodes[i].ArchiveNode(), TMNF_CLASS_CPlugMaterial)) continue;
    CGameCtnReplayStaticSolidArchiveMaterialDefinition definition;
    definition.InstallResolved(material_nodes[i], *resolved);
    graph.AddMaterialDefinition(definition);
  }
}

struct Walker {
  VehicleModel *out = nullptr;
  const PackSet *packs = nullptr;
  TextureLibrary *textures = nullptr;
  // Visuals with no usable vertex or index stream. Reported so a silently
  // half-decoded car is distinguishable from a car that simply has few parts.
  std::uint32_t skipped = 0;

  void Visit(CPlugTree &tree, const GmIso4 &parent_iso, const CPlugMaterial *inherited, bool visible,
             std::uint8_t part) {
    GmIso4 iso;
    tree.ComposeCollisionIso(parent_iso, iso);

    const char *name = TreeName(tree);
    const bool node_visible = visible && tree.IsVisible();
    const CPlugMaterial *material = tree.Material() != nullptr ? tree.Material() : inherited;
    const std::uint8_t node_part = part == VEHICLE_PART_BODY ? PartForName(name) : part;

    if (CPlugVisual *visual = tree.Visual(); visual != nullptr && node_visible && !IsCollisionOnlyName(name)) {
      Append(*visual, iso, material, node_part);
    }

    // A visual mip node holds the same shape at several detail levels, ordered
    // nearest first and appended after the node's ordinary children. Only the
    // finest is wanted; the rest would be drawn inside it.
    const auto *mip = dynamic_cast<const CPlugTreeVisualMip *>(&tree);
    const unsigned long base_children =
        mip != nullptr ? tree.GetChildCount() - mip->LevelCount() : tree.GetChildCount();
    const unsigned long child_count = mip != nullptr && mip->LevelCount() > 0u ? base_children + 1u : base_children;

    for (unsigned long i = 0; i < child_count; ++i) {
      if (CPlugTree *child = tree.GetChild(i); child != nullptr) {
        Visit(*child, iso, material, node_visible, node_part);
      }
    }
  }

private:
  void Append(CPlugVisual &visual, const GmIso4 &iso, const CPlugMaterial *material, std::uint8_t part) {
    const unsigned long vertex_count = visual.GetTotalVertexCount();
    const std::vector<GxVertex> vertices = visual.CanonicalVertices(1, 1, 1);
    unsigned long index_count = 0u;
    unsigned short *indices = nullptr;
    visual.GetVertexIndexation(index_count, indices);
    if (vertex_count == 0u || vertices.size() != vertex_count || index_count < 3u || indices == nullptr) {
      ++skipped;
      return;
    }

    const ft_color base = BaseColorFor(material, part);
    const bool has_color = visual.HasVertexColor();
    const bool has_normal = visual.HasVertexNormal();

    // The surface's own picture, when the material could be named. Its first
    // coordinate set is the one that carries it.
    GxTexCoordSet uv;
    const bool has_uv = visual.VStreamOrClassic_GetTexCoordSet(uv, 0u, nullptr) != 0 && uv.Count() == vertex_count;
    std::uint32_t layer = kNoTextureLayer;
    if (has_uv && material != nullptr && packs != nullptr && textures != nullptr) {
      const std::string &path = material->ReplayRenderDefinition().MaterialPlainPath();
      if (!path.empty()) {
        // The car reads no opacity from its pictures: their fourth channel is
        // specular strength, and taking it as alpha is what made the body and
        // its wheels see-through.
        if (const std::optional<std::uint32_t> found = textures->Layer(*packs, path, false)) {
          layer = *found;
          // The bodywork's own livery, which a chosen skin replaces. The
          // material is named for it: every car in the game has a "<Car>Skin"
          // material and it is the only one a livery archive supplies.
          if (out->skin_layer == kNoTextureLayer && NamesTheLivery(path)) out->skin_layer = layer;
        }
      }
    }

    for (unsigned long i = 0; i + 2 < index_count; i += 3) {
      const unsigned short i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
      if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;

      VehicleFace face;
      face.a = TransformPoint(iso, vertices[i0].position);
      face.b = TransformPoint(iso, vertices[i1].position);
      face.c = TransformPoint(iso, vertices[i2].position);
      face.layer = layer;
      if (layer != kNoTextureLayer) {
        const unsigned short corner[3] = {i0, i1, i2};
        for (int k = 0; k < 3; ++k) {
          const GxTexCoord4 coord = uv.Coordinate4At(corner[k]);
          // v runs the other way in the game's meshes than in a DDS; see the
          // same turn in the track walker.
          face.uv[k] = ft_vec2{coord.u, 1.0f - coord.v};
        }
      }

      const ft_vec3 geometric = Cross(Sub(face.b, face.a), Sub(face.c, face.a));
      if (LengthSq(geometric) < 1e-14f) continue;

      // Authored normals decide which way a face points, and the winding is
      // corrected against them so back faces can be culled.
      face.normal = Normalize(geometric);
      if (has_normal) {
        const ft_vec3 authored = Add(Add(TransformDirection(iso, vertices[i0].normal),
                                         TransformDirection(iso, vertices[i1].normal)),
                                     TransformDirection(iso, vertices[i2].normal));
        if (LengthSq(authored) > 1e-12f) {
          if (Dot(geometric, authored) < 0.f) {
            std::swap(face.b, face.c);
            std::swap(face.uv[1], face.uv[2]);
            face.normal = Scale(face.normal, -1.f);
          }
          face.normal = Normalize(authored, face.normal);
        }
      }

      // On a textured panel the picture is the appearance and the colour only
      // carries the alpha; elsewhere the authored vertex colour is all there is.
      face.color = layer != kNoTextureLayer ? ft_color{1.f, 1.f, 1.f, base.a} : base;
      if (has_color && layer == kNoTextureLayer) {
        const auto &c = vertices[i0].color;
        const auto tint = [](float v) { return std::clamp(v * 2.f, 0.f, 2.f); };
        face.color = ft_color{base.r * tint(c[0]), base.g * tint(c[1]), base.b * tint(c[2]), base.a};
      }
      face.part = part;
      out->faces.push_back(face);
    }
  }
};

} // namespace

std::string VehiclePackName(fv::VehicleModel vehicle) {
  switch (vehicle) {
  case fv::VehicleModel::SnowCar: return "Alpine";
  case fv::VehicleModel::DesertCar: return "Speed";
  case fv::VehicleModel::RallyCar: return "Rally";
  case fv::VehicleModel::IslandCar: return "Island";
  case fv::VehicleModel::CoastCar: return "Coast";
  case fv::VehicleModel::BayCar: return "Bay";
  case fv::VehicleModel::StadiumCar: return "Stadium";
  case fv::VehicleModel::Unknown: break;
  }
  return {};
}

// The seven cars the game ships, by the folder each keeps its liveries in.
// These are not the pack names: the pack is the environment ("Stadium",
// "Speed") and the folder is the car ("StadiumCar", "American").
std::string VehicleSkinFolder(fv::VehicleModel vehicle) {
  switch (vehicle) {
  case fv::VehicleModel::SnowCar: return "SnowCar";
  case fv::VehicleModel::DesertCar: return "American";
  case fv::VehicleModel::RallyCar: return "Rally";
  case fv::VehicleModel::IslandCar: return "SportCar";
  case fv::VehicleModel::CoastCar: return "CoastCar";
  case fv::VehicleModel::BayCar: return "BayCar";
  case fv::VehicleModel::StadiumCar: return "StadiumCar";
  case fv::VehicleModel::Unknown: break;
  }
  return {};
}

bool LoadVehicleModel(ft_game *game, PackSet &packs, TextureLibrary &textures, const std::string &pack_name,
                      VehicleModel *out) {
  if (out == nullptr || game->packs.empty() || pack_name.empty()) return false;
  out->faces.clear();

  const std::vector<std::byte> packlist = ReadWholeFile(game->packs + "/packlist.dat");
  const std::vector<std::byte> pak = ReadWholeFile(game->packs + "/" + pack_name + ".pak");
  if (packlist.empty() || pak.empty()) {
    Log(game, FT_LOG_WARN, "Could not read %s.pak; drawing the modelled car instead.", pack_name.c_str());
    return false;
  }

  InstalledPackKeyCatalog keys;
  CPlugFilePack pack;
  if (!keys.LoadFromMemory(packlist.data(), packlist.size(), "") ||
      !pack.OpenFromMemory(pak.data(), pak.size(), keys, pack_name.c_str())) {
    Log(game, FT_LOG_WARN, "Could not open %s.pak; drawing the modelled car instead.", pack_name.c_str());
    return false;
  }

  const std::optional<InstalledVehicleAssetGraph> assets = InstalledVehicleAssetGraph::ResolveFromPack(pack);
  if (!assets.has_value() || !assets->solid.IsValid()) {
    Log(game, FT_LOG_WARN, "%s.pak has no vehicle solid reference.", pack_name.c_str());
    return false;
  }

  // The validator's own vehicle loader decodes this solid a shorter way, but
  // that path hands the decoded bytes back to its caller without keeping them in
  // the session, and the geometry is stored as offsets into exactly those bytes.
  // It works for the collision surfaces, whose shapes are in the archive graph,
  // and leaves every visual without vertices. Going through the dependency queue
  // instead is the path the track's meshes take: it retains each payload and
  // pulls in whatever the solid references.
  StaticSolidArchiveCatalog catalog;
  if (!catalog.LoadFromInstalledPack(&pack)) {
    Log(game, FT_LOG_WARN, "%s.pak has no solid catalogue.", pack_name.c_str());
    return false;
  }

  StaticSolidArchiveLoadSession archive;
  if (!archive.InstallPackSource(pack)) return false;

  CGameCtnReplayStaticSolidDescriptorDependencyQueue queue;
  CGameCtnReplayArchiveStaticModelCollection models;
  std::uint32_t missing = 0u;
  if (!queue.RequireDescriptor(assets->solid.selectedPath.c_str()) ||
      !queue.DecodeReachablePayloadGraph(&catalog, &archive, &models, &missing) || !archive.HasPayloads()) {
    Log(game, FT_LOG_WARN, "The vehicle solid in %s.pak could not be decoded.", pack_name.c_str());
    return false;
  }

  // The materials, before the trees are built from them.
  {
    InstalledPackAssetRepository repository;
    if (repository.Configure(pak.data(), pak.size(), keys, pack_name.c_str())) {
      NameVehicleMaterials(repository, packs, assets->solid.logicalPath, archive,
                           archive.SelectPayloadForDescriptor(assets->solid.selectedPath.c_str()));
    }
  }

  StaticSolidArchiveAssembler assembler;
  if (!assembler.Assemble(archive.ArchiveGraph(), archive)) return false;

  const StaticSolidArchiveId id = archive.SelectPayloadForDescriptor(assets->solid.selectedPath.c_str());
  CPlugTree *root = assembler.ModelRoot(id, std::nullopt);
  if (root == nullptr) root = assembler.CollisionRoot(id);
  if (root == nullptr) {
    Log(game, FT_LOG_WARN, "The vehicle solid in %s.pak has no model tree.", pack_name.c_str());
    return false;
  }
  assembler.ApplyReplacementMaterials(root, id);

  GmIso4 identity;
  identity.SetIdentity();
  GmIso4 root_iso;
  root->ComposeCollisionIso(identity, root_iso);

  // The finest level the module is willing to draw, per world on screen. The
  // stadium car is authored at fifty thousand triangles, sixteen thousand and
  // three and a half thousand; the track around it is a quarter of a million
  // against a budget of kTriangleBudget, so the finest car is a small share of
  // the frame and the one the camera sits close enough to see. Lower this to
  // trade the car's panel gaps back for the triangles, which is worth doing
  // when several prediction ghosts are on screen at once, since each draws its
  // own.
  constexpr std::size_t kBudget = 60000;
  const std::vector<CPlugTree *> levels = DetailLevels(*root);
  std::size_t chosen = 0;
  std::uint32_t skipped = 0;
  for (; chosen < std::max<std::size_t>(levels.size(), 1u); ++chosen) {
    out->faces.clear();
    Walker walker;
    walker.out = out;
    walker.packs = &packs;
    walker.textures = &textures;
    if (levels.empty()) {
      walker.Visit(*root, identity, nullptr, true, VEHICLE_PART_BODY);
    } else {
      walker.Visit(*levels[chosen], root_iso, root->Material(), root->IsVisible(), VEHICLE_PART_BODY);
    }
    skipped = walker.skipped;
    // Some older packs keep their finest authored tree disabled and make the
    // next detail level the first visible one. An empty level is therefore not
    // a cheap model to accept; keep looking until one actually has geometry.
    Log(game, FT_LOG_TRACE, "car detail level %zu: %zu triangles", chosen + 1u, out->faces.size());
    if (!out->faces.empty() && (out->faces.size() <= kBudget || chosen + 1u >= levels.size())) break;
  }

  if (out->faces.empty()) {
    Log(game, FT_LOG_WARN, "The vehicle model in %s.pak decoded to no geometry.", pack_name.c_str());
    return false;
  }

  // The wheels are drawn in their own space so they can be turned, so each one
  // needs the hub it turns about.
  std::size_t wheel_faces = 0;
  for (std::uint8_t part = VEHICLE_PART_WHEEL_FL; part <= VEHICLE_PART_WHEEL_RL; ++part) {  // wheels only
    Aabb box;
    for (const VehicleFace &face : out->faces) {
      if (face.part != part) continue;
      box.Add(face.a);
      box.Add(face.b);
      box.Add(face.c);
      ++wheel_faces;
    }
    if (!box.Valid()) continue;
    const ft_vec3 hub = box.Center();
    out->hub[part] = hub;
    for (VehicleFace &face : out->faces) {
      if (face.part != part) continue;
      face.a = Sub(face.a, hub);
      face.b = Sub(face.b, hub);
      face.c = Sub(face.c, hub);
    }
  }

  // Where each hinged link is bolted to the chassis. A suspension arm runs
  // outboard from the car's centre line, so its inboard end is whichever end
  // sits nearer that line, and the span between the two is what turns the
  // wheel's travel into an angle. Reach is signed: it points the way the wheel
  // end lies, which is what keeps left and right swinging opposite ways.
  for (std::uint8_t part = VEHICLE_PART_LINK_FIRST; part < VEHICLE_PART_BODY; ++part) {
    Aabb box;
    for (const VehicleFace &face : out->faces) {
      if (face.part != part) continue;
      box.Add(face.a);
      box.Add(face.b);
      box.Add(face.c);
    }
    if (!box.Valid()) continue;
    const bool outboard_is_positive = std::fabs(box.mx.x) >= std::fabs(box.mn.x);
    const float inboard_x = outboard_is_positive ? box.mn.x : box.mx.x;
    const float outboard_x = outboard_is_positive ? box.mx.x : box.mn.x;
    const ft_vec3 centre = box.Center();
    out->pivot[part] = ft_vec3{inboard_x, centre.y, centre.z};
    out->reach[part] = outboard_x - inboard_x;
  }

  // How much of the car found its own picture. A body drawn in flat white is
  // what this looks like when it is zero, and that is a material-naming problem
  // rather than a rendering one, so it is worth saying which.
  std::size_t painted = 0u;
  for (const VehicleFace &face : out->faces)
    if (face.layer != kNoTextureLayer) ++painted;

  Log(game, FT_LOG_INFO,
      "Loaded the %s car model at detail level %zu of %zu: %zu triangles (%zu on the wheels, %zu textured).",
      pack_name.c_str(), chosen + 1u, std::max<std::size_t>(levels.size(), 1u), out->faces.size(), wheel_faces,
      painted);
  if (skipped != 0u) Log(game, FT_LOG_TRACE, "Skipped %u vehicle parts with no readable geometry.", skipped);
  return true;
}

} // namespace tmnf
