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

// A TMNF tick is 10 ms, so the engine runs this game at 100 ticks per second.
inline constexpr std::uint32_t kTickMs = 10u;
// The countdown the original game plays before the clock starts. The sandbox
// simulates it for us and reports tick 0 at the moment the race begins, so it
// never shows up in the timeline.
inline constexpr std::uint32_t kPrestartMs = 2600u;

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
inline constexpr std::size_t kTriangleBudget = 200000u;

// --- input record ------------------------------------------------------------

enum InputField { FIELD_ACCELERATE = 0, FIELD_BRAKE, FIELD_STEER, FIELD_RESPAWN, FIELD_COUNT };

// Laid out so the record is eight bytes with natural alignment; the engine
// stores arrays of these in snippets and project files.
struct TmnfInput {
  std::uint8_t accelerate = 0;
  std::uint8_t brake = 0;
  std::uint8_t respawn = 0;
  std::uint8_t reserved = 0;
  // Full lock either way, matching the game's own analog range.
  std::int32_t steer = 0;

  bool operator==(const TmnfInput &o) const {
    return accelerate == o.accelerate && brake == o.brake && respawn == o.respawn && steer == o.steer;
  }
  bool operator!=(const TmnfInput &o) const { return !(*this == o); }
};

static_assert(sizeof(TmnfInput) == 8, "the input record must stay a fixed eight bytes");

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

using InputPlanPtr = std::shared_ptr<const InputPlan>;

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

// The single directional light the whole game is lit by. The renderer's 3D
// path is flat unlit vertex colour, so every surface bakes its shade against
// this at load and pays nothing per frame.
inline const ft_vec3 kSunDirection = Normalize(ft_vec3{0.38f, 0.86f, 0.34f});

// --- track geometry ----------------------------------------------------------

// A layer this large is never a real page of the texture array, and marks a
// triangle drawn with its colour alone.
inline constexpr std::uint32_t kNoTextureLayer = 0xFFFFFFFFu;

struct Triangle {
  ft_vec3 a, b, c;
  // The colour is the light baked in at load; on a textured triangle it is what
  // multiplies the texture, which is how one flat sun still shades a track that
  // the renderer lights not at all.
  std::uint32_t color = 0xFFFFFFFFu;
  ft_vec2 uv[3]{};
  std::uint32_t layer = kNoTextureLayer;
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

// --- the track's materials ---------------------------------------------------

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
// from 32 to 1024 pixels. The page has to be big enough that a road surface
// still reads as concrete a few metres ahead of the car: at 256 the mip chain
// had turned every surface back into the flat colour this whole path exists to
// replace. At 512 a full track costs about forty megabytes of pages.
inline constexpr std::uint32_t kTexturePageSize = 512u;
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
  // Hands the decoded layers to the engine as one array texture. Called once,
  // after everything a level needs has been decoded.
  bool Upload(ft_game *game);
  void Destroy(ft_game *game);
  void Clear();

  ft_texture *Texture() const { return texture_; }
  std::size_t LayerCount() const { return layers_.size(); }

private:
  static std::optional<std::string> DiffuseImagePath(const PackSet &packs, const std::string &material_path);

  std::vector<std::vector<std::uint8_t>> layers_;
  std::unordered_map<std::string, std::optional<std::uint32_t>> by_material_;
  std::unordered_map<std::string, std::optional<std::uint32_t>> by_image_;
  ft_texture *texture_ = nullptr;
  std::size_t uploaded_ = 0u;
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

struct ft_world {
  std::optional<tmnf::fve::PhysicsSandboxState> state;
  tmnf::fve::PhysicsSandboxStateView view{};
  // The plan this world's state was produced under. Copying a world only
  // touches a refcount.
  tmnf::InputPlanPtr plan;
  // Which editor world this belongs to; -1 marks a scratch world the engine
  // simulates to answer a question rather than to show.
  std::int32_t index = -1;
  // The editor track this world's single player is recorded on, so authored
  // input can be read ahead instead of discovered one tick at a time.
  std::int32_t track = -1;
};

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

  // The original game's own race cameras, decoded from the installed packs.
  std::optional<tmnf::fv::camera::RaceCameraEnvironment> race_cameras;
  std::unique_ptr<tmnf::fv::camera::RaceCameraSession> race_session;
  std::uint32_t race_session_profile = 0xFFFFFFFFu;
  std::uint64_t race_session_time_ms = 0u;
  bool race_session_started = false;

  // The car's authored model, decoded from the pack the vehicle comes from.
  tmnf::VehicleModel vehicle;

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
