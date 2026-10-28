// DDNet as a FrameTee game module.
//
// This file is the ABI surface: identity, constraints, input schema, levels,
// worlds and the vtable at the bottom. The game's own subsystems live beside it
// (dd_gfx.c, dd_render.c, dd_particles.c, dd_anim_*.c) and none of them are
// visible to the engine.

#include "dd_assets.h"
#include "dd_character_state.h"
#include "dd_input_effects.h"
#include "dd_internal.h"
#include "dd_maps.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Input schema
// -----------------------------------------------------------------------------
//
// dd_input_t packs its booleans into flags, which is precisely why the engine
// edits inputs through named fields instead of reaching into the record.

enum ddnet_input_field {
  IN_DIRECTION = 0,
  IN_TARGET,
  IN_JUMP,
  IN_FIRE,
  IN_FIRE_PRESS,
  IN_HOOK,
  IN_WEAPON,
  IN_KILL,
  IN_EYES,
  IN_EMOTE,
  IN_SIT,
  IN_TELE_OUT,
  // What a player does to the world besides moving its tee. Every one of them
  // is applied before the tick its record is for, like the kill.
  IN_TEAM,
  IN_LOCK,
  IN_SPEC,
  IN_CONNECTION,
  // DDNet's player flags. Chatting and the spectator camera keep the input
  // from the tee; the others only show.
  IN_PLAYING,
  IN_IN_MENU,
  IN_CHATTING,
  IN_SCOREBOARD,
  IN_AIM,
  IN_SPEC_CAM,
  IN_COUNT
};

static const char *const weapon_labels[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
static const char *const eye_labels[] = {"Normal", "Angry", "Pain", "Happy", "Blink", "Surprise"};
static const char *const lock_labels[] = {"Unchanged", "Locked", "Unlocked"};
static const char *const connection_labels[] = {"Unchanged", "Connected", "Disconnected"};
#if DD_UNIQUE
static const char *const side_labels[] = {"Unchanged", "Red", "Blue"};
// What Unique Race has none of (a team, its lock, /spec, other players joining): hidden.
#define DD_DDNET_ONLY FT_INPUT_FLAG_EDITOR_HIDDEN
#else
#define DD_DDNET_ONLY 0
#endif

// The player flag of each of the fields IN_PLAYING to IN_SPEC_CAM.
static const uint8_t player_flag_of_field[] = {
    DDNET_PLAYERFLAG_PLAYING, DDNET_PLAYERFLAG_IN_MENU, DDNET_PLAYERFLAG_CHATTING,
    DDNET_PLAYERFLAG_SCOREBOARD, DDNET_PLAYERFLAG_AIM, DDNET_PLAYERFLAG_SPEC_CAM,
};

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
                 .description = "The fire button is held",
                 .kind = FT_INPUT_BOOL,
                 .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_LATCHED,
                 .color = {1.0f, 0.5f, 0.35f, 1.0f}},
    [IN_FIRE_PRESS] = {.id = "fire_press",
                       .display_name = "Fire press",
                       .description = "The fire button went down during this tick. Fire going on is a press too, so this only "
                                      "adds the presses Fire does not show: a tap between two ticks, or pressing again while held",
                       .kind = FT_INPUT_BOOL,
                       .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_TRIGGER,
                       .color = {1.0f, 0.75f, 0.6f, 1.0f}},
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
                   .max_value = DDNET_NUM_WEAPONS - 1,
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
                 .flags = FT_INPUT_FLAG_EDITOR_HIDDEN,
                 .min_value = 0,
                 .max_value = NUM_EYES - 1,
                 .enum_labels = eye_labels,
                 .enum_count = (uint32_t)(sizeof(eye_labels) / sizeof(eye_labels[0]))},
    [IN_EMOTE] = {.id = "emote", .display_name = "Emoticon", .kind = FT_INPUT_INT, .flags = FT_INPUT_FLAG_EDITOR_HIDDEN, .min_value = 0, .max_value = 15},
    [IN_SIT] = {.id = "sit", .display_name = "Sit", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_EDITOR_HIDDEN},
    [IN_TELE_OUT] = {.id = "tele_out",
                     .display_name = "Tele out",
                     .description = "Which exit a teleporter with several takes: this value modulo their number, in map order",
                     .kind = FT_INPUT_INT,
                     .flags = FT_INPUT_FLAG_PLAYER_STATE,
                     .max_value = 255},
#if DD_UNIQUE
    // (in m_Side, see dd_input_t)
    [IN_TEAM] = {.id = "side",
                 .display_name = "Side",
                 .description = "Fastcap: the flag the tee spawns at while set (/red, /blue); a change kills it to respawn there",
                 .kind = FT_INPUT_ENUM,
                 .flags = FT_INPUT_FLAG_PLAYER_STATE,
                 .min_value = 0,
                 .max_value = DD_SIDE_COUNT - 1,
                 .enum_labels = side_labels,
                 .enum_count = (uint32_t)(sizeof(side_labels) / sizeof(side_labels[0])),
                 .color = {0.95f, 0.55f, 0.55f, 1.0f}},
#else
    [IN_TEAM] = {.id = "team",
                 .display_name = "Team",
                 .description = "Puts the player into this team while set; -1 leaves its team to the game",
                 .kind = FT_INPUT_INT,
                 .flags = FT_INPUT_FLAG_PLAYER_STATE,
                 .min_value = -1,
                 .max_value = DDNET_NUM_TEAMS - 1,
                 .default_value = -1,
                 .color = {0.55f, 0.85f, 0.95f, 1.0f}},
#endif
    [IN_LOCK] = {.id = "lock",
                 .display_name = "Team lock",
                 .kind = FT_INPUT_ENUM,
                 .flags = FT_INPUT_FLAG_PLAYER_STATE | DD_DDNET_ONLY,
                 .min_value = 0,
                 .max_value = DD_LOCK_COUNT - 1,
                 .enum_labels = lock_labels,
                 .enum_count = (uint32_t)(sizeof(lock_labels) / sizeof(lock_labels[0])),
                 .color = {0.95f, 0.8f, 0.45f, 1.0f}},
    [IN_SPEC] = {.id = "spec", .display_name = "Spec", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE | DD_DDNET_ONLY, .color = {0.75f, 0.75f, 0.8f, 1.0f}},
    [IN_CONNECTION] = {.id = "connection",
                       .display_name = "Connection",
                       .kind = FT_INPUT_ENUM,
                       .flags = FT_INPUT_FLAG_PLAYER_STATE | DD_DDNET_ONLY,
                       .min_value = 0,
                       .max_value = DD_CONNECTION_COUNT - 1,
                       .enum_labels = connection_labels,
                       .enum_count = (uint32_t)(sizeof(connection_labels) / sizeof(connection_labels[0])),
                       .color = {0.6f, 0.9f, 0.6f, 1.0f}},
    [IN_PLAYING] = {.id = "flag_playing", .display_name = "Playing", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
    [IN_IN_MENU] = {.id = "flag_in_menu", .display_name = "In menu", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
    [IN_CHATTING] = {.id = "flag_chatting", .display_name = "Chatting", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
    [IN_SCOREBOARD] = {.id = "flag_scoreboard", .display_name = "Scoreboard", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
    [IN_AIM] = {.id = "flag_aim", .display_name = "Aim", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
    [IN_SPEC_CAM] = {.id = "flag_spec_cam", .display_name = "Spectator camera", .kind = FT_INPUT_BOOL, .flags = FT_INPUT_FLAG_PLAYER_STATE},
};

static const ft_input_control input_controls[] = {
    {.id = "left", .display_name = "Move Left", .category = DD_GAME_NAME, .default_binding = "A", .field = IN_DIRECTION, .value = -1, .flags = FT_CONTROL_ADD},
    {.id = "right", .display_name = "Move Right", .category = DD_GAME_NAME, .default_binding = "D", .field = IN_DIRECTION, .value = 1, .flags = FT_CONTROL_ADD},
    {.id = "jump", .display_name = "Jump", .category = DD_GAME_NAME, .default_binding = "Space", .field = IN_JUMP, .value = 1},
    {.id = "fire", .display_name = "Fire Weapon", .category = DD_GAME_NAME, .default_binding = "MouseLeft", .field = IN_FIRE, .value = 1},
    // On the same button as Fire Weapon: every press is recorded on the tick it happens on, also one
    // let go of again before that tick is recorded, which holding alone would miss.
    {.id = "fire_press",
     .display_name = "Fire Press",
     .description = "Records every press of the fire button, also a tap shorter than a tick. Bind it like Fire Weapon.",
     .category = DD_GAME_NAME,
     .default_binding = "MouseLeft",
     .field = IN_FIRE_PRESS,
     .value = 1,
     .flags = FT_CONTROL_PRESSED},
    {.id = "hook", .display_name = "Hook", .category = DD_GAME_NAME, .default_binding = "MouseRight", .field = IN_HOOK, .value = 1},
    {.id = "hammer", .display_name = "Switch to Hammer", .category = DD_GAME_NAME, .default_binding = "1", .field = IN_WEAPON, .value = 0, .flags = FT_CONTROL_PRESSED},
    {.id = "gun", .display_name = "Switch to Gun", .category = DD_GAME_NAME, .default_binding = "2", .field = IN_WEAPON, .value = 1, .flags = FT_CONTROL_PRESSED},
    {.id = "shotgun", .display_name = "Switch to Shotgun", .category = DD_GAME_NAME, .default_binding = "3", .field = IN_WEAPON, .value = 2, .flags = FT_CONTROL_PRESSED},
    {.id = "grenade", .display_name = "Switch to Grenade", .category = DD_GAME_NAME, .default_binding = "4", .field = IN_WEAPON, .value = 3, .flags = FT_CONTROL_PRESSED},
    {.id = "laser", .display_name = "Switch to Laser", .category = DD_GAME_NAME, .default_binding = "5", .field = IN_WEAPON, .value = 4, .flags = FT_CONTROL_PRESSED},
    {.id = "kill", .display_name = "Kill", .category = DD_GAME_NAME, .default_binding = "K", .field = IN_KILL, .value = 1, .flags = FT_CONTROL_PRESSED},
};

// The layout projects store records in.
_Static_assert(sizeof(dd_input_t) == 16 && _Alignof(dd_input_t) == 8 && offsetof(dd_input_t, m_Flags) == 12 &&
                   offsetof(dd_input_t, m_Team) == 14 && offsetof(dd_input_t, m_PlayerFlags) == 15,
               "dd_input_t must keep the layout projects store");

static const ft_input_schema input_schema = {
    .struct_size = sizeof(ft_input_schema),
    .record_size = sizeof(dd_input_t),
    .record_align = _Alignof(dd_input_t),
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
  dd_input_t *input = record;
  memset(input, 0, sizeof(*input));
  // A zero aim would leave the tee pointing at itself, which no real client
  // ever sends; straight up matches what DDNet does on spawn.
  input->m_TargetY = -1;
  // Gun is DDNet's neutral wanted-weapon value. A zeroed record requests the
  // hammer and would switch away from the weapon the tee spawns with.
  input->m_WantedWeapon = DDNET_WEAPON_GUN;
#if DD_UNIQUE
  // (a tee of fastcap spawns with the grenade in hand)
  if (game && game->current_level && game->current_level->config.gamemode == DDNET_MODE_UNIQUE_FASTCAP)
    input->m_WantedWeapon = DDNET_WEAPON_GRENADE;
#else
  (void)game;
#endif
}

static int64_t ddnet_input_get(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const dd_input_t *in = record;
  switch (field) {
  case IN_DIRECTION:
    return in->m_Direction;
  case IN_JUMP:
    return in->m_Jump != 0;
  case IN_FIRE:
    return (in->m_Fire & 1) != 0;
  case IN_FIRE_PRESS:
    return get_flag_fire_press(in) != 0;
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
  case IN_TEAM:
#if DD_UNIQUE
    return dd_input_fastcap_side(in);
#else
    return dd_input_team(in);
#endif
  case IN_LOCK:
    return get_flag_lock(in);
  case IN_SPEC:
    return get_flag_spec(in) != 0;
  case IN_CONNECTION:
    return get_flag_connection(in);
  case IN_PLAYING:
  case IN_IN_MENU:
  case IN_CHATTING:
  case IN_SCOREBOARD:
  case IN_AIM:
  case IN_SPEC_CAM:
    return (in->m_PlayerFlags & player_flag_of_field[field - IN_PLAYING]) != 0;
  default:
    return 0;
  }
}

static void ddnet_input_set(ft_game *game, void *record, uint32_t field, int64_t value) {
  (void)game;
  dd_input_t *in = record;
  switch (field) {
  case IN_DIRECTION:
    in->m_Direction = (int8_t)(value < -1 ? -1 : (value > 1 ? 1 : value));
    break;
  case IN_JUMP:
    in->m_Jump = value ? 1 : 0;
    break;
  case IN_FIRE:
    in->m_Fire = value ? 1 : 0;
    break;
  case IN_FIRE_PRESS:
    set_flag_fire_press(in, value != 0);
    break;
  case IN_HOOK:
    in->m_Hook = value ? 1 : 0;
    break;
  case IN_WEAPON:
    in->m_WantedWeapon = (uint8_t)(value < 0 ? 0 : (value >= DDNET_NUM_WEAPONS ? DDNET_NUM_WEAPONS - 1 : value));
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
  case IN_TEAM:
#if DD_UNIQUE
    in->m_Side = (uint8_t)(value < 0 || value >= DD_SIDE_COUNT ? DD_SIDE_KEEP : value);
#else
    dd_input_set_team(in, (int)(value < -1 ? -1 : (value >= DDNET_NUM_TEAMS ? DDNET_NUM_TEAMS - 1 : value)));
#endif
    break;
  case IN_LOCK:
    set_flag_lock(in, (uint8_t)(value < 0 || value >= DD_LOCK_COUNT ? DD_LOCK_KEEP : value));
    break;
  case IN_SPEC:
    set_flag_spec(in, value != 0);
    break;
  case IN_CONNECTION:
    set_flag_connection(in, (uint8_t)(value < 0 || value >= DD_CONNECTION_COUNT ? DD_CONNECTION_KEEP : value));
    break;
  case IN_PLAYING:
  case IN_IN_MENU:
  case IN_CHATTING:
  case IN_SCOREBOARD:
  case IN_AIM:
  case IN_SPEC_CAM: {
    const uint8_t flag = player_flag_of_field[field - IN_PLAYING];
    in->m_PlayerFlags = (uint8_t)(value ? in->m_PlayerFlags | flag : in->m_PlayerFlags & ~flag);
    break;
  }
  default:
    break;
  }
}

static ft_vec2 ddnet_input_get_vec2(ft_game *game, const void *record, uint32_t field) {
  (void)game;
  const dd_input_t *in = record;
  if (field != IN_TARGET) return (ft_vec2){0.f, 0.f};
  return (ft_vec2){(float)in->m_TargetX, (float)in->m_TargetY};
}

static void ddnet_input_set_vec2(ft_game *game, void *record, uint32_t field, ft_vec2 value) {
  (void)game;
  dd_input_t *in = record;
  if (field != IN_TARGET) return;
  in->m_TargetX = (int16_t)value.x;
  in->m_TargetY = (int16_t)value.y;
}

static void ddnet_input_describe(ft_game *game, const void *record, char *out, size_t out_size) {
  (void)game;
  const dd_input_t *in = record;
  const char *dir = in->m_Direction < 0 ? "<" : (in->m_Direction > 0 ? ">" : "-");
  snprintf(out, out_size, "%s%s%s%s%s %s", dir, in->m_Jump ? " jump" : "", (in->m_Fire & 1) ? " fire" : "",
           get_flag_fire_press(in) ? " press" : "", in->m_Hook ? " hook" : "",
           in->m_WantedWeapon < DDNET_NUM_WEAPONS ? weapon_labels[in->m_WantedWeapon] : "?");
}

// -----------------------------------------------------------------------------
// Entity properties
// -----------------------------------------------------------------------------

// Starting overrides are written in this order. Whether the player is in the
// game comes before everything about its tee but where it is (the property
// games list first), and its team before its race and switches, which joining
// a team changes.
enum ddnet_player_prop {
  PROP_POSITION = 0,
  PROP_CONNECTED,
  PROP_TEAM,
  PROP_TEAM_LOCKED,
  PROP_RACE_STARTED,
  PROP_RACE_ELAPSED,
  PROP_SWITCHES_OFF,
  PROP_VELOCITY,
  PROP_ACTIVE_WEAPON,
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
  PROP_CAN_JUMP,
  PROP_HOOK_STATE,
  PROP_HOOKED_PLAYER,
  PROP_RACE_TIME,
  PROP_DEEP_FROZEN,
  PROP_LIVE_FROZEN,
  PROP_ENDLESS_HOOK,
  PROP_JETPACK,
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
    [PROP_CONNECTED] = {.id = "connected", .display_name = "Connected", .group = "Team", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_TEAM] = {.id = "team",
                   .display_name = "Team",
                   .group = "Team",
                   .kind = FT_VALUE_INT,
                   .flags = DD_PROP_START,
                   .min_value = 0,
                   .max_value = DDNET_NUM_TEAMS - 1},
    [PROP_TEAM_LOCKED] = {.id = "team_locked", .display_name = "Team locked", .group = "Team", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_RACE_STARTED] = {.id = "race_started", .display_name = "Race started", .group = "Race", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_RACE_ELAPSED] = {.id = "race_elapsed",
                           .display_name = "Time since start",
                           .group = "Race",
                           .unit = "s",
                           .kind = FT_VALUE_FLOAT,
                           .flags = DD_PROP_START,
                           .min_value = 0,
                           .max_value = 86400},
    // As text, which is what a property can hold: "2 5-7".
    [PROP_SWITCHES_OFF] = {.id = "switches_off",
                           .display_name = "Deactivated switches",
                           .group = "Team",
                           .kind = FT_VALUE_STRING,
                           .flags = DD_PROP_START},
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
                            .group = "Weapons",
                            .kind = FT_VALUE_INT,
                            .flags = FT_PROP_WRITABLE | FT_PROP_STARTING | FT_PROP_SUMMARY,
                            .min_value = 0,
                            .max_value = DDNET_NUM_WEAPONS - 1},
    [PROP_HAS_SHOTGUN] = {.id = "has_shotgun", .display_name = "Shotgun", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_GRENADE] = {.id = "has_grenade", .display_name = "Grenade", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_LASER] = {.id = "has_laser", .display_name = "Laser", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HAS_NINJA] = {.id = "has_ninja", .display_name = "Ninja", .group = "Weapons", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_HEALTH] = {.id = "health", .display_name = "Health", .group = "Weapons", .kind = FT_VALUE_INT, .flags = FT_PROP_WRITABLE},
    [PROP_ARMOR] = {.id = "armor", .display_name = "Armor", .group = "Weapons", .kind = FT_VALUE_INT, .flags = FT_PROP_WRITABLE},
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
    [PROP_ENDLESS_JUMP] = {.id = "endless_jump", .display_name = "Endless jump", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_GROUNDED] = {.id = "grounded", .display_name = "Grounded", .group = "Movement", .kind = FT_VALUE_BOOL},
    // A jump from the ground or in the air, whichever the tee would make now.
    [PROP_CAN_JUMP] = {.id = "can_jump", .display_name = "Can jump", .group = "Movement", .kind = FT_VALUE_BOOL},
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
    [PROP_ENDLESS_HOOK] = {.id = "endless_hook", .display_name = "Endless hook", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
    [PROP_JETPACK] = {.id = "jetpack", .display_name = "Jetpack", .group = "Powers", .kind = FT_VALUE_BOOL, .flags = DD_PROP_START},
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

#if DD_UNIQUE
// A tee of Unique Race has fewer properties: these, in this order. Its grenades have ammo with
// sv_health_and_ammo (PROP_GRENADES, a property only it has).
enum { PROP_GRENADES = PROP_COUNT };
static const uint32_t unique_prop_ids[] = {PROP_POSITION, PROP_RACE_STARTED, PROP_RACE_ELAPSED, PROP_VELOCITY,
                                           PROP_ACTIVE_WEAPON, PROP_HAS_GRENADE, PROP_GRENADES, PROP_HEALTH,
                                           PROP_ARMOR, PROP_FREEZE_TIME, PROP_JUMPS, PROP_JUMPS_LEFT,
                                           PROP_GROUNDED, PROP_CAN_JUMP, PROP_HOOK_STATE, PROP_RACE_TIME};
#define DD_PLAYER_PROP_COUNT ((uint32_t)(sizeof(unique_prop_ids) / sizeof(unique_prop_ids[0])))
static const ft_prop_desc grenades_prop = {.id = "grenades",
                                           .display_name = "Grenades",
                                           .group = "Weapons",
                                           .kind = FT_VALUE_INT,
                                           .flags = DD_PROP_START,
                                           .min_value = 0,
                                           .max_value = DDNET_GRENADE_MAX_AMMO};
// (filled from the lists above by fill_unique_props, before the module is handed to the engine)
static ft_prop_desc unique_player_props[sizeof(unique_prop_ids) / sizeof(unique_prop_ids[0])];

static void fill_unique_props(void) {
  for (uint32_t i = 0; i < DD_PLAYER_PROP_COUNT; ++i) {
    const uint32_t id = unique_prop_ids[i];
    unique_player_props[i] = id == PROP_GRENADES ? grenades_prop : player_props[id];
    // (health and armor count with sv_health_and_ammo: a tee can start with less)
    if (id == PROP_HEALTH || id == PROP_ARMOR) unique_player_props[i].flags |= FT_PROP_STARTING;
  }
}

// The property of the tee at an index of the class's list, PROP_COUNT + 1 for none.
static uint32_t player_prop_id(uint32_t index) { return index < DD_PLAYER_PROP_COUNT ? unique_prop_ids[index] : PROP_COUNT + 1; }
#else
#define DD_PLAYER_PROP_COUNT PROP_COUNT
static uint32_t player_prop_id(uint32_t index) { return index; }
#endif

// Projectiles and lasers, so they can be picked and inspected in the viewport.
// The editor knows none of this: it walks whatever classes a game publishes and
// reads whatever properties they list.
enum ddnet_projectile_prop {
  PROJ_POSITION = 0,
  PROJ_ORIGIN,
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

// A projectile keeps its owner, weapon, spawn tick and direction for its whole
// flight, and those tell two apart (a shotgun's pellets by direction). A
// bouncing one restarts its flight at each bounce, and its trail with it.
static const ft_prop_desc projectile_props[PROJ_PROP_COUNT] = {
    [PROJ_POSITION] = {"position", "Position", "Motion", "tiles", FT_VALUE_VEC2, FT_PROP_SUMMARY, 0, 0},
    // where it was fired from, or bounced off last
    [PROJ_ORIGIN] = {"origin", "Origin", "Motion", "tiles", FT_VALUE_VEC2, 0, 0, 0},
    [PROJ_DIRECTION] = {"direction", "Direction", "Motion", NULL, FT_VALUE_VEC2, FT_PROP_IDENTITY, 0, 0},
    [PROJ_TYPE] = {"type", "Weapon", "Identity", NULL, FT_VALUE_INT, FT_PROP_SUMMARY | FT_PROP_IDENTITY, 0, 0},
    [PROJ_OWNER] = {"owner", "Owner", "Identity", NULL, FT_VALUE_INT, FT_PROP_IDENTITY, 0, 0},
    [PROJ_START_TICK] = {"start_tick", "Start tick", "Timing", NULL, FT_VALUE_INT, FT_PROP_IDENTITY, 0, 0},
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

enum ddnet_hook_prop {
  HOOK_POSITION = 0,
  HOOK_ORIGIN,
  HOOK_DIRECTION,
  HOOK_OWNER,
  HOOK_PROP_COUNT
};

// A tee's hook while it is out, from its throw until it is back. The owner and
// the direction it was thrown in last its whole way, and a hook only comes back
// out after a tick at the tee. The origin is where it is thrown from: the tee,
// or the teleporter it came out of.
static const ft_prop_desc hook_props[HOOK_PROP_COUNT] = {
    [HOOK_POSITION] = {"position", "Position", "Motion", "tiles", FT_VALUE_VEC2, FT_PROP_SUMMARY, 0, 0},
    [HOOK_ORIGIN] = {"origin", "Origin", "Motion", "tiles", FT_VALUE_VEC2, 0, 0, 0},
    [HOOK_DIRECTION] = {"direction", "Direction", "Motion", NULL, FT_VALUE_VEC2, FT_PROP_IDENTITY, 0, 0},
    [HOOK_OWNER] = {"owner", "Owner", "Identity", NULL, FT_VALUE_INT, FT_PROP_SUMMARY | FT_PROP_IDENTITY, 0, 0},
};

enum { DD_CLASS_PLAYER = 0,
       DD_CLASS_PROJECTILE,
       DD_CLASS_LASER,
       DD_CLASS_HOOK,
       DD_CLASS_COUNT };

static const ft_entity_class entity_classes[] = {
#if DD_UNIQUE
    // (the grenades of Unique Race are neither explosive bullets, nor freeze, nor bounce: the last
    // properties of a projectile are left out)
    [DD_CLASS_PLAYER] = {.id = "player", .display_name = "Tee", .props = unique_player_props, .prop_count = DD_PLAYER_PROP_COUNT},
    [DD_CLASS_PROJECTILE] = {.id = "projectile", .display_name = "Projectile", .props = projectile_props, .prop_count = PROJ_EXPLOSIVE},
#else
    [DD_CLASS_PLAYER] = {.id = "player", .display_name = "Tee", .props = player_props, .prop_count = PROP_COUNT},
    [DD_CLASS_PROJECTILE] = {.id = "projectile", .display_name = "Projectile", .props = projectile_props, .prop_count = PROJ_PROP_COUNT},
#endif
    [DD_CLASS_LASER] = {.id = "laser", .display_name = "Laser", .props = laser_props, .prop_count = LASER_PROP_COUNT},
    [DD_CLASS_HOOK] = {.id = "hook", .display_name = "Hook", .props = hook_props, .prop_count = HOOK_PROP_COUNT},
};

// Entities hang off the world in linked lists, so an index means "the nth one
// still alive" of the class (the laser list also holds the map's turrets,
// draggers and laser walls, which are not lasers).
static bool projectile_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out);
static bool laser_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out);
static bool hook_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out);

// A hook is out from its throw until it is back at the tee, which is when the
// game draws it.
static bool hook_out(const ddnet_character_t *chr) { return chr && chr->core.hook_state >= DDNET_HOOK_RETRACT_START; }

// The player whose hook is the nth one out, or -1.
static int hook_player(const ft_world *world, int32_t index) {
  if (!world || index < 0) return -1;
  int32_t seen = 0;
  for (int player = 0; player < world->player_count; ++player)
    if (hook_out(ddnet_player_character(world, player)) && seen++ == index) return player;
  return -1;
}

#if !DD_UNIQUE
static const ddnet_entity_t *entity_at(const ft_world *world, int type, ddnet_entity_kind_t kind, int32_t index) {
  if (!world || index < 0) return NULL;
  int32_t seen = 0;
  for (int i = world->core.first_entity[type]; i != -1; i = world->core.entities[i].link.next) {
    const ddnet_entity_t *ent = &world->core.entities[i];
    if (ent->kind == kind && seen++ == index) return ent;
  }
  return NULL;
}

static int32_t entity_list_count(const ft_world *world, int type, ddnet_entity_kind_t kind) {
  if (!world) return 0;
  int32_t count = 0;
  for (int i = world->core.first_entity[type]; i != -1; i = world->core.entities[i].link.next)
    count += world->core.entities[i].kind == kind;
  return count;
}
#endif

// The race time of a player that finished, in seconds; -1 until then.
static float race_time(const ft_world *world, int client_id) { return dd_world_finish_time(world, client_id); }

static bool ddnet_entity_prop_get(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, ft_value *out) {
  (void)game;
  ddnet_world_entities_current(world);
  if (entity_class == DD_CLASS_PROJECTILE) return projectile_prop_get(world, entity, prop, out);
  if (entity_class == DD_CLASS_LASER) return laser_prop_get(world, entity, prop, out);
  if (entity_class == DD_CLASS_HOOK) return hook_prop_get(world, entity, prop, out);
  if (entity_class != FT_ENTITY_CLASS_PLAYER) return false;
  prop = player_prop_id(prop);
#if !DD_UNIQUE
  // (the one property of a player that has no tee)
  if (prop == PROP_CONNECTED) {
    const int client_id = ddnet_player_client(world, entity);
    if (client_id < 0) return false;
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = world->core.players[client_id].active};
    return true;
  }
#endif
  const ddnet_character_t *chr = ddnet_player_character(world, entity);
  if (!chr || prop > PROP_COUNT) return false;
  const ddnet_character_core_t *c = &chr->core;
#if !DD_UNIQUE
  const int team = world->core.players[ddnet_player_client(world, entity)].team;
#endif

  switch (prop) {
#if DD_UNIQUE
  case PROP_GRENADES:
    *out = (ft_value){.kind = FT_VALUE_INT,
                      .as.i = c->weapons[DDNET_WEAPON_GRENADE].got ? c->weapons[DDNET_WEAPON_GRENADE].ammo : 0};
    return true;
#else
  case PROP_TEAM:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = team};
    return true;
  case PROP_TEAM_LOCKED:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = ddnet_world_team(&world->core, team).locked};
    return true;
  case PROP_SWITCHES_OFF: {
    // (read on the main thread and copied by the caller before the next read; a
    // starting override holds 255 characters)
    static char text[256];
    const dd_switch_set off = dd_team_switches_off(&world->core, team);
    if (!dd_switch_set_format(&off, text, sizeof(text))) return false;
    *out = (ft_value){.kind = FT_VALUE_STRING, .as.s = text};
    return true;
  }
#endif
  case PROP_RACE_STARTED:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = dd_player_race_started(chr)};
    return true;
  case PROP_RACE_ELAPSED:
    *out = (ft_value){.kind = FT_VALUE_FLOAT,
                      .as.f = (double)dd_player_race_ticks(ddnet_player_world(world, entity), chr) / GAME_TICK_SPEED};
    return true;
  case PROP_POSITION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {chr->pos.x / PX_PER_TILE, chr->pos.y / PX_PER_TILE}};
    return true;
  case PROP_VELOCITY:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {c->vel.x, c->vel.y}};
    return true;
  case PROP_ACTIVE_WEAPON:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->active_weapon};
    return true;
  case PROP_HEALTH:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = chr->health};
    return true;
  case PROP_ARMOR:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = chr->armor};
    return true;
  case PROP_FREEZE_TIME:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = chr->freeze_time};
    return true;
  case PROP_HAS_SHOTGUN:
  case PROP_HAS_GRENADE:
  case PROP_HAS_LASER:
  case PROP_HAS_NINJA:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->weapons[prop - (PROP_HAS_SHOTGUN - 2)].got};
    return true;
  case PROP_JUMPS:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->jumps};
    return true;
  case PROP_JUMPS_LEFT:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->jumped_total};
    return true;
  case PROP_GROUNDED:
    *out = (ft_value){.kind = FT_VALUE_BOOL,
                      .as.b = world->level && ddnet_collision_is_on_ground(&world->level->collision, c->pos, 28.f)};
    return true;
  case PROP_CAN_JUMP: {
    // (the core's own test: the second bit of `jumped` is set once the air jumps are gone, and a tee
    // on the ground jumps from it unless it has no jumps at all)
    const bool grounded = world->level && ddnet_collision_is_on_ground(&world->level->collision, c->pos, 28.f);
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !(c->jumped & 2) || (grounded && c->jumps != 0)};
    return true;
  }
  case PROP_HOOK_STATE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->hook_state};
    return true;
  case PROP_RACE_TIME:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = race_time(world, ddnet_player_client(world, entity))};
    return true;
#if !DD_UNIQUE
  case PROP_ENDLESS_JUMP:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->endless_jump};
    return true;
  case PROP_HOOKED_PLAYER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = c->hooked_player >= 0 ? ddnet_client_player(world, c->hooked_player) : -1};
    return true;
  case PROP_DEEP_FROZEN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->deep_frozen};
    return true;
  case PROP_ENDLESS_HOOK:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->endless_hook};
    return true;
  case PROP_JETPACK:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->jetpack};
    return true;
  case PROP_SOLO:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->solo};
    return true;
  case PROP_LIVE_FROZEN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->live_frozen};
    return true;
  case PROP_TELEGUN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->has_telegun_gun};
    return true;
  case PROP_TELEGRENADE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->has_telegun_grenade};
    return true;
  case PROP_TELELASER:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = c->has_telegun_laser};
    return true;
  // The physics stores these as things that are switched off; the editor shows
  // them as things that work, so both sides read the way their owner thinks.
  case PROP_COLLIDE_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->collision_disabled};
    return true;
  case PROP_HOOK_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->hook_hit_disabled};
    return true;
  case PROP_HAMMER_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->hammer_hit_disabled};
    return true;
  case PROP_SHOTGUN_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->shotgun_hit_disabled};
    return true;
  case PROP_GRENADE_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->grenade_hit_disabled};
    return true;
  case PROP_LASER_HITS_OTHERS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = !c->laser_hit_disabled};
    return true;
#endif
  default:
    return false;
  }
}

static bool ddnet_entity_prop_set(ft_game *game, ft_world *world, uint32_t entity_class, int32_t entity, uint32_t prop, const ft_value *value) {
  (void)game;
  if (entity_class != FT_ENTITY_CLASS_PLAYER || !value) return false;
  const int client_id = ddnet_player_client(world, entity);
  if (client_id < 0) return false;
  prop = player_prop_id(prop);
#if !DD_UNIQUE
  // A player that is not in the game when the world starts has no tee, and one
  // that is stands at the spawn like the others.
  if (prop == PROP_CONNECTED) {
    if (!value->as.b) ddnet_player_leave(&world->core, client_id);
    else if (!world->core.players[client_id].active)
      return ddnet_player_join(&world->core, client_id) && ddnet_player_spawn(&world->core, client_id);
    return true;
  }
#endif
  ddnet_character_t *chr = ddnet_player_character_mut(world, entity);
  if (!chr || prop > PROP_COUNT) return false;
  ddnet_character_core_t *c = &chr->core;

  switch (prop) {
#if DD_UNIQUE
  case PROP_GRENADES:
    if (value->as.i < 0 || value->as.i > DDNET_GRENADE_MAX_AMMO) return false;
    // (without sv_health_and_ammo a grenade has no ammo to count: -1)
    if (!c->weapons[DDNET_WEAPON_GRENADE].got || !ddnet_player_world(world, entity)->config.sv_health_and_ammo) return false;
    c->weapons[DDNET_WEAPON_GRENADE].ammo = (int)value->as.i;
    break;
#else
  case PROP_TEAM:
    if (value->as.i < 0 || value->as.i >= DDNET_NUM_TEAMS) return false;
    dd_player_set_team(&world->core, client_id, (int)value->as.i);
    break;
  case PROP_TEAM_LOCKED: {
    // (team 0 cannot be locked)
    const int team = world->core.players[client_id].team;
    if (team == DDNET_TEAM_FLOCK) return !value->as.b;
    if (ddnet_world_team(&world->core, team).locked != value->as.b) ddnet_world_lock_team(&world->core, team, value->as.b);
    break;
  }
  case PROP_SWITCHES_OFF: {
    dd_switch_set off;
    if (value->kind != FT_VALUE_STRING || !dd_switch_set_parse(value->as.s, &off)) return false;
    dd_team_set_switches_off(&world->core, client_id, &off);
    break;
  }
#endif
  case PROP_RACE_STARTED:
    dd_player_set_race_started(ddnet_player_world_mut(world, entity), dd_space_client(client_id), chr, value->as.b);
    break;
  case PROP_RACE_ELAPSED:
    if (!(value->as.f >= 0.0) || value->as.f > 86400.0) return false;
    if (dd_player_race_started(chr)) chr->start_time = ddnet_engine_tick(world) - (int)lround(value->as.f * GAME_TICK_SPEED);
    break;
  case PROP_POSITION:
    chr->pos = (ddnet_vec2_t){value->as.v.x * PX_PER_TILE, value->as.v.y * PX_PER_TILE};
    chr->prev_pos = chr->pos;
    c->pos = chr->pos;
    break;
  case PROP_VELOCITY:
    c->vel = (ddnet_vec2_t){value->as.v.x, value->as.v.y};
    break;
  case PROP_ACTIVE_WEAPON:
    if (value->as.i < 0 || value->as.i >= DDNET_NUM_WEAPONS) return false;
    c->active_weapon = (int)value->as.i;
    if (!c->weapons[c->active_weapon].got) dd_set_weapon_got(ddnet_player_world(world, entity), c, (int)value->as.i, true);
    break;
  case PROP_HEALTH:
    chr->health = (int)value->as.i;
    break;
  case PROP_ARMOR:
    chr->armor = (int)value->as.i;
    break;
  case PROP_FREEZE_TIME:
    chr->freeze_time = (int)value->as.i;
    break;
  case PROP_HAS_SHOTGUN:
  case PROP_HAS_GRENADE:
  case PROP_HAS_LASER:
  case PROP_HAS_NINJA: {
    const int weapon = (int)prop - (PROP_HAS_SHOTGUN - 2);
    dd_set_weapon_got(ddnet_player_world(world, entity), c, weapon, value->as.b);
    // A tee cannot hold a weapon it does not have; fall back to the hammer,
    // which every tee keeps.
    if (!value->as.b && c->active_weapon == weapon) c->active_weapon = DDNET_WEAPON_HAMMER;
#if !DD_UNIQUE
    if (c->weapons[DDNET_WEAPON_NINJA].got) {
      c->active_weapon = DDNET_WEAPON_NINJA;
      c->ninja.activation_tick = world->core.tick;
    }
#endif
    break;
  }
  case PROP_JUMPS:
    if (value->as.i < 0 || value->as.i > 255) return false;
    c->jumps = (int)value->as.i;
    break;
  case PROP_JUMPS_LEFT:
    c->jumped_total = (int)value->as.i;
    break;
#if !DD_UNIQUE
  case PROP_ENDLESS_JUMP:
    c->endless_jump = value->as.b;
    break;
  case PROP_DEEP_FROZEN:
    c->deep_frozen = value->as.b;
    break;
  case PROP_ENDLESS_HOOK:
    c->endless_hook = value->as.b;
    break;
  case PROP_JETPACK:
    c->jetpack = value->as.b;
    break;
  case PROP_SOLO:
    c->solo = value->as.b;
    world->core.players[client_id].is_solo = value->as.b;
    break;
  case PROP_LIVE_FROZEN:
    c->live_frozen = value->as.b;
    break;
  case PROP_TELEGUN:
    c->has_telegun_gun = value->as.b;
    break;
  case PROP_TELEGRENADE:
    c->has_telegun_grenade = value->as.b;
    break;
  case PROP_TELELASER:
    c->has_telegun_laser = value->as.b;
    break;
  case PROP_COLLIDE_OTHERS:
    c->collision_disabled = !value->as.b;
    break;
  case PROP_HOOK_OTHERS:
    c->hook_hit_disabled = !value->as.b;
    break;
  case PROP_HAMMER_HITS_OTHERS:
    c->hammer_hit_disabled = !value->as.b;
    break;
  case PROP_SHOTGUN_HITS_OTHERS:
    c->shotgun_hit_disabled = !value->as.b;
    break;
  case PROP_GRENADE_HITS_OTHERS:
    c->grenade_hit_disabled = !value->as.b;
    break;
  case PROP_LASER_HITS_OTHERS:
    c->laser_hit_disabled = !value->as.b;
    break;
#endif
  default:
    return false;
  }
  dd_world_character_changed(world, client_id);
  return true;
}

static int32_t ddnet_entity_count(ft_game *game, const ft_world *world, uint32_t entity_class) {
  (void)game;
  if (!world) return 0;
  ddnet_world_entities_current(world);
  switch (entity_class) {
  case DD_CLASS_PLAYER:
    return world->player_count;
#if DD_UNIQUE
  case DD_CLASS_PROJECTILE: {
    int32_t count = 0;
    for (int client_id = 0; client_id < world->client_count; ++client_id)
      if (world->spaces[client_id]) count += world->spaces[client_id]->num_projectiles;
    return count;
  }
  case DD_CLASS_LASER:
    return 0;
#else
  case DD_CLASS_PROJECTILE:
    return entity_list_count(world, DDNET_ENTTYPE_PROJECTILE, DDNET_ENTITY_PROJECTILE);
  case DD_CLASS_LASER:
    return entity_list_count(world, DDNET_ENTTYPE_LASER, DDNET_ENTITY_LASER);
#endif
  case DD_CLASS_HOOK: {
    int32_t count = 0;
    for (int player = 0; player < world->player_count; ++player) count += hook_out(ddnet_player_character(world, player));
    return count;
  }
  default:
    return 0;
  }
}

// The nth projectile of a world, and the world of the physics it flies in (for Unique Race the
// world of the client `owner`).
static const ddnet_entity_t *projectile_at(const ft_world *world, int32_t entity, const ddnet_world_t **space, int *owner) {
  if (!world || entity < 0) return NULL;
#if DD_UNIQUE
  // (the grenades of the players' worlds, in client order)
  for (int client_id = 0; client_id < world->client_count; ++client_id) {
    const ddnet_world_t *client_space = world->spaces[client_id];
    if (!client_space) continue;
    if (entity < client_space->num_projectiles) {
      *space = client_space;
      *owner = client_id;
      return &client_space->projectiles[entity];
    }
    entity -= client_space->num_projectiles;
  }
  return NULL;
#else
  *space = &world->core;
  *owner = -1;
  return entity_at(world, DDNET_ENTTYPE_PROJECTILE, DDNET_ENTITY_PROJECTILE, entity);
#endif
}

static bool projectile_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out) {
  const ddnet_world_t *space = NULL;
  int owner = -1;
  const ddnet_entity_t *ent = projectile_at(world, entity, &space, &owner);
  if (!ent) return false;
  const ddnet_projectile_t *proj = &ent->u.projectile;

  switch (prop) {
  case PROJ_POSITION: {
    // Where it is *now*, which for a projectile is a function of flight time
    // rather than a stored position.
    const float time = (space->tick - proj->start_tick) / (float)GAME_TICK_SPEED;
    const ddnet_vec2_t pos = ddnet_projectile_get_pos(space, ent, time);
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {pos.x / PX_PER_TILE, pos.y / PX_PER_TILE}};
    return true;
  }
  case PROJ_ORIGIN:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {ent->pos.x / PX_PER_TILE, ent->pos.y / PX_PER_TILE}};
    return true;
  case PROJ_DIRECTION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {proj->direction.x, proj->direction.y}};
    return true;
#if DD_UNIQUE
  // (a grenade of the tee of its world, the ticks it has left)
  case PROJ_TYPE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = DDNET_WEAPON_GRENADE};
    return true;
  case PROJ_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = ddnet_client_player(world, owner)};
    return true;
  case PROJ_START_TICK:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->start_tick};
    return true;
  case PROJ_LIFESPAN:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->life_span};
    return true;
#else
  case PROJ_TYPE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->type};
    return true;
  case PROJ_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->owner >= 0 ? ddnet_client_player(world, proj->owner) : -1};
    return true;
  case PROJ_START_TICK:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->start_tick};
    return true;
  case PROJ_LIFESPAN:
    // ticks left, -1 for none (the bullets of map shotguns)
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->expire_tick == INT_MAX ? -1 : proj->expire_tick - space->tick - 1};
    return true;
  case PROJ_EXPLOSIVE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = proj->explosive};
    return true;
  case PROJ_FREEZE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = proj->freeze};
    return true;
  case PROJ_BOUNCING:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = proj->bouncing};
    return true;
#endif
  default:
    return false;
  }
}

static bool laser_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out) {
#if DD_UNIQUE
  // (Unique Race has no lasers)
  (void)world;
  (void)entity;
  (void)prop;
  (void)out;
  return false;
#else
  const ddnet_entity_t *ent = entity_at(world, DDNET_ENTTYPE_LASER, DDNET_ENTITY_LASER, entity);
  if (!ent) return false;
  const ddnet_laser_t *laser = &ent->u.laser;

  switch (prop) {
  case LASER_POSITION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {ent->pos.x / PX_PER_TILE, ent->pos.y / PX_PER_TILE}};
    return true;
  case LASER_FROM:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {laser->from.x / PX_PER_TILE, laser->from.y / PX_PER_TILE}};
    return true;
  case LASER_ENERGY:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = laser->energy};
    return true;
  case LASER_BOUNCES:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->bounces};
    return true;
  case LASER_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->owner >= 0 ? ddnet_client_player(world, laser->owner) : -1};
    return true;
  case LASER_TYPE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->type};
    return true;
  case LASER_EVAL_TICK:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = laser->eval_tick};
    return true;
  default:
    return false;
  }
#endif
}

// How the prediction draws projectiles, lasers and hooks: lasers in DDNet's colours, the rest as
// Ultraforce draws them. A projectile's path goes on to where the map stops it, and an explosive
// one's ends in its explosion, drawn as far as it reaches.
static bool ddnet_entity_trail(ft_game *game, const ft_world *world, uint32_t entity_class, int32_t entity,
                               ft_entity_trail *out) {
  (void)game;
  if (!world) return false;
  ddnet_world_entities_current(world);
  switch (entity_class) {
  case DD_CLASS_PROJECTILE: {
    const ddnet_world_t *space = NULL;
    int owner = -1;
    const ddnet_entity_t *ent = projectile_at(world, entity, &space, &owner);
    if (!ent) return false;
    const ddnet_projectile_t *proj = &ent->u.projectile;
    // CProjectile::Tick: the coming tick takes it on by a tick's flight, or as far as the map lets it.
    const ddnet_vec2_t from = ddnet_projectile_get_pos(space, ent, (space->tick - proj->start_tick) / (float)GAME_TICK_SPEED);
    ddnet_vec2_t to = ddnet_projectile_get_pos(space, ent, (space->tick + 1 - proj->start_tick) / (float)GAME_TICK_SPEED);
    if (world->level) ddnet_collision_intersect_line(&world->level->collision, from, to, &to, NULL);
    out->has_next = true;
    out->next = (ft_vec2){to.x / PX_PER_TILE, to.y / PX_PER_TILE};
    out->width = 0.75f;
#if DD_UNIQUE
    const bool explosive = true;
#else
    const bool explosive = proj->explosive;
#endif
    if (!explosive) {
      out->color = (ft_color){1.f, 0.8f, 0.f, 1.f};
      return true;
    }
    out->color = (ft_color){1.f, 0.f, 0.f, 1.f};
    out->end_radius = 135.f / PX_PER_TILE; // (CGameContext::CreateExplosion)
    out->end_fill = (ft_color){1.f, 0.2f, 0.f, 0.15f};
    out->end_ring = (ft_color){1.f, 0.3f, 0.f, 0.6f};
    return true;
  }
#if !DD_UNIQUE
  case DD_CLASS_LASER: {
    const ddnet_entity_t *ent = entity_at(world, DDNET_ENTTYPE_LASER, DDNET_ENTITY_LASER, entity);
    if (!ent) return false;
    // (the client's laser colours at their defaults)
    const bool shotgun = ent->u.laser.type == DDNET_WEAPON_SHOTGUN;
    out->color = shotgun ? (ft_color){0.85f, 0.65f, 0.35f, 1.f} : (ft_color){0.498f, 0.498f, 1.f, 1.f};
    out->rim = shotgun ? (ft_color){0.1224f, 0.0951f, 0.0423f, 1.f} : (ft_color){0.0744f, 0.0744f, 0.2472f, 1.f};
    out->width = 0.75f;
    out->dot = 3.f;
    return true;
  }
#endif
  case DD_CLASS_HOOK: {
    // (as Ultraforce draws a hook: the rope from the tee at every tick it holds on, and nothing of the throw)
    const ddnet_character_t *chr = ddnet_player_character(world, hook_player(world, entity));
    if (!chr) return false;
    out->color = (ft_color){1.f, 0.5f, 0.f, 0.6f};
    out->width = 0.5f;
    out->rope = chr->core.hook_state == DDNET_HOOK_GRABBED;
    out->rope_only = true;
    out->anchor = (ft_vec2){chr->pos.x / PX_PER_TILE, chr->pos.y / PX_PER_TILE};
    return true;
  }
  default:
    return false;
  }
}

static bool hook_prop_get(const ft_world *world, int32_t entity, uint32_t prop, ft_value *out) {
  const int player = hook_player(world, entity);
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  if (!chr) return false;
  const ddnet_character_core_t *c = &chr->core;

  switch (prop) {
  case HOOK_POSITION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {c->hook_pos.x / PX_PER_TILE, c->hook_pos.y / PX_PER_TILE}};
    return true;
  case HOOK_ORIGIN: {
    // the base the physics measures the hook's length from (Unique Race's maps have no hook teleporters)
#if DD_UNIQUE
    const ddnet_vec2_t base = chr->pos;
#else
    const ddnet_vec2_t base = c->new_hook ? c->hook_tele_base : chr->pos;
#endif
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {base.x / PX_PER_TILE, base.y / PX_PER_TILE}};
    return true;
  }
  case HOOK_DIRECTION:
    *out = (ft_value){.kind = FT_VALUE_VEC2, .as.v = {c->hook_dir.x, c->hook_dir.y}};
    return true;
  case HOOK_OWNER:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = player};
    return true;
  default:
    return false;
  }
}

// -----------------------------------------------------------------------------
// Levels
// -----------------------------------------------------------------------------

#if DD_UNIQUE
// Unique Race plays a map in the game mode its server runs it in, which the map shows: a map of
// FastCap has the two flags, and the copy of one that is played without weapons is named *_no_wpns
// (the server configs of unique-maps say so for all of its 642 maps). `prototype` is a world of the
// map in race.
static ddnet_gamemode_t unique_map_mode(const ddnet_world_t *prototype, const char *name) {
  const ddnet_vec2_t *flags = prototype->flag_pos;
  if (flags[0].x == 0.f && flags[0].y == 0.f && flags[1].x == 0.f && flags[1].y == 0.f) return DDNET_MODE_UNIQUE_RACE;
  static const char suffix[] = "_no_wpns";
  const size_t length = name ? strlen(name) : 0, suffix_length = sizeof(suffix) - 1;
  return length >= suffix_length && strcmp(name + length - suffix_length, suffix) == 0 ? DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS
                                                                                     : DDNET_MODE_UNIQUE_FASTCAP;
}

static const char *unique_mode_name(ddnet_gamemode_t mode) {
  return mode == DDNET_MODE_UNIQUE_FASTCAP               ? "FastCap"
         : mode == DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS ? "FastCap without weapons"
                                                         : "Race";
}
#else
// The physics has DDNet's DDRace game type. (Race and FastCap are Unique Race's, the build of these
// sources as the game unique.)
static const ft_game_variant variants[] = {
    {.id = "ddrace", .display_name = "DDRace"},
};
#endif

static ft_level *level_finish(ft_game *game, map_data_t *map, const char *variant_id, const char *name) {
#if DD_UNIQUE
  // (the game mode comes from the map, see unique_map_mode)
  (void)variant_id;
  // (what Unique Race's maps never have, which its physics leaves out: the map is played without it)
  const char *unsupported = ddnet_map_unsupported(map);
  if (unsupported)
    dd_log(game, FT_LOG_WARN, "Map '%s' is not a map of Unique Race: it has %s, and Unique Race plays the map as if that were not there.",
           name ? name : "?", unsupported);
#else
  if (variant_id && *variant_id && strcmp(variant_id, "ddrace") != 0)
    dd_log(game, FT_LOG_WARN, "The game type '%s' is not supported; the map is played as DDRace.", variant_id);
#endif

  ft_level *level = calloc(1, sizeof(ft_level));
  if (!level) {
    free_map_data(map);
    return NULL;
  }
  level->map = *map;
#if DD_UNIQUE
  // (in race first, which shows whether the map is one of FastCap)
  level->config = ddnet_config_default(DDNET_MODE_UNIQUE_RACE);
#else
  level->config = ddnet_config_default(DDNET_MODE_DDRACE);
#endif
  bool ready = ddnet_collision_init(&level->collision, &level->map) &&
               ddnet_world_init(&level->prototype, &level->collision, &level->config);
#if DD_UNIQUE
  const ddnet_gamemode_t mode = ready ? unique_map_mode(&level->prototype, name) : DDNET_MODE_UNIQUE_RACE;
  if (mode != DDNET_MODE_UNIQUE_RACE) {
    ddnet_world_free(&level->prototype);
    level->config = ddnet_config_default(mode);
    ready = ddnet_world_init(&level->prototype, &level->collision, &level->config);
  }
#endif
  if (!ready) {
    dd_log(game, FT_LOG_ERROR, "Map '%s' could not be prepared.", name ? name : "?");
    ddnet_collision_free(&level->collision);
    free_map_data(&level->map);
    free(level);
    return NULL;
  }

  snprintf(level->name, sizeof(level->name), "%s", name ? name : "map");
#if DD_UNIQUE
  dd_log(game, FT_LOG_INFO, "Map '%s' is played in %s.", level->name, unique_mode_name(level->config.gamemode));
#endif
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
  const char *name = (game && game->engine && game->engine->get_level_name) ? game->engine->get_level_name() : NULL;
  if (!name || !*name || strcmp(name, "unnamed_level") == 0) name = "map";
  return level_finish(game, &map, variant_id, name);
}

static void ddnet_level_destroy(ft_game *game, ft_level *level) {
  if (!level) return;
  if (game->current_level == level) game->current_level = NULL;
  dd_map_destroy(game, level);
  ddnet_world_free(&level->prototype);
  ddnet_collision_free(&level->collision);
  free_map_data(&level->map);
  free(level);
}

// The map file is already held contiguously by the loader, so embedding a level
// in a project is a straight copy of those bytes.
static size_t ddnet_level_serialize(ft_game *game, const ft_level *level, void *out, size_t out_size) {
  (void)game;
  if (!level || !level->loaded) return 0;
  const map_data_t *map = &level->map;
  if (!map->_map_file_data || map->_map_file_size == 0) return 0;
  if (!out) return map->_map_file_size;
  if (out_size < map->_map_file_size) return 0;
  memcpy(out, map->_map_file_data, map->_map_file_size);
  return map->_map_file_size;
}

static bool ddnet_level_info(ft_game *game, const ft_level *level, ft_level_info *out) {
  (void)game;
  if (!level || !level->loaded || !out) return false;
  const map_data_t *map = &level->map;
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

// Room in a world's arrays for `clients` client ids and `players` players.
static bool dd_world_reserve(ft_world *world, int clients, int players) {
  if (players > world->player_room) {
    const int room = players > 2 * world->player_room ? players : 2 * world->player_room;
    uint8_t *client_ids = realloc(world->client_ids, (size_t)room * sizeof(*client_ids));
    if (!client_ids) return false;
    world->client_ids = client_ids;
    world->player_room = room;
  }
  if (clients > world->client_room) {
    const int room = clients > 2 * world->client_room ? clients : 2 * world->client_room;
#if DD_UNIQUE
    ddnet_world_t **spaces = realloc(world->spaces, (size_t)room * sizeof(*spaces));
    if (!spaces) return false;
    memset(&spaces[world->client_room], 0, (size_t)(room - world->client_room) * sizeof(*spaces));
    world->spaces = spaces;
    struct dd_space_effects *effects = realloc(world->space_effects, (size_t)room * sizeof(*effects));
    if (!effects) return false;
    world->space_effects = effects;
#endif
    dd_input_t *inputs = realloc(world->inputs, (size_t)room * sizeof(*inputs));
    if (!inputs) return false;
    world->inputs = inputs;
    int *pain_ticks = realloc(world->pain_ticks, (size_t)room * sizeof(*pain_ticks));
    if (!pain_ticks) return false;
    world->pain_ticks = pain_ticks;
    memset(&inputs[world->client_room], 0, (size_t)(room - world->client_room) * sizeof(*inputs));
    memset(&pain_ticks[world->client_room], 0, (size_t)(room - world->client_room) * sizeof(*pain_ticks));
    world->client_room = room;
  }
  return true;
}

#if DD_UNIQUE
static void space_free(ddnet_world_t *space) {
  if (!space) return;
  ddnet_world_free(space);
  free(space);
}

// Drops the world of the physics of client `client_id`, and the client ids after the last one a
// player has.
static void space_drop(ft_world *world, int client_id) {
  space_free(world->spaces[client_id]);
  world->spaces[client_id] = NULL;
  while (world->client_count > 0 && !world->spaces[world->client_count - 1])
    --world->client_count;
}
#else
// Whether a player of the world has client id `client_id`, connected or not.
static bool client_taken(const ft_world *world, int client_id) {
  for (int i = 0; i < world->player_count; ++i)
    if (world->client_ids[i] == client_id) return true;
  return false;
}
#endif

// A new player at `at_index` of the engine's players (or the end): the lowest
// free client id joins the world and spawns right away, so that it stands at the
// spawn like the players a world is made with. A player that left the game keeps
// its id to come back with. Returns the player, or -1.
//
// Unique Race: the player gets a world of the physics of its own (a copy of the level's, at the
// world's tick), which it is in from the start.
static int32_t add_player(ft_world *world, int32_t at_index) {
#if DD_UNIQUE
  int client_id = 0;
  while (client_id < world->client_count && world->spaces[client_id])
    ++client_id;
  if (client_id >= DDNET_MAX_CLIENTS || !dd_world_reserve(world, client_id + 1, world->player_count + 1)) return -1;
  ddnet_world_t *space = calloc(1, sizeof(*space));
  if (!space) return -1;
  if (!ddnet_world_copy(space, &world->level->prototype)) {
    space_free(space);
    return -1;
  }
  space->tick = world->tick;
  ddnet_player_spawn(space);
  world->spaces[client_id] = space;
  if (client_id >= world->client_count) world->client_count = client_id + 1;
#else
  if (world->player_count >= DDNET_MAX_CLIENTS) return -1;
  int client_id = 0;
  while (client_id < DDNET_MAX_CLIENTS && client_taken(world, client_id))
    ++client_id;
  if (client_id == DDNET_MAX_CLIENTS || !dd_world_reserve(world, client_id + 1, world->player_count + 1) ||
      !ddnet_player_join(&world->core, client_id))
    return -1;
  ddnet_player_spawn(&world->core, client_id);
#endif
  dd_input_t *held = &world->inputs[client_id];
  memset(held, 0, sizeof(*held));
  held->m_TargetY = -1;
  held->m_WantedWeapon = DDNET_WEAPON_GUN;
  world->pain_ticks[client_id] = -GAME_TICK_SPEED;

  const int last = world->player_count++;
  if (at_index < 0 || at_index >= last) {
    world->client_ids[last] = (uint8_t)client_id;
    return last;
  }
  // Track order is the user's, so a player inserted in the middle moves the
  // ones after it down; their clients stay who they are.
  memmove(&world->client_ids[at_index + 1], &world->client_ids[at_index], (size_t)(last - at_index));
  world->client_ids[at_index] = (uint8_t)client_id;
  return at_index;
}

static ft_world *dd_world_create(ft_game *game, const ft_world_desc *desc) {
  if (!desc || !desc->level || !desc->level->loaded) return NULL;
  ft_level *level = (ft_level *)desc->level;

  ft_world *world = calloc(1, sizeof(ft_world));
  if (!world) return NULL;
  world->level = level;
  world->game = game;
  world->index = desc->world_index;
#if DD_UNIQUE
  world->tick = level->prototype.tick;
  world->collision = &level->collision;
#else
  if (!ddnet_world_copy(&world->core, &level->prototype)) {
    free(world);
    return NULL;
  }
#endif

  const int32_t wanted = desc->player_count > 0 ? desc->player_count : 0;
  for (int32_t i = 0; i < wanted; ++i) {
    if (add_player(world, -1) < 0) {
      dd_log(game, FT_LOG_ERROR, "Could not create %d players.", wanted);
      break;
    }
  }
  return world;
}

void dd_world_release(ft_world *world) {
#if DD_UNIQUE
  for (int i = 0; i < world->client_count; ++i)
    space_free(world->spaces[i]);
  free(world->spaces);
  free(world->space_effects);
  world->spaces = NULL;
  world->space_effects = NULL;
  world->client_count = 0;
#else
  ddnet_world_free(&world->core);
#endif
  free(world->client_ids);
  free(world->inputs);
  free(world->pain_ticks);
  world->client_ids = NULL;
  world->inputs = NULL;
  world->pain_ticks = NULL;
  world->player_room = world->client_room = 0;
  dd_replay_free(world);
  free(world->physics_particle_events);
  free(world->physics_damage_events);
  free(world->physics_sound_events);
  free(world->audio_sounds);
}

static void dd_world_destroy(ft_game *game, ft_world *world) {
  (void)game;
  if (!world) return;
  dd_world_release(world);
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

void dd_world_copy(ft_game *game, ft_world *dst, const ft_world *src) {
  if (!dst || !src) return;
  const int world_index = dst->index;
  dst->level = src->level;
  dst->game = game;
  // Keep the destination's identity. Prediction worlds use index -1 so their
  // speculative ticks cannot emit particles into a visible simulation group.
  dst->index = world_index;
  // ddnet_world_copy reuses whatever dst already allocated (and only copies
  // what changed since the two were the same), which is what keeps the engine's
  // constant snapshotting affordable.
  const int clients = dd_world_clients(src);
#if DD_UNIQUE
  if (!dd_world_reserve(dst, clients, src->player_count)) {
    dst->player_count = 0;
    return;
  }
  for (int i = 0; i < clients; ++i) {
    if (!src->spaces[i]) {
      space_free(dst->spaces[i]);
      dst->spaces[i] = NULL;
      continue;
    }
    if (!dst->spaces[i]) dst->spaces[i] = calloc(1, sizeof(ddnet_world_t));
    if (dst->spaces[i]) ddnet_world_copy(dst->spaces[i], src->spaces[i]);
  }
  for (int i = clients; i < dst->client_count; ++i) {
    space_free(dst->spaces[i]);
    dst->spaces[i] = NULL;
  }
  dst->client_count = clients;
  dst->tick = src->tick;
  dst->collision = src->collision;
#else
  ddnet_world_copy(&dst->core, &src->core);
  if (!dd_world_reserve(dst, clients, src->player_count)) {
    dst->player_count = 0;
    return;
  }
#endif
  dst->entities_stale = src->entities_stale;
  // (only the players and client ids that src has)
  dst->player_count = src->player_count;
  if (src->player_count) memcpy(dst->client_ids, src->client_ids, (size_t)src->player_count * sizeof(*dst->client_ids));
  if (clients) {
    memcpy(dst->inputs, src->inputs, (size_t)clients * sizeof(*dst->inputs));
    memcpy(dst->pain_ticks, src->pain_ticks, (size_t)clients * sizeof(*dst->pain_ticks));
  }
  dst->replay_recording = src->replay_recording;
  dst->replay_tick = src->replay_tick;
  memcpy(dst->replay_clients, src->replay_clients, sizeof(dst->replay_clients));
  dd_replay_copy(dst, src);
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
  dd_world_unbind_effects(dst);
}

// Players the engine has no input for keep holding their last one, which is how a track that ran
// out of snippets behaves in DDNet. A plain step is a replay step without recordings, so players
// whose replay just ended are handed back to the physics.
static void dd_world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count) {
  dd_recording_world_step(game, world, inputs, NULL, player_count);
}

static int32_t dd_world_tick(ft_game *game, const ft_world *world) {
  (void)game;
  return ddnet_engine_tick(world);
}

static int32_t dd_world_player_count(ft_game *game, const ft_world *world) {
  (void)game;
  return world ? world->player_count : 0;
}

static bool dd_world_player_view(ft_game *game, const ft_world *world, int32_t player, ft_player_view *out) {
  (void)game;
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  if (!chr || !out) return false;
  const int client_id = ddnet_player_client(world, player);
  out->position = (ft_vec2){chr->pos.x / PX_PER_TILE, chr->pos.y / PX_PER_TILE};
  out->velocity = (ft_vec2){chr->core.vel.x, chr->core.vel.y};
  out->aim = (ft_vec2){(float)chr->core.input.target_x / PX_PER_TILE, (float)chr->core.input.target_y / PX_PER_TILE};
  out->flags = FT_PLAYER_ALIVE;
  if (chr->freeze_time > 0 || dd_tee_deep_frozen(chr)) out->flags |= FT_PLAYER_DISABLED;
  if (dd_world_client(world, client_id)->finish_tick >= 0) out->flags |= FT_PLAYER_FINISHED;
  out->run_start_tick = chr->race_state == DDNET_RACE_NONE ? -1 : chr->start_time;
  return true;
}

static void ddnet_collect_events(ft_game *game, const ft_world *previous, const ft_world *world,
                                 void (*emit)(void *user, const ft_timeline_event *event), void *user) {
  (void)game;
  if (!previous || !world || !emit) return;

  const int count = world->player_count < previous->player_count ? world->player_count : previous->player_count;
  for (int player = 0; player < count; ++player) {
    const ddnet_character_t *before = ddnet_player_character(previous, player);
    const ddnet_character_t *after = ddnet_player_character(world, player);
    const int client_id = ddnet_player_client(world, player);

#if DD_UNIQUE
    // (Unique Race has no time checkpoints)
    (void)before;
    (void)after;
#else
    if (before && after && after->last_time_cp >= 0 && after->last_time_cp < DDNET_MAX_CHECKPOINTS &&
        after->last_time_cp != before->last_time_cp) {
      char text[64];
      snprintf(text, sizeof(text), "Checkpoint %d: %.3fs", after->last_time_cp + 1, after->current_time_cp[after->last_time_cp]);
      const ft_timeline_event event = {.struct_size = sizeof(ft_timeline_event),
                                       .tick = ddnet_engine_tick(world),
                                       .player = player,
                                       .category = "checkpoint",
                                       .text = text,
                                       .color = {0.35f, 0.75f, 1.0f, 1.0f}};
      emit(user, &event);
    }
#endif

    if (client_id >= 0 && ddnet_player_client(previous, player) == client_id &&
        dd_world_client(previous, client_id) && dd_world_client(previous, client_id)->finish_tick < 0 &&
        dd_world_client(world, client_id)->finish_tick >= 0) {
      char text[64];
      snprintf(text, sizeof(text), "Finish: %.3fs", race_time(world, client_id));
      const ft_timeline_event event = {.struct_size = sizeof(ft_timeline_event),
                                       .tick = ddnet_engine_tick(world),
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
  const ddnet_character_t *source = ddnet_player_character(frame->world, frame->source_player);
  const ddnet_character_t *target = ddnet_player_character(frame->world, frame->target_player);
  if (!source || !target) return;
  dd_input_t *input = inout_record;
  input->m_TargetX = (int16_t)(source->pos.x - target->pos.x);
  input->m_TargetY = (int16_t)(source->pos.y - target->pos.y);
}

static int32_t dd_world_add_player(ft_game *game, ft_world *world, int32_t at_index, const ft_player_setup *setup) {
  (void)setup;
  if (!world) return -1;
  const int32_t player = add_player(world, at_index);
  if (player < 0) {
    dd_log(game, FT_LOG_ERROR, "Adding a player failed.");
    return -1;
  }
  dd_replay_insert_player(world, player);
  return player;
}

static bool dd_world_remove_player(ft_game *game, ft_world *world, int32_t player) {
  (void)game;
  if (!world || player < 0 || player >= world->player_count) return false;
#if DD_UNIQUE
  // (its world of the physics goes with it)
  space_drop(world, world->client_ids[player]);
#else
  ddnet_player_leave(&world->core, world->client_ids[player]);
#endif
  memmove(&world->client_ids[player], &world->client_ids[player + 1], (size_t)(world->player_count - player - 1));
  --world->player_count;
  dd_replay_remove_player(world, player);
  return true;
}

// --- serialization -----------------------------------------------------------
//
// Only what a starting state needs: the tick, the tees and their teams. Entities
// in flight (projectiles, lasers) are deliberately dropped, because a project
// stores a point to simulate from, not a mid-flight snapshot.

#if DD_UNIQUE
// Unique Race: the tick and the tee (dd_character_state_unique).
#define DDNET_STATE_MAGIC 0x55515731u /* "UQW1" */
#define DDNET_STATE_VERSION 1
typedef dd_character_state_unique dd_character_state_t;
#else
#define DDNET_STATE_MAGIC 0x444E5732u /* "DDNW2" */
#define DDNET_STATE_VERSION 3
typedef dd_character_state_v3 dd_character_state_t;
#endif

typedef struct {
  uint32_t magic;
  uint32_t version;
  int32_t game_tick;
  int32_t character_count;
  uint32_t character_size;
} ddnet_state_header;

static size_t dd_world_serialize(ft_game *game, const ft_world *world, void *out, size_t out_size) {
  (void)game;
  if (!world) return 0;
  const size_t needed = sizeof(ddnet_state_header) + (size_t)world->player_count * sizeof(dd_character_state_t);
  if (!out) return needed;
  if (out_size < needed) return 0;

  ddnet_state_header header = {.magic = DDNET_STATE_MAGIC,
                               .version = DDNET_STATE_VERSION,
                               .game_tick = ddnet_engine_tick(world),
                               .character_count = world->player_count,
                               .character_size = (uint32_t)sizeof(dd_character_state_t)};
  memcpy(out, &header, sizeof(header));
  for (int i = 0; i < world->player_count; ++i)
    dd_character_state_write((char *)out + sizeof(header) + (size_t)i * sizeof(dd_character_state_t), world, i);
  return needed;
}

static bool dd_world_deserialize(ft_game *game, ft_world *world, const void *data, size_t size) {
  if (!world || !data || size < sizeof(ddnet_state_header)) return false;

  ddnet_state_header header;
  memcpy(&header, data, sizeof(header));
#if DD_UNIQUE
  if (header.magic != DDNET_STATE_MAGIC || header.version != DDNET_STATE_VERSION) return false;
  const size_t character_size = sizeof(dd_character_state_t);
#else
  if (header.magic != DDNET_STATE_MAGIC || (header.version != 2 && header.version != 3)) return false;
  const size_t character_size = header.version == 3 ? sizeof(dd_character_state_v3) : sizeof(dd_character_state_v2);
#endif
  if (header.character_size != character_size) {
    dd_log(game, FT_LOG_WARN, "Stored world was written by a different physics build; ignoring it.");
    return false;
  }
  if (header.character_count < 0 || header.character_count > DDNET_MAX_CLIENTS) return false;
  if ((size_t)header.character_count > (size - sizeof(header)) / character_size) return false;

  // A deserialized world is a fresh starting point: nobody in it replays anything yet, and its tees
  // are the stored ones at the spawn of a new world.
  dd_replay_free(world);
  world->replay_recording = NULL;
  memset(world->replay_clients, 0, sizeof(world->replay_clients));
  ft_level *level = world->level;
#if DD_UNIQUE
  for (int i = 0; i < world->client_count; ++i) {
    space_free(world->spaces[i]);
    world->spaces[i] = NULL;
  }
  world->client_count = 0;
  world->tick = level->prototype.tick;
  world->collision = &level->collision;
#else
  ddnet_world_copy(&world->core, &level->prototype);
#endif
  world->player_count = 0;
  for (int i = 0; i < header.character_count; ++i)
    if (add_player(world, -1) < 0) return false;
  for (int i = 0; i < header.character_count; ++i)
    dd_character_state_read(world, i, (const char *)data + sizeof(header) + (size_t)i * character_size, header.version);
  world->physics_particle_event_count = 0;
  world->physics_damage_event_count = 0;
  world->physics_sound_event_count = 0;
  world->render_physics_effects = false;
  dd_world_unbind_effects(world);
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

static const ft_exporter_desc ddnet_ghost_exporter = {.id = "ghost",
                                                      .display_name = "DDNet Ghost",
                                                      .file_extension = "gho",
                                                      .filter_name = "DDNet Ghost"};

static const ft_exporter_desc *const ddnet_exporters[] = {
    &ddnet_exporter,
    &ddnet_ghost_exporter,
};
#define DD_EXPORTER_COUNT ((uint32_t)(sizeof(ddnet_exporters) / sizeof(ddnet_exporters[0])))

static uint32_t ddnet_exporter_count(ft_game *game) {
  (void)game;
  return DD_EXPORTER_COUNT;
}

static const ft_exporter_desc *ddnet_exporter_desc(ft_game *game, uint32_t index) {
  (void)game;
  return index < DD_EXPORTER_COUNT ? ddnet_exporters[index] : NULL;
}

static bool ddnet_export_run(ft_game *game, uint32_t index, const ft_export_request *request) {
  if (index >= DD_EXPORTER_COUNT) return false;
  if (ddnet_exporters[index] == &ddnet_ghost_exporter) return dd_ghost_export_request(game, request);
  if (ddnet_exporters[index] == &ddnet_exporter) return dd_demo_export(game, request);
  return false;
}

// A path of resolve (the engine's data or cache path of this game) in DDNet's directory instead.
static size_t ddnet_directory_path(size_t (*resolve)(const char *, char *, size_t), bool create, const char *relative,
                                   char *out, size_t out_size) {
#if DD_UNIQUE
  // (".../unique/": this game's id between the last two separators)
  char base[1024];
  const size_t length = resolve("", base, sizeof(base));
  if (length > 1 && length < sizeof(base) && (base[length - 1] == '/' || base[length - 1] == '\\')) {
    const size_t end = length - 1;
    size_t start = end;
    while (start > 0 && base[start - 1] != '/' && base[start - 1] != '\\')
      --start;
    if (end - start == strlen("unique") && memcmp(base + start, "unique", end - start) == 0) {
      if (create) {
        char directory[1024];
        snprintf(directory, sizeof(directory), "%.*sddnet", (int)start, base);
        dd_assets_make_directory(directory);
      }
      const int needed = snprintf(out, out_size, "%.*sddnet%c%s", (int)start, base, base[end], relative ? relative : "");
      return needed > 0 ? (size_t)needed : 0;
    }
  }
#else
  (void)create;
#endif
  return resolve(relative, out, out_size);
}

size_t dd_data_path(const ft_engine_api *engine, const char *relative, char *out, size_t out_size) {
  return ddnet_directory_path(engine->resolve_data_path, false, relative, out, out_size);
}

size_t dd_cache_path(const ft_engine_api *engine, const char *relative, char *out, size_t out_size) {
  return ddnet_directory_path(engine->resolve_cache_path, true, relative, out, out_size);
}

static ft_game *ddnet_create(const ft_engine_api *engine) {
  ft_game *game = calloc(1, sizeof(ft_game));
  if (!game) return NULL;
  game->engine = engine;
  // Recordings open on worker threads, which must not ask the engine for paths.
  if (engine->resolve_cache_path) dd_cache_path(engine, "", game->cache_dir, sizeof(game->cache_dir));

  // Presentation defaults. These are the game's, not the editor's, which is why
  // they no longer sit in the engine's ui_handler_t.
  game->settings = (dd_settings_t){.render_map = true,
                                   .others_alpha = 40,
                                   .chat_team_colors = true,
                                   .map_detail = true,
                                   .entities_view = false,
                                   .render_players = true,
                                   .render_weapons = true,
                                   .render_particles = true,
                                   .render_pickups = true,
                                   .render_cursor = true,
                                   .render_cursor_follow = true,
                                   .render_chat = true,
                                   .chat_font_size = 60,
                                   .chat_width = 200,
                                   .render_nameplates = true,
                                   .render_emoticons = true,
                                   .render_freeze_bars = true,
                                   .render_entity_text = true,
                                   .entity_text_size = 100,
                                   .render_speedups = true,
                                   .render_doors = true,
                                   .nameplate_size = 50,
                                   .nameplate_clan = true,
                                   .nameplate_clan_size = 30,
                                   .nameplate_offset = 30,
                                   .nameplate_team_colors = true,
                                   .show_key_presses = true,
                                   .key_press_size = 30,
                                   .center_dot = false,
                                   .speed_hold_top = true,
                                   .cursor_scale = 1.0f};

  ft_engine_state state;
  memset(&state, 0, sizeof(state));
  if (engine && engine->get_state) engine->get_state(&state);
  game->headless = state.headless;
  if (!game->headless) dd_audio_load(game);

  dd_log(game, FT_LOG_INFO, DD_GAME_NAME " game module ready (ABI %u).", FT_GAME_ABI_VERSION);
  return game;
}

// Once a frame, before anything of it is drawn.
static void ddnet_update(ft_game *game, const ft_engine_state *state) {
  dd_assets_update(game);
  dd_race_hud_update(game, state);
  dd_speed_hud_update(game, state);
}

static void ddnet_destroy(ft_game *game) {
  if (!game) return;
  dd_player_panel_cleanup(game);
  dd_skin_browser_cleanup(game);
  dd_export_window_cleanup(game);
  for (int i = 0; i < game->particle_count; ++i)
    dd_particles_cleanup(&game->particles[i]);
  free(game->particles);
  free(game->seen_teams);
  dd_race_hud_cleanup(game);
  dd_speed_hud_cleanup(game);
  dd_assets_cleanup(game);
  dd_audio_unload(game);
  free(game);
}

static bool ddnet_resources_create(ft_game *game) { return dd_gfx_create(game); }

static void ddnet_resources_destroy(ft_game *game) {
  // Destroying the custom pipeline waits for outstanding GPU work. Do that
  // before releasing browser/map ImGui descriptors which may have appeared in
  // the preceding frame.
  dd_gfx_destroy(game);
  dd_assets_release_gpu(game);
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

static void ddnet_map_settings_menu(const ft_game *game) {
  const bool has_level = game && game->current_level;
  if (!igBeginMenu("Map Server Settings", has_level)) return;

  const map_data_t *map = &game->current_level->map;
  if (map->num_settings <= 0) {
    igTextDisabled("This map has no embedded server settings.");
  } else {
    igTextDisabled("%d embedded command%s (read-only)", map->num_settings, map->num_settings == 1 ? "" : "s");
    igSeparator();
    for (int i = 0; i < map->num_settings; ++i) {
      const char *setting = map->settings && map->settings[i] ? map->settings[i] : "";
      igBulletText("%s", setting[0] ? setting : "(empty)");
    }
  }
  igEndMenu();
}

// DDNet's start screen is its map browser: picking a map is how a run begins.
// The editor hands over the panel and this game fills it.
static void ddnet_ui(ft_game *game, const ft_ui_frame *frame) {
  if (game->headless || !frame) return;
  dd_imgui_attach(game->engine);

  switch (frame->slot) {
  case FT_UI_MAIN_MENU:
    if (igBeginMenu("DDNet", true)) {
      ddnet_map_settings_menu(game);
      // The physics the project runs on: DDNet's, or the Unique Race mod's (the other build of this
      // module), which takes the project along.
      if (igBeginMenu("Physics", game->current_level != NULL && game->engine->switch_backend)) {
        if (igMenuItem_Bool("DDNet", NULL, !DD_UNIQUE, true) && DD_UNIQUE) game->engine->switch_backend("ddnet");
        if (igMenuItem_Bool("Unique Race", NULL, DD_UNIQUE, true) && !DD_UNIQUE) game->engine->switch_backend("unique");
        igEndMenu();
      }
      igSeparator();
      igMenuItem_BoolPtr("Assets", NULL, &game->show_assets, true);
      igMenuItem_BoolPtr("Skin Browser", NULL, &game->show_skin_browser, true);
      igMenuItem_BoolPtr("Timeline Events", NULL, &game->show_events, true);
      if (igMenuItem_Bool("Export Demo...", NULL, false, game->current_level != NULL)) dd_export_window_open(game);
      if (igMenuItem_Bool("Export Ghost...", NULL, false, game->current_level != NULL)) dd_ghost_export_window_open(game);
      igEndMenu();
    }
    break;
  case FT_UI_STATUS_BAR:
    if (game->current_level && game->current_level->name[0]) {
      const char *name = game->current_level->name;
      if (strcmp(name, "map") == 0 && game->engine && game->engine->get_level_name) {
        const char *engine_name = game->engine->get_level_name();
        if (engine_name && engine_name[0] && strcmp(engine_name, "unnamed_level") != 0) {
          snprintf(game->current_level->name, sizeof(game->current_level->name), "%s", engine_name);
          name = game->current_level->name;
        }
      }
#if DD_UNIQUE
      igText("%s (Unique Race, %s)", name, unique_mode_name(game->current_level->config.gamemode));
#else
      igText("%s", name);
#endif
    }
    break;
  case FT_UI_PANELS:
    dd_player_panel_render(game, frame);
    if (game->show_skin_browser) dd_skin_browser_render(game, frame);
    if (game->show_assets) dd_assets_window_render(game);
    dd_events_render(game, frame);
    dd_export_window_render(game);
    dd_ghost_export_window_render(game);
    break;
  default:
    break;
  }
}

// The map browser is available before DDNet's runtime is constructed.
static void ddnet_splash(const ft_engine_api *engine, void **context, const ft_ui_frame *frame) {
  if (!engine || !frame || frame->state.headless) return;
  dd_imgui_attach(engine);
  online_map_manager_t *maps = *context;
  if (!maps) {
    maps = calloc(1, sizeof(*maps));
    if (!maps) return;
    online_map_manager_init(maps, engine);
    *context = maps;
  }
  const ImVec2 avail = igGetContentRegionAvail();
  render_online_map_browser(engine, maps, avail.x, avail.y);
}

static void ddnet_splash_destroy(void *context) {
  online_map_manager_t *maps = context;
  online_map_manager_cleanup(maps, maps->engine);
  free(maps);
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
  if (frame) dd_audio_listen(game, &frame->state.camera.visible);
  dd_render(game, frame);
}

static const ft_game_module module = {
    .struct_size = sizeof(ft_game_module),
    .abi_version = FT_GAME_ABI_VERSION,
    .abi_revision = FT_GAME_ABI_REVISION,

#if DD_UNIQUE
    // Unique Race is a backend of DDNet: the maps of its category in DDNet's browser open with it, and a
    // project moves between the two (DDNet > Physics).
    .info = {.struct_size = sizeof(ft_game_info),
             .id = "unique",
             .display_name = "Unique Race",
             .version = "1.0.0",
             .author = "Teero",
             .url = "https://uniqueclan.net",
             .backend_of = "ddnet"},

    // As many players in a world as DDNet has, each racing alone (a solo server, see ft_world).
    .constraints = {.struct_size = sizeof(ft_game_constraints),
                    .caps = FT_CAP_DYNAMIC_PLAYERS | FT_CAP_LINKED_INPUTS | FT_CAP_WORLD_SERIALIZE | FT_CAP_EXPORTERS |
                            FT_CAP_LEVEL_FROM_MEMORY | FT_CAP_TIMELINE_EVENTS | FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS |
                            FT_CAP_HOSTS_STARTING_STATE | FT_CAP_RECORDINGS,
                    .min_players = 0,
                    .max_players = DDNET_MAX_CLIENTS,
#else
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
                            FT_CAP_HOSTS_STARTING_STATE | FT_CAP_RECORDINGS,
                    .min_players = 0,
                    .max_players = DDNET_MAX_CLIENTS,
#endif
                    .ticks_per_second = 50,
                    .units_per_tile = 1.f,
                    .default_camera_height = 20.f,
#if !DD_UNIQUE
                    .variants = variants,
                    .variant_count = (uint32_t)(sizeof(variants) / sizeof(variants[0])),
#endif
                    .camera_modes = dd_camera_modes,
                    .camera_mode_count = DD_CAMERA_MODE_COUNT,
                    .level_extension = "map",
#if DD_UNIQUE
                    .level_filter_name = "Unique Race map",
#else
                    .level_filter_name = "DDNet map",
#endif
                    // (Unique Race's servers record DDNet's demos)
                    .recording_extension = "demo",
                    .recording_filter_name = "DDNet demo"},

    .input_schema = &input_schema,
    .entity_classes = entity_classes,
    .entity_class_count = DD_CLASS_COUNT,

    .splash = ddnet_splash,
    .splash_destroy = ddnet_splash_destroy,

    .recording_open = dd_recording_open,
    .recording_destroy = dd_recording_destroy,
    .recording_info = dd_recording_info,
    .recording_player = dd_recording_player,
    .recording_level_matches = dd_recording_level_matches,
    .recording_tick_flags = dd_recording_tick_flags,
    .recording_input = dd_recording_input,
    .world_step_playback = dd_recording_world_step,
    .world_place_playback = dd_recording_world_place,
    .recording_events = dd_recording_events,
    .create = ddnet_create,
    .destroy = ddnet_destroy,
    .update = ddnet_update,

    .level_load_path = ddnet_level_load_path,
    .level_load_memory = ddnet_level_load_memory,
    .level_destroy = ddnet_level_destroy,
    .level_info = ddnet_level_info,
    .level_serialize = ddnet_level_serialize,

    .world_create = dd_world_create,
    .world_destroy = dd_world_destroy,
    .world_copy = dd_world_copy,
    .world_step = dd_world_step,
    .world_tick = dd_world_tick,
    .world_player_count = dd_world_player_count,
    .world_player_view = dd_world_player_view,
    .world_add_player = dd_world_add_player,
    .world_remove_player = dd_world_remove_player,
    .world_serialize = dd_world_serialize,
    .world_deserialize = dd_world_deserialize,

    .input_default = ddnet_input_default,
    .input_get = ddnet_input_get,
    .input_set = ddnet_input_set,
    .input_get_vec2 = ddnet_input_get_vec2,
    .input_set_vec2 = ddnet_input_set_vec2,
    .input_describe = ddnet_input_describe,

    .entity_prop_get = ddnet_entity_prop_get,
    .entity_prop_set = ddnet_entity_prop_set,
    .entity_count = ddnet_entity_count,
    .entity_trail = ddnet_entity_trail,

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

    // (the demo export's settings)
    .project_save = dd_export_project_save,
    .project_load = dd_export_project_load,

    .input_get_float = NULL,
    .input_set_float = NULL,
    .linked_actions = linked_actions,
    .linked_action_count = (uint32_t)(sizeof(linked_actions) / sizeof(linked_actions[0])),
    .linked_input_update = ddnet_linked_input_update,
    .input_effect_count = dd_input_effect_count,
    .input_effect_desc = dd_input_effect_desc,
    .input_effect_default = dd_input_effect_default,
    .input_effect_apply = dd_input_effect_apply,
    .input_effect_ui = dd_input_effect_ui,
    .world_audio = dd_world_audio,
    .audio_spatialize = dd_audio_spatialize,
    .event_audio = dd_event_audio,
};

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  if (engine_abi_version != FT_GAME_ABI_VERSION) return NULL;
#if DD_UNIQUE
  fill_unique_props();
#endif
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
  SET_RENDER_MAP = 0,
  SET_RENDER_PLAYERS,
  SET_OTHERS_ALPHA,
  SET_RENDER_WEAPONS,
  SET_RENDER_PARTICLES,
  SET_RENDER_PICKUPS,
  SET_RENDER_CURSOR,
  SET_CURSOR_SCALE,
  SET_CURSOR_FOLLOW,
  SET_CENTER_DOT,
  SET_RENDER_CHAT,
  SET_CHAT_FONT_SIZE,
  SET_CHAT_WIDTH,
  SET_CHAT_TEAM_COLORS,
  SET_RENDER_EMOTICONS,
  SET_RENDER_FREEZE_BARS,
  SET_RENDER_ENTITY_TEXT,
  SET_ENTITY_TEXT_SIZE,
  SET_RENDER_SPEEDUPS,
  SET_RENDER_DOORS,
  SET_RENDER_NAMEPLATES,
  SET_NAMEPLATE_SIZE,
  SET_NAMEPLATE_CLAN,
  SET_NAMEPLATE_CLAN_SIZE,
  SET_NAMEPLATE_OFFSET,
  SET_NAMEPLATE_TEAM_COLORS,
  SET_ENTITIES_VIEW,
  SET_MAP_DETAIL,
  SET_CURSOR_ALL,
  SET_SHOW_KEY_PRESSES,
  SET_KEY_PRESS_SIZE,
  SET_RENDER_TIMER,
  SET_RENDER_SPLITS,
  SET_PICKUP_HITBOXES,
  SET_RENDER_SPEED,
  SET_RENDER_SPEED_CURVE,
  SET_SPEED_HOLD_TOP,
  SET_RENDER_POSITION,
  // DDNet's assets (dd_assets.c): a pack per type, then the parts mixed in from others.
  SET_ASSET_FIRST,
  SET_ASSET_MIX_FIRST = SET_ASSET_FIRST + DD_ASSET_TYPE_COUNT,
  SET_COUNT = SET_ASSET_MIX_FIRST + DD_ASSET_TYPE_COUNT
};

static const ft_setting_desc ddnet_settings[SET_COUNT] = {
    [SET_ENTITIES_VIEW] = {"entities_view", "Entities view", "Show collision and entity tiles instead of the map design", "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_MAP_DETAIL] = {"map_detail", "Map detail", "Render decorative detail layers", "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    // Groups are what the Preferences window makes its pages from, so each one
    // is a short list of things you would change together. The order below is
    // only how the source reads: a group is gathered by name, not by adjacency.
    [SET_RENDER_MAP] = {"render_map", "Map", "The map's own tile layers and the overlays drawn onto them", "World",
                        FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_PLAYERS] = {"render_players", "Tees", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_OTHERS_ALPHA] = {"others_alpha", "Opacity of other teams",
                          "Of the tees, plates, shots and effects of tees in another team than the selected one, in percent",
                          "World", FT_VALUE_INT, 0, 100, FT_SETTING_RENDER},
    [SET_RENDER_WEAPONS] = {"render_weapons", "Weapons and hooks", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_PARTICLES] = {"render_particles", "Particles", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_PICKUPS] = {"render_pickups", "Pickups", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_PICKUP_HITBOXES] = {"pickup_hitboxes", "Pickup hitboxes", "The circle a tee's center has to enter to collect a pickup",
                             "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_CURSOR] = {"render_cursor", "Show crosshairs", NULL, "Crosshair", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_CURSOR_SCALE] = {"cursor_scale", "Crosshair scale", NULL, "Crosshair", FT_VALUE_FLOAT, 0.1, 2.0, FT_SETTING_RENDER},
    [SET_CURSOR_FOLLOW] = {"cursor_follow", "Crosshair in follow camera", NULL, "Crosshair", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_CURSOR_ALL] = {"cursor_all", "Crosshairs of every player",
                        "Not only on the player the camera follows (the game's follow camera, or the camera animation's characters)",
                        "Crosshair", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_CENTER_DOT] = {"center_dot", "Show center dot", "Marks the tee's exact position", "Crosshair", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_TIMER] = {"render_timer", "Race timer", NULL, "HUD", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_SPLITS] = {"render_splits", "Checkpoint splits",
                           "Under the timer after a checkpoint or the finish: how far the tee is behind the fastest other group "
                           "there, or its own time where no other group is faster",
                           "HUD", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_SPEED] = {"render_speed", "Speed", "How fast the tee moves along the map, against the most the velocity ramp lets it reach",
                          "HUD", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_SPEED_CURVE] = {"render_speed_curve", "Velocity ramp plot",
                                "What the velocity ramp makes of every raw speed, with the tee on it", "HUD", FT_VALUE_BOOL, 0, 0,
                                FT_SETTING_RENDER},
    [SET_SPEED_HOLD_TOP] = {"speed_hold_top", "Hold the top speed",
                            "Show the fastest the tee has been so far instead of its speed at each tick, which wobbles as it bounces",
                            "HUD", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_POSITION] = {"render_position", "Position", "How far along the map the tee is, in blocks, out of the map's width",
                             "HUD", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_CHAT] = {"render_chat", "Show chat", NULL, "Chat", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_CHAT_FONT_SIZE] = {"chat_font_size", "Chat font size", NULL, "Chat", FT_VALUE_INT, 10, 100, FT_SETTING_RENDER},
    [SET_CHAT_WIDTH] = {"chat_width", "Chat width", NULL, "Chat", FT_VALUE_INT, 140, 400, FT_SETTING_RENDER},
    [SET_CHAT_TEAM_COLORS] = {"chat_team_colors", "Team colors", NULL, "Chat", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_EMOTICONS] = {"render_emoticons", "Emoticons", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_FREEZE_BARS] = {"render_freeze_bars", "Freeze bars", NULL, "World", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_ENTITY_TEXT] = {"render_entity_text", "Entity text", NULL, "Map entities", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_ENTITY_TEXT_SIZE] = {"entity_text_size", "Entity text size", NULL, "Map entities", FT_VALUE_INT, 20, 100, FT_SETTING_RENDER},
    [SET_RENDER_SPEEDUPS] = {"render_speedups", "Speedup arrows", NULL, "Map entities", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_DOORS] = {"render_doors", "Doors", NULL, "Map entities", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_RENDER_NAMEPLATES] = {"render_nameplates", "Show nameplates", NULL, "Nameplates", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_NAMEPLATE_SIZE] = {"nameplate_size", "Nameplate size", NULL, "Nameplates", FT_VALUE_INT, -50, 100, FT_SETTING_RENDER},
    [SET_NAMEPLATE_CLAN] = {"nameplate_clan", "Show clan", NULL, "Nameplates", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_NAMEPLATE_CLAN_SIZE] = {"nameplate_clan_size", "Clan size", NULL, "Nameplates", FT_VALUE_INT, -50, 100, FT_SETTING_RENDER},
    [SET_NAMEPLATE_OFFSET] = {"nameplate_offset", "Nameplate offset", NULL, "Nameplates", FT_VALUE_INT, 10, 50, FT_SETTING_RENDER},
    [SET_NAMEPLATE_TEAM_COLORS] = {"nameplate_team_colors", "Team colors", NULL, "Nameplates", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_SHOW_KEY_PRESSES] = {"show_key_presses", "Show players' key presses", NULL, "Key presses", FT_VALUE_BOOL, 0, 0, FT_SETTING_RENDER},
    [SET_KEY_PRESS_SIZE] = {"key_press_size", "Size of key press icons", NULL, "Key presses", FT_VALUE_INT, -50, 100, FT_SETTING_RENDER},
    // Chosen in the Assets window (dd_assets_window.c).
    [SET_ASSET_FIRST + DD_ASSET_ENTITIES] = {"asset_entities", "Entities", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_FIRST + DD_ASSET_GAME] = {"asset_game", "Game", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_FIRST + DD_ASSET_EMOTICONS] = {"asset_emoticons", "Emoticons", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_FIRST + DD_ASSET_PARTICLES] = {"asset_particles", "Particles", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_FIRST + DD_ASSET_HUD] = {"asset_hud", "HUD", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_FIRST + DD_ASSET_EXTRAS] = {"asset_extras", "Extras", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_ENTITIES] = {"asset_entities_mix", "Entities mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_GAME] = {"asset_game_mix", "Game mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_EMOTICONS] = {"asset_emoticons_mix", "Emoticons mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_PARTICLES] = {"asset_particles_mix", "Particles mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_HUD] = {"asset_hud_mix", "HUD mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
    [SET_ASSET_MIX_FIRST + DD_ASSET_EXTRAS] = {"asset_extras_mix", "Extras mix", NULL, "Assets", FT_VALUE_STRING, 0, 0, FT_SETTING_HIDDEN},
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
  if (index >= SET_ASSET_FIRST && index < SET_COUNT) {
    const bool mix = index >= SET_ASSET_MIX_FIRST;
    *out = (ft_value){.kind = FT_VALUE_STRING,
                      .as.s = dd_assets_setting(game, (int)(index - (mix ? SET_ASSET_MIX_FIRST : SET_ASSET_FIRST)), mix)};
    return true;
  }
  switch (index) {
  case SET_ENTITIES_VIEW:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->entities_view};
    return true;
  case SET_MAP_DETAIL:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->map_detail};
    return true;
  case SET_RENDER_MAP:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_map};
    return true;
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
  case SET_PICKUP_HITBOXES:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->pickup_hitboxes};
    return true;
  case SET_RENDER_SPEED:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_speed};
    return true;
  case SET_RENDER_SPEED_CURVE:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_speed_curve};
    return true;
  case SET_SPEED_HOLD_TOP:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->speed_hold_top};
    return true;
  case SET_RENDER_POSITION:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_position};
    return true;
  case SET_RENDER_CURSOR:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_cursor};
    return true;
  case SET_RENDER_TIMER:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_timer};
    return true;
  case SET_RENDER_SPLITS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_splits};
    return true;
  case SET_CURSOR_SCALE:
    *out = (ft_value){.kind = FT_VALUE_FLOAT, .as.f = s->cursor_scale};
    return true;
  case SET_CURSOR_FOLLOW:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_cursor_follow};
    return true;
  case SET_CURSOR_ALL:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_cursor_all};
    return true;
  case SET_CENTER_DOT:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->center_dot};
    return true;
  case SET_RENDER_CHAT:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_chat};
    return true;
  case SET_CHAT_FONT_SIZE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->chat_font_size};
    return true;
  case SET_CHAT_WIDTH:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->chat_width};
    return true;
  case SET_CHAT_TEAM_COLORS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->chat_team_colors};
    return true;
  case SET_OTHERS_ALPHA:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->others_alpha};
    return true;
  case SET_RENDER_EMOTICONS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_emoticons};
    return true;
  case SET_RENDER_FREEZE_BARS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_freeze_bars};
    return true;
  case SET_RENDER_ENTITY_TEXT:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_entity_text};
    return true;
  case SET_ENTITY_TEXT_SIZE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->entity_text_size};
    return true;
  case SET_RENDER_NAMEPLATES:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_nameplates};
    return true;
  case SET_NAMEPLATE_SIZE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->nameplate_size};
    return true;
  case SET_NAMEPLATE_CLAN:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->nameplate_clan};
    return true;
  case SET_NAMEPLATE_CLAN_SIZE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->nameplate_clan_size};
    return true;
  case SET_NAMEPLATE_OFFSET:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->nameplate_offset};
    return true;
  case SET_NAMEPLATE_TEAM_COLORS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->nameplate_team_colors};
    return true;
  case SET_SHOW_KEY_PRESSES:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->show_key_presses};
    return true;
  case SET_KEY_PRESS_SIZE:
    *out = (ft_value){.kind = FT_VALUE_INT, .as.i = s->key_press_size};
    return true;
  case SET_RENDER_SPEEDUPS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_speedups};
    return true;
  case SET_RENDER_DOORS:
    *out = (ft_value){.kind = FT_VALUE_BOOL, .as.b = s->render_doors};
    return true;
  default:
    return false;
  }
}

// The descriptor's bounds are advice to the UI; a hand-edited config can still
// arrive with anything, and these settings feed sizes and offsets directly.
static int64_t clamp_setting(int64_t value, int64_t low, int64_t high) {
  return value < low ? low : (value > high ? high : value);
}

static bool ddnet_setting_set(ft_game *game, uint32_t index, const ft_value *value) {
  dd_settings_t *s = &game->settings;
  if (index >= SET_ASSET_FIRST && index < SET_COUNT) {
    if (value->kind != FT_VALUE_STRING) return false;
    const bool mix = index >= SET_ASSET_MIX_FIRST;
    dd_assets_set_setting(game, (int)(index - (mix ? SET_ASSET_MIX_FIRST : SET_ASSET_FIRST)), mix, value->as.s);
    return true;
  }
  switch (index) {
  case SET_ENTITIES_VIEW:
    s->entities_view = value->as.b;
    return true;
  case SET_MAP_DETAIL:
    s->map_detail = value->as.b;
    return true;
  case SET_RENDER_MAP:
    s->render_map = value->as.b;
    return true;
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
  case SET_PICKUP_HITBOXES:
    s->pickup_hitboxes = value->as.b;
    return true;
  case SET_RENDER_SPEED:
    s->render_speed = value->as.b;
    return true;
  case SET_RENDER_SPEED_CURVE:
    s->render_speed_curve = value->as.b;
    return true;
  case SET_SPEED_HOLD_TOP:
    s->speed_hold_top = value->as.b;
    return true;
  case SET_RENDER_POSITION:
    s->render_position = value->as.b;
    return true;
  case SET_RENDER_CURSOR:
    s->render_cursor = value->as.b;
    return true;
  case SET_RENDER_TIMER:
    s->render_timer = value->as.b;
    return true;
  case SET_RENDER_SPLITS:
    s->render_splits = value->as.b;
    return true;
  case SET_CURSOR_SCALE:
    s->cursor_scale = (float)value->as.f;
    return true;
  case SET_CURSOR_FOLLOW:
    s->render_cursor_follow = value->as.b;
    return true;
  case SET_CURSOR_ALL:
    s->render_cursor_all = value->as.b;
    return true;
  case SET_CENTER_DOT:
    s->center_dot = value->as.b;
    return true;
  case SET_RENDER_CHAT:
    s->render_chat = value->as.b;
    return true;
  case SET_CHAT_FONT_SIZE:
    s->chat_font_size = (int)clamp_setting(value->as.i, 10, 100);
    return true;
  case SET_CHAT_WIDTH:
    s->chat_width = (int)clamp_setting(value->as.i, 140, 400);
    return true;
  case SET_CHAT_TEAM_COLORS:
    s->chat_team_colors = value->as.b;
    return true;
  case SET_OTHERS_ALPHA:
    s->others_alpha = (int)clamp_setting(value->as.i, 0, 100);
    return true;
  case SET_RENDER_EMOTICONS:
    s->render_emoticons = value->as.b;
    return true;
  case SET_RENDER_FREEZE_BARS:
    s->render_freeze_bars = value->as.b;
    return true;
  case SET_RENDER_ENTITY_TEXT:
    s->render_entity_text = value->as.b;
    return true;
  case SET_ENTITY_TEXT_SIZE:
    s->entity_text_size = (int)clamp_setting(value->as.i, 20, 100);
    // The number sheets bake the size in, so this re-uploads them.
    dd_text_set_entity_scale(game, s->entity_text_size);
    return true;
  case SET_RENDER_NAMEPLATES:
    s->render_nameplates = value->as.b;
    return true;
  case SET_NAMEPLATE_SIZE:
    s->nameplate_size = (int)clamp_setting(value->as.i, -50, 100);
    return true;
  case SET_NAMEPLATE_CLAN:
    s->nameplate_clan = value->as.b;
    return true;
  case SET_NAMEPLATE_CLAN_SIZE:
    s->nameplate_clan_size = (int)clamp_setting(value->as.i, -50, 100);
    return true;
  case SET_NAMEPLATE_OFFSET:
    s->nameplate_offset = (int)clamp_setting(value->as.i, 10, 50);
    return true;
  case SET_NAMEPLATE_TEAM_COLORS:
    s->nameplate_team_colors = value->as.b;
    return true;
  case SET_SHOW_KEY_PRESSES:
    s->show_key_presses = value->as.b;
    return true;
  case SET_KEY_PRESS_SIZE:
    s->key_press_size = (int)clamp_setting(value->as.i, -50, 100);
    return true;
  case SET_RENDER_SPEEDUPS:
    s->render_speedups = value->as.b;
    return true;
  case SET_RENDER_DOORS:
    s->render_doors = value->as.b;
    return true;
  default:
    return false;
  }
}
