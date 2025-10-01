#ifndef ANIM_DATA_H
#define ANIM_DATA_H

#include "anim_system.h"
#include "cglm/types.h"
#include "gamecore.h"

// Based on content.py
typedef struct {
  int firedelay;
  float offsetx;
  float offsety;
  float muzzleoffsetx;
  float muzzleoffsety;
  float muzzleduration;
  int num_muzzles;
  vec2 body_size;
  vec2 muzzle_size;
} weapon_spec_t;

typedef struct {
  weapon_spec_t id[NUM_WEAPONS];
} weapon_specs_t;

typedef struct {
  weapon_specs_t weapons;
} data_container_t;

extern const data_container_t game_data;

extern const animation_t anim_base;
extern const animation_t anim_idle;
extern const animation_t anim_walk;
extern const animation_t anim_run_left;
extern const animation_t anim_run_right;
extern const animation_t anim_inair;
extern const animation_t anim_sit_left;
extern const animation_t anim_sit_right;
extern const animation_t anim_hammer_swing;
extern const animation_t anim_ninja_swing;

#endif // ANIM_DATA_H
