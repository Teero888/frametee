#include "entity_inspector.h"

#include <math.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <string.h>
#include <system/include_cimgui.h>

static float distance_squared_to_segment(float point_x, float point_y, float start_x, float start_y, float end_x, float end_y) {
  const float delta_x = end_x - start_x;
  const float delta_y = end_y - start_y;
  const float length_squared = delta_x * delta_x + delta_y * delta_y;
  float amount = 0.0f;
  if (length_squared > 0.0f) {
    amount = ((point_x - start_x) * delta_x + (point_y - start_y) * delta_y) / length_squared;
    amount = fminf(fmaxf(amount, 0.0f), 1.0f);
  }
  const float nearest_x = start_x + amount * delta_x;
  const float nearest_y = start_y + amount * delta_y;
  const float distance_x = point_x - nearest_x;
  const float distance_y = point_y - nearest_y;
  return distance_x * distance_x + distance_y * distance_y;
}

static const char *weapon_name(int weapon) {
  switch (weapon) {
  case WEAPON_HAMMER:
    return "Hammer";
  case WEAPON_GUN:
    return "Gun";
  case WEAPON_SHOTGUN:
    return "Shotgun";
  case WEAPON_GRENADE:
    return "Grenade";
  case WEAPON_LASER:
    return "Laser";
  case WEAPON_NINJA:
    return "Ninja";
  default:
    return "Unknown";
  }
}

static const char *kind_name(entity_inspector_kind_t kind) {
  switch (kind) {
  case ENTITY_INSPECTOR_PROJECTILE:
    return "Projectile";
  case ENTITY_INSPECTOR_LASER:
    return "Laser";
  default:
    return "None";
  }
}

static const char *bool_name(bool value) { return value ? "true" : "false"; }

static entity_base_snapshot_t snapshot_base(const SEntity *entity) {
  return (entity_base_snapshot_t){
      .world = (uintptr_t)entity->m_pWorld,
      .collision = (uintptr_t)entity->m_pCollision,
      .prev_type_entity = (uintptr_t)entity->m_pPrevTypeEntity,
      .next_type_entity = (uintptr_t)entity->m_pNextTypeEntity,
      .pos = entity->m_Pos,
      .obj_type = entity->m_ObjType,
      .number = entity->m_Number,
      .layer = entity->m_Layer,
      .marked_for_destroy = entity->m_MarkedForDestroy,
      .spawned = entity->m_Spawned,
  };
}

static projectile_snapshot_t snapshot_projectile(const SProjectile *projectile) {
  return (projectile_snapshot_t){
      .base = snapshot_base(&projectile->m_Base),
      .direction = projectile->m_Direction,
      .tuning = (uintptr_t)projectile->m_pTuning,
      .life_span = projectile->m_LifeSpan,
      .owner = projectile->m_Owner,
      .owner_spawn_generation = projectile->m_OwnerSpawnGeneration,
      .type = projectile->m_Type,
      .start_tick = projectile->m_StartTick,
      .bouncing = projectile->m_Bouncing,
      .explosive = projectile->m_Explosive,
      .freeze = projectile->m_Freeze,
      .is_solo = projectile->m_IsSolo,
  };
}

static laser_snapshot_t snapshot_laser(const SLaser *laser) {
  return (laser_snapshot_t){
      .base = snapshot_base(&laser->m_Base),
      .tuning = (uintptr_t)laser->m_pTuning,
      .from = laser->m_From,
      .dir = laser->m_Dir,
      .tele_pos = laser->m_TelePos,
      .prev_pos = laser->m_PrevPos,
      .was_tele = laser->m_WasTele,
      .energy = laser->m_Energy,
      .bounces = laser->m_Bounces,
      .eval_tick = laser->m_EvalTick,
      .owner = laser->m_Owner,
      .zero_energy_bounce_in_last_tick = laser->m_ZeroEnergyBounceInLastTick,
      .type = laser->m_Type,
      .teleport_cancelled = laser->m_TeleportCancelled,
      .is_blue_teleport = laser->m_IsBlueTeleport,
  };
}

void entity_inspector_clear(entity_inspector_t *inspector) {
  if (!inspector) return;
  memset(inspector, 0, sizeof(*inspector));
}

bool entity_inspector_pick(entity_inspector_t *inspector, const SWorldCore *world, gfx_handler_t *gfx, float intra, float mouse_x, float mouse_y,
                           int timeline_tick) {
  if (!inspector || !world || !gfx) return false;

  entity_inspector_kind_t best_kind = ENTITY_INSPECTOR_NONE;
  const SProjectile *best_projectile = NULL;
  const SLaser *best_laser = NULL;
  mvec2 best_previous_position = vec2_init(0.0f, 0.0f);
  mvec2 best_current_position = vec2_init(0.0f, 0.0f);
  int best_type_index = -1;
  float best_distance_squared = 24.0f * 24.0f;

  int type_index = 0;
  for (const SProjectile *projectile = (const SProjectile *)world->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; projectile;
       projectile = (const SProjectile *)projectile->m_Base.m_pNextTypeEntity, ++type_index) {
    const float previous_time = (world->m_GameTick - projectile->m_StartTick - 1) / (float)GAME_TICK_SPEED;
    const float current_time = (world->m_GameTick - projectile->m_StartTick) / (float)GAME_TICK_SPEED;
    const mvec2 previous_position = prj_get_pos((SProjectile *)projectile, previous_time);
    const mvec2 current_position = prj_get_pos((SProjectile *)projectile, current_time);
    const float rendered_x = (vgetx(previous_position) + (vgetx(current_position) - vgetx(previous_position)) * intra) / 32.0f;
    const float rendered_y = (vgety(previous_position) + (vgety(current_position) - vgety(previous_position)) * intra) / 32.0f;
    float screen_x, screen_y;
    world_to_screen(gfx, rendered_x, rendered_y, &screen_x, &screen_y);
    const float delta_x = mouse_x - screen_x;
    const float delta_y = mouse_y - screen_y;
    const float distance_squared = delta_x * delta_x + delta_y * delta_y;
    if (distance_squared <= best_distance_squared) {
      best_distance_squared = distance_squared;
      best_kind = ENTITY_INSPECTOR_PROJECTILE;
      best_projectile = projectile;
      best_laser = NULL;
      best_previous_position = previous_position;
      best_current_position = current_position;
      best_type_index = type_index;
    }
  }

  type_index = 0;
  for (const SLaser *laser = (const SLaser *)world->m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; laser;
       laser = (const SLaser *)laser->m_Base.m_pNextTypeEntity, ++type_index) {
    float start_x, start_y, end_x, end_y;
    world_to_screen(gfx, vgetx(laser->m_From) / 32.0f, vgety(laser->m_From) / 32.0f, &start_x, &start_y);
    world_to_screen(gfx, vgetx(laser->m_Base.m_Pos) / 32.0f, vgety(laser->m_Base.m_Pos) / 32.0f, &end_x, &end_y);
    const float distance_squared = distance_squared_to_segment(mouse_x, mouse_y, start_x, start_y, end_x, end_y);
    if (distance_squared <= 12.0f * 12.0f && distance_squared <= best_distance_squared) {
      best_distance_squared = distance_squared;
      best_kind = ENTITY_INSPECTOR_LASER;
      best_projectile = NULL;
      best_laser = laser;
      best_previous_position = laser->m_From;
      best_current_position = laser->m_Base.m_Pos;
      best_type_index = type_index;
    }
  }

  if (best_kind == ENTITY_INSPECTOR_NONE) return false;

  entity_inspector_clear(inspector);
  inspector->valid = true;
  inspector->show = true;
  inspector->kind = best_kind;
  inspector->timeline_tick = timeline_tick;
  inspector->world_tick = world->m_GameTick;
  inspector->type_index = best_type_index;
  inspector->previous_position = best_previous_position;
  inspector->current_position = best_current_position;

  if (best_kind == ENTITY_INSPECTOR_PROJECTILE) {
    inspector->projectile = snapshot_projectile(best_projectile);
    if (best_projectile->m_pTuning) {
      inspector->has_tuning = true;
      inspector->tuning = *best_projectile->m_pTuning;
    }
  } else {
    inspector->laser = snapshot_laser(best_laser);
    if (best_laser->m_pTuning) {
      inspector->has_tuning = true;
      inspector->tuning = *best_laser->m_pTuning;
    }
  }
  return true;
}

static void render_base_properties(const entity_base_snapshot_t *base) {
  igText("m_pWorld: 0x%llx", (unsigned long long)base->world);
  igText("m_pCollision: 0x%llx", (unsigned long long)base->collision);
  igText("m_pPrevTypeEntity: 0x%llx", (unsigned long long)base->prev_type_entity);
  igText("m_pNextTypeEntity: 0x%llx", (unsigned long long)base->next_type_entity);
  igText("m_Pos: (%.9g, %.9g)", (double)vgetx(base->pos), (double)vgety(base->pos));
  igText("m_ObjType: %d", base->obj_type);
  igText("m_Number: %d", base->number);
  igText("m_Layer: %d", base->layer);
  igText("m_MarkedForDestroy: %s", bool_name(base->marked_for_destroy));
  igText("m_Spawned: %s", bool_name(base->spawned));
}

static void render_tuning_properties(const STuningParams *tuning) {
#define MACRO_TUNING_PARAM(Name, Value) igText("m_" #Name ": %.9g", (double)tuning->m_##Name);
#include <ddnet_physics/tuning.h>
#undef MACRO_TUNING_PARAM
}

void entity_inspector_render(entity_inspector_t *inspector) {
  if (!inspector || !inspector->valid || !inspector->show) return;

  igSetNextWindowSize((ImVec2){460.0f, 620.0f}, ImGuiCond_FirstUseEver);
  if (igBegin("Entity Inspector", &inspector->show, ImGuiWindowFlags_None)) {
    igText("%s #%d", kind_name(inspector->kind), inspector->type_index);
    igText("Timeline tick: %d", inspector->timeline_tick);
    igText("World tick: %d", inspector->world_tick);
    igText("Previous position: (%.9g, %.9g)", (double)vgetx(inspector->previous_position), (double)vgety(inspector->previous_position));
    igText("Current position: (%.9g, %.9g)", (double)vgetx(inspector->current_position), (double)vgety(inspector->current_position));

    if (igButton("Clear Selection", (ImVec2){0.0f, 0.0f})) {
      entity_inspector_clear(inspector);
      igEnd();
      return;
    }

    if (inspector->kind == ENTITY_INSPECTOR_PROJECTILE) {
      const projectile_snapshot_t *projectile = &inspector->projectile;
      const float direction_x = vgetx(projectile->direction);
      const float direction_y = vgety(projectile->direction);
      igSeparator();
      igText("Projectile type: %s (%d)", weapon_name(projectile->type), projectile->type);
      igText("Trajectory age: %d ticks", inspector->world_tick - projectile->start_tick);
      igText("Direction magnitude: %.9g", (double)sqrtf(direction_x * direction_x + direction_y * direction_y));

      if (igCollapsingHeader_TreeNodeFlags("Base Entity", ImGuiTreeNodeFlags_DefaultOpen)) render_base_properties(&projectile->base);

      if (igCollapsingHeader_TreeNodeFlags("Projectile Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        igText("m_Direction: (%.9g, %.9g)", (double)direction_x, (double)direction_y);
        igText("m_pTuning: 0x%llx", (unsigned long long)projectile->tuning);
        igText("m_LifeSpan: %d", projectile->life_span);
        igText("m_Owner: %d", projectile->owner);
        igText("m_OwnerSpawnGeneration: %u", projectile->owner_spawn_generation);
        igText("m_Type: %d (%s)", projectile->type, weapon_name(projectile->type));
        igText("m_StartTick: %d", projectile->start_tick);
        igText("m_Bouncing: %d", projectile->bouncing);
        igText("m_Explosive: %s", bool_name(projectile->explosive));
        igText("m_Freeze: %s", bool_name(projectile->freeze));
        igText("m_IsSolo: %s", bool_name(projectile->is_solo));
      }
    } else if (inspector->kind == ENTITY_INSPECTOR_LASER) {
      const laser_snapshot_t *laser = &inspector->laser;
      const float segment_x = vgetx(laser->base.pos) - vgetx(laser->from);
      const float segment_y = vgety(laser->base.pos) - vgety(laser->from);
      igSeparator();
      igText("Laser type: %s (%d)", weapon_name(laser->type), laser->type);
      igText("Segment length: %.9g", (double)sqrtf(segment_x * segment_x + segment_y * segment_y));

      if (igCollapsingHeader_TreeNodeFlags("Base Entity", ImGuiTreeNodeFlags_DefaultOpen)) render_base_properties(&laser->base);

      if (igCollapsingHeader_TreeNodeFlags("Laser Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        igText("m_pTuning: 0x%llx", (unsigned long long)laser->tuning);
        igText("m_From: (%.9g, %.9g)", (double)vgetx(laser->from), (double)vgety(laser->from));
        igText("m_Dir: (%.9g, %.9g)", (double)vgetx(laser->dir), (double)vgety(laser->dir));
        igText("m_TelePos: (%.9g, %.9g)", (double)vgetx(laser->tele_pos), (double)vgety(laser->tele_pos));
        igText("m_PrevPos: (%.9g, %.9g)", (double)vgetx(laser->prev_pos), (double)vgety(laser->prev_pos));
        igText("m_WasTele: %s", bool_name(laser->was_tele));
        igText("m_Energy: %.9g", (double)laser->energy);
        igText("m_Bounces: %d", laser->bounces);
        igText("m_EvalTick: %d", laser->eval_tick);
        igText("m_Owner: %d", laser->owner);
        igText("m_ZeroEnergyBounceInLastTick: %s", bool_name(laser->zero_energy_bounce_in_last_tick));
        igText("m_Type: %d (%s)", laser->type, weapon_name(laser->type));
        igText("m_TeleportCancelled: %s", bool_name(laser->teleport_cancelled));
        igText("m_IsBlueTeleport: %s", bool_name(laser->is_blue_teleport));
      }
    }

    if (inspector->has_tuning && igCollapsingHeader_TreeNodeFlags("Tuning Properties", 0)) render_tuning_properties(&inspector->tuning);
  }
  igEnd();
}

void entity_inspector_render_highlight(const entity_inspector_t *inspector, gfx_handler_t *gfx) {
  if (!inspector || !inspector->valid || !inspector->show || !gfx) return;

  vec2 previous = {vgetx(inspector->previous_position) / 32.0f, vgety(inspector->previous_position) / 32.0f};
  vec2 current = {vgetx(inspector->current_position) / 32.0f, vgety(inspector->current_position) / 32.0f};
  renderer_submit_line(gfx, Z_LAYER_CURSOR, previous, current, (vec4){1.0f, 0.85f, 0.1f, 0.95f}, 2.0f / 32.0f);
  renderer_submit_circle_filled(gfx, Z_LAYER_CURSOR, current, 22.0f / 32.0f, (vec4){1.0f, 0.85f, 0.1f, 0.28f}, 20);
}
