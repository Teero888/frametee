// Loading a challenge and turning the validator's render scene into something
// the engine's immediate 3D primitives can draw.
//
// ForeverValidator hands over the real authored geometry: meshes, instances and
// materials. It does not hand over pixels, and the material paths come back
// empty, so colour here is classified from the physical surface id and the
// lighting is baked into each triangle at load. The engine's 3D path is flat
// unlit vertex colour drawn one triangle at a time, so anything that can be
// decided once must be decided here.

#include "tmnf_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace tmnf {
namespace {

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// --- material classification -------------------------------------------------

} // namespace

// TrackMania's physical surface ids. Only the ones that read differently on
// screen are named; anything else gets a colour of its own further down.
ft_color SurfaceColor(std::uint8_t surface, bool *known) {
  *known = true;
  switch (surface) {
  case 0: return ft_color{0.55f, 0.55f, 0.57f, 1.f};  // concrete
  case 1: return ft_color{0.48f, 0.48f, 0.50f, 1.f};  // pavement
  case 2: return ft_color{0.30f, 0.52f, 0.24f, 1.f};  // grass
  case 3: return ft_color{0.72f, 0.88f, 0.95f, 1.f};  // ice
  case 4: return ft_color{0.56f, 0.58f, 0.62f, 1.f};  // metal
  case 5: return ft_color{0.83f, 0.74f, 0.52f, 1.f};  // sand
  case 6: return ft_color{0.52f, 0.38f, 0.26f, 1.f};  // dirt
  case 8: return ft_color{0.48f, 0.36f, 0.25f, 1.f};  // dirt road
  case 9:
  case 10: return ft_color{0.20f, 0.20f, 0.22f, 1.f}; // rubber
  case 12: return ft_color{0.46f, 0.44f, 0.42f, 1.f}; // rock
  case 13: return ft_color{0.18f, 0.42f, 0.62f, 0.75f}; // water
  case 14: return ft_color{0.55f, 0.40f, 0.24f, 1.f};  // wood
  case 15: return ft_color{0.80f, 0.24f, 0.20f, 1.f};  // danger
  case 16: return ft_color{0.30f, 0.31f, 0.34f, 1.f};  // asphalt
  case 21: return ft_color{0.92f, 0.94f, 0.97f, 1.f};  // snow
  case 26: return ft_color{0.95f, 0.65f, 0.12f, 1.f};  // turbo
  case 27: return ft_color{0.66f, 0.82f, 0.92f, 1.f};  // road ice
  default: break;
  }
  *known = false;
  return ft_color{0.5f, 0.5f, 0.52f, 1.f};
}

namespace {

// A stable colour per material, so two unrecognised surfaces never collapse
// into the same grey.
ft_color HashedColor(std::uint64_t id) {
  std::uint64_t h = id * 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;
  const float hue = static_cast<float>(h & 0xFFFFu) / 65535.f;
  // A narrow sweep around neutral: track surfaces are grey-brown, and a full
  // rainbow would read as debug output rather than a track.
  return ft_color{0.42f + 0.16f * std::sin(hue * 2.f * kPi),
                  0.42f + 0.14f * std::sin(hue * 2.f * kPi + 2.094f),
                  0.44f + 0.16f * std::sin(hue * 2.f * kPi + 4.188f), 1.f};
}

// What a surface looks like. The scene carries no texture paths and no material
// colours — every path field comes back empty — so the only thing to go on is
// the physical surface id, which is exactly the distinction a driver cares
// about anyway: what the car will do when it touches this.
//
// Block names are deliberately not used to colour anything. A block name names
// a whole thirty-two metre tile, so keying a colour on "this is the start line"
// paints the entire starting straight, walls and all, rather than the line
// painted across it.
struct MaterialLook {
  ft_color color{0.5f, 0.5f, 0.52f, 1.f};
  bool emissive = false;
};

MaterialLook ClassifyMaterial(const fve::PhysicsSandboxRenderMaterial *material) {
  MaterialLook look;
  if (!material) {
    look.color = ft_color{0.5f, 0.5f, 0.52f, 1.f};
    return look;
  }

  if (material->water) {
    look.color = ft_color{0.16f, 0.40f, 0.60f, 0.72f};
    return look;
  }

  bool known = false;
  look.color = SurfaceColor(material->surfaceMaterialId, &known);
  if (known) {
    // A booster is the one surface that has to be seen before it is driven on.
    look.emissive = material->surfaceMaterialId == 26u;
    return look;
  }

  // An unrecognised surface still gets a colour of its own rather than joining
  // everything else in the same grey.
  look.color = HashedColor(material->id);
  return look;
}

// --- geometry ----------------------------------------------------------------

ft_vec3 TransformPoint(const fve::PhysicsSandboxTransform &t, const fv::Vector3 &p) {
  return ft_vec3{
      t.basisX.x * p.x + t.basisY.x * p.y + t.basisZ.x * p.z + t.translation.x,
      t.basisX.y * p.x + t.basisY.y * p.y + t.basisZ.y * p.z + t.translation.y,
      t.basisX.z * p.x + t.basisY.z * p.y + t.basisZ.z * p.z + t.translation.z,
  };
}

ft_vec3 TransformNormal(const fve::PhysicsSandboxTransform &t, const fv::Vector3 &n) {
  return ft_vec3{
      t.basisX.x * n.x + t.basisY.x * n.y + t.basisZ.x * n.z,
      t.basisX.y * n.x + t.basisY.y * n.y + t.basisZ.y * n.z,
      t.basisX.z * n.x + t.basisY.z * n.y + t.basisZ.z * n.z,
  };
}

float BasisDeterminant(const fve::PhysicsSandboxTransform &t) {
  return t.basisX.x * (t.basisY.y * t.basisZ.z - t.basisY.z * t.basisZ.y) -
         t.basisY.x * (t.basisX.y * t.basisZ.z - t.basisX.z * t.basisZ.y) +
         t.basisZ.x * (t.basisX.y * t.basisY.z - t.basisX.z * t.basisY.y);
}

// Lighting, baked once. A hemisphere term keeps undersides from going flat
// black without needing a second light, and the sun does the rest.
ft_color ApplyLight(ft_color base, ft_vec3 normal, bool emissive) {
  if (emissive) return MixColor(base, ft_color{1.f, 1.f, 1.f, base.a}, 0.15f);
  const float sun = std::max(0.f, Dot(normal, kSunDirection));
  const float sky = 0.5f + 0.5f * normal.y;
  const float light = 0.26f + 0.30f * sky + 0.52f * sun;
  return ft_color{base.r * light, base.g * light, base.b * light, base.a};
}

bool ShouldDrawPurpose(fve::PhysicsSandboxScenePurpose purpose) {
  switch (purpose) {
  // Collision-only and editor-only objects have no business on screen: they
  // are the invisible walls, gate triggers and placement helpers.
  case fve::PhysicsSandboxScenePurpose::Clip:
  case fve::PhysicsSandboxScenePurpose::Helper:
  case fve::PhysicsSandboxScenePurpose::CheckpointTrigger:
  case fve::PhysicsSandboxScenePurpose::DedicatedInitialCollision: return false;
  default: return true;
  }
}

// Möller-Trumbore, used only by the camera's line of sight.
bool SegmentTriangle(ft_vec3 origin, ft_vec3 direction, const Triangle &tri, float *out_t) {
  const ft_vec3 e1 = Sub(tri.b, tri.a);
  const ft_vec3 e2 = Sub(tri.c, tri.a);
  const ft_vec3 p = Cross(direction, e2);
  const float det = Dot(e1, p);
  if (std::fabs(det) < 1e-7f) return false;
  const float inv = 1.f / det;
  const ft_vec3 s = Sub(origin, tri.a);
  const float u = Dot(s, p) * inv;
  if (u < 0.f || u > 1.f) return false;
  const ft_vec3 q = Cross(s, e1);
  const float v = Dot(direction, q) * inv;
  if (v < 0.f || u + v > 1.f) return false;
  const float t = Dot(e2, q) * inv;
  if (t < 0.f || t > 1.f) return false;
  *out_t = t;
  return true;
}

} // namespace

// --- the triangle grid -------------------------------------------------------

void TriangleGrid::Clear() {
  cells.clear();
  dim_x = dim_z = 0;
}

void TriangleGrid::Build(std::vector<Triangle> &triangles, const Aabb &bounds) {
  Clear();
  if (triangles.empty() || !bounds.Valid()) return;

  const float span_x = std::max(1.f, bounds.mx.x - bounds.mn.x);
  const float span_z = std::max(1.f, bounds.mx.z - bounds.mn.z);

  // Enough cells that a frustum rejects most of the track without so many that
  // walking them costs more than the triangles would.
  constexpr int kMaxDim = 96;
  cell_size = std::max(24.f, std::max(span_x, span_z) / static_cast<float>(kMaxDim));
  origin_x = bounds.mn.x;
  origin_z = bounds.mn.z;
  dim_x = std::clamp(static_cast<int>(span_x / cell_size) + 1, 1, kMaxDim);
  dim_z = std::clamp(static_cast<int>(span_z / cell_size) + 1, 1, kMaxDim);

  const std::size_t cell_count = static_cast<std::size_t>(dim_x) * static_cast<std::size_t>(dim_z);
  cells.assign(cell_count, GridCell{});

  const auto cell_of = [&](const Triangle &tri) {
    const float cx = (tri.a.x + tri.b.x + tri.c.x) / 3.f;
    const float cz = (tri.a.z + tri.b.z + tri.c.z) / 3.f;
    const int ix = std::clamp(static_cast<int>((cx - origin_x) / cell_size), 0, dim_x - 1);
    const int iz = std::clamp(static_cast<int>((cz - origin_z) / cell_size), 0, dim_z - 1);
    return static_cast<std::size_t>(iz) * static_cast<std::size_t>(dim_x) + static_cast<std::size_t>(ix);
  };

  // Count, prefix sum, scatter: one pass more than a vector of vectors, and no
  // per-cell allocation at all.
  for (const Triangle &tri : triangles) cells[cell_of(tri)].count++;

  std::uint32_t running = 0;
  for (GridCell &cell : cells) {
    cell.first = running;
    running += cell.count;
    cell.count = 0;
  }

  std::vector<Triangle> sorted(triangles.size());
  for (const Triangle &tri : triangles) {
    GridCell &cell = cells[cell_of(tri)];
    sorted[cell.first + cell.count++] = tri;
    // The box holds whole triangles, not just their centres, so a cell that
    // passes the frustum really does contain everything it claims.
    cell.bounds.Add(tri.a);
    cell.bounds.Add(tri.b);
    cell.bounds.Add(tri.c);
  }
  triangles.swap(sorted);
}

// --- track discovery ---------------------------------------------------------

std::vector<std::byte> ReadFileBytes(const char *path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const auto size = static_cast<std::size_t>(file.tellg());
  file.seekg(0);
  std::vector<std::byte> bytes(size);
  file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
  return bytes;
}

std::string ResolveTracks(const ft_engine_api *api) {
  const auto usable = [](const std::string &directory) {
    std::error_code error;
    return !directory.empty() && std::filesystem::is_directory(directory, error);
  };

  if (const char *env = std::getenv("FRAMETEE_TMNF_TRACKS"); env && usable(env)) return env;
  if (api && api->resolve_data_path) {
    char buffer[1024];
    api->resolve_data_path("Tracks", buffer, sizeof(buffer));
    if (usable(buffer)) return buffer;
  }
  for (const char *candidate : {"games/tmnf/Tracks", "../games/tmnf/Tracks"})
    if (usable(candidate)) return candidate;
  return {};
}

void ScanTracks(ft_game *game) {
  game->scanned = true;
  game->campaigns.clear();
  game->tracks_root = ResolveTracks(game->engine);
  if (game->tracks_root.empty()) return;

  std::error_code error;
  const std::filesystem::path root(game->tracks_root);
  std::vector<std::filesystem::path> files;
  for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
    if (error) break;
    if (!it->is_regular_file(error)) continue;
    if (Lowered(it->path().filename().string()).find(".challenge.gbx") == std::string::npos) continue;
    files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());

  for (const auto &file : files) {
    std::string campaign = std::filesystem::relative(file.parent_path(), root, error).string();
    if (campaign.empty() || campaign == ".") campaign = "Tracks";
    if (game->campaigns.empty() || game->campaigns.back().name != campaign)
      game->campaigns.push_back(Campaign{campaign, {}});

    std::string name = file.filename().string();
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos) name.resize(dot);

    TrackEntry entry;
    entry.path = file.string();
    entry.name = std::move(name);
    game->campaigns.back().tracks.push_back(std::move(entry));
  }

  Log(game, FT_LOG_INFO, "Found %zu tracks in %zu campaigns under %s", files.size(), game->campaigns.size(),
      game->tracks_root.c_str());
}

// --- loading -----------------------------------------------------------------

namespace {

// Anything wider than any real track: the sky dome and the ground plane the
// environment is painted on.
constexpr float kSceneryFootprint = 1500.f;

bool IsDistantScenery(const fve::PhysicsSandboxRenderScene &scene, const fve::PhysicsSandboxRenderInstance &instance) {
  if (instance.meshIndex >= scene.meshes.size()) return false;
  const auto &mesh = scene.meshes[instance.meshIndex];

  // The mesh already carries its own bounds; transforming the eight corners is
  // enough and costs nothing next to walking every vertex.
  Aabb box;
  for (int i = 0; i < 8; ++i) {
    const fv::Vector3 corner{(i & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                             (i & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                             (i & 4) ? mesh.boundsMax.z : mesh.boundsMin.z};
    box.Add(TransformPoint(instance.worldTransform, corner));
  }
  if (!box.Valid()) return false;
  return box.mx.x - box.mn.x > kSceneryFootprint || box.mx.z - box.mn.z > kSceneryFootprint;
}

struct BuildStats {
  std::size_t instances_drawn = 0;
  std::size_t instances_skipped = 0;
  std::size_t lod_skipped = 0;
};

// TrackMania's terrain is not a surface. A single grass tile is a hundred and
// twenty-eight vertical cards, half a metre tall and a quarter of a metre
// apart, standing in a thirty-two metre square: real grass blades, meant for a
// textured, alpha-tested, lit renderer. Through a flat untextured one they are
// a million coplanar sheets fighting each other for the depth buffer, and on a
// short Nations track they are four fifths of the entire map.
//
// So an instance that is a field of cards, or a flat slab, is replaced by the
// one horizontal quad it is standing on. Returns false when the instance is
// neither and has to be kept as authored.
constexpr std::size_t kSlabTriangleThreshold = 64u;
constexpr float kSlabHeight = 2.0f;
constexpr float kSlabFootprint = 4.0f;

// Grass, dirt, sand and snow: the surfaces TrackMania draws as fields of cards.
bool IsGroundSurface(const fve::PhysicsSandboxRenderMaterial *material) {
  if (!material) return false;
  switch (material->surfaceMaterialId) {
  case 2:  // grass
  case 5:  // sand
  case 6:  // dirt
  case 8:  // dirt road
  case 21: // snow
    return true;
  default: return false;
  }
}

bool AppendGroundSlab(const fve::PhysicsSandboxRenderMesh &mesh, const fve::PhysicsSandboxRenderInstance &instance,
                      const fve::PhysicsSandboxRenderMaterial *material, ft_color color, std::vector<Triangle> &out,
                      Aabb &bounds) {
  if (!IsGroundSurface(material)) return false;
  if (mesh.indices.size() / 3u <= kSlabTriangleThreshold) return false;

  Aabb box;
  for (const auto &vertex : mesh.vertices) box.Add(TransformPoint(instance.worldTransform, vertex.position));
  if (!box.Valid()) return false;
  // Short, and wide enough that a quad is a fair description of it.
  if (box.mx.y - box.mn.y > kSlabHeight) return false;
  if (box.mx.x - box.mn.x < kSlabFootprint || box.mx.z - box.mn.z < kSlabFootprint) return false;

  double up_area = 0.0;
  double up_height = 0.0;
  double side_area = 0.0;
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;
    const ft_vec3 a = TransformPoint(instance.worldTransform, mesh.vertices[i0].position);
    const ft_vec3 b = TransformPoint(instance.worldTransform, mesh.vertices[i1].position);
    const ft_vec3 c = TransformPoint(instance.worldTransform, mesh.vertices[i2].position);
    const ft_vec3 face = Cross(Sub(b, a), Sub(c, a));
    const float area = Length(face) * 0.5f;
    if (area < 1e-6f) continue;
    const float ny = std::fabs(face.y) / (2.f * area);
    if (ny > 0.7f) {
      up_area += area;
      up_height += static_cast<double>(area) * (a.y + b.y + c.y) / 3.0;
    } else if (ny < 0.3f) {
      side_area += area;
    }
  }

  const double total = up_area + side_area;
  if (total <= 0.0) return false;

  // The quad spans the instance's bounding box, so the instance has to actually
  // fill that box. A long barrier curving across the map is short and wide too,
  // and its box is most of a district: collapsing that paints the whole
  // district. Requiring the geometry to cover its own footprint is what tells a
  // field of grass apart from a fence taking the scenic route.
  const double footprint = static_cast<double>(box.mx.x - box.mn.x) * static_cast<double>(box.mx.z - box.mn.z);
  if (footprint <= 0.0 || total < footprint * 0.6) return false;

  float y;
  if (up_area > side_area) {
    // A slab: sit the quad on the surface the upward faces describe, which is
    // not the top of the box because a slab usually has a lip around it.
    y = static_cast<float>(up_height / up_area);
  } else if (side_area / total > 0.8) {
    // A field of cards: sit the quad on the ground they are planted in, a
    // shade below so that real terrain underneath still wins the depth test.
    y = box.mn.y - 0.02f;
  } else {
    return false;
  }

  const ft_vec3 corner[4] = {{box.mn.x, y, box.mn.z}, {box.mx.x, y, box.mn.z}, {box.mx.x, y, box.mx.z},
                             {box.mn.x, y, box.mx.z}};
  const std::uint32_t lit = PackColor(ApplyLight(color, ft_vec3{0.f, 1.f, 0.f}, false));
  out.push_back(Triangle{corner[0], corner[1], corner[2], lit});
  out.push_back(Triangle{corner[0], corner[2], corner[3], lit});
  for (const ft_vec3 &p : corner) bounds.Add(p);
  return true;
}

void AppendInstance(const fve::PhysicsSandboxRenderScene &scene, const fve::PhysicsSandboxRenderInstance &instance,
                    std::vector<Triangle> &out, Aabb &bounds) {
  if (instance.meshIndex >= scene.meshes.size()) return;
  const auto &mesh = scene.meshes[instance.meshIndex];
  const auto *material =
      instance.materialIndex < scene.materials.size() ? &scene.materials[instance.materialIndex] : nullptr;
  const MaterialLook look = ClassifyMaterial(material);

  if (AppendGroundSlab(mesh, instance, material, look.color, out, bounds)) return;

  // A mirrored placement reverses triangle winding. Where the mesh carries
  // authored normals the winding is fixed per triangle against them instead,
  // which is exact; the determinant is the fallback for meshes that do not.
  const bool mirrored = BasisDeterminant(instance.worldTransform) < 0.f;

  // Not reserve(): asking for exactly what is needed, once per instance, makes
  // the vector reallocate on every instance and turns a load into minutes.
  // Geometric growth is what the vector already does on its own.
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::uint32_t i0 = mesh.indices[i];
    const std::uint32_t i1 = mesh.indices[mirrored ? i + 2 : i + 1];
    const std::uint32_t i2 = mesh.indices[mirrored ? i + 1 : i + 2];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;

    const auto &v0 = mesh.vertices[i0];
    const auto &v1 = mesh.vertices[i1];
    const auto &v2 = mesh.vertices[i2];

    Triangle tri;
    tri.a = TransformPoint(instance.worldTransform, v0.position);
    tri.b = TransformPoint(instance.worldTransform, v1.position);
    tri.c = TransformPoint(instance.worldTransform, v2.position);

    ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
    if (LengthSq(face) < 1e-12f) continue;
    ft_vec3 normal = Normalize(face);

    if (mesh.hasNormals) {
      // Authored normals say which way the surface faces no matter how the
      // indices are wound, so they decide both the shading and, by swapping the
      // last two corners when they disagree, the front face the renderer keeps.
      const ft_vec3 authored = Normalize(TransformNormal(
          instance.worldTransform,
          fv::Vector3{v0.normal.x + v1.normal.x + v2.normal.x, v0.normal.y + v1.normal.y + v2.normal.y,
                      v0.normal.z + v1.normal.z + v2.normal.z}));
      if (Dot(face, authored) < 0.f) {
        std::swap(tri.b, tri.c);
        face = Scale(face, -1.f);
      }
      normal = authored;
    }

    ft_color base = look.color;
    if (mesh.hasVertexColors) {
      // Authored vertex colour is what paints the stadium's own detail; it is
      // tinted rather than replaced so the surface still reads as its material.
      const auto &c = v0.color;
      base = MixColor(base, ft_color{base.r * c.x * 2.f, base.g * c.y * 2.f, base.b * c.z * 2.f, base.a}, 0.6f);
    }

    tri.color = PackColor(ApplyLight(base, normal, look.emissive));
    out.push_back(tri);

    bounds.Add(tri.a);
    bounds.Add(tri.b);
    bounds.Add(tri.c);
  }
}

// Everything the render scene could not give us. A track that decodes no
// authored visuals is still drivable, so its collision hull is drawn instead.
void AppendCollisionFallback(ft_game *game, ft_level *level) {
  auto scene = game->sandbox->ReadScene();
  if (!scene) return;

  for (const auto &triangle : scene.Value().collisionTriangles) {
    Triangle tri;
    tri.a = ToVec3(triangle.a);
    tri.b = ToVec3(triangle.b);
    tri.c = ToVec3(triangle.c);
    const ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
    if (LengthSq(face) < 1e-12f) continue;
    const ft_vec3 normal = Normalize(face);
    const ft_color base =
        normal.y > 0.75f ? ft_color{0.36f, 0.38f, 0.42f, 1.f} : ft_color{0.52f, 0.40f, 0.38f, 1.f};
    tri.color = PackColor(ApplyLight(base, normal, false));
    level->track.push_back(tri);
    level->world_bounds.Add(tri.a);
    level->world_bounds.Add(tri.b);
    level->world_bounds.Add(tri.c);
  }
}

} // namespace

ft_level *LevelLoad(ft_game *game, const char *path) {
  if (!game || !path) return nullptr;
  if (game->packs.empty()) {
    Log(game, FT_LOG_ERROR, "Cannot open a track without the installed game packs.");
    return nullptr;
  }

  const std::vector<std::byte> bytes = ReadFileBytes(path);
  if (bytes.empty()) {
    Log(game, FT_LOG_ERROR, "Could not read the challenge file '%s'.", path);
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(game->mutex);
  if (!OpenSandbox(game, bytes.data(), bytes.size(), path)) return nullptr;

  auto initial = game->sandbox->CaptureState();
  if (!initial) {
    Log(game, FT_LOG_ERROR, "Could not capture the starting state.");
    game->sandbox.reset();
    return nullptr;
  }

  auto *level = new ft_level();
  level->initial.emplace(std::move(initial).Value());
  level->start = level->initial->View();

  auto name = game->sandbox->ReadMapName();
  level->name = name ? name.Value() : "TrackMania track";

  // The car's own collision ellipsoids, so what is drawn is the shape the
  // simulation actually pushes around rather than a guess at one.
  if (auto scene = game->sandbox->ReadScene()) level->car_shape = std::move(scene).Value().carEllipsoids;

  // The car's authored model. Decoding it costs about as much as one track
  // block, and the same car serves every track that uses it, so it is loaded
  // once and kept. A failure here is not a failure to open the track: the
  // modelled car stands in and the run is unaffected.
  if (const std::string pack = VehiclePackName(level->start.vehicleModel);
      !pack.empty() && game->vehicle.pack != pack) {
    game->vehicle.pack = pack;
    game->vehicle.loaded = LoadVehicleModel(game, pack, &game->vehicle);
    if (!game->vehicle.loaded) game->vehicle.faces.clear();
  }

  BuildStats stats;
  Aabb backdrop_bounds;
  // The blocks the track is actually built from, as opposed to the hills and
  // scenery around them. This is what the editor should frame its camera on.
  Aabb played_bounds;
  if (auto handle = game->sandbox->ReadRenderScene(); handle && handle.Value()) {
    const auto &scene = *handle.Value();
    for (const auto &instance : scene.instances) {
      if (!instance.visible) {
        ++stats.instances_skipped;
        continue;
      }
      // The scene carries every level of detail for a block. Drawing more than
      // the finest one triples the triangle count and makes the coarser shells
      // fight the real surface for the depth buffer.
      if (instance.lodLevel != 0u) {
        ++stats.lod_skipped;
        continue;
      }
      if (!ShouldDrawPurpose(instance.purpose)) {
        ++stats.instances_skipped;
        continue;
      }

      ++stats.instances_drawn;
      // A stadium scene ends in a few kilometre-wide sky and ground planes.
      // They belong on screen, but not in the level's bounds: the engine frames
      // its camera on those, and a track that is really three hundred metres
      // across would otherwise open five kilometres away.
      if (instance.renderLayer == fve::PhysicsSandboxRenderLayer::Background || IsDistantScenery(scene, instance)) {
        AppendInstance(scene, instance, level->backdrop, backdrop_bounds);
      } else {
        const std::size_t before = level->track.size();
        AppendInstance(scene, instance, level->track, level->world_bounds);
        if (instance.purpose == fve::PhysicsSandboxScenePurpose::PlacedBlock) {
          for (std::size_t i = before; i < level->track.size(); ++i) {
            played_bounds.Add(level->track[i].a);
            played_bounds.Add(level->track[i].b);
            played_bounds.Add(level->track[i].c);
          }
        }
      }
    }
  }

  if (level->track.empty()) {
    Log(game, FT_LOG_WARN, "No authored visuals decoded; drawing the collision hull instead.");
    AppendCollisionFallback(game, level);
  }

  if (!level->world_bounds.Valid()) {
    const ft_vec3 spawn = ToVec3(level->start.car.position);
    level->world_bounds.Add(Sub(spawn, ft_vec3{64.f, 8.f, 64.f}));
    level->world_bounds.Add(Add(spawn, ft_vec3{64.f, 64.f, 64.f}));
  }

  level->track_grid.Build(level->track, level->world_bounds);
  if (!level->backdrop.empty()) level->backdrop_grid.Build(level->backdrop, backdrop_bounds);

  // The engine frames its camera on these bounds and treats them as the ground
  // plane, so they are the extent of the blocks that make up the track rather
  // than of the countryside around them.
  const Aabb &framing = played_bounds.Valid() ? played_bounds : level->world_bounds;
  level->bounds = ft_rect{framing.mn.x, framing.mn.z, std::max(32.f, framing.mx.x - framing.mn.x),
                          std::max(32.f, framing.mx.z - framing.mn.z)};

  Log(game, FT_LOG_INFO, "Loaded '%s': %u checkpoints, %zu track triangles, %zu backdrop triangles",
      level->name.c_str(), level->start.checkpointsTotal, level->track.size(), level->backdrop.size());
  if (stats.lod_skipped) Log(game, FT_LOG_TRACE, "Skipped %zu coarse level-of-detail instances.", stats.lod_skipped);

  game->level = level;
  CameraReset(game);
  return level;
}

void LevelDestroy(ft_game *game, ft_level *level) {
  if (!level) return;
  if (game && game->level == level) {
    game->level = nullptr;
    CloseSandbox(game);
  }
  delete level;
}

// --- line of sight -----------------------------------------------------------

bool SegmentHit(const ft_level *level, ft_vec3 start, ft_vec3 end, float *out_fraction) {
  if (!level || !out_fraction) return false;
  const TriangleGrid &grid = level->track_grid;
  if (grid.cells.empty()) return false;

  const ft_vec3 direction = Sub(end, start);
  if (LengthSq(direction) < 1e-8f) return false;

  Aabb segment;
  segment.Add(start);
  segment.Add(end);

  const int min_x = std::clamp(static_cast<int>((segment.mn.x - grid.origin_x) / grid.cell_size), 0, grid.dim_x - 1);
  const int max_x = std::clamp(static_cast<int>((segment.mx.x - grid.origin_x) / grid.cell_size), 0, grid.dim_x - 1);
  const int min_z = std::clamp(static_cast<int>((segment.mn.z - grid.origin_z) / grid.cell_size), 0, grid.dim_z - 1);
  const int max_z = std::clamp(static_cast<int>((segment.mx.z - grid.origin_z) / grid.cell_size), 0, grid.dim_z - 1);

  float nearest = 1.f;
  bool hit = false;
  for (int iz = min_z; iz <= max_z; ++iz) {
    for (int ix = min_x; ix <= max_x; ++ix) {
      const GridCell &cell = grid.cells[static_cast<std::size_t>(iz) * static_cast<std::size_t>(grid.dim_x) +
                                        static_cast<std::size_t>(ix)];
      if (cell.count == 0) continue;
      if (cell.bounds.mn.y > segment.mx.y || cell.bounds.mx.y < segment.mn.y) continue;
      for (std::uint32_t i = 0; i < cell.count; ++i) {
        const Triangle &tri = level->track[cell.first + i];
        float t = 1.f;
        if (SegmentTriangle(start, direction, tri, &t) && t < nearest) {
          nearest = t;
          hit = true;
        }
      }
    }
  }

  *out_fraction = nearest;
  return hit;
}

} // namespace tmnf
