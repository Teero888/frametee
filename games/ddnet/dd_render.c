// How DDNet looks. Tees, hooks, weapons, projectiles, lasers, pickups, flags
// and trajectory lines.
//
// This used to live in the engine's user_interface.c, which meant the editor
// itself knew what a tee was. It now runs entirely inside the game module and
// reaches the screen through ft_engine_api, so the engine draws what it is
// handed without knowing any of it.

#include "dd_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float lint2(float a, float b, float f) { return a + f * (b - a); }

static void lerp2(const vec2 a, const vec2 b, float f, vec2 out) {
  out[0] = lint2(a[0], b[0], f);
  out[1] = lint2(a[1], b[1], f);
}

static void tile_pos(mvec2 v, vec2 out) {
  out[0] = vgetx(v) / PX_PER_TILE;
  out[1] = vgety(v) / PX_PER_TILE;
}

static void render_cursor(ft_game *game, const ft_render_frame *frame);

// Scratch big enough for any input record the engine will hand back.
typedef struct {
  uint8_t bytes[64];
} input_record_bytes_t;

static const ft_player_setup *setup_for(const ft_render_frame *frame, int index) {
  if (index < 0 || (uint32_t)index >= frame->player_setup_count) return NULL;
  return &frame->player_setups[index];
}

// --- tee parts ---------------------------------------------------------------

static void submit_tee_hand(ft_game *game, const vec2 center_phys, const vec2 dir, float angle_offset, float off_x, float off_y,
                            int skin, const vec3 col_body, bool custom, bool hook_hand) {
  vec2 dir_y = {-dir[1], dir[0]};
  if (dir[0] < 0.0f) {
    dir_y[0] = -dir_y[0];
    dir_y[1] = -dir_y[1];
  }
  vec2 hand = {(center_phys[0] + dir[0] * (1.0f + off_x) + dir_y[0] * off_y) / PX_PER_TILE,
               (center_phys[1] + dir[1] * (1.0f + off_x) + dir_y[1] * off_y) / PX_PER_TILE};

  // Mirrored into the renderer's angle convention, the same way aim_angle is.
  const float sign = dir[0] < 0.0f ? -1.0f : 1.0f;
  const float render_angle = atan2f(-dir[1], dir[0]) - sign * angle_offset;
  dd_hand_push(game, hand, 10.0f / PX_PER_TILE, skin, render_angle, (float *)col_body, custom, hook_hand);
}

static void render_fastcap_flag(ft_game *game, int team, vec2 pos) {
  const uint32_t sprite = team == 0 ? GAMESKIN_FLAG_RED : GAMESKIN_FLAG_BLUE;
  const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, sprite);
  if (!rect) return;
  const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
  vec2 size = {8.0f * ((float)rect->w / f), 8.0f * ((float)rect->h / f)};
  vec2 draw_pos = {pos[0], pos[1] - size[1] * 0.5f};
  dd_draw_sprite(game, game->gfx.gameskin, DD_Z_PICKUPS, draw_pos, size, 0.f, sprite, (vec4){1.f, 1.f, 1.f, 1.f});
}

static void render_fastcap_flags(ft_game *game, const SWorldCore *world, float intra) {
  if (!world->m_pConfig->m_SvFastcap) return;

  const SCollision *collision = world->m_pCollision;
  // FastCap progress is encoded per character: no flags means unstarted,
  // exactly one means carrying, and both means finished.
  bool flag_at_stand[2] = {collision->m_aFastcapFlagPresent[0], collision->m_aFastcapFlagPresent[1]};
  for (int i = 0; i < world->m_NumCharacters; ++i) {
    for (int team = 0; team < 2; ++team) {
      if (world->m_pCharacters[i].m_aGotFastcapFlag[team]) flag_at_stand[team] = false;
    }
  }

  for (int team = 0; team < 2; ++team) {
    if (!flag_at_stand[team]) continue;
    vec2 pos;
    tile_pos(collision->m_aFastcapFlagPositions[team], pos);
    render_fastcap_flag(game, team, pos);
  }

  for (int i = 0; i < world->m_NumCharacters; ++i) {
    const SCharacterCore *character = &world->m_pCharacters[i];
    if (character->m_FinishTick >= 0) continue;
    const int held = character->m_aGotFastcapFlag[0] + character->m_aGotFastcapFlag[1];
    if (held != 1) continue;

    for (int team = 0; team < 2; ++team) {
      if (!collision->m_aFastcapFlagPresent[team] || !character->m_aGotFastcapFlag[team]) continue;
      vec2 prev, cur, pos;
      tile_pos(character->m_PrevPos, prev);
      tile_pos(character->m_Pos, cur);
      lerp2(prev, cur, intra, pos);
      render_fastcap_flag(game, team, pos);
    }
  }
}

// --- one tee -----------------------------------------------------------------

typedef struct {
  int skin;
  vec3 body_col;
  vec3 feet_col;
  bool custom;
  vec2 dir;
  dd_anim_state_t anim;
  bool in_air;
  bool stationary;
  bool inactive;
  float attack_ticks_passed;
} tee_visual_t;

static void build_tee_visual(ft_game *game, const ft_render_frame *frame, const SWorldCore *world, const SWorldCore *prev_world, int index,
                             float intra, const vec2 pos, tee_visual_t *out) {
  const SCharacterCore *core = &world->m_pCharacters[index];

  dd_anim_state_set(&out->anim, &anim_base, 0.0f);

  out->stationary = fabsf(vgetx(core->m_Vel) * 256.f) <= 1;
  const bool running = fabsf(vgetx(core->m_Vel) * 256.f) >= 5000;
  const bool want_other_dir =
      (core->m_Input.m_Direction == -1 && vgetx(core->m_Vel) > 0) || (core->m_Input.m_Direction == 1 && vgetx(core->m_Vel) < 0);
  out->inactive = get_flag_sit(&core->m_Input);
  out->in_air = !(core->m_pCollision->m_pTileInfos[core->m_BlockIdx] & INFO_CANGROUND) ||
                !(check_point(core->m_pCollision, vec2_init(vgetx(core->m_Pos), vgety(core->m_Pos) + 16)));
  out->attack_ticks_passed = (world->m_GameTick - core->m_AttackTick) + intra;
  const float last_attack_time = out->attack_ticks_passed / (float)GAME_TICK_SPEED;

  float walk_time = fmodf(pos[0] * PX_PER_TILE, 100.0f) / 100.0f;
  float run_time = fmodf(pos[0] * PX_PER_TILE, 200.0f) / 200.0f;
  if (walk_time < 0.0f) walk_time += 1.0f;
  if (run_time < 0.0f) run_time += 1.0f;

  if (out->in_air) dd_anim_state_add(&out->anim, &anim_inair, 0.0f, 1.0f);
  else if (out->stationary) {
    if (out->inactive) dd_anim_state_add(&out->anim, core->m_Input.m_Direction < 0 ? &anim_sit_left : &anim_sit_right, 0.0f, 1.0f);
    else dd_anim_state_add(&out->anim, &anim_idle, 0.0f, 1.0f);
  } else if (!want_other_dir) {
    if (running) dd_anim_state_add(&out->anim, vgetx(core->m_Vel) < 0.0f ? &anim_run_left : &anim_run_right, run_time, 1.0f);
    else dd_anim_state_add(&out->anim, &anim_walk, walk_time, 1.0f);
  }
  if (core->m_ActiveWeapon == WEAPON_HAMMER) dd_anim_state_add(&out->anim, &anim_hammer_swing, last_attack_time * 5.f, 1.0f);
  if (core->m_ActiveWeapon == WEAPON_NINJA) dd_anim_state_add(&out->anim, &anim_ninja_swing, last_attack_time * 2.f, 1.0f);

  const SPlayerInput *prev_input = &prev_world->m_pCharacters[index].m_Input;
  out->dir[0] = lint2((float)prev_input->m_TargetX, (float)core->m_Input.m_TargetX, intra);
  out->dir[1] = lint2((float)prev_input->m_TargetY, (float)core->m_Input.m_TargetY, intra);
  glm_vec2_normalize(out->dir);

  const ft_player_setup *setup = setup_for(frame, index);
  out->skin = setup && setup->appearance_id ? dd_gfx_skin_index(game, setup->appearance_id) : game->gfx.default_skin;
  out->custom = setup ? setup->use_custom_color : false;
  glm_vec3_copy((vec3){1.f, 1.f, 1.f}, out->feet_col);
  glm_vec3_copy((vec3){0.f, 0.f, 0.f}, out->body_col);

  if (out->custom && setup) {
    out->body_col[0] = setup->primary_color.r;
    out->body_col[1] = setup->primary_color.g;
    out->body_col[2] = setup->primary_color.b;
    out->feet_col[0] = setup->secondary_color.r;
    out->feet_col[1] = setup->secondary_color.g;
    out->feet_col[2] = setup->secondary_color.b;
  }

  if (core->m_FreezeTime > 0 || core->m_ActiveWeapon == WEAPON_NINJA) {
    out->skin = game->gfx.ninja_skin;
    out->custom = false;
  }
  // Jumping off the last available jump dims the feet, which is DDNet's tell
  // that there is nothing left.
  if (core->m_JumpedTotal >= core->m_Jumps - 1) {
    if (out->custom) {
      out->feet_col[0] *= 0.5f;
      out->feet_col[1] *= 0.5f;
      out->feet_col[2] *= 0.5f;
    } else {
      out->feet_col[0] = 0.5f;
    }
  }
}

static int tee_eye_state(const SWorldCore *world, const SCharacterCore *core) {
  int eye = get_flag_eye_state(&core->m_Input);
  if (core->m_FreezeTime > 0 && eye == 0) eye = EYE_BLINK;
  const int damage_age = world->m_GameTick - core->m_DamageTick;
  if (damage_age >= 0 && damage_age < GAME_TICK_SPEED / 2) eye = EYE_PAIN;
  return eye;
}

static void render_weapon(ft_game *game, const SWorldCore *world, const SCharacterCore *core, const SCharacterCore *prev_core,
                          const tee_visual_t *tee, float intra, const vec2 pos) {
  if (core->m_FreezeTime || core->m_ActiveWeapon >= NUM_WEAPONS) return;
  const dd_weapon_spec_t *spec = &dd_game_data.weapons.id[core->m_ActiveWeapon];
  const float aim_angle = atan2f(-tee->dir[1], tee->dir[0]);
  const bool is_sit = tee->inactive && !tee->in_air && tee->stationary;
  const float flip_factor = (tee->dir[0] < 0.0f) ? -1.0f : 1.0f;

  vec2 phys_prev = {vgetx(core->m_PrevPos), vgety(core->m_PrevPos)};
  vec2 phys_curr = {vgetx(core->m_Pos), vgety(core->m_Pos)};
  vec2 phys_pos;
  lerp2(phys_prev, phys_curr, intra, phys_pos);

  vec2 weapon_pos;
  glm_vec2_copy(phys_pos, weapon_pos);

  float anim_attach_angle_rad = tee->anim.attach.angle * (2.0f * M_PI);
  float weapon_angle = anim_attach_angle_rad + aim_angle;
  int weapon_sprite_id = -1;

  if (core->m_ActiveWeapon == WEAPON_HAMMER) {
    weapon_sprite_id = GAMESKIN_HAMMER_BODY;
    weapon_pos[0] += tee->anim.attach.x;
    weapon_pos[1] += tee->anim.attach.y;
    weapon_pos[1] += spec->offsety;
    if (tee->dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;
    if (is_sit) weapon_pos[1] += 3.0f;

    if (!tee->inactive) {
      weapon_angle = M_PI / 2.0f - flip_factor * anim_attach_angle_rad;
    } else {
      weapon_angle = tee->dir[0] < 0.0 ? 100.f : 500.f;
    }
  } else if (core->m_ActiveWeapon == WEAPON_NINJA) {
    weapon_sprite_id = GAMESKIN_NINJA_BODY;
    weapon_pos[1] += spec->offsety;
    if (is_sit) weapon_pos[1] += 3.0f;
    if (tee->dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;
    weapon_angle = -M_PI / 2.0f + flip_factor * anim_attach_angle_rad;

    const float attack_time_sec = tee->attack_ticks_passed / (float)GAME_TICK_SPEED;
    if (attack_time_sec <= 1.0f / 6.0f && spec->num_muzzles > 0) {
      const int muzzle_idx = world->m_GameTick % spec->num_muzzles;
      vec2 hadoken_dir = {vgetx(core->m_Pos) - vgetx(prev_core->m_Pos), vgety(core->m_Pos) - vgety(prev_core->m_Pos)};
      if (glm_vec2_norm2(hadoken_dir) < 0.0001f) {
        hadoken_dir[0] = 1.0f;
        hadoken_dir[1] = 0.0f;
      }
      glm_vec2_normalize(hadoken_dir);
      const float hadoken_angle = atan2f(-hadoken_dir[1], hadoken_dir[0]);

      vec2 muzzle_phys;
      glm_vec2_copy(phys_pos, muzzle_phys);
      muzzle_phys[0] -= hadoken_dir[0] * spec->muzzleoffsetx;
      muzzle_phys[1] -= hadoken_dir[1] * spec->muzzleoffsetx;

      const uint32_t muzzle_sprite = GAMESKIN_NINJA_MUZZLE1 + muzzle_idx;
      const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, muzzle_sprite);
      if (rect) {
        const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
        vec2 muzzle_size = {160.0f * ((float)rect->w / f) / PX_PER_TILE, 160.0f * ((float)rect->h / f) / PX_PER_TILE};
        vec2 render_pos = {muzzle_phys[0] / PX_PER_TILE, muzzle_phys[1] / PX_PER_TILE};
        dd_draw_sprite(game, game->gfx.gameskin, DD_Z_WEAPONS, render_pos, muzzle_size, hadoken_angle, muzzle_sprite,
                       (vec4){1.f, 1.f, 1.f, 1.f});
      }
    }
  } else {
    switch (core->m_ActiveWeapon) {
    case WEAPON_GUN: weapon_sprite_id = GAMESKIN_GUN_BODY; break;
    case WEAPON_SHOTGUN: weapon_sprite_id = GAMESKIN_SHOTGUN_BODY; break;
    case WEAPON_GRENADE: weapon_sprite_id = GAMESKIN_GRENADE_BODY; break;
    case WEAPON_LASER: weapon_sprite_id = GAMESKIN_LASER_BODY; break;
    default: break;
    }

    float recoil = 0.0f;
    const float a = tee->attack_ticks_passed / 5.0f;
    if (a < 1.0f) recoil = sinf(a * M_PI);

    weapon_pos[0] += tee->dir[0] * (spec->offsetx - recoil * 10.0f);
    weapon_pos[1] += tee->dir[1] * (spec->offsetx - recoil * 10.0f);
    weapon_pos[1] += spec->offsety;
    if (is_sit) weapon_pos[1] += 3.0f;

    if ((core->m_ActiveWeapon == WEAPON_GUN || core->m_ActiveWeapon == WEAPON_SHOTGUN) && spec->num_muzzles > 0) {
      if (tee->attack_ticks_passed > 0 && tee->attack_ticks_passed < spec->muzzleduration + 3.0f) {
        const int muzzle_idx = world->m_GameTick % spec->num_muzzles;
        vec2 muzzle_dir_y = {-tee->dir[1], tee->dir[0]};
        const float offset_y = -spec->muzzleoffsety * flip_factor;

        vec2 muzzle_phys;
        glm_vec2_copy(weapon_pos, muzzle_phys);
        muzzle_phys[0] += tee->dir[0] * spec->muzzleoffsetx + muzzle_dir_y[0] * offset_y;
        muzzle_phys[1] += tee->dir[1] * spec->muzzleoffsetx + muzzle_dir_y[1] * offset_y;

        const uint32_t muzzle_sprite = (core->m_ActiveWeapon == WEAPON_GUN ? GAMESKIN_GUN_MUZZLE1 : GAMESKIN_SHOTGUN_MUZZLE1) + muzzle_idx;
        const float w = 96.0f, h = 64.0f;
        const float f = sqrtf(w * w + h * h);
        vec2 muzzle_size = {spec->visual_size * (w / f) * (4.0f / 3.0f) / PX_PER_TILE, spec->visual_size * (h / f) / PX_PER_TILE};
        muzzle_size[1] *= flip_factor;

        vec2 render_pos = {muzzle_phys[0] / PX_PER_TILE, muzzle_phys[1] / PX_PER_TILE};
        dd_draw_sprite(game, game->gfx.gameskin, DD_Z_WEAPONS, render_pos, muzzle_size, weapon_angle, muzzle_sprite,
                       (vec4){1.f, 1.f, 1.f, 1.f});
      }
    }
  }

  if (weapon_sprite_id != -1) {
    const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, (uint32_t)weapon_sprite_id);
    if (rect) {
      const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
      vec2 weapon_size = {spec->visual_size * ((float)rect->w / f) / PX_PER_TILE, spec->visual_size * ((float)rect->h / f) / PX_PER_TILE};
      weapon_size[1] *= flip_factor;
      vec2 render_pos = {weapon_pos[0] / PX_PER_TILE, weapon_pos[1] / PX_PER_TILE};
      dd_draw_sprite(game, game->gfx.gameskin, DD_Z_WEAPONS, render_pos, weapon_size, weapon_angle, (uint32_t)weapon_sprite_id,
                     (vec4){1.f, 1.f, 1.f, 1.f});
    }
  }
  (void)pos;

  // Only these three are held with a visible hand in DDNet.
  switch (core->m_ActiveWeapon) {
  case WEAPON_GUN: submit_tee_hand(game, weapon_pos, tee->dir, -3.0f * M_PI / 4.0f, -15.0f, 4.0f, tee->skin, tee->body_col, tee->custom, false); break;
  case WEAPON_SHOTGUN: submit_tee_hand(game, weapon_pos, tee->dir, -M_PI / 2.0f, -5.0f, 4.0f, tee->skin, tee->body_col, tee->custom, false); break;
  case WEAPON_GRENADE: submit_tee_hand(game, weapon_pos, tee->dir, -M_PI / 2.0f, -4.0f, 7.0f, tee->skin, tee->body_col, tee->custom, false); break;
  default: break;
  }
}

static void render_hook(ft_game *game, const SWorldCore *world, const SCharacterCore *core, const SCharacterCore *prev_core,
                        const tee_visual_t *tee, float intra, const vec2 pos) {
  if (core->m_HookState < 1 || (prev_core->m_HookState == HOOK_IDLE && intra <= 0.25)) return;

  vec2 hook_pos;
  {
    vec2 from, to;
    tile_pos(prev_core->m_HookPos, from);
    tile_pos(core->m_HookPos, to);
    if (core->m_HookedPlayer != -1 && core->m_HookedPlayer < world->m_NumCharacters) {
      const SCharacterCore *hooked = &world->m_pCharacters[core->m_HookedPlayer];
      tile_pos(hooked->m_PrevPos, from);
      tile_pos(hooked->m_Pos, to);
    }
    lerp2(from, to, intra, hook_pos);
  }

  vec2 direction;
  glm_vec2_sub(hook_pos, (float *)pos, direction);
  const float length = glm_vec2_norm(direction);
  glm_vec2_normalize(direction);
  const float angle = atan2f(-direction[1], direction[0]);

  if (length > 0) {
    vec2 center_pos = {pos[0] + direction[0] * (length - 0.5f) * 0.5f, pos[1] + direction[1] * (length - 0.5f) * 0.5f};
    vec2 chain_size = {-length + 0.5f, 0.5f};
    // The chain repeats its sprite along its length. DDNet stretches it by 1.5,
    // which is what the old renderer's tile_uv flag computed.
    const ft_sprite_draw draw = {.pos = {center_pos[0], center_pos[1]},
                                 .size = {chain_size[0], chain_size[1]},
                                 .rotation = angle,
                                 .sprite_index = GAMESKIN_HOOK_CHAIN,
                                 .color = {1.f, 1.f, 1.f, 1.f},
                                 .tiling = {chain_size[0] * 1.5f, 1.f}};
    dd_draw_sprites(game, game->gfx.gameskin, DD_Z_HOOK, &draw, 1);
  }

  const ft_sprite_rect *head = dd_sprite_rect(game, game->gfx.gameskin, GAMESKIN_HOOK_HEAD);
  if (head) {
    vec2 head_size = {(float)head->w / 64.0f, (float)head->h / 64.0f};
    dd_draw_sprite(game, game->gfx.gameskin, DD_Z_HOOK, hook_pos, head_size, angle, GAMESKIN_HOOK_HEAD, (vec4){1.f, 1.f, 1.f, 1.f});
  }

  vec2 hook_center = {pos[0] * PX_PER_TILE, pos[1] * PX_PER_TILE};
  submit_tee_hand(game, hook_center, direction, -M_PI / 2.0f, 20.0f, 0.0f, tee->skin, tee->body_col, tee->custom, true);
}

// --- entities ----------------------------------------------------------------

static void render_projectiles_and_lasers(ft_game *game, const SWorldCore *world, float intra) {
  int id = 0;
  for (SProjectile *ent = (SProjectile *)world->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE]; ent;
       ent = (SProjectile *)ent->m_Base.m_pNextTypeEntity) {
    const float pt = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick - 1) / (float)GAME_TICK_SPEED;
    const float ct = (ent->m_Base.m_pWorld->m_GameTick - ent->m_StartTick) / (float)GAME_TICK_SPEED;
    vec2 from, to, p;
    tile_pos(prj_get_pos(ent, pt), from);
    tile_pos(prj_get_pos(ent, ct), to);
    lerp2(from, to, intra, p);

    dd_draw_sprite(game, game->gfx.gameskin, DD_Z_PROJECTILES, p, (vec2){1.f, 1.f},
                   -((world->m_GameTick + intra) / 50.f) * 4.f * M_PI + id, GAMESKIN_GRENADE_PROJ, (vec4){1.f, 1.f, 1.f, 1.f});
    ++id;
  }

  for (SLaser *ent = (SLaser *)world->m_apFirstEntityTypes[WORLD_ENTTYPE_LASER]; ent; ent = (SLaser *)ent->m_Base.m_pNextTypeEntity) {
    vec2 p1, p0;
    tile_pos(ent->m_Base.m_Pos, p1);
    tile_pos(ent->m_From, p0);

    vec4 laser_col = {0.f, 0.f, 1.f, 0.9f};
    vec4 shotgun_col = {0.570315f, 0.4140625f, 0.25f, 0.9f};
    float *col = ent->m_Type == WEAPON_LASER ? laser_col : shotgun_col;
    dd_draw_line(game, DD_Z_LINES, p0, p1, col, 0.25f);
    dd_draw_circle(game, DD_Z_LINES, p0, 0.2f, col, 8);
  }
}

static void render_pickups(ft_game *game, const ft_render_frame *frame, const SWorldCore *world, float intra) {
  const ft_level *level = frame->level;
  if (!game->settings.render_pickups || !level || level->num_pickups <= 0) return;

  const bool unique_race = world->m_UniqueRace;
  const int selected = frame->selected_player;

  ft_sprite_draw *draws = malloc(sizeof(ft_sprite_draw) * (size_t)level->num_pickups);
  if (!draws) return;
  uint32_t count = 0;

  const float animation_time = intra + (float)world->m_GameTick;

  ft_camera camera;
  game->engine->camera_get(&camera);
  const float margin = 4.0f;
  const float min_x = camera.visible.x - margin;
  const float max_x = camera.visible.x + camera.visible.w + margin;
  const float min_y = camera.visible.y - margin;
  const float max_y = camera.visible.y + camera.visible.h + margin;

  for (int i = 0; i < level->num_pickups; ++i) {
    vec2 pos;
    tile_pos(level->pickup_positions[i], pos);
    if (pos[0] < min_x || pos[0] > max_x || pos[1] < min_y || pos[1] > max_y) continue;

    const SPickup pickup = level->pickups[i];
    vec2 size = {1.0f, 1.0f};
    int idx = -1;

    // Unique Race hides everything but grenades, and health/ammo servers hide
    // pickups the viewed tee is still on cooldown for.
    if (unique_race && ((pickup.m_Type == POWERUP_WEAPON && pickup.m_Subtype != WEAPON_GRENADE) || pickup.m_Type == POWERUP_NINJA)) continue;
    if (selected >= 0 && world->m_pConfig->m_SvHealthAndAmmo && world->m_pPickupCooldowns) {
      const SPickupCooldownList *cooldowns = &world->m_pPickupCooldowns[selected];
      bool on_cooldown = false;
      for (int c = 0; c < cooldowns->m_NumEntries; ++c) {
        if (cooldowns->m_pEntries[c].m_Key == level->pickup_cooldown_keys[i] && world->m_GameTick <= cooldowns->m_pEntries[c].m_EndTick) {
          on_cooldown = true;
          break;
        }
      }
      if (on_cooldown) continue;
    }

    if (pickup.m_Type == POWERUP_HEALTH || pickup.m_Type == POWERUP_ARMOR) idx = GAMESKIN_PICKUP_HEALTH + pickup.m_Type;
    else if (pickup.m_Type >= POWERUP_ARMOR_SHOTGUN) idx = GAMESKIN_PICKUP_ARMOR_SHOTGUN + pickup.m_Type - POWERUP_ARMOR_SHOTGUN;
    else if (pickup.m_Type == POWERUP_WEAPON && pickup.m_Subtype < NUM_WEAPONS) idx = GAMESKIN_PICKUP_HAMMER + pickup.m_Subtype;
    else if (pickup.m_Type == POWERUP_NINJA) idx = GAMESKIN_PICKUP_NINJA;
    if (idx < 0) continue;

    const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, (uint32_t)idx);
    if (rect && rect->w > 0 && rect->h > 0) {
      const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
      const float scale_x = (float)rect->w / f;
      const float scale_y = (float)rect->h / f;

      if (pickup.m_Type == POWERUP_HEALTH || pickup.m_Type == POWERUP_ARMOR || pickup.m_Type >= POWERUP_ARMOR_SHOTGUN) {
        size[0] = 1.f / scale_x;
        size[1] = 1.f / scale_y;
      } else if (pickup.m_Type == POWERUP_WEAPON) {
        const dd_weapon_spec_t *spec = &dd_game_data.weapons.id[pickup.m_Subtype];
        size[0] = spec->visual_size * scale_x / PX_PER_TILE;
        size[1] = spec->visual_size * scale_y / PX_PER_TILE;
      } else if (pickup.m_Type == POWERUP_NINJA) {
        size[0] = 4.f * scale_x;
        size[1] = 4.f * scale_y;
        pos[0] -= 10.f / PX_PER_TILE;
      }
    }

    // Pickups bob on a per-position phase so a row of them does not pulse in unison.
    const float offset = pos[1] + pos[0];
    pos[0] += (cosf((animation_time / GAME_TICK_SPEED) * 2.0f + offset) * 2.5f) / PX_PER_TILE;
    pos[1] += (sinf((animation_time / GAME_TICK_SPEED) * 2.0f + offset) * 2.5f) / PX_PER_TILE;

    draws[count++] = (ft_sprite_draw){.pos = {pos[0], pos[1]},
                                      .size = {size[0], size[1]},
                                      .rotation = 0.f,
                                      .sprite_index = (uint32_t)idx,
                                      .color = {1.f, 1.f, 1.f, 1.f},
                                      .tiling = {1.f, 1.f}};
  }

  if (count > 0) dd_draw_sprites(game, game->gfx.gameskin, DD_Z_PICKUPS, draws, count);
  free(draws);
}

// --- entry point -------------------------------------------------------------

static void render_entities(ft_game *game, const ft_render_frame *frame) {
  const ft_world *world_handle = frame->world;
  const ft_world *prev_handle = frame->previous_world ? frame->previous_world : frame->world;
  if (!world_handle) return;

  const SWorldCore *world = &world_handle->core;
  const SWorldCore *prev_world = &prev_handle->core;
  if (world->m_NumCharacters != prev_world->m_NumCharacters) prev_world = world;

  const float intra = frame->alpha;
  const int selected = frame->selected_player;

  render_pickups(game, frame, world, intra);

  if (game->settings.render_players) {
    dd_skins_begin(game);

    // Cull to the viewport with a margin wide enough that a tee entering the
    // screen is already drawn by the time any part of it is visible.
    ft_camera camera;
    game->engine->camera_get(&camera);
    const float margin = 6.0f;
    const float min_x = camera.visible.x - margin;
    const float max_x = camera.visible.x + camera.visible.w + margin;
    const float min_y = camera.visible.y - margin;
    const float max_y = camera.visible.y + camera.visible.h + margin;

    for (int i = 0; i < world->m_NumCharacters; ++i) {
      const SCharacterCore *core = &world->m_pCharacters[i];
      const SCharacterCore *prev_core = &prev_world->m_pCharacters[i];

      vec2 from, to, p;
      tile_pos(core->m_PrevPos, from);
      tile_pos(core->m_Pos, to);
      lerp2(from, to, intra, p);

      if (p[0] < min_x || p[0] > max_x || p[1] < min_y || p[1] > max_y) {
        if (!(frame->state.recording && i == selected)) continue;
      }

      tee_visual_t tee;
      build_tee_visual(game, frame, world, prev_world, i, intra, p, &tee);
      const int eye = tee_eye_state(world, core);

      dd_skin_push(game, p, 1.0f, tee.skin, eye, tee.dir, &tee.anim, tee.body_col, tee.feet_col, tee.custom);

      if (!frame->state.recording && i == selected) {
        // Marker triangle floating above the selected tee, pointing down at it.
        const float width = 1.0f, height = 0.8f, gap = 0.35f;
        vec4 marker = {frame->accent.r, frame->accent.g, frame->accent.b, 0.5f};
        vec2 tip = {p[0], p[1] - 1.0f - gap};
        vec2 left = {p[0] - width * 0.5f, tip[1] - height};
        vec2 right = {p[0] + width * 0.5f, tip[1] - height};
        dd_draw_triangle(game, DD_Z_LINES, tip, left, right, marker);
      }

      if (game->settings.center_dot) {
        const map_data_t *map = &world->m_pCollision->m_MapData;
        const int idx = (int)p[1] * map->width + (int)p[0];
        bool freeze = false;
        if (idx >= 0 && idx < map->width * map->height) {
          freeze = map->game_layer.data[idx] == TILE_FREEZE;
          if (!freeze && map->front_layer.data) freeze = map->front_layer.data[idx] == TILE_FREEZE;
        }
        dd_draw_circle(game, DD_Z_LINES + 1.0f, p, 2.f / PX_PER_TILE, freeze ? (vec4){0, 0, 1, 1} : (vec4){0, 1, 0, 1}, 4);
      }

      if (game->settings.render_weapons) {
        render_hook(game, world, core, prev_core, &tee, intra, p);
        render_weapon(game, world, core, prev_core, &tee, intra, p);
      }
    }

    dd_skins_flush(game);
  }

  render_projectiles_and_lasers(game, world, intra);
  render_fastcap_flags(game, world, intra);

}

void dd_render(ft_game *game, const ft_render_frame *frame) {
  if (!game->gfx.ready || !frame) return;

  switch (frame->pass) {
  case FT_PASS_LEVEL_BACKGROUND: dd_map_render(game, frame); break;
  case FT_PASS_ENTITIES: render_entities(game, frame); break;
  case FT_PASS_OVERLAY:
    if (frame->active) render_cursor(game, frame);
    break;
  case FT_PASS_LEVEL_FOREGROUND: {
    // Particles ride above the level but below the HUD. The simulation is
    // advanced to the rendered moment first, then both layers are drawn.
    if (!game->settings.render_particles || frame->world_index < 0) break;
    dd_particles_advance(game, frame->world_index, frame->level, frame->tick, frame->alpha);
    dd_particle_system_t *ps = dd_particles_for(game, frame->world_index);
    if (ps) {
      dd_particles_render(ps, game, 0);
      dd_particles_render(ps, game, 1);
    }
    break;
  }
  default: break;
  }
}

// --- crosshair ---------------------------------------------------------------

// DDNet draws the cursor on a screen mapped at zoom 1, so its size on screen is
// independent of the editor's zoom. These two conversions reproduce that.
static float aim_units_to_pixels(const ft_camera *camera) {
  const float amount = 1150.0f * 1000.0f;
  const float w_max = 1500.0f;
  const float h_max = 1050.0f;
  if (!(camera->aspect > 0.0f) || camera->viewport.y <= 0.f) return 1.0f;

  float height = sqrtf(amount / camera->aspect);
  if (height * camera->aspect > w_max) height = w_max / camera->aspect;
  if (height > h_max) height = h_max;
  return camera->viewport.y / height;
}

static float tiles_to_pixels(const ft_camera *camera) {
  const float width = camera->visible.w;
  if (!(fabsf(width) > 1e-6f)) return 1.0f;
  return camera->viewport.x / width;
}

static void render_cursor(ft_game *game, const ft_render_frame *frame) {
  if (!game->gfx.cursor) return;
  const int selected = frame->selected_player;
  if (selected < 0 || !frame->world) return;
  if (selected >= frame->world->core.m_NumCharacters) return;

  // Only while recording, or when the camera is locked to the tee. In free view
  // the crosshair has nothing to sit against and just floats.
  const bool locked = frame->state.camera.mode == DD_CAMERA_FOLLOW && game->settings.render_cursor_follow;
  if (!frame->state.recording && !locked) return;

  const SCharacterCore *core = &frame->world->core.m_pCharacters[selected];

  // Aim interpolated between the two ticks, exactly like the tee it belongs to.
  // While recording, get_player_input hands back the live input for the tick
  // under the playhead, so the crosshair tracks the mouse directly.
  ft_vec2 aim = {(float)core->m_Input.m_TargetX, (float)core->m_Input.m_TargetY};
  {
    input_record_bytes_t previous, current;
    const bool have_prev = game->engine->get_player_input(frame->state.selected_player, frame->tick - 1, &previous);
    const bool have_cur = game->engine->get_player_input(frame->state.selected_player, frame->tick, &current);
    if (have_cur) {
      const SPlayerInput *cur = (const SPlayerInput *)&current;
      const SPlayerInput *prev = have_prev ? (const SPlayerInput *)&previous : cur;
      aim.x = lint2((float)prev->m_TargetX, (float)cur->m_TargetX, frame->alpha);
      aim.y = lint2((float)prev->m_TargetY, (float)cur->m_TargetY, frame->alpha);
    }
  }

  const uint32_t weapon = core->m_ActiveWeapon < CURSOR_SPRITE_COUNT ? core->m_ActiveWeapon : 0;
  const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.cursor, weapon);
  if (!rect) return;

  ft_camera camera;
  game->engine->camera_get(&camera);
  const float zoom1_tiles = aim_units_to_pixels(&camera) * PX_PER_TILE / tiles_to_pixels(&camera);

  const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
  const float sprite_units = 64.0f * game->settings.cursor_scale;
  vec2 size = {sprite_units * ((float)rect->w / f) / PX_PER_TILE * zoom1_tiles,
               sprite_units * ((float)rect->h / f) / PX_PER_TILE * zoom1_tiles};

  // Anchored on the drawn tee, not on its tick position: against a camera that
  // moves smoothly, the raw position makes the crosshair jitter every tick.
  vec2 from, to, tee_pos;
  tile_pos(core->m_PrevPos, from);
  tile_pos(core->m_Pos, to);
  lerp2(from, to, frame->alpha, tee_pos);

  vec2 pos = {tee_pos[0] + aim.x / PX_PER_TILE * zoom1_tiles, tee_pos[1] + aim.y / PX_PER_TILE * zoom1_tiles};
  dd_draw_sprite(game, game->gfx.cursor, DD_Z_CURSOR, pos, size, 0.f, weapon, (vec4){1.f, 1.f, 1.f, 1.f});
}

// --- camera ------------------------------------------------------------------

// DDNet's camera modes. Which modes exist and where each one points is the
// game's business; the engine only owns panning, zooming and the projection.
const ft_camera_mode dd_camera_modes[DD_CAMERA_MODE_COUNT] = {
    [DD_CAMERA_FREE] = {"free", "Free view", "Pan and zoom freely", FT_CAMERA_MODE_FREE},
    [DD_CAMERA_FOLLOW] = {"follow", "Lock to tee", "Keeps the selected tee centred", FT_CAMERA_MODE_DIRECTED},
};

bool dd_camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  (void)game;
  if (!frame || !inout || !frame->world) return false;

  // Recording always follows the tee being driven, whatever mode is selected:
  // steering something off-screen is not a thing anyone wants to do.
  const bool follow = frame->recording || frame->mode == DD_CAMERA_FOLLOW;
  if (!follow) return false;

  const int player = frame->player;
  if (player < 0 || player >= frame->world->core.m_NumCharacters) return false;

  const SCharacterCore *core = &frame->world->core.m_pCharacters[player];

  // Interpolate exactly as the tee itself is drawn. Snapping to the tick
  // position while the tee moves smoothly is what made the lock look jittery.
  vec2 from, to, pos;
  tile_pos(core->m_PrevPos, from);
  tile_pos(core->m_Pos, to);
  lerp2(from, to, frame->alpha, pos);

  inout->position = (ft_vec2){pos[0], pos[1]};
  return true;
}

// --- status readout ----------------------------------------------------------

// What the editor shows beside the viewport while scrubbing. The wording and
// precision are DDNet's, which is the point: the engine cannot know that
// velocity is worth showing in blocks per second, or that a race timer exists.
uint32_t dd_status_lines(ft_game *game, const ft_world *world, int32_t player, float alpha, char *out, uint32_t max_lines,
                         uint32_t line_size) {
  (void)game;
  if (!world || player < 0 || player >= world->core.m_NumCharacters || max_lines == 0) return 0;
  const SCharacterCore *c = &world->core.m_pCharacters[player];

  uint32_t count = 0;
#define LINE(...)                                                       \
  do {                                                                  \
    if (count >= max_lines) return count;                               \
    snprintf(out + (size_t)count * line_size, line_size, __VA_ARGS__);   \
    ++count;                                                            \
  } while (0)

  // Positions are reported relative to the map's origin, the same way DDNet's
  // own debug output does, hence the 200-tile offset.
  const int pos_x = (int)(vgetx(c->m_Pos) - 200 * 32);
  const int pos_y = (int)(vgety(c->m_Pos) - 200 * 32);
  const float vel_x = vgetx(c->m_Vel);
  const float vel_y = vgety(c->m_Vel);

  LINE("Character:");
  LINE("Pos: %d, %d; (%.4f, %.4f)", pos_x, pos_y, pos_x / 32.f, pos_y / 32.f);
  LINE("Vel: %.2f, %.2f; (%.2f, %.2f BPS)", vel_x * c->m_VelRamp, vel_y, vel_x * c->m_VelRamp * (50.f / 32.f), vel_y * (50.f / 32.f));
  LINE("Freeze: %d", c->m_FreezeTime);
  LINE("Reload: %d", c->m_ReloadTimer);
  LINE("Weapon: %d", c->m_ActiveWeapon);
  LINE("Weapons: [ %d, %d, %d, %d, %d, %d ]", c->m_aWeaponGot[0], c->m_aWeaponGot[1], c->m_aWeaponGot[2], c->m_aWeaponGot[3],
       c->m_aWeaponGot[4], c->m_aWeaponGot[5]);

  if (world->core.m_pConfig && world->core.m_pConfig->m_SvHealthAndAmmo) {
    LINE("Health: %d", c->m_Health);
    LINE("Shield: %d", c->m_Armor);
    LINE("Ammo: %d", c->m_aWeaponAmmo[c->m_ActiveWeapon]);
  }

  // Race timer, counting up until the run finishes and then holding.
  float race_time = c->m_RaceTime;
  if (race_time < 0.0f && c->m_StartTick >= 0) {
    const float now = (float)world->core.m_GameTick + alpha;
    race_time = (now - (float)c->m_StartTime - c->m_StartTickOffset) / GAME_TICK_SPEED;
    if (race_time < 0.0f) race_time = 0.0f;
  }
  if (race_time >= 0.0f) {
    const int minutes = (int)race_time / 60;
    const float seconds = fmodf(race_time, 60.0f);
    LINE(c->m_FinishTick >= 0 ? "Finish Time: %02d:%06.3f" : "Time: %02d:%06.3f", minutes, seconds);
  }
#undef LINE

  return count;
}

// The line shown beside a tee in the editor's player list: its finish time once
// it has one, otherwise how far it got.
bool dd_player_label(ft_game *game, const ft_world *world, int32_t player, char *out, size_t out_size) {
  (void)game;
  if (!world || player < 0 || player >= world->core.m_NumCharacters) return false;
  const SCharacterCore *c = &world->core.m_pCharacters[player];

  if (c->m_FinishTick > 0) {
    float time = c->m_RaceTime;
    if (time < 0.0f && c->m_StartTick >= 0)
      time = ((float)world->core.m_GameTick - (float)c->m_StartTime - c->m_StartTickOffset) / GAME_TICK_SPEED;
    if (time < 0.0f) time = 0.0f;
    snprintf(out, out_size, "%02d:%06.3f", (int)time / 60, fmodf(time, 60.0f));
    return true;
  }
  if (c->m_LastTimeCp >= 0 && c->m_LastTimeCp < NUM_TIME_CHECKPOINTS) {
    snprintf(out, out_size, "CP%d %.3fs", c->m_LastTimeCp + 1, c->m_aTimeCp[c->m_LastTimeCp]);
    return true;
  }
  return false;
}
