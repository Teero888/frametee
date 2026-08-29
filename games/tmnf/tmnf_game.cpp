// The module vtable: what the engine sees of TrackMania Nations Forever.
//
// Everything substantial lives next door, the sandbox in tmnf_sandbox.cpp, the
// track in tmnf_level.cpp, the drawing in tmnf_render.cpp, the view in
// tmnf_camera.cpp and the start screen in tmnf_ui.cpp. What is left here is the
// description of the game itself: its inputs, the properties it exposes for
// inspection, its settings, and the thin forwarding that binds the rest to the
// ABI.

#include "tmnf_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace tmnf {

void Log(const ft_game *game, ft_log_level level, const char *fmt, ...) {
  if (!game || !game->engine || !game->engine->log) return;
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  game->engine->log(level, "TMNF", buffer);
}

namespace {

// --- input schema ------------------------------------------------------------

const ft_input_field kInputFields[FIELD_COUNT] = {
    {"accelerate", "Accelerate", "Throttle", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f,
     nullptr, 0, {0.40f, 1.00f, 0.50f, 1.f}},
    {"brake", "Brake", "Brake and reverse", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f, nullptr,
     0, {1.00f, 0.45f, 0.40f, 1.f}},
    {"steer", "Steer", "Full lock is 65536 either way, exactly as the game stores it", FT_INPUT_INT,
     FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X, -65536, 65536, 0, 0.f, 0.f, 0.f, nullptr, 0,
     {0.45f, 0.75f, 1.00f, 1.f}},
    // Respawn acts on the tick it is pressed rather than while held, which is
    // what the latched and trigger flags tell the timeline to draw and to
    // clear after every tick.
    {"respawn", "Respawn", "Return to the last checkpoint", FT_INPUT_BOOL,
     FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_LATCHED | FT_INPUT_FLAG_TRIGGER, 0, 1, 0, 0.f, 0.f, 0.f, nullptr, 0,
     {1.00f, 0.85f, 0.35f, 1.f}},
};

const ft_input_control kInputControls[] = {
    {"accelerate", "Accelerate", "Throttle", "TMNF", "UpArrow", FIELD_ACCELERATE, 1, 0, nullptr},
    {"accelerate_alt", "Accelerate (alternate)", "Throttle", "TMNF", "W", FIELD_ACCELERATE, 1, 0, nullptr},
    {"brake", "Brake", "Brake and reverse", "TMNF", "DownArrow", FIELD_BRAKE, 1, 0, nullptr},
    {"brake_alt", "Brake (alternate)", "Brake and reverse", "TMNF", "S", FIELD_BRAKE, 1, 0, nullptr},
    {"steer_left", "Steer left", "Full lock left", "TMNF", "LeftArrow", FIELD_STEER, -65536, FT_CONTROL_ADD, nullptr},
    {"steer_left_alt", "Steer left (alternate)", "Full lock left", "TMNF", "A", FIELD_STEER, -65536, FT_CONTROL_ADD,
     nullptr},
    {"steer_right", "Steer right", "Full lock right", "TMNF", "RightArrow", FIELD_STEER, 65536, FT_CONTROL_ADD,
     nullptr},
    {"steer_right_alt", "Steer right (alternate)", "Full lock right", "TMNF", "D", FIELD_STEER, 65536, FT_CONTROL_ADD,
     nullptr},
    {"respawn", "Respawn", "Return to the last checkpoint", "TMNF", "Delete", FIELD_RESPAWN, 1, 0, nullptr},
    {"respawn_alt", "Respawn (alternate)", "Return to the last checkpoint", "TMNF", "Backspace", FIELD_RESPAWN, 1, 0,
     nullptr},
};

const ft_input_schema kInputSchema = {
    sizeof(ft_input_schema),
    sizeof(TmnfInput),
    alignof(TmnfInput),
    kInputFields,
    FIELD_COUNT,
    kInputControls,
    static_cast<std::uint32_t>(sizeof(kInputControls) / sizeof(kInputControls[0])),
};

// --- properties --------------------------------------------------------------

// Index 0 must be the car's position: the engine reads it to keep a free 3D
// view pointed at the car when the game does not direct the camera itself.
enum PropIndex {
  PROP_POSITION = 0,
  PROP_VELOCITY,
  PROP_SPEED,
  PROP_GEAR,
  PROP_RPM,
  PROP_STEERING,
  PROP_TURNING_RATE,
  PROP_LOCAL_SPEED,
  PROP_WHEELS_ON_GROUND,
  PROP_SLIDING,
  PROP_FREE_WHEELING,
  PROP_TURBO,
  PROP_ENGINE,
  PROP_CHECKPOINTS,
  PROP_RESPAWNS,
  PROP_TIME,
  PROP_COUNT,
};

// What a run can be set up with, rather than only watched: where the car is,
// how fast it is going, and whether the engine drives it. The editor renders a
// control for each of these and writes them into the world a group starts from.
constexpr std::uint32_t kStartable = FT_PROP_WRITABLE | FT_PROP_STARTING;

const ft_prop_desc kCarProps[PROP_COUNT] = {
    {"position", "Position", "Motion", "m", FT_VALUE_VEC3, FT_PROP_SUMMARY | kStartable, 0.0, 0.0},
    {"velocity", "Velocity", "Motion", "m/s", FT_VALUE_VEC3, FT_PROP_SUMMARY | kStartable, 0.0, 0.0},
    {"speed", "Speed", "Motion", "km/h", FT_VALUE_FLOAT, FT_PROP_SUMMARY, 0.0, 0.0},
    {"gear", "Gear", "Drivetrain", nullptr, FT_VALUE_INT, 0, 0.0, 0.0},
    {"rpm", "RPM", "Drivetrain", nullptr, FT_VALUE_FLOAT, 0, 0.0, 0.0},
    {"steering", "Steering", "Controls", nullptr, FT_VALUE_FLOAT, 0, -1.0, 1.0},
    {"turning_rate", "Turning rate", "Controls", nullptr, FT_VALUE_FLOAT, 0, 0.0, 0.0},
    {"local_speed", "Local speed", "Motion", "m/s", FT_VALUE_VEC3, 0, 0.0, 0.0},
    {"wheels_on_ground", "Wheels on ground", "Contact", nullptr, FT_VALUE_INT, FT_PROP_SUMMARY, 0.0, 4.0},
    {"sliding", "Sliding", "Contact", nullptr, FT_VALUE_BOOL, 0, 0.0, 0.0},
    {"free_wheeling", "Free wheeling", "Contact", nullptr, FT_VALUE_BOOL, 0, 0.0, 0.0},
    {"turbo", "Turbo", "Drivetrain", nullptr, FT_VALUE_FLOAT, 0, 0.0, 0.0},
    // Off is the coasting car: the powertrain stops integrating and never
    // starts again, which is a start worth trying a corner from.
    {"engine", "Engine running", "Drivetrain", nullptr, FT_VALUE_BOOL, kStartable, 0.0, 0.0},
    {"checkpoints", "Checkpoints", "Race", nullptr, FT_VALUE_INT, FT_PROP_SUMMARY, 0.0, 0.0},
    {"respawns", "Respawns", "Race", nullptr, FT_VALUE_INT, 0, 0.0, 0.0},
    {"time", "Race time", "Race", "s", FT_VALUE_FLOAT, 0, 0.0, 0.0},
};

const ft_entity_class kEntityClasses[] = {
    {"car", "Car", kCarProps, PROP_COUNT},
};

// --- settings ----------------------------------------------------------------

enum SettingIndex {
  SETTING_BACKGROUND = 0,
  SETTING_BACKFACE_CULL,
  SETTING_COLLISION,
  SETTING_COUNT,
};

const ft_setting_desc kSettings[SETTING_COUNT] = {
    {"draw_background", "Draw the stadium", "Draw the scenery shell behind the track.", "Rendering", FT_VALUE_BOOL, 0.0,
     0.0},
    {"backface_cull", "Cull back faces", "Skip triangles facing away from the camera. Halves the work on solid "
                                         "geometry.",
     "Rendering", FT_VALUE_BOOL, 0.0, 0.0},
    {"draw_collision", "Show the collision shape",
     "Draw the ellipsoids the simulation collides with, over the car.", "Rendering", FT_VALUE_BOOL, 0.0, 0.0},
};

// --- lifecycle ---------------------------------------------------------------

ft_game *GameCreate(const ft_engine_api *engine) {
  auto *game = new ft_game();
  game->engine = engine;
  game->headless = !engine || !engine->texture_create;
  game->packs = ResolvePacks(engine);

  if (game->packs.empty() || ResolveTracks(engine).empty()) {
    game->status = "TrackMania game data is missing. ";
    game->status += kGameDataInstallHint;
    Log(game, FT_LOG_WARN, "%s", game->status.c_str());
  } else {
    game->status = "Packs: " + game->packs;
    Log(game, FT_LOG_INFO, "%s", game->status.c_str());
  }
  return game;
}

void GameDestroy(ft_game *game) {
  if (!game) return;
  ReleaseThumbnails(game);
  game->race_session.reset();
  game->race_cameras.reset();
  CloseSandbox(game);
  delete game;
}

ft_level *LevelLoadPath(ft_game *game, const char *path, const char *variant) {
  (void)variant;
  return LevelLoad(game, path);
}

bool LevelInfo(ft_game *game, const ft_level *level, ft_level_info *out) {
  (void)game;
  if (!level || !out) return false;
  out->struct_size = sizeof(*out);
  out->name = level->name.c_str();
  out->bounds = level->bounds;
  // A TrackMania block is thirty-two metres square, which is the only tile the
  // game has.
  out->width_tiles = static_cast<std::int32_t>(level->bounds.w / 32.f);
  out->height_tiles = static_cast<std::int32_t>(level->bounds.h / 32.f);
  out->default_spawn = ft_vec2{level->start.car.position.x, level->start.car.position.z};
  return true;
}

// --- worlds ------------------------------------------------------------------

std::int32_t WorldTick(ft_game *, const ft_world *world) {
  return world ? static_cast<std::int32_t>(world->view.tick) : 0;
}

std::int32_t WorldPlayerCount(ft_game *, const ft_world *world) { return world ? 1 : 0; }

bool WorldPlayerView(ft_game *, const ft_world *world, std::int32_t player, ft_player_view *out) {
  if (!world || !out || player != 0) return false;
  const auto &view = world->view;
  const auto &car = view.car;
  out->struct_size = sizeof(*out);
  out->position = ft_vec2{car.position.x, car.position.z};
  out->velocity = ft_vec2{car.linearSpeed.x, car.linearSpeed.z};
  out->aim = ft_vec2{0.f, 0.f};
  out->flags = FT_PLAYER_ALIVE | (view.raceCompleted ? FT_PLAYER_FINISHED : 0u);
  // The sandbox runs the countdown for us and reports tick zero at the moment
  // the clock starts, so a run always begins on tick zero.
  out->run_start_tick = 0;
  return true;
}

// --- input records -----------------------------------------------------------

void InputDefault(ft_game *, void *record) {
  if (!record) return;
  const TmnfInput value{};
  std::memcpy(record, &value, sizeof(value));
}

void InputSet(ft_game *, void *record, std::uint32_t field, std::int64_t value) {
  auto *in = static_cast<TmnfInput *>(record);
  if (!in) return;
  switch (field) {
  case FIELD_ACCELERATE: in->accelerate = value ? 1u : 0u; break;
  case FIELD_BRAKE: in->brake = value ? 1u : 0u; break;
  case FIELD_RESPAWN: in->respawn = value ? 1u : 0u; break;
  case FIELD_STEER:
    in->steer = static_cast<std::int32_t>(std::clamp<std::int64_t>(value, -kAnalogInputScale, kAnalogInputScale));
    break;
  default: break;
  }
}

std::int64_t InputGet(ft_game *, const void *record, std::uint32_t field) {
  const auto *in = static_cast<const TmnfInput *>(record);
  if (!in) return 0;
  switch (field) {
  case FIELD_ACCELERATE: return in->accelerate;
  case FIELD_BRAKE: return in->brake;
  case FIELD_RESPAWN: return in->respawn;
  case FIELD_STEER: return in->steer;
  default: return 0;
  }
}

void InputDescribe(ft_game *, const void *record, char *out, std::size_t out_size) {
  const auto *in = static_cast<const TmnfInput *>(record);
  if (!in || !out || out_size == 0) return;
  char steer[32] = "";
  if (in->steer != 0) std::snprintf(steer, sizeof(steer), " steer %+d", in->steer);
  std::snprintf(out, out_size, "%s%s%s%s", in->accelerate ? "gas " : "", in->brake ? "brake " : "",
                in->respawn ? "respawn " : "", steer);
  if (out[0] == '\0') std::snprintf(out, out_size, "-");
}

// --- property reflection -----------------------------------------------------

std::int32_t EntityCount(ft_game *, const ft_world *world, std::uint32_t entity_class) {
  return (world && entity_class == FT_ENTITY_CLASS_PLAYER) ? 1 : 0;
}

int WheelsOnGround(const sim::CarState &car) {
  int count = 0;
  for (bool contact : car.wheelContact) count += contact ? 1 : 0;
  return count;
}

bool AnyWheelSliding(const sim::CarState &car) {
  for (bool sliding : car.wheelSliding)
    if (sliding) return true;
  return car.sliding;
}

bool EntityPropGet(ft_game *, const ft_world *world, std::uint32_t entity_class, std::int32_t entity,
                   std::uint32_t prop, ft_value *out) {
  if (!world || !out || entity_class != FT_ENTITY_CLASS_PLAYER || entity != 0) return false;
  const auto &view = world->view;
  const auto &car = view.car;

  const auto vec3 = [&](ft_vec3 value) {
    out->kind = FT_VALUE_VEC3;
    out->as.v3 = value;
    return true;
  };
  const auto number = [&](double value) {
    out->kind = FT_VALUE_FLOAT;
    out->as.f = value;
    return true;
  };
  const auto integer = [&](std::int64_t value) {
    out->kind = FT_VALUE_INT;
    out->as.i = value;
    return true;
  };
  const auto boolean = [&](bool value) {
    out->kind = FT_VALUE_BOOL;
    out->as.b = value;
    return true;
  };

  switch (prop) {
  case PROP_POSITION: return vec3(ToVec3(car.position));
  case PROP_VELOCITY: return vec3(ToVec3(car.linearSpeed));
  // The simulation reports metres per second; the game's own readout, and
  // every speed a TrackMania run is discussed in, is kilometres per hour.
  case PROP_SPEED: return number(ToKmh(car.signedSpeed));
  case PROP_GEAR: return integer(car.gear);
  case PROP_RPM: return number(car.rpm);
  case PROP_STEERING: return number(view.steering);
  case PROP_TURNING_RATE: return number(car.turningRate);
  case PROP_LOCAL_SPEED: return vec3(ToVec3(car.localSpeed));
  case PROP_WHEELS_ON_GROUND: return integer(WheelsOnGround(car));
  case PROP_SLIDING: return boolean(AnyWheelSliding(car));
  case PROP_FREE_WHEELING: return boolean(car.freeWheeling);
  case PROP_TURBO: return number(car.turbo);
  case PROP_ENGINE: return boolean(world->state.EngineOn());
  case PROP_CHECKPOINTS: return integer(view.checkpointsCollected);
  case PROP_RESPAWNS: return integer(view.respawnCount);
  case PROP_TIME: return number(static_cast<double>(view.timeMs) / 1000.0);
  default: return false;
  }
}

// The three things about a car a run can be *set up* with. Everything else the
// simulation reports is a consequence of driving and cannot be dictated.
bool EntityPropSet(ft_game *game, ft_world *world, std::uint32_t entity_class, std::int32_t entity, std::uint32_t prop,
                   const ft_value *value) {
  if (!game || !world || !value || entity_class != FT_ENTITY_CLASS_PLAYER || entity != 0) return false;
  if (!game->world || !world->state) return false;

  sim::World::StateEdit edit;
  switch (prop) {
  case PROP_POSITION:
    if (value->kind != FT_VALUE_VEC3) return false;
    edit.position = sim::Vector3{value->as.v3.x, value->as.v3.y, value->as.v3.z};
    break;
  case PROP_VELOCITY:
    if (value->kind != FT_VALUE_VEC3) return false;
    edit.linearSpeed = sim::Vector3{value->as.v3.x, value->as.v3.y, value->as.v3.z};
    break;
  case PROP_ENGINE:
    if (value->kind != FT_VALUE_BOOL) return false;
    edit.engineOn = value->as.b;
    break;
  default: return false;
  }

  // One simulation serves every world, so editing a state is as much a use of
  // it as stepping one is.
  std::lock_guard<std::mutex> lock(game->mutex);
  sim::State edited = game->world->WithEdit(world->state, edit);
  if (!edited) return false;
  world->state = std::move(edited);
  world->view = world->state.View();
  return true;
}

// --- readouts ----------------------------------------------------------------

std::uint32_t StatusLines(ft_game *, const ft_world *world, std::int32_t player, float alpha, char *out,
                          std::uint32_t max_lines, std::uint32_t line_size) {
  (void)alpha;
  if (!world || !out || max_lines == 0 || line_size == 0 || player != 0) return 0;
  const auto &view = world->view;
  const auto &car = view.car;

  std::uint32_t count = 0;
  const auto add = [&](const char *fmt, ...) {
    if (count >= max_lines) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out + count * line_size, line_size, fmt, args);
    va_end(args);
    ++count;
  };

  add("Speed: %.1f km/h", ToKmh(car.signedSpeed));
  add("Gear %d  %.0f rpm", car.gear, car.rpm);
  add("Pos: %.2f %.2f %.2f", car.position.x, car.position.y, car.position.z);
  add("Wheels: %d/4%s%s", WheelsOnGround(car), AnyWheelSliding(car) ? " sliding" : "",
      car.freeWheeling ? " freewheel" : "");
  if (car.turbo > 0.001f) add("Turbo: %.2f", car.turbo);
  add("Checkpoints: %u/%u", view.checkpointsCollected, view.checkpointsTotal);
  if (view.respawnCount > 0) add("Respawns: %u", view.respawnCount);
  if (view.raceCompleted && view.finishTimeMs.has_value()) {
    add("Finished: %.2fs", static_cast<double>(*view.finishTimeMs) / 1000.0);
  } else {
    add("Time: %.2fs", static_cast<double>(view.timeMs) / 1000.0);
  }
  return count;
}

bool PlayerLabel(ft_game *, const ft_world *world, std::int32_t player, char *out, std::size_t out_size) {
  if (!world || !out || out_size == 0 || player != 0) return false;
  const auto &view = world->view;
  if (view.raceCompleted && view.finishTimeMs.has_value()) {
    std::snprintf(out, out_size, "%.2fs", static_cast<double>(*view.finishTimeMs) / 1000.0);
  } else {
    std::snprintf(out, out_size, "%.0f km/h", ToKmh(view.car.signedSpeed));
  }
  return true;
}

void CollectEvents(ft_game *, const ft_world *previous, const ft_world *world,
                   void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  if (!previous || !world || !emit) return;
  const auto &before = previous->view;
  const auto &now = world->view;

  char text[96];
  ft_timeline_event event{};
  event.struct_size = sizeof(event);
  event.tick = static_cast<std::int32_t>(now.tick);
  event.player = 0;
  event.text = text;

  if (now.checkpointsCollected > before.checkpointsCollected) {
    std::snprintf(text, sizeof(text), "CP %u/%u  %.2fs", now.checkpointsCollected, now.checkpointsTotal,
                  static_cast<double>(now.timeMs) / 1000.0);
    event.category = "checkpoint";
    event.color = ft_color{0.96f, 0.78f, 0.15f, 1.f};
    emit(user, &event);
  }

  if (now.respawnCount > before.respawnCount) {
    std::snprintf(text, sizeof(text), "Respawn #%u", now.respawnCount);
    event.category = "respawn";
    event.color = ft_color{1.f, 0.55f, 0.25f, 1.f};
    emit(user, &event);
  }

  if (now.completedLaps > before.completedLaps && !now.raceCompleted) {
    std::snprintf(text, sizeof(text), "Lap %u/%u", now.completedLaps, now.totalLaps);
    event.category = "lap";
    event.color = ft_color{0.45f, 0.70f, 1.f, 1.f};
    emit(user, &event);
  }

  if (!before.raceCompleted && now.raceCompleted) {
    const double finish = now.finishTimeMs.has_value() ? static_cast<double>(*now.finishTimeMs) / 1000.0
                                                       : static_cast<double>(now.timeMs) / 1000.0;
    std::snprintf(text, sizeof(text), "Finish  %.2fs", finish);
    event.category = "finish";
    event.color = ft_color{0.30f, 0.90f, 0.40f, 1.f};
    emit(user, &event);
  }
}

// --- settings ----------------------------------------------------------------

std::uint32_t SettingCount(ft_game *) { return SETTING_COUNT; }

const ft_setting_desc *SettingDesc(ft_game *, std::uint32_t index) {
  return index < SETTING_COUNT ? &kSettings[index] : nullptr;
}

bool SettingGet(ft_game *game, std::uint32_t index, ft_value *out) {
  if (!game || !out) return false;
  switch (index) {
  case SETTING_BACKGROUND:
    out->kind = FT_VALUE_BOOL;
    out->as.b = game->settings.draw_background;
    return true;
  case SETTING_BACKFACE_CULL:
    out->kind = FT_VALUE_BOOL;
    out->as.b = game->settings.backface_cull;
    return true;
  case SETTING_COLLISION:
    out->kind = FT_VALUE_BOOL;
    out->as.b = game->settings.draw_collision;
    return true;
  default: return false;
  }
}

bool SettingSet(ft_game *game, std::uint32_t index, const ft_value *value) {
  if (!game || !value) return false;
  switch (index) {
  case SETTING_BACKGROUND: game->settings.draw_background = value->as.b; return true;
  case SETTING_BACKFACE_CULL: game->settings.backface_cull = value->as.b; return true;
  case SETTING_COLLISION: game->settings.draw_collision = value->as.b; return true;
  default: return false;
  }
}

// --- the vtable --------------------------------------------------------------

// Where this game's own windows open the first time they are seen.
constexpr ft_panel_desc kPanels[] = {
    {"Player Info", FT_DOCK_LEFT},
};

ft_game_module BuildModule() {
  ft_game_module module{};
  module.struct_size = sizeof(ft_game_module);
  module.abi_version = FT_GAME_ABI_VERSION;
  module.abi_revision = FT_GAME_ABI_REVISION;

  module.info.struct_size = sizeof(ft_game_info);
  module.info.id = "tmnf";
  module.info.display_name = "TrackMania Nations Forever";
  module.info.version = "1.0.0";
  module.info.author = "FrameTee";
  module.info.thumbnail = "thumbnail.png";

  module.constraints.struct_size = sizeof(ft_game_constraints);
  module.constraints.caps = FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS | FT_CAP_TIMELINE_EVENTS | FT_CAP_HOSTS_STARTING_STATE;
  module.constraints.dimensions = FT_DIMENSIONS_3D;
  module.constraints.min_players = 1;
  module.constraints.max_players = 1;
  module.constraints.ticks_per_second = 1000 / static_cast<int>(kTickMs);
  module.constraints.units_per_tile = 32.f;
  module.constraints.default_camera_height = 6.f;
  module.constraints.camera_modes = kCameraModes;
  module.constraints.camera_mode_count = kCameraModeCount;
  module.constraints.level_extension = "Gbx";
  module.constraints.level_filter_name = "TrackMania challenge";

  module.input_schema = &kInputSchema;
  module.entity_classes = kEntityClasses;
  module.entity_class_count = 1;

  module.create = GameCreate;
  module.destroy = GameDestroy;

  module.level_load_path = LevelLoadPath;
  module.level_destroy = LevelDestroy;
  module.level_info = LevelInfo;

  module.world_create = WorldCreate;
  module.world_destroy = WorldDestroy;
  module.world_copy = WorldCopy;
  module.world_step = WorldStep;
  module.world_tick = WorldTick;
  module.world_player_count = WorldPlayerCount;
  module.world_player_view = WorldPlayerView;

  module.input_default = InputDefault;
  module.input_get = InputGet;
  module.input_set = InputSet;
  module.input_describe = InputDescribe;

  module.entity_count = EntityCount;
  module.entity_prop_get = EntityPropGet;
  module.entity_prop_set = EntityPropSet;

  module.render = Render;
  module.ui = Ui;
  module.panels = kPanels;
  module.panel_count = static_cast<std::uint32_t>(sizeof(kPanels) / sizeof(kPanels[0]));
  module.camera_update = CameraUpdate;

  module.status_lines = StatusLines;
  module.player_label = PlayerLabel;
  module.collect_events = CollectEvents;

  module.setting_count = SettingCount;
  module.setting_desc = SettingDesc;
  module.setting_get = SettingGet;
  module.setting_set = SettingSet;
  return module;
}

} // namespace
} // namespace tmnf

extern "C" FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(std::uint32_t engine_abi_version) {
  static const ft_game_module module = tmnf::BuildModule();
  return engine_abi_version == FT_GAME_ABI_VERSION ? &module : nullptr;
}
