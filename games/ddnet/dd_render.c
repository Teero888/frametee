// How DDNet looks. Tees, hooks, weapons, projectiles, lasers, pickups, flags
// and trajectory lines.
//
// This used to live in the engine's user_interface.c, which meant the editor
// itself knew what a tee was. It now runs entirely inside the game module and
// reaches the screen through ft_engine_api, so the engine draws what it is
// handed without knowing any of it.

#include "dd_internal.h"

#include <stddef.h>
#include "dd_profile.h"

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

static void tile_pos(ddnet_vec2_t v, vec2 out) {
  out[0] = v.x / PX_PER_TILE;
  out[1] = v.y / PX_PER_TILE;
}

static uint64_t frame_followed_players(const ft_render_frame *frame);

int dd_view_player(const ft_render_frame *frame) {
  if (!frame || !frame->world) return -1;
  const int count = frame->world->player_count;
  if (frame->selected_player >= 0 && frame->selected_player < count) return frame->selected_player;
  const uint64_t followed = frame_followed_players(frame);
  for (int player = 0; player < count && player < 64; ++player)
    if ((followed >> player) & 1u) return player;
  return -1;
}

int dd_view_team(const ft_render_frame *frame) {
  if (!frame || !frame->world) return 0;
  const int client_id = ddnet_player_client(frame->world, dd_view_player(frame));
  return client_id >= 0 ? frame->world->core.players[client_id].team : 0;
}

int dd_seen_team(const ft_game *game, int world_index, int player) {
  if (world_index < 0 || world_index >= game->seen_team_worlds || player < 0 || player >= DDNET_MAX_CLIENTS) return 0;
  return game->seen_teams[world_index][player];
}

// Remembers the teams of a world's players for dd_seen_team.
static void see_teams(ft_game *game, const ft_world *world, int world_index) {
  if (world_index < 0) return;
  if (world_index >= game->seen_team_worlds) {
    uint8_t(*grown)[DDNET_MAX_CLIENTS] = realloc(game->seen_teams, (size_t)(world_index + 1) * sizeof(*grown));
    if (!grown) return;
    memset(&grown[game->seen_team_worlds], 0, (size_t)(world_index + 1 - game->seen_team_worlds) * sizeof(*grown));
    game->seen_teams = grown;
    game->seen_team_worlds = world_index + 1;
  }
  for (int player = 0; player < DDNET_MAX_CLIENTS; ++player) {
    const int client_id = ddnet_player_client(world, player);
    game->seen_teams[world_index][player] = client_id >= 0 ? world->core.players[client_id].team : 0;
  }
}

float dd_team_alpha(const ft_game *game, const ft_world *world, int viewer, int player) {
  const int seen_as = ddnet_player_client(world, viewer), owner = ddnet_player_client(world, player);
  if (seen_as < 0 || owner < 0 || seen_as == owner) return 1.f;
  const ddnet_player_t *a = &world->core.players[seen_as], *b = &world->core.players[owner];
  if (!a->is_solo && !b->is_solo && a->team == b->team) return 1.f;
  return (float)game->settings.others_alpha / 100.f;
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

static void submit_tee_hand(ft_game *game, const vec2 center_phys, const vec2 dir, float base_angle, float angle_offset, float off_x, float off_y,
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
  const float render_angle = base_angle - sign * angle_offset;
  dd_hand_push(game, hand, 10.0f / PX_PER_TILE, skin, render_angle, (float *)col_body, custom, hook_hand);
}

// --- one tee -----------------------------------------------------------------

typedef struct {
  int skin;
  vec3 body_col;
  vec3 feet_col;
  bool custom;
  vec2 dir;
  float aim_angle;
  dd_anim_state_t anim;
  bool in_air;
  bool stationary;
  bool inactive;
  float attack_ticks_passed;
} tee_visual_t;

static void build_tee_visual(ft_game *game, const ft_render_frame *frame, const ft_world *world, const ft_world *prev_world,
                             int index, float intra, const vec2 pos, tee_visual_t *out) {
  const ddnet_character_t *chr = ddnet_player_character(world, index);
  const ddnet_character_t *prev_chr = ddnet_player_character(prev_world, index);
  if (!prev_chr) prev_chr = chr;
  const ddnet_character_core_t *core = &chr->core;

  static dd_anim_state_t s_anim_base_idle;
  static dd_anim_state_t s_anim_base_inair;
  static bool s_anim_precomp_done = false;
  if (!s_anim_precomp_done) {
    dd_anim_state_set(&s_anim_base_idle, &anim_base, 0.0f);
    dd_anim_state_add(&s_anim_base_idle, &anim_idle, 0.0f, 1.0f);
    dd_anim_state_set(&s_anim_base_inair, &anim_base, 0.0f);
    dd_anim_state_add(&s_anim_base_inair, &anim_inair, 0.0f, 1.0f);
    s_anim_precomp_done = true;
  }

  out->stationary = fabsf(core->vel.x * 256.f) <= 1;
  const bool running = fabsf(core->vel.x * 256.f) >= 5000;
  const bool want_other_dir = (core->input.direction == -1 && core->vel.x > 0) || (core->input.direction == 1 && core->vel.x < 0);
  out->inactive = dd_player_inactive(world, index);
  // CPlayers::RenderPlayer: below the drawn tee
  out->in_air = !world->level || !ddnet_collision_check_point(&world->level->collision, pos[0] * PX_PER_TILE, pos[1] * PX_PER_TILE + 16.0f);
  out->attack_ticks_passed = (world->core.tick - chr->attack_tick) + intra;
  const float last_attack_time = out->attack_ticks_passed / (float)GAME_TICK_SPEED;

  if (out->in_air) {
    out->anim = s_anim_base_inair;
  } else if (out->stationary) {
    if (out->inactive) {
      dd_anim_state_set(&out->anim, &anim_base, 0.0f);
      dd_anim_state_add(&out->anim, core->input.direction < 0 ? &anim_sit_left : &anim_sit_right, 0.0f, 1.0f);
    } else {
      out->anim = s_anim_base_idle;
    }
  } else {
    dd_anim_state_set(&out->anim, &anim_base, 0.0f);
    if (!want_other_dir) {
      float walk_time = fmodf(pos[0] * PX_PER_TILE, 100.0f) / 100.0f;
      float run_time = fmodf(pos[0] * PX_PER_TILE, 200.0f) / 200.0f;
      if (walk_time < 0.0f) walk_time += 1.0f;
      if (run_time < 0.0f) run_time += 1.0f;
      if (running) dd_anim_state_add(&out->anim, core->vel.x < 0.0f ? &anim_run_left : &anim_run_right, run_time, 1.0f);
      else dd_anim_state_add(&out->anim, &anim_walk, walk_time, 1.0f);
    }
  }
  if (core->active_weapon == DDNET_WEAPON_HAMMER) dd_anim_state_add(&out->anim, &anim_hammer_swing, last_attack_time * 5.f, 1.0f);
  if (core->active_weapon == DDNET_WEAPON_NINJA) dd_anim_state_add(&out->anim, &anim_ninja_swing, last_attack_time * 2.f, 1.0f);

  out->dir[0] = lint2((float)prev_chr->core.input.target_x, (float)core->input.target_x, intra);
  out->dir[1] = lint2((float)prev_chr->core.input.target_y, (float)core->input.target_y, intra);
  glm_vec2_normalize(out->dir);
  out->aim_angle = atan2f(-out->dir[1], out->dir[0]);

  dd_player_profile_t profile;
  dd_profile_from_setup(setup_for(frame, index), &profile);
  out->skin = dd_gfx_skin_index(game, profile.skin);
  out->custom = profile.use_custom_color != 0;
  glm_vec3_copy((vec3){1.f, 1.f, 1.f}, out->feet_col);
  glm_vec3_copy((vec3){0.f, 0.f, 0.f}, out->body_col);

  if (out->custom) {
    dd_hsl_to_rgb(profile.color_body, out->body_col);
    dd_hsl_to_rgb(profile.color_feet, out->feet_col);
  }

  if (chr->freeze_time > 0 || core->active_weapon == DDNET_WEAPON_NINJA) {
    out->skin = game->gfx.ninja_skin;
    out->custom = false;
    // The native-color path still reads feet_col.r for air-jump dimming, so
    // no part of the player's custom tint may remain when forcing x_ninja.
    glm_vec3_copy((vec3){1.f, 1.f, 1.f}, out->body_col);
    glm_vec3_copy((vec3){1.f, 1.f, 1.f}, out->feet_col);
  }
  // The feet are dimmed while the tee has no air jump left: the used-up bit of
  // m_Jumped (CPlayers::RenderPlayer's m_GotAirJump), which the physics keeps
  // clear for a tee with endless jumps, as DDNet's server does.
  if (core->jumped & 2) {
    if (out->custom) {
      out->feet_col[0] *= 0.5f;
      out->feet_col[1] *= 0.5f;
      out->feet_col[2] *= 0.5f;
    } else {
      out->feet_col[0] = 0.5f;
    }
  }
}

static int tee_eye_state(const ft_world *world, int index, const ddnet_character_t *chr) {
  const int client_id = ddnet_player_client(world, index);
  const dd_input_t *record = &world->inputs[client_id];
  // An inactive tee sleeps: its eyes are closed over any others, frozen or not.
  if (dd_player_inactive(world, index)) return EYE_BLINK;
  int eye = get_flag_eye_state(record);
  if (chr->freeze_time > 0 && eye == 0) eye = EYE_BLINK;
  const int pain_age = world->core.tick - world->pain_ticks[client_id];
  if (pain_age >= 0 && pain_age < GAME_TICK_SPEED / 2) eye = EYE_PAIN;
  return eye;
}

static void render_weapon(ft_game *game, const ft_world *world, const ddnet_character_t *chr, const ddnet_character_t *prev_chr,
                          const tee_visual_t *tee, float intra, const vec2 pos) {
  (void)intra;
  const int active_weapon = chr->core.active_weapon;
  if (chr->freeze_time || active_weapon < 0 || active_weapon >= DDNET_NUM_WEAPONS) return;
  const dd_weapon_spec_t *spec = &dd_game_data.weapons.id[active_weapon];
  const float aim_angle = tee->aim_angle;
  const bool is_sit = tee->inactive && !tee->in_air && tee->stationary;
  const float flip_factor = (tee->dir[0] < 0.0f) ? -1.0f : 1.0f;

  vec2 phys_pos = {pos[0] * PX_PER_TILE, pos[1] * PX_PER_TILE};
  vec2 weapon_pos = {phys_pos[0], phys_pos[1]};

  float anim_attach_angle_rad = tee->anim.attach.angle * (2.0f * M_PI);
  float weapon_angle = anim_attach_angle_rad + aim_angle;
  int weapon_sprite_id = -1;

  if (active_weapon == DDNET_WEAPON_HAMMER) {
    weapon_sprite_id = GAMESKIN_HAMMER_BODY;
    weapon_pos[0] += tee->anim.attach.x;
    weapon_pos[1] += tee->anim.attach.y;
    weapon_pos[1] += spec->offsety;
    if (tee->dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;
    if (is_sit) weapon_pos[1] += 3.0f;

    // An inactive tee rests its hammer once a swing is over. DDNet's rotations are clockwise and
    // this renderer's the other way round, so its resting angles are negated too.
    if (!tee->inactive || tee->attack_ticks_passed / (float)GAME_TICK_SPEED * 5.f < 1.f) {
      weapon_angle = M_PI / 2.0f - flip_factor * anim_attach_angle_rad;
    } else {
      weapon_angle = tee->dir[0] < 0.0 ? -100.f : -500.f;
    }
  } else if (active_weapon == DDNET_WEAPON_NINJA) {
    weapon_sprite_id = GAMESKIN_NINJA_BODY;
    weapon_pos[1] += spec->offsety;
    if (is_sit) weapon_pos[1] += 3.0f;
    if (tee->dir[0] < 0.0f) weapon_pos[0] -= spec->offsetx;
    weapon_angle = -M_PI / 2.0f + flip_factor * anim_attach_angle_rad;

    const float attack_time_sec = tee->attack_ticks_passed / (float)GAME_TICK_SPEED;
    if (attack_time_sec <= 1.0f / 6.0f && spec->num_muzzles > 0) {
      const int muzzle_idx = world->core.tick % spec->num_muzzles;
      vec2 hadoken_dir = {chr->pos.x - prev_chr->pos.x, chr->pos.y - prev_chr->pos.y};
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
    switch (active_weapon) {
    case DDNET_WEAPON_GUN:
      weapon_sprite_id = GAMESKIN_GUN_BODY;
      break;
    case DDNET_WEAPON_SHOTGUN:
      weapon_sprite_id = GAMESKIN_SHOTGUN_BODY;
      break;
    case DDNET_WEAPON_GRENADE:
      weapon_sprite_id = GAMESKIN_GRENADE_BODY;
      break;
    case DDNET_WEAPON_LASER:
      weapon_sprite_id = GAMESKIN_LASER_BODY;
      break;
    default:
      break;
    }

    float recoil = 0.0f;
    const float a = tee->attack_ticks_passed / 5.0f;
    if (a < 1.0f) recoil = sinf(a * M_PI);

    weapon_pos[0] += tee->dir[0] * (spec->offsetx - recoil * 10.0f);
    weapon_pos[1] += tee->dir[1] * (spec->offsetx - recoil * 10.0f);
    weapon_pos[1] += spec->offsety;
    if (is_sit) weapon_pos[1] += 3.0f;

    if ((active_weapon == DDNET_WEAPON_GUN || active_weapon == DDNET_WEAPON_SHOTGUN) && spec->num_muzzles > 0) {
      if (tee->attack_ticks_passed > 0 && tee->attack_ticks_passed < spec->muzzleduration + 3.0f) {
        const int muzzle_idx = world->core.tick % spec->num_muzzles;
        vec2 muzzle_dir_y = {-tee->dir[1], tee->dir[0]};
        const float offset_y = -spec->muzzleoffsety * flip_factor;

        vec2 muzzle_phys;
        glm_vec2_copy(weapon_pos, muzzle_phys);
        muzzle_phys[0] += tee->dir[0] * spec->muzzleoffsetx + muzzle_dir_y[0] * offset_y;
        muzzle_phys[1] += tee->dir[1] * spec->muzzleoffsetx + muzzle_dir_y[1] * offset_y;

        const uint32_t muzzle_sprite = (active_weapon == DDNET_WEAPON_GUN ? GAMESKIN_GUN_MUZZLE1 : GAMESKIN_SHOTGUN_MUZZLE1) + muzzle_idx;
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

  static vec2 s_weapon_base_size[DDNET_NUM_WEAPONS];
  static bool s_weapon_size_init = false;
  if (!s_weapon_size_init) {
    const int sprites[DDNET_NUM_WEAPONS] = {
        GAMESKIN_HAMMER_BODY, GAMESKIN_GUN_BODY, GAMESKIN_SHOTGUN_BODY,
        GAMESKIN_GRENADE_BODY, GAMESKIN_LASER_BODY, GAMESKIN_NINJA_BODY};
    for (int w = 0; w < DDNET_NUM_WEAPONS; ++w) {
      const dd_weapon_spec_t *wspec = &dd_game_data.weapons.id[w];
      const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, (uint32_t)sprites[w]);
      if (rect) {
        const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
        s_weapon_base_size[w][0] = wspec->visual_size * ((float)rect->w / f) / PX_PER_TILE;
        s_weapon_base_size[w][1] = wspec->visual_size * ((float)rect->h / f) / PX_PER_TILE;
      }
    }
    s_weapon_size_init = true;
  }

  if (weapon_sprite_id != -1) {
    vec2 weapon_size = {s_weapon_base_size[active_weapon][0],
                        s_weapon_base_size[active_weapon][1] * flip_factor};
    vec2 render_pos = {weapon_pos[0] / PX_PER_TILE, weapon_pos[1] / PX_PER_TILE};
    dd_weapon_push(game, render_pos, weapon_size, weapon_angle, (uint32_t)weapon_sprite_id);
  }
  (void)pos;

  // Only these three are held with a visible hand in DDNet.
  switch (active_weapon) {
  case DDNET_WEAPON_GUN:
    submit_tee_hand(game, weapon_pos, tee->dir, aim_angle, -3.0f * M_PI / 4.0f, -15.0f, 4.0f, tee->skin, tee->body_col, tee->custom, false);
    break;
  case DDNET_WEAPON_SHOTGUN:
    submit_tee_hand(game, weapon_pos, tee->dir, aim_angle, -M_PI / 2.0f, -5.0f, 4.0f, tee->skin, tee->body_col, tee->custom, false);
    break;
  case DDNET_WEAPON_GRENADE:
    submit_tee_hand(game, weapon_pos, tee->dir, aim_angle, -M_PI / 2.0f, -4.0f, 7.0f, tee->skin, tee->body_col, tee->custom, false);
    break;
  default:
    break;
  }
}

static void render_hook(ft_game *game, const ft_world *world, const ddnet_character_t *chr, const ddnet_character_t *prev_chr,
                        const tee_visual_t *tee, float intra, const vec2 pos) {
  if (chr->core.hook_state < 1 || (prev_chr->core.hook_state == DDNET_HOOK_IDLE && intra <= 0.25)) return;

  vec2 hook_pos;
  {
    vec2 from, to;
    tile_pos(prev_chr->core.hook_pos, from);
    tile_pos(chr->core.hook_pos, to);
    const ddnet_character_t *hooked =
        chr->core.hooked_player >= 0 ? ddnet_world_character((ddnet_world_t *)&world->core, chr->core.hooked_player) : NULL;
    if (hooked) {
      tile_pos(hooked->prev_pos, from);
      tile_pos(hooked->pos, to);
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
    dd_hook_push(game, &draw);
  }

  static vec2 s_hook_head_size = {0};
  static bool s_hook_head_init = false;
  if (!s_hook_head_init) {
    const ft_sprite_rect *head = dd_sprite_rect(game, game->gfx.gameskin, GAMESKIN_HOOK_HEAD);
    if (head) {
      s_hook_head_size[0] = (float)head->w / 64.0f;
      s_hook_head_size[1] = (float)head->h / 64.0f;
    }
    s_hook_head_init = true;
  }
  const ft_sprite_draw head_draw = {.pos = {hook_pos[0], hook_pos[1]},
                                    .size = {s_hook_head_size[0], s_hook_head_size[1]},
                                    .rotation = angle,
                                    .sprite_index = GAMESKIN_HOOK_HEAD,
                                    .color = {1.f, 1.f, 1.f, 1.f},
                                    .tiling = {1.f, 1.f}};
  dd_hook_push(game, &head_draw);

  vec2 hook_center = {pos[0] * PX_PER_TILE, pos[1] * PX_PER_TILE};
  submit_tee_hand(game, hook_center, direction, angle, -M_PI / 2.0f, 20.0f, 0.0f, tee->skin, tee->body_col, tee->custom, true);
}

// --- entities ----------------------------------------------------------------

void dd_render_projectile(ft_game *game, const vec2 from, const vec2 to, float intra, int type, int tick, int start_tick) {
  vec2 p;
  lerp2(from, to, intra, p);
  uint32_t sprite = GAMESKIN_GRENADE_PROJ;
  if (type == DDNET_WEAPON_GUN) sprite = GAMESKIN_GUN_PROJ;
  else if (type == DDNET_WEAPON_SHOTGUN) sprite = GAMESKIN_SHOTGUN_PROJ;
  float rotation;
  // DDNet spins a grenade twice a second from the client's clock plus its
  // snapshot item index, which shifts whenever a projectile ahead of it comes
  // or goes and makes it jump. Its own age turns it smoothly instead.
  if (type == DDNET_WEAPON_GRENADE)
    rotation = -(((float)(tick - start_tick) + intra) / 50.f) * 4.f * M_PI;
  else
    rotation = atan2f(-(to[1] - from[1]), to[0] - from[0]);
  dd_draw_sprite(game, game->gfx.gameskin, DD_Z_PROJECTILES, p, (vec2){1.f, 1.f}, rotation, sprite, (vec4){1.f, 1.f, 1.f, 1.f});
}

// DDNet's laser types (LASERTYPE_*) and turret kinds (LASERGUNTYPE_*), as its
// client draws them.
enum { LASER_RIFLE, LASER_SHOTGUN, LASER_DOOR, LASER_FREEZE, LASER_DRAGGER, LASER_GUN, LASER_PLASMA };
enum { LASERGUN_UNFREEZE, LASERGUN_EXPLOSIVE, LASERGUN_FREEZE, LASERGUN_EXPFREEZE };

// ColorHSLA(packed) turned into ColorRGBA, as DDNet's color_cast does it.
static void laser_color(uint32_t packed, vec4 out) {
  const float h = (float)((packed >> 16) & 0xff) / 255.f, s = (float)((packed >> 8) & 0xff) / 255.f,
              l = (float)(packed & 0xff) / 255.f;
  const float h1 = h * 6.f;
  const float c = (1.f - fabsf(2.f * l - 1.f)) * s;
  const float x = c * (1.f - fabsf(fmodf(h1, 2.f) - 1.f));
  float r = 0.f, g = 0.f, b = 0.f;
  switch ((int)h1) {
  case 0: r = c; g = x; break;
  case 1: r = x; g = c; break;
  case 2: g = c; b = x; break;
  case 3: g = x; b = c; break;
  case 4: r = x; b = c; break;
  default: r = c; b = x; break;
  }
  const float m = l - c / 2.f;
  out[0] = r + m;
  out[1] = g + m;
  out[2] = b + m;
  out[3] = 1.f;
}

// The scale DDNet's GetSpriteScale gives a sprite of the extras sheet.
static float extras_scale(ft_game *game, uint32_t sprite) {
  const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.extras, sprite);
  if (!rect || rect->w <= 0 || rect->h <= 0) return 0.70710678f;
  return (float)rect->w / sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
}

// CItems::RenderLaser: a laser of DDNet's `type` from `from` to `to` (tiles), whose body is `ticks_body`
// ticks old (it thins out over the bounce delay) and whose head turns with `ticks_head`.
static void draw_laser(ft_game *game, const vec2 from, const vec2 to, int type, int subtype, float ticks_body,
                       float ticks_head, float bounce_delay_ms) {
  uint32_t outer_hsl = 11176233, inner_hsl = 11206591; // cl_laser_rifle_*
  switch (type) {
  case LASER_SHOTGUN: outer_hsl = 1866773, inner_hsl = 1467241; break;
  case LASER_DOOR: outer_hsl = 7667473, inner_hsl = 7701379; break;
  case LASER_FREEZE: outer_hsl = 11613223, inner_hsl = 12001153; break;
  case LASER_DRAGGER: outer_hsl = 57618, inner_hsl = 42398; break;
  case LASER_GUN:
  case LASER_PLASMA:
    if (subtype == LASERGUN_FREEZE || subtype == LASERGUN_EXPFREEZE) outer_hsl = 11613223, inner_hsl = 12001153;
    break;
  default: break;
  }
  vec4 outer, inner;
  laser_color(outer_hsl, outer);
  laser_color(inner_hsl, inner);

  if (type == LASER_DRAGGER) {
    ticks_head *= (float)(((subtype >> 1) % 3) * 4) + 1.f;
    ticks_head *= (subtype & 1) ? -1.f : 1.f;
  }

  vec2 dir;
  glm_vec2_sub((float *)to, (float *)from, dir);
  const float len = glm_vec2_norm(dir) * PX_PER_TILE;
  if (len > 0.f) {
    // rubber band effect
    if (type == LASER_DRAGGER) ticks_body = fminf(fmaxf(sqrtf(len) / 5.f, 1.f), 5.f);
    const float ms = ticks_body * 1000.f / GAME_TICK_SPEED;
    const float delay = type == LASER_RIFLE || type == LASER_SHOTGUN ? (bounce_delay_ms > 0 ? bounce_delay_ms : 150.f) : 150.f;
    float a = ms / delay;
    a = fminf(fmaxf(a, 0.f), 1.f);
    const float ia = 1.f - a;
    // DDNet's 7 and 5 are offsets on each side of the beam; FrameTee's line primitive takes the full width.
    dd_draw_line(game, DD_Z_LASERS, (float *)from, (float *)to, outer, 14.f * ia / PX_PER_TILE);
    // RenderLaser's ExtraOutline: the inner body stops a unit short of the head, and of the other end but
    // for a door.
    glm_vec2_normalize(dir);
    glm_vec2_scale(dir, 1.f / PX_PER_TILE, dir);
    vec2 inner_from = {from[0], from[1]};
    if (type != LASER_DOOR) glm_vec2_add(inner_from, dir, inner_from);
    vec2 inner_to = {to[0] - dir[0], to[1] - dir[1]};
    dd_draw_line(game, DD_Z_LASERS + 0.01f, inner_from, inner_to, inner, 10.f * ia / PX_PER_TILE);
  }

  // The engine turns sprites the other way round its y-down world than DDNet does.
  if (type == LASER_DOOR) {
    game->engine->draw_rect(DD_Z_LASERS + 0.02f, (ft_vec2){to[0] - 8.f / PX_PER_TILE, to[1] - 8.f / PX_PER_TILE},
                            (ft_vec2){16.f / PX_PER_TILE, 16.f / PX_PER_TILE}, (ft_color){outer[0], outer[1], outer[2], outer[3]});
    game->engine->draw_rect(DD_Z_LASERS + 0.03f, (ft_vec2){to[0] - 6.f / PX_PER_TILE, to[1] - 6.f / PX_PER_TILE},
                            (ft_vec2){12.f / PX_PER_TILE, 12.f / PX_PER_TILE}, (ft_color){inner[0], inner[1], inner[2], inner[3]});
  } else if (type == LASER_DRAGGER) {
    const float base = 20.f * extras_scale(game, EXTRA_PULLEY) / PX_PER_TILE;
    for (int in = 0; in < 2; ++in) {
      float *color = in ? inner : outer;
      // the circle at the end of the laser
      if (len > 0.f) {
        const float size = base * (in ? 4.f / 5.f : 1.f);
        dd_draw_sprite(game, game->gfx.extras, DD_Z_LASERS + 0.02f + 0.01f * in, (float *)from, (vec2){size, size}, 0.f,
                       EXTRA_PULLEY, color);
      }
      // the rotating orbs: rotate(vec2(10, 0), orb * 120 + ticks_head), in degrees
      const float size = base * (in ? 0.75f - 1.f / 5.f : 0.75f);
      for (int orb = 0; orb < 3; ++orb) {
        const float degrees = (float)(orb * 120) + ticks_head;
        const float rad = degrees * (float)M_PI / 180.f;
        vec2 p = {from[0] + cosf(rad) * 10.f / PX_PER_TILE, from[1] + sinf(rad) * 10.f / PX_PER_TILE};
        dd_draw_sprite(game, game->gfx.extras, DD_Z_LASERS + 0.04f + 0.01f * in, p, (vec2){size, size},
                       -(ticks_head + (float)orb * (float)M_PI * 2.f / 3.f), EXTRA_PULLEY, color);
      }
    }
  } else if (type == LASER_FREEZE) {
    const float pulsation = 6.f / 5.f + 1.f / 10.f * sinf(ticks_head / 2.f);
    const float angle = atan2f(to[1] - from[1], to[0] - from[0]);
    const float base = 20.f * extras_scale(game, EXTRA_HECTAGON) / PX_PER_TILE;
    const float outer_size = base * 6.f / 5.f * pulsation;
    dd_draw_sprite(game, game->gfx.extras, DD_Z_LASERS + 0.02f, (float *)to, (vec2){outer_size, outer_size}, -angle,
                   EXTRA_HECTAGON, outer);
    const float flake = 20.f * extras_scale(game, EXTRA_SNOWFLAKE) / PX_PER_TILE * pulsation;
    // snowflakes are white
    dd_draw_sprite(game, game->gfx.extras, DD_Z_LASERS + 0.03f, (float *)to, (vec2){flake, flake}, -angle, EXTRA_SNOWFLAKE,
                   (vec4){1.f, 1.f, 1.f, 1.f});
  } else {
    // One of the three splat particles, picked and spun by the tick itself.
    const int ticks = (int)ticks_head;
    const uint32_t head = PARTICLE_SPLAT01 + (uint32_t)(((ticks % 3) + 3) % 3);
    const float rotation = -(float)ticks;
    dd_draw_sprite(game, game->gfx.particles, DD_Z_LASERS + 0.02f, (float *)to, (vec2){0.75f, 0.75f}, rotation, head, outer);
    dd_draw_sprite(game, game->gfx.particles, DD_Z_LASERS + 0.03f, (float *)to, (vec2){0.625f, 0.625f}, rotation, head, inner);
  }
}

void dd_render_laser(ft_game *game, const vec2 from, const vec2 to, bool rifle, float age_ticks, float bounce_delay_ms, int tick,
                     float intra) {
  draw_laser(game, from, to, rifle ? LASER_RIFLE : LASER_SHOTGUN, 0, age_ticks, (float)tick + intra, bounce_delay_ms);
}

// The switch of an entity is off for a team (it blinks or shows as a dot), see CItems::OnRender.
static bool switch_off(const ddnet_world_t *core, int number, int team) {
  return number > 0 && number < core->num_switchers && !ddnet_world_switch(core, number, team).status;
}

// Whether a dragger is left out for the viewer, which sees its beam instead (CDragger::DraggerBeamUsingDraggerId).
static bool dragger_beam_shown(const ft_world *world, const ddnet_dragger_t *dragger, int viewer) {
  if (viewer < 0 || !ddnet_world_character((ddnet_world_t *)&world->core, viewer)) return false;
  const int team = world->core.players[viewer].team;
  const int team_target = ddnet_dragger_target(&world->core, dragger, team);
  const int target = world->core.players[viewer].is_solo || team_target < 0 ? viewer : team_target;
  if (target < 0 || ddnet_dragger_beam(&world->core, dragger, target) == -1) return false;
  return ddnet_world_character((ddnet_world_t *)&world->core, target) && world->core.players[target].team == team;
}

// What DDNet's server sends of the world and its client draws (see "Effects, sounds and drawing" in the
// physics' README): projectiles, lasers, and the laser walls, turrets, plasma, draggers and their beams of
// the laser list.
// What belongs to the tee of client `owner` (-1 for none) is drawn with its team's opacity
// (CItems: a projectile or laser with an owner, which for plasma and dragger beams is the tee they
// are for). Returns the world's own opacity, to go back to.
static float owned_alpha(ft_game *game, const ft_world *world, int viewer_player, int owner) {
  const float world_alpha = game->gfx.world_alpha;
  if (owner >= 0) game->gfx.world_alpha = world_alpha * dd_team_alpha(game, world, viewer_player, ddnet_client_player(world, owner));
  return world_alpha;
}

static void render_projectiles_and_lasers(ft_game *game, const ft_render_frame *frame, const ft_world *world, float intra) {
  const ddnet_world_t *core = &world->core;
  const int tick = core->tick;
  const int team = dd_view_team(frame);
  const int viewer_player = dd_view_player(frame);
  const int viewer = ddnet_player_client(world, viewer_player);
  // CItems::OnRender's blinking, on the ticks of a second
  const int ticks = tick % GAME_TICK_SPEED;
  const bool blink_slow = (ticks % 22) < 4, blink_proj = (ticks % 20) < 2, blink_fast = (ticks % 6) < 2;

  for (int i = core->first_entity[DDNET_ENTTYPE_PROJECTILE]; i != -1; i = core->entities[i].link.next) {
    const ddnet_entity_t *ent = &core->entities[i];
    const ddnet_projectile_t *proj = &ent->u.projectile;
    if (switch_off(core, ent->number, team) && (proj->explosive ? blink_fast : blink_proj)) continue;
    const float pt = (tick - proj->start_tick - 1) / (float)GAME_TICK_SPEED;
    const float ct = (tick - proj->start_tick) / (float)GAME_TICK_SPEED;
    vec2 from, to;
    tile_pos(ddnet_projectile_get_pos(core, ent, pt), from);
    tile_pos(ddnet_projectile_get_pos(core, ent, ct), to);
    const float world_alpha = owned_alpha(game, world, viewer_player, proj->owner);
    dd_render_projectile(game, from, to, intra, proj->type, tick, proj->start_tick);
    game->gfx.world_alpha = world_alpha;
  }

  // the start tick DDNet's client gives laser walls, draggers and turrets
  const int dragger_start = tick / 7 * 7 > tick - 4 ? tick / 7 * 7 : tick - 4;
  const int gun_start = tick / 7 * 7;
  const float head = (float)tick + intra;
  for (int i = core->first_entity[DDNET_ENTTYPE_LASER]; i != -1; i = core->entities[i].link.next) {
    const ddnet_entity_t *ent = &core->entities[i];
    const bool off = switch_off(core, ent->number, team);
    vec2 at;
    tile_pos(ent->pos, at);
    const int owner = ent->kind == DDNET_ENTITY_LASER          ? ent->u.laser.owner
                      : ent->kind == DDNET_ENTITY_PLASMA       ? ent->u.plasma.for_client_id
                      : ent->kind == DDNET_ENTITY_DRAGGER_BEAM ? ent->u.dragger_beam.for_client_id
                                                               : -1;
    const float world_alpha = owned_alpha(game, world, viewer_player, owner);
    switch (ent->kind) {
    case DDNET_ENTITY_LASER: {
      const ddnet_laser_t *laser = &ent->u.laser;
      vec2 from;
      tile_pos(laser->from, from);
      const int zone = laser->tune_zone >= 0 && laser->tune_zone < core->num_tune_zones ? laser->tune_zone : 0;
      draw_laser(game, from, at, laser->type == DDNET_WEAPON_SHOTGUN ? LASER_SHOTGUN : LASER_RIFLE, 0,
                 (float)(tick - laser->eval_tick) + intra, head, core->tuning_values[zone].laser_bounce_delay);
      break;
    }
    case DDNET_ENTITY_LIGHT: {
      if (off && blink_fast) break;
      // a laser wall whose switch is off is its head alone
      vec2 from;
      tile_pos(off ? ent->pos : ent->u.light.to, from);
      draw_laser(game, from, at, LASER_FREEZE, 0, (float)(tick - dragger_start) + intra, head, 150.f);
      break;
    }
    case DDNET_ENTITY_GUN: {
      if (off && blink_slow) break;
      const int subtype = (ent->u.gun.explosive ? 1 : 0) | (ent->u.gun.freeze ? 2 : 0);
      draw_laser(game, at, at, LASER_GUN, subtype, (float)(tick - gun_start) + intra, head, 150.f);
      break;
    }
    case DDNET_ENTITY_PLASMA: {
      if (off && blink_slow) break;
      const int subtype = (ent->u.plasma.explosive ? 1 : 0) | (ent->u.plasma.freeze ? 2 : 0);
      draw_laser(game, at, at, LASER_PLASMA, subtype, (float)(tick - ent->u.plasma.eval_tick) + intra, head, 150.f);
      break;
    }
    case DDNET_ENTITY_DRAGGER: {
      const ddnet_dragger_t *dragger = &ent->u.dragger;
      if ((off && blink_slow) || dragger_beam_shown(world, dragger, viewer)) break;
      const int strength = (int)lroundf(dragger->strength - 1.f);
      const int subtype = (dragger->ignore_walls ? 1 : 0) | ((strength < 0 ? 0 : strength > 2 ? 2 : strength) << 1);
      draw_laser(game, at, at, LASER_DRAGGER, subtype, (float)(tick - dragger_start) + intra, head, 150.f);
      break;
    }
    case DDNET_ENTITY_DRAGGER_BEAM: {
      const ddnet_dragger_beam_t *beam = &ent->u.dragger_beam;
      if (!beam->active || (off && blink_slow)) break;
      const ddnet_character_t *target = ddnet_world_character((ddnet_world_t *)core, beam->for_client_id);
      if (!target) break;
      const float dx = target->pos.x - ent->pos.x, dy = target->pos.y - ent->pos.y;
      if (sqrtf(dx * dx + dy * dy) >= (float)core->config.sv_dragger_range) break;
      // from the dragger to the tee, where it is drawn
      vec2 prev, cur, to;
      tile_pos(target->prev_pos, prev);
      tile_pos(target->pos, cur);
      lerp2(prev, cur, intra, to);
      const int strength = (int)lroundf(beam->strength - 1.f);
      const int subtype = (beam->ignore_walls ? 1 : 0) | ((strength < 0 ? 0 : strength > 2 ? 2 : strength) << 1);
      draw_laser(game, at, to, LASER_DRAGGER, subtype, (float)(tick - dragger_start) + intra, head, 150.f);
      break;
    }
    default:
      break;
    }
    game->gfx.world_alpha = world_alpha;
  }
}

// Whether the selected player is marked; frames from before the flag always mark it.
static bool frame_highlights_selected(const ft_render_frame *frame) {
  return frame->struct_size < offsetof(ft_render_frame, highlight_selected) + sizeof(frame->highlight_selected) ||
         frame->highlight_selected;
}

static void render_pickups(ft_game *game, const ft_render_frame *frame, const ft_world *world, const ft_world *prev_world, float intra) {
  if (!game->settings.render_pickups || !frame->active) return;
  const ddnet_world_t *core = &world->core;
  const int team = dd_view_team(frame);
  const bool blink = (core->tick % GAME_TICK_SPEED) % 22 < 4;

  ft_sprite_draw stack_draws[128];
  int capacity = 128;
  ft_sprite_draw *draws = stack_draws;
  uint32_t count = 0;

  const float animation_time = intra + (float)core->tick;

  ft_camera camera;
  game->engine->camera_get(&camera);
  const float margin = 4.0f;
  const float min_x = camera.visible.x - margin;
  const float max_x = camera.visible.x + camera.visible.w + margin;
  const float min_y = camera.visible.y - margin;
  const float max_y = camera.visible.y + camera.visible.h + margin;

  for (int i = core->first_entity[DDNET_ENTTYPE_PICKUP]; i != -1; i = core->entities[i].link.next) {
    const ddnet_entity_t *ent = &core->entities[i];
    if (ent->kind != DDNET_ENTITY_PICKUP) continue;
    if (switch_off(core, ent->number, team) && blink) continue;
    // Pickups on conveyors move: drawn between where they were and are, like DDNet's client.
    vec2 pos, cur;
    tile_pos(ent->pos, cur);
    glm_vec2_copy(cur, pos);
    if (prev_world && i < prev_world->core.num_entities && prev_world->core.entities[i].kind == DDNET_ENTITY_PICKUP) {
      vec2 prev;
      tile_pos(prev_world->core.entities[i].pos, prev);
      lerp2(prev, cur, intra, pos);
    }
    if (pos[0] < min_x || pos[0] > max_x || pos[1] < min_y || pos[1] > max_y) continue;

    const int type = ent->u.pickup.type, subtype = ent->u.pickup.subtype;
    vec2 size = {1.0f, 1.0f};
    int idx = -1;
    if (type == DDNET_POWERUP_HEALTH || type == DDNET_POWERUP_ARMOR) idx = GAMESKIN_PICKUP_HEALTH + type;
    else if (type >= DDNET_POWERUP_ARMOR_SHOTGUN && type <= DDNET_POWERUP_ARMOR_LASER)
      idx = GAMESKIN_PICKUP_ARMOR_SHOTGUN + type - DDNET_POWERUP_ARMOR_SHOTGUN;
    else if (type == DDNET_POWERUP_WEAPON && subtype >= 0 && subtype < DDNET_NUM_WEAPONS) idx = GAMESKIN_PICKUP_HAMMER + subtype;
    else if (type == DDNET_POWERUP_NINJA) idx = GAMESKIN_PICKUP_NINJA;
    if (idx < 0) continue;

    const ft_sprite_rect *rect = dd_sprite_rect(game, game->gfx.gameskin, (uint32_t)idx);
    if (rect && rect->w > 0 && rect->h > 0) {
      const float f = sqrtf((float)rect->w * rect->w + (float)rect->h * rect->h);
      const float scale_x = (float)rect->w / f;
      const float scale_y = (float)rect->h / f;

      if (type == DDNET_POWERUP_HEALTH || type == DDNET_POWERUP_ARMOR || type >= DDNET_POWERUP_ARMOR_SHOTGUN) {
        size[0] = 1.f / scale_x;
        size[1] = 1.f / scale_y;
      } else if (type == DDNET_POWERUP_WEAPON) {
        const dd_weapon_spec_t *spec = &dd_game_data.weapons.id[subtype];
        size[0] = spec->visual_size * scale_x / PX_PER_TILE;
        size[1] = spec->visual_size * scale_y / PX_PER_TILE;
      } else if (type == DDNET_POWERUP_NINJA) {
        size[0] = 4.f * scale_x;
        size[1] = 4.f * scale_y;
        pos[0] -= 10.f / PX_PER_TILE;
      }
    }

    // Pickups bob on a per-position phase so a row of them does not pulse in unison.
    const float offset = pos[1] + pos[0];
    pos[0] += (cosf((animation_time / GAME_TICK_SPEED) * 2.0f + offset) * 2.5f) / PX_PER_TILE;
    pos[1] += (sinf((animation_time / GAME_TICK_SPEED) * 2.0f + offset) * 2.5f) / PX_PER_TILE;

    if ((int)count == capacity) {
      ft_sprite_draw *grown = malloc(sizeof(*grown) * (size_t)capacity * 2);
      if (!grown) break;
      memcpy(grown, draws, sizeof(*grown) * count);
      if (draws != stack_draws) free(draws);
      draws = grown;
      capacity *= 2;
    }
    draws[count++] = (ft_sprite_draw){.pos = {pos[0], pos[1]},
                                      .size = {size[0], size[1]},
                                      .rotation = 0.f,
                                      .sprite_index = (uint32_t)idx,
                                      .color = {1.f, 1.f, 1.f, 1.f},
                                      .tiling = {1.f, 1.f}};
  }

  if (count > 0) dd_draw_sprites(game, game->gfx.gameskin, DD_Z_PICKUPS, draws, count);
  if (draws != stack_draws) free(draws);
}

// --- entry point -------------------------------------------------------------

static void render_entities(ft_game *game, const ft_render_frame *frame) {
  const ft_world *world = frame->world;
  const ft_world *prev_world = frame->previous_world ? frame->previous_world : frame->world;
  if (!world) return;
  if (world->player_count != prev_world->player_count) prev_world = world;

  const float intra = frame->alpha;
  const int selected = frame->selected_player;
  const int viewer = dd_view_player(frame);
  // The world's own opacity, which a tee of another team draws a part of.
  const float world_alpha = game->gfx.world_alpha;

  render_pickups(game, frame, world, prev_world, intra);

  if (game->settings.render_players) {
    // Cull to the viewport with a margin wide enough that a tee entering the
    // screen is already drawn by the time any part of it is visible.
    const ft_rect *vis = &frame->state.camera.visible;
    const float margin = 6.0f;
    const float min_x = vis->x - margin;
    const float max_x = vis->x + vis->w + margin;
    const float min_y = vis->y - margin;
    const float max_y = vis->y + vis->h + margin;

    for (int i = 0; i < world->player_count; ++i) {
      // No tee: dead, in /spec, or not in its recording at this tick.
      const ddnet_character_t *chr = ddnet_player_character(world, i);
      if (!chr || dd_replay_absent(world, i)) continue;
      const ddnet_character_t *prev_chr = ddnet_player_character(prev_world, i);
      if (!prev_chr) prev_chr = chr;

      vec2 from, to, p;
      tile_pos(chr->prev_pos, from);
      tile_pos(chr->pos, to);
      lerp2(from, to, intra, p);

      if (p[0] < min_x || p[0] > max_x || p[1] < min_y || p[1] > max_y) {
        if (!(frame->state.recording && i == selected)) continue;
      }

      // Everything of a tee in another team is drawn faded, as CPlayers does.
      game->gfx.world_alpha = world_alpha * dd_team_alpha(game, world, viewer, i);

      if (dd_player_specced(world, i)) {
        // DDNet's spectating tee: idle, blinking, facing right, in the x_spec skin.
        dd_anim_state_t idle;
        dd_anim_state_set(&idle, &anim_base, 0.0f);
        dd_anim_state_add(&idle, &anim_idle, 0.0f, 1.0f);
        dd_skin_push(game, p, 1.0f, game->gfx.spec_skin, EYE_BLINK, (vec2){1.f, 0.f}, &idle, (vec3){0.f, 0.f, 0.f}, (vec3){1.f, 1.f, 1.f},
                     false);
        continue;
      }

      tee_visual_t tee;
      build_tee_visual(game, frame, world, prev_world, i, intra, p, &tee);
      const int eye = tee_eye_state(world, i, chr);

      dd_skin_push(game, p, 1.0f, tee.skin, eye, tee.dir, &tee.anim, tee.body_col, tee.feet_col, tee.custom);

      if (!frame->state.recording && i == selected && frame_highlights_selected(frame)) {
        // Marker triangle floating above the selected tee, pointing down at it.
        const float width = 1.0f, height = 0.8f, gap = 0.35f;
        vec4 marker = {frame->accent.r, frame->accent.g, frame->accent.b, 0.5f};
        vec2 tip = {p[0], p[1] - 1.0f - gap};
        vec2 left = {p[0] - width * 0.5f, tip[1] - height};
        vec2 right = {p[0] + width * 0.5f, tip[1] - height};
        dd_draw_triangle(game, DD_Z_LINES, tip, left, right, marker);
      }

      if (game->settings.center_dot && world->level) {
        const ddnet_collision_t *collision = &world->level->collision;
        const int x = (int)(p[0] * PX_PER_TILE), y = (int)(p[1] * PX_PER_TILE);
        const bool freeze = ddnet_collision_get_tile(collision, x, y) == TILE_FREEZE ||
                            ddnet_collision_get_front_tile(collision, x, y) == TILE_FREEZE;
        dd_draw_circle(game, DD_Z_LINES + 1.0f, p, 2.f / PX_PER_TILE, freeze ? (vec4){0, 0, 1, 1} : (vec4){0, 1, 0, 1}, 4);
      }

      if (game->settings.render_weapons) {
        render_hook(game, world, chr, prev_chr, &tee, intra, p);
        render_weapon(game, world, chr, prev_chr, &tee, intra, p);
      }
    }
    game->gfx.world_alpha = world_alpha;
  }

  render_projectiles_and_lasers(game, frame, world, intra);
  dd_recording_render_entities(game, world, intra, viewer);
}

void dd_render(ft_game *game, const ft_render_frame *frame) {
  if (!game->gfx.ready || !frame) return;

  switch (frame->pass) {
  case FT_PASS_LEVEL_BACKGROUND:
    if (!game->settings.render_map) break;
    if (game->settings.entities_view) {
      dd_map_render(game, frame);
      dd_render_map_overlays(game, frame);
    } else dd_map_design_render(game, frame);
    break;
  case FT_PASS_LEVEL_FOREGROUND:
    if (game->settings.render_map && !game->settings.entities_view) dd_map_design_render(game, frame);
    break;
  case FT_PASS_ENTITIES: {
    if (frame->first_world || frame->world_index <= 0) {
      dd_skins_begin(game);
    }
    // Entity passes are per world. The old port tried to draw particles from a
    // shared level pass, whose world_index is deliberately -1, so none could
    // ever be selected or rendered.
    // The world's own drawing fades with its opacity; doors fade by themselves, and the batches
    // flushed below already carry each world's.
    game->gfx.world_alpha = frame->world_index >= 0 && frame->opacity < 1.f ? (frame->opacity > 0.f ? frame->opacity : 0.f) : 1.f;
    if (game->settings.render_particles && frame->world_index >= 0) {
      dd_particles_advance(game, frame->world_index, frame->level, frame->tick, frame->alpha);
      dd_particle_system_t *particles = dd_particles_for(game, frame->world_index);
      if (particles) dd_particles_render(particles, game, -1);
    }
    render_entities(game, frame);
    if (frame->world) see_teams(game, frame->world, frame->world_index);
    game->gfx.world_alpha = 1.f;
    dd_render_doors(game, frame);
    if (frame->last_world || frame->world_index < 0 || frame->world_index >= frame->world_count - 1) {
      dd_skins_flush(game);
    }
    break;
  }
  case FT_PASS_OVERLAY:
    dd_render_world_overlays(game, frame);
    render_cursor(game, frame);
    break;
  default:
    break;
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

// One player's crosshair: `aim` relative to the tee, in DDNet's aim units.
static void draw_crosshair(ft_game *game, const ft_render_frame *frame, const ddnet_character_t *chr, ft_vec2 aim) {
  const uint32_t weapon = chr->core.active_weapon >= 0 && chr->core.active_weapon < CURSOR_SPRITE_COUNT ? (uint32_t)chr->core.active_weapon : 0;
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
  tile_pos(chr->prev_pos, from);
  tile_pos(chr->pos, to);
  lerp2(from, to, frame->alpha, tee_pos);

  vec2 pos = {tee_pos[0] + aim.x / PX_PER_TILE * zoom1_tiles, tee_pos[1] + aim.y / PX_PER_TILE * zoom1_tiles};
  const float opacity = frame->opacity < 1.f ? (frame->opacity > 0.f ? frame->opacity : 0.f) : 1.f;
  dd_draw_sprite(game, game->gfx.cursor, DD_Z_CURSOR, pos, size, 0.f, weapon, (vec4){1.f, 1.f, 1.f, opacity});
}

static uint64_t frame_followed_players(const ft_render_frame *frame) {
  return frame->struct_size >= offsetof(ft_render_frame, followed_players) + sizeof(frame->followed_players) ? frame->followed_players : 0;
}

// Crosshairs on the players a camera follows: the selected one while recording or while the game's
// follow camera is locked to it (only in the focused world), the camera animation's characters,
// or, if asked, everyone.
static void render_cursor(ft_game *game, const ft_render_frame *frame) {
  if (!game->gfx.cursor || !frame->world) return;
  const ft_world *world = frame->world;
  const ft_world *previous = frame->previous_world;
  const int selected = frame->active ? frame->selected_player : -1;
  // In free view the crosshair has nothing to sit against and just floats; under the camera
  // animation the game's own mode shows nothing (its subjects get theirs as `followed`).
  const bool animated = frame->struct_size >= offsetof(ft_render_frame, camera_animated) + sizeof(frame->camera_animated) &&
                        frame->camera_animated;
  const bool locked = frame->state.camera.mode == DD_CAMERA_FOLLOW && game->settings.render_cursor_follow && !animated;
  const bool selected_shown = selected >= 0 && selected < world->player_count && (frame->state.recording || locked);
  const uint64_t followed = frame_followed_players(frame);

  for (int player = 0; player < world->player_count; ++player) {
    const bool is_selected = selected_shown && player == selected;
    if (!is_selected && !game->settings.render_cursor_all && !(player < 64 && ((followed >> player) & 1u))) continue;
    if (dd_replay_absent(frame->world, player) || dd_player_specced(frame->world, player)) continue;
    const ddnet_character_t *chr = ddnet_player_character(world, player);
    if (!chr) continue;

    // Aim interpolated between the two ticks, exactly like the tee it belongs to:
    // from the input each world was stepped with, which is the timeline's input
    // of the tick before it. Asking the timeline for this tick and the one before
    // put the crosshair a tick ahead of the weapon.
    ft_vec2 aim = {(float)chr->core.input.target_x, (float)chr->core.input.target_y};
    if (!(is_selected && frame->state.recording)) {
      const ddnet_character_t *prev = previous ? ddnet_player_character(previous, player) : NULL;
      if (!prev) prev = chr;
      aim.x = lint2((float)prev->core.input.target_x, (float)chr->core.input.target_x, frame->alpha);
      aim.y = lint2((float)prev->core.input.target_y, (float)chr->core.input.target_y, frame->alpha);
    } else {
      // While recording, get_player_input hands back the live input for the tick
      // under the playhead, so the crosshair tracks the mouse directly.
      input_record_bytes_t previous_input, current;
      const bool have_prev = game->engine->get_player_input(frame->state.selected_player, frame->tick - 1, &previous_input);
      const bool have_cur = game->engine->get_player_input(frame->state.selected_player, frame->tick, &current);
      if (have_cur) {
        const dd_input_t *cur = (const dd_input_t *)&current;
        const dd_input_t *prev = have_prev ? (const dd_input_t *)&previous_input : cur;
        aim.x = lint2((float)prev->m_TargetX, (float)cur->m_TargetX, frame->alpha);
        aim.y = lint2((float)prev->m_TargetY, (float)cur->m_TargetY, frame->alpha);
      }
    }
    draw_crosshair(game, frame, chr, aim);
  }
}

// --- camera ------------------------------------------------------------------

// DDNet's camera modes. Which modes exist and where each one points is the
// game's business; the engine only owns panning, zooming and the projection.
const ft_camera_mode dd_camera_modes[DD_CAMERA_MODE_COUNT] = {
    [DD_CAMERA_FREE] = {"free", "Free view", "Pan and zoom freely", FT_CAMERA_MODE_FREE, 0.f},
    // DDNet's own view: about 1430 units wide on a 16:9 screen.
    [DD_CAMERA_FOLLOW] = {"follow", "Lock to tee", "Keeps the selected tee centred", FT_CAMERA_MODE_DIRECTED, 1430.f / 32.f},
};

bool dd_camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout) {
  (void)game;
  if (!frame || !inout || !frame->world) return false;

  // Recording always follows the tee being driven, whatever mode is selected:
  // steering something off-screen is not a thing anyone wants to do.
  const bool follow = frame->recording || frame->mode == DD_CAMERA_FOLLOW;
  if (!follow) return false;

  const ddnet_character_t *chr = ddnet_player_character(frame->world, frame->player);
  if (!chr) return false;

  // Interpolate exactly as the tee itself is drawn. Snapping to the tick
  // position while the tee moves smoothly is what made the lock look jittery.
  vec2 from, to, pos;
  tile_pos(chr->prev_pos, from);
  tile_pos(chr->pos, to);
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
  const ddnet_character_t *chr = ddnet_player_character(world, player);
  if (!chr || max_lines == 0) return 0;
  const ddnet_character_core_t *c = &chr->core;
  const ddnet_player_t *p = &world->core.players[ddnet_player_client(world, player)];

  uint32_t count = 0;
#define LINE(...)                                                      \
  do {                                                                 \
    if (count >= max_lines) return count;                              \
    snprintf(out + (size_t)count * line_size, line_size, __VA_ARGS__); \
    ++count;                                                           \
  } while (0)

  const int pos_x = (int)chr->pos.x;
  const int pos_y = (int)chr->pos.y;
  const float vel_x = c->vel.x;
  const float vel_y = c->vel.y;

  const float vel_scaled_x = roundf(vel_x * 256.0f);
  const float vel_scaled_y = roundf(vel_y * 256.0f);

  float speed_x = vel_scaled_x / 256.0f * (50.0f / 32.0f);
  if (vel_scaled_x >= -1.0f && vel_scaled_x <= 1.0f) {
    speed_x = 0.0f;
  }
  float speed_y = vel_scaled_y / 256.0f * (50.0f / 32.0f);
  if (vel_scaled_y >= -128.0f && vel_scaled_y <= 128.0f) {
    speed_y = 0.0f;
  }

  LINE("Character:");
  LINE("Pos: %d, %d; (%.4f, %.4f)", pos_x, pos_y, pos_x / 32.f, pos_y / 32.f);
  LINE("Vel: %.2f, %.2f; (%.2f, %.2f BPS)", vel_x, vel_y, speed_x, speed_y);
  LINE("Freeze: %d", chr->freeze_time);
  LINE("Reload: %d", chr->reload_timer);
  LINE("Weapon: %d", c->active_weapon);
  LINE("Weapons: [ %d, %d, %d, %d, %d, %d ]", c->weapons[0].got, c->weapons[1].got, c->weapons[2].got, c->weapons[3].got,
       c->weapons[4].got, c->weapons[5].got);

  // Race timer, counting up until the run finishes and then holding.
  float race_time = -1.f;
  if (p->finish_tick >= 0) {
    race_time = (float)p->finish_time_ticks / GAME_TICK_SPEED;
  } else if (chr->race_state == DDNET_RACE_STARTED) {
    race_time = ((float)world->core.tick + alpha - (float)chr->start_time) / GAME_TICK_SPEED;
    if (race_time < 0.0f) race_time = 0.0f;
  }
  if (race_time >= 0.0f) {
    const int minutes = (int)race_time / 60;
    const float seconds = fmodf(race_time, 60.0f);
    LINE(p->finish_tick >= 0 ? "Finish Time: %02d:%06.3f" : "Time: %02d:%06.3f", minutes, seconds);
  }
#undef LINE

  return count;
}

// The line shown beside a tee in the editor's player list: its finish time once
// it has one, otherwise how far it got.
bool dd_player_label(ft_game *game, const ft_world *world, int32_t player, char *out, size_t out_size) {
  (void)game;
  const int client_id = ddnet_player_client(world, player);
  if (client_id < 0) return false;
  const ddnet_player_t *p = &world->core.players[client_id];
  if (p->finish_tick >= 0) {
    const float time = (float)p->finish_time_ticks / GAME_TICK_SPEED;
    snprintf(out, out_size, "%02d:%06.3f", (int)time / 60, fmodf(time, 60.0f));
    return true;
  }
  const ddnet_character_t *c = ddnet_player_character(world, player);
  if (c && c->last_time_cp >= 0 && c->last_time_cp < DDNET_MAX_CHECKPOINTS) {
    snprintf(out, out_size, "CP%d %.3fs", c->last_time_cp + 1, c->current_time_cp[c->last_time_cp]);
    return true;
  }
  return false;
}
