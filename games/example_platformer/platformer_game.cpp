// A small raylib-powered single-player platformer, written in C++, to
// demonstrate that the FrameTee game layer is not shaped around DDNet.
//
// It is deliberately small but not a stub: it has levels, collectibles, a goal
// and a finish time, which is the least a TAS tool needs to be pointed at
// something. It exists to prove four things:
//
//  1. A game module needs nothing from FrameTee but <frametee/game_abi.h>.
//     It remains free to use a game-side library such as raylib without
//     exposing raylib or C++ types across the boundary.
//
//  2. A game defines its own input vocabulary. This one has three fields
//     (left, right, jump) and no aim at all, and the editor's timeline, snippet
//     editor and keybinds adapt to that through the schema.
//
//  3. A game constrains the engine. This one is single player and always will
//     be: min_players == max_players == 1 with no FT_CAP_DYNAMIC_PLAYERS, so the
//     editor hides adding and removing players entirely, the way it would for
//     Trackmania or a Mario run. Timeline groups are unaffected: a group is just
//     another instance of this same simulation, so any number may exist.
//
//  4. A game owns its start screen. There are no level files here, so the
//     splash lists the levels compiled into this module and opens one by asking
//     the engine for a path only this module knows how to read.

#include <frametee/game_abi.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <raylib.h>
#include <raymath.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cimgui.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

constexpr float kGravity = 0.035f;
constexpr float kMoveAccel = 0.55f;
constexpr float kFriction = 0.82f;
constexpr float kMaxSpeed = 0.42f;
constexpr float kJumpSpeed = 0.72f;
constexpr int kLevelWidth = 96;
constexpr int kLevelHeight = 32;
constexpr int kMaxCoins = 32; // one bit each, so a world stays trivially copyable
constexpr float kPickupRadius = 0.75f;
constexpr int kTicksPerSecond = 60;

// --- input -------------------------------------------------------------------

enum InputField { kLeft = 0, kRight, kJump, kFieldCount };

// One byte per button. The engine copies these around without ever looking
// inside, which is what lets it store a game's inputs it has never seen.
struct PlatformerInput {
  uint8_t left;
  uint8_t right;
  uint8_t jump;
  uint8_t padding;
};

const ft_input_field kInputFields[kFieldCount] = {
    {"left", "Left", "Hold to accelerate left", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X, 0, 1, 0, 0.f, 0.f, 0.f,
     nullptr, 0, {0.4f, 0.7f, 1.0f, 1.0f}},
    {"right", "Right", "Hold to accelerate right", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X, 0, 1, 0, 0.f, 0.f,
     0.f, nullptr, 0, {0.4f, 1.0f, 0.7f, 1.0f}},
    {"jump", "Jump", "Only fires from the ground", FT_INPUT_BOOL, FT_INPUT_FLAG_TIMELINE_LANE, 0, 1, 0, 0.f, 0.f, 0.f, nullptr, 0,
     {1.0f, 0.85f, 0.35f, 1.0f}},
};

const ft_input_control kInputControls[] = {
    {"left", "Move Left", "Accelerate the player left", "Raylib Platformer", "A", kLeft, 1, 0, nullptr},
    {"right", "Move Right", "Accelerate the player right", "Raylib Platformer", "D", kRight, 1, 0, nullptr},
    {"jump", "Jump", "Jump while grounded", "Raylib Platformer", "Space", kJump, 1, 0, nullptr},
};

const ft_input_schema kInputSchema = {
    sizeof(ft_input_schema), sizeof(PlatformerInput), alignof(PlatformerInput), kInputFields, kFieldCount,
    kInputControls, static_cast<uint32_t>(sizeof(kInputControls) / sizeof(kInputControls[0])),
};

// --- properties --------------------------------------------------------------

enum PlayerProp { kPropPosition = 0, kPropVelocity, kPropGrounded, kPropCoins, kPlayerPropCount };

const ft_prop_desc kPlayerProps[kPlayerPropCount] = {
    {"position", "Position", "Movement", "tiles", FT_VALUE_VEC2, FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY, 0.0, 0.0},
    {"velocity", "Velocity", "Movement", "tiles/tick", FT_VALUE_VEC2, FT_PROP_WRITABLE | FT_PROP_SUMMARY, 0.0, 0.0},
    {"grounded", "Grounded", "Movement", nullptr, FT_VALUE_BOOL, 0, 0.0, 0.0},
    {"coins", "Coins", "Progress", nullptr, FT_VALUE_INT, FT_PROP_SUMMARY, 0.0, 0.0},
};

enum CoinProp { kCoinPosition = 0, kCoinTaken, kCoinPropCount };

const ft_prop_desc kCoinProps[kCoinPropCount] = {
    {"position", "Position", nullptr, "tiles", FT_VALUE_VEC2, FT_PROP_SUMMARY, 0.0, 0.0},
    {"taken", "Collected", nullptr, nullptr, FT_VALUE_BOOL, FT_PROP_SUMMARY, 0.0, 0.0},
};

// Class 0 must be the player. Everything after it is pickable in the viewport
// as long as it exposes a property called "position".
enum EntityClass { kClassPlayer = 0, kClassCoin, kEntityClassCount };

const ft_entity_class kEntityClasses[kEntityClassCount] = {
    {"player", "Runner", kPlayerProps, kPlayerPropCount},
    {"coin", "Coin", kCoinProps, kCoinPropCount},
};

// --- settings ----------------------------------------------------------------

enum Setting { kSettingGrid = 0, kSettingTrail, kSettingCount };

const ft_setting_desc kSettings[kSettingCount] = {
    {"show_grid", "Tile grid", "Draw a line on every tile boundary", "Platformer", FT_VALUE_BOOL, 0.0, 0.0},
    {"camera_lead", "Camera lead", "How far the camera looks ahead of the runner, in tiles", "Platformer", FT_VALUE_FLOAT, 0.0, 8.0},
};

// --- camera ------------------------------------------------------------------

enum CameraMode { kCameraFree = 0, kCameraFollow, kCameraModeCount };

const ft_camera_mode kCameraModes[kCameraModeCount] = {
    {"free", "Free", "Pan and zoom by hand", 0},
    {"follow", "Follow runner", "Keeps the runner centred, leading in the direction of travel", 0},
};

} // namespace

// --- handles -----------------------------------------------------------------
//
// The ABI declares these as opaque structs, so the module defines them however
// it likes. Here they are ordinary C++ objects.

struct ft_level {
  std::vector<uint8_t> solid; // kLevelWidth * kLevelHeight, 1 where blocked
  std::vector<Vector2> coins;
  Vector2 spawn{4.f, 4.f};
  Vector2 goal{0.f, 0.f};
  char name[64] = "Example";
  int index = 0;

  bool blocked(int x, int y) const {
    if (x < 0 || y < 0 || x >= kLevelWidth || y >= kLevelHeight) return true;
    return solid[static_cast<size_t>(y) * kLevelWidth + x] != 0;
  }
};

// Kept trivially copyable on purpose: world_copy is on the engine's hot path,
// and serialization is then a memcpy. The level pointer is session state, not
// run state, so it is restored rather than loaded on deserialize.
struct ft_world {
  const ft_level *level = nullptr;
  int32_t tick = 0;
  Vector2 position{4.f, 4.f};
  Vector2 velocity{0.f, 0.f};
  bool grounded = false;
  uint32_t coins_taken = 0;  // bit per coin index
  int32_t finish_tick = -1;  // tick the goal was reached on, or -1
};

static_assert(std::is_trivially_copyable_v<ft_world>, "FrameTee snapshots copy raylib-backed worlds by value");

struct ft_game {
  const ft_engine_api *engine = nullptr;
  bool show_grid = false;
  float camera_lead = 3.0f;
};

namespace {

int coin_count(const ft_level *level) { return level ? static_cast<int>(level->coins.size()) : 0; }
int coins_taken(const ft_world *world) { return world ? static_cast<int>(std::popcount(world->coins_taken)) : 0; }

ft_vec2 to_ft(Vector2 value) { return {value.x, value.y}; }
Vector2 to_ray(ft_vec2 value) { return {value.x, value.y}; }

ft_color to_ft(Color value, float opacity = 1.f) {
  const Vector4 normalized = ColorNormalize(value);
  return {normalized.x, normalized.y, normalized.z, normalized.w * opacity};
}

// --- levels ------------------------------------------------------------------
//
// Three layouts compiled into the module. A game with real level files would
// parse them here instead; the engine only ever hands over a path.

struct BuiltinLevel {
  const char *id;
  const char *name;
  const char *description;
};

const BuiltinLevel kBuiltinLevels[] = {
    {"builtin:steps", "Steps", "Four platforms and a straight run. Start here."},
    {"builtin:towers", "Towers", "Vertical climbing, with the goal up top."},
    {"builtin:gaps", "Gaps", "Wide jumps that need the full run-up."},
};
constexpr int kBuiltinLevelCount = static_cast<int>(sizeof(kBuiltinLevels) / sizeof(kBuiltinLevels[0]));

ft_level *build_level(int index) {
  auto *level = new ft_level();
  level->solid.assign(static_cast<size_t>(kLevelWidth) * kLevelHeight, 0);
  level->index = (index >= 0 && index < kBuiltinLevelCount) ? index : 0;
  std::snprintf(level->name, sizeof(level->name), "%s", kBuiltinLevels[level->index].name);

  auto set = [&](int x, int y) {
    if (x >= 0 && y >= 0 && x < kLevelWidth && y < kLevelHeight) level->solid[static_cast<size_t>(y) * kLevelWidth + x] = 1;
  };
  auto floor_run = [&](int x0, int x1, int y) {
    for (int x = x0; x < x1; ++x) set(x, y);
  };
  auto pillar = [&](int x, int y0, int y1) {
    for (int y = y0; y < y1; ++y) set(x, y);
  };
  auto coin = [&](float x, float y) {
    if (level->coins.size() < kMaxCoins) level->coins.push_back({x, y});
  };

  // Every level is closed in by a border.
  floor_run(0, kLevelWidth, kLevelHeight - 1);
  pillar(0, 0, kLevelHeight);
  pillar(kLevelWidth - 1, 0, kLevelHeight);

  const float ground = static_cast<float>(kLevelHeight) - 2.f;
  level->spawn = {4.f, ground};

  switch (level->index) {
  case 1: { // Towers: climb a staircase of pillars.
    for (int i = 0; i < 6; ++i) {
      const int x = 12 + i * 12;
      const int top = kLevelHeight - 5 - i * 4;
      floor_run(x, x + 7, top);
      coin(static_cast<float>(x) + 3.f, static_cast<float>(top) - 1.5f);
    }
    level->goal = {static_cast<float>(12 + 5 * 12) + 3.f, static_cast<float>(kLevelHeight - 5 - 5 * 4) - 1.f};
    break;
  }
  case 2: { // Gaps: islands with nothing underneath.
    floor_run(1, 14, kLevelHeight - 8);
    floor_run(22, 34, kLevelHeight - 8);
    floor_run(42, 54, kLevelHeight - 10);
    floor_run(62, 78, kLevelHeight - 8);
    coin(8.f, static_cast<float>(kLevelHeight - 10));
    coin(28.f, static_cast<float>(kLevelHeight - 10));
    coin(48.f, static_cast<float>(kLevelHeight - 12));
    coin(70.f, static_cast<float>(kLevelHeight - 10));
    level->spawn = {4.f, static_cast<float>(kLevelHeight - 9)};
    level->goal = {74.f, static_cast<float>(kLevelHeight - 9)};
    break;
  }
  default: { // Steps: the original layout, now with something to collect.
    floor_run(8, 20, kLevelHeight - 6);
    floor_run(26, 40, kLevelHeight - 11);
    floor_run(46, 60, kLevelHeight - 7);
    floor_run(66, 88, kLevelHeight - 14);
    coin(14.f, static_cast<float>(kLevelHeight - 8));
    coin(33.f, static_cast<float>(kLevelHeight - 13));
    coin(53.f, static_cast<float>(kLevelHeight - 9));
    coin(76.f, static_cast<float>(kLevelHeight - 16));
    level->goal = {86.f, static_cast<float>(kLevelHeight - 15)};
    break;
  }
  }
  return level;
}

// The engine hands back whatever string the splash asked it to open, so this is
// where a path turns into a level. Anything unrecognised falls back to the
// first level rather than failing, so opening an old project still works.
int level_index_from_path(const char *path) {
  if (!path) return 0;
  for (int i = 0; i < kBuiltinLevelCount; ++i) {
    if (std::strcmp(path, kBuiltinLevels[i].id) == 0) return i;
  }
  return 0;
}

// Axis-separated collision against the tile grid: move on one axis and ask
// raylib to test the runner rectangle against the candidate solid tiles.
bool collides_with_level(const ft_level *level, Rectangle runner) {
  const int x0 = static_cast<int>(std::floor(runner.x));
  const int y0 = static_cast<int>(std::floor(runner.y));
  const int x1 = static_cast<int>(std::floor(runner.x + runner.width));
  const int y1 = static_cast<int>(std::floor(runner.y + runner.height));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      if (level->blocked(x, y) && CheckCollisionRecs(runner, Rectangle{static_cast<float>(x), static_cast<float>(y), 1.f, 1.f})) return true;
    }
  }
  return false;
}

void move_axis(const ft_level *level, Vector2 &pos, float &vel, bool horizontal, bool &grounded) {
  constexpr float kHalf = 0.4f;
  float &axis = horizontal ? pos.x : pos.y;
  const float next = axis + vel;
  const float edge = vel > 0.f ? next + kHalf : next - kHalf;
  const Rectangle candidate = horizontal ? Rectangle{next - kHalf, pos.y - kHalf, kHalf * 2.f, kHalf * 2.f}
                                         : Rectangle{pos.x - kHalf, next - kHalf, kHalf * 2.f, kHalf * 2.f};

  if (!collides_with_level(level, candidate)) {
    axis = next;
    return;
  }
  if (!horizontal && vel > 0.f) grounded = true;
  axis = vel > 0.f ? std::floor(edge) - kHalf - 0.001f : std::floor(edge) + 1.f + kHalf + 0.001f;
  vel = 0.f;
}

// --- module entry points -----------------------------------------------------

ft_game *game_create(const ft_engine_api *engine) {
  auto *game = new ft_game();
  game->engine = engine;
  if (engine && engine->log) engine->log(FT_LOG_INFO, "Platformer", "Example platformer module ready.");
  return game;
}

void game_destroy(ft_game *game) { delete game; }

ft_level *level_load_path(ft_game *, const char *path, const char *) { return build_level(level_index_from_path(path)); }

// Nothing about a level here is worth storing, so a project only needs to
// remember which one it was. Serializing the id back out keeps a project
// openable without the original path.
ft_level *level_load_memory(ft_game *, const void *data, size_t size, const char *) {
  char id[64] = {0};
  if (data && size > 0) std::snprintf(id, sizeof(id), "%.*s", static_cast<int>(size < sizeof(id) - 1 ? size : sizeof(id) - 1),
                                      static_cast<const char *>(data));
  return build_level(level_index_from_path(id));
}

size_t level_serialize(ft_game *, const ft_level *level, void *out, size_t out_size) {
  if (!level) return 0;
  const char *id = kBuiltinLevels[level->index].id;
  const size_t needed = std::strlen(id);
  if (!out) return needed;
  if (out_size < needed) return 0;
  std::memcpy(out, id, needed);
  return needed;
}

void level_destroy(ft_game *, ft_level *level) { delete level; }

bool level_info(ft_game *, const ft_level *level, ft_level_info *out) {
  if (!level || !out) return false;
  out->name = level->name;
  out->width_tiles = kLevelWidth;
  out->height_tiles = kLevelHeight;
  out->bounds = {0.f, 0.f, static_cast<float>(kLevelWidth), static_cast<float>(kLevelHeight)};
  out->default_spawn = to_ft(level->spawn);
  return true;
}

ft_world *world_create(ft_game *, const ft_world_desc *desc) {
  auto *world = new ft_world();
  world->level = desc ? desc->level : nullptr;
  world->position = world->level ? world->level->spawn : Vector2{4.f, static_cast<float>(kLevelHeight) - 3.f};
  return world;
}

void world_destroy(ft_game *, ft_world *world) { delete world; }

void world_copy(ft_game *, ft_world *dst, const ft_world *src) {
  if (dst && src) *dst = *src;
}

void world_step(ft_game *, ft_world *world, const void *inputs, uint32_t player_count) {
  if (!world || !world->level) return;
  PlatformerInput in{};
  if (inputs && player_count > 0) std::memcpy(&in, inputs, sizeof(in));

  const float wanted = (in.right ? 1.f : 0.f) - (in.left ? 1.f : 0.f);
  world->velocity.x = Clamp((world->velocity.x + wanted * kMoveAccel) * kFriction, -kMaxSpeed, kMaxSpeed);

  if (in.jump && world->grounded) world->velocity.y = -kJumpSpeed;
  world->velocity.y += kGravity;

  world->grounded = false;
  move_axis(world->level, world->position, world->velocity.x, true, world->grounded);
  move_axis(world->level, world->position, world->velocity.y, false, world->grounded);

  // Collect anything close enough, then check the goal. Both are pure functions
  // of position, so a rewind lands on exactly the same state.
  const int coins = coin_count(world->level);
  for (int i = 0; i < coins; ++i) {
    if (world->coins_taken & (1u << i)) continue;
    const Vector2 c = world->level->coins[static_cast<size_t>(i)];
    if (CheckCollisionCircles(world->position, 0.4f, c, kPickupRadius)) world->coins_taken |= (1u << i);
  }

  if (world->finish_tick < 0 && coins_taken(world) == coins &&
      CheckCollisionCircleRec(world->position, 0.4f, Rectangle{world->level->goal.x - 0.5f, world->level->goal.y - 1.5f, 1.f, 2.f}))
    world->finish_tick = world->tick;

  world->tick++;
}

int32_t world_tick(ft_game *, const ft_world *world) { return world ? world->tick : 0; }

// Always exactly one player. The engine reads this together with the
// constraints and stops offering anything that would change it.
int32_t world_player_count(ft_game *, const ft_world *) { return 1; }

bool world_player_view(ft_game *, const ft_world *world, int32_t player, ft_player_view *out) {
  if (!world || player != 0 || !out) return false;
  out->position = to_ft(world->position);
  out->velocity = to_ft(world->velocity);
  out->aim = {0.f, 0.f};
  out->flags = FT_PLAYER_ALIVE;
  // The run is timed from the first tick, so the editor can align timings.
  out->run_start_tick = 0;
  return true;
}

size_t world_serialize(ft_game *, const ft_world *world, void *out, size_t out_size) {
  if (!world) return 0;
  const size_t needed = sizeof(ft_world);
  if (!out) return needed;
  if (out_size < needed) return 0;
  std::memcpy(out, world, needed);
  return needed;
}

bool world_deserialize(ft_game *, ft_world *world, const void *data, size_t size) {
  if (!world || !data || size < sizeof(ft_world)) return false;
  const ft_level *level = world->level; // the level belongs to this session
  std::memcpy(world, data, sizeof(ft_world));
  world->level = level;
  return true;
}

// Game-owned project data is opaque to FrameTee. Even this example uses a
// versioned marker so the size-query/write/read convention is visible in a
// complete module and exercised by the engine's round-trip tests.
constexpr uint8_t kProjectData[] = {'R', 'A', 'Y', 'P', 1, 0, 0, 0};

size_t project_save(ft_game *, void *out, size_t out_size) {
  if (!out) return sizeof(kProjectData);
  if (out_size < sizeof(kProjectData)) return 0;
  std::memcpy(out, kProjectData, sizeof(kProjectData));
  return sizeof(kProjectData);
}

bool project_load(ft_game *, const void *data, size_t size) {
  return data && size == sizeof(kProjectData) && std::memcmp(data, kProjectData, sizeof(kProjectData)) == 0;
}

void input_default(ft_game *, void *record) { std::memset(record, 0, sizeof(PlatformerInput)); }

int64_t input_get(ft_game *, const void *record, uint32_t field) {
  const auto *in = static_cast<const PlatformerInput *>(record);
  switch (field) {
  case kLeft: return in->left != 0;
  case kRight: return in->right != 0;
  case kJump: return in->jump != 0;
  default: return 0;
  }
}

void input_set(ft_game *, void *record, uint32_t field, int64_t value) {
  auto *in = static_cast<PlatformerInput *>(record);
  const uint8_t v = value ? 1 : 0;
  switch (field) {
  case kLeft: in->left = v; break;
  case kRight: in->right = v; break;
  case kJump: in->jump = v; break;
  default: break;
  }
}

ft_vec2 input_get_vec2(ft_game *, const void *, uint32_t) { return {0.f, 0.f}; }
void input_set_vec2(ft_game *, void *, uint32_t, ft_vec2) {}

void input_describe(ft_game *, const void *record, char *out, size_t out_size) {
  const auto *in = static_cast<const PlatformerInput *>(record);
  std::snprintf(out, out_size, "%s%s%s", in->left ? "<" : "", in->right ? ">" : "", in->jump ? " ^" : "");
}

int32_t entity_count(ft_game *, const ft_world *world, uint32_t entity_class) {
  switch (entity_class) {
  case kClassPlayer: return 1;
  case kClassCoin: return coin_count(world ? world->level : nullptr);
  default: return 0;
  }
}

bool entity_prop_get(ft_game *, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, ft_value *out) {
  if (!world || !out) return false;

  if (entity_class == kClassCoin) {
    if (entity < 0 || entity >= coin_count(world->level)) return false;
    switch (prop) {
    case kCoinPosition:
      out->kind = FT_VALUE_VEC2;
      out->as.v = to_ft(world->level->coins[static_cast<size_t>(entity)]);
      return true;
    case kCoinTaken:
      out->kind = FT_VALUE_BOOL;
      out->as.b = (world->coins_taken & (1u << entity)) != 0;
      return true;
    default: return false;
    }
  }

  if (entity_class != kClassPlayer || entity != 0) return false;
  switch (prop) {
  case kPropPosition:
    out->kind = FT_VALUE_VEC2;
    out->as.v = to_ft(world->position);
    return true;
  case kPropVelocity:
    out->kind = FT_VALUE_VEC2;
    out->as.v = to_ft(world->velocity);
    return true;
  case kPropGrounded:
    out->kind = FT_VALUE_BOOL;
    out->as.b = world->grounded;
    return true;
  case kPropCoins:
    out->kind = FT_VALUE_INT;
    out->as.i = coins_taken(world);
    return true;
  default: return false;
  }
}

bool entity_prop_set(ft_game *, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, const ft_value *value) {
  if (entity_class != kClassPlayer || entity != 0 || !world || !value) return false;
  switch (prop) {
  case kPropPosition: world->position = to_ray(value->as.v); return true;
  case kPropVelocity: world->velocity = to_ray(value->as.v); return true;
  default: return false;
  }
}

// --- presentation ------------------------------------------------------------

// raylib owns game-side vectors, collision and colours. FrameTee owns the
// Vulkan target, so this adapter translates raylib presentation values into
// host draw commands instead of asking raylib to open a second window.
void render(ft_game *game, const ft_render_frame *frame) {
  if (!game || !game->engine || !frame || !frame->level) return;
  const ft_engine_api *api = game->engine;

  if (frame->pass == FT_PASS_LEVEL_BACKGROUND) {
    // Only what the camera can see. The engine precomputes the rectangle
    // precisely so a game does not have to guess at it.
    ft_camera camera = {};
    camera.struct_size = sizeof(camera);
    api->camera_get(&camera);
    const int x0 = static_cast<int>(std::floor(camera.visible.x)) - 1;
    const int y0 = static_cast<int>(std::floor(camera.visible.y)) - 1;
    const int x1 = static_cast<int>(std::ceil(camera.visible.x + camera.visible.w)) + 1;
    const int y1 = static_cast<int>(std::ceil(camera.visible.y + camera.visible.h)) + 1;

    const ft_color tile = to_ft(Color{77, 87, 107, 255});
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        if (!frame->level->blocked(x, y)) continue;
        if (x < 0 || y < 0 || x >= kLevelWidth || y >= kLevelHeight) continue;
        api->draw_rect(1.f, {static_cast<float>(x), static_cast<float>(y)}, {1.f, 1.f}, tile);
      }
    }

    if (game->show_grid) {
      const ft_color line = to_ft(WHITE, 0.06f);
      for (int x = x0; x < x1; ++x)
        api->draw_line(1.5f, {static_cast<float>(x), static_cast<float>(y0)}, {static_cast<float>(x), static_cast<float>(y1)}, line, 0.02f);
      for (int y = y0; y < y1; ++y)
        api->draw_line(1.5f, {static_cast<float>(x0), static_cast<float>(y)}, {static_cast<float>(x1), static_cast<float>(y)}, line, 0.02f);
    }
    return;
  }

  if (frame->pass != FT_PASS_ENTITIES || !frame->world) return;
  const ft_world *now = frame->world;
  const ft_world *before = frame->previous_world ? frame->previous_world : now;
  const float a = frame->alpha;
  const float opacity = frame->opacity > 0.f ? frame->opacity : 1.f;

  // Coins that are still out there. They pulse on the tick clock rather than
  // wall time, so a paused editor shows a still frame.
  const int coins = coin_count(frame->level);
  const float pulse = 0.12f * std::sin(static_cast<float>(frame->tick) * 0.15f);
  for (int i = 0; i < coins; ++i) {
    if (now->coins_taken & (1u << i)) continue;
    const Vector2 c = frame->level->coins[static_cast<size_t>(i)];
    api->draw_circle(4.f, to_ft(c), 0.34f + pulse, to_ft(GOLD, opacity), 16);
  }

  // The goal, open once everything is collected.
  const bool open = coins_taken(now) == coins;
  const ft_color goal = open ? to_ft(LIME, opacity) : to_ft(GRAY, opacity);
  api->draw_rect(4.f, {frame->level->goal.x - 0.5f, frame->level->goal.y - 1.5f}, {1.0f, 2.0f}, goal);

  const Vector2 pos = Vector2Lerp(before->position, now->position, a);

  ft_color body = to_ft(ORANGE, opacity);
  if (frame->player_setup_count > 0) {
    body.r = frame->player_setups[0].primary_color.r;
    body.g = frame->player_setups[0].primary_color.g;
    body.b = frame->player_setups[0].primary_color.b;
  }
  api->draw_rect(6.f, {pos.x - 0.4f, pos.y - 0.4f}, {0.8f, 0.8f}, body);
  // A dot that leans the way the runner is moving, so direction reads at a glance.
  const float lean = now->velocity.x > 0.f ? 0.14f : (now->velocity.x < 0.f ? -0.14f : 0.f);
  api->draw_circle(6.1f, {pos.x + lean, pos.y - 0.15f}, 0.18f, to_ft(Color{26, 26, 31, 255}, opacity), 12);
}

bool camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  if (!frame || !inout || frame->mode != kCameraFollow || !frame->world) return false;
  const auto *now = static_cast<const ft_world *>(static_cast<const void *>(frame->world));
  const auto *before = frame->previous_world ? static_cast<const ft_world *>(static_cast<const void *>(frame->previous_world)) : now;
  const float a = frame->alpha;
  const float lead = game ? game->camera_lead : 0.f;
  const Vector2 position = Vector2Lerp(before->position, now->position, a);
  const Vector2 velocity = Vector2Lerp(before->velocity, now->velocity, a);
  inout->position.x = position.x + velocity.x * lead * 2.f;
  inout->position.y = position.y;
  return true;
}

void format_time(int32_t ticks, char *out, size_t out_size) {
  const float seconds = static_cast<float>(ticks) / static_cast<float>(kTicksPerSecond);
  std::snprintf(out, out_size, "%d:%06.3f", static_cast<int>(seconds) / 60, std::fmod(seconds, 60.f));
}

uint32_t status_lines(ft_game *, const ft_world *world, int32_t player, float, char *out, uint32_t max_lines, uint32_t line_size) {
  if (!world || player != 0 || !out || max_lines == 0) return 0;
  uint32_t written = 0;
  auto line = [&](const char *fmt, auto... args) {
    if (written >= max_lines) return;
    std::snprintf(out + static_cast<size_t>(written) * line_size, line_size, fmt, args...);
    ++written;
  };

  line("pos %.3f, %.3f", world->position.x, world->position.y);
  line("vel %.4f, %.4f", world->velocity.x, world->velocity.y);
  line("%s", world->grounded ? "grounded" : "airborne");
  line("coins %d/%d", coins_taken(world), coin_count(world->level));
  if (world->finish_tick >= 0) {
    char time[32];
    format_time(world->finish_tick, time, sizeof(time));
    line("finished %s", time);
  }
  return written;
}

bool player_label(ft_game *, const ft_world *world, int32_t player, char *out, size_t out_size) {
  if (!world || player != 0 || !out) return false;
  if (world->finish_tick >= 0) {
    char time[32];
    format_time(world->finish_tick, time, sizeof(time));
    std::snprintf(out, out_size, "%s", time);
    return true;
  }
  const int coins = coin_count(world->level);
  if (coins == 0) return false;
  std::snprintf(out, out_size, "%d/%d", coins_taken(world), coins);
  return true;
}

// Anything worth a mark on the timeline. The engine calls this after each tick
// it simulated, with the world on either side of it.
void collect_events(ft_game *, const ft_world *previous, const ft_world *world,
                    void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  if (!previous || !world || !emit) return;

  const uint32_t fresh = world->coins_taken & ~previous->coins_taken;
  if (fresh) {
    ft_timeline_event event = {};
    event.struct_size = sizeof(event);
    event.tick = world->tick;
    event.player = 0;
    event.category = "pickup";
    event.text = "Coin";
    event.color = {1.0f, 0.83f, 0.25f, 1.0f};
    emit(user, &event);
  }

  if (world->finish_tick >= 0 && previous->finish_tick < 0) {
    ft_timeline_event event = {};
    event.struct_size = sizeof(event);
    event.tick = world->tick;
    event.player = 0;
    event.category = "finish";
    event.text = "Finish";
    event.color = {0.35f, 1.0f, 0.5f, 1.0f};
    emit(user, &event);
  }
}

// --- settings ----------------------------------------------------------------

uint32_t setting_count(ft_game *) { return kSettingCount; }
const ft_setting_desc *setting_desc(ft_game *, uint32_t index) { return index < kSettingCount ? &kSettings[index] : nullptr; }

bool setting_get(ft_game *game, uint32_t index, ft_value *out) {
  if (!game || !out) return false;
  switch (index) {
  case kSettingGrid:
    out->kind = FT_VALUE_BOOL;
    out->as.b = game->show_grid;
    return true;
  case kSettingTrail:
    out->kind = FT_VALUE_FLOAT;
    out->as.f = game->camera_lead;
    return true;
  default: return false;
  }
}

bool setting_set(ft_game *game, uint32_t index, const ft_value *value) {
  if (!game || !value) return false;
  switch (index) {
  case kSettingGrid: game->show_grid = value->as.b; return true;
  case kSettingTrail: game->camera_lead = value->as.f; return true;
  default: return false;
  }
}

// --- start screen ------------------------------------------------------------
//
// There are no level files, so this game's start screen is the list of levels
// built into it. The ig* symbols resolve against the editor at load time, so
// nothing here links ImGui: only the headers are needed.

void ui(ft_game *game, const ft_ui_frame *frame) {
  if (!game || !game->engine || !frame || frame->slot != FT_UI_SPLASH) return;

  igPushFont(nullptr, 22.f);
  igTextUnformatted("Raylib Platformer", nullptr);
  igPopFont();
  igTextDisabled("Three raylib-backed levels. Collect every coin, then reach the goal.");
  igSpacing();
  igSeparator();
  igSpacing();

  const ImVec2 avail = igGetContentRegionAvail();
  const float card_width = avail.x > 640.f ? (avail.x - 24.f) / 3.f : avail.x;

  for (int i = 0; i < kBuiltinLevelCount; ++i) {
    igPushID_Int(i);
    igBeginGroup();

    const bool clicked = igInvisibleButton("##level", ImVec2{card_width, 92.f}, 0);
    const bool hovered = igIsItemHovered(0);

    const ImVec2 min = igGetItemRectMin();
    const ImVec2 max = igGetItemRectMax();
    ImDrawList *draw = igGetWindowDrawList();
    const ImU32 background = hovered ? igGetColorU32_Vec4(ImVec4{38.f / 255.f, 46.f / 255.f, 62.f / 255.f, 1.f})
                                     : igGetColorU32_Vec4(ImVec4{28.f / 255.f, 33.f / 255.f, 45.f / 255.f, 1.f});
    const ImU32 border = hovered ? igGetColorU32_Vec4(ImVec4{90.f / 255.f, 175.f / 255.f, 1.f, 1.f})
                                 : igGetColorU32_Vec4(ImVec4{48.f / 255.f, 56.f / 255.f, 75.f / 255.f, 140.f / 255.f});
    const ImU32 title = igGetColorU32_Vec4(ImVec4{235.f / 255.f, 240.f / 255.f, 250.f / 255.f, 1.f});
    const ImU32 description = igGetColorU32_Vec4(ImVec4{150.f / 255.f, 160.f / 255.f, 180.f / 255.f, 1.f});

    ImDrawList_AddRectFilled(draw, min, max, background, 6.f, 0);
    ImDrawList_AddRect(draw, min, max, border, 6.f, 0, 1.f);
    ImDrawList_AddText_Vec2(draw, ImVec2{min.x + 12.f, min.y + 12.f}, title, kBuiltinLevels[i].name, nullptr);
    ImDrawList_AddText_Vec2(draw, ImVec2{min.x + 12.f, min.y + 34.f}, description, kBuiltinLevels[i].description, nullptr);

    if (clicked) game->engine->request_level(kBuiltinLevels[i].id);

    igEndGroup();
    igPopID();
    if (card_width < avail.x && i + 1 < kBuiltinLevelCount) igSameLine(0.f, 12.f);
  }
}

const ft_game_module kModule = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,

    .info = {.struct_size = sizeof(ft_game_info),
             .id = "example-platformer",
             .display_name = "Raylib Platformer (C++)",
             .version = "2.0.0",
             .author = "FrameTee",
             .url = nullptr,
             .thumbnail = "thumbnail.png"},

    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    // Deliberately narrow: no dynamic players and no dummies.
                    // The editor adapts to both.
                    .caps = FT_CAP_WORLD_SERIALIZE | FT_CAP_LEVEL_FROM_MEMORY | FT_CAP_TIMELINE_EVENTS | FT_CAP_RENDERS_LEVEL |
                            FT_CAP_HEADLESS,
                    .min_players = 1,
                    .max_players = 1,
                    .ticks_per_second = kTicksPerSecond,
                    .units_per_tile = 1.f,
                    .default_camera_height = 18.f,
                    .variants = nullptr,
                    .variant_count = 0,
                    .camera_modes = kCameraModes,
                    .camera_mode_count = kCameraModeCount,
                    .level_extension = nullptr,
                    .level_filter_name = nullptr},

    .input_schema = &kInputSchema,
    .entity_classes = kEntityClasses,
    .entity_class_count = kEntityClassCount,

    .create = game_create,
    .destroy = game_destroy,
    .update = nullptr,

    .level_load_path = level_load_path,
    .level_load_memory = level_load_memory,
    .level_destroy = level_destroy,
    .level_info = level_info,
    .level_serialize = level_serialize,

    .world_create = world_create,
    .world_destroy = world_destroy,
    .world_copy = world_copy,
    .world_step = world_step,
    .world_tick = world_tick,
    .world_player_count = world_player_count,
    .world_player_view = world_player_view,
    .world_add_player = nullptr, // fixed cast; the engine never asks
    .world_remove_player = nullptr,
    .world_serialize = world_serialize,
    .world_deserialize = world_deserialize,

    .input_default = input_default,
    .input_get = input_get,
    .input_set = input_set,
    .input_get_vec2 = input_get_vec2,
    .input_set_vec2 = input_set_vec2,
    .input_describe = input_describe,

    .entity_prop_get = entity_prop_get,
    .entity_prop_set = entity_prop_set,
    .entity_count = entity_count,

    .render = render,
    .resources_create = nullptr,
    .resources_destroy = nullptr,

    .ui = ui,
    .collect_events = collect_events,

    .exporter_count = nullptr,
    .exporter_desc = nullptr,
    .export_run = nullptr,

    .status_lines = status_lines,
    .player_label = player_label,
    .camera_update = camera_update,

    .setting_count = setting_count,
    .setting_desc = setting_desc,
    .setting_get = setting_get,
    .setting_set = setting_set,

    .project_save = project_save,
    .project_load = project_load,
    .input_get_float = nullptr,
    .input_set_float = nullptr,
    .linked_actions = nullptr,
    .linked_action_count = 0,
    .linked_input_update = nullptr,
};

} // namespace

extern "C" FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  if (engine_abi_version != FT_GAME_ABI_VERSION) return nullptr;
  return &kModule;
}
