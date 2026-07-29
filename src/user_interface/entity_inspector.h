#ifndef ENTITY_INSPECTOR_H
#define ENTITY_INSPECTOR_H

#include <ddnet_physics/gamecore.h>
#include <stdbool.h>
#include <stdint.h>

struct gfx_handler_t;

typedef enum {
  ENTITY_INSPECTOR_NONE,
  ENTITY_INSPECTOR_PROJECTILE,
  ENTITY_INSPECTOR_LASER,
} entity_inspector_kind_t;

typedef struct {
  uintptr_t world;
  uintptr_t collision;
  uintptr_t prev_type_entity;
  uintptr_t next_type_entity;
  mvec2 pos;
  int obj_type;
  int number;
  int layer;
  bool marked_for_destroy;
  bool spawned;
} entity_base_snapshot_t;

typedef struct {
  entity_base_snapshot_t base;
  mvec2 direction;
  uintptr_t tuning;
  int life_span;
  int owner;
  uint32_t owner_spawn_generation;
  int type;
  int start_tick;
  int bouncing;
  bool explosive;
  bool freeze;
  bool is_solo;
} projectile_snapshot_t;

typedef struct {
  entity_base_snapshot_t base;
  uintptr_t tuning;
  mvec2 from;
  mvec2 dir;
  mvec2 tele_pos;
  mvec2 prev_pos;
  bool was_tele;
  float energy;
  int bounces;
  int eval_tick;
  int owner;
  bool zero_energy_bounce_in_last_tick;
  int type;
  bool teleport_cancelled;
  bool is_blue_teleport;
} laser_snapshot_t;

typedef struct {
  bool valid;
  bool show;
  entity_inspector_kind_t kind;
  int timeline_tick;
  int world_tick;
  int type_index;
  mvec2 previous_position;
  mvec2 current_position;
  projectile_snapshot_t projectile;
  laser_snapshot_t laser;
  bool has_tuning;
  STuningParams tuning;
} entity_inspector_t;

void entity_inspector_clear(entity_inspector_t *inspector);
bool entity_inspector_pick(entity_inspector_t *inspector, const SWorldCore *world, struct gfx_handler_t *gfx, float intra, float mouse_x,
                           float mouse_y, int timeline_tick);
void entity_inspector_render(entity_inspector_t *inspector);
void entity_inspector_render_highlight(const entity_inspector_t *inspector, struct gfx_handler_t *gfx);

#endif
