// Loading a challenge and turning the validator's render scene into something
// the engine's immediate 3D primitives can draw.
//
// ForeverValidator hands over the real authored geometry: meshes, instances,
// materials and texture coordinates. What it does not hand over is which
// picture goes on a surface, because its material paths come back empty, so
// that one link is made separately (tmnf_scene.cpp) and the texture itself is
// decoded from the installed game (tmnf_texture.cpp). A surface whose picture
// cannot be found falls back to a colour classified from its physical surface
// id, which is the distinction a driver cares about anyway.
//
// The engine's 3D path draws one triangle at a time with no lighting of its
// own, so a surface's appearance is its picture and the colour it was authored
// with, resolved here at load and costing nothing per frame.

#include "tmnf_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
  // How the shader says the surface is drawn, as opposed to what colour to fall
  // back to when its picture could not be found.
  MaterialStyle style;
};

MaterialLook ClassifyMaterial(const TrackMaterial &material, std::uint32_t index, const MaterialStyle &style) {
  MaterialLook look;
  look.style = style;
  if (material.water || style.water) {
    look.color = ft_color{0.16f, 0.40f, 0.60f, 0.72f};
    return look;
  }

  bool known = false;
  look.color = SurfaceColor(material.surface, &known);
  if (known) return look;

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
  if (!api || !api->resolve_data_path) return {};
  char buffer[1024];
  api->resolve_data_path("GameData/Tracks", buffer, sizeof(buffer));
  std::error_code error;
  if (std::filesystem::is_directory(buffer, error)) return buffer;
  return {};
}

bool DescribeCampaign(const std::filesystem::path &relative, Campaign *campaign) {
  std::vector<std::string> parts;
  for (const std::filesystem::path &part : relative) {
    const std::string value = part.string();
    if (!value.empty() && value != ".") parts.push_back(value);
  }

  std::size_t first = 0;
  if (!parts.empty() && Lowered(parts[0]) == "campaigns") first = 1;
  if (first < parts.size() && Lowered(parts[first]) == "baseeditorsimple") return false;

  campaign->path = relative.generic_string();
  campaign->collection = first < parts.size() ? parts[first++] : "Tracks";

  if (campaign->collection == "Nations") {
    if (first < parts.size()) campaign->difficulty = parts[first];
  } else if (campaign->collection == "StarTrack") {
    if (first < parts.size()) campaign->environment = parts[first++];
    if (first < parts.size()) campaign->difficulty = parts[first];
  } else if (campaign->collection == "United") {
    if (first < parts.size()) campaign->mode = parts[first++];
    if (campaign->mode == "Race" && first < parts.size()) campaign->environment = parts[first++];
    if (first < parts.size()) campaign->difficulty = parts[first];
  } else if (first < parts.size()) {
    // A custom installation may contain additional collections. Treat the
    // remaining folder as a mode so those tracks stay reachable without
    // leaking the complete system path into the UI.
    campaign->mode = parts[first];
  }

  return true;
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
    std::filesystem::path relative = std::filesystem::relative(file.parent_path(), root, error);
    if (relative.empty() || relative == ".") relative = "Tracks";
    const std::string campaign_path = relative.generic_string();
    if (game->campaigns.empty() || game->campaigns.back().path != campaign_path) {
      Campaign campaign;
      if (!DescribeCampaign(relative, &campaign)) continue;
      game->campaigns.push_back(std::move(campaign));
    }

    std::string name = file.filename().string();
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos) name.resize(dot);

    TrackEntry entry;
    entry.path = file.string();
    entry.name = std::move(name);
    game->campaigns.back().tracks.push_back(std::move(entry));
  }

  game->selected_campaign = 0;
  for (int index = 0; index < static_cast<int>(game->campaigns.size()); ++index) {
    const Campaign &campaign = game->campaigns[static_cast<std::size_t>(index)];
    if (campaign.collection == "Nations" && campaign.difficulty == "White") {
      game->selected_campaign = index;
      break;
    }
  }

  std::size_t track_count = 0;
  for (const Campaign &campaign : game->campaigns) track_count += campaign.tracks.size();
  Log(game, FT_LOG_INFO, "Found %zu playable tracks in %zu campaigns under %s", track_count, game->campaigns.size(),
      game->tracks_root.c_str());
}

// --- loading -----------------------------------------------------------------

namespace {

// The time of day the map was saved with, which is what decides its sky. It is
// in the header's descriptor rather than anywhere the simulation reads, so it is
// taken straight out of the file: the header is a short piece of XML at the
// front, ahead of the compressed body.
std::string MoodOf(const std::byte *bytes, std::size_t size) {
  constexpr std::size_t kHeaderSearchLimit = 8192u;
  const char *text = reinterpret_cast<const char *>(bytes);
  const std::string_view head(text, std::min(size, kHeaderSearchLimit));
  const std::size_t at = head.find("mood=\"");
  if (at == std::string_view::npos) return {};
  const std::size_t start = at + 6u;
  const std::size_t end = head.find('"', start);
  if (end == std::string_view::npos) return {};
  return std::string(head.substr(start, end - start));
}

// Anything wider than any real track: the ground plane the environment is
// painted on, and the scenery on the skyline behind it.
constexpr float kSceneryFootprint = 1500.f;
// The sky dome is a sphere fifteen kilometres tall around the whole map. Nothing
// a track is built from is remotely that tall, so its height is what identifies
// it, and it has to be identified, because it carries no material and so has
// nothing else to recognise it by.
constexpr float kSkyDomeHeight = 5000.f;

bool IsSkyDome(const TrackMesh &mesh, const TrackInstance &instance) {
  // Cheap rejects first: this is asked of every instance on the track, and the
  // dome is two of them.
  if (mesh.vertices.empty() || mesh.vertices.size() > 4096u || !mesh.has_uv) return false;
  Aabb box;
  for (const TrackVertex &vertex : mesh.vertices) box.Add(TransformPoint(instance.transform, vertex.position));
  return box.Valid() && box.mx.y - box.mn.y > kSkyDomeHeight;
}

bool IsDistantScenery(const TrackMesh &mesh, const TrackInstance &instance) {
  Aabb box;
  for (const TrackVertex &vertex : mesh.vertices) box.Add(TransformPoint(instance.transform, vertex.position));
  if (!box.Valid()) return false;
  return box.mx.x - box.mn.x > kSceneryFootprint || box.mx.z - box.mn.z > kSceneryFootprint;
}

// The grass a stadium's field is covered in, which is not a texture but
// geometry: over every flat grass tile the block stands four shells, each a
// grid of upright cards half a metre tall carrying a strip of blades. The game
// draws them near the camera and fades them out with distance; there is nothing
// here to fade them with, so they are all drawn, all of the time. Across the
// campaign they are between two fifths and six sevenths of every triangle a
// level owns (on the first track, 1.25 million of grass against 300 thousand
// of everything else) and since the frame budget is sixty thousand spent
// nearest first, it is the grass around the car that spends it rather than the
// track.
//
// Nothing names them: the shells come off the same material as the flat ground
// under them, so what identifies them is their shape. A shell is a field of
// cards, every triangle standing on end, no taller than the grass, and spread
// across the footprint of a whole tile in both directions at once. A fence or a
// railing stands on end too, but it is a line rather than a field: narrow in
// one direction, which is what keeps it drawn.
constexpr float kGrassBladeHeight = 1.f;
constexpr float kGrassBladeFootprint = 8.f;
// How far from upright a card may lean and still count as one, as the sine of
// the angle: a quarter is about fourteen degrees. The shells are exactly
// vertical, and this is only slack for the arithmetic.
constexpr float kGrassBladeLean = 0.25f;

bool IsGrassBlades(const TrackMesh &mesh) {
  if (mesh.indices.size() < 3u * 64u) return false;

  Aabb box;
  for (const TrackVertex &vertex : mesh.vertices) box.Add(vertex.position);
  if (!box.Valid() || box.mx.y - box.mn.y > kGrassBladeHeight) return false;
  if (box.mx.x - box.mn.x < kGrassBladeFootprint || box.mx.z - box.mn.z < kGrassBladeFootprint) return false;

  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) return false;
    const ft_vec3 &a = mesh.vertices[i0].position;
    const ft_vec3 face = Cross(Sub(mesh.vertices[i1].position, a), Sub(mesh.vertices[i2].position, a));
    // Lying flat rather than standing on end, so this is a surface and not a
    // card: the ground of the tile itself, or a mesh that merely happens to be
    // thin.
    const float area = LengthSq(face);
    if (area <= 0.f || face.y * face.y > kGrassBladeLean * kGrassBladeLean * area) return false;
  }
  return true;
}

// Where a terrain modifier keeps its counterpart of a material: the same file
// name under the modifier's own folder. A material with nothing there simply
// keeps its own picture, which is what makes trying this cheap.
std::optional<std::string> TerrainModifierPath(const std::string &material_path) {
  const std::size_t slash = material_path.find_last_of("\\/");
  if (slash == std::string::npos) return std::nullopt;
  return "Stadium\\Media\\MaterialTerrainModifierDirt\\" + material_path.substr(slash + 1u);
}

struct BuildStats {
  std::size_t instances_drawn = 0;
  std::size_t instances_skipped = 0;
  std::size_t lod_skipped = 0;
  std::size_t grass_skipped = 0;
  std::size_t sky_instances = 0;
  std::size_t dirt_swapped = 0;
};

void AppendInstance(const TrackScene &scene, const TrackInstance &instance, std::uint32_t layer,
                    TextureAnimation animation, const MaterialStyle &style,
                    std::vector<Triangle> &out, Aabb &bounds) {
  if (instance.mesh >= scene.meshes.size() || instance.material >= scene.materials.size()) return;
  // Collision proxies, fence depth stand-ins and fake shadow skirts sit in the
  // same geometry as the surfaces they belong to. Drawing them puts a layer of
  // flat panels a millimetre off every wall on the track, which is most of what
  // reads as z-fighting.
  if (style.invisible) return;

  const TrackMesh &mesh = scene.meshes[instance.mesh];
  const TrackMaterial &material = scene.materials[instance.material];
  const MaterialLook look = ClassifyMaterial(material, instance.material, style);

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

    const std::uint32_t triangle_layer = layer;
    const TextureAnimation triangle_animation = animation;

    Triangle tri;
    tri.a = TransformPoint(instance.transform, v0.position);
    tri.b = TransformPoint(instance.transform, v1.position);
    tri.c = TransformPoint(instance.transform, v2.position);
    tri.two_sided = look.style.double_sided;
    tri.animation = triangle_animation;
    if (triangle_layer != kNoTextureLayer && (mesh.has_uv || look.style.world_uv)) {
      tri.layer = triangle_layer;
      if (look.style.world_uv) {
        // Terrain carries no useful coordinates of its own: it is textured by
        // where it stands, one tile of the picture every sixteen metres. Reading
        // its authored coordinates instead is what smeared a single texel across
        // a whole field of grass.
        tri.uv[0] = ft_vec2{tri.a.x * kWorldUvScale, tri.a.z * kWorldUvScale};
        tri.uv[1] = ft_vec2{tri.b.x * kWorldUvScale, tri.b.z * kWorldUvScale};
        tri.uv[2] = ft_vec2{tri.c.x * kWorldUvScale, tri.c.z * kWorldUvScale};
      } else {
        tri.uv[0] = v0.uv;
        tri.uv[1] = v1.uv;
        tri.uv[2] = v2.uv;
      }
    }

    const ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
    if (LengthSq(face) < 1e-12f) continue;

    if (mesh.has_normal) {
      // Authored normals say which way the surface faces no matter how the
      // indices are wound, so swapping the last two corners when the two
      // disagree is what decides the front face back-face culling keeps.
      const ft_vec3 authored =
          Normalize(TransformNormal(instance.transform, Add(Add(v0.normal, v1.normal), v2.normal)));
      if (Dot(face, authored) < 0.f) {
        std::swap(tri.b, tri.c);
        std::swap(tri.uv[1], tri.uv[2]);
      }
    }

    // On a textured surface the picture is the appearance, so the colour only
    // carries the alpha: multiplying it by a guess at the appearance would be
    // painting the same thing twice.
    ft_color base = tri.layer != kNoTextureLayer ? ft_color{1.f, 1.f, 1.f, look.color.a} : look.color;
    if (mesh.has_color) {
      // Authored vertex colour is what paints the stadium's own detail; it is
      // tinted rather than replaced so the surface still reads as its material.
      const ft_color &c = v0.color;
      base = MixColor(base, ft_color{base.r * c.r * 2.f, base.g * c.g * 2.f, base.b * c.b * 2.f, base.a}, 0.6f);
    }

    // Zero alpha is the pass's signal for an additive draw; see
    // data/shaders/primitive3d.frag.glsl. A glow carries no coverage of its
    // own -- it only adds light to whatever it is mounted on.
    // The start-light archive only places its glow face, so that layer also
    // carries the recovered opaque lens/housing texture. Other glow materials
    // retain the zero-alpha additive convention.
    if (look.style.additive && animation.kind != TextureAnimationKind::StartLights) base.a = 0.f;

    tri.color = PackColor(base);
    out.push_back(tri);

    bounds.Add(tri.a);
    bounds.Add(tri.b);
    bounds.Add(tri.c);
  }
}

// Everything the render scene could not give us. A track that decodes no
// authored visuals is still drivable, so its collision hull is drawn instead.
void AppendCollisionFallback(ft_game *game, ft_level *level) {
  if (!game->world) return;

  for (const auto &triangle : game->world->Collision()) {
    Triangle tri;
    tri.a = ToVec3(triangle.a);
    tri.b = ToVec3(triangle.b);
    tri.c = ToVec3(triangle.c);
    const ft_vec3 face = Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a));
    if (LengthSq(face) < 1e-12f) continue;
    const ft_vec3 normal = Normalize(face);
    const ft_color base =
        normal.y > 0.75f ? ft_color{0.36f, 0.38f, 0.42f, 1.f} : ft_color{0.52f, 0.40f, 0.38f, 1.f};
    tri.color = PackColor(base);
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
    Log(game, FT_LOG_ERROR, "Cannot open a track without the installed game data. %s", kGameDataInstallHint);
    return nullptr;
  }

  const std::vector<std::byte> bytes = ReadFileBytes(path);
  if (bytes.empty()) {
    Log(game, FT_LOG_ERROR, "Could not read the challenge file '%s'.", path);
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(game->mutex);
  if (!OpenSandbox(game, bytes.data(), bytes.size(), path)) return nullptr;

  auto *level = new ft_level();
  level->path = path;
  level->source = bytes;
  level->initial = game->world->Start();
  level->start = level->initial.View();
  level->name = game->world->MapName().empty() ? "TrackMania track" : game->world->MapName();

  // The car's own collision ellipsoids, so what is drawn is the shape the
  // simulation actually pushes around rather than a guess at one.
  level->car_shape = game->world->CarShape();

  // The installed packs, opened for what the simulation never reads: the
  // pictures.
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
  // the simulation. Its own scene is the same geometry, but it carries no
  // material paths and, on several environments, no materials at all: the
  // materials live in each tile's solid and are only reachable by reading them
  // (see tmnf_scene.cpp). Everything on screen therefore comes from here.
  TrackScene scene;
  const bool decoded = !EnvironmentPackName(level->start.mapEnvironment).empty() && game->packs_open.IsOpen() &&
                       BuildTrackScene(game, game->packs_open, bytes.data(), bytes.size(),
                                       EnvironmentPackName(level->start.mapEnvironment), &scene);
  level->checkpoints = scene.checkpoints;

  // The sky, which belongs to the time of day the map was saved with rather
  // than to anything in its geometry. Loaded before the materials so that its
  // page is in the array whether or not the track textured anything.
  const std::string environment = EnvironmentPackName(level->start.mapEnvironment);
  const std::string mood = MoodOf(bytes.data(), bytes.size());
  const std::optional<std::uint32_t> sky_layer =
      game->packs_open.IsOpen() ? game->textures.SkyLayer(game->packs_open, environment, mood) : std::nullopt;
  if (!sky_layer && !mood.empty()) {
    Log(game, FT_LOG_WARN, "No %s sky for the %s mood; the dome will be drawn flat.", environment.c_str(),
        mood.c_str());
  }

  // Each material's picture, decoded once and kept as a page of one array, and
  // beside it how the material's shader says the surface is drawn.
  static const MaterialStyle kDefaultStyle;
  std::vector<std::uint32_t> material_layers;
  std::vector<std::uint32_t> material_dirt_layers;
  std::vector<TextureAnimation> material_animations;
  std::vector<MaterialStyle> material_styles;
  if (decoded) {
    material_layers.assign(scene.materials.size(), kNoTextureLayer);
    material_dirt_layers.assign(scene.materials.size(), kNoTextureLayer);
    material_animations.assign(scene.materials.size(), TextureAnimation{});
    material_styles.assign(scene.materials.size(), MaterialStyle{});
    std::size_t textured = 0;
    std::optional<std::uint32_t> direction_sign_layer;
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      if (scene.materials[i].path.empty()) continue;
      material_styles[i] = game->textures.Style(game->packs_open, scene.materials[i].path);
      // Whether this surface reads the picture's alpha as opacity decides which
      // page it gets, because the same picture is a cut-out for one material
      // and carries specular strength for the next.
      const bool keep_alpha = material_styles[i].transparent && !material_styles[i].additive;
      if (const std::optional<std::uint32_t> layer =
              game->textures.Layer(game->packs_open, scene.materials[i].path, keep_alpha)) {
        material_layers[i] = *layer;
        ++textured;
      }
      if (const std::optional<std::string> dirt = TerrainModifierPath(scene.materials[i].path)) {
        if (const std::optional<std::uint32_t> layer = game->textures.Layer(game->packs_open, *dirt, keep_alpha))
          material_dirt_layers[i] = *layer;
      }

      const std::string lower = Lowered(scene.materials[i].path);
      if (lower.find("stadiumscreen2x1east.material.gbx") != std::string::npos ||
          lower.find("stadiumscreen2x1west.material.gbx") != std::string::npos ||
          lower.find("stadiumwarpscreen2x1east.material.gbx") != std::string::npos ||
          lower.find("stadiumwarpscreen2x1west.material.gbx") != std::string::npos) {
        if (!direction_sign_layer) direction_sign_layer = game->textures.DirectionSignLayer(game->packs_open);
        if (direction_sign_layer) material_layers[i] = *direction_sign_layer;
      }
      if (material_layers[i] != kNoTextureLayer)
        material_animations[i] = game->textures.Animation(material_layers[i]);
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
          "must be in data/games/tmnf/GameData beside data/games/tmnf/Packs. Until then the track is drawn in flat "
          "surface colours. %s",
          textured, named, kGameDataInstallHint);
    } else {
      Log(game, FT_LOG_INFO, "Textured %zu of %zu track materials.", textured, scene.materials.size());
    }
    // Which materials came back without a picture. A surface drawn in one flat
    // colour is always one of these, and the name is what says whether the
    // material was never found or its texture was.
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      if (scene.materials[i].path.empty() || material_layers[i] != kNoTextureLayer) continue;
      Log(game, FT_LOG_TRACE, "  no texture for material '%s'", scene.materials[i].path.c_str());
    }
  }

  BuildStats stats;
  enum : std::uint8_t { kUnknown, kYes, kNo };
  std::vector<std::uint8_t> grass_blades(scene.meshes.size(), kUnknown);
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
    // Decided once per mesh: one shell is stood up on every grass tile of the
    // track, so walking its triangles again for each placement would cost more
    // than dropping them saves.
    if (grass_blades[instance.mesh] == kUnknown)
      grass_blades[instance.mesh] = IsGrassBlades(scene.meshes[instance.mesh]) ? kYes : kNo;
    if (grass_blades[instance.mesh] == kYes) {
      ++stats.grass_skipped;
      continue;
    }

    ++stats.instances_drawn;

    std::uint32_t layer =
        instance.material < material_layers.size() ? material_layers[instance.material] : kNoTextureLayer;
    if (instance.terrain_dirt && instance.material < material_dirt_layers.size() &&
        material_dirt_layers[instance.material] != kNoTextureLayer) {
      layer = material_dirt_layers[instance.material];
      ++stats.dirt_swapped;
    }
    MaterialStyle style =
        instance.material < material_styles.size() ? material_styles[instance.material] : kDefaultStyle;
    TextureAnimation animation = instance.material < material_animations.size()
                                     ? material_animations[instance.material]
                                     : TextureAnimation{};

    // The game enables these flare passes only for the Night mood. Their
    // diffuse lamp faces remain in the scene for every mood, so suppressing the
    // extra daytime halo both matches the original and saves an additive draw.
    // Start lights are stateful race presentation rather than mood lighting;
    // keep their recovered face even if an archive happens to inherit a
    // night-only additive shader.
    if (style.night_only && Lowered(mood) != "night" &&
        animation.kind != TextureAnimationKind::StartLights)
      style.invisible = true;

    // The sky. It is the one thing in the scene with no material on it at all,
    // so nothing above could have found its picture; what it gets instead is the
    // mood's own, composed at load. Marked as a backdrop shell, and two-sided
    // because the only place it is ever looked at from is inside it.
    if (sky_layer && IsSkyDome(scene.meshes[instance.mesh], instance)) {
      layer = *sky_layer;
      style = MaterialStyle{};
      style.unlit = true;
      style.double_sided = true;
      ++stats.sky_instances;
    }

    // A stadium scene ends in a few kilometre-wide sky and ground planes. They
    // belong on screen, but not in the level's bounds: the engine frames its
    // camera on those, and a track that is really three hundred metres across
    // would otherwise open five kilometres away.
    //
    // The sky goes there whatever size its mesh is, because what makes a
    // backdrop is being behind everything rather than being wide.
    //
    // An additive glow is unlit too, but it is not a shell: the lamps on the
    // start gantry and the flares on the spot housings stand in the world, at a
    // place, and sending them to the backdrop draws them behind the very thing
    // they are lit on. Only a shell that is not adding light to something is a
    // backdrop.
    if ((style.unlit && !style.additive) || IsDistantScenery(scene.meshes[instance.mesh], instance)) {
      AppendInstance(scene, instance, layer, animation, style,
                     level->backdrop, backdrop_bounds);
    } else if (style.transparent || style.additive) {
      AppendInstance(scene, instance, layer, animation, style,
                     level->translucent, level->world_bounds);
    } else {
      const std::size_t before = level->track.size();
      AppendInstance(scene, instance, layer, animation, style,
                     level->track, level->world_bounds);
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
  if (!level->translucent.empty()) level->translucent_grid.Build(level->translucent, level->world_bounds);
  if (!level->backdrop.empty()) level->backdrop_grid.Build(level->backdrop, backdrop_bounds);

  // The engine frames its camera on these bounds and treats them as the ground
  // plane, so they are the extent of the blocks that make up the track rather
  // than of the countryside around them.
  const Aabb &framing = played_bounds.Valid() ? played_bounds : level->world_bounds;
  level->bounds = ft_rect{framing.mn.x, framing.mn.z, std::max(32.f, framing.mx.x - framing.mn.x),
                          std::max(32.f, framing.mx.z - framing.mn.z)};

  Log(game, FT_LOG_INFO,
      "Loaded '%s' (%s, %s): %u checkpoints, %zu track triangles, %zu blended, %zu backdrop triangles, %zu sky",
      level->name.c_str(), environment.c_str(), mood.empty() ? "no mood" : mood.c_str(), level->start.checkpointsTotal,
      level->track.size(), level->translucent.size(), level->backdrop.size(), stats.sky_instances);
  if (stats.lod_skipped) Log(game, FT_LOG_TRACE, "Skipped %zu coarse level-of-detail instances.", stats.lod_skipped);
  if (stats.grass_skipped) Log(game, FT_LOG_TRACE, "Skipped %zu shells of grass blades.", stats.grass_skipped);
  if (stats.dirt_swapped)
    Log(game, FT_LOG_TRACE, "Repainted %zu surfaces for the terrain they stand on.", stats.dirt_swapped);

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
