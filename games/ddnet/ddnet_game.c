// DDNet as a FrameTee game module.
//
// This file is the ABI surface: identity, constraints, input schema, levels,
// worlds and the vtable at the bottom. The game's own subsystems live beside it
// (dd_gfx.c, dd_render.c, dd_particles.c, dd_anim_*.c) and none of them are
// visible to the engine.

#include "dd_internal.h"
#include "dd_maps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Input schema
// -----------------------------------------------------------------------------
//
// SPlayerInput packs its booleans into a bitfield, which is precisely why the
// engine edits inputs through named fields instead of reaching into the record.

enum ddnet_input_field {
  IN_DIRECTION = 0,
  IN_TARGET,
  IN_JUMP,
  IN_FIRE,
  IN_HOOK,
  IN_WEAPON,
  IN_KILL,
  IN_EYES,
  IN_EMOTE,
  IN_SIT,
  IN_TELE_OUT,
  IN_COUNT
};

static const char *const weapon_labels[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
static const char *const eye_labels[] = {"Normal", "Angry", "Pain", "Happy", "Blink", "Surprise"};

static const ft_input_field input_fields[IN_COUNT] = {
    [IN_DIRECTION] = {.id = "direction",
                      .display_name = "Direction",
                      .description = "-1 left, 0 still, 1 right",
                      .kind = FT_INPUT_INT,
                      .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X,
                      .min_value = -1,
                      .max_value = 1,
                      .color = {0.35f, 0.75f, 1.0f, 1.0f}},
    [IN_TARGET] = {.id = "target",
                   .display_name = "Aim",
                   .description = "Cursor offset from the tee, in pixels",
                   .kind = FT_INPUT_VEC2,
                   .flags = FT_INPUT_FLAG_MIRROR_X | FT_INPUT_FLAG_MIRROR_Y | FT_INPUT_FLAG_RECORDING_CURSOR,
                   .min_float = -1000.f,
                   .max_float = 1000.f,
                   .color = {0.9f, 0.9f, 0.4f, 1.0f}},
    [IN_JUMP] = {.id = "jump",
                 .display_name = "Jump",
                 .kind = FT_INPUT_BOOL,
                 .flags = FT_INPUT_FLAG_TIMELINE_LANE,
                 .color = {0.4f, 1.0f, 0.5f, 1.0f}},
    [IN_FIRE] = {.id = "fire",
                 .display_name = "Fire",
                 .description = "Counts weapon triggers; the physics fires on odd values",
                 .kind = FT_INPUT_BOOL,
                 .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_LATCHED,
                 .color = {1.0f, 0.5f, 0.35f, 1.0f}},
    [IN_HOOK] = {.id = "hook",
                 .display_name = "Hook",
                 .kind = FT_INPUT_BOOL,
                 .flags = FT_INPUT_FLAG_TIMELINE_LANE,
                 .color = {1.0f, 0.75f, 0.3f, 1.0f}},
    [IN_WEAPON] = {.id = "weapon",
                   .display_name = "Weapon",
                   .kind = FT_INPUT_ENUM,
                   .flags = FT_INPUT_FLAG_TIMELINE_LANE,
                   .min_value = 0,
                   .max_value = NUM_WEAPONS - 1,
                   .enum_labels = weapon_labels,
                   .enum_count = (uint32_t)(sizeof(weapon_labels) / sizeof(weapon_labels[0])),
                   .color = {0.8f, 0.6f, 1.0f, 1.0f}},
    [IN_KILL] = {.id = "kill",
                 .display_name = "Kill",
                 .kind = FT_INPUT_BOOL,
                 .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_TRIGGER,
                 .color = {1.0f, 0.3f, 0.3f, 1.0f}},
    [IN_EYES] = {.id = "eyes",
                 .display_name = "Eyes",
                 .kind = FT_INPUT_ENUM,
                 .min_value = 0,
                 .max_value = NUM_EYES - 1,
                 .enum_labels = eye_labels,
                 .enum_count = (uint32_t)(sizeof(eye_labels) / sizeof(eye_labels[0]))},
    [IN_EMOTE] = {.id = "emote", .display_name = "Emoticon", .kind = FT_INPUT_INT, .min_value = 0, .max_value = 15},
    [IN_SIT] = {.id = "sit", .display_name = "Sit", .kind = FT_INPUT_BOOL},
    [IN_TELE_OUT] = {.id = "tele_out", .display_name = "Tele out", .kind = FT_INPUT_INT, .flags = FT_INPUT_FLAG_INTERNAL, .max_value = 255},
};

static const ft_input_control input_controls[] = {
    {.id = "left", .display_name = "Move Left", .category = "DDNet", .default_binding = "A", .field = IN_DIRECTION, .value = -1, .flags = FT_CONTROL_ADD},
    {.id = "right", .display_name = "Move Right", .category = "DDNet", .default_binding = "D", .field = IN_DIRECTION, .value = 1, .flags = FT_CONTROL_ADD},
    {.id = "jump", .display_name = "Jump", .category = "DDNet", .default_binding = "Space", .field = IN_JUMP, .value = 1},
    {.id = "fire", .display_name = "Fire Weapon", .category = "DDNet", .default_binding = "MouseLeft", .field = IN_FIRE, .value = 1},
    {.id = "hook", .display_name = "Hook", .category = "DDNet", .default_binding = "MouseRight", .field = IN_HOOK, .value = 1},
    {.id = "hammer", .display_name = "Switch to Hammer", .category = "DDNet", .default_binding = "1", .field = IN_WEAPON, .value = 0, .flags = FT_CONTROL_PRESSED},
    {.id = "gun", .display_name = "Switch to Gun", .category = "DDNet", .default_binding = "2", .field = IN_WEAPON, .value = 1, .flags = FT_CONTROL_PRESSED},
    {.id = "shotgun", .display_name = "Switch to Shotgun", .category = "DDNet", .default_binding = "3", .field = IN_WEAPON, .value = 2, .flags = FT_CONTROL_PRESSED},
    {.id = "grenade", .display_name = "Switch to Grenade", .category = "DDNet", .default_binding = "4", .field = IN_WEAPON, .value = 3, .flags = FT_CONTROL_PRESSED},
    {.id = "laser", .display_name = "Switch to Laser", .category = "DDNet", .default_binding = "5", .field = IN_WEAPON, .value = 4, .flags = FT_CONTROL_PRESSED},
    {.id = "kill", .display_name = "Kill", .category = "DDNet", .default_binding = "K", .field = IN_KILL, .value = 1, .flags = FT_CONTROL_PRESSED},
};

static const ft_input_schema input_schema = {
    .struct_size = sizeof(ft_input_schema),
    .record_size = sizeof(SPlayerInput),
    .record_align = _Alignof(SPlayerInput),
    .fields = input_fields,
    .field_count = IN_COUNT,
    .controls = input_controls,
    .control_count = (uint32_t)(sizeof(input_controls) / sizeof(input_controls[0])),
};

enum { LINKED_AIM_AT_SOURCE = 0 };

static const ft_linked_action linked_actions[] = {
    {.id = "aim_at_source",
     .display_name = "Aim at source player",
     .description = "Point this linked tee directly at the player being recorded",
     .default_binding = NULL},
};

static void ddnet_input_default(ft_game *game, void *record) {
  (void)game;
  SPlayerInput *input = record;
  memset(input, 0, sizeof(*input));
  // A zero aim would leave the tee pointing at itself, which no real client
  // ever sends; straight up matches what DDNet does on spawn.
  input->m_TargetY = -1;
}

static int64_t ddnet_input_get(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const SPlayerInput *in = record;
  switch (field) {
  case IN_DIRECTION:
    return in->m_Direction;
  case IN_JUMP:
    return in->m_Jump != 0;
  case IN_FIRE:
    return (in->m_Fire & 1) != 0;
  case IN_HOOK:
    return in->m_Hook != 0;
  case IN_WEAPON:
    return in->m_WantedWeapon;
  case IN_KILL:
    return get_flag_kill(in) != 0;
  case IN_EYES:
    return get_flag_eye_state(in);
  case IN_EMOTE:
    return get_flag_emote_index(in);
  case IN_SIT:
    return get_flag_sit(in) != 0;
  case IN_TELE_OUT:
    return in->m_TeleOut;
  default:
    return 0;
  }
}

static void ddnet_input_set(ft_game *game, void *record, uint32_t field, int64_t value) {
  (void)game;
  SPlayerInput *in = record;
  switch (field) {
  case IN_DIRECTION:
    in->m_Direction = (int8_t)(value < -1 ? -1 : (value > 1 ? 1 : value));
    break;
  case IN_JUMP:
    in->m_Jump = value ? 1 : 0;
    break;
  // Fire is a counter in the protocol: the physics acts on odd values, so
  // toggling the low bit is what "pressed this tick" means.
  case IN_FIRE:
    in->m_Fire = value ? (in->m_Fire | 1) : (uint8_t)(in->m_Fire & ~1);
    break;
  case IN_HOOK:
    in->m_Hook = value ? 1 : 0;
    break;
  case IN_WEAPON:
    in->m_WantedWeapon = (uint8_t)(value < 0 ? 0 : (value >= NUM_WEAPONS ? NUM_WEAPONS - 1 : value));
    break;
  case IN_KILL:
    set_flag_kill(in, value != 0);
    break;
  case IN_EYES:
    set_flag_eye_state(in, (uint8_t)value);
    break;
  case IN_EMOTE:
    set_flag_emote_index(in, (uint8_t)value);
    set_flag_emote_trigger(in, value != 0);
    break;
  case IN_SIT:
    set_flag_sit(in, value != 0);
    break;
  case IN_TELE_OUT:
    in->m_TeleOut = (uint8_t)value;
    break;
  default:
    break;
  }
}

static ft_vec2 ddnet_input_get_vec2(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const SPlayerInput *in = record;
  if (field != IN_TARGET) return (ft_vec2){0.f, 0.f};
  return (ft_vec2){(float)in->m_TargetX, (float)in->m_TargetY};
}

static void ddnet_input_set_vec2(ft_game *game, void *record, uint32_t field, ft_vec2 value) {
  (void)game;
  SPlayerInput *in = record;
  if (field != IN_TARGET) return;
  in->m_TargetX = (int16_t)value.x;
  in->m_TargetY = (int16_t)value.y;
}

static void ddnet_input_describe(ft_game *game, const void *record, char *out, size_t out_size) {
  (void)game;
  const SPlayerInput *in = record;
  const char *dir = in->m_Direction < 0 ? "<" : (in->m_Direction > 0 ? ">" : "-");
  snprintf(out, out_size, "%s%s%s%s %s", dir, in->m_Jump ? " jump" : "", (in->m_Fire & 1) ? " fire" : "", in->m_Hook ? " hook" : "",
           in->m_WantedWeapon < NUM_WEAPONS ? weapon_labels[in->m_WantedWeapon] : "?");
}

// -----------------------------------------------------------------------------
// Entity properties
// -----------------------------------------------------------------------------

enum ddnet_player_prop {
  PROP_POSITION = 0,
  PROP_VELOCITY,
  PROP_ACTIVE_WEAPON,
  PROP_HAS_HAMMER,
  PROP_HAS_GUN,
  PROP_HAS_SHOTGUN,
  PROP_HAS_GRENADE,
  PROP_HAS_LASER,
  PROP_HAS_NINJA,
  PROP_HEALTH,
  PROP_ARMOR,
  PROP_FREEZE_TIME,
  PROP_JUMPS,
  PROP_JUMPS_LEFT,
  PROP_ENDLESS_JUMP,
  PROP_GROUNDED,
  PROP_HOOK_STATE,
  PROP_HOOKED_PLAYER,
  PROP_RACE_TIME,
  PROP_DEEP_FROZEN,
  PROP_LIVE_FROZEN,
  PROP_JETPACK,
  PROP_ENDLESS_HOOK,
  PROP_SOLO,
  PROP_COLLIDE_OTHERS,
  PROP_HOOK_OTHERS,
  PROP_HAMMER_HITS_OTHERS,
  PROP_SHOTGUN_HITS_OTHERS,
  PROP_GRENADE_HITS_OTHERS,
  PROP_LASER_HITS_OTHERS,
  PROP_TELEGUN,
  PROP_TELEGRENADE,
  PROP_TELELASER,
  PROP_COUNT
};

// Every flag below is a starting override the editor can offer, because every
// one of them is something a run can be set up with: a tee that begins with a
// jetpack, on its third jump, unable to hook anyone.
#define DD_PROP_START (FT_PROP_WRITABLE | FT_PROP_STARTING)

static const ft_prop_desc player_props[PROP_COUNT] = {
    [PROP_POSITION] = {.id = "position",
                       .display_name = "Position",
                       .group = "Movement",
                       .unit = "tiles",
                       .kind = FT_VALUE_VEC2,
                       .flags = FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY},
    [PROP_VELOCITY] = {.id = "velocity",
                       .display_name = "Velocity",
                       .group = "Movement",
                       .unit = "units/tick",
                       .kind = FT_VALUE_VEC2,
                       .flags = FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY},
    [PROP_ACTIVE_WEAPON] = {.id = "active_weapon",
                            .display_name = "Active weapon",
                            .group = "Combat",
                            .kind = FT_VALUE_INT,
                            .flags = FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY,
                            .min_value = 0,
                            .max_value = NUM_WEAPONS - 1},
    [PROP_HAS_HAMMER] = {.id = "has_hammer", .display_name = "Hammer", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_GUN] = {.id = "has_gun", .display_name = "Gun", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_SHOTGUN] = {.id = "has_shotgun", .display_name = "Shotgun", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_GRENADE] = {.id = "has_grenade", .display_name = "Grenade", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_LASER] = {.id = "has_laser", .display_name = "Laser", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_NINJA] = {.id = "has_ninja", .display_name = "Ninja", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HEALTH] = {.id = "health", .display_name = "Health", .group = "Combat", .kind = FT_VALUE_INT, .flags = FT_PROP_WRITABLE},
    [PROP_ARMOR] = {.id = "armor", .display_name = "Armor", .group = "Combat", .kind = FT_VALUE_INT, .flags = FT_PROP_WRITABLE},
    [PROP_FREEZE_TIME] = {.id = "freeze_time",
                          .display_name = "Freeze time",
                          .group = "State",
                          .unit = "ticks",
                          .kind = FT_VALUE_INT,
                          .flags = DD_PROP_START | FT_PROP_SUMMARY,
                          .min_value = 0,
                          .max_value = 500},
    [PROP_JUMPS] = {.id = "jumps",
                    .display_name = "Jumps",
                    .group = "Movement",
                    .kind = FT_VALUE_INT,
                    .flags = DD_PROP_START,
                    .min_value = 0,
                    .max_value = 255},
    [PROP_JUMPS_LEFT] = {.id = "jumps_left",
                         .display_name = "Jumps already used",
                         .group = "Movement",
                         .kind = FT_VALUE_INT,
                         .flags = DD_PROP_START,
                         .min_value = 0,
                         .max_value = 255},
    [PROP_ENDLESS_JUMP] = {.id = "endless_jump", .display_name = "Endless jump", .group = "Movement", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_GROUNDED] = {.id = "grounded", .display_name = "Grounded", .group = "Movement", .kind = FT_VALUE_BOOL},
    [PROP_HOOK_STATE] = {.id = "hook_state", .display_name = "Hook state", .group = "Movement", .kind = FT_VALUE_INT},
    [PROP_HOOKED_PLAYER] = {.id = "hooked_player", .display_name = "Hooked player", .group = "Movement", .kind = FT_VALUE_INT},
    [PROP_RACE_TIME] = {.id = "race_time",
                        .display_name = "Race time",
                        .group = "Race",
                        .unit = "s",
                        .kind = FT_VALUE_FLOAT,
                        .flags = FT_PROP_SUMMARY | FT_PROP_READ_ONLY_UI},
    [PROP_DEEP_FROZEN] = {.id = "deep_frozen", .display_name = "Deep frozen", .group = "State", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_LIVE_FROZEN] = {.id = "live_frozen", .display_name = "Live frozen", .group = "State", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_JETPACK] = {.id = "jetpack", .display_name = "Jetpack", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_ENDLESS_HOOK] = {.id = "endless_hook", .display_name = "Endless hook", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_SOLO] = {.id = "solo", .display_name = "Solo", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_TELEGUN] = {.id = "telegun", .display_name = "Telegun", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_TELEGRENADE] = {.id = "telegrenade", .display_name = "Telegrenade", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_TELELASER] = {.id = "telelaser", .display_name = "Telelaser", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    // Stored inverted: DDNet disables these, the editor asks whether they work,
    // because "can hook others" reads better on a checkbox than "hook disabled".
    [PROP_COLLIDE_OTHERS] = {.id = "collide_others",
                             .display_name = "Collides with others",
                             .group = "Interaction",
                             .kind = FT_VALUE_BOOL,
                             .flags = DD_PROP_START},
    [PROP_HOOK_OTHERS] = {.id = "hook_others", .display_name = "Hooks others", .group = "Interaction", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAMMER_HITS_OTHERS] = {.id = "hammer_hits_others",
                                 .display_name = "Hammer hits others",
                                 .group = "Interaction",
                                 .kind = FT_VALUE_BOOL,
                                 .flags = DD_PROP_START},
    [PROP_SHOTGUN_HITS_OTHERS] = {.id = "shotgun_hits_others",
                                  .display_name = "Shotgun hits others",
                                  .group = "Interaction",
                                  .kind = FT_VALUE_BOOL,
                                  .flags = DD_PROP_START},
    [PROP_GRENADE_HITS_OTHERS] = {.id = "grenade_hits_others",
                                  .display_name = "Grenade hits others",
                                  .group = "Interaction",
                                  .kind = FT_VALUE_BOOL,
                                  .flags = DD_PROP_START},
    [PROP_LASER_HITS_OTHERS] = {.id = "laser_hits_others",
                                .display_name = "Laser hits others",
                                .group = "Interaction",
                                .kind = FT_VALUE_BOOL,
                                .flags = DD_PROP_START},
};

// Projectiles and lasers, so they can be picked and inspected in the viewport.
// The editor knows none of this: it walks whatever classes a game publishes and
// reads whatever properties they list.
enum ddnet_projectile_prop {
  PROJ_POSITION = 0,
  PROJ_DIRECTION,
  PROJ_TYPE,
  PROJ_OWNER,
  PROJ_START_TICK,
  PROJ_LIFESPAN,
  PROJ_EXPLOSIVE,
  PROJ_FREEZE,
  PROJ_BOUNCING,
  PROJ_PROP_COUNT
};

static const ft_prop_desc projectile_props[PROJ_PROP_COUNT] = {
    [PROJ_POSITION] = {"position", "Position", "Motion", "tiles", FT_VALUE_VEC2, FT_PROP_SUMMARY, 0, 0},
    [PROJ_DIRECTION] = {"direction", "Direction", "Motion", NULL, FT_VALUE_VEC2, 0, 0, 0},
    [PROJ_TYPE] = {"type", "Weapon", "Identity", NULL, FT_VALUE_INT, FT_PROP_SUMMARY, 0, 0},
    [PROJ_OWNER] = {"owner", "Owner", "Identity", NULL, FT_VALUE_INT, 0, 0, 0},
    [PROJ_START_TICK] = {"start_tick", "Start tick", "Timing", NULL, FT_VALUE_INT, 0, 0, 0},
    [PROJ_LIFESPAN] = {"lifespan", "Lifespan", "Timing", "ticks", FT_VALUE_INT, FT_PROP_SUMMARY, 0, 0},
    [PROJ_EXPLOSIVE] = {"explosive", "Explosive", "Behaviour", NULL, FT_VALUE_BOOL, 0, 0, 0},
    [PROJ_FREEZE] = {"freeze", "Freezes", "Behaviour", NULL, FT_VALUE_BOOL, 0, 0, 0},
    [PROJ_BOUNCING] = {"bouncing", "Bouncing", "Behaviour", NULL, FT_VALUE_INT, 0, 0, 0},
};

enum ddnet_laser_prop {
  LASER_POSITION = 0,
  LASER_FROM,
  LASER_ENERGY,
  LASER_BOUNCES,
  LASER_OWNER,
  LASER_TYPE,
  LASER_EVAL_TICK,
  LASER_PROP_COUNT
};

static const ft_prop_desc laser_props[LASER_PROP_COUNT] = {
    [LASER_POSITION] = {"position", "Position", "Motion", "tiles", FT_VALUE_VEC2, FT_PROP_SUMMARY, 0, 0},
    [LASER_FROM] = {"from", "From", "Motion", "tiles", FT_VALUE_VEC2, 0, 0, 0},
    [LASER_ENERGY] = {"energy", "Energy", "Motion", NULL, FT_VALUE_FLOAT, FT_PROP_SUMMARY, 0, 0},
    [LASER_BOUNCES] = {"bounces", "Bounces", "Motion", NULL, FT_VALUE_INT, 0, 0, 0},
    [LASER_OWNER] = {"owner", "Owner", "Identity", NULL, FT_VALUE_INT, 0, 0, 0},
    [LASER_TYPE] = {"type", "Weapon", "Identity", NULL, FT_VALUE_INT, 0, 0, 0},
    [LASER_EVAL_TICK] = {"eval_tick", "Eval tick", "Timing", NULL, FT_VALUE_INT, 0, 0, 0},
};

enum { DD_CLASS_PLAYER = 0,
       DD_CLASS_PROJECTILE,
       DD_CLASS_LASER,
       DD_CLASS_COUNT };

static const ft_entity_class entity_classes[] = {
    [DD_CLASS_PLAYER] = {.id = "player", .display_name = "Tee", .props = player_props, .prop_count = PROP_COUNT},
    [DD_CLASS_PROJECTILE] = {.id = "projectile", .display_name = "Projectile", .props = projectile_props, .prop_count = PROJ_PROP_COUNT},
    [DD_CLASS_LASER] = {.id = "laser", .display_name = "Laser", .props = laser_props, .prop_count = LASER_PROP_COUNT},
};

// Entities hang off the world in singly linked lists, so an index means "the
// nth one still alive".
static bool projectile_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out);
static bool laser_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out);

static SEntity *entity_at(const ft_world *world, int type, int32_t index) {
  if (!world || index < 0) return NULL;
  int32_t seen = 0;
  for (SEntity *ent = world->core.m_apFirstEntityTypes[type]; ent; ent = ent->m_pNextTypeEntity) {
    if (seen++ == index) return ent;
  }
  return NULL;
}

static int32_t entity_list_count(const ft_world *world, int type) {
  if (!world) return 0;
  int32_t count = 0;
  for (SEntity *ent = world->core.m_apFirstEntityTypes[type]; ent; ent = ent->m_pNextTypeEntity)
    ++count;
  return count;
}

static SCharacterCore *character_at(const ft_world *world, int32_t index) {
  if (!world || index < 0 || index >= world->core.m_NumCharacters) return NULL;
  return &world->core.m_pCharacters[index];
}

static bool ddnet_entity_prop_get(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, ft_value *out) {
  (void)game;
  if (entity_class == DD_CLASS_PROJECTILE) return projectile_prop_get(world, entity, prop, out);
  if (entity_class == DD_CLASS_LASER) return laser_prop_get(world, entity, prop, out);
  if (entity_class != FT_ENTITY_CLASS_PLAYER) return false;
  const SCharacterCore *c = character_at(world, entity);
  if (!c || prop >= PROP_COUNT) return false;

  switch (prop) {
  case PROP_POSITION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {vgetx(c->m_Pos) / PX_PER_TILE, vgety(c->m_Pos) / PX_PER_TILE}};
    return true;
  case PROP_VELOCITY:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {vgetx(c->m_Vel), vgety(c->m_Vel)}};
    return true;
  case PROP_ACTIVE_WEAPON:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_ActiveWeapon};
    return true;
  case PROP_HEALTH:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_Health};
    return true;
  case PROP_ARMOR:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_Armor};
    return true;
  case PROP_FREEZE_TIME:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_FreezeTime};
    return true;
  case PROP_HAS_HAMMER:
  case PROP_HAS_GUN:
  case PROP_HAS_SHOTGUN:
  case PROP_HAS_GRENADE:
  case PROP_HAS_LASER:
  case PROP_HAS_NINJA:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_aWeaponGot[prop - PROP_HAS_HAMMER]};
    return true;
  case PROP_JUMPS:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_Jumps};
    return true;
  case PROP_JUMPS_LEFT:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_JumpedTotal};
    return true;
  case PROP_ENDLESS_JUMP:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_EndlessJump};
    return true;
  case PROP_GROUNDED:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_Grounded};
    return true;
  case PROP_HOOK_STATE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_HookState};
    return true;
  case PROP_HOOKED_PLAYER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->m_HookedPlayer};
    return true;
  case PROP_RACE_TIME:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = c->m_RaceTime};
    return true;
  case PROP_DEEP_FROZEN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_DeepFrozen};
    return true;
  case PROP_JETPACK:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_Jetpack};
    return true;
  case PROP_ENDLESS_HOOK:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_EndlessHook};
    return true;
  case PROP_SOLO:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_Solo};
    return true;
  case PROP_LIVE_FROZEN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_LiveFrozen};
    return true;
  case PROP_TELEGUN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_HasTelegunGun};
    return true;
  case PROP_TELEGRENADE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_HasTelegunGrenade};
    return true;
  case PROP_TELELASER:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->m_HasTelegunLaser};
    return true;
  // The physics stores these as things that are switched off; the editor shows
  // them as things that work, so both sides read the way their owner thinks.
  case PROP_COLLIDE_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_CollisionDisabled};
    return true;
  case PROP_HOOK_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_HookHitDisabled};
    return true;
  case PROP_HAMMER_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_HammerHitDisabled};
    return true;
  case PROP_SHOTGUN_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_ShotgunHitDisabled};
    return true;
  case PROP_GRENADE_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_GrenadeHitDisabled};
    return true;
  case PROP_LASER_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->m_LaserHitDisabled};
    return true;
  default:
    return false;
  }
}

static bool ddnet_entity_prop_set(ft_game *game, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, const ft_value *value) {
  (void)game;
  if (entity_class != FT_ENTITY_CLASS_PLAYER) return false;
  SCharacterCore *c = character_at(world, entity);
  if (!c || prop >= PROP_COUNT || !value) return false;

  switch (prop) {
  case PROP_POSITION:
    c->m_Pos = vec2_init(value->as.v.x * PX_PER_TILE, value->as.v.y * PX_PER_TILE);
    c->m_PrevPos = c->m_Pos;
    // Tile lookups are cached per position, so they have to be refreshed or the
    // tee keeps colliding against wherever it used to be.
    cc_calc_indices(c);
    return true;
  case PROP_VELOCITY:
    c->m_Vel = vec2_init(value->as.v.x, value->as.v.y);
    return true;
  case PROP_ACTIVE_WEAPON:
    if (value->as.i < 0 || value->as.i >= NUM_WEAPONS) return false;
    c->m_ActiveWeapon = (unsigned char)value->as.i;
    c->m_aWeaponGot[value->as.i] = true;
    return true;
  case PROP_HEALTH:
    c->m_Health = (int8_t)value->as.i;
    return true;
  case PROP_ARMOR:
    c->m_Armor = (int8_t)value->as.i;
    return true;
  case PROP_FREEZE_TIME:
    c->m_FreezeTime = (int)value->as.i;
    return true;
  case PROP_HAS_HAMMER:
  case PROP_HAS_GUN:
  case PROP_HAS_SHOTGUN:
  case PROP_HAS_GRENADE:
  case PROP_HAS_LASER:
  case PROP_HAS_NINJA: {
    const int weapon = (int)prop - PROP_HAS_HAMMER;
    c->m_aWeaponGot[weapon] = value->as.b;
    // A tee cannot hold a weapon it does not have; fall back to the hammer,
    // which every tee keeps.
    if (!value->as.b && c->m_ActiveWeapon == weapon) c->m_ActiveWeapon = WEAPON_HAMMER;
    return true;
  }
  case PROP_JUMPS:
    if (value->as.i < 0 || value->as.i > 255) return false;
    c->m_Jumps = (int)value->as.i;
    return true;
  case PROP_JUMPS_LEFT:
    c->m_JumpedTotal = (int)value->as.i;
    return true;
  case PROP_ENDLESS_JUMP:
    c->m_EndlessJump = value->as.b;
    return true;
  case PROP_DEEP_FROZEN:
    c->m_DeepFrozen = value->as.b;
    return true;
  case PROP_JETPACK:
    c->m_Jetpack = value->as.b;
    return true;
  case PROP_ENDLESS_HOOK:
    c->m_EndlessHook = value->as.b;
    return true;
  case PROP_SOLO:
    c->m_Solo = value->as.b;
    return true;
  case PROP_LIVE_FROZEN:
    c->m_LiveFrozen = value->as.b;
    return true;
  case PROP_TELEGUN:
    c->m_HasTelegunGun = value->as.b;
    return true;
  case PROP_TELEGRENADE:
    c->m_HasTelegunGrenade = value->as.b;
    return true;
  case PROP_TELELASER:
    c->m_HasTelegunLaser = value->as.b;
    return true;
  case PROP_COLLIDE_OTHERS:
    c->m_CollisionDisabled = !value->as.b;
    return true;
  case PROP_HOOK_OTHERS:
    c->m_HookHitDisabled = !value->as.b;
    return true;
  case PROP_HAMMER_HITS_OTHERS:
    c->m_HammerHitDisabled = !value->as.b;
    return true;
  case PROP_SHOTGUN_HITS_OTHERS:
    c->m_ShotgunHitDisabled = !value->as.b;
    return true;
  case PROP_GRENADE_HITS_OTHERS:
    c->m_GrenadeHitDisabled = !value->as.b;
    return true;
  case PROP_LASER_HITS_OTHERS:
    c->m_LaserHitDisabled = !value->as.b;
    return true;
  default:
    return false;
  }
}

static int32_t ddnet_entity_count(ft_game *game, const ft_world *world, uint32_t entity_class) {
  (void)game;
  if (!world) return 0;
  switch (entity_class) {
  case DD_CLASS_PLAYER:
    return world->core.m_NumCharacters;
  case DD_CLASS_PROJECTILE:
    return entity_list_count(world, WORLD_ENTTYPE_PROJECTILE);
  case DD_CLASS_LASER:
    return entity_list_count(world, WORLD_ENTTYPE_LASER);
  default:
    return 0;
  }
}

static bool projectile_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out) {
  SProjectile *proj = (SProjectile *)entity_at(world, WORLD_ENTTYPE_PROJECTILE, entity);
  if (!proj) return false;

  switch (prop) {
  case PROJ_POSITION: {
    // Where it is *now*, which for a projectile is a function of flight time
    // rather than a stored position.
    const float time = (world->core.m_GameTick - proj->m_StartTick) / (float)GAME_TICK_SPEED;
    const mvec2 pos = prj_get_pos(proj, time);
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {vgetx(pos) / PX_PER_TILE, vgety(pos) / PX_PER_TILE}};
    return true;
  }
  case PROJ_DIRECTION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {vgetx(proj->m_Direction), vgety(proj->m_Direction)}};
    return true;
  case PROJ_TYPE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->m_Type};
    return true;
  case PROJ_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->m_Owner};
    return true;
  case PROJ_START_TICK:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->m_StartTick};
    return true;
  case PROJ_LIFESPAN:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->m_LifeSpan};
    return true;
  case PROJ_EXPLOSIVE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = proj->m_Explosive};
    return true;
  case PROJ_FREEZE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = proj->m_Freeze};
    return true;
  case PROJ_BOUNCING:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->m_Bouncing};
    return true;
  default:
    return false;
  }
}

static bool laser_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out) {
  SLaser *laser = (SLaser *)entity_at(world, WORLD_ENTTYPE_LASER, entity);
  if (!laser) return false;

  switch (prop) {
  case LASER_POSITION:
    *out = (ft_value){.kind = FT_VALUE_VEC2,
                      .as.v = {vgetx(laser->m_Base.m_Pos) / PX_PER_TILE, vgety(laser->m_Base.m_Pos) / PX_PER_TILE}};
    return true;
  case LASER_FROM:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {vgetx(laser->m_From) / PX_PER_TILE, vgety(laser->m_From) / PX_PER_TILE}};
    return true;
  case LASER_ENERGY:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = laser->m_Energy};
    return true;
  case LASER_BOUNCES:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->m_Bounces};
    return true;
  case LASER_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->m_Owner};
    return true;
  case LASER_TYPE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->m_Type};
    return true;
  case LASER_EVAL_TICK:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->m_EvalTick};
    return true;
  default:
    return false;
  }
}

// -----------------------------------------------------------------------------
// Levels
// -----------------------------------------------------------------------------

static const ft_game_variant variants[] = {
    {.id = "ddrace", .display_name = "DDRace"},
    {.id = "race", .display_name = "Race"},
    {.id = "fastcap", .display_name = "FastCap"},
    {.id = "fastcap_no_weapons", .display_name = "FastCap (no weapons)"},
};

static EGameMode variant_to_mode(const char *variant_id) {
  if (!variant_id) return GAME_MODE_DDRACE;
  if (strcmp(variant_id, "race") == 0) return GAME_MODE_RACE;
  if (strcmp(variant_id, "fastcap") == 0) return GAME_MODE_FASTCAP;
  if (strcmp(variant_id, "fastcap_no_weapons") == 0) return GAME_MODE_FASTCAP_NO_WPNS;
  return GAME_MODE_DDRACE;
}

static ft_level *level_finish(ft_game *game, map_data_t *map, const char *variant_id, const char *name) {
  ft_level *level = calloc(1, sizeof(ft_level));
  if (!level) {
    free_map_data(map);
    return NULL;
  }

  level->mode = variant_to_mode(variant_id);
  if (!init_game_mode(&level->prototype, &level->collision, &level->grid, &level->config, map, level->mode)) {
    dd_log(game, FT_LOG_ERROR, "Map '%s' could not be prepared for %s.", name ? name : "?", variant_id ? variant_id : "ddrace");
    free_map_data(map);
    free(level);
    return NULL;
  }

  snprintf(level->name, sizeof(level->name), "%s", name ? name : "map");
  dd_level_build_pickups(level);
  dd_map_create(game, level);
  level->loaded = true;
  for (int i = 0; i < game->particle_count; ++i)
    dd_particles_reset(&game->particles[i]);
  if (game->preserve_demo_export_on_level_load)
    game->preserve_demo_export_on_level_load = false;
  else
    dd_export_window_cleanup(game);
  game->current_level = level;
  return level;
}

static void strip_to_stem(const char *path, char *out, size_t out_size) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) slash = back;
#endif
  const char *file = slash ? slash + 1 : path;
  snprintf(out, out_size, "%s", file);
  char *dot = strrchr(out, '.');
  if (dot && dot != out) *dot = '\0';
}

static ft_level *ddnet_level_load_path(ft_game *game, const char *path, const char *variant_id) {
  if (!path) return NULL;
  map_data_t map = load_map(path);
  if (!map.game_layer.data) {
    dd_log(game, FT_LOG_ERROR, "Failed to read map '%s'.", path);
    return NULL;
  }
  char name[128];
  strip_to_stem(path, name, sizeof(name));
  return level_finish(game, &map, variant_id, name);
}

static ft_level *ddnet_level_load_memory(ft_game *game, const void *data, size_t size, const char *variant_id) {
  if (!data || size == 0) return NULL;
  // The DDNet map loader takes ownership of its input buffer. Project data is
  // engine-owned and released as soon as loading finishes, so hand the loader
  // a private copy instead of leaving the level with a dangling/double-owned
  // map pointer.
  unsigned char *owned_data = malloc(size);
  if (!owned_data) return NULL;
  memcpy(owned_data, data, size);
  map_data_t map = load_map_from_memory(owned_data, size);
  if (!map.game_layer.data) {
    free(owned_data);
    dd_log(game, FT_LOG_ERROR, "Failed to parse an in-memory map of %zu bytes.", size);
    return NULL;
  }
  return level_finish(game, &map, variant_id, "map");
}

static void ddnet_level_destroy(ft_game *game, ft_level *level) {
  if (!level) return;
  if (game->current_level == level) game->current_level = NULL;
  dd_map_destroy(game, level);
  free(level->pickups);
  free(level->pickup_positions);
  free(level->pickup_cooldown_keys);
  free(level->ninja_pickup_indices);
  tg_destroy(&level->grid);
  wc_free(&level->prototype);
  free_collision(&level->collision);
  free(level);
}

// The map file is already held contiguously by the loader, so embedding a level
// in a project is a straight copy of those bytes.
static size_t ddnet_level_serialize(ft_game *game, const ft_level *level, void *out, size_t out_size) {
  (void)game;
  if (!level || !level->loaded) return 0;
  const map_data_t *map = &level->collision.m_MapData;
  if (!map->_map_file_data || map->_map_file_size == 0) return 0;
  if (!out) return map->_map_file_size;
  if (out_size < map->_map_file_size) return 0;
  memcpy(out, map->_map_file_data, map->_map_file_size);
  return map->_map_file_size;
}

static bool ddnet_level_info(ft_game *game, const ft_level *level, ft_level_info *out) {
  (void)game;
  if (!level || !level->loaded || !out) return false;
  const map_data_t *map = &level->collision.m_MapData;
  out->name = level->name;
  out->width_tiles = map->width;
  out->height_tiles = map->height;
  out->bounds = (ft_rect){0.f, 0.f, (float)map->width, (float)map->height};
  out->default_spawn = (ft_vec2){(float)map->width * 0.5f, (float)map->height * 0.5f};
  return true;
}

// -----------------------------------------------------------------------------
// Worlds
// -----------------------------------------------------------------------------

static ft_world *ddnet_world_create(ft_game *game, const ft_world_desc *desc) {
  if (!desc || !desc->level || !desc->level->loaded) return NULL;
  ft_level *level = (ft_level *)desc->level;

  ft_world *world = calloc(1, sizeof(ft_world));
  if (!world) return NULL;
  world->core = wc_empty();
  world->level = level;
  world->game = game;
  world->index = desc->world_index;
  wc_copy_world(&world->core, &level->prototype);

  const int32_t wanted = desc->player_count > 0 ? desc->player_count : 0;
  if (wanted > world->core.m_NumCharacters) {
    if (!wc_add_character(&world->core, wanted - world->core.m_NumCharacters)) {
      dd_log(game, FT_LOG_ERROR, "Could not create %d characters.", wanted);
    }
  }
  return world;
}

static void ddnet_world_destroy(ft_game *game, ft_world *world) {
  (void)game;
  if (!world) return;
  wc_free(&world->core);
  free(world->physics_particle_events);
  free(world->physics_damage_events);
  free(world->physics_sound_events);
  free(world);
}

static bool copy_effect_events(void **destination, int *capacity, const void *source, int count, size_t item_size) {
  if (count <= 0) return true;
  if (*capacity < count) {
    void *events = realloc(*destination, (size_t)count * item_size);
    if (!events) return false;
    *destination = events;
    *capacity = count;
  }
  memcpy(*destination, source, (size_t)count * item_size);
  return true;
}

static void ddnet_world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  if (!dst || !src) return;
  const int world_index = dst->index;
  dst->level = src->level;
  dst->game = game;
  // Keep the destination's identity. Prediction worlds use index -1 so their
  // speculative ticks cannot emit particles into a visible simulation group.
  dst->index = world_index;
  // wc_copy_world reuses whatever dst already allocated, which is what keeps
  // the engine's constant snapshotting affordable.
  wc_copy_world(&dst->core, (SWorldCore *)&src->core);
  dst->physics_particle_event_count =
      copy_effect_events((void **)&dst->physics_particle_events, &dst->physics_particle_event_capacity,
                         src->physics_particle_events, src->physics_particle_event_count,
                         sizeof(*dst->physics_particle_events))
          ? src->physics_particle_event_count
          : 0;
  dst->physics_damage_event_count =
      copy_effect_events((void **)&dst->physics_damage_events, &dst->physics_damage_event_capacity,
                         src->physics_damage_events, src->physics_damage_event_count, sizeof(*dst->physics_damage_events))
          ? src->physics_damage_event_count
          : 0;
  dst->physics_sound_event_count =
      copy_effect_events((void **)&dst->physics_sound_events, &dst->physics_sound_event_capacity,
                         src->physics_sound_events, src->physics_sound_event_count, sizeof(*dst->physics_sound_events))
          ? src->physics_sound_event_count
          : 0;
  dst->render_physics_effects = false;
  dst->core.user_data = NULL;
  dst->core.particle = NULL;
  dst->core.damage_indicator = NULL;
  dst->core.sound = NULL;
}

static void ddnet_world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  if (!world) return;
  // Effects raised during the tick land in this world's particle system.
  const int tick_before = world->core.m_GameTick;
  const bool effects_bound = dd_particles_bind(game, world);
  const SPlayerInput *records = inputs;
  const int count = world->core.m_NumCharacters;
  for (int i = 0; i < count; ++i) {
    // Players the engine has no input for keep holding their last one, which is
    // how a track that ran out of snippets behaves in DDNet.
    if (records && (uint32_t)i < player_count)
      cc_on_input(&world->core.m_pCharacters[i], &records[i]);
    else
      cc_on_input(&world->core.m_pCharacters[i], &world->core.m_pCharacters[i].m_Input);
  }
  wc_tick(&world->core);
  dd_particles_finish(game, world, tick_before, effects_bound);
}

static int32_t ddnet_world_tick(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? world->core.m_GameTick : 0;
}

static int32_t ddnet_world_player_count(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? world->core.m_NumCharacters : 0;
}

static bool ddnet_world_player_view(ft_game *game, const ft_world *world, int32_t player, ft_player_view *out) {
  (void)game;
  const SCharacterCore *c = character_at(world, player);
  if (!c || !out) return false;

  out->position = (ft_vec2){vgetx(c->m_Pos) / PX_PER_TILE, vgety(c->m_Pos) / PX_PER_TILE};
  out->velocity = (ft_vec2){vgetx(c->m_Vel), vgety(c->m_Vel)};
  out->aim = (ft_vec2){(float)c->m_Input.m_TargetX / PX_PER_TILE, (float)c->m_Input.m_TargetY / PX_PER_TILE};
  out->flags = FT_PLAYER_ALIVE;
  if (c->m_FreezeTime > 0 || c->m_DeepFrozen) out->flags |= FT_PLAYER_DISABLED;
  if (c->m_RaceTime >= 0.f) out->flags |= FT_PLAYER_FINISHED;
  out->run_start_tick = c->m_StartTick;
  return true;
}

static void ddnet_collect_events(ft_game *game, const ft_world *previous, const ft_world *world,
                                 void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  (void)game;
  if (!previous || !world || !emit) return;

  const int count = world->core.m_NumCharacters < previous->core.m_NumCharacters ? world->core.m_NumCharacters
                                                                                 : previous->core.m_NumCharacters;
  for (int player = 0; player < count; ++player) {
    const SCharacterCore *before = &previous->core.m_pCharacters[player];
    const SCharacterCore *after = &world->core.m_pCharacters[player];

    if (after->m_LastTimeCp >= 0 && after->m_LastTimeCp < NUM_TIME_CHECKPOINTS && after->m_LastTimeCp != before->m_LastTimeCp) {
      char text[64];
      snprintf(text, sizeof(text), "Checkpoint %d: %.3fs", after->m_LastTimeCp + 1, after->m_aTimeCp[after->m_LastTimeCp]);
      const ft_timeline_event event = {.struct_size = sizeof(ft_timeline_event),
                                       .tick = world->core.m_GameTick,
                                       .player = player,
                                       .category = "checkpoint",
                                       .text = text,
                                       .color = {0.35f, 0.75f, 1.0f, 1.0f}};
      emit(user, &event);
    }

    if (before->m_FinishTick < 0 && after->m_FinishTick >= 0) {
      char text[64];
      snprintf(text, sizeof(text), "Finish: %.3fs", after->m_RaceTime >= 0.f ? after->m_RaceTime : 0.f);
      const ft_timeline_event event = {.struct_size = sizeof(ft_timeline_event),
                                       .tick = world->core.m_GameTick,
                                       .player = player,
                                       .category = "finish",
                                       .text = text,
                                       .color = {0.35f, 1.0f, 0.5f, 1.0f}};
      emit(user, &event);
    }
  }
}

static void ddnet_linked_input_update(ft_game *game, const ft_linked_input_frame *frame, void *inout_record) {
  (void)game;
  if (!frame || !frame->world || !inout_record || (frame->actions_down & (UINT64_C(1) << LINKED_AIM_AT_SOURCE)) == 0) return;
  const SCharacterCore *source = character_at(frame->world, frame->source_player);
  const SCharacterCore *target = character_at(frame->world, frame->target_player);
  if (!source || !target) return;
  SPlayerInput *input = inout_record;
  input->m_TargetX = (int16_t)(vgetx(source->m_Pos) - vgetx(target->m_Pos));
  input->m_TargetY = (int16_t)(vgety(source->m_Pos) - vgety(target->m_Pos));
}

static int32_t ddnet_world_add_player(ft_game *game, ft_world *world, int32_t at_index, const ft_player_setup *setup) {
  (void)setup;
  if (!world) return -1;
  if (!wc_add_character(&world->core, 1)) {
    dd_log(game, FT_LOG_ERROR, "Adding a character failed.");
    return -1;
  }

  const int last = world->core.m_NumCharacters - 1;
  if (at_index < 0 || at_index >= last) return last;

  // Track order is the user's, so a character inserted in the middle has to be
  // rotated into place and every id below it renumbered.
  SCharacterCore moved = world->core.m_pCharacters[last];
  memmove(&world->core.m_pCharacters[at_index + 1], &world->core.m_pCharacters[at_index], sizeof(SCharacterCore) * (last - at_index));
  world->core.m_pCharacters[at_index] = moved;
  for (int i = 0; i < world->core.m_NumCharacters; ++i) {
    world->core.m_pCharacters[i].m_Id = i;
    cc_calc_indices(&world->core.m_pCharacters[i]);
  }
  return at_index;
}

static bool ddnet_world_remove_player(ft_game *game, ft_world *world, int32_t player) {
  (void)game;
  if (!world || player < 0 || player >= world->core.m_NumCharacters) return false;
  wc_remove_character(&world->core, player);
  for (int i = 0; i < world->core.m_NumCharacters; ++i)
    world->core.m_pCharacters[i].m_Id = i;
  return true;
}

// --- serialization -----------------------------------------------------------
//
// Only what a starting state needs: the tick and the characters. Entities in
// flight (projectiles, lasers) are deliberately dropped, because a project
// stores a point to simulate from, not a mid-flight snapshot.

#define DDNET_STATE_MAGIC 0x444E5731u /* "DDNW1" */

typedef struct {
  uint32_t magic;
  uint32_t version;
  int32_t game_tick;
  int32_t character_count;
  uint32_t character_size;
} ddnet_state_header;

static size_t ddnet_world_serialize(ft_game *game, const ft_world *world, void *out, size_t out_size) {
  (void)game;
  if (!world) return 0;
  const size_t needed = sizeof(ddnet_state_header) + (size_t)world->core.m_NumCharacters * sizeof(SCharacterCore);
  if (!out) return needed;
  if (out_size < needed) return 0;

  ddnet_state_header header = {.magic = DDNET_STATE_MAGIC,
                               .version = 1,
                               .game_tick = world->core.m_GameTick,
                               .character_count = world->core.m_NumCharacters,
                               .character_size = (uint32_t)sizeof(SCharacterCore)};
  memcpy(out, &header, sizeof(header));
  if (world->core.m_NumCharacters > 0)
    memcpy((char *)out + sizeof(header), world->core.m_pCharacters, (size_t)world->core.m_NumCharacters * sizeof(SCharacterCore));
  return needed;
}

static bool ddnet_world_deserialize(ft_game *game, ft_world *world, const void *data, size_t size) {
  if (!world || !data || size < sizeof(ddnet_state_header)) return false;

  ddnet_state_header header;
  memcpy(&header, data, sizeof(header));
  if (header.magic != DDNET_STATE_MAGIC || header.version != 1) return false;
  // A character grew or shrank between builds, so the bytes cannot be trusted.
  if (header.character_size != sizeof(SCharacterCore)) {
    dd_log(game, FT_LOG_WARN, "Stored world was written by a different physics build; ignoring it.");
    return false;
  }
  if (header.character_count < 0) return false;
  if (size < sizeof(header) + (size_t)header.character_count * sizeof(SCharacterCore)) return false;

  while (world->core.m_NumCharacters > header.character_count)
    wc_remove_character(&world->core, world->core.m_NumCharacters - 1);
  if (header.character_count > world->core.m_NumCharacters)
    if (!wc_add_character(&world->core, header.character_count - world->core.m_NumCharacters)) return false;

  const SCharacterCore *stored = (const SCharacterCore *)((const char *)data + sizeof(header));
  for (int i = 0; i < header.character_count; ++i) {
    SCharacterCore *live = &world->core.m_pCharacters[i];
    // Everything except the back-pointers, which belong to this world and would
    // be dangling if they came out of the file.
    struct WorldCore *world_ptr = live->m_pWorld;
    SCollision *collision = live->m_pCollision;
    STuningParams *tuning = live->m_pTuning;
    *live = stored[i];
    live->m_pWorld = world_ptr;
    live->m_pCollision = collision;
    live->m_pTuning = tuning;
    live->m_Id = i;
    cc_calc_indices(live);
  }
  world->core.m_GameTick = header.game_tick;
  world->physics_particle_event_count = 0;
  world->physics_damage_event_count = 0;
  world->physics_sound_event_count = 0;
  world->render_physics_effects = false;
  world->core.user_data = NULL;
  world->core.particle = NULL;
  world->core.damage_indicator = NULL;
  world->core.sound = NULL;
  return true;
}

// -----------------------------------------------------------------------------
// Module lifecycle
// -----------------------------------------------------------------------------

static uint32_t ddnet_setting_count(ft_game *game);
static const ft_setting_desc *ddnet_setting_desc(ft_game *game, uint32_t index);
static bool ddnet_setting_get(ft_game *game, uint32_t index, ft_value *out);
static bool ddnet_setting_set(ft_game *game, uint32_t index, const ft_value *value);

static const ft_exporter_desc ddnet_exporter = {.id = "demo",
                                                .display_name = "DDNet Demo",
                                                .file_extension = "demo",
                                                .filter_name = "DDNet Demo"};

static uint32_t ddnet_exporter_count(ft_game *game) {
  (void)game;
  return 1;
}

static const ft_exporter_desc *ddnet_exporter_desc(ft_game *game, uint32_t index) {
  (void)game;
  return index == 0 ? &ddnet_exporter : NULL;
}

static bool ddnet_export_run(ft_game *game, uint32_t index, const ft_export_request *request) {
  return index == 0 && dd_demo_export(game, request);
}

static ft_game *ddnet_create(const ft_engine_api *engine) {
  ft_game *game = calloc(1, sizeof(ft_game));
  if (!game) return NULL;
  game->engine = engine;

  // Presentation defaults. These are the game's, not the editor's, which is why
  // they no longer sit in the engine's ui_handler_t.
  game->settings = (dd_settings_t){.render_players = true,
                                   .render_weapons = true,
                                   .render_particles = true,
                                   .render_pickups = true,
                                   .render_cursor_follow = true,
                                   .center_dot = false,
                                   .cursor_scale = 1.0f};
  game->auto_finish_events = true;

  ft_engine_state state;
  memset(&state, 0, sizeof(state));
  if (engine && engine->get_state) engine->get_state(&state);
  game->headless = state.headless;

  dd_log(game, FT_LOG_INFO, "DDNet game module ready (ABI %u).", FT_GAME_ABI_VERSION);
  return game;
}

static void ddnet_destroy(ft_game *game) {
  if (!game) return;
  dd_player_panel_cleanup(game);
  dd_skin_browser_cleanup(game);
  dd_export_window_cleanup(game);
  for (int i = 0; i < game->particle_count; ++i)
    dd_particles_cleanup(&game->particles[i]);
  free(game->particles);
  free(game);
}

static bool ddnet_resources_create(ft_game *game) { return dd_gfx_create(game); }

static void ddnet_resources_destroy(ft_game *game) {
  // Destroying the custom pipeline waits for outstanding GPU work. Do that
  // before releasing browser/map ImGui descriptors which may have appeared in
  // the preceding frame.
  dd_gfx_destroy(game);
  if (game->maps) {
    online_map_manager_cleanup(game->maps, game);
    free(game->maps);
    game->maps = NULL;
  }
  dd_player_panel_cleanup(game);
  dd_skin_browser_cleanup(game);
}

// Where this game's windows want to open the first time they are ever seen.
// The player panel takes the spot beside the editor's player list, which is
// where the editor's own one used to sit.
static const ft_panel_desc ddnet_panels[] = {
    {.window_title = "Player Info", .dock = FT_DOCK_LEFT},
    {.window_title = "Skin Browser", .dock = FT_DOCK_RIGHT},
};

// DDNet's start screen is its map browser: picking a map is how a run begins.
// The editor hands over the panel and this game fills it.
static void ddnet_ui(ft_game *game, const ft_ui_frame *frame) {
  if (game->headless || !frame) return;
  dd_imgui_attach(game->engine);

  switch (frame->slot) {
  case FT_UI_MAIN_MENU:
    if (igBeginMenu("DDNet", true)) {
      igMenuItem_BoolPtr("Skin Browser", NULL, &game->show_skin_browser, true);
      igMenuItem_BoolPtr("Timeline Events", NULL, &game->show_events, true);
      if (igMenuItem_Bool("Export Demo...", NULL, false, game->current_level != NULL)) dd_export_window_open(game);
      igEndMenu();
    }
    break;
  case FT_UI_PANELS:
    dd_player_panel_render(game, frame);
    if (game->show_skin_browser) dd_skin_browser_render(game, frame);
    // Finish-event generation follows recording even while its editor is
    // closed, so let the manager handle this slot every frame.
    dd_events_render(game, frame);
    dd_export_window_render(game);
    break;
  case FT_UI_SPLASH: {
    if (!game->maps) {
      game->maps = calloc(1, sizeof(*game->maps));
      if (game->maps) online_map_manager_init(game->maps, game);
    }
    if (!game->maps) break;
    const ImVec2 avail = igGetContentRegionAvail();
    render_online_map_browser(game, game->maps, avail.x, avail.y);
    break;
  }
  default:
    break;
  }
}

// Keeps one particle system per world the editor is showing.
static void ensure_particle_systems(ft_game *game, int count) {
  if (count <= game->particle_count) return;
  dd_particle_system_t *grown = realloc(game->particles, (size_t)count * sizeof(*grown));
  if (!grown) return;
  game->particles = grown;
  for (int i = game->particle_count; i < count; ++i)
    dd_particles_init(&game->particles[i]);
  game->particle_count = count;
}

static void ddnet_render(ft_game *game, const ft_render_frame *frame) {
  if (frame && frame->world_index >= 0) ensure_particle_systems(game, frame->world_index + 1);
  dd_render(game, frame);
}

static const ft_game_module module = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,

    .info = {.struct_size = sizeof(ft_game_info),
             .id = "ddnet",
             .display_name = "DDNet",
             .version = "1.0.0",
             .author = "Teero",
             .url = "https://ddnet.org",
             .thumbnail = "thumbnail.png"},

    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_DYNAMIC_PLAYERS | FT_CAP_LINKED_INPUTS | FT_CAP_WORLD_SERIALIZE | FT_CAP_EXPORTERS |
                            FT_CAP_LEVEL_FROM_MEMORY | FT_CAP_TIMELINE_EVENTS | FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS |
                            FT_CAP_HOSTS_STARTING_STATE,
                    .min_players = 0,
                    .max_players = 64,
                    .ticks_per_second = 50,
                    .units_per_tile = 1.f,
                    .default_camera_height = 20.f,
                    .variants = variants,
                    .variant_count = (uint32_t)(sizeof(variants) / sizeof(variants[0])),
                    .camera_modes = dd_camera_modes,
                    .camera_mode_count = DD_CAMERA_MODE_COUNT,
                    .level_extension = "map",
                    .level_filter_name = "DDNet map"},

    .input_schema = &input_schema,
    .entity_classes = entity_classes,
    .entity_class_count = DD_CLASS_COUNT,

    .create = ddnet_create,
    .destroy = ddnet_destroy,

    .level_load_path = ddnet_level_load_path,
    .level_load_memory = ddnet_level_load_memory,
    .level_destroy = ddnet_level_destroy,
    .level_info = ddnet_level_info,
    .level_serialize = ddnet_level_serialize,

    .world_create = ddnet_world_create,
    .world_destroy = ddnet_world_destroy,
    .world_copy = ddnet_world_copy,
    .world_step = ddnet_world_step,
    .world_tick = ddnet_world_tick,
    .world_player_count = ddnet_world_player_count,
    .world_player_view = ddnet_world_player_view,
    .world_add_player = ddnet_world_add_player,
    .world_remove_player = ddnet_world_remove_player,
    .world_serialize = ddnet_world_serialize,
    .world_deserialize = ddnet_world_deserialize,

    .input_default = ddnet_input_default,
    .input_get = ddnet_input_get,
    .input_set = ddnet_input_set,
    .input_get_vec2 = ddnet_input_get_vec2,
    .input_set_vec2 = ddnet_input_set_vec2,
    .input_describe = ddnet_input_describe,

    .entity_prop_get = ddnet_entity_prop_get,
    .entity_prop_set = ddnet_entity_prop_set,
    .entity_count = ddnet_entity_count,

    .render = ddnet_render,
    .resources_create = ddnet_resources_create,
    .resources_destroy = ddnet_resources_destroy,
    .ui = ddnet_ui,
    .panels = ddnet_panels,
    .panel_count = (uint32_t)(sizeof(ddnet_panels) / sizeof(ddnet_panels[0])),
    .collect_events = ddnet_collect_events,

    .exporter_count = ddnet_exporter_count,
    .exporter_desc = ddnet_exporter_desc,
    .export_run = ddnet_export_run,

    .player_label = dd_player_label,
    .status_lines = dd_status_lines,
    .camera_update = dd_camera_update,

    .setting_count = ddnet_setting_count,
    .setting_desc = ddnet_setting_desc,
    .setting_get = ddnet_setting_get,
    .setting_set = ddnet_setting_set,

    .project_save = dd_export_project_save,
    .project_load = dd_export_project_load,

    .input_get_float = NULL,
    .input_set_float = NULL,
    .linked_actions = linked_actions,
    .linked_action_count = (uint32_t)(sizeof(linked_actions) / sizeof(linked_actions[0])),
    .linked_input_update = ddnet_linked_input_update,
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  if (engine_abi_version != FT_GAME_ABI_VERSION) return NULL;
  return &module;
}

// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------
//
// How DDNet draws itself. These used to sit in the editor's own graphics menu,
// which meant the editor knew what a tee and a hook were. The editor now renders
// whatever this table describes and stores the values under this game's id.

enum ddnet_setting {
  SET_RENDER_PLAYERS = 0,
  SET_RENDER_WEAPONS,
  SET_RENDER_PARTICLES,
  SET_RENDER_PICKUPS,
  SET_CURSOR_SCALE,
  SET_CURSOR_FOLLOW,
  SET_CENTER_DOT,
  SET_AUTO_FINISH_EVENTS,
  SET_COUNT
};

static const ft_setting_desc ddnet_settings[SET_COUNT] = {
    [SET_RENDER_PLAYERS] = {"render_players", "Tees", NULL, "Rendering", FT_VALUE_BOOL, 0, 0},
    [SET_RENDER_WEAPONS] = {"render_weapons", "Weapons and hooks", NULL, "Rendering", FT_VALUE_BOOL, 0, 0},
    [SET_RENDER_PARTICLES] = {"render_particles", "Particles", NULL, "Rendering", FT_VALUE_BOOL, 0, 0},
    [SET_RENDER_PICKUPS] = {"render_pickups", "Pickups", NULL, "Rendering", FT_VALUE_BOOL, 0, 0},
    [SET_CURSOR_SCALE] = {"cursor_scale", "Crosshair scale", NULL, "Crosshair", FT_VALUE_FLOAT, 0.1, 2.0},
    [SET_CURSOR_FOLLOW] = {"cursor_follow", "Crosshair in follow camera", NULL, "Crosshair", FT_VALUE_BOOL, 0, 0},
    [SET_CENTER_DOT] = {"center_dot", "Show center dot", "Marks the tee's exact position", "Rendering", FT_VALUE_BOOL, 0, 0},
    [SET_AUTO_FINISH_EVENTS] = {"auto_finish_events", "Generate finish events while recording", NULL, "Timeline events",
                                FT_VALUE_BOOL, 0, 0},
};

static uint32_t ddnet_setting_count(ft_game *game) {
  (void)game;
  return SET_COUNT;
}

static const ft_setting_desc *ddnet_setting_desc(ft_game *game, uint32_t index) {
  (void)game;
  return index < SET_COUNT ? &ddnet_settings[index] : NULL;
}

static bool ddnet_setting_get(ft_game *game, uint32_t index, ft_value *out) {
  const dd_settings_t *s = &game->settings;
  switch (index) {
  case SET_RENDER_PLAYERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_players};
    return true;
  case SET_RENDER_WEAPONS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_weapons};
    return true;
  case SET_RENDER_PARTICLES:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_particles};
    return true;
  case SET_RENDER_PICKUPS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_pickups};
    return true;
  case SET_CURSOR_SCALE:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = s->cursor_scale};
    return true;
  case SET_CURSOR_FOLLOW:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_cursor_follow};
    return true;
  case SET_CENTER_DOT:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->center_dot};
    return true;
  case SET_AUTO_FINISH_EVENTS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = game->auto_finish_events};
    return true;
  default:
    return false;
  }
}

static bool ddnet_setting_set(ft_game *game, uint32_t index, const ft_value *value) {
  dd_settings_t *s = &game->settings;
  switch (index) {
  case SET_RENDER_PLAYERS:
    s->render_players = value->as.b;
    return true;
  case SET_RENDER_WEAPONS:
    s->render_weapons = value->as.b;
    return true;
  case SET_RENDER_PARTICLES:
    s->render_particles = value->as.b;
    return true;
  case SET_RENDER_PICKUPS:
    s->render_pickups = value->as.b;
    return true;
  case SET_CURSOR_SCALE:
    s->cursor_scale = (float)value->as.f;
    return true;
  case SET_CURSOR_FOLLOW:
    s->render_cursor_follow = value->as.b;
    return true;
  case SET_CENTER_DOT:
    s->center_dot = value->as.b;
    return true;
  case SET_AUTO_FINISH_EVENTS:
    game->auto_finish_events = value->as.b;
    return true;
  default:
    return false;
  }
}
