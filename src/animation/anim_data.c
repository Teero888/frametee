#include "anim_data.h"
#include "anim_system.h"
#include <ddnet_physics/gamecore.h>
#include <string.h>

const data_container_t game_data = {
    .weapons = {.id = {[WEAPON_HAMMER] = {.firedelay = 125, .offsetx = 4, .offsety = -20, .visual_size = 96.0f, .num_muzzles = 0},

                       [WEAPON_GUN] = {.firedelay = 125,
                                       .offsetx = 32,
                                       .offsety = 4,
                                       .muzzleoffsetx = 58.8752f,
                                       .muzzleoffsety = 6,
                                       .muzzleduration = 5,
                                       .visual_size = 64.0f,
                                       .num_muzzles = 3},

                       [WEAPON_SHOTGUN] = {.firedelay = 500,
                                           .offsetx = 24,
                                           .offsety = -2,
                                           .muzzleoffsetx = 83.3128f,
                                           .muzzleoffsety = 6,
                                           .muzzleduration = 5,
                                           .visual_size = 96.0f,
                                           .num_muzzles = 3},

                       [WEAPON_GRENADE] = {.firedelay = 500, .offsetx = 24, .offsety = -2, .visual_size = 96.0f, .num_muzzles = 0},

                       [WEAPON_LASER] = {.firedelay = 800, .offsetx = 24, .offsety = -2, .visual_size = 92.0f, .num_muzzles = 0},

                       [WEAPON_NINJA] = {.firedelay = 800,
                                         .offsetx = 0,
                                         .offsety = 0,
                                         .muzzleoffsetx = 40,
                                         .muzzleoffsety = -4,
                                         .muzzleduration = 5,
                                         .visual_size = 96.0f,
                                         .num_muzzles = 3}}}};

/* ---- Base ---- */
static const anim_keyframe_t base_body[] = {{0.0f, 0.0f, -4.0f, 0.0f}};
static const anim_keyframe_t base_backfoot[] = {{0.0f, 0.0f, 10.0f, 0.0f}};
static const anim_keyframe_t base_frontfoot[] = {{0.0f, 0.0f, 10.0f, 0.0f}};

const animation_t anim_base = {"base", {1, base_body}, {1, base_backfoot}, {1, base_frontfoot}, {0, NULL}};

/* ---- Idle ---- */
static const anim_keyframe_t idle_backfoot[] = {{0.0f, -7.0f, 0.0f, 0.0f}};
static const anim_keyframe_t idle_frontfoot[] = {{0.0f, 7.0f, 0.0f, 0.0f}};

const animation_t anim_idle = {"idle", {0, NULL}, {1, idle_backfoot}, {1, idle_frontfoot}, {0, NULL}};

/* ---- Walk ---- */
static const anim_keyframe_t walk_body[] = {{0.0f, 0, 0, 0}, {0.2f, 0, -1, 0}, {0.4f, 0, 0, 0}, {0.6f, 0, 0, 0}, {0.8f, 0, -1, 0}, {1.0f, 0, 0, 0}};
static const anim_keyframe_t walk_back[] = {{0.0f, 8, 0, 0}, {0.2f, -8, 0, 0}, {0.4f, -10, -4, 0.2f}, {0.6f, -8, -8, 0.3f}, {0.8f, 4, -4, -0.2f}, {1.0f, 8, 0, 0}};
static const anim_keyframe_t walk_front[] = {{0.0f, -10, -4, 0.2f}, {0.2f, -8, -8, 0.3f}, {0.4f, 4, -4, -0.2f}, {0.6f, 8, 0, 0}, {0.8f, 8, 0, 0}, {1.0f, -10, -4, 0.2f}};

const animation_t anim_walk = {"walk", {6, walk_body}, {6, walk_back}, {6, walk_front}, {0, NULL}};

/* ---- Run Right ---- */
static const anim_keyframe_t runr_body[] = {{0.0f, 0, -1, 0}, {0.2f, 0, 0, 0}, {0.4f, 0, 0, 0}, {0.6f, 0, -1, 0}, {0.8f, 0, 0, 0}, {1.0f, 0, -1, 0}};
static const anim_keyframe_t runr_back[] = {{0.0f, -18, -8, 0.27f}, {0.2f, 0, -8, 0.2f}, {0.4f, 13, -4.5f, -0.05f}, {0.6f, 7, 0, 0}, {0.8f, -6, 0, 0}, {1.0f, -18, -8, 0.27f}};
static const anim_keyframe_t runr_front[] = {{0.0f, 11, -2.5f, -0.05f}, {0.2f, -3, 0, 0}, {0.4f, -18, -8, 0.27f}, {0.6f, -11, -8, 0.3f}, {0.8f, 14, -5, -0.1f}, {1.0f, 11, -2.5f, -0.05f}};

const animation_t anim_run_right = {"run_right", {6, runr_body}, {6, runr_back}, {6, runr_front}, {0, NULL}};

/* ---- Run Left ---- */
static const anim_keyframe_t runl_body[] = {{0.0f, 0, -1, 0}, {0.2f, 0, 0, 0}, {0.4f, 0, -1, 0}, {0.6f, 0, 0, 0}, {0.8f, 0, 0, 0}, {1.0f, 0, -1, 0}};
static const anim_keyframe_t runl_back[] = {{0.0f, 18, -8, -0.27f}, {0.2f, 6, 0, 0}, {0.4f, -7, 0, 0}, {0.6f, -13, -4.5f, 0.05f}, {0.8f, 0, -8, -0.2f}, {1.0f, 18, -8, -0.27f}};
static const anim_keyframe_t runl_front[] = {{0.0f, -11, -2.5f, 0.05f}, {0.2f, -14, -5, 0.1f}, {0.4f, 11, -8, -0.3f}, {0.6f, 18, -8, -0.27f}, {0.8f, 3, 0, 0}, {1.0f, -11, -2.5f, 0.05f}};

const animation_t anim_run_left = {"run_left", {6, runl_body}, {6, runl_back}, {6, runl_front}, {0, NULL}};

/* ---- Hammer Swing ---- */
static const anim_keyframe_t hammer_attach[] = {
    {0.0f, 0, 0, -0.10f}, {0.3f, 0, 0, 0.25f}, {0.4f, 0, 0, 0.30f}, {0.5f, 0, 0, 0.25f}, {1.0f, 0, 0, -0.10f}};
const animation_t anim_hammer_swing = {"hammer_swing", {0, NULL}, {0, NULL}, {0, NULL}, {5, hammer_attach}};

/* ---- Ninja Swing ---- */
static const anim_keyframe_t ninja_attach[] = {{0.00f, 0, 0, -0.25f}, {0.10f, 0, 0, -0.05f}, {0.15f, 0, 0, 0.35f}, {0.42f, 0, 0, 0.40f}, {0.50f, 0, 0, 0.35f}, {1.00f, 0, 0, -0.25f}};
const animation_t anim_ninja_swing = {"ninja_swing", {0, NULL}, {0, NULL}, {0, NULL}, {6, ninja_attach}};

/* ---- Inair ---- */
static const anim_keyframe_t inair_backfoot[] = {{0.0f, -3.0f, 0.0f, -0.1f}};
static const anim_keyframe_t inair_frontfoot[] = {{0.0f, 3.0f, 0.0f, -0.1f}};

const animation_t anim_inair = {"inair", {0, NULL}, {1, inair_backfoot}, {1, inair_frontfoot}, {0, NULL}};

/* ---- Sit Left ---- */
static const anim_keyframe_t sitl_body[] = {{0.0f, 0.0f, 3.0f, 0.0f}};
static const anim_keyframe_t sitl_backfoot[] = {{0.0f, -12.0f, 0.0f, 0.1f}};
static const anim_keyframe_t sitl_frontfoot[] = {{0.0f, -8.0f, 0.0f, 0.1f}};

const animation_t anim_sit_left = {"sit_left", {1, sitl_body}, {1, sitl_backfoot}, {1, sitl_frontfoot}, {0, NULL}};

/* ---- Sit Right ---- */
static const anim_keyframe_t sitr_body[] = {{0.0f, 0.0f, 3.0f, 0.0f}};
static const anim_keyframe_t sitr_backfoot[] = {{0.0f, 12.0f, 0.0f, -0.1f}};
static const anim_keyframe_t sitr_frontfoot[] = {{0.0f, 8.0f, 0.0f, -0.1f}};

const animation_t anim_sit_right = {"sit_right", {1, sitr_body}, {1, sitr_backfoot}, {1, sitr_frontfoot}, {0, NULL}};
