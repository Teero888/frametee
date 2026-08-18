// TrackMania Nations Forever, as a FrameTee game.
//
// The physics is not reimplemented here. ForeverValidator is a deterministic
// reconstruction of the original game's simulation and this module is only the
// adapter between its sandbox and the engine's ABI, which is why nothing in
// this directory ever patches the validator: everything below is built on its
// published API.
//
// Nothing in this header crosses the ABI. The engine only ever sees the opaque
// ft_game / ft_level / ft_world handles declared in <frametee/game_abi.h>; this
// is what the module's own translation units share with each other.

#ifndef TMNF_INTERNAL_H
#define TMNF_INTERNAL_H

#include <frametee/game_abi.h>

// The layouts a TMNF plugin is allowed to see live here, so this header and the
// one plugins compile against can never disagree about them.
#include <tmnf/tmnf_game.h>

#include <forevervalidator/camera.h>
#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include "tmnf_math.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tmnf {

using forevervalidator::AnalogInputState;
using forevervalidator::kAnalogInputScale;
using forevervalidator::OpenInstalledPackDirectory;
using forevervalidator::ReplayIdentity;
using forevervalidator::SimulationBackend;

namespace fv = forevervalidator;
namespace fve = forevervalidator::experimental;

// How far ahead the sandbox's control plan reaches. Rebuilding a plan is linear
// in the horizon, so this is a real trade between how long a run may be and how
// cheap an input edit is; three minutes covers every Nations track with room to
// spare and is adjustable in the editor's settings.
inline constexpr std::uint32_t kDefaultHorizonMs = 180000u;
inline constexpr std::uint32_t kHorizonGrowthMs = 120000u;
inline constexpr std::uint32_t kMinHorizonMs = 30000u;
inline constexpr std::uint32_t kMaxHorizonMs = 1800000u;

// The renderer's 3D stream is a fixed size and silently drops whatever does not
// fit, so the module keeps a budget of its own a comfortable margin below it and
// spends the difference on the car, the lines and the editor's own overlays.
// This is well past what a view of a decimated track normally needs; it exists
// so that a huge map degrades by losing its far side rather than by dropping to
// single figures of frames a second.
inline constexpr std::size_t kTriangleBudget = 380000u;
// Reserved off the top of it for the sky and the ground plane behind the track.
// A stadium's shell is a few thousand triangles and the track around it is
// hundreds of thousands, so spending what the track leaves over left the sky
// undrawn on every real map.
inline constexpr std::size_t kBackdropBudget = 30000u;

// --- input plans -------------------------------------------------------------

// The sandbox is driven by an event timeline rather than a per-tick array, and
// a captured state carries the plan it was produced with. A plan is therefore
// immutable once installed and shared by pointer between every world that was
// simulated under it, which is what makes the engine's constant snapshotting
// affordable.
struct InputPlan {
  // Authored input for ticks [0, ticks.size()).
  std::vector<TmnfInput> ticks;
  // What every later tick holds. Speculating that the current input keeps being
  // held is what makes prediction lines, which feed one constant record for
  // hundreds of ticks, cost a single plan rebuild instead of one per tick.
  TmnfInput tail{};

  const TmnfInput &At(std::size_t tick) const {
    return tick < ticks.size() ? ticks[tick] : tail;
  }
};


// `base` is whatever the sandbox seeded its own timeline with when the
// scenario loaded — for a canonical timeline that is the event which starts the
// race. Replacing the input list replaces all of it, so those events have to be
// carried into every plan or the race never begins and nothing that depends on
// it, from respawns to the finish line, ever fires.
std::vector<fve::PhysicsSandboxInputEvent> BuildInputEvents(const std::vector<fve::PhysicsSandboxInputEvent> &base,
                                                            const InputPlan &plan, std::uint32_t horizon_ms);

// Speed is reported in metres per second; the editor shows the game's own
// kilometres per hour.
inline float ToKmh(float metres_per_second) { return metres_per_second * 3.6f; }

// --- track geometry ----------------------------------------------------------

// A layer this large is never a real page of the texture array, and marks a
// triangle drawn with its colour alone.
inline constexpr std::uint32_t kNoTextureLayer = 0xFFFFFFFFu;

struct Triangle {
  ft_vec3 a, b, c;
  // The surface's own colour. On a textured triangle it is white and
  // only carries the alpha, because the picture is the appearance.
  std::uint32_t color = 0xFFFFFFFFu;
  ft_vec2 uv[3]{};
  std::uint32_t layer = kNoTextureLayer;
  // Looked at from both sides, so it survives the back-face cull whichever way
  // it is wound. Fences, banners and the sky are all sheets with nothing behind
  // them, and culling a sheet is the same as deleting it half the time.
  bool two_sided = false;
};

// Triangles are bucketed by centroid into columns of a flat grid; each bucket
// keeps the box that actually contains its triangles, so a bucket whose box is
// outside the frustum can be rejected wholesale instead of per triangle.
struct GridCell {
  Aabb bounds;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

struct TriangleGrid {
  float cell_size = 48.f;
  int dim_x = 0, dim_z = 0;
  float origin_x = 0.f, origin_z = 0.f;
  std::vector<GridCell> cells;

  // Reorders `triangles` in place so that every cell's members are contiguous.
  // Drawing then walks each visible cell as a straight run of memory instead of
  // chasing an index array through a sixty megabyte scatter, which at these
  // triangle counts is the difference between a frame and several.
  void Build(std::vector<Triangle> &triangles, const Aabb &bounds);
  void Clear();
};

// --- the installed packs -----------------------------------------------------

// One reference a GBX file makes to another file. `by_name` is clear on the
// reference a file makes to itself, which in a pack of hashed names is the only
// place its real name is written down.
struct GbxReference {
  std::string path;
  std::string name;
  bool by_name = true;
};

struct GbxFile {
  std::uint32_t class_id = 0u;
  std::vector<unsigned char> bytes;
  std::vector<GbxReference> references;
};

struct Pack;
} // namespace tmnf

class InstalledPackKeyCatalog;

namespace tmnf {

// The installed packs, opened for reading. This is the module's own window onto
// the game's data, beside the one the sandbox opens for the simulation: the
// sandbox reads the packs for physics and never keeps a texture.
class PackSet {
public:
  PackSet();
  ~PackSet();
  PackSet(const PackSet &) = delete;
  PackSet &operator=(const PackSet &) = delete;

  bool Open(const std::string &packs_dir, const std::vector<std::string> &pack_names);
  void Close();
  bool IsOpen() const { return !packs_.empty(); }

  bool Read(std::string_view plain_path, std::vector<unsigned char> *out) const;
  bool References(std::string_view plain_path, GbxFile *out) const;
  // The physical surface a material declares. The one thing about a material
  // the validator does decode, and the check that a material found by another
  // route is the one the scene meant.
  std::optional<std::uint8_t> MaterialSurface(std::string_view plain_path) const;
  std::vector<std::string> PathsOfClass(std::uint32_t class_id) const;
  // Every material in the packs, named as it names itself rather than as it is
  // filed. See tmnf_pack.cpp.
  const std::vector<std::string> &MaterialLogicalPaths() const;

private:
  bool OpenOne(const std::string &name);
  bool ReadFromGameData(const std::string &plain_path, std::vector<unsigned char> *out) const;

  std::string root_;
  // The installed game's data directory, where the files the packs only refer
  // to actually live.
  std::string data_root_;
  std::unique_ptr<InstalledPackKeyCatalog> keys_;
  std::vector<std::unique_ptr<Pack>> packs_;
  mutable std::optional<std::vector<std::string>> material_paths_;
  // Directory listings of the installed game, lowercased, so a reference that
  // spells a file differently than the disk does still finds it.
  mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> directories_;
};

// --- what a material binds where ---------------------------------------------

// One texture a material names, under the sampler it binds it to. The sampler
// is the only thing that says what the texture is *for*: "Diffuse" is the
// surface's own picture and everything beside it — "Normal", "Specular",
// "Occlusion", the environment cubes — modifies it. File names cannot be used
// for this, because plenty of them end in the same letters for other reasons.
struct MaterialTextureSlot {
  std::string sampler;
  std::string name;             // as the material spells it
  std::uint32_t node_index = 0; // where it sat in the file's reference table
  std::string path;             // resolved plain path of the .Texture.gbx
};

// Every texture a material names. See tmnf_material.cpp.
bool ReadMaterialTextures(const PackSet &packs, const std::string &material_path,
                          std::vector<MaterialTextureSlot> *out);
// The one of them that is the surface's own picture, by the sampler it is bound
// to rather than by what it is called.
std::optional<std::string> DiffuseTextureOf(const std::vector<MaterialTextureSlot> &slots);

// --- the track's materials ---------------------------------------------------

// How a material is meant to be drawn, as opposed to what picture is on it.
//
// TrackMania puts this in the shader a material is built from rather than in the
// material itself: "TDiff PX2 Trans 2Sided" is a transparent, two-sided surface
// and "Techno/Media/Material/Sky" is the sky, whatever texture either happens to
// name. The shader is referenced by path, so the path is what these are read
// from; see MaterialDrawStyle in tmnf_texture.cpp for the table.
//
// The vocabulary and the classifications follow GbxTools3D, which is a viewer
// for these same environments and has already worked out which shader means
// what: https://github.com/BigBang1112/gbx-tools-3d
struct MaterialStyle {
  // A shell drawn behind everything rather than a surface standing in the
  // world: the sky, and the glows and flares that go with it. These are routed
  // into the backdrop bucket, which is drawn out of a reserve of its own.
  bool unlit = false;
  // Looked at from both sides, so the winding says nothing about which faces
  // are worth drawing.
  bool double_sided = false;
  // Keeps the alpha its texture carries. Everything else is forced opaque,
  // because a stray alpha channel on a surface that was never meant to use one
  // is what turns a wall into a window.
  bool transparent = false;
  // Never drawn: collision proxies, fence depth stand-ins, fake shadow skirts.
  // These are in the geometry and were never meant to be seen.
  bool invisible = false;
  // Textured by where a surface is in the world rather than by its authored
  // coordinates. Terrain is laid out this way — a grass tile carries no useful
  // coordinates of its own and is expected to take them from the ground plane.
  bool world_uv = false;
  // Adds its light to what is behind it: glows, lit signs, spot flares.
  bool additive = false;
  bool water = false;
};

// World-space texturing divides the coordinate by this, so one tile of the
// picture covers sixteen metres of ground. It is the game's own constant.
inline constexpr float kWorldUvScale = 1.f / 16.f;

struct TrackMaterial {
  // Where the material was resolved from. Empty when it could not be named,
  // which is what makes a surface fall back to a flat colour.
  std::string path;
  // The physical surface it declares. This is also what the sandbox reports for
  // the same material, and so the check that the two agree about which material
  // is which.
  std::uint8_t surface = 0u;
  bool water = false;
};

// A vertex as the game authored it. The coordinate is the surface's own; the
// colour is the shading painted into the mesh.
struct TrackVertex {
  ft_vec3 position{};
  ft_vec3 normal{0.f, 1.f, 0.f};
  ft_vec2 uv{};
  ft_color color{1.f, 1.f, 1.f, 1.f};
};

struct TrackMesh {
  std::vector<TrackVertex> vertices;
  std::vector<std::uint32_t> indices;
  bool has_normal = false;
  bool has_uv = false;
  bool has_color = false;
};

// A tile's placement: the basis of its rotation and scale, and where it sits.
struct TrackTransform {
  ft_vec3 basis_x{1.f, 0.f, 0.f};
  ft_vec3 basis_y{0.f, 1.f, 0.f};
  ft_vec3 basis_z{0.f, 0.f, 1.f};
  ft_vec3 translation{};
};

// What a piece of the scene is for: what to draw, what to frame the camera on,
// and what was never meant to be seen.
enum TrackPurpose : std::uint8_t {
  TRACK_PURPOSE_BLOCK = 0, // an authored track block
  TRACK_PURPOSE_SCENERY,   // decoration, terrain, the stadium around it
  TRACK_PURPOSE_HIDDEN,    // clips, triggers, editor helpers
};

struct TrackInstance {
  std::uint32_t mesh = 0u;
  std::uint32_t material = 0u;
  TrackTransform transform;
  TrackPurpose purpose = TRACK_PURPOSE_SCENERY;
  std::uint32_t lod = 0u;
  bool visible = true;
};

// A track's geometry and materials, decoded from the installed game. Meshes are
// shared between the instances that place them, exactly as the game shares a
// block between the places it is laid down.
struct TrackScene {
  std::vector<TrackMesh> meshes;
  std::vector<TrackMaterial> materials;
  std::vector<TrackInstance> instances;
};

// Decodes the track a second time to learn what its materials are called, which
// is the only way to find the picture on a surface; see tmnf_scene.cpp.
bool BuildTrackScene(ft_game *game, const PackSet &packs, const void *challenge_bytes,
                     std::size_t challenge_size, const std::string &pack_name, TrackScene *out);

// The pack an environment's blocks live in.
std::string EnvironmentPackName(fv::MapEnvironment environment);

// --- textures ----------------------------------------------------------------

// Every layer of a texture array is the same size, and the game's textures run
// from 32 to 2048 pixels, so one size has to be chosen for all of them. It is
// picked at upload time from what the track actually decoded rather than fixed
// here, because the two things that go wrong pull in opposite directions: a
// page smaller than the textures blurs a road into the flat colour this whole
// path exists to replace, and a page larger than the memory available fails to
// allocate. See TextureLibrary::Upload.
//
// The ceiling on the page. Stadium's own surfaces are authored at 1024 and its
// advertising boards at 2048; past 1024 the difference is a logo's edge on a
// hoarding, and the cost is four times the memory for it.
inline constexpr std::uint32_t kMaxTexturePageSize = 1024u;
// Below this a track stops reading as its own textures at all, so a level that
// cannot afford the pages loses layers rather than resolution.
inline constexpr std::uint32_t kMinTexturePageSize = 256u;
// What the array as a whole may cost. A stadium track decodes about forty
// textures and an island one about a hundred and fifty, so this is what decides
// whether they land at 1024 or at 512.
inline constexpr std::size_t kTextureMemoryBudget = 320u * 1024u * 1024u;
// The ceiling is the engine's array limit rather than anything about the game;
// a full stadium track uses about a hundred and fifty.
inline constexpr std::size_t kMaxTextureLayers = 512u;

// The pictures the track and the car are painted with, as one array texture.
// Layers are keyed on the image file, so the many materials that share a
// texture share a layer.
class TextureLibrary {
public:
  TextureLibrary();
  ~TextureLibrary();
  TextureLibrary(const TextureLibrary &) = delete;
  TextureLibrary &operator=(const TextureLibrary &) = delete;

  // The layer a material's surface texture was decoded into, decoding it now if
  // this is the first time it has been asked for. Empty when the material names
  // no surface texture, or names one this module cannot read.
  std::optional<std::uint32_t> Layer(const PackSet &packs, const std::string &material_path);
  // How the material is meant to be drawn, read from the shader it is built
  // from. Cached alongside the layer, because both come from the same file.
  MaterialStyle Style(const PackSet &packs, const std::string &material_path);
  // The sky, composed from the environment's mood.
  //
  // The dome in a track's scene carries no material at all, so there is nothing
  // to look a picture up by. The picture is not in the packs either: it is a
  // property of the time of day the map was saved with, and lives beside the
  // rest of that mood's assets in the installed game. Two of them, in fact —
  // a ceiling over the whole sky and a panorama that fades in towards the
  // horizon — which is why this composes rather than just loads.
  std::optional<std::uint32_t> SkyLayer(const PackSet &packs, const std::string &environment, const std::string &mood);
  // Hands the decoded layers to the engine as one array texture. Called once,
  // after everything a level needs has been decoded.
  bool Upload(ft_game *game);
  void Destroy(ft_game *game);
  void Clear();

  ft_texture *Texture() const { return texture_; }
  std::size_t LayerCount() const { return layers_.size(); }

private:
  static std::optional<std::string> DiffuseImagePath(const PackSet &packs, const std::string &material_path);
  std::uint32_t ChoosePageSize() const;

  // A decoded image at the size it was authored. Resampling to the array's page
  // is left until upload, when what the whole track needs is known: deciding it
  // one texture at a time is what forced every page to a guess made before any
  // of them had been read.
  struct Page {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    // Whether any material that paints with this image is actually transparent.
    // TrackMania stores specular strength in the alpha channel of plenty of
    // ordinary opaque textures, so sampling alpha everywhere turns walls into
    // windows and takes most of the car with it. A page nothing draws
    // transparently is forced opaque at upload.
    bool alpha_used = false;
  };

  std::vector<Page> layers_;
  std::unordered_map<std::string, std::optional<std::uint32_t>> by_material_;
  std::unordered_map<std::string, std::optional<std::uint32_t>> by_image_;
  std::unordered_map<std::string, MaterialStyle> style_by_material_;
  ft_texture *texture_ = nullptr;
  std::size_t uploaded_ = 0u;
  std::uint32_t page_size_ = 0u;
};

// --- the wheels turning ------------------------------------------------------

// The editor draws several worlds at once — the run and its prediction ghosts —
// and each turns its own wheels.
inline constexpr std::size_t kMaxSpinnyWorlds = 16u;

// How far a car's wheels have rolled.
//
// The simulation reports how fast the car is going and never how far a wheel
// has turned, so the angle is integrated from the speed. That makes it
// presentation state rather than part of the run, and it has to behave when the
// timeline is not simply playing forwards: scrubbing, stepping back, jumping to
// a checkpoint. The rule is that a step small enough to be playback advances
// the angle and anything larger just re-anchors, because catching up across a
// ten second jump would spin the wheel through a thousand revolutions nobody
// asked to see.
struct WheelSpin {
  float angle = 0.f;
  std::uint64_t time_ms = 0u;
  bool anchored = false;

  // The angle at `now`, given the speed the car is doing there.
  float Advance(std::uint64_t now, float metres_per_second, float wheel_radius) {
    // Anything past this is a seek rather than a frame.
    constexpr std::uint64_t kContinuousMs = 250u;
    if (anchored && now > time_ms && now - time_ms <= kContinuousMs && wheel_radius > 1e-3f) {
      const float seconds = static_cast<float>(now - time_ms) * 0.001f;
      angle += metres_per_second * seconds / wheel_radius;
      // Kept in range so the float never loses the precision a slow roll needs.
      angle = std::fmod(angle, 2.f * kPi);
    }
    time_ms = now;
    anchored = true;
    return angle;
  }
};

// --- the vehicle model -------------------------------------------------------

// The car's parts. The four wheels are in the order the simulation reports
// contact and sliding in, so a wheel's state indexes straight into these.
enum VehiclePart : std::uint8_t {
  VEHICLE_PART_WHEEL_FL = 0,
  VEHICLE_PART_WHEEL_FR,
  VEHICLE_PART_WHEEL_RR,
  VEHICLE_PART_WHEEL_RL,
  VEHICLE_PART_BODY,
  VEHICLE_PART_COUNT,
};

inline bool IsWheelPart(std::uint8_t part) { return part < VEHICLE_PART_BODY; }

struct VehicleFace {
  ft_vec3 a, b, c;
  // The plane the face lies in, corrected against the authored normals at
  // decode so a mirrored part is still wound outwards.
  ft_vec3 normal;
  ft_color color{1.f, 1.f, 1.f, 1.f};
  ft_vec2 uv[3]{};
  std::uint32_t layer = kNoTextureLayer;
  std::uint8_t part = VEHICLE_PART_BODY;
};

// The car as authored, in vehicle space. Wheel faces are stored relative to
// their own hub so the front pair can be turned with the steering.
struct VehicleModel {
  std::vector<VehicleFace> faces;
  ft_vec3 hub[VEHICLE_PART_COUNT]{};
  // The pack this was decoded from, so switching between tracks that use the
  // same car does not decode it again.
  std::string pack;
  bool loaded = false;
};

// Decodes the vehicle's authored model out of an installed pack. Returns false
// when the pack cannot be read or the model cannot be decoded, and the caller
// falls back to the modelled car.
bool LoadVehicleModel(ft_game *game, PackSet &packs, TextureLibrary &textures, const std::string &pack_name,
                      VehicleModel *out);

// The pack a vehicle's assets live in. Each car ships in the pack of the
// environment it was designed for, which is not always the one the track is in:
// a Stadium car can be driven on an Island track.
std::string VehiclePackName(fv::VehicleModel vehicle);

// The colour TrackMania's physical surface ids read as on screen. Shared so the
// track and the car are coloured by the same table.
ft_color SurfaceColor(std::uint8_t surface, bool *known);

// --- levels, worlds and the game ---------------------------------------------

struct TrackEntry {
  std::string path;
  std::string name;
  ft_texture *texture = nullptr;
  void *thumbnail = nullptr; // ImTextureRef*, opaque here so this header stays free of ImGui
  bool thumbnail_tried = false;
  bool visible_this_frame = false;
};

struct Campaign {
  std::string name;
  std::vector<TrackEntry> tracks;
};

struct Settings {
  bool draw_background = true;
  // Safe because every authored mesh carries normals and winding is corrected
  // against them at load, so a culled triangle really is one facing away.
  bool backface_cull = true;
  // Off by default: it is an inspection aid, not a way to drive.
  bool draw_collision = false;
  int horizon_seconds = static_cast<int>(kDefaultHorizonMs / 1000u);
};

} // namespace tmnf

// The three handles the engine knows about. They live at global scope because
// the ABI declares them there.
struct ft_level {
  std::string name;
  ft_rect bounds{};
  tmnf::Aabb world_bounds;
  tmnf::fve::PhysicsSandboxStateView start{};
  std::optional<tmnf::fve::PhysicsSandboxState> initial;

  // Foreground track geometry and the stadium shell behind it, kept apart so
  // the shell can be skipped without walking it every frame.
  std::vector<tmnf::Triangle> track;
  std::vector<tmnf::Triangle> backdrop;
  tmnf::TriangleGrid track_grid;
  tmnf::TriangleGrid backdrop_grid;

  // The car's own collision ellipsoids, in vehicle space. Drawing these is what
  // makes the car on screen the same shape the simulation is pushing around.
  std::vector<tmnf::fve::PhysicsSandboxEllipsoid> car_shape;
};

// ft_world is defined by <tmnf/tmnf_game.h>, included above.

struct ft_game {
  const ft_engine_api *engine = nullptr;
  bool headless = false;

  // One sandbox serves every world: a world is a captured state, and stepping
  // one restores it, advances a tick and captures the result.
  std::optional<tmnf::fve::PhysicsSandbox> sandbox;
  std::mutex mutex;
  std::uint32_t horizon_ms = tmnf::kDefaultHorizonMs;
  // The sandbox's own seeded events, kept so that rewriting the input timeline
  // never drops them.
  std::vector<tmnf::fve::PhysicsSandboxInputEvent> base_events;

  std::string packs;
  std::string status;
  ft_level *level = nullptr;

  tmnf::Settings settings;

  // The original game's own race camera, decoded from the installed packs.
  std::optional<tmnf::fv::camera::RaceCameraEnvironment> race_cameras;
  std::unique_ptr<tmnf::fv::camera::RaceCameraSession> race_session;
  std::uint64_t race_session_time_ms = 0u;
  bool race_session_started = false;

  // The car's authored model, decoded from the pack the vehicle comes from.
  tmnf::VehicleModel vehicle;

  // How far each world's wheels have turned. The simulation reports a speed but
  // never an angle, so it is integrated here — which means it belongs to the
  // presentation and not to the run, and a timeline that jumps must not try to
  // catch up across the gap. See tmnf::WheelSpin.
  tmnf::WheelSpin wheel_spin[tmnf::kMaxSpinnyWorlds];

  // The installed packs, and the pictures pulled out of them. Both belong to
  // the level rather than to the game, but a car is shared between tracks that
  // use it, so both outlive one.
  tmnf::PackSet packs_open;
  tmnf::TextureLibrary textures;

  // The track browser on the start screen.
  std::string tracks_root;
  std::vector<tmnf::Campaign> campaigns;
  int selected_campaign = 0;
  bool scanned = false;
};

namespace tmnf {

// --- logging -----------------------------------------------------------------

// The engine's log takes a plain message, so formatting is the module's job.
void Log(const ft_game *game, ft_log_level level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// --- tmnf_sandbox.cpp --------------------------------------------------------

std::string ResolvePacks(const ft_engine_api *api);
bool OpenSandbox(ft_game *game, const void *bytes, std::size_t size, const char *identity);
void CloseSandbox(ft_game *game);

ft_world *WorldCreate(ft_game *game, const ft_world_desc *desc);
void WorldDestroy(ft_game *game, ft_world *world);
void WorldCopy(ft_game *game, ft_world *dst, const ft_world *src);
void WorldStep(ft_game *game, ft_world *world, const void *inputs, std::uint32_t player_count);

// --- tmnf_level.cpp ----------------------------------------------------------

std::vector<std::byte> ReadFileBytes(const char *path);
std::string ResolveTracks(const ft_engine_api *api);
void ScanTracks(ft_game *game);

ft_level *LevelLoad(ft_game *game, const char *path);
void LevelDestroy(ft_game *game, ft_level *level);

// Shortest hit along a segment against the track, as a fraction in [0, 1].
// The race cameras use it to keep the view from sinking into the scenery.
bool SegmentHit(const ft_level *level, ft_vec3 start, ft_vec3 end, float *out_fraction);

// --- tmnf_render.cpp ---------------------------------------------------------

void Render(ft_game *game, const ft_render_frame *frame);

// --- tmnf_car.cpp ------------------------------------------------------------

struct CarPose;
void DrawCar(ft_game *game, const ft_render_frame *frame, const CarPose &pose);

// --- tmnf_camera.cpp ---------------------------------------------------------

extern const ft_camera_mode kCameraModes[];
extern const std::uint32_t kCameraModeCount;

void CameraReset(ft_game *game);
bool CameraUpdate(ft_game *game, const ft_camera_frame *frame, ft_camera *inout);

// --- tmnf_ui.cpp -------------------------------------------------------------

void UiAttach(const ft_engine_api *engine);
void Ui(ft_game *game, const ft_ui_frame *frame);
void ReleaseThumbnails(ft_game *game);

// --- shared helpers ----------------------------------------------------------

inline ft_vec3 ToVec3(const fv::Vector3 &v) { return ft_vec3{v.x, v.y, v.z}; }

inline Quat CarRotation(const fve::PhysicsSandboxCarState &car) {
  return Quat{car.rotationX, car.rotationY, car.rotationZ, car.rotationW};
}

// Where the car is at a sub-tick moment, interpolated exactly the way the
// renderer interpolates so a directed camera does not stutter against a
// smoothly drawn car.
struct CarPose {
  ft_vec3 position{};
  Quat rotation{};
  ft_vec3 forward{0.f, 0.f, 1.f};
  ft_vec3 right{1.f, 0.f, 0.f};
  ft_vec3 up{0.f, 1.f, 0.f};
  ft_vec3 velocity{};
};

CarPose InterpolateCar(const ft_world *previous, const ft_world *current, float alpha);

} // namespace tmnf

#endif // TMNF_INTERNAL_H
