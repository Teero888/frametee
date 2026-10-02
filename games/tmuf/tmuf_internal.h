#ifndef TMUF_INTERNAL_H
#define TMUF_INTERNAL_H

// TrackMania United Forever as a FrameTee game module.
//
// The physics is libs/tmuf_physics: a world here is a tmuf_world, stepped with
// the game's own inputs. The library also hands over what the game draws as
// plain data (tmuf_track_visuals); turning that into pictures on screen is
// this module's job (tm_scene.c).

#include <frametee/game_abi.h>
#include <tmuf_physics/tmuf_physics.h>

#include "tmuf_gpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- input record --------------------------------------------------------

// The game's own inputs, as tmuf_input takes them.
typedef struct tm_input {
  uint8_t accelerate;
  uint8_t brake;
  uint8_t respawn;
  // A key event that leaves the input as it was (a second steering key):
  // imported from replays, where it cancels a master jump.
  uint8_t input_event;
  int32_t steer; // -65536 (full left) .. 65536 (full right)
} tm_input;

enum tm_field {
  TM_FIELD_ACCELERATE,
  TM_FIELD_BRAKE,
  TM_FIELD_STEER,
  TM_FIELD_RESPAWN,
  TM_FIELD_INPUT_EVENT,
  TM_FIELD_COUNT,
};

// --- player profiles ------------------------------------------------------

// A track's profile (ft_player_setup): the skin its car wears, as the game
// names skin packs ("Skins\\Vehicles\\StadiumCar\\FRA.zip", under GameData);
// "" for the car's own.
#define TM_PROFILE_MAGIC "TMP1"
#define TM_GROUPS_MAX 64 // the timeline groups whose cars are drawn
#define TM_SHADOW_CARS 8 // the cars with shadows, marks and particles: the active one and other shown ones
typedef struct tm_profile {
  char magic[4];
  char skin[256];
  char name[64]; // the nickname exported replays carry (older profiles end before it)
} tm_profile;
// the skin of player `player` in these setups, "" when none
const char *tm_profile_skin(const ft_player_setup *setups, uint32_t count, int32_t player);
// the name of player `player` in these setups, "" when none
const char *tm_profile_name(const ft_player_setup *setups, uint32_t count, int32_t player);
// Where skin packs are looked for, as the game stores their paths
// ("Skins\\Vehicles\\<car>\\<pack>.zip"): under GameData (the stock ones),
// then under a player's TrackMania documents (their own): the module's
// Documents folder beside GameData, ~/Documents/TrackMania and Wine's.
// Fills roots (TM_SKIN_ROOTS of them); the count. [0] is GameData's.
#define TM_SKIN_ROOTS 4
uint32_t tm_skin_roots(ft_game *game, char roots[TM_SKIN_ROOTS][1024]);

// --- levels and worlds -----------------------------------------------------

typedef struct tm_scene tm_scene;   // the level on the GPU (tm_scene.c)
typedef struct tm_car_model tm_car_model; // the car on the GPU (tm_car.c)

// The light of the map's time of day (tmuf_weather.c): the weather's
// shader constants
typedef struct tm_light {
  float sun_dir[3];              // GbxLightDirDir0: where the light goes
  float sun_rgb[3];              // GbxLightDirRgb0 (the moon's at night)
  float sun_rgb_double_sided[3]; // GbxLightDirRgbDblSided0
  float ambient[4];              // GbxLightAmbient
  float clouds_min[3], clouds_max[3]; // GbxCloudsRgbMin/Max
  float fog_rgb[3], fog_start, fog_end;
  float day_time[2]; // GbxDayTime.xy: the clock, the remapped time
  float spec_intensity, spec_power;
  bool moon;
  float shadow_car_intensity; // the mood's ShadowCarIntensity
  float clouds_scale[2], clouds_speed[2], clouds_offset[2], clouds_period;
  // Day and Sunrise: the lightmaps hold the sky's light and the sun's
  // visibility (r, g), read as GbxLightGenP_ScaleAD_TransDA and
  // GbxLightFromMap_ScaleAD_TransDA say (y 0: Sunset, Night: colours)
  float lightgen[4], light_from_map[4];
} tm_light;

struct ft_level {
  char name[256];
  tm_light light;
  tmuf_track *track;
  void *map; // the .Challenge.Gbx bytes, for exporting replays
  size_t map_size;
  tmuf_challenge_info *info; // what the map's header says (medal times, author), NULL if unread
  struct tm_level_audio *audio; // its sounds' samples, NULL when nothing is heard (tmuf_audio.c)
  float bounds_min[3], bounds_max[3]; // of everything drawn
  tm_scene *scene;                  // built on first draw, NULL headless
  tm_car_model *car;                // likewise
  // per timeline group (its index): the wheels' marks, the car's smoke and
  // spray, made with the car
  struct tm_marks *marks[TM_GROUPS_MAX];
  struct tm_particles *particles[TM_GROUPS_MAX];
  struct tm_leaves *leaves; // Rally's falling leaves (tmuf_leaves.c)
  bool leaves_tried;
};

// What the race interface shows of the car, kept tick by tick
// (CTrackManiaRace::UpdateAsync, InputRace; tmuf_hud.c)
typedef struct tm_race_display {
  float dist;          // km/h x ms driven (race+0x264)
  uint32_t speed;      // Race.CarSpeedDisplay, km/h
  uint32_t distance;   // Race.CarDistanceDisplay, m
} tm_race_display;

// What the car's sound needs of a step's state, before and after it
// (tmuf_audio.c): the game reads the ticks around its frame
typedef struct tm_audio_frame {
  uint32_t tick;
  float forward_speed, rpm, gas, brake, local_vz, speed;
  int reverse, gear, engine_state, input_window_exceeded, free_wheeling, in_water, burnout;
  uint32_t wheel_count;
  uint8_t wheel_contact[TMUF_CAR_MAX_WHEELS], wheel_slipping[TMUF_CAR_MAX_WHEELS];
  uint8_t wheel_material[TMUF_CAR_MAX_WHEELS], wheel_front[TMUF_CAR_MAX_WHEELS];
  uint8_t last_wheel_material, last_body_material;
  uint32_t turbo_start;
  int splashes;
  uint32_t checkpoints;
  int completed;
  uint32_t stunt_score;
} tm_audio_frame;

// The car's sound state, carried from step to step while the world is heard
// (tmuf_world_set_sound), and the sound of the last step
#define TM_AUDIO_SOUNDS 128
typedef struct tm_audio {
  uint32_t tick; // the world's tick it was made for; any other: stale
  uint8_t wheel_material; // the last wheel material (Water while spraying, after a splash)
  // the surface sounds' (rear, front, body) rolling and skid loops' gains,
  // per distinct sample
#define TM_SURFACE_LOOPS 32
  float surface[3][TM_SURFACE_LOOPS];
  // when the car's one-shots that do not restart while playing end, in ticks
  uint32_t busy_until[TMUF_CAR_SOUND_COUNT];
  ft_audio_sample busy_sample[TMUF_CAR_SOUND_COUNT];
  // the woosh's volume, and what its last probe found
  float woosh, woosh_target;
  // the last heard step's sound (each world's own, made when it is first
  // heard): the tick it is for
  uint32_t heard_tick;
  uint32_t count;
  ft_audio_sound *sounds; // TM_AUDIO_SOUNDS
} tm_audio;

struct ft_world {
  tmuf_world w;
  tm_race_display display;
  const ft_level *level;
  tm_audio audio;
};

// --- the game --------------------------------------------------------------

typedef struct tm_settings {
  bool draw_track;
  bool draw_car;
  bool near_detail; // draw the detail the game shows close by (grass tufts...)
  int antialiasing; // samples per pixel (the game's Antialiasing: 1..16)
  bool bloom;       // the game's post effects (PostFxEnable: the bloom)
  bool shadows;     // the car's shadow (Shadows)
  bool flares;      // the lights' lens flares
  bool water_reflection; // the sea's reflection (the game makes it when the card has 2x multisampling)
  bool headlights;       // the car's headlight lighting the scenery at night (TmCarProjector)
  bool marks;            // the wheels' marks on the ground
  bool particles;        // the car's smoke, gravel and water spray
  int anisotropy;   // texture filtering: the game's FilterAnisoQ as samples (1: trilinear .. 16)
  bool countdown;   // the HUD's countdown at the start (the race camera only)
  bool race_time;   // the HUD's race time at the bottom (the race camera only)
  bool speed;       // the HUD's speed at the bottom right (the race camera only)
  bool challenge_info; // the HUD's card with the map's name and author, top right
  bool splits;         // the HUD's checkpoint time and its difference to the best run
  bool music;          // the race music
  int music_volume;    // its volume, percent
  int sfx_volume;      // everything else's, percent
} tm_settings;

// A car's headlight projector as posed this frame (its night-only
// GxLightFrustum, night_island_spec.md §5): where it is, its axes in the
// world (x left, y up, z ahead), its frustum (x min, y min, near, x max,
// y max, far: extents at distance 1), intensity, colour and picture
typedef struct tm_projector {
  float pos[3], axes[3][3];
  float frustum[6];
  float intensity, rgb[3];
  tg_texture picture;
} tm_projector;

struct ft_game {
  const ft_engine_api *engine;
  tmuf_packs *packs;
  ft_level *level; // the level loaded last
  tm_settings settings;
  // drawing (tmuf_gpu.h): the scene into `frame`, which the engine shows
  tg *gpu;
  tg_program *track_program;
  tg_program *grass_program; // the grass marks (tmuf_marks.c)
  tg_program *block_program; // the game's normal-mapped blocks (shaders/block.*)
  tg_program *track_depth_program, *block_depth_program; // their vertex shaders alone: the depth prepass
  tg_program *fence_program; // grass tufts (shaders/fence.*)
  tg_program *water_program; // the sea (shaders/water.*)
  tg_program *car_program;   // the car's skin (shaders/car.*)
  tg_program *hud_program;   // pictures on screen (shaders/hud.*)
  tg_program *post_program;  // the post effects (shaders/post.frag, tmuf_post.c)
  tg_program *clouds_program; // the sky's 3D clouds (shaders/clouds.*, tmuf_clouds.c)
  struct tm_shadow *shadow;   // the car's shadow (tmuf_shadow.c)
  struct tm_pssm *pssm;       // the sun's shadow maps (tmuf_pssm.c)
  struct tm_ssao *ssao;       // the ambient occlusion (tmuf_ssao.c)
  struct tm_flares *flares;   // the lights' flares (tmuf_flares.c)
  struct tm_bake *bake;       // the lightmap bake's programs (tmuf_bake.c)
  struct tg_target *bake_leftover[5]; // its work targets, freed after the frame
  // the timeline's groups shown (the engine's entity pass of the last frame:
  // the module draws its frame in the level pass, which has only the active
  // group's world): their indices and opacities, and the list being made
  struct {
    uint32_t count;
    int32_t group[TM_GROUPS_MAX];
    float opacity[TM_GROUPS_MAX];
  } shown, shown_next;
  // the car's headlight this frame, lighting the scenery (tm_car_projector)
  // the view is the car's inside (a MediaTracker clip's Internal camera): the
  // followed car is not drawn (tm_camera_update)
  bool camera_inside;
  bool has_projector;
  tm_projector projector;
  // the wheels' marks the scene draws this frame (tmuf_marks.c): each shown
  // group's, with that car's frame
  struct {
    struct tm_marks *marks;
    ft_render_frame frame;
  } marks_drawn[TM_SHADOW_CARS];
  uint32_t marks_drawn_count;
  // the car whose LightFromMap tints the marks and particles being emitted
  // (the active one's, NULL for the other groups')
  const struct tm_car_model *tint_car;
  tg_program *lfm_block_program, *lfm_track_program; // the car's LightFromMap (shaders/lfm.*)
  tg_target *bloom[3];       // the bright parts, full size; then a quarter size, twice
  tg_texture countdown;      // 321Go.dds
  tg_texture led_font;       // Led_00.dds: the race time's font
  // the best run's checkpoint times the splits compare with (tmuf_hud.c):
  // for the shown world at its checkpoint count, the fastest finished run of
  // the timeline's other worlds (count 0: none)
  struct {
    const ft_world *world;
    uint32_t checkpoints;
    uint32_t count;
    uint32_t *times;
  } split_best;
  tg_texture united_pages[4]; // United_0<n>.dds: the interface's own letters (pages 0, 1, 3)
  tg_texture race_time_back; // UiBgBottomCenterRace.dds
  tg_texture speed_back;     // UiBgCard.dds
  // sound (tmuf_audio.c): the samples loaded, by file, and where the
  // active view hears from (the camera's eye and right axis)
  struct tm_audio_bank *audio_bank;
  struct {
    bool valid;
    float eye[3], right[3];
  } listener;
  ft_texture *frame;
  uint32_t frame_width, frame_height;
  ft_pipeline *present;
  ft_mesh *quad;
};

// tmuf_audio.c: the car's, the race's and the ambience's sound
void tm_audio_level_load(ft_game *game, ft_level *level);
void tm_audio_level_free(ft_game *game, ft_level *level);
void tm_audio_free(ft_game *game);
// before a step: whether there is sound (switching the world's sound on or
// off), and if so the state it starts from
bool tm_audio_prepare(ft_game *game, ft_world *world, tm_audio_frame *before);
// after it: the sound state goes on, and a heard step's sound is made
void tm_audio_step(ft_game *game, ft_world *world, const tm_audio_frame *before);
void tm_audio_copy(ft_world *dst, const ft_world *src);
void tm_audio_world_init(ft_world *world);
void tm_audio_world_free(ft_world *world);
// the horn's presses of a replay, as timeline events
#define TM_EVENT_HORN "horn"
bool tm_event_audio(ft_game *game, const ft_timeline_event *event, ft_audio_sound *out);
void tm_recording_events(ft_game *game, const ft_recording *recording, const int32_t *world_players,
                         uint32_t player_count, void (*emit)(void *user, const ft_timeline_event *event), void *user);
bool tm_world_audio(ft_game *game, const ft_world *world, ft_audio_step *out);
void tm_audio_spatialize(ft_game *game, int32_t world_index, const ft_audio_sound *sound, float gain[2]);
void tm_audio_listen(ft_game *game, const ft_camera *camera);

void tm_log(const ft_game *game, ft_log_level level, const char *fmt, ...);
// TM_PROFILE: a stage boundary named `name` (a string literal): the CPU time
// to the next one and the GPU's (tg_mark), averaged over 120 frames to stderr
void tm_prof(ft_game *game, const char *name);
// the frame's last boundary; reports every 120 frames
void tm_prof_frame_end(ft_game *game);
// the module's read-backs (tg_target_read_queue keys)
enum { TM_READ_FLARES = 1, TM_READ_LFM = 2 };

// tm_scene.c: the track on the GPU
tm_scene *tm_scene_create(ft_game *game, ft_level *level);
void tm_scene_destroy(ft_game *game, tm_scene *scene);
void tm_scene_render(ft_game *game, tm_scene *scene, const ft_render_frame *frame);
bool tm_track_resources_create(ft_game *game);
void tm_track_resources_destroy(ft_game *game);

// One texture per image file, loaded the first time a picture uses it.
// A video picture (Bink), all its frames (tmuf_video.c)
typedef struct tm_video {
  tg_texture *frames;
  uint32_t count;
  float fps;
} tm_video;
bool tm_video_load(ft_game *game, const char *path, tm_video *out);
void tm_video_free(ft_game *game, tm_video *video);
// the frame shown `seconds` into the looped video
tg_texture tm_video_frame(const tm_video *video, double seconds);

typedef struct tm_picture_table {
  uint32_t count, cap;
  char **keys; // the file or pack path
  tg_texture *textures; // 0 when it could not be loaded
  tm_video *videos;     // per entry: its frames when it is a video (count 0 otherwise)
  char mood_dir[1024]; // GameData/<environment>/Media/Moods/<mood>/, "" if none
  const tmuf_weather *weather; // its mood's skin: pictures in place of the collection's
} tm_picture_table;
void tm_picture_table_init(ft_game *game, tm_picture_table *table, const tmuf_track *track);
// The mood's light: the sun's and the ambient colour (LightSun.tga,
// LightAmbient.tga; white sun and grey ambient when they are no flat colour)
void tm_picture_table_free(ft_game *game, tm_picture_table *table);
// the texture index of a material's picture in table, UINT32_MAX for none
uint32_t tm_picture_index(ft_game *game, tm_picture_table *table, const tmuf_visual_material *m);
// the texture of an entry at a time: a video's frame, else the picture
tg_texture tm_picture_at(const tm_picture_table *table, uint32_t index, double seconds);
// A sampler the mood fills in (the sky's gradient and panorama): which one a
// material draws with, TM_MOOD_NONE for none.
typedef enum tm_mood_slot { TM_MOOD_NONE, TM_MOOD_GRADIENT, TM_MOOD_PANORAMA } tm_mood_slot;
tm_mood_slot tm_mood_slot_of(const tmuf_visual_material *m);
// a material that only glows (its one picture is "Glow", added to what is
// behind it: lamps, start lights)
bool tm_is_glow(const tmuf_visual_material *m);
// the texture of a material that is drawn (its picture), NULL for none
const tmuf_visual_texture *tm_picture_texture(const tmuf_visual_material *m);
// where texture t is sampled on a mesh's vertex at world position `world`
void tm_texcoord(const tmuf_visual_texture *t, const tmuf_visual_mesh *m, uint32_t vertex, const float world[3],
                 float out[2]);
// where a material's picture is cut out by its alpha (0: never)
float tm_alpha_cutoff(const tmuf_visual_material *m);

// Must match shaders/track.vert and track.frag.
#define TM_PSSM_CASCADES 5

typedef struct tm_track_uniforms {
  float view_proj[16];
  float lod_bias;
  float opacity;
  float alpha_cutoff;
  float blend;          // 1: Blend1/Blend2/BlendI
  float gen[4];         // world axes of Blend2 (x, y) and BlendI (z, w) coordinates
  float blend2[8];      // Blend2 transform: a b c d, tx ty, -, -
  float blend_mask[8];  // BlendI transform
  float color_scale[4]; // vertex colour scale, whether to use it, whether there is an occlusion map
  float stripe_gen[4];  // world axes of the Stripe coordinates, whether there is one
  float stripe[8];      // Stripe transform
  float blend3[4];      // x: 1 when the ground has a third layer (Blend3, over the others by its alpha)
  float water[4];       // the sea: height, whether there is one, time of day, 1 / WaterDepthMax
  float eye[4];
  float horizon0[4], horizon1[4]; // the sky's horizon clouds' colours (horizon0[3]: whether there are some)
  float fog[4], fog_color[4];     // linear fog (fog_color[3]: whether the draw is fogged)
  float clouds_u[4], clouds_v[4];
  float lawn[4]; // the Stadium's lawn: its light (1 lightmap, 2 Lighting map), factor, occlusion
  float lightgen[4]; // GbxLightGenP_ScaleAD_TransDA; y 0 when the lightmap holds colours
  float light_dir[4], light_rgb[4]; // the sun (GbxLightDirDir0, GbxLightDirRgb0)
  float scenery[4];   // the vertex-lit scenery: x 1 when lit so, y, z GbxPrelight_ScaleTrans, w 1 double sided
  float light_dbl[4]; // GbxLightDirRgbDblSided0
  // a second picture (track.frag): x what it is (TM_EXTRA_*), y the
  // scenery's shader family (TM_KIND_*); its coordinates' 2x3 transform
  float extra[4];
  float extra_t[8];
  // the car's headlight (tm_projector): its position (w: 1 / far^2), its
  // axes (x, y: w the frustum's centre at distance 1), and x, y: 1 / the
  // frustum's half extents, z: its intensity (DeferedScale), w: how this
  // draw receives it (0 not, 1 lit by N.L, 2 alpha tested: without)
  float proj_pos[4], proj_x[4], proj_y[4], proj_z[4], proj[4];
  // the sun's shadow maps (tmuf_pssm.c): the static map's rows (world ->
  // its coordinates and depth), each cascade's rows, cell in the atlas and
  // split (x its far view depth, y 1 / its fade, z 1 when used); the
  // camera's forward axis, w 1 when the draw receives the shadows
  float pssm_static[3][4];
  float pssm_cascade[TM_PSSM_CASCADES][3][4];
  float pssm_cell[TM_PSSM_CASCADES][4];
  float pssm_split[TM_PSSM_CASCADES][4];
  float pssm_view[4];
  // the ambient occlusion (tmuf_ssao.c): x 1 when the draw receives it, y z
  // 1 / the view's size; its MidGray
  float ao[4], ao_mid_gray[4];
  // the vertices' frame in the world (a car part's), column major; [15]
  // 0: the vertices are in the world
  float to_world[16];
  // the scenery's specular lobe around the sun (the generated SpecularCubeL
  // of the "CSpecL" shaders: Bay's buildings): x 1 when the draw has one;
  // y 1: the "Spec FCOut" shaders (Bay's far city: EnvCubic by the Fresnel)
  float spec_cube[4];
} tm_track_uniforms;
#define TM_TRACK_TEXTURES 16 // the track program's pictures

// the track program's second picture (tm_track_uniforms.extra[0])
enum {
  TM_EXTRA_NONE,
  TM_EXTRA_SELF_ILLUM, // SelfIllum on the Diffuse's coordinates (the family adds it)
  TM_EXTRA_GLOW,       // a Glow added (the start lights': its row by the countdown)
  TM_EXTRA_SEQUENCER,  // the chasing light signs: the Glow times the scrolling Anim
  TM_EXTRA_CEILING,    // the sky: its ceiling (the stars) under the panorama by its alpha
  TM_EXTRA_BORDERS,    // a ground's Borders over its Grass by the Borders' alpha (Island's beaches)
};
// the vertex-lit scenery's pixel shader families (tm_track_uniforms.extra[1])
enum {
  TM_KIND_X2,       // 2 clouds D light (Diff X2 ..., DiffG X2 ...)
  TM_KIND_SELF_I,   // D light + SelfIllum (DiffG_SelfI CSpecL PC3)
  TM_KIND_T_SELF_I, // 2 clouds D, unlit (TSelfI PX2 PC3)
  TM_KIND_SELF_I_X2 // 2 clouds (D light + SelfIllum) (DiffG_SelfI X2 ...)
};

// Must match shaders/water.*.
typedef struct tm_water_vertex {
  float pos[3];
  uint32_t color; // BGRA
} tm_water_vertex;

typedef struct tm_water_uniforms {
  float view_proj[16];
  float eye[4];
  float params[4]; // time (s), BumpScaleUV, BumpSpeedUV, RefracPertubPC3
  float params2[4]; // time of day, 1 when there is a reflection
  float reflect_view_proj[16]; // the mirrored camera the reflection was drawn with
  float refract_view_proj[16]; // the camera the refraction was drawn with (the view's, widened)
} tm_water_uniforms;

typedef struct tm_track_vertex {
  float pos[3];
  float uv[2];
  uint32_t color;        // BGRA
  float uv_occlusion[2]; // where the occlusion map is sampled
  uint32_t normal;       // 10:10:10 signed, as the meshes store it
  float uv3[2];          // a blended ground's third layer (Blend3, the mesh's uv set)
  float uv_prelight[2];  // in the map's lightmap atlas
  uint32_t prelight;     // COLOR0, BGRA: the scenery's baked ambient light (tmuf_track_prelight_instance)
} tm_track_vertex;

// Must match shaders/block.vert and block.frag.
typedef struct tm_block_vertex {
  float pos[3];
  float uv[2];          // Diffuse, Specular, Normal
  float uv_lighting[2]; // Lighting (or Occlusion)
  float uv_prelight[2]; // the map's lightmap
  uint32_t normal, tangent, binormal; // 10:10:10 signed, world space
} tm_block_vertex;

typedef struct tm_block_uniforms {
  float view_proj[16];
  float eye[4];
  float light_dir[4];
  float light_rgb[4];
  float clouds_u[4], clouds_v[4];
  float fog[4];
  float fog_color[4];
  float params[4]; // alpha cutoff, PreLightGen map, Lighting map is an occlusion only
  float lightgen[4]; // GbxLightGenP_ScaleAD_TransDA; y 0 when the lightmap holds colours
  float water[4];    // the water's height, 1 refraction / -1 reflection, time of day, 1 / WaterDepthMax
  // the sun's shadow maps (as tm_track_uniforms'); pssm_view w 1 to use them
  float pssm_static[3][4];
  float pssm_cascade[TM_PSSM_CASCADES][3][4];
  float pssm_cell[TM_PSSM_CASCADES][4];
  float pssm_split[TM_PSSM_CASCADES][4];
  float pssm_view[4];
} tm_block_uniforms;

// Must match shaders/car.*.
typedef struct tm_car_uniforms {
  float view_proj[16];
  float to_world[16];
  float eye[4];
  float light_dir[4];
  float light_rgb[4];
  float ambient[4];
  float clouds_u[4], clouds_v[4];
  float fade[4];   // GbxGameUnder_BlendIn_Fade_FOut_FIn
  float params[4]; // LightFromMap without its picture, dirt intensity, dirt gloss, details
  float balls_rgb[8][4], balls_pos[8][4];                 // Light8Balls (world)
  float spots_rgb[8][4], spots_pos[8][4], spots_dir[8][4]; // Light8Spots
  float hemi_dir[8][4], hemi_rgb[8][4];                   // HemiSpec's lights
  float counts[4]; // balls, spots, HemiSpec lights, LightFromMap has its picture
  float dov[4];    // eye -> the car's centre
  float lfm_u[4], lfm_v[4];
  float light_from_map[4]; // GbxLightFromMap_ScaleAD_TransDA (tm_light), y 0 for colours
  float water[4]; // drawn into the sea's refraction: its height, 1, the time of day, 1 / WaterDepthMax
} tm_car_uniforms;

// Must match shaders/fence.vert.
typedef struct tm_fence_vertex {
  float pos[3];
  float uv[2];
  float base_y; // the ground the tuft stands on
  uint32_t axis_x, axis_z; // the fence's frame in the world, 10:10:10 signed
} tm_fence_vertex;

typedef struct tm_fence_uniforms {
  float view_proj[16];
  float eye[4];
  float params[4]; // TexScaleU, SlideStartZ_Y1, SlideIntens, the level's far distance
} tm_fence_uniforms;

// A material's blended ground: its Blend2 and BlendI pictures and how their
// coordinates are generated. has false when it is not one.
typedef struct tm_blend {
  bool has;
  uint32_t second, mask; // texture indices
  float gen[4];
  float blend2[8], blend_mask[8];
  uint32_t stripe; // a lawn's Stripe picture, UINT32_MAX: none
  float stripe_gen[4], stripe_transform[8];
  uint32_t third; // Blend3 (rocks over grass...), UINT32_MAX: none
} tm_blend;
void tm_blend_of(ft_game *game, tm_picture_table *table, const tmuf_visual_material *m, tm_blend *out);
// a material's baked occlusion map ("Occlusion"), NULL for none
const tmuf_visual_texture *tm_occlusion_texture(const tmuf_visual_material *m);
// a material's texture of that sampler with an image, NULL for none
const tmuf_visual_texture *tm_sampler(const tmuf_visual_material *m, const char *name);
// the index of texture t in table, the mood's picture in place of a
// collection default (DefaultCube..., <Environment>Sky...); UINT32_MAX: none
uint32_t tm_map_index(ft_game *game, tm_picture_table *table, const tmuf_visual_texture *t);
// a mesh vertex's packed normal (up when it has none)
uint32_t tm_vertex_normal(const tmuf_visual_mesh *m, uint32_t vertex);
// the vertex colour of a mesh's vertex (white when it has none)
uint32_t tm_vertex_color(const tmuf_visual_mesh *m, uint32_t vertex);

// tm_image.c: DDS and TGA to RGBA8 (free *rgba with free)
bool tm_image_decode(const uint8_t *data, size_t size, uint8_t **rgba, uint32_t *width, uint32_t *height);
// ... or as the GPU takes them: BC blocks when `bc` (tg_supports_bc) and the
// file has its mip levels, else RGBA8 with a mip chain (free with tm_image_free)
typedef tg_image tm_image;
bool tm_image_load(const uint8_t *data, size_t size, bool bc, tm_image *out);
void tm_image_free(tm_image *image);
// RGBA8 pixels (malloc'd, taken over) with a mip chain made for them
bool tm_image_load_rgba(uint8_t *rgba, uint32_t width, uint32_t height, tm_image *out);

// tm_camera.c
extern const ft_camera_mode tm_camera_modes[];
extern const uint32_t tm_camera_mode_count;
bool tm_camera_update(ft_game *game, const ft_camera_frame *frame, ft_camera *inout);
// the game's near plane for a perspective view the engine made
void tm_camera_lens(ft_camera *camera);

// tm_car.c
tm_car_model *tm_car_create(ft_game *game, ft_level *level);
void tm_car_destroy(ft_game *game, tm_car_model *car);
void tm_car_render(ft_game *game, tm_car_model *car, const ft_render_frame *frame);
void tm_car_render_depth(ft_game *game, tm_car_model *car, const ft_render_frame *frame);
// the car into the sea's refraction (tmuf_scene.c): what of it is under the
// water, in the water's fog
void tm_car_render_refraction(ft_game *game, tm_car_model *car, const ft_render_frame *frame, float water_y,
                              float day_time, tg_texture water_fog);

// Where the car is drawn between two ticks.
typedef struct tm_pose {
  ft_vec3 position;
  float rotation[3][3]; // row-major, world = rotation * local + position
  ft_vec3 velocity;
} tm_pose;
tm_pose tm_pose_at(const ft_world *previous, const ft_world *world, float alpha);

// tmuf_weather.c: the light of the map's time of day (A01's sunset
// without a weather); the clouds map's rows (GbxVPositionToTexCoord_MapClouds)
// at a time in seconds
void tm_light_of(ft_game *game, const tmuf_track *track, tm_light *out);
void tm_light_clouds(const tm_light *light, double seconds, float u[4], float v[4]);

// tmuf_clouds.c: the sky's 3D clouds (Stadium); their light-occlusion
// picture first in a frame, then the clouds after the opaque scene
typedef struct tm_clouds tm_clouds;
tm_clouds *tm_clouds_create(ft_game *game, const tmuf_track *track, tm_picture_table *table);
void tm_clouds_destroy(ft_game *game, tm_clouds *clouds);
void tm_clouds_occlusion(ft_game *game, tm_clouds *clouds, const tm_light *light, const ft_camera *cam,
                         uint32_t time_ms);
void tm_clouds_render(ft_game *game, tm_clouds *clouds, const tm_light *light, const ft_camera *cam, uint32_t time_ms);

// tmuf_shadow.c: the car's shadow from the sun. begin (the car's frame),
// cast its parts, end; then the scenery it reaches is drawn again to receive it
typedef struct tm_shadow tm_shadow;
// tmuf_pssm.c: the sun's shadow maps on the vertex-lit scenery and the
// Stadium's by day. setup fits
// them to the view (the receivers' uniforms; true when the static map is
// to be drawn again); begin (the static map or the cascades' atlas), cast
// the casters that reach a map (-1 the static one), end
typedef struct tm_pssm tm_pssm;
bool tm_pssm_resources_create(ft_game *game);
void tm_pssm_resources_destroy(ft_game *game);
void tm_pssm_forget(ft_game *game);
bool tm_pssm_setup(ft_game *game, const float light[3], const float zone_min[3], const float zone_max[3],
                   const ft_camera *cam, tm_track_uniforms *u);
bool tm_pssm_begin(ft_game *game, bool statics);
void tm_pssm_end(ft_game *game);
bool tm_pssm_reaches(ft_game *game, int map, const float centre[3], float radius);
// (block: the mesh holds tm_block_vertex, else tm_track_vertex)
void tm_pssm_cast(ft_game *game, int map, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                  tg_texture picture, float alpha_test, bool two_sided, bool block);
// a part placed by (r, t) (its mesh's vertices in its own frame, tm_track_vertex)
void tm_pssm_cast_part(ft_game *game, int map, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                       const float r[3][3], const float t[3]);
tg_texture tm_pssm_static_texture(const ft_game *game);
tg_texture tm_pssm_atlas_texture(const ft_game *game);

// tmuf_ssao.c: the ambient occlusion of the vertex-lit scenery: begin, the
// receivers' depth, end (the occlusion and its blur, by the mood's
// parameters), then its picture for the receivers
typedef struct tm_ssao tm_ssao;
bool tm_ssao_resources_create(ft_game *game);
void tm_ssao_resources_destroy(ft_game *game);
bool tm_ssao_begin(ft_game *game, const ft_camera *cam);
void tm_ssao_depth(ft_game *game, const ft_camera *cam, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                   tg_texture picture, float alpha_test, bool two_sided);
void tm_ssao_end(ft_game *game, const ft_camera *cam, const tmuf_ambient_occlusion *params);
tg_texture tm_ssao_texture(const ft_game *game);

bool tm_shadow_resources_create(ft_game *game);
void tm_shadow_resources_destroy(ft_game *game);
void tm_shadow_clear(ft_game *game); // none this frame (yet); the active car's slot selected
// which car the calls below cast for: 0 the active one, then the other shown
// groups' (up to TM_SHADOW_CARS); false when it cannot be had
bool tm_shadow_select(ft_game *game, int car);
bool tm_shadow_begin(ft_game *game, const tm_light *light, const float car_r[3][3], const float car_t[3]);
void tm_shadow_cast(ft_game *game, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                    const float model_r[3][3], const float model_t[3]);
void tm_shadow_end(ft_game *game);
// the lamps' shadows of the car (up to TM_SHADOW_LAMPS): the car seen from
// the lamp, then its parts cast (as tm_shadow_cast), then ended
#define TM_SHADOW_LAMPS 3
bool tm_shadow_lamp_begin(ft_game *game, int i, const float lamp[3], float grey, const float car_r[3][3],
                          const float car_t[3]);
void tm_shadow_lamp_cast(ft_game *game, int i, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                         const float model_r[3][3], const float model_t[3]);
void tm_shadow_lamp_end(ft_game *game, int i);
// the car's fake blob shadow (shadow_spec §10) this frame: its projector
void tm_shadow_blob(ft_game *game, const tm_projector *projector);
bool tm_shadow_reaches(const ft_game *game, const float centre[3], float radius);
// a receiver (the scenery within radius of centre) drawn again under every
// car's shadows that reach it
void tm_shadow_receive(ft_game *game, bool block, tg_mesh *mesh, uint32_t first_index, uint32_t index_count,
                       const float view_proj[16], const float centre[3], float radius);
// the scenery near a point drawn in the lightmap (LightFromMap: shaders/lfm.*,
// its uniforms: to_clip[16], depth_row[4], fade[4]); false without a lightmap
bool tm_scene_light_from_map(ft_game *game, const tm_scene *scene, const float centre[3], float radius,
                             const float *uniforms);
// tmuf_car.c: the car's LightFromMap (before the scene), its parts into the
// shadow (tmuf_shadow.c)
void tm_car_light_from_map(ft_game *game, tm_car_model *car, const tm_scene *scene, const ft_render_frame *frame);
// after the frame: the cell read back (2 x 2); then the colour under a world
// point near the car (the marks' and particles' birth colour), false without
// (while the frame is recorded: queues this frame's read, takes an earlier one's)
void tm_car_lfm_read(ft_game *game, tm_car_model *car);
bool tm_car_lfm_tint(const tm_car_model *car, const float p[3], float rgb[3]);
void tm_car_shadow(ft_game *game, tm_car_model *car, const ft_render_frame *frame);
// the car into the sun's shadow map `map` (a cascade), as tm_car_shadow placed it
void tm_car_pssm_cast(ft_game *game, tm_car_model *car, int map);

// tmuf_flares.c: the lights' flares (the sun, lamps, the car's lights),
// after the transparent draws, before the post effects
typedef struct tm_flares tm_flares;
bool tm_flares_resources_create(ft_game *game);
void tm_flares_resources_destroy(ft_game *game);
// the lights' flares this frame and their tests (occlusion queries in the
// frame's pass, before tg_frame_resolve); then drawn over the resolved frame
void tm_flares_test(ft_game *game, const ft_level *level, const ft_render_frame *frame);
void tm_flares_render(ft_game *game, const ft_level *level, const ft_render_frame *frame, tg_texture clouds_occlusion);
// the lamps' fake ground reflection (with the flares, on maps whose spots
// have it): the frame's alpha then holds the ground's gloss (track.frag);
// drawn after the opaque scene and the sky
bool tm_flares_ground_reflects(ft_game *game, const ft_level *level);
void tm_flares_ground_reflection(ft_game *game, const ft_level *level, const ft_camera *cam);
// after the frame: the lights' tests read back, for the next frame's easing
void tm_flares_after_frame(ft_game *game);
// a texture's coordinates transform at a time (a b c d, tx ty, 0 0): its
// shader function's animation, else its own transform (tmuf_scene.c)
void tm_texture_uv(const tmuf_visual_texture *t, double seconds, float out[8]);
// the car's own lights where they are this frame (after tm_car_render)
typedef struct tm_car_light {
  const tmuf_light *light;
  float position[3], direction[3];
} tm_car_light;
uint32_t tm_car_lights(const tm_car_model *car, tm_car_light *out, uint32_t cap);

// tmuf_marks.c: the wheels' marks (skid_spec.md): update runs the ticks up
// to the frame's; render draws them (after the opaque scene)
typedef struct tm_marks tm_marks;
tm_marks *tm_marks_create(ft_game *game, ft_level *level);
void tm_marks_destroy(ft_game *game, tm_marks *marks);
void tm_marks_update(ft_game *game, tm_marks *marks, const ft_render_frame *frame);
void tm_marks_render(ft_game *game, tm_marks *marks, const ft_render_frame *frame);
// the grass marks' flattened tufts: into the frame's alpha before the tufts read it
void tm_marks_flatten(ft_game *game, tm_marks *marks, const ft_render_frame *frame);

// tmuf_bake.c: the map's lightmap when no shipped cache has it, by day
typedef struct tm_bake tm_bake;
typedef struct tm_bake_draw {
  tg_mesh *mesh;
  uint32_t first, count;
  bool block;              // tm_block_vertex, else tm_track_vertex
  bool caster, receiver;   // in the shadow maps; in the lightmap atlas
  tg_texture picture;      // the alpha-tested caster's, 0 none
  float cutoff;            // its alpha test (0.2), 0 none
  bool sky_caster;         // in the sky's and the spots' shadow maps (the zone's, not the decoration)
  bool ambient;            // lit by the ambient cube (its material has no CubeAmbient layer: RGB mode)
  float center[3], radius; // its sphere
} tm_bake_draw;
typedef struct tm_bake_input {
  const tmuf_track *track;
  const tm_bake_draw *draws;  // the scene's batches
  uint32_t draw_count;
  const tm_bake_draw *pieces; // each placement on its own (the spots')
  uint32_t piece_count;
  float box_min[3], box_max[3]; // the casters' world box
  float sun_dir[3];             // where the sun's (or the moon's) light travels
  bool sunrise;                 // the Sunrise mood (a 10-degree sun, power 1), else Day
  // RGB mode (Sunset, Night): the lightmap holds colours
  bool rgb;
  float dir_rgb[3];       // the directional light's colour
  float dir_angle;        // its EmittAngularSize (degrees): Night 40, Sunset 4
  tg_texture ambient_cube; // the mood's AmbCubeP
} tm_bake_input;
bool tm_bake_resources_create(ft_game *game);
void tm_bake_resources_destroy(ft_game *game);
// AD mode: records the bake into the frame; the lightmap target
// (mipmapped), NULL on failure. RGB mode: starts it (its spots take many
// frames); NULL, then tm_bake_continue each frame until it gives the target
tg_target *tm_bake_lightmap(ft_game *game, const tm_bake_input *in);
tg_target *tm_bake_continue(ft_game *game);
bool tm_bake_running(const ft_game *game);
void tm_bake_cancel(ft_game *game);
// after the frame: its work targets freed
void tm_bake_after_frame(ft_game *game);

// tmuf_particles.c: the car's smoke, gravel and water spray and splash
typedef struct tm_particles tm_particles;
tm_particles *tm_particles_create(ft_game *game, ft_level *level);
void tm_particles_destroy(ft_game *game, tm_particles *particles);
void tm_particles_update(ft_game *game, tm_particles *particles, const ft_render_frame *frame);
void tm_particles_render(ft_game *game, tm_particles *particles, const ft_render_frame *frame);
// particle.vert / particle.frag: vertices pos float3, uv float2, colour RGBA8;
// uniforms view_proj, mode, two uv transforms; two textures
tg_program *tm_particle_program_create(ft_game *game);

// Rally's falling leaves (tmuf_leaves.c): NULL for a map without leaf emitters
typedef struct tm_leaves tm_leaves;
tm_leaves *tm_leaves_create(ft_game *game, ft_level *level);
void tm_leaves_destroy(ft_game *game, tm_leaves *leaves);
// steps the leaves to the frame's time and camera (CSceneMobilLeaves::
// OnRenderBefore) and draws them
void tm_leaves_render(ft_game *game, tm_leaves *leaves, const ft_render_frame *frame);
bool tm_car_projector(ft_game *game, tm_car_model *car, tm_projector *out);
bool tm_car_blob(ft_game *game, tm_car_model *car, tm_projector *out);
// the clouds' light occlusion picture (0: none)
tg_texture tm_scene_clouds_occlusion(const tm_scene *scene);
tg_texture tm_clouds_occlusion_texture(const tm_clouds *clouds);

// tmuf_post.c: the post effects (the bloom) over the finished frame
bool tm_post_resources_create(ft_game *game);
void tm_post_resources_destroy(ft_game *game);
void tm_post_render(ft_game *game, uint32_t width, uint32_t height);
// four bilinear taps of src (offsets in uv) into a target: the game's blurs
void tm_post_taps(ft_game *game, tg_target *into, tg_texture src, const float offsets[4][2], const float weights[4]);

// tm_hud.c: the race's countdown and time over the frame
bool tm_hud_resources_create(ft_game *game);
void tm_hud_resources_destroy(ft_game *game);
void tm_hud_render(ft_game *game, const ft_render_frame *frame);
// a text without the game's formatting codes ($o, $fff, $$, $l[..])
void tm_strip_formatting(const char *in, char *out, size_t size);
// the speed and distance the race interface shows, after a tick
void tm_race_display_step(ft_world *world);

// tm_ui.c
#define TM_PANEL_PLAYER "Player Info##tmuf_player"
void tm_ui(ft_game *game, const ft_ui_frame *frame);
void tm_splash(const ft_engine_api *engine, void **context, const ft_ui_frame *frame);
void tm_splash_destroy(void *context);

// tm_export.c
uint32_t tm_exporter_count(ft_game *game);
const ft_exporter_desc *tm_exporter_desc(ft_game *game, uint32_t index);
bool tm_export_run(ft_game *game, uint32_t index, const ft_export_request *request);
bool tm_export_replay(ft_game *game, const char *path, int32_t track);

// tm_replay.c: replays as recordings (FT_CAP_RECORDINGS)
ft_recording *tm_recording_open(ft_game *game, const void *data, size_t size, const char *name,
                                bool (*progress)(void *user, float fraction), void *progress_user, char *error,
                                size_t error_size);
void tm_recording_destroy(ft_game *game, ft_recording *recording);
bool tm_recording_info(ft_game *game, const ft_recording *recording, ft_recording_info *out);
bool tm_recording_player(ft_game *game, const ft_recording *recording, uint32_t index, ft_recording_player *out);
bool tm_recording_level_matches(ft_game *game, const ft_recording *recording, const ft_level *level);
void tm_recording_tick_flags(ft_game *game, const ft_recording *recording, int32_t player, int32_t first_tick,
                             uint32_t count, uint8_t *out);
bool tm_recording_input(ft_game *game, const ft_recording *recording, int32_t player, int32_t tick, void *out_record);
void tm_world_step_playback(ft_game *game, ft_world *world, const void *inputs, const ft_player_playback *playback,
                            uint32_t player_count);
void tm_world_step(ft_game *game, ft_world *world, const void *inputs, uint32_t player_count);

// tm_imgui.c
void tm_imgui_attach(const ft_engine_api *engine);

#endif
