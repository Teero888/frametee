// TrackMania Nations Forever .Replay.Gbx export.
//
// GBX.NET deliberately does not advertise CGameCtnReplayRecord writing yet,
// but its chunk definitions and CSceneVehicleCar.Sample encoder document the
// TMF archive precisely. This is the small, TMF-only subset FrameTee needs:
// one embedded challenge, one ghost per selected timeline world, the input
// events, and the fixed-step state stream used to draw the ghost in game.

#include "tmnf_internal.h"

#include "format/compression/lzo1x.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace tmnf {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kReplayRootClass = 0x2407e000u;
constexpr std::uint32_t kReplayHeaderBasic = 0x03093000u;
constexpr std::uint32_t kReplayHeaderXml = 0x03093001u;
constexpr std::uint32_t kReplayChallenge = 0x03093002u;
constexpr std::uint32_t kReplayUnknown = 0x03093007u;
constexpr std::uint32_t kReplayGhosts = 0x03093014u;
constexpr std::uint32_t kReplayClip = 0x03093015u;
constexpr std::uint32_t kGhostClass = 0x03092000u;
constexpr std::uint32_t kGhostState = 0x0303f005u;
constexpr std::uint32_t kFacade = 0xfacade01u;
constexpr std::uint32_t kSkip = 0x534b4950u;
constexpr std::uint32_t kNull = 0xffffffffu;
constexpr std::uint32_t kTranslatedName = 0x80000000u;
constexpr std::uint32_t kLocalName = 0x40000000u;
constexpr std::uint32_t kInputClockOffset = 100000u;
constexpr std::uint32_t kSamplePeriodMs = 100u;
constexpr std::uint32_t kSamplePeriodTicks = kSamplePeriodMs / kTickMs;
constexpr std::uint32_t kSampleBytes = 61u;
constexpr float kPi = 3.14159265358979323846f;

// An input event names its action by indexing the table the input archive
// writes ahead of the events, so the two have to agree; kInputActions below is
// that table.
enum : std::uint8_t {
  kActionAccelerate = 0u,
  kActionBrake,
  kActionSteer,
  kActionRespawn,
  kActionRaceRunning,
  kActionFinishLine,
};

constexpr std::array<std::string_view, 6> kInputActions = {"Accelerate",         "Brake",      "Steer",
                                                           "Respawn",            "_FakeIsRaceRunning",
                                                           "_FakeFinishLine"};

const ft_exporter_desc kReplayExporter = {
    "replay",
    "TrackMania replay",
    "Replay.Gbx",
    "TrackMania replay",
};

class Writer {
public:
  void U8(std::uint8_t value) { bytes_.push_back(value); }

  void U16(std::uint16_t value) {
    U8(static_cast<std::uint8_t>(value));
    U8(static_cast<std::uint8_t>(value >> 8u));
  }

  void U32(std::uint32_t value) {
    U8(static_cast<std::uint8_t>(value));
    U8(static_cast<std::uint8_t>(value >> 8u));
    U8(static_cast<std::uint8_t>(value >> 16u));
    U8(static_cast<std::uint8_t>(value >> 24u));
  }

  void F32(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t), "TMF stores 32-bit IEEE floats");
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bits);
  }

  void String(std::string_view value) {
    U32(static_cast<std::uint32_t>(value.size()));
    Raw(value.data(), value.size());
  }

  void Raw(const void *data, std::size_t size) {
    if (size == 0u) return;
    const auto *first = static_cast<const std::uint8_t *>(data);
    bytes_.insert(bytes_.end(), first, first + size);
  }

  void Raw(const Bytes &bytes) { Raw(bytes.data(), bytes.size()); }

  const Bytes &Data() const { return bytes_; }
  Bytes Take() { return std::move(bytes_); }
  std::size_t Size() const { return bytes_.size(); }

private:
  Bytes bytes_;
};

class IdWriter {
public:
  void Name(Writer &writer, std::string_view name, std::uint32_t kind = kLocalName) {
    if (!version_written_) {
      writer.U32(3u);
      version_written_ = true;
    }
    NameWithoutVersion(writer, name, kind);
  }

  void NameWithoutVersion(Writer &writer, std::string_view name, std::uint32_t kind = kLocalName) {
    if (name.empty()) {
      writer.U32(kNull);
      return;
    }
    // Repeating an inline name is legal. It keeps this writer independent of
    // a dictionary and makes every emitted chunk easy to inspect.
    writer.U32(kind);
    writer.String(name);
  }

  void Ident(Writer &writer, std::string_view id, std::string_view collection, std::string_view author,
             std::uint32_t id_kind = kLocalName) {
    Name(writer, id, id_kind);
    NameWithoutVersion(writer, collection, kLocalName);
    NameWithoutVersion(writer, author, kTranslatedName);
  }

private:
  bool version_written_ = false;
};

struct MapMetadata {
  std::string uid;
  std::string environment;
  std::string author;
};

struct InputEvent {
  std::uint32_t time_ms = 0u;
  std::uint8_t action = 0u;
  std::uint32_t data = 0u;
};

struct ExportGhost {
  std::uint32_t world_index = 0u;
  std::int32_t track = -1;
  std::string nickname;
  std::vector<sim::StateView> samples;
  std::vector<InputEvent> inputs;
  std::vector<std::uint32_t> checkpoints;
  std::uint32_t duration_ms = 0u;
  std::uint32_t race_time_ms = 0u;
  std::uint32_t respawns = 0u;
  std::uint32_t stunt_score = 0u;
  std::uint32_t laps = 1u;
  bool finished = false;
  // The livery this driver wears, by the archive's name. Empty means the car
  // the game draws by default.
  std::string skin;
  fv::VehicleModel vehicle = fv::VehicleModel::Unknown;
};

bool FitsU32(std::size_t size) { return size <= std::numeric_limits<std::uint32_t>::max(); }

template <typename T>
T ClampRound(float value, T low, T high) {
  if (!std::isfinite(value)) return low;
  const double rounded = std::round(static_cast<double>(value));
  return static_cast<T>(std::clamp(rounded, static_cast<double>(low), static_cast<double>(high)));
}

std::string XmlUnescape(std::string text) {
  struct Replacement {
    const char *encoded;
    const char *plain;
  };
  constexpr Replacement replacements[] = {
      {"&quot;", "\""},
      {"&apos;", "'"},
      {"&lt;", "<"},
      {"&gt;", ">"},
      {"&amp;", "&"},
  };
  for (const Replacement &replacement : replacements) {
    std::size_t at = 0u;
    while ((at = text.find(replacement.encoded, at)) != std::string::npos) {
      text.replace(at, std::strlen(replacement.encoded), replacement.plain);
      at += std::strlen(replacement.plain);
    }
  }
  return text;
}

std::string XmlEscape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '&':
      escaped += "&amp;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

std::string Attribute(std::string_view source, std::string_view element, std::string_view attribute) {
  const std::size_t begin = source.find(element);
  if (begin == std::string_view::npos) return {};
  const std::size_t end = source.find('>', begin);
  if (end == std::string_view::npos) return {};
  const std::string needle = std::string(attribute) + "=\"";
  const std::size_t value = source.find(needle, begin);
  if (value == std::string_view::npos || value >= end) return {};
  const std::size_t first = value + needle.size();
  const std::size_t last = source.find('"', first);
  if (last == std::string_view::npos || last > end) return {};
  return XmlUnescape(std::string(source.substr(first, last - first)));
}

MapMetadata ReadMapMetadata(const ft_level *level) {
  MapMetadata metadata;
  if (level && !level->source.empty()) {
    const std::string_view source(reinterpret_cast<const char *>(level->source.data()), level->source.size());
    metadata.uid = Attribute(source, "<ident", "uid");
    metadata.author = Attribute(source, "<ident", "author");
    metadata.environment = Attribute(source, "<desc", "envir");
  }
  if (metadata.environment.empty() && level) metadata.environment = EnvironmentPackName(level->start.mapEnvironment);
  if (metadata.author.empty()) metadata.author = "Nadeo";
  return metadata;
}

std::string VehicleName(fv::VehicleModel vehicle) {
  switch (vehicle) {
  case fv::VehicleModel::SnowCar:
    return "SnowCar";
  case fv::VehicleModel::DesertCar:
    return "DesertCar";
  case fv::VehicleModel::RallyCar:
    return "RallyCar";
  case fv::VehicleModel::IslandCar:
    return "IslandCar";
  case fv::VehicleModel::CoastCar:
    return "CoastCar";
  case fv::VehicleModel::BayCar:
    return "BayCar";
  case fv::VehicleModel::StadiumCar:
    return "StadiumCar";
  case fv::VehicleModel::Unknown:
    return "StadiumCar";
  }
  return "StadiumCar";
}

bool RequestIncludes(const ft_export_request *request, std::int32_t track) {
  if (!request->players) return true;
  return std::find(request->players, request->players + request->player_count, track) !=
         request->players + request->player_count;
}

std::string NicknameFor(ft_game *game, std::int32_t track) {
  const PlayerProfile profile = ProfileForTrack(game, track);
  if (profile.name[0] != '\0') return profile.name;

  ft_player_setup setup{};
  setup.struct_size = sizeof(setup);
  if (game && game->engine && game->engine->get_player_setup && game->engine->get_player_setup(track, &setup) &&
      setup.track_name && setup.track_name[0] != '\0') {
    return setup.track_name;
  }
  return "FrameTee";
}

std::uint32_t PackVector(const fv::Vector3 &value) {
  const float magnitude = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  std::int16_t encoded_magnitude = std::numeric_limits<std::int16_t>::min();
  std::int8_t heading = 0;
  std::int8_t pitch = 0;
  if (std::isfinite(magnitude) && magnitude >= 1e-6f) {
    encoded_magnitude = ClampRound<std::int16_t>(std::log(magnitude) * 1000.f,
                                                 std::numeric_limits<std::int16_t>::min() + 1,
                                                 std::numeric_limits<std::int16_t>::max());
    heading = ClampRound<std::int8_t>(std::atan2(value.y, value.x) * 127.f / kPi, -128, 127);
    const float z = std::clamp(value.z / magnitude, -1.f, 1.f);
    pitch = ClampRound<std::int8_t>(std::asin(z) * 127.f / (kPi * 0.5f), -128, 127);
  }
  return static_cast<std::uint16_t>(encoded_magnitude) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(heading)) << 16u) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(pitch)) << 24u);
}

void WriteQuat6(Writer &writer, float x, float y, float z, float w) {
  const float norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!std::isfinite(norm) || norm < 1e-8f) {
    x = y = z = 0.f;
    w = 1.f;
  } else {
    x /= norm;
    y /= norm;
    z /= norm;
    w /= norm;
  }

  w = std::clamp(w, -1.f, 1.f);
  const float angle = std::acos(w);
  const float sin_angle = std::sin(angle);
  float axis_x = 1.f, axis_y = 0.f, axis_z = 0.f;
  if (std::abs(sin_angle) > 1e-5f) {
    axis_x = x / sin_angle;
    axis_y = y / sin_angle;
    axis_z = z / sin_angle;
  }
  writer.U16(ClampRound<std::uint16_t>(angle * 65535.f / kPi, 0u, 65535u));
  const std::int16_t heading =
      ClampRound<std::int16_t>(std::atan2(axis_y, axis_x) * 32767.f / kPi, -32768, 32767);
  const std::int16_t pitch = ClampRound<std::int16_t>(
      std::asin(std::clamp(axis_z, -1.f, 1.f)) * 32767.f / (kPi * 0.5f), -32768, 32767);
  writer.U16(static_cast<std::uint16_t>(heading));
  writer.U16(static_cast<std::uint16_t>(pitch));
}

// A wheel's dampen length rides in a byte spanning [-2, 2] metres.
std::uint8_t PackDamperLength(float metres) {
  return ClampRound<std::uint8_t>((metres + 2.f) * 255.f / 4.f, 0u, 255u);
}

std::uint8_t SurfaceByte(std::uint16_t surface) {
  return surface <= 255u ? static_cast<std::uint8_t>(surface) : static_cast<std::uint8_t>(kSurfaceAsphalt);
}

// A wheel's angle rides in a u16 that spans 512 turns of pi before it wraps.
constexpr float kWheelAngleRange = 512.f * kPi;

std::uint16_t PackWheelAngle(float angle) {
  if (!std::isfinite(angle)) return 0u;
  float wrapped = std::fmod(angle, kWheelAngleRange);
  if (wrapped < 0.f) wrapped += kWheelAngleRange;
  return ClampRound<std::uint16_t>(wrapped * 65535.f / kWheelAngleRange, 0u, 65535u);
}

void WriteSample(Writer &writer, const sim::StateView &state) {
  const std::size_t begin = writer.Size();
  const auto &car = state.car;
  writer.F32(car.position.x);
  writer.F32(car.position.y);
  writer.F32(car.position.z);
  WriteQuat6(writer, car.rotationX, car.rotationY, car.rotationZ, car.rotationW);
  writer.U32(PackVector(car.linearSpeed));
  writer.U32(PackVector(car.angularSpeed));

  writer.U16(ClampRound<std::uint16_t>((car.signedSpeed + 1000.f) * 65535.f / 11000.f, 0u, 65535u));
  writer.U16(ClampRound<std::uint16_t>((car.localSpeed.x + 1000.f) * 65535.f / 2000.f, 0u, 65535u));
  writer.U16(ClampRound<std::uint16_t>(car.rpm * 65535.f / 30000.f, 0u, 65535u));
  // Each wheel rolls on its own angle, so the outer wheel of a turn runs ahead
  // of the inner one exactly as it does in a recorded replay.
  for (std::size_t wheel = 0u; wheel < 4u; ++wheel)
    writer.U16(PackWheelAngle(car.wheelSpinAngle[wheel]));

  writer.U8(ClampRound<std::uint8_t>((state.steering + 1.f) * 127.5f, 0u, 255u));
  writer.U8(ClampRound<std::uint8_t>(state.accelerate * 255.f, 0u, 255u));
  writer.U8(ClampRound<std::uint8_t>(state.brake * 255.f, 0u, 255u));
  writer.U8(0u);
  writer.U8(0u);   // version 8's U12
  writer.U8(128u); // neutral signed controls
  writer.U8(128u);
  writer.U8(ClampRound<std::uint8_t>(car.turboBoostFactor * 255.f, 0u, 255u));
  // The front wheel's own steer angle, which the simulation already carries
  // the sign and the speed-damped magnitude of. Deriving it from the steering
  // axis instead points the wheels out of the corner and swings them further
  // than the car ever does.
  const float front_wheel_angle = std::clamp(car.wheelSteerAngle[0], -kPi, kPi);
  writer.U8(ClampRound<std::uint8_t>((front_wheel_angle + kPi) * 255.f / (2.f * kPi), 0u, 255u));

  // Suspension travel, one wheel at a time. Without it the body tilts on its
  // rotation alone and the wheels stay pinned to their arches.
  for (std::size_t wheel = 0u; wheel < 4u; ++wheel) {
    writer.U8(PackDamperLength(car.wheelDamperAbsorb[wheel]));
    writer.U8(SurfaceByte(car.wheelSurface[wheel]));
  }
  writer.U8(0u);
  std::uint8_t front_flags = 0u;
  if (car.wheelSliding[0]) front_flags |= 0x40u;
  if (car.wheelContact[0]) front_flags |= 0x80u;
  writer.U8(front_flags);
  std::uint8_t other_flags = 0u;
  if (car.wheelSliding[1]) other_flags |= 0x01u;
  if (car.wheelContact[1]) other_flags |= 0x02u;
  if (car.wheelSliding[2]) other_flags |= 0x04u;
  if (car.wheelContact[2]) other_flags |= 0x08u;
  if (car.wheelSliding[3]) other_flags |= 0x10u;
  if (car.wheelContact[3]) other_flags |= 0x20u;
  writer.U8(other_flags);
  const bool dirt = std::any_of(car.wheelSurface.begin(), car.wheelSurface.end(), [](std::uint16_t surface) {
    return surface == kSurfaceDirt || surface == kSurfaceGrass || surface == kSurfaceWetGrass;
  });
  writer.U8(dirt ? 255u : 0u);

  (void)begin;
  // CSceneVehicleCar sample version 9 is always exactly 61 bytes.
  if (writer.Size() - begin != kSampleBytes) std::abort();
}

bool ZlibCompress(const Bytes &source, Bytes *out) {
  if (!out || source.size() > std::numeric_limits<uLong>::max()) return false;
  const uLong source_size = static_cast<uLong>(source.size());
  uLongf output_size = compressBound(source_size);
  out->assign(static_cast<std::size_t>(output_size), 0u);
  const int result = compress2(out->data(), &output_size, source.data(), source_size, Z_BEST_COMPRESSION);
  if (result != Z_OK) {
    out->clear();
    return false;
  }
  out->resize(static_cast<std::size_t>(output_size));
  return true;
}

bool LzoCompress(const Bytes &source, Bytes *out) {
  if (!out) return false;
  // The standard LZO1X worst-case bound. The validator's compressor writes to
  // a caller-owned buffer just like miniLZO.
  const std::size_t extra = source.size() / 16u + 67u;
  if (source.size() > std::numeric_limits<std::size_t>::max() - extra) return false;
  out->assign(source.size() + extra, 0u);
  std::size_t output_size = out->size();
  Lzo1xDictionary dictionary;
  if (lzo1x_1_compress(source.data(), source.size(), out->data(), &output_size, dictionary) != LZO_E_OK ||
      output_size > out->size()) {
    out->clear();
    return false;
  }
  out->resize(output_size);
  return true;
}

void Wrapped(Writer &writer, std::uint32_t chunk, const Writer &payload) {
  writer.U32(chunk);
  writer.U32(kSkip);
  writer.U32(static_cast<std::uint32_t>(payload.Size()));
  writer.Raw(payload.Data());
}

bool CollectGhost(ft_game *game, const ft_export_request *request, const ft_timeline_world_info &world,
                  std::int32_t track, ExportGhost *out, std::uint64_t completed_before, std::uint64_t total_work) {
  if (!game || !request || !out) return false;
  ExportGhost ghost;
  ghost.world_index = static_cast<std::uint32_t>(world.world_index);
  ghost.track = track;
  ghost.nickname = NicknameFor(game, track);
  ghost.skin = ProfileForTrack(game, track).skin;

  const std::int64_t span = static_cast<std::int64_t>(request->end_tick) - request->start_tick;
  constexpr std::uint64_t kMaximumDurationMs =
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) - kInputClockOffset;
  if (span < 0 || static_cast<std::uint64_t>(span) * kTickMs > kMaximumDurationMs) {
    Log(game, FT_LOG_ERROR, "The requested replay range is too long for TMNF.");
    return false;
  }
  ghost.duration_ms = static_cast<std::uint32_t>(span) * kTickMs;

  // The race turns on at the replay's own time zero. Both the start reset and
  // every respawn hang off this edge, so a ghost without it neither starts nor
  // respawns when the game replays it.
  ghost.inputs.push_back({kInputClockOffset, kActionRaceRunning, 1u});

  TmnfInput previous_input{};
  std::uint32_t previous_checkpoints = 0u;
  bool have_state = false;
  sim::StateView last_state{};
  for (std::int64_t relative = 0; relative <= span; ++relative) {
    const std::int64_t global64 = static_cast<std::int64_t>(request->start_tick) + relative;
    if (global64 < std::numeric_limits<std::int32_t>::min() ||
        global64 > std::numeric_limits<std::int32_t>::max()) {
      Log(game, FT_LOG_ERROR, "The requested replay range is outside the timeline clock.");
      return false;
    }

    const ft_world *current_world = nullptr;
    if (!game->engine->timeline_world_pair(ghost.world_index, static_cast<std::int32_t>(global64), nullptr,
                                           &current_world) ||
        !current_world) {
      Log(game, FT_LOG_ERROR, "Could not sample timeline world %u at tick %d.", ghost.world_index,
          static_cast<std::int32_t>(global64));
      return false;
    }
    last_state = current_world->view;
    if (!have_state) {
      previous_checkpoints = last_state.checkpointsCollected;
      ghost.vehicle = last_state.vehicleModel;
      have_state = true;
    } else if (last_state.checkpointsCollected > previous_checkpoints) {
      for (std::uint32_t checkpoint = previous_checkpoints; checkpoint < last_state.checkpointsCollected; ++checkpoint)
        ghost.checkpoints.push_back(static_cast<std::uint32_t>(last_state.timeMs));
      previous_checkpoints = last_state.checkpointsCollected;
    }
    if (relative % kSamplePeriodTicks == 0) ghost.samples.push_back(last_state);

    // State at end_tick has already consumed the input at end_tick - 1.
    if (relative < span) {
      TmnfInput input{};
      if (global64 >= world.start_offset) {
        const std::int64_t local64 = global64 - world.start_offset;
        if (local64 > std::numeric_limits<std::int32_t>::max() ||
            !game->engine->get_player_input(track, static_cast<std::int32_t>(local64), &input)) {
          Log(game, FT_LOG_ERROR, "Could not read input track %d at tick %d.", track,
              static_cast<std::int32_t>(local64));
          return false;
        }
      }
      // The game applies every event stamped at or before the time the tick it
      // is advancing to, and the input at `relative` is consumed by the tick
      // that advances to `relative + 1`. Stamping an event a tick ahead of the
      // state it was read beside is what keeps it out of the preceding tick.
      const std::uint32_t event_time = kInputClockOffset + static_cast<std::uint32_t>(relative + 1) * kTickMs;
      if (input.accelerate != previous_input.accelerate)
        ghost.inputs.push_back({event_time, kActionAccelerate, input.accelerate ? 1u : 0u});
      if (input.brake != previous_input.brake)
        ghost.inputs.push_back({event_time, kActionBrake, input.brake ? 1u : 0u});
      if (input.steer != previous_input.steer) {
        // Steering rides on a signed 24-bit field the game negates as it reads
        // it, so the two's complement of the analog position goes on the wire.
        const std::uint32_t encoded = static_cast<std::uint32_t>(-static_cast<std::int64_t>(input.steer)) & 0x00ffffffu;
        ghost.inputs.push_back({event_time, kActionSteer, encoded});
      }
      // Respawn is counted once per tick it is held rather than once per press,
      // which is how the simulation reads it, and the replay carries only
      // events. Holding the key therefore has to be spelled out one press per
      // tick, with a single release once it comes back up.
      if (input.respawn)
        ghost.inputs.push_back({event_time, kActionRespawn, 1u});
      else if (previous_input.respawn)
        ghost.inputs.push_back({event_time, kActionRespawn, 0u});
      previous_input = input;
    }

    if (request->progress && (relative % 100 == 0 || relative == span)) {
      const float fraction = total_work == 0u
                                 ? 0.8f
                                 : 0.8f * static_cast<float>(completed_before + relative + 1u) /
                                       static_cast<float>(total_work);
      request->progress(request->progress_user, fraction, "Sampling TrackMania replay");
    }
  }

  if (!have_state) return false;
  // The uniform archive layout carries a per-sample size only when there are
  // at least two samples. Duplicate a very short segment's endpoint so readers
  // still get the canonical 61-byte fixed-step form.
  while (ghost.samples.size() < 2u)
    ghost.samples.push_back(last_state);
  ghost.respawns = last_state.respawnCount;
  ghost.stunt_score = last_state.stuntsScore.value_or(0u);
  ghost.laps = last_state.totalLaps > 0u ? last_state.totalLaps : 1u;

  // A run that crossed the line ends there. The event clock stops at the
  // finish, the finish itself is both the last input event and the last
  // checkpoint time, and only then is the replay something the game will offer
  // to validate. State samples still run past it, which is what a recorded
  // replay does: the car rolls on after the line.
  ghost.finished = last_state.finishTimeMs.has_value();
  ghost.race_time_ms = last_state.finishTimeMs.value_or(ghost.duration_ms);
  if (ghost.finished) {
    ghost.duration_ms = ghost.race_time_ms;
    const std::uint32_t finish_time = kInputClockOffset + ghost.race_time_ms;
    ghost.inputs.erase(std::remove_if(ghost.inputs.begin(), ghost.inputs.end(),
                                      [finish_time](const InputEvent &event) {
                                        return event.time_ms > finish_time;
                                      }),
                       ghost.inputs.end());
    ghost.inputs.push_back({finish_time, kActionFinishLine, 1u});
    ghost.checkpoints.push_back(ghost.race_time_ms);
  }
  *out = std::move(ghost);
  return true;
}

bool CollectGhosts(ft_game *game, const ft_export_request *request, std::vector<ExportGhost> *out) {
  if (!game || !request || !out || !game->engine || !game->engine->timeline_world_count ||
      !game->engine->timeline_world_info || !game->engine->timeline_world_pair ||
      !game->engine->timeline_player_track || !game->engine->get_player_input) {
    return false;
  }

  struct Selection {
    ft_timeline_world_info world{};
    std::int32_t track = -1;
  };
  std::vector<Selection> selected;
  const std::uint32_t world_count = game->engine->timeline_world_count();
  for (std::uint32_t world_index = 0u; world_index < world_count; ++world_index) {
    ft_timeline_world_info world{};
    world.struct_size = sizeof(world);
    if (!game->engine->timeline_world_info(world_index, &world)) continue;
    for (std::uint32_t local = 0u; local < world.player_count; ++local) {
      const std::int32_t track = game->engine->timeline_player_track(world_index, local);
      if (track >= 0 && RequestIncludes(request, track)) selected.push_back({world, track});
    }
  }
  if (selected.empty()) {
    Log(game, FT_LOG_ERROR, "Replay export has no selected players.");
    return false;
  }
  if (selected.size() > 256u) {
    Log(game, FT_LOG_ERROR, "Replay export supports at most 256 selected ghosts.");
    return false;
  }

  const std::uint64_t ticks_per_ghost =
      static_cast<std::uint64_t>(static_cast<std::int64_t>(request->end_tick) - request->start_tick + 1);
  const std::uint64_t total_work = ticks_per_ghost * selected.size();
  out->reserve(selected.size());
  for (std::size_t index = 0u; index < selected.size(); ++index) {
    ExportGhost ghost;
    if (!CollectGhost(game, request, selected[index].world, selected[index].track, &ghost,
                      ticks_per_ghost * index, total_work)) {
      return false;
    }
    out->push_back(std::move(ghost));
  }
  return true;
}

bool WriteGhostState(Writer &writer, const ExportGhost &ghost) {
  Writer state;
  state.U32(0x0a02b000u); // CSceneVehicleCar
  state.U32(1u);          // fixed time step
  state.U32(0u);          // encoding mode
  state.U32(kSamplePeriodMs);
  state.U32(9u); // SVehicleSimpleState_ReplayAfter081205
  if (ghost.samples.size() > std::numeric_limits<std::uint32_t>::max() / kSampleBytes) return false;
  state.U32(static_cast<std::uint32_t>(ghost.samples.size()) * kSampleBytes);
  for (const sim::StateView &sample : ghost.samples)
    WriteSample(state, sample);
  state.U32(static_cast<std::uint32_t>(ghost.samples.size()));
  state.U32(0u); // first sample offset
  state.U32(kSampleBytes);

  Bytes compressed;
  if (!FitsU32(state.Size()) || !ZlibCompress(state.Data(), &compressed) || !FitsU32(compressed.size())) return false;
  writer.U32(kGhostState);
  writer.U32(static_cast<std::uint32_t>(state.Size()));
  writer.U32(static_cast<std::uint32_t>(compressed.size()));
  writer.Raw(compressed);
  return true;
}

bool WriteGhost(Writer &writer, IdWriter &ids, const ExportGhost &ghost, const MapMetadata &map,
                std::size_t ghost_index) {
  if (!WriteGhostState(writer, ghost) || !FitsU32(ghost.inputs.size()) || !FitsU32(ghost.checkpoints.size()))
    return false;

  Writer payload;
  payload.U32(ghost.race_time_ms);
  Wrapped(writer, 0x03092005u, payload);
  payload = Writer{};
  payload.U32(ghost.respawns);
  Wrapped(writer, 0x03092008u, payload);
  payload = Writer{};
  // The light trail colour every real TMF ghost carries. Nothing in FrameTee
  // paints one, so the ghost gets the game's own default.
  payload.F32(1.f);
  payload.F32(0.f);
  payload.F32(0.f);
  Wrapped(writer, 0x03092009u, payload);
  payload = Writer{};
  payload.U32(ghost.stunt_score);
  Wrapped(writer, 0x0309200au, payload);
  payload = Writer{};
  payload.U32(static_cast<std::uint32_t>(ghost.checkpoints.size()));
  for (std::uint32_t time : ghost.checkpoints) {
    payload.U32(time);
    payload.U32(0u);
  }
  Wrapped(writer, 0x0309200bu, payload);

  writer.U32(0x0309200cu);
  writer.U32(0u);
  writer.U32(0x0309200eu);
  ids.Name(writer, "FrameTee-" + std::to_string(ghost.track) + "-" + std::to_string(ghost_index), kTranslatedName);
  writer.U32(0x0309200fu);
  writer.String("FrameTee");
  writer.U32(0x03092010u);
  ids.Name(writer, map.uid, kTranslatedName);

  // The old security key: a word and a 128-bit signature the game stamps on a
  // ghost it recorded itself. FrameTee cannot produce one, so it stays zero and
  // the header advertises the replay as not validable.
  writer.U32(0x03092012u);
  writer.U32(0u);
  for (int word = 0; word < 4; ++word)
    writer.U32(0u);

  payload = Writer{};
  payload.U32(0u);
  payload.U32(0u);
  Wrapped(writer, 0x03092013u, payload);
  payload = Writer{};
  payload.U32(7u);
  Wrapped(writer, 0x03092014u, payload);

  writer.U32(0x03092015u);
  ids.Name(writer, ghost.nickname, kTranslatedName);

  // The livery the ghost wears, named the way the game names it: a pack
  // descriptor pointing at the archive beside its own game data, and the
  // matching flag for the driver's avatar. A ghost with no livery chosen
  // carries no descriptor at all and the game draws the stock car.
  const std::string car = VehicleSkinFolder(ghost.vehicle);
  const bool has_skin = !ghost.skin.empty() && !car.empty();
  payload = Writer{};
  payload.U32(has_skin ? 1u : 0u);
  if (has_skin) {
    payload.U8(1u); // descriptor version: TMF carries no checksum
    payload.String("Skins\\Vehicles\\" + car + "\\" + ghost.skin + ".zip");
    payload.String(""); // no locator: the file is installed rather than fetched
  }
  payload.String(ghost.nickname);
  payload.String(has_skin ? "Skins\\Avatars\\Flags\\" + ghost.skin + ".dds" : "");
  Wrapped(writer, 0x03092017u, payload);

  writer.U32(0x03092018u);
  ids.Ident(writer, VehicleName(ghost.vehicle), "Vehicles", "Nadeo");

  writer.U32(0x03092019u);
  writer.U32(ghost.duration_ms);
  if (ghost.duration_ms != 0u) {
    writer.U32(0u); // CInputEventsStore version
    writer.U32(static_cast<std::uint32_t>(kInputActions.size()));
    for (std::string_view action : kInputActions)
      ids.Name(writer, action);
    writer.U32(static_cast<std::uint32_t>(ghost.inputs.size()));
    writer.U32(0u); // CountLimit
    for (const InputEvent &event : ghost.inputs) {
      writer.U32(event.time_ms);
      writer.U8(event.action);
      writer.U32(event.data);
    }
    writer.String("2.11.26");
    writer.U32(0u); // executable checksum
    writer.U32(0u); // OS kind
    writer.U32(0u); // CPU kind
    // The shape a recorded replay carries: the race's own settings, where `ct`
    // is how many checkpoint times the ghost holds.
    writer.String("<id>Unassigned</id><laps>" + std::to_string(ghost.laps) + "</laps><ct>" +
                  std::to_string(ghost.checkpoints.size()) + "</ct>");
    writer.U32(0u); // validation seed
  }
  writer.U32(kFacade);
  return true;
}

bool MakeBody(const ft_level *level, const MapMetadata &map, const std::vector<ExportGhost> &ghosts, Bytes *out) {
  if (!level || !out || level->source.empty() || !FitsU32(level->source.size()) ||
      ghosts.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  Writer body;
  body.U32(kReplayChallenge);
  body.U32(static_cast<std::uint32_t>(level->source.size()));
  body.Raw(level->source.data(), level->source.size());
  Writer unknown;
  unknown.U32(kNull);
  Wrapped(body, kReplayUnknown, unknown);

  body.U32(kReplayGhosts);
  body.U32(10u); // deprecated node-array archive version
  body.U32(static_cast<std::uint32_t>(ghosts.size()));
  IdWriter ids;
  for (std::size_t index = 0u; index < ghosts.size(); ++index) {
    body.U32(static_cast<std::uint32_t>(index + 1u));
    body.U32(kGhostClass);
    if (!WriteGhost(body, ids, ghosts[index], map, index)) return false;
  }
  body.U32(0u);
  body.U32(0u); // empty SOldShowTime array
  body.U32(kReplayClip);
  body.U32(kNull);
  body.U32(kFacade);
  *out = body.Take();
  return FitsU32(out->size());
}

Bytes MakeHeaderBasic(const MapMetadata &map, const ExportGhost &ghost) {
  Writer header;
  IdWriter ids;
  header.U32(7u);
  ids.Ident(header, map.uid, map.environment, map.author, kTranslatedName);
  header.U32(ghost.race_time_ms);
  header.String(ghost.nickname);
  header.String("FrameTee");
  return header.Take();
}

Bytes MakeHeaderXml(const MapMetadata &map, const ExportGhost &ghost) {
  const std::string xml = "<header type=\"replay\" version=\"TMr.7\" exever=\"2.11.26\"><challenge uid=\"" +
                          XmlEscape(map.uid) + "\"/><times best=\"" + std::to_string(ghost.race_time_ms) +
                          "\" respawns=\"" + std::to_string(ghost.respawns) + "\" stuntscore=\"" +
                          std::to_string(ghost.stunt_score) + "\" validable=\"" + (ghost.finished ? "1" : "0") +
                          "\"/></header>";
  Writer header;
  header.String(xml);
  return header.Take();
}

bool MakeReplay(const ft_level *level, const MapMetadata &map, const std::vector<ExportGhost> &ghosts, Bytes *out) {
  if (!level || !out || ghosts.empty()) return false;
  Bytes body;
  Bytes compressed_body;
  if (!MakeBody(level, map, ghosts, &body) || !LzoCompress(body, &compressed_body) || !FitsU32(compressed_body.size()))
    return false;

  const Bytes basic = MakeHeaderBasic(map, ghosts.front());
  const Bytes xml = MakeHeaderXml(map, ghosts.front());
  const std::size_t header_size = 4u + 16u + basic.size() + xml.size();
  if (!FitsU32(header_size) || !FitsU32(body.size()) || ghosts.size() >= std::numeric_limits<std::uint32_t>::max())
    return false;

  Writer file;
  file.U8('G');
  file.U8('B');
  file.U8('X');
  file.U16(6u);
  file.U8('B'); // binary
  file.U8('U'); // uncompressed header
  file.U8('C'); // compressed body
  file.U8('R'); // reference table present
  file.U32(kReplayRootClass);
  file.U32(static_cast<std::uint32_t>(header_size));
  file.U32(2u);
  file.U32(kReplayHeaderBasic);
  file.U32(static_cast<std::uint32_t>(basic.size()));
  file.U32(kReplayHeaderXml);
  file.U32(static_cast<std::uint32_t>(xml.size()) | 0x80000000u);
  file.Raw(basic);
  file.Raw(xml);
  file.U32(static_cast<std::uint32_t>(ghosts.size() + 1u));
  file.U32(0u); // no external nodes
  file.U32(static_cast<std::uint32_t>(body.size()));
  file.U32(static_cast<std::uint32_t>(compressed_body.size()));
  file.Raw(compressed_body);
  *out = file.Take();
  return true;
}

} // namespace

std::uint32_t ExporterCount(ft_game *) { return 1u; }

const ft_exporter_desc *ExporterDesc(ft_game *, std::uint32_t index) {
  return index == 0u ? &kReplayExporter : nullptr;
}

bool ExportRun(ft_game *game, std::uint32_t index, const ft_export_request *request) {
  if (index != 0u || !game || !request || !request->path || request->end_tick < request->start_tick ||
      !game->level) {
    return false;
  }
  if (game->level->source.empty()) {
    Log(game, FT_LOG_ERROR, "The loaded challenge has no source bytes to embed in the replay.");
    return false;
  }

  const MapMetadata map = ReadMapMetadata(game->level);
  if (map.uid.empty()) {
    Log(game, FT_LOG_ERROR, "Could not read the challenge UID required by a Replay.Gbx file.");
    return false;
  }

  try {
    std::vector<ExportGhost> ghosts;
    if (!CollectGhosts(game, request, &ghosts)) return false;
    if (request->progress) request->progress(request->progress_user, 0.85f, "Compressing TrackMania replay");

    Bytes replay;
    if (!MakeReplay(game->level, map, ghosts, &replay)) {
      Log(game, FT_LOG_ERROR, "Could not serialize the TrackMania replay archive.");
      return false;
    }
    std::ofstream file(request->path, std::ios::binary | std::ios::trunc);
    if (file) file.write(reinterpret_cast<const char *>(replay.data()), static_cast<std::streamsize>(replay.size()));
    file.close();
    if (!file) {
      Log(game, FT_LOG_ERROR, "Could not write replay '%s'.", request->path);
      return false;
    }
    if (request->progress) request->progress(request->progress_user, 1.f, "TrackMania replay complete");
    Log(game, FT_LOG_INFO, "Exported %zu ghost%s to '%s'.", ghosts.size(), ghosts.size() == 1u ? "" : "s",
        request->path);
    return true;
  } catch (const std::bad_alloc &) {
    Log(game, FT_LOG_ERROR, "Not enough memory to export the TrackMania replay.");
    return false;
  }
}

} // namespace tmnf
