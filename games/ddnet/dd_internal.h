// Shared internals of the DDNet game module.
//
// Nothing in here crosses the ABI. The engine only ever sees the opaque
// ft_game / ft_level / ft_world handles declared in <frametee/game_abi.h>; this
// header is what the module's own translation units share with each other.

#ifndef DD_INTERNAL_H
#define DD_INTERNAL_H

#include "include/ddnet/ddnet_game.h"

#include <ddnet_physics/gamecore.h>
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

// DDNet physics works in pixels, 32 to a tile. The editor's world units are
// tiles, so everything crossing the ABI is scaled by this.
#define PX_PER_TILE 32.0f

// Draw order inside the module's own passes. The engine sorts every command in
// the frame by this float, whichever pass produced it, so these bands are what
// actually decides the picture. They follow the DDNet client's component order
// in gameclient.cpp: m_Items (pickups, projectiles, lasers, doors) below
// m_Players (hook, weapon, tee), then m_MapLayersForeground -- the game layer
// and its entity overlays, which is why the map sits above the tees -- then the
// explosions and the nameplates.
#define DD_Z_PICKUPS 1.f
#define DD_Z_PARTICLES_BACK 2.f
#define DD_Z_LASERS 2.5f
#define DD_Z_HOOK 3.f
#define DD_Z_PROJECTILES 4.f
#define DD_Z_HOOK_HAND 4.5f
#define DD_Z_WEAPONS 5.f
#define DD_Z_WEAPON_HAND 5.5f
#define DD_Z_SKINS 6.f
#define DD_Z_MAP 7.0f
#define DD_Z_MAP_TELE_TEXT 7.1f
#define DD_Z_MAP_SPEEDUP 7.2f
#define DD_Z_MAP_SPEEDUP_TEXT 7.3f
#define DD_Z_MAP_SWITCH_TEXT 7.4f
#define DD_Z_PARTICLES_FRONT 8.0f
// The editor's own markers, which belong on top of the game whatever it drew.
#define DD_Z_LINES 9.0f
#define DD_Z_OVERLAYS 10.0f
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
typedef struct dd_physics_particle_event {
  float x;
  float y;
  int type;
  int client_id;
} dd_physics_particle_event_t;

typedef struct dd_physics_damage_event {
  float x;
  float y;
  float angle;
  int amount;
  int client_id;
} dd_physics_damage_event_t;

typedef struct dd_physics_sound_event {
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

enum { DD_EMOTICON_COUNT = 16 };

// DDNet's hud.png freeze bar sprites, followed by horizontally mirrored copies
// of the same four. CFreezeBars draws half its pieces with a reversed texture
// subset; the sprite path has no free UV subset, so the mirror is baked into
// the sheet and selected by sprite index instead.
enum { DD_FREEZE_FULL_LEFT = 0,
       DD_FREEZE_FULL,
       DD_FREEZE_EMPTY,
       DD_FREEZE_EMPTY_RIGHT,
       DD_FREEZE_MIRRORED = 4,
       DD_FREEZE_SPRITE_COUNT = 8 };

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
void dd_particles_update_sim(dd_particle_system_t *ps, SCollision *collision);
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
void dd_particles_rewind_to_tick(dd_particle_system_t *ps, int replay_tick);

void dd_particles_create_explosion(dd_particle_system_t *ps, SCollision *collision, vec2 pos);
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
void dd_events_scan_recording(ft_game *game, const ft_ui_frame *frame);
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
  // Fades the whole instance. The shader's output is premultiplied, so this
  // scales colour and coverage together.
  float alpha;
} dd_skin_instance_t;

enum { DD_SKIN_MODE_TEE = 0,
       DD_SKIN_MODE_HAND = 1 };

typedef struct {
  char name[64];
  bool used;
  bool loaded;
} dd_skin_slot_t;

// DDNet text is rendered by the game module, through the same atlas path as
// its other sprites. This keeps it inside the viewport render target and gives
// it exactly the same camera transform and clipping as the map and players.
// DDNet keeps the fill and the outline in two separate textures and draws the
// string twice, which is what lets it colour the outline independently. The
// same split here, plus one dilated variant per outline thickness so the ring
// can also match the size DDNet would have baked for a given screen size.
enum { DD_TEXT_OUTLINE_VARIANTS = 4 };

typedef struct {
  uint32_t codepoint;
  uint32_t glyph_index;
  uint32_t sprite_index;
  uint32_t outline_sprite[DD_TEXT_OUTLINE_VARIANTS];
  uint32_t width;
  uint32_t height;
  float offset_x;
  float offset_y;
  float advance_x;
  bool visible;
} dd_text_glyph_t;

typedef struct {
  void *library;
  void *face;
  void *font_data;
  ft_texture *source_texture;
  ft_atlas *atlas;
  ft_texture *entity_source_textures[3];
  ft_atlas *entity_atlases[3];
  dd_text_glyph_t *glyphs;
  uint32_t glyph_count;
  float baked_size;
  // cl_text_entities_size, as a percentage. The entity number sheets bake it
  // in, so changing it re-uploads them exactly as CMapImages does.
  int entity_scale;
} dd_text_renderer_t;

typedef struct {
  bool ready;

  ft_texture *gameskin_texture;
  ft_texture *particles_texture;
  ft_texture *extras_texture;
  ft_texture *emoticons_texture;
  ft_texture *speedup_arrow_texture;
  ft_texture *freeze_bar_texture;
  ft_atlas *gameskin;
  ft_atlas *particles;
  ft_atlas *extras;
  ft_atlas *emoticons;
  // DDNet's editor/speed_arrow.png: a single sprite covering the whole sheet.
  ft_atlas *speedup_arrow;
  ft_atlas *freeze_bar;
  // The crosshair uses its own sprite table over the same sheet.
  ft_atlas *cursor;
  dd_text_renderer_t text;
  ft_sprite_rect cursor_rects[CURSOR_SPRITE_COUNT];
  ft_sprite_rect gameskin_rects[GAMESKIN_SPRITE_COUNT];
  ft_sprite_rect particle_rects[PARTICLE_SPRITE_COUNT];
  ft_sprite_rect extra_rects[EXTRA_SPRITE_COUNT];
  ft_sprite_rect emoticon_rects[DD_EMOTICON_COUNT];
  ft_sprite_rect speedup_arrow_rect;
  ft_sprite_rect freeze_bar_rects[DD_FREEZE_SPRITE_COUNT];

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
  // Tees drawn as part of the HUD rather than the world -- the chat's avatars.
  // They need their own batch because a batch carries one z for all of it, and
  // theirs has to clear the map and the overlays the world tees sit under.
  dd_skin_instance_t *overlay_batch;
  uint32_t overlay_batch_count;
  uint32_t overlay_batch_capacity;
} dd_gfx_t;

// Presentation settings. These belong to the game, not the editor, which is why
// they moved out of the engine's ui_handler_t.
typedef struct {
  // The map's own tile layers. Every other visibility toggle below already
  // belongs to the game; what counts as "the level" does too, since only the
  // game knows what it is made of.
  bool render_map;
  bool render_players;
  bool render_weapons;
  bool render_particles;
  bool render_pickups;
  bool render_cursor_follow;
  bool render_chat;
  // cl_chat_size and cl_chat_width. DDNet keeps width/size at or above
  // CHAT_FONTSIZE_WIDTH_RATIO so the two never fight each other.
  int chat_font_size;
  int chat_width;
  bool render_nameplates;
  bool render_emoticons;
  bool render_freeze_bars;
  bool render_entity_text;
  // DDNet's cl_text_entities_size, 20 to 100 percent.
  int entity_text_size;
  bool render_speedups;
  bool render_doors;
  // The cl_nameplates_* family, in DDNet's units: the two sizes are the
  // percentages a font size is derived from and the offset is world pixels.
  int nameplate_size;
  bool nameplate_clan;
  int nameplate_clan_size;
  int nameplate_offset;
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
  struct dd_skin_browser_t *skin_browser;
  struct dd_player_panel_t *player_panel;
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
bool dd_text_create(ft_game *game);
void dd_text_destroy(ft_game *game);
float dd_text_width(ft_game *game, float size, const char *text);
void dd_text_draw(ft_game *game, float z, ft_vec2 position, float size, ft_color color, const char *text);
// The full form: an outline colour of its own, and the on-screen pixel size the
// outline thickness should be chosen for. DDNet picks that from the size it
// bakes a glyph at -- the real screen size for HUD text like chat, and a fixed
// interface size for world text like nameplates, which is why it is a separate
// argument rather than derived from `size`.
void dd_text_draw_outlined(ft_game *game, float z, ft_vec2 position, float size, ft_color color, ft_color outline,
                           float outline_reference_px, const char *text);
enum { DD_ENTITY_TEXT_TOP = 0,
       DD_ENTITY_TEXT_CENTER,
       DD_ENTITY_TEXT_BOTTOM,
       DD_ENTITY_TEXT_STYLE_COUNT };
void dd_entity_text_draw(ft_game *game, float z, int style, int x, int y, int value, ft_color color);
// Re-bakes the entity number sheets when cl_text_entities_size changes.
void dd_text_set_entity_scale(ft_game *game, int scale);
// Resolves a skin name to a layer in the skin array, loading it on demand.
// Returns the default skin's layer when the named one cannot be loaded.
int dd_gfx_skin_index(ft_game *game, const char *name);
// Loads a skin chosen by the DDNet browser from an arbitrary local path.
int dd_gfx_load_skin_path(ft_game *game, const char *name, const char *path);
// Where a skin called `name` lives, in the order this module looks: its cache,
// the skins shipped beside the game, then the client's own directories.
bool dd_gfx_find_skin_file(ft_game *game, const char *name, char *out, size_t out_size);
// Optional tinting for a thumbnail: the colours a particular tee wears, or NULL
// for the plain skin as it comes.
typedef struct dd_skin_colors_t {
  vec3 body;
  vec3 feet;
  bool custom;
} dd_skin_colors_t;

// Renders one thumbnail through the same pipeline and texture arrays used by
// the in-game tee renderer.
bool dd_gfx_render_skin_preview(ft_game *game, const char *name, const char *path, const dd_skin_colors_t *colors,
                                ft_texture *destination, uint32_t destination_x, uint32_t destination_y);

void dd_skin_browser_render(ft_game *game, const ft_ui_frame *frame);
void dd_skin_browser_cleanup(ft_game *game);
// Downloads a skin by name into the module's cache, or reports the copy that is
// already there. Shared with the player panel, which fetches the same way.
bool dd_skin_fetch(ft_game *game, const char *name, char *out_path, size_t out_size);

// The panel this game draws for the selected player: nickname, clan, skin and
// tee colours. What a player can be customised into is DDNet's own business, so
// the window belongs to the module rather than to the editor.
void dd_player_panel_render(ft_game *game, const ft_ui_frame *frame);
void dd_player_panel_cleanup(ft_game *game);
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
// A tee in HUD space: an explicit alpha, and its own flush so it can be drawn
// from a pass that runs after the world's tees have already gone out.
void dd_skin_push_overlay(ft_game *game, vec2 pos, float scale, int skin, int eye, vec2 dir, const dd_anim_state_t *anim,
                          vec3 col_body, vec3 col_feet, bool custom, float alpha);
void dd_skins_flush_overlay(ft_game *game);

// Retrieves the sprite rectangle a draw needs for its aspect ratio.
const ft_sprite_rect *dd_sprite_rect(ft_game *game, ft_atlas *atlas, uint32_t index);

void dd_render(ft_game *game, const ft_render_frame *frame);
void dd_render_world_overlays(ft_game *game, const ft_render_frame *frame);
void dd_render_map_overlays(ft_game *game, const ft_render_frame *frame);
void dd_render_doors(ft_game *game, const ft_render_frame *frame);

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
