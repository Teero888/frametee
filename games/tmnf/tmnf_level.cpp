// Loading a challenge and turning the validator's render scene into something
// the engine's immediate 3D primitives can draw.
//
// ForeverValidator hands over the real authored geometry: meshes, instances,
// materials and texture coordinates. What it does not hand over is which
// picture goes on a surface, because its material paths come back empty — so
// that one link is made separately (tmnf_scene.cpp) and the texture itself is
// decoded from the installed game (tmnf_texture.cpp). A surface whose picture
// cannot be found falls back to a colour classified from its physical surface
// id, which is the distinction a driver cares about anyway.
//
// The engine's 3D path draws one triangle at a time with no lighting of its
// own, so the sun is baked into every triangle here at load and costs nothing
// per frame.

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

// What a surface looks like when its picture could not be found. The scene
// carries no material colours, so the only thing to go on is the physical
// surface id, which is exactly the distinction a driver cares about anyway:
// what the car will do when it touches this.
//
// Block names are deliberately not used to colour anything. A block name names
// a whole thirty-two metre tile, so keying a colour on "this is the start line"
// paints the entire starting straight, walls and all, rather than the line
// painted across it.
struct MaterialLook {
  ft_color color{0.5f, 0.5f, 0.52f, 1.f};
  bool emissive = false;
};

MaterialLook ClassifyMaterial(const TrackMaterial &material, std::uint32_t index) {
  MaterialLook look;
  if (material.water) {
    look.color = ft_color{0.16f, 0.40f, 0.60f, 0.72f};
    return look;
  }

  bool known = false;
  look.color = SurfaceColor(material.surface, &known);
  if (known) {
    // A booster is the one surface that has to be seen before it is driven on.
    look.emissive = material.surface == 26u;
    return look;
  }

  // An unrecognised surface still gets a colour of its own rather than joining
  // everything else in the same grey.
  look.color = HashedColor(index);
  return look;
}

// --- geometry ----------------------------------------------------------------

ft_vec3 TransformNormal(const TrackTransform &t, ft_vec3 n) {
  return Add(Add(Scale(t.basis_x, n.x), Scale(t.basis_y, n.y)), Scale(t.basis_z, n.z));
}

ft_vec3 TransformPoint(const TrackTransform &t, ft_vec3 p) { return Add(TransformNormal(t, p), t.translation); }

float BasisDeterminant(const TrackTransform &t) {
  return Dot(t.basis_x, Cross(t.basis_y, t.basis_z));
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
  for (const char *candidate : {"games/tmnf/GameData/Tracks", "../games/tmnf/GameData/Tracks"})
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

bool IsDistantScenery(const TrackMesh &mesh, const TrackInstance &instance) {
  Aabb box;
  for (const TrackVertex &vertex : mesh.vertices) box.Add(TransformPoint(instance.transform, vertex.position));
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
bool IsGroundSurface(const TrackMaterial &material) {
  switch (material.surface) {
  case 2:  // grass
  case 5:  // sand
  case 6:  // dirt
  case 8:  // dirt road
  case 21: // snow
    return true;
  default: return false;
  }
}

bool AppendGroundSlab(const TrackMesh &mesh, const TrackInstance &instance, const TrackMaterial &material,
                      ft_color color, std::vector<Triangle> &out, Aabb &bounds) {
  if (!IsGroundSurface(material)) return false;
  if (mesh.indices.size() / 3u <= kSlabTriangleThreshold) return false;

  Aabb box;
  for (const TrackVertex &vertex : mesh.vertices) box.Add(TransformPoint(instance.transform, vertex.position));
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
    const ft_vec3 a = TransformPoint(instance.transform, mesh.vertices[i0].position);
    const ft_vec3 b = TransformPoint(instance.transform, mesh.vertices[i1].position);
    const ft_vec3 c = TransformPoint(instance.transform, mesh.vertices[i2].position);
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

void AppendInstance(const TrackScene &scene, const TrackInstance &instance, std::uint32_t layer,
                    std::vector<Triangle> &out, Aabb &bounds) {
  if (instance.mesh >= scene.meshes.size() || instance.material >= scene.materials.size()) return;
  const TrackMesh &mesh = scene.meshes[instance.mesh];
  const TrackMaterial &material = scene.materials[instance.material];
  const MaterialLook look = ClassifyMaterial(material, instance.material);

  if (AppendGroundSlab(mesh, instance, material, look.color, out, bounds)) return;

  // A mirrored placement reverses triangle winding. Where the mesh carries
  // authored normals the winding is fixed per triangle against them instead,
  // which is exact; the determinant is the fallback for meshes that do not.
  const bool mirrored = BasisDeterminant(instance.transform) < 0.f;

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
    tri.a = TransformPoint(instance.transform, v0.position);
    tri.b = TransformPoint(instance.transform, v1.position);
    tri.c = TransformPoint(instance.transform, v2.position);
    if (mesh.has_uv && layer != kNoTextureLayer) {
      tri.layer = layer;
      tri.uv[0] = v0.uv;
      tri.uv[1] = v1.uv;
      tri.uv[2] = v2.uv;
    }

    ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
    if (LengthSq(face) < 1e-12f) continue;
    ft_vec3 normal = Normalize(face);

    if (mesh.has_normal) {
      // Authored normals say which way the surface faces no matter how the
      // indices are wound, so they decide both the shading and, by swapping the
      // last two corners when they disagree, the front face the renderer keeps.
      const ft_vec3 authored =
          Normalize(TransformNormal(instance.transform, Add(Add(v0.normal, v1.normal), v2.normal)));
      if (Dot(face, authored) < 0.f) {
        std::swap(tri.b, tri.c);
        std::swap(tri.uv[1], tri.uv[2]);
        face = Scale(face, -1.f);
      }
      normal = authored;
    }

    // On a textured surface the colour is only the light: the picture is the
    // appearance, and multiplying it by a guess at the appearance would be
    // painting the same thing twice.
    ft_color base = tri.layer != kNoTextureLayer ? ft_color{1.f, 1.f, 1.f, look.color.a} : look.color;
    if (mesh.has_color) {
      // Authored vertex colour is what paints the stadium's own detail; it is
      // tinted rather than replaced so the surface still reads as its material.
      const ft_color &c = v0.color;
      base = MixColor(base, ft_color{base.r * c.r * 2.f, base.g * c.g * 2.f, base.b * c.b * 2.f, base.a}, 0.6f);
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

  // The installed packs, opened for what the sandbox never reads: the pictures.
  // The environment's own pack holds the track, and the two shared ones hold
  // what every environment draws from.
  if (!game->packs_open.IsOpen()) {
    if (const std::string pack = EnvironmentPackName(level->start.mapEnvironment); !pack.empty())
      game->packs_open.Open(game->packs, {pack, VehiclePackName(level->start.vehicleModel), "Game", "Resource"});
  }

  // The car's authored model. Decoding it costs about as much as one track
  // block, and the same car serves every track that uses it, so it is loaded
  // once and kept. A failure here is not a failure to open the track: the
  // modelled car stands in and the run is unaffected.
  if (const std::string pack = VehiclePackName(level->start.vehicleModel);
      !pack.empty() && game->vehicle.pack != pack) {
    game->vehicle.pack = pack;
    game->vehicle.loaded = LoadVehicleModel(game, game->packs_open, game->textures, pack, &game->vehicle);
    if (!game->vehicle.loaded) game->vehicle.faces.clear();
  }

  // The track itself, decoded from the installed game rather than taken from
  // the sandbox. The sandbox's own scene is the same geometry, but it carries no
  // material paths and, on several environments, no materials at all: the
  // materials live in each tile's solid and are only reachable by reading them
  // (see tmnf_scene.cpp). Everything on screen therefore comes from here.
  TrackScene scene;
  const bool decoded = !EnvironmentPackName(level->start.mapEnvironment).empty() && game->packs_open.IsOpen() &&
                       BuildTrackScene(game, game->packs_open, bytes.data(), bytes.size(),
                                       EnvironmentPackName(level->start.mapEnvironment), &scene);

  // Each material's picture, decoded once and kept as a page of one array.
  std::vector<std::uint32_t> material_layers;
  if (decoded) {
    material_layers.assign(scene.materials.size(), kNoTextureLayer);
    std::size_t textured = 0;
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      if (scene.materials[i].path.empty()) continue;
      if (const std::optional<std::uint32_t> layer = game->textures.Layer(game->packs_open, scene.materials[i].path)) {
        material_layers[i] = *layer;
        ++textured;
      }
    }
    std::size_t named = 0;
    for (const TrackMaterial &material : scene.materials)
      if (!material.path.empty()) ++named;
    // A handful of textures do live in the packs, so "none at all" is not the
    // symptom; "almost none of the ones a track actually names" is.
    if (named != 0u && textured * 4u < named) {
      // The packs alone cannot draw a track. Saying so is the difference
      // between a five minute fix and an afternoon wondering why every surface
      // is one flat colour.
      Log(game, FT_LOG_WARN,
          "Only %zu of %zu track materials found a texture. The packs hold the descriptors; the images themselves "
          "live in the installed game's GameData, beside its Packs. Point FRAMETEE_TMNF_PACKS at a full TrackMania "
          "installation, or set FRAMETEE_TMNF_GAMEDATA to its GameData directory. Until then the track is drawn in "
          "flat surface colours.",
          textured, named);
    } else {
      Log(game, FT_LOG_INFO, "Textured %zu of %zu track materials.", textured, scene.materials.size());
    }
  }

  BuildStats stats;
  Aabb backdrop_bounds;
  // The blocks the track is actually built from, as opposed to the hills and
  // scenery around them. This is what the editor should frame its camera on.
  Aabb played_bounds;
  for (const TrackInstance &instance : scene.instances) {
    if (!instance.visible || instance.purpose == TRACK_PURPOSE_HIDDEN) {
      ++stats.instances_skipped;
      continue;
    }
    // The scene carries every level of detail for a block. Drawing more than
    // the finest one triples the triangle count and makes the coarser shells
    // fight the real surface for the depth buffer.
    if (instance.lod != 0u) {
      ++stats.lod_skipped;
      continue;
    }
    if (instance.mesh >= scene.meshes.size()) continue;

    ++stats.instances_drawn;
    const std::uint32_t layer =
        instance.material < material_layers.size() ? material_layers[instance.material] : kNoTextureLayer;

    // A stadium scene ends in a few kilometre-wide sky and ground planes. They
    // belong on screen, but not in the level's bounds: the engine frames its
    // camera on those, and a track that is really three hundred metres across
    // would otherwise open five kilometres away.
    if (IsDistantScenery(scene.meshes[instance.mesh], instance)) {
      AppendInstance(scene, instance, layer, level->backdrop, backdrop_bounds);
    } else {
      const std::size_t before = level->track.size();
      AppendInstance(scene, instance, layer, level->track, level->world_bounds);
      if (instance.purpose == TRACK_PURPOSE_BLOCK) {
        for (std::size_t i = before; i < level->track.size(); ++i) {
          played_bounds.Add(level->track[i].a);
          played_bounds.Add(level->track[i].b);
          played_bounds.Add(level->track[i].c);
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

  game->textures.Upload(game);
  // A hundred megabytes of pack, held open only to decode from. Everything that
  // needed it has been decoded, and the next track will open it again.
  game->packs_open.Close();

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
