// Shared internals of the DDNet game module.
//
// Nothing in here crosses the ABI. The engine only ever sees the opaque
// ft_game / ft_level / ft_world handles declared in <frametee/game_abi.h>; this
// header is what the module's own translation units share with each other.

#ifndef DD_INTERNAL_H
#define DD_INTERNAL_H

#include <frametee/game_abi.h>

#include <ddnet_physics/gamecore.h>
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

// DDNet physics works in pixels, 32 to a tile. The editor's world units are
// tiles, so everything crossing the ABI is scaled by this.
#define PX_PER_TILE 32.0f

// Draw order inside the module's own passes. These are the module's business:
// the engine only sorts by the float it is given.
#define DD_Z_PICKUPS 1.f
#define DD_Z_PARTICLES_BACK 2.f
#define DD_Z_HOOK 3.f
#define DD_Z_PROJECTILES 4.f
#define DD_Z_HOOK_HAND 4.5f
#define DD_Z_WEAPONS 5.f
#define DD_Z_WEAPON_HAND 5.5f
#define DD_Z_SKINS 6.f
#define DD_Z_PARTICLES_FRONT 8.0f
#define DD_Z_MAP 7.0f
#define DD_Z_LINES 9.0f
#define DD_Z_CURSOR 100.0f

#define DD_MAX_SKINS 128
#define DD_SKIN_PREVIEW_LAYER (DD_MAX_SKINS - 1)
#define DD_MAX_PLAYER_SKINS DD_SKIN_PREVIEW_LAYER
#define DD_SKIN_ATLAS_W 512
#define DD_SKIN_ATLAS_H 352

// --- handles ----------------------------------------------------------------

// Physics callbacks are transient, but a demo snapshot is assembled after the
// step that raised them. Keep the callback output on the DDNet world itself so
// effects from entities that were created and destroyed in one tick are not
// lost. Only callback types that become demo events need to be retained here;
// smoke, bullet trails and confetti are client-side presentation effects.
typedef struct {
  float x;
  float y;
  int type;
  int client_id;
} dd_physics_particle_event_t;

typedef struct {
  float x;
  float y;
  float angle;
  int amount;
  int client_id;
} dd_physics_damage_event_t;

typedef struct {
  float x;
  float y;
  int sound_id;
  int client_id;
} dd_physics_sound_event_t;

struct ft_level {
  SCollision collision;
  STeeGrid grid;
  SConfig config;
  SWorldCore prototype; // pristine world, cloned for every ft_world
  EGameMode mode;
  char name[128];
  bool loaded;

  // Pickups are derived from the map once, then drawn every frame.
  SPickup *pickups;
  mvec2 *pickup_positions;
  int *pickup_cooldown_keys;
  int *ninja_pickup_indices;
  int num_ninja_pickups;
  int num_pickups;

  // Tile layers uploaded for rendering; owned by the module, rebuilt per level.
  ft_texture *layer_textures[3];
};

struct ft_world {
  SWorldCore core;
  ft_level *level;
  ft_game *game;
  // Which editor world this belongs to. Every cached copy of one shares it, so
  // effects raised while stepping land in the right particle system.
  int index;
  dd_physics_particle_event_t *physics_particle_events;
  int physics_particle_event_count;
  int physics_particle_event_capacity;
  dd_physics_damage_event_t *physics_damage_events;
  int physics_damage_event_count;
  int physics_damage_event_capacity;
  dd_physics_sound_event_t *physics_sound_events;
  int physics_sound_event_count;
  int physics_sound_event_capacity;
  bool render_physics_effects;
};

// --- sprites ----------------------------------------------------------------
// game.png, 32x16 grid
typedef enum {
  GAMESKIN_HAMMER_BODY,
  GAMESKIN_GUN_BODY,
  GAMESKIN_GUN_PROJ,
  GAMESKIN_GUN_MUZZLE1,
  GAMESKIN_GUN_MUZZLE2,
  GAMESKIN_GUN_MUZZLE3,
  GAMESKIN_SHOTGUN_BODY,
  GAMESKIN_SHOTGUN_PROJ,
  GAMESKIN_SHOTGUN_MUZZLE1,
  GAMESKIN_SHOTGUN_MUZZLE2,
  GAMESKIN_SHOTGUN_MUZZLE3,
  GAMESKIN_GRENADE_BODY,
  GAMESKIN_GRENADE_PROJ,
  GAMESKIN_LASER_BODY,
  GAMESKIN_LASER_PROJ,
  GAMESKIN_NINJA_BODY,
  GAMESKIN_NINJA_MUZZLE1,
  GAMESKIN_NINJA_MUZZLE2,
  GAMESKIN_NINJA_MUZZLE3,
  GAMESKIN_HEALTH_FULL,
  GAMESKIN_HEALTH_EMPTY,
  GAMESKIN_ARMOR_FULL,
  GAMESKIN_ARMOR_EMPTY,
  GAMESKIN_HOOK_CHAIN,
  GAMESKIN_HOOK_HEAD,
  GAMESKIN_PARTICLE_0,
  GAMESKIN_PARTICLE_1,
  GAMESKIN_PARTICLE_2,
  GAMESKIN_PARTICLE_3,
  GAMESKIN_PARTICLE_4,
  GAMESKIN_PARTICLE_5,
  GAMESKIN_PARTICLE_6,
  GAMESKIN_PARTICLE_7,
  GAMESKIN_PARTICLE_8,
  GAMESKIN_STAR_0,
  GAMESKIN_STAR_1,
  GAMESKIN_STAR_2,
  GAMESKIN_PICKUP_HEALTH,
  GAMESKIN_PICKUP_ARMOR,
  GAMESKIN_PICKUP_HAMMER,
  GAMESKIN_PICKUP_GUN,
  GAMESKIN_PICKUP_SHOTGUN,
  GAMESKIN_PICKUP_GRENADE,
  GAMESKIN_PICKUP_LASER,
  GAMESKIN_PICKUP_NINJA,
  GAMESKIN_PICKUP_ARMOR_SHOTGUN,
  GAMESKIN_PICKUP_ARMOR_GRENADE,
  GAMESKIN_PICKUP_ARMOR_NINJA,
  GAMESKIN_PICKUP_ARMOR_LASER,
  GAMESKIN_FLAG_BLUE,
  GAMESKIN_FLAG_RED,
  GAMESKIN_SPRITE_COUNT
} dd_gameskin_sprite_t;

// particles.png, 8x8 grid
typedef enum {
  PARTICLE_SLICE,
  PARTICLE_BALL,
  PARTICLE_SPLAT01,
  PARTICLE_SPLAT02,
  PARTICLE_SPLAT03,
  PARTICLE_SMOKE,
  PARTICLE_SHELL,
  PARTICLE_EXPL01,
  PARTICLE_AIRJUMP,
  PARTICLE_HIT01,
  PARTICLE_SPRITE_COUNT
} dd_particle_sprite_t;

// extras.png, 16x16 grid
typedef enum { EXTRA_SNOWFLAKE,
               EXTRA_SPARKLE,
               EXTRA_PULLEY,
               EXTRA_HECTAGON,
               EXTRA_SPRITE_COUNT } dd_extra_sprite_t;

typedef enum { CURSOR_HAMMER,
               CURSOR_GUN,
               CURSOR_SHOTGUN,
               CURSOR_GRENADE,
               CURSOR_LASER,
               CURSOR_NINJA,
               CURSOR_SPRITE_COUNT } dd_cursor_sprite_t;

// The particle system tags a sprite with which sheet it came from by offsetting
// the index, the same way the engine's renderer used to.
#define PARTICLE_SPRITE_OFFSET 1000
#define EXTRA_SPRITE_OFFSET 2000

// --- animation --------------------------------------------------------------

typedef struct {
  float time;
  float x, y;
  float angle;
} dd_anim_keyframe_t;

typedef struct {
  int num_frames;
  const dd_anim_keyframe_t *frames;
} dd_anim_sequence_t;

typedef struct {
  const char *name;
  dd_anim_sequence_t body;
  dd_anim_sequence_t back_foot;
  dd_anim_sequence_t front_foot;
  dd_anim_sequence_t attach;
} dd_animation_t;

typedef struct {
  dd_anim_keyframe_t body;
  dd_anim_keyframe_t back_foot;
  dd_anim_keyframe_t front_foot;
  dd_anim_keyframe_t attach;
} dd_anim_state_t;

void dd_anim_state_set(dd_anim_state_t *s, const dd_animation_t *a, float t);
void dd_anim_state_add(dd_anim_state_t *s, const dd_animation_t *a, float t, float amt);

extern const dd_animation_t anim_base;
extern const dd_animation_t anim_idle;
extern const dd_animation_t anim_inair;
extern const dd_animation_t anim_walk;
extern const dd_animation_t anim_run_left;
extern const dd_animation_t anim_run_right;
extern const dd_animation_t anim_hammer_swing;
extern const dd_animation_t anim_ninja_swing;
extern const dd_animation_t anim_sit_left;
extern const dd_animation_t anim_sit_right;

// Weapon presentation data, from DDNet's content.py.
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
  float visual_size;
} dd_weapon_spec_t;

typedef struct {
  dd_weapon_spec_t id[NUM_WEAPONS];
} dd_weapon_specs_t;

typedef struct {
  dd_weapon_specs_t weapons;
} dd_data_container_t;

extern const dd_data_container_t dd_game_data;

// --- particles --------------------------------------------------------------

#define DD_MAX_PARTICLES (1024 * 8)
#define DD_MAX_FLOW_EVENTS 64

typedef enum {
  GROUP_PROJECTILE_TRAIL = 0,
  GROUP_TRAIL_EXTRA,
  GROUP_EXPLOSIONS,
  GROUP_EXTRA,
  GROUP_GENERAL,
  NUM_PARTICLE_GROUPS
} dd_particle_group_t;

typedef struct {
  vec2 current_pos;
  vec2 current_vel;
  vec2 prev_pos;
  double last_sim_time;
  double prev_sim_time;
  double spawn_time;
  float life_span;
  float gravity;
  float friction;
  float flow_affected;
  uint32_t current_seed;
  int creation_tick;
  int group;
  int sprite_index;
  bool collides;

  vec2 start_pos;
  vec2 start_vel;
  float start_size;
  float end_size;
  float rot;
  float rot_speed;
  vec4 color;
  bool use_alpha_fading;
  float start_alpha;
  float end_alpha;
  uint32_t seed;
} dd_particle_t;

typedef struct {
  double time;
  vec2 pos;
  float strength;
  bool active;
  int creation_tick;
} dd_flow_event_t;

typedef struct {
  dd_particle_t *particles;
  int active_count;
  dd_flow_event_t flow_events[DD_MAX_FLOW_EVENTS];
  int next_flow_index;
  double current_time;
  int last_simulated_tick;
  uint32_t rng_seed;
} dd_particle_system_t;

void dd_particles_init(dd_particle_system_t *ps);
void dd_particles_reset(dd_particle_system_t *ps);
void dd_particles_cleanup(dd_particle_system_t *ps);
void dd_particles_update_sim(dd_particle_system_t *ps, const map_data_t *map);
void dd_particles_render(dd_particle_system_t *ps, ft_game *game, int layer);
// Installs the physics' effect callbacks. Every step retains its demo events on
// `world`; visible worlds additionally spawn into their particle system.
bool dd_particles_bind(ft_game *game, ft_world *world);
// Call after the tick with the tick number from before it, and whatever
// dd_particles_bind returned.
void dd_particles_finish(ft_game *game, ft_world *world, int tick_before, bool bound);
// Advances the visible particle simulation to `tick + alpha`.
void dd_particles_advance(ft_game *game, int world_index, const ft_level *level, int tick, float alpha);
dd_particle_system_t *dd_particles_for(ft_game *game, int world_index);
void dd_particles_prune_by_time(dd_particle_system_t *ps, double min_time);

void dd_particles_create_explosion(dd_particle_system_t *ps, vec2 pos);
void dd_particles_create_smoke(dd_particle_system_t *ps, vec2 pos, vec2 vel, float alpha, float time_passed);
void dd_particles_create_bullet_trail(dd_particle_system_t *ps, vec2 pos, float alpha, float time_passed);
void dd_particles_create_player_death(dd_particle_system_t *ps, vec2 pos, vec4 blood_color);
void dd_particles_create_star(dd_particle_system_t *ps, vec2 pos);
void dd_particles_create_hammer_hit(dd_particle_system_t *ps, vec2 pos, float alpha);
void dd_particles_create_air_jump(dd_particle_system_t *ps, vec2 pos, float alpha);
void dd_particles_create_player_spawn(dd_particle_system_t *ps, vec2 pos, float alpha);
void dd_particles_create_confetti(dd_particle_system_t *ps, vec2 pos, float alpha);
void dd_particles_create_damage_ind(dd_particle_system_t *ps, vec2 pos, vec2 dir, float alpha);
void dd_particles_create_powerup_shine(dd_particle_system_t *ps, vec2 pos, vec2 size, float alpha);
void dd_particles_create_freezing_flakes(dd_particle_system_t *ps, vec2 pos, vec2 size, float alpha);

// --- DDNet demo timeline events ---------------------------------------------

typedef enum {
  DD_EVENT_CHAT = 0,
  DD_EVENT_BROADCAST,
  DD_EVENT_KILLMSG,
  DD_EVENT_SOUND_GLOBAL,
  DD_EVENT_EMOTICON,
  DD_EVENT_VOTE_SET,
  DD_EVENT_VOTE_STATUS,
  DD_EVENT_DDRACE_TIME,
  DD_EVENT_RECORD,
  DD_EVENT_COUNT
} dd_event_type_t;

#define DD_EVENT_PAYLOAD_MAGIC 0x44444556u /* "DDEV" */

typedef struct {
  uint32_t magic;
  int32_t type;
  int32_t team;
  int32_t client_id;
  char message[256];
  int32_t killer;
  int32_t victim;
  int32_t weapon;
  int32_t mode_special;
  int32_t sound_id;
  int32_t emoticon;
  int32_t vote_timeout;
  char reason[256];
  int32_t vote_yes;
  int32_t vote_no;
  int32_t vote_pass;
  int32_t vote_total;
  int32_t time;
  int32_t check;
  int32_t finish;
  int32_t server_time_best;
  int32_t player_time_best;
} dd_event_payload_t;

void dd_events_render(ft_game *game, const ft_ui_frame *frame);
bool dd_event_decode(const ft_timeline_event *event, dd_event_payload_t *out);

// --- graphics ---------------------------------------------------------------

// Instance layout of the tee shader. Must match data/games/ddnet/shaders/skin.vert.
typedef struct {
  vec2 pos;
  float scale;
  int skin_index;
  int eye_state;
  vec4 body;
  vec4 back_foot;
  vec4 front_foot;
  vec4 attach;
  vec2 dir;
  vec3 col_body;
  vec3 col_feet;
  int col_custom;
  int mode; // 0 whole tee, 1 just a hand
} dd_skin_instance_t;

enum { DD_SKIN_MODE_TEE = 0,
       DD_SKIN_MODE_HAND = 1 };

typedef struct {
  char name[64];
  bool used;
  bool loaded;
} dd_skin_slot_t;

typedef struct {
  bool ready;

  ft_texture *gameskin_texture;
  ft_texture *particles_texture;
  ft_texture *extras_texture;
  ft_atlas *gameskin;
  ft_atlas *particles;
  ft_atlas *extras;
  // The crosshair uses its own sprite table over the same sheet.
  ft_atlas *cursor;
  ft_sprite_rect cursor_rects[CURSOR_SPRITE_COUNT];
  ft_sprite_rect gameskin_rects[GAMESKIN_SPRITE_COUNT];
  ft_sprite_rect particle_rects[PARTICLE_SPRITE_COUNT];
  ft_sprite_rect extra_rects[EXTRA_SPRITE_COUNT];

  // Tee rendering: one array texture of skin sheets, one of their grayscale
  // versions for tinting, and the pipeline that composes them.
  ft_texture *skin_array;
  ft_texture *skin_color_array;
  ft_pipeline *skin_pipeline;
  dd_skin_slot_t skins[DD_MAX_SKINS];
  int default_skin;
  int ninja_skin;

  // Level rendering.
  ft_texture *entities;
  ft_pipeline *map_pipeline;
  ft_mesh *map_mesh;

  dd_skin_instance_t *skin_batch;
  uint32_t skin_batch_count;
  uint32_t skin_batch_capacity;
  // Hands batch separately: they draw at their own depth, behind the bodies.
  dd_skin_instance_t *hand_batch;
  uint32_t hand_batch_count;
  uint32_t hand_batch_capacity;
  // Hook hands sit behind weapons, while hands gripping a weapon sit in front.
  // They cannot share one z-sorted draw batch.
  dd_skin_instance_t *hook_hand_batch;
  uint32_t hook_hand_batch_count;
  uint32_t hook_hand_batch_capacity;
} dd_gfx_t;

// Presentation settings. These belong to the game, not the editor, which is why
// they moved out of the engine's ui_handler_t.
typedef struct {
  bool render_players;
  bool render_weapons;
  bool render_particles;
  bool render_pickups;
  bool render_cursor_follow;
  bool center_dot;
  float cursor_scale;
} dd_settings_t;

typedef struct {
  bool enabled;
  int track_count;
  bool *tracks;
  int32_t *pings;
} dd_demo_export_world_t;

struct ft_game {
  const ft_engine_api *engine;
  ft_level *current_level;
  dd_gfx_t gfx;
  dd_settings_t settings;
  // One particle system per world the engine shows, indexed by world_index.
  dd_particle_system_t *particles;
  int particle_count;
  bool headless;

  // The start screen this game shows before a run: its map browser. Held by
  // pointer so this header stays free of the browser's own types.
  struct online_map_manager_t *maps;
  struct dd_skin_browser_t *skin_browser;
  bool show_skin_browser;
  bool show_events;
  bool auto_finish_events;

  // DDNet owns its demo configuration: ranges, group/track selection and
  // protocol-specific ping values never enter the game-independent editor.
  bool open_demo_export;
  int demo_export_start_tick;
  int demo_export_end_tick;
  char demo_export_error[160];
  dd_demo_export_world_t *demo_export_worlds;
  int demo_export_world_count;
  bool preserve_demo_export_on_level_load;
};

void dd_log(ft_game *game, ft_log_level level, const char *fmt, ...);

bool dd_gfx_create(ft_game *game);
void dd_gfx_destroy(ft_game *game);
// Resolves a skin name to a layer in the skin array, loading it on demand.
// Returns the default skin's layer when the named one cannot be loaded.
int dd_gfx_skin_index(ft_game *game, const char *name);
// Loads a skin chosen by the DDNet browser from an arbitrary local path.
int dd_gfx_load_skin_path(ft_game *game, const char *name, const char *path);
// Renders one browser thumbnail through the same pipeline and texture arrays
// used by the in-game tee renderer.
bool dd_gfx_render_skin_preview(ft_game *game, const char *name, const char *path, ft_texture *destination, uint32_t destination_x,
                                uint32_t destination_y);

void dd_skin_browser_render(ft_game *game, const ft_ui_frame *frame);
void dd_skin_browser_cleanup(ft_game *game);
bool dd_demo_export(ft_game *game, const ft_export_request *request);
bool dd_demo_export_with_pings(ft_game *game, const ft_export_request *request, const int32_t *player_pings);
void dd_export_window_open(ft_game *game);
void dd_export_window_render(ft_game *game);
void dd_export_window_cleanup(ft_game *game);
size_t dd_export_project_save(ft_game *game, void *out, size_t out_size);
bool dd_export_project_load(ft_game *game, const void *data, size_t size);

void dd_draw_sprite(ft_game *game, ft_atlas *atlas, float z, vec2 pos, vec2 size, float rotation, uint32_t sprite, vec4 color);
void dd_draw_sprites(ft_game *game, ft_atlas *atlas, float z, const ft_sprite_draw *draws, uint32_t count);
void dd_draw_line(ft_game *game, float z, vec2 a, vec2 b, vec4 color, float thickness);
void dd_draw_circle(ft_game *game, float z, vec2 center, float radius, vec4 color, uint32_t segments);
void dd_draw_triangle(ft_game *game, float z, vec2 a, vec2 b, vec2 c, vec4 color);

void dd_skins_begin(ft_game *game);
void dd_skin_push(ft_game *game, vec2 pos, float scale, int skin, int eye, vec2 dir, const dd_anim_state_t *anim, vec3 col_body, vec3 col_feet,
                  bool custom);
void dd_hand_push(ft_game *game, vec2 pos, float scale, int skin, float angle, vec3 col_body, bool custom, bool hook_hand);
void dd_skins_flush(ft_game *game);

// Retrieves the sprite rectangle a draw needs for its aspect ratio.
const ft_sprite_rect *dd_sprite_rect(ft_game *game, ft_atlas *atlas, uint32_t index);

void dd_render(ft_game *game, const ft_render_frame *frame);

enum { DD_CAMERA_FREE = 0,
       DD_CAMERA_FOLLOW,
       DD_CAMERA_MODE_COUNT };
extern const ft_camera_mode dd_camera_modes[DD_CAMERA_MODE_COUNT];
bool dd_camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout);
bool dd_player_label(ft_game *game, const ft_world *world, int32_t player, char *out, size_t out_size);
uint32_t dd_status_lines(ft_game *game, const ft_world *world, int32_t player, float alpha, char *out, uint32_t max_lines,
                         uint32_t line_size);
void dd_level_build_pickups(ft_level *level);

void dd_map_create(ft_game *game, ft_level *level);
void dd_map_destroy(ft_game *game, ft_level *level);
void dd_map_render(ft_game *game, const ft_render_frame *frame);

// PNG decoding lives with the other graphics code so only one translation unit
// instantiates stb_image.
unsigned char *dd_decode_png(const void *data, size_t size, int *out_w, int *out_h, int *out_channels);
void dd_free_png(unsigned char *pixels);

#endif // DD_INTERNAL_H
