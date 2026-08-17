// The track's visual scene, decoded from the installed game.
//
// The sandbox builds a render scene of its own and it is very nearly enough:
// real meshes, real instances, real texture coordinates. What it cannot carry is
// which picture goes on a surface — and on several environments it cannot even
// carry which material a surface uses, reporting a whole Island track as one
// nameless material.
//
// That is not an oversight in the sandbox. A material belongs to the tile, not
// to the map: each block's solid names the materials it draws with, and the
// validator links them only when the solid declares a material node. A great
// many tiles declare a shader node instead, and those the validator leaves
// unresolved, because a physics reconstruction has no use for either.
//
// So the track is loaded a second time here, through the same asset repository,
// and two things are put back before the scene is assembled: each material that
// did resolve is written back with the file path it came from, and each one that
// did not is paired up from the tile's own reference table. From those paths the
// textures follow (see tmnf_texture.cpp).
//
// What comes out is the whole scene — meshes, placements and materials — and it
// is what the module draws. Loading the map twice is the price; it buys the
// actual appearance of the game, which nothing else on offer does.

#include "tmnf_internal.h"

// ForeverValidator internals; see tmnf_vehicle_model.cpp for why this module
// reads them and what that costs.
#include "engine/game/material_render_definition.h"
#include "engine/rendering/plug_material.h"
#include "engine/scene/plug_solid.h"
#include "engine/rendering/plug_tree.h"
#include "engine/rendering/plug_visual.h"
#include "engine/scene/replay_scene_placements.h"
#include "engine/scene/static_scene_model.h"
#include "format/pack/block_info_catalog/installed_pack_asset_repository.h"
#include "format/pack/installed/installed_pack_key_catalog.h"
#include "format/archive/archive_class_ids.h"
#include "format/replay/replay_file.h"
#include "format/static_solid/static_scene_archive_loader.h"
#include "format/static_solid/static_scene_asset_linker.h"
#include "format/static_solid/static_solid_archive_definitions.h"
#include "format/static_solid/static_solid_archive_graph.h"
#include "format/static_solid/static_solid_archive_graph_writer.h"
#include "simulation/replay/replay_challenge_construction.h"
#include "simulation/replay/replay_challenge_factory.h"
#include "simulation/replay/replay_scene_definition.h"
#include "simulation/replay/replay_scene_definition_factory.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

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

// One material reference, resolved to a definition and to the string that
// actually resolved it.
//
// A reference is written relative to whatever file made it, so it may arrive as
// "Island\\Material\\IslandGrass.Material.Gbx" while the repository keys the same
// material under "Island\\Media\\Material\\IslandGrass.Material.Gbx". Both are
// tried, and the one that worked is what gets recorded — the other names nothing
// and a texture lookup following it would find nothing.
std::optional<std::pair<ResolvedMaterialDefinition, std::string>> ResolveMaterialReference(
    InstalledPackAssetRepository &assets, const std::string &pack_name, const std::string &reference) {
  if (std::optional<ResolvedMaterialDefinition> resolved = assets.ResolveMaterialPath(reference))
    return std::make_pair(std::move(*resolved), reference);

  const std::size_t slash = reference.find_last_of('\\');
  if (slash == std::string::npos) return std::nullopt;
  const std::string name = reference.substr(slash + 1u);
  if (std::optional<ResolvedMaterialDefinition> resolved = assets.ResolveMaterial(name)) {
    std::string under_pack;
    under_pack.reserve(pack_name.size() + name.size() + 17u);
    under_pack.append(pack_name).append("\\Media\\Material\\").append(name);
    return std::make_pair(std::move(*resolved), std::move(under_pack));
  }
  return std::nullopt;
}

// Every material in the packs, resolved through the repository before the track
// is, and remembered by the handle the repository gave it.
//
// The graph names a material only by that handle — an index into the order the
// repository happened to resolve things in — and the repository keeps the path
// behind it to itself. Resolving the whole vocabulary up front turns the index
// back into a path, and does it exhaustively: after this, every material the
// track load asks for is already cached, so every handle the graph ends up
// holding is one of these. Doing it afterwards instead leaves the materials the
// load reached by some spelling this never tried unaccounted for, and they come
// out untextured.
std::unordered_map<std::uint32_t, std::string> ResolveMaterialVocabulary(InstalledPackAssetRepository &assets,
                                                                        const PackSet &packs,
                                                                        const std::string &pack_name) {
  std::unordered_map<std::uint32_t, std::string> by_index;
  const auto remember = [&](const std::optional<ResolvedMaterialDefinition> &resolved, const std::string &path) {
    if (resolved && resolved->material.asset.IsValid())
      by_index.emplace(resolved->material.asset.RepositoryIndex(), path);
  };
  for (const std::string &reference : packs.MaterialLogicalPaths()) {
    // Both spellings, because either may be the one a tile wrote and either may
    // be the one the load ends up holding a handle to.
    remember(assets.ResolveMaterialPath(reference), reference);
    if (auto resolved = ResolveMaterialReference(assets, pack_name, reference))
      remember(std::optional<ResolvedMaterialDefinition>(resolved->first), resolved->second);
  }
  return by_index;
}

// Writes each material's own file path into the archive graph, so that the
// materials the trees are built from carry it.
//
// The definitions are rewritten in place. Nothing else about them changes: the
// surface, the remaps and the identity are carried across untouched, and the
// only addition is a string the validator had no use for.
void NameMaterials(const std::unordered_map<std::uint32_t, std::string> &by_index,
                   StaticSolidArchiveLoadSession &session) {
  if (by_index.empty()) return;

  std::vector<CGameCtnReplayStaticSolidArchiveMaterialDefinition> definitions;
  session.ArchiveGraph().SurfaceGraph().ForEachMaterialDefinition(
      [&](const CGameCtnReplayStaticSolidArchiveMaterialDefinition &definition) {
        definitions.push_back(definition);
        return 1;
      });
  if (definitions.empty()) return;

  CGameCtnReplayStaticSolidArchiveSurfaceGraph &graph = session.MutableArchiveGraph().SurfaceGraph();
  graph.TruncateMaterialDefinitions(0u);
  for (const CGameCtnReplayStaticSolidArchiveMaterialDefinition &source : definitions) {
    ResolvedMaterialDefinition resolved;
    resolved.material.asset = source.Asset();
    resolved.material.surface = source.Surface();
    resolved.material.render = source.Render();
    resolved.remaps = source.Remaps();
    if (source.Asset().IsValid()) {
      const auto found = by_index.find(source.Asset().RepositoryIndex());
      // The selected path is the pack's own routing and this module never wants
      // it; writing a wrong one would be worse than writing none.
      if (found != by_index.end()) resolved.material.render.SetMaterialPaths(found->second, std::string());
    }
    CGameCtnReplayStaticSolidArchiveMaterialDefinition named;
    named.InstallResolved(source.Material(), resolved);
    graph.AddMaterialDefinition(named);
  }
}

// The materials a tile carries itself, attached to the archive graph by hand.
//
// A material lives in the tile's own solid, not in the map, and the tile names
// it in its reference table. The validator links those for us when the solid
// declares a material node — and does nothing when the solid declares a shader
// node instead, which is how a great many tiles are authored. Those tiles come
// out of the assembler with no material at all, and there is then nothing to
// say what they should look like: one stadium track loses its platform this
// way, and a whole Island track loses everything.
//
// So the pairing is made here, per tile. The reference table names the
// materials in order and the tree links say which node each part draws with;
// both come off the same list. The counts have to agree before any of it is
// used, and a node that the validator already resolved is left alone.
void NameMaterialsFromSolids(InstalledPackAssetRepository &assets, const PackSet &packs,
                             const std::string &pack_name, StaticSolidArchiveLoadSession &session) {
  std::vector<CGameCtnReplayStaticSolidArchiveNodeIdentity> already;
  session.ArchiveGraph().SurfaceGraph().ForEachMaterialDefinition(
      [&](const CGameCtnReplayStaticSolidArchiveMaterialDefinition &definition) {
        already.push_back(definition.Material());
        return 1;
      });
  const auto defined = [&](CGameCtnReplayStaticSolidArchiveNodeIdentity node) {
    for (const auto &seen : already)
      if (seen.Matches(node)) return true;
    return false;
  };

  std::vector<std::pair<StaticSolidArchiveId, std::string>> payloads;
  session.ForEachPayload([&](StaticSolidArchiveId id, const StaticSolidArchivePayload &payload) {
    const char *path = payload.PlainPackPath();
    if (path != nullptr && *path != '\0') payloads.emplace_back(id, path);
    return 1;
  });

  for (const auto &entry : payloads) {
    const StaticSolidArchiveId id = entry.first;
    const std::string &solid_path = entry.second;
    GbxFile solid;
    if (!packs.References(solid_path, &solid)) continue;

    std::vector<std::string> material_paths;
    for (const GbxReference &reference : solid.references) {
      if (reference.name.find(".Material.") == std::string::npos) continue;
      material_paths.push_back(reference.path);
    }
    if (material_paths.empty()) continue;

    // A tile names its materials through its shaders as often as directly, and
    // the tree assembler takes either, so either counts here too.
    std::vector<CGameCtnReplayStaticSolidArchiveNodeIdentity> nodes;
    session.ArchiveGraph().TreeGraph().ForEachTreeSourceLink(
        [&](const CGameCtnReplayStaticSolidArchiveTreeSourceLink &link) {
          if (!link.HasMaterialOrShader()) return 1;
          const auto node = link.HasMaterialNode() ? link.Material() : link.Shader();
          if (!node.MatchesPayload(id) || defined(node)) return 1;
          for (const auto &seen : nodes)
            if (seen.Matches(node)) return 1;
          nodes.push_back(node);
          return 1;
        });
    if (nodes.size() != material_paths.size()) continue;

    CGameCtnReplayStaticSolidArchiveGraphWriter writer(&session.MutableArchiveGraph(), id);
    CGameCtnReplayStaticSolidArchiveSurfaceGraph &graph = session.MutableArchiveGraph().SurfaceGraph();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      auto resolved = ResolveMaterialReference(assets, pack_name, material_paths[i]);
      if (!resolved) continue;
      resolved->first.material.render.SetMaterialPaths(resolved->second, std::string());
      // The node has to be declared a material as well as defined as one: the
      // assembler counts material nodes before it allocates any.
      if (!writer.AppendNode(nodes[i].ArchiveNode(), TMNF_CLASS_CPlugMaterial)) continue;
      CGameCtnReplayStaticSolidArchiveMaterialDefinition definition;
      definition.InstallResolved(nodes[i], resolved->first);
      graph.AddMaterialDefinition(definition);
    }
  }
}

ft_vec3 ToVec(const GmVec3 &v) { return ft_vec3{v.x, v.y, v.z}; }

TrackTransform ToTransform(const GmIso4 &iso) {
  TrackTransform out;
  out.basis_x = ToVec(iso.rotation.basisX);
  out.basis_y = ToVec(iso.rotation.basisY);
  out.basis_z = ToVec(iso.rotation.basisZ);
  out.translation = ToVec(iso.translation);
  return out;
}

TrackPurpose ToPurpose(StaticScenePurpose purpose) {
  switch (purpose) {
  case StaticScenePurpose::PlacedBlock: return TRACK_PURPOSE_BLOCK;
  case StaticScenePurpose::Clip:
  case StaticScenePurpose::Helper:
  case StaticScenePurpose::CheckpointTrigger:
  case StaticScenePurpose::DedicatedInitialCollision: return TRACK_PURPOSE_HIDDEN;
  default: return TRACK_PURPOSE_SCENERY;
  }
}

// Walks a scene model's tree and collects what it draws.
//
// A mesh is kept once per visual and an instance records where it was placed, so
// a block laid down fifty times costs one mesh and fifty transforms — which is
// how the game stores it and the only way a whole track fits in memory twice
// over.
class TreeWalker {
public:
  explicit TreeWalker(TrackScene *scene) : scene_(scene) {}

  void Walk(CPlugTree &tree, const GmIso4 &parent_iso, const CPlugMaterial *inherited, bool visible,
            std::uint32_t lod, TrackPurpose purpose) {
    GmIso4 iso;
    tree.ComposeCollisionIso(parent_iso, iso);
    const bool node_visible = visible && tree.IsVisible();
    const CPlugMaterial *material = tree.Material() != nullptr ? tree.Material() : inherited;

    if (CPlugVisual *visual = tree.Visual(); visual != nullptr) {
      if (const std::optional<std::uint32_t> mesh = MeshIndex(*visual)) {
        TrackInstance instance;
        instance.mesh = *mesh;
        instance.material = MaterialIndex(material);
        instance.transform = ToTransform(iso);
        instance.purpose = purpose;
        instance.lod = lod;
        instance.visible = node_visible;
        scene_->instances.push_back(instance);
      }
    }

    // A visual mip node holds the same shape at several detail levels. They are
    // kept and marked, not dropped, because the level a surface belongs to is
    // what decides whether it is drawn.
    auto *mip = dynamic_cast<CPlugTreeVisualMip *>(&tree);
    for (unsigned long i = 0; i < tree.GetChildCount(); ++i) {
      CPlugTree *child = tree.GetChild(i);
      if (child == nullptr) continue;
      std::uint32_t child_lod = lod;
      if (mip != nullptr) {
        for (unsigned long level = 0; level < mip->LevelCount(); ++level) {
          if (mip->LevelTree(level) == child) {
            child_lod = lod + static_cast<std::uint32_t>(level);
            break;
          }
        }
      }
      Walk(*child, iso, material, node_visible, child_lod, purpose);
    }
  }

private:
  std::uint32_t MaterialIndex(const CPlugMaterial *material) {
    const auto found = materials_.find(material);
    if (found != materials_.end()) return found->second;
    TrackMaterial out;
    if (material != nullptr) {
      out.path = material->ReplayRenderDefinition().MaterialPlainPath();
      out.surface = static_cast<std::uint8_t>(material->SurfaceMaterialId());
      out.water = material->ReplayRenderDefinition().HasBitmapRenderWater();
    }
    const auto index = static_cast<std::uint32_t>(scene_->materials.size());
    scene_->materials.push_back(std::move(out));
    materials_.emplace(material, index);
    return index;
  }

  std::optional<std::uint32_t> MeshIndex(CPlugVisual &visual) {
    const auto found = meshes_.find(&visual);
    if (found != meshes_.end()) return found->second;

    const std::optional<std::uint32_t> index = BuildMesh(visual);
    meshes_.emplace(&visual, index);
    return index;
  }

  std::optional<std::uint32_t> BuildMesh(CPlugVisual &visual) {
    const unsigned long vertex_count = visual.GetTotalVertexCount();
    const std::vector<GxVertex> vertices = visual.CanonicalVertices(1, 1, 1);
    if (vertex_count == 0u || vertices.size() != vertex_count) return std::nullopt;

    unsigned long index_count = 0u;
    unsigned short *indices = nullptr;
    visual.GetVertexIndexation(index_count, indices);
    if (index_count == 0u || indices == nullptr || index_count % 3u != 0u) return std::nullopt;

    TrackMesh mesh;
    mesh.has_normal = visual.HasVertexNormal();
    mesh.has_color = visual.HasVertexColor();

    // The first coordinate set is the surface's own; a second, where there is
    // one, is the lightmap, which this module has no lightmap to sample.
    GxTexCoordSet uv;
    mesh.has_uv = visual.VStreamOrClassic_GetTexCoordSet(uv, 0u, nullptr) != 0 && uv.Count() == vertex_count;

    mesh.vertices.resize(vertex_count);
    for (unsigned long i = 0; i < vertex_count; ++i) {
      TrackVertex &out = mesh.vertices[i];
      out.position = ToVec(vertices[i].position);
      out.normal = ToVec(vertices[i].normal);
      const auto &c = vertices[i].color;
      out.color = ft_color{c[0], c[1], c[2], c[3]};
      if (mesh.has_uv) {
        const GxTexCoord4 coord = uv.Coordinate4At(i);
        // The game measures v upwards from the bottom of the picture, while a
        // DDS stores its first row at the top, so the two disagree and the
        // texture arrives upside down unless v is turned over here. Under
        // repeat wrapping 1-v and -v are the same sample, and this spelling
        // survives coordinates that tile past one.
        out.uv = ft_vec2{coord.u, 1.0f - coord.v};
      }
    }

    mesh.indices.reserve(index_count);
    for (unsigned long i = 0; i < index_count; ++i) {
      if (indices[i] >= vertex_count) return std::nullopt;
      mesh.indices.push_back(indices[i]);
    }

    const auto index = static_cast<std::uint32_t>(scene_->meshes.size());
    scene_->meshes.push_back(std::move(mesh));
    return index;
  }

  TrackScene *scene_;
  std::unordered_map<const CPlugMaterial *, std::uint32_t> materials_;
  std::unordered_map<const CPlugVisual *, std::optional<std::uint32_t>> meshes_;
};

} // namespace

std::string EnvironmentPackName(fv::MapEnvironment environment) {
  switch (environment) {
  case fv::MapEnvironment::Alpine: return "Alpine";
  case fv::MapEnvironment::Speed: return "Speed";
  case fv::MapEnvironment::Rally: return "Rally";
  case fv::MapEnvironment::Island: return "Island";
  case fv::MapEnvironment::Coast: return "Coast";
  case fv::MapEnvironment::Bay: return "Bay";
  case fv::MapEnvironment::Stadium: return "Stadium";
  case fv::MapEnvironment::Unknown: break;
  }
  return {};
}

bool BuildTrackScene(ft_game *game, const PackSet &packs, const void *challenge_bytes,
                     std::size_t challenge_size, const std::string &pack_name, TrackScene *out) {
  if (out == nullptr || challenge_bytes == nullptr || challenge_size == 0u || pack_name.empty()) return false;
  out->meshes.clear();
  out->materials.clear();
  out->instances.clear();

  const std::vector<std::byte> packlist = ReadFile(game->packs + "/packlist.dat");
  const std::vector<std::byte> pak = ReadFile(game->packs + "/" + pack_name + ".pak");
  if (packlist.empty() || pak.empty()) return false;

  InstalledPackKeyCatalog keys;
  if (!keys.LoadFromMemory(packlist.data(), packlist.size(), "")) return false;

  InstalledPackAssetRepository assets;
  if (!assets.Configure(pak.data(), pak.size(), keys, pack_name.c_str())) {
    Log(game, FT_LOG_WARN, "Could not open %s.pak for the track's textures.", pack_name.c_str());
    return false;
  }

  // Before anything is loaded, so that the load can only ever hit entries this
  // already knows the path of.
  const std::unordered_map<std::uint32_t, std::string> material_paths =
      ResolveMaterialVocabulary(assets, packs, pack_name);

  ReplayFile challenge;
  if (ReadChallengeBytes(static_cast<const std::uint8_t *>(challenge_bytes), challenge_size, &challenge) !=
      ReplayFileReadError::Success) {
    Log(game, FT_LOG_WARN, "Could not re-read the challenge for its textures.");
    return false;
  }
  const CGameCtnReplayMapInput &map_input = challenge.MapInput();

  ReplaySceneDefinition definition;
  if (!BuildReplaySceneDefinition(map_input, assets, assets, definition) ||
      !definition.AppendAutomaticBase(map_input, assets)) {
    Log(game, FT_LOG_WARN, "Could not resolve the track's scene definition.");
    return false;
  }

  CGameCtnChallengeConstruction construction;
  ReplayChallengeBuildReport report;
  if (!BuildReplayChallenge(map_input, definition, construction, report) || construction.Challenge() == nullptr) {
    Log(game, FT_LOG_WARN, "Could not rebuild the track for its textures.");
    return false;
  }

  ReplaySceneBlockPlacements placements;
  placements.PopulateFromChallenge(*construction.Challenge());
  placements.SetRetainedLogicalStartPlacements(construction.RetainedLogicalStartPlacements());

  // The archives, loaded here rather than through the repository's own
  // BuildStaticScene, so that the graph they produce can be looked at before
  // the trees are assembled from it. That is the one moment at which a material
  // still knows which file it came from.
  StaticSolidArchiveLoadSession session;
  CGameCtnReplayArchiveStaticModelCollection archive_models;
  const StaticSolidArchiveLoadSession::ReplayArchiveResult loaded = session.LoadReplayArchives(
      {assets, map_input, placements, archive_models, ReplayStaticArchiveRole::Complete});
  if (!loaded.loaded || loaded.selectedMissing != 0u) {
    Log(game, FT_LOG_WARN, "Could not load the track's solid archives.");
    return false;
  }

  NameMaterials(material_paths, session);
  NameMaterialsFromSolids(assets, packs, pack_name, session);

  StaticSceneModelCollection models;
  if (!BuildStaticSceneFromArchive(session, archive_models, placements, models)) {
    Log(game, FT_LOG_WARN, "Could not build the track's static scene.");
    return false;
  }

  TreeWalker walker(out);
  for (const StaticSceneModel &model : models.Models()) {
    const TrackPurpose purpose = ToPurpose(model.Purpose());
    if (purpose == TRACK_PURPOSE_HIDDEN) continue;
    CPlugSolid *solid = model.Prototype().SourceSolid();
    CPlugTree *root = solid != nullptr ? solid->CollisionTree() : nullptr;
    if (root == nullptr) continue;
    walker.Walk(*root, model.WorldIso(), nullptr, true, 0u, purpose);
  }

  if (out->instances.empty()) return false;

  std::size_t named = 0;
  for (const TrackMaterial &material : out->materials)
    if (!material.path.empty()) ++named;
  Log(game, FT_LOG_INFO, "Decoded the track: %zu meshes, %zu placements, %zu materials (%zu named).",
      out->meshes.size(), out->instances.size(), out->materials.size(), named);
  return true;
}

} // namespace tmnf
