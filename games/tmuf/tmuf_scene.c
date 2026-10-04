// The track on the GPU (tmuf_gpu.h).
//
// tmuf_physics hands over what the game draws (tmuf_track_visuals): meshes,
// where each is placed, and for each material the image files its shader
// samples. Here every placement is transformed into world space once, sorted
// by the picture it shows and uploaded as one static mesh; a frame is one
// draw per picture. Unlit: a surface looks like its picture, nothing more.
//
// Some geometry is only drawn near the camera (the game's visual mip levels:
// grass tufts, detailed props). Those placements are grouped in cells of
// CELL metres per picture and distance range, and a cell is drawn while the
// camera is within its range.

#include "tmuf_internal.h"

#include "block_frag_spv.h"
#include "car_frag_spv.h"
#include "car_vert_spv.h"
#include "fence_frag_spv.h"
#include "fence_vert_spv.h"
#include "water_frag_spv.h"
#include "clouds_frag_spv.h"
#include "lfm_frag_spv.h"
#include "lfm_vert_spv.h"
#include "clouds_vert_spv.h"
#include "water_vert_spv.h"
#include "block_vert_spv.h"
#include "track_frag_spv.h"
#include "depthonly_frag_spv.h"
#include "depthonly_vert_spv.h"
#include "grassmark_frag_spv.h"
#include "track_vert_spv.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>

#define CELL 64.f

typedef tm_track_vertex track_vertex;
typedef tm_track_uniforms track_uniforms;

typedef struct batch {
  uint8_t family;   // FAMILY_*
  uint32_t texture; // index into scene->pictures, UINT32_MAX: white
  float alpha_cutoff;
  tm_blend blend;
  uint32_t occlusion; // texture index, UINT32_MAX: none
  bool glow;          // added to what is behind
  // the material's blending (tmuf_gpu.h's D3DBLEND factors): blended draws
  // come after the opaque ones and write no depth
  bool blended;
  uint8_t blend_src, blend_dst;
  // the block family: its maps (MAP_*), the Lighting map an occlusion only
  uint32_t maps[8];
  bool occlusion_only;
  uint8_t variant; // block.frag's
  uint32_t mask;   // an advert's Mask, UINT32_MAX: none
  bool start_light; // the start's lights, showing the countdown
  // the Stadium's lawn (track.frag): its light (1 the map's lightmap, 2 its
  // Lighting map, in the occlusion's slot), its occlusion, Fresnel and clouds
  uint8_t lawn;
  bool lawn_occlusion;
  uint32_t fresnel, clouds;
  bool sky; // the sky's dome: never under the sea
  bool gradient; // the sky's GradientV: its picture clamped (v below 0 under the horizon)
  bool lightmapped; // its material reads the map's lightmap (PreLightGen)
  bool in_lightmap; // and its meshes have the coordinates: in the atlas (else the game's shaders without it)
  bool reflected; // in the sea's reflection: its shader's VId flags Reflected, not Refracted nor HideAlways
  bool zone;      // holds the zone's blocks or terrain (not only its decoration): casts the sun's shadow
  bool underground; // under the sea (VIdHideWhenUnderground): drawn into its refraction only
  uint32_t material; // its material (tmuf_visuals.materials), UINT32_MAX for none
  // lit as the vertex-lit environments' scenery is (COLOR0 baked, the sun per
  // vertex: track.vert), double sided (the foliage: the double-sided light)
  bool prelit, double_sided;
  uint32_t horizon; // its horizon clouds (texture index), UINT32_MAX: none
  bool spec_lobe; // the scenery's specular lobe around the sun ("CSpecL": its SpecularCubeL)
  // its placements in scene->recv_pieces (prelit pictures): the headlight's
  // receivers are picked per placement, as the game's per-piece boxes
  uint32_t recv_first, recv_count;
  bool gloss;     // CSpecL_Pixel: its picture's alpha x LightReflect into the frame's alpha
  uint8_t address[2]; // its picture's addressing in u and v (D3DTEXTUREADDRESS, 0: wrap)
  // the scenery's "Spec FCOut" shaders: its EnvCubic by the Fresnel, the
  // sun's lobe; 1 opaque (Bay's far city, ps_21 of its trace: no shadow, no
  // AO), 2 the glass ("Trans", ps_29: blended, the static map's shadow)
  uint8_t fc_out;
  uint32_t env;
  // a second picture (TM_EXTRA_*, in the program's last slot), the
  // scenery's shader family (TM_KIND_*), and the second picture's animation
  // (its shader function, when it has one) or transform
  uint8_t extra_mode, kind;
  uint32_t extra;
  const tmuf_visual_texture *extra_texture;
  uint32_t first_index, index_count;
  bool ranged;          // drawn only at some camera distances
  float lod_near, lod_far;
  float center[3], radius;
  float cull_center[3], cull_radius; // the sphere around its vertices (frustum culling)
  // a level of a CPlugTreeVisualMip: drawn when the mip picks it this frame
  // (UINT32_MAX: none)
  uint32_t mip, mip_level;
  // a vertex animation (the Stadium's flags: tmuf_visual_sequence), one
  // placement a batch: drawn each frame from its mesh, tweened between two
  // sub-visuals; its first sub-visual's vertices as the rest are
  const tmuf_visual_instance *anim;
  const tmuf_visual_mesh *anim_mesh;
  uint8_t *anim_template; // the family's vertices
} batch;

struct tm_scene {
  // the lightmap baked on the first frame when no cache has it (tmuf_bake.c)
  bool bake_pending;
  tg_target *baked;
  // each lightmapped placement's part of its batch: the bake's spots' pieces
  struct scene_piece {
    uint32_t batch, first, count;
    float center[3], radius;
  } *pieces;
  uint32_t piece_count;
  struct scene_piece *recv_pieces; // the prelit pictures' placements, per batch (batch.recv_first)
  uint32_t recv_piece_count;
  float cast_min[3], cast_max[3]; // the casters' world box
  tg_mesh *meshes[4]; // per family
  // the view the batches are culled against (batch_visible): the frustum's
  // four side planes, normalised (cull_set)
  bool cull_on;
  float cull[4][4];
  tg_target *reflection; // the sea's reflection (sea_reflection_spec.md), made when first drawn
  tg_target *refraction; // what is under it (mipmapped), likewise
  tg_target *export_depth; // the depth handed to the engine (tm_scene_render)
  float zone_min[3], zone_max[3]; // the zone's blocks and terrain (not its decoration): the static shadow map's
  // the sea: its height, fog (a table of time of day and depth) and the time
  // of day the mood is at
  bool water;
  float water_y, day_time;
  uint32_t water_fog;
  // the sprite visuals (Rally's tree crowns), turned to the camera each
  // frame (draw_sprites)
  struct scene_sprites {
    const tmuf_visual_mesh *mesh;
    tmuf_iso4 location;
    uint32_t picture, clouds;
    uint32_t mip, mip_level;
    float lod_near, lod_far;
    float centre[3], radius;
  } *sprites;
  uint32_t sprite_count;
  tm_track_vertex *sprite_vertices;
  uint32_t sprite_vertex_cap;
  tm_picture_table pictures;
  tm_light light; // the map's time of day (tmuf_weather.c)
  // the map's lightmap atlas (PreLightGen): the game's cache of it, 0 when
  // there is none (the game bakes it then)
  tg_texture lightmap;
  float lightgen[4]; // how its texels read (tm_light's), zero without it
  const tmuf_scenery_light *scenery; // the vertex-lit environments' light, NULL for none
  tm_clouds *clouds; // the sky's 3D clouds (Stadium), NULL for none
  uint32_t batch_count;
  batch *batches;
  // the map's visual mips and the level each draws this frame (tm_scene_render)
  const tmuf_visual_mip *mips;
  uint32_t mip_count;
  uint32_t *mip_now;
};

// --- pipeline ----------------------------------------------------------------

bool tm_track_resources_create(ft_game *game) {
  if (!game->gpu) return true; // headless
  const tg_attr attrs[] = {
      {0, offsetof(track_vertex, pos), TG_FLOAT3},
      {1, offsetof(track_vertex, uv), TG_FLOAT2},
      {2, offsetof(track_vertex, color), TG_UINT1},
      {3, offsetof(track_vertex, uv_occlusion), TG_FLOAT2},
      {4, offsetof(track_vertex, normal), TG_UINT1},
      {5, offsetof(track_vertex, uv3), TG_FLOAT2},
      {6, offsetof(track_vertex, uv_prelight), TG_FLOAT2},
      {7, offsetof(track_vertex, prelight), TG_UINT1},
  };
  const tg_program_desc desc = {.vertex_spirv = k_track_vert_spv,
                                .vertex_spirv_size = sizeof k_track_vert_spv,
                                .fragment_spirv = k_track_frag_spv,
                                .fragment_spirv_size = sizeof k_track_frag_spv,
                                .vertex_stride = sizeof(track_vertex),
                                .attrs = attrs,
                                .attr_count = 8,
                                .uniform_size = sizeof(track_uniforms),
                                .texture_count = TM_TRACK_TEXTURES,
                                .cube_mask = 1u << 15}; // the Spec FCOut family's EnvCubic
  game->track_program = tg_program_create(game->gpu, &desc);
  // the grass marks (tmuf_marks.c) on the same vertices
  tg_program_desc grass = desc;
  grass.fragment_spirv = k_grassmark_frag_spv;
  grass.fragment_spirv_size = sizeof k_grassmark_frag_spv;
  game->grass_program = tg_program_create(game->gpu, &grass);
  const tg_attr block_attrs[] = {
      {0, offsetof(tm_block_vertex, pos), TG_FLOAT3},        {1, offsetof(tm_block_vertex, uv), TG_FLOAT2},
      {2, offsetof(tm_block_vertex, uv_lighting), TG_FLOAT2}, {3, offsetof(tm_block_vertex, uv_prelight), TG_FLOAT2},
      {4, offsetof(tm_block_vertex, normal), TG_UINT1},       {5, offsetof(tm_block_vertex, tangent), TG_UINT1},
      {6, offsetof(tm_block_vertex, binormal), TG_UINT1},
  };
  const tg_program_desc block = {.vertex_spirv = k_block_vert_spv,
                                 .vertex_spirv_size = sizeof k_block_vert_spv,
                                 .fragment_spirv = k_block_frag_spv,
                                 .fragment_spirv_size = sizeof k_block_frag_spv,
                                 .vertex_stride = sizeof(tm_block_vertex),
                                 .attrs = block_attrs,
                                 .attr_count = 7,
                                 .uniform_size = sizeof(tm_block_uniforms),
                                 .texture_count = 12,
                                 .cube_mask = 1u << 4 | 1u << 5};
  game->block_program = tg_program_create(game->gpu, &block);
  // the depth prepass: the positions alone (as the vertex shaders' own), no
  // colour; the uniforms' view_proj only
  tg_program_desc track_depth = desc, block_depth = block;
  track_depth.vertex_spirv = block_depth.vertex_spirv = k_depthonly_vert_spv;
  track_depth.vertex_spirv_size = block_depth.vertex_spirv_size = sizeof k_depthonly_vert_spv;
  track_depth.fragment_spirv = block_depth.fragment_spirv = k_depthonly_frag_spv;
  track_depth.fragment_spirv_size = block_depth.fragment_spirv_size = sizeof k_depthonly_frag_spv;
  track_depth.attr_count = block_depth.attr_count = 1;
  track_depth.uniform_size = block_depth.uniform_size = 16 * sizeof(float);
  track_depth.texture_count = block_depth.texture_count = 0;
  track_depth.cube_mask = block_depth.cube_mask = 0;
  game->track_depth_program = tg_program_create(game->gpu, &track_depth);
  game->block_depth_program = tg_program_create(game->gpu, &block_depth);
  const tg_attr fence_attrs[] = {{0, offsetof(tm_fence_vertex, pos), TG_FLOAT3},
                                 {1, offsetof(tm_fence_vertex, uv), TG_FLOAT2},
                                 {2, offsetof(tm_fence_vertex, base_y), TG_FLOAT1},
                                 {3, offsetof(tm_fence_vertex, axis_x), TG_UINT1},
                                 {4, offsetof(tm_fence_vertex, axis_z), TG_UINT1}};
  const tg_program_desc fence = {.vertex_spirv = k_fence_vert_spv,
                                 .vertex_spirv_size = sizeof k_fence_vert_spv,
                                 .fragment_spirv = k_fence_frag_spv,
                                 .fragment_spirv_size = sizeof k_fence_frag_spv,
                                 .vertex_stride = sizeof(tm_fence_vertex),
                                 .attrs = fence_attrs,
                                 .attr_count = 5,
                                 .uniform_size = sizeof(tm_fence_uniforms),
                                 .texture_count = 3};
  game->fence_program = tg_program_create(game->gpu, &fence);
  const tg_attr water_attrs[] = {{0, offsetof(tm_water_vertex, pos), TG_FLOAT3},
                                 {1, offsetof(tm_water_vertex, color), TG_UINT1}};
  const tg_program_desc water = {.vertex_spirv = k_water_vert_spv,
                                 .vertex_spirv_size = sizeof k_water_vert_spv,
                                 .fragment_spirv = k_water_frag_spv,
                                 .fragment_spirv_size = sizeof k_water_frag_spv,
                                 .vertex_stride = sizeof(tm_water_vertex),
                                 .attrs = water_attrs,
                                 .attr_count = 2,
                                 .uniform_size = sizeof(tm_water_uniforms),
                                 .texture_count = 5};
  game->water_program = tg_program_create(game->gpu, &water);
  // the sky's clouds (tmuf_clouds.c): world position, uv, colour, occlusion coordinates
  const tg_attr cloud_attrs[] = {{0, 0, TG_FLOAT3}, {1, 12, TG_FLOAT2}, {2, 20, TG_FLOAT4}, {3, 36, TG_FLOAT4}};
  const tg_program_desc clouds = {.vertex_spirv = k_clouds_vert_spv,
                                  .vertex_spirv_size = sizeof k_clouds_vert_spv,
                                  .fragment_spirv = k_clouds_frag_spv,
                                  .fragment_spirv_size = sizeof k_clouds_frag_spv,
                                  .vertex_stride = 52,
                                  .attrs = cloud_attrs,
                                  .attr_count = 4,
                                  .uniform_size = 24 * sizeof(float),
                                  .texture_count = 2};
  game->clouds_program = tg_program_create(game->gpu, &clouds);
  const tg_attr car_attrs[] = {{0, offsetof(track_vertex, pos), TG_FLOAT3},
                               {1, offsetof(track_vertex, uv), TG_FLOAT2},
                               {2, offsetof(track_vertex, normal), TG_UINT1}};
  const tg_program_desc car = {.vertex_spirv = k_car_vert_spv,
                               .vertex_spirv_size = sizeof k_car_vert_spv,
                               .fragment_spirv = k_car_frag_spv,
                               .fragment_spirv_size = sizeof k_car_frag_spv,
                               .vertex_stride = sizeof(track_vertex),
                               .attrs = car_attrs,
                               .attr_count = 3,
                               .uniform_size = sizeof(tm_car_uniforms),
                               .texture_count = 8,
                               .cube_mask = 1u << 2};
  game->car_program = tg_program_create(game->gpu, &car);
  // LightFromMap: the scenery's position and lightmap coordinates
  const tg_attr lfm_block_attrs[] = {{0, offsetof(tm_block_vertex, pos), TG_FLOAT3},
                                     {1, offsetof(tm_block_vertex, uv_prelight), TG_FLOAT2}};
  const tg_attr lfm_track_attrs[] = {{0, offsetof(track_vertex, pos), TG_FLOAT3},
                                     {1, offsetof(track_vertex, uv_prelight), TG_FLOAT2}};
  tg_program_desc lfm = {.vertex_spirv = k_lfm_vert_spv,
                         .vertex_spirv_size = sizeof k_lfm_vert_spv,
                         .fragment_spirv = k_lfm_frag_spv,
                         .fragment_spirv_size = sizeof k_lfm_frag_spv,
                         .vertex_stride = sizeof(tm_block_vertex),
                         .attrs = lfm_block_attrs,
                         .attr_count = 2,
                         .uniform_size = 24 * sizeof(float),
                         .texture_count = 1};
  game->lfm_block_program = tg_program_create(game->gpu, &lfm);
  lfm.vertex_stride = sizeof(track_vertex);
  lfm.attrs = lfm_track_attrs;
  game->lfm_track_program = tg_program_create(game->gpu, &lfm);
  if (!game->track_program || !game->block_program || !game->fence_program || !game->water_program ||
      !game->car_program) {
    tm_log(game, FT_LOG_ERROR, "Cannot create the track programs: %s", tg_error(game->gpu));
    return false;
  }
  return true;
}

void tm_track_resources_destroy(ft_game *game) {
  if (game->track_program) tg_program_destroy(game->gpu, game->track_program);
  if (game->grass_program) tg_program_destroy(game->gpu, game->grass_program);
  if (game->block_program) tg_program_destroy(game->gpu, game->block_program);
  if (game->track_depth_program) tg_program_destroy(game->gpu, game->track_depth_program);
  if (game->block_depth_program) tg_program_destroy(game->gpu, game->block_depth_program);
  game->track_depth_program = game->block_depth_program = NULL;
  if (game->fence_program) tg_program_destroy(game->gpu, game->fence_program);
  if (game->water_program) tg_program_destroy(game->gpu, game->water_program);
  if (game->car_program) tg_program_destroy(game->gpu, game->car_program);
  if (game->clouds_program) tg_program_destroy(game->gpu, game->clouds_program);
  if (game->lfm_block_program) tg_program_destroy(game->gpu, game->lfm_block_program);
  if (game->lfm_track_program) tg_program_destroy(game->gpu, game->lfm_track_program);
  game->water_program = game->car_program = game->clouds_program = NULL;
  game->lfm_block_program = game->lfm_track_program = NULL;
  game->track_program = game->grass_program = NULL;
  game->block_program = NULL;
  game->fence_program = NULL;
}

// --- pictures ------------------------------------------------------------------

static bool ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return !*a && !*b;
}

static bool has_image(const tmuf_visual_texture *t) { return t->file || t->pack_file; }

// Which of a material's textures is the surface's picture: its Diffuse, else
// the first layer of a blended ground, else anything that is not a map of some
// other quantity (normals, specular, occlusion, reflections...).
static const tmuf_visual_texture *sampler_named(const tmuf_visual_material *m, const char *name);
static uint32_t map_index(ft_game *game, tm_picture_table *table, const tmuf_visual_texture *t);

// whether a material has a layer (a sampler) of that name, a picture or not
static bool has_layer(const tmuf_visual_material *m, const char *name) {
  for (uint32_t i = 0; m && i < m->texture_count; i++)
    if (m->textures[i].sampler && strcmp(m->textures[i].sampler, name) == 0) return true;
  return false;
}

static const tmuf_visual_texture *picture_of(const tmuf_visual_material *m) {
  static const char *const preferred[] = {"Diffuse", "Diffuse_Gloss", "Blend1",  "Soil",
                                          "Grass",   "Panorama",      "Advert", "Translucent"};
  for (size_t p = 0; p < sizeof preferred / sizeof preferred[0]; p++)
    for (uint32_t i = 0; i < m->texture_count; i++)
      if (m->textures[i].sampler && ieq(m->textures[i].sampler, preferred[p]) && has_image(&m->textures[i]))
        return &m->textures[i];
  static const char *const other[] = {"Specular", "Normal",  "Occlusion", "Fresnel", "Clouds",   "CubeAmbient",
                                      "ReflectSoft", "PreLightGen", "PreLight", "BlendI", "Mask", "Stripe",
                                      "Refrac",   "Reflect", "Vertex",    "Glow",    "SelfIllum", "FakeSpec",
                                      "EnvMap",   "DispH01", "SpecularCubeL", "CubeSxSySz", "RenderEnv",
                                      "DirtMarks", "EnvCubic", "HemiSpec", "CubeInvHemi", "LightFromMap",
                                      "Anim"};
  for (uint32_t i = 0; i < m->texture_count; i++) {
    if (!has_image(&m->textures[i]) || !m->textures[i].sampler) continue;
    bool skip = false;
    for (size_t k = 0; k < sizeof other / sizeof other[0] && !skip; k++)
      skip = ieq(m->textures[i].sampler, other[k]);
    if (!skip) return &m->textures[i];
  }
  return NULL;
}


static tg_texture load_image(ft_game *game, const char *file, const char *pack_file) {
  const ft_engine_api *api = game->engine;
  uint8_t *data = NULL;
  size_t size = 0;
  void *engine_data = NULL;
  if (file) {
    if (!api->read_file(file, &engine_data, &size)) return 0;
    data = engine_data;
  } else if (pack_file) {
    data = tmuf_packs_read(game->packs, pack_file, &size);
  }
  if (!data) return 0;
  tm_image image;
  const bool ok = tm_image_load(data, size, tg_supports_bc(game->gpu), &image);
  if (engine_data) api->free_file_data(engine_data);
  else tmuf_free(data);
  if (!ok) {
    tm_log(game, FT_LOG_WARN, "Cannot decode %s", file ? file : pack_file);
    return 0;
  }
  const tg_texture texture = tg_texture_create(game->gpu, &image);
  if (!texture)
    tm_log(game, FT_LOG_WARN, "Cannot upload %s (%ux%u, format %d, %u levels, %u faces): %s", file ? file : pack_file,
           image.width, image.height, (int)image.format, image.levels, image.faces, tg_error(game->gpu));
  tm_image_free(&image);
  return texture;
}

// The mood (time of day) of a map is in its decoration's name: "Sunset",
// "Day", "30x30Sunrise"... Its pictures are in GameData/<environment>/Media/
// Moods/<mood>/, and the game's weather puts them on the sky.
void tm_picture_table_init(ft_game *game, tm_picture_table *table, const tmuf_track *track) {
  memset(table, 0, sizeof *table);
  static const char *const moods[] = {"Sunrise", "Sunset", "Night", "Day"};
  const char *decoration = tmuf_track_decoration(track);
  for (size_t i = 0; i < sizeof moods / sizeof moods[0]; i++) {
    if (!strstr(decoration, moods[i])) continue;
    char relative[512];
    snprintf(relative, sizeof relative, "GameData/%s/Media/Moods/%s/", tmuf_track_environment(track), moods[i]);
    tm_data_resolve_path(game->engine, relative, table->mood_dir, sizeof table->mood_dir);
    break;
  }
  table->weather = tmuf_track_weather(track);
}

tg_texture tm_picture_at(const tm_picture_table *table, uint32_t index, double seconds) {
  if (index >= table->count) return 0;
  return table->videos[index].count ? tm_video_frame(&table->videos[index], seconds) : table->textures[index];
}

void tm_picture_table_free(ft_game *game, tm_picture_table *table) {
  for (uint32_t i = 0; i < table->count; i++) {
    if (table->videos[i].count) tm_video_free(game, &table->videos[i]); // its frames, the picture among them
    else if (table->textures[i]) tg_texture_destroy(game->gpu, table->textures[i]);
    free(table->keys[i]);
  }
  free(table->keys);
  free(table->textures);
  free(table->videos);
  memset(table, 0, sizeof *table);
}

tm_mood_slot tm_mood_slot_of(const tmuf_visual_material *m) {
  if (!m) return TM_MOOD_NONE;
  for (uint32_t i = 0; i < m->texture_count; i++) {
    const tmuf_visual_texture *t = &m->textures[i];
    if (!t->sampler) continue;
    if (ieq(t->sampler, "GradientV")) return TM_MOOD_GRADIENT;
    if (ieq(t->sampler, "Panorama") && !has_image(t)) return TM_MOOD_PANORAMA;
  }
  return TM_MOOD_NONE;
}

static bool path_ends_with(const char *path, const char *suffix);

// the mood's skin (tmuf_weather.skin_entries): a picture of its own in place
// of a collection's (Sunset's StadiumRoadBorderD.dds, with blue bands)
static void mood_skin(const tm_picture_table *table, const char **key, const char **file, const char **pack_file) {
  const tmuf_weather *w = table->weather;
  if (!w) return;
  for (uint32_t i = 0; i < w->skin_entry_count; i++) {
    const tmuf_weather_skin_entry *e = &w->skin_entries[i];
    // a bitmap's entry swaps the image its bitmap names (the mood's
    // "Clouds": BayFXClouds.dds for Techno's flat DefaultClouds.dds)
    const bool bitmap = (e->image.file || e->image.pack_file) && (e->default_image.file || e->default_image.pack_file);
    const tmuf_weather_file from = bitmap ? e->default_image : e->default_file, to = bitmap ? e->image : e->file;
    if (!to.file && !to.pack_file) continue;
    const char *const defaults[2] = {from.file, from.pack_file};
    const char *const paths[2] = {*file, *pack_file};
    bool match = false;
    for (int d = 0; d < 2 && !match; d++)
      for (int p = 0; p < 2 && !match; p++)
        match = defaults[d] && paths[p] && strlen(defaults[d]) == strlen(paths[p]) &&
                path_ends_with(paths[p], defaults[d]);
    if (!match) continue;
    *file = to.file, *pack_file = to.pack_file;
    *key = *file ? *file : *pack_file;
    return;
  }
}

static uint32_t table_add(ft_game *game, tm_picture_table *table, const char *key, const char *file,
                          const char *pack_file) {
  mood_skin(table, &key, &file, &pack_file);
  for (uint32_t i = 0; i < table->count; i++)
    if (strcmp(table->keys[i], key) == 0) return table->textures[i] ? i : UINT32_MAX;
  if (table->count == table->cap) {
    const uint32_t cap = table->cap ? table->cap * 2 : 64;
    char **keys = realloc(table->keys, sizeof *keys * cap);
    if (keys) table->keys = keys;
    tg_texture *textures = keys ? realloc(table->textures, sizeof *textures * cap) : NULL;
    if (textures) table->textures = textures;
    tm_video *videos = textures ? realloc(table->videos, sizeof *videos * cap) : NULL;
    if (!keys || !textures || !videos) return UINT32_MAX;
    table->videos = videos;
    table->cap = cap;
  }
  char *copy = malloc(strlen(key) + 1);
  if (!copy) return UINT32_MAX;
  strcpy(copy, key);
  table->keys[table->count] = copy;
  memset(&table->videos[table->count], 0, sizeof table->videos[table->count]);
  const size_t n = file ? strlen(file) : 0;
  if (n > 4 && strcasecmp(file + n - 4, ".bik") == 0) {
    // a Bink video (the arrow signs): all its frames, the first as the picture
    if (tm_video_load(game, file, &table->videos[table->count]))
      table->textures[table->count] = table->videos[table->count].frames[0];
    else {
      tm_log(game, FT_LOG_WARN, "Cannot decode the video %s", file);
      table->textures[table->count] = 0;
    }
  } else {
    table->textures[table->count] = load_image(game, file, pack_file);
  }
  const uint32_t index = table->count++;
  return table->textures[index] ? index : UINT32_MAX;
}

// the same path, whatever its separators and case
static bool path_ends_with(const char *path, const char *suffix) {
  const size_t n = strlen(path), k = strlen(suffix);
  if (k > n) return false;
  for (size_t i = 0; i < k; i++) {
    char a = path[n - k + i], b = suffix[i];
    a = a == '\\' ? '/' : (char)tolower((unsigned char)a);
    b = b == '\\' ? '/' : (char)tolower((unsigned char)b);
    if (a != b) return false;
  }
  return true;
}

// A block skin's picture in place of the block's `picture`: the file of the
// skin's pack (a zip under GameData) its rule names (tmuf_block_skin), for
// the rule that replaces that picture; UINT32_MAX when none does
static uint32_t skin_index(ft_game *game, tm_picture_table *table, const tmuf_block_skin *skin,
                           const tmuf_visual_texture *picture) {
  const char *path = picture ? (picture->file ? picture->file : picture->pack_file) : NULL;
  if (!skin || !path) return UINT32_MAX;
  for (uint32_t r = 0; r < skin->rule_count; r++) {
    const tmuf_block_skin_rule *rule = &skin->rules[r];
    if (!rule->target || !rule->pattern || !path_ends_with(path, rule->target)) continue;
    // a pack that is a picture itself (a video: the Bay's arrow signs)
    const size_t fn = strlen(skin->file);
    if (fn > 4 && strcasecmp(skin->file + fn - 4, ".zip") != 0) {
      char relative[1200], disk[1200];
      snprintf(relative, sizeof relative, "GameData/%s", skin->file);
      for (char *c = relative; *c; c++)
        if (*c == '\\') *c = '/';
      if (!tm_data_resolve_path(game->engine, relative, disk, sizeof disk)) return UINT32_MAX;
      return table_add(game, table, disk, disk, NULL);
    }
    char key[1200];
    snprintf(key, sizeof key, "skin:%s|%s", skin->file, rule->pattern);
    for (uint32_t i = 0; i < table->count; i++)
      if (strcmp(table->keys[i], key) == 0) return table->textures[i] ? i : UINT32_MAX;
    // the pack's file: the pattern (a leading '*' for any start) with a
    // picture's extension
    tg_texture texture = 0;
    char relative[1200], zip_path[1200];
    snprintf(relative, sizeof relative, "GameData/%s", skin->file);
    for (char *c = relative; *c; c++)
      if (*c == '\\') *c = '/';
    void *zip = NULL;
    size_t zip_size = 0;
    if (tm_data_resolve_path(game->engine, relative, zip_path, sizeof zip_path) &&
        game->engine->read_file(zip_path, &zip, &zip_size)) {
      const char *core = rule->pattern[0] == '*' ? rule->pattern + 1 : rule->pattern;
      static const char *const ext[] = {".dds", ".tga", ".png", ".jpg"};
      for (size_t e = 0; e < sizeof ext / sizeof ext[0] && !texture; e++) {
        char name[256];
        snprintf(name, sizeof name, "%s%s", core, ext[e]);
        size_t size = 0;
        uint8_t *data = tmuf_zip_extract(zip, zip_size, name, &size);
        if (!data) continue;
        tm_image image;
        if (tm_image_load(data, size, tg_supports_bc(game->gpu), &image)) {
          texture = tg_texture_create(game->gpu, &image);
          tm_image_free(&image);
        }
        tmuf_free(data);
      }
      game->engine->free_file_data(zip);
    }
    if (!texture) tm_log(game, FT_LOG_WARN, "Cannot read the skin %s (%s)", skin->file, rule->pattern);
    const uint32_t index = table_add(game, table, key, NULL, NULL);
    if (index == UINT32_MAX && table->count && strcmp(table->keys[table->count - 1], key) == 0) {
      table->textures[table->count - 1] = texture;
      return texture ? table->count - 1 : UINT32_MAX;
    }
    return index;
  }
  return UINT32_MAX;
}

// Water refracts and reflects what is around (render targets the game draws
// each frame); until those are drawn here it is the sea's colour.
static bool is_water(const tmuf_visual_material *m) {
  for (uint32_t i = 0; i < m->texture_count; i++)
    if (m->textures[i].sampler && (ieq(m->textures[i].sampler, "Refrac") || ieq(m->textures[i].sampler, "Reflect")))
      return true;
  return false;
}

static uint32_t water_index(ft_game *game, tm_picture_table *table) {
  static const char key[] = "(water)";
  for (uint32_t i = 0; i < table->count; i++)
    if (strcmp(table->keys[i], key) == 0) return table->textures[i] ? i : UINT32_MAX;
  const uint32_t index = table_add(game, table, key, NULL, NULL);
  if (index != UINT32_MAX || table->count == 0 || strcmp(table->keys[table->count - 1], key) != 0) return index;
  const uint8_t sea[4] = {52, 104, 118, 255};
  const tg_image image = {TG_RGBA8, 1, 1, 1, 1, sea, sizeof sea};
  table->textures[table->count - 1] = tg_texture_create(game->gpu, &image);
  return table->textures[table->count - 1] ? table->count - 1 : UINT32_MAX;
}

// a glow's picture: its Glow, else its SelfIllum (the chasing light signs')
static const tmuf_visual_texture *glow_of(const tmuf_visual_material *m) {
  return sampler_named(m, "Glow") ? sampler_named(m, "Glow") : sampler_named(m, "SelfIllum");
}

bool tm_is_glow(const tmuf_visual_material *m) {
  return m && !picture_of(m) && glow_of(m) != NULL;
}

const tmuf_visual_texture *tm_picture_texture(const tmuf_visual_material *m) {
  if (!m) return NULL;
  if (tm_is_glow(m)) return glow_of(m);
  for (uint32_t i = 0; i < m->texture_count; i++) {
    const tmuf_visual_texture *t = &m->textures[i];
    if (t->sampler && (ieq(t->sampler, "GradientV") || (ieq(t->sampler, "Panorama") && !has_image(t)))) return t;
  }
  return picture_of(m);
}

// A texture's archived 2x3 transform as (a b c d, tx ty) with u = a x + c y +
// tx, v = b x + d y + ty: the file stores its rows (u = t0 x + t1 y + t4, v =
// t2 x + t3 y + t5: Bay's sea floor, GbxVPositionToTexCoord0/1 of its trace)
static void stored_transform(const tmuf_visual_texture *t, float out[6]) {
  const float *x = t->transform;
  out[0] = x[0], out[1] = x[2], out[2] = x[1], out[3] = x[3], out[4] = x[4], out[5] = x[5];
}

// The coordinates a texture is sampled at on a vertex, as the game's fixed
// pipeline makes them (CPlugBitmapAddress): a uv set of the mesh, or
// generated from the world position (EGxUVGenerate WorldVertexXY/XZ/YZ),
// then the address's 2x3 transform. The game's v runs up the picture.
void tm_texcoord(const tmuf_visual_texture *t, const tmuf_visual_mesh *m, uint32_t vertex, const float world[3],
                 float out[2]) {
  float u = 0.f, v = 0.f;
  bool generated = false;
  if (t && t->texcoord == TMUF_TEXCOORD_GENERATED) {
    switch (t->generate) {
    case 3: u = world[0], v = world[1], generated = true; break; // WorldVertexXY
    case 4: u = world[0], v = world[2], generated = true; break; // WorldVertexXZ
    case 5: u = world[1], v = world[2], generated = true; break; // WorldVertexYZ
    default: break;
    }
  }
  if (!generated) {
    uint32_t set = t && t->texcoord < m->uv_set_count ? t->texcoord : 0;
    if (set < m->uv_set_count && m->uv_dims[set] >= 2) {
      u = m->uv_sets[set][(size_t)vertex * m->uv_dims[set]];
      v = m->uv_sets[set][(size_t)vertex * m->uv_dims[set] + 1];
    }
  }
  if (t && t->has_transform) {
    float x[6];
    stored_transform(t, x);
    const float tu = x[0] * u + x[2] * v + x[4], tv = x[1] * u + x[3] * v + x[5];
    u = tu, v = tv;
  }
  // as the game has them (its pictures are bottom row first, tm_image.c)
  out[0] = u;
  out[1] = v;
}

static const tmuf_visual_texture *sampler_named(const tmuf_visual_material *m, const char *name) {
  for (uint32_t i = 0; m && i < m->texture_count; i++)
    if (m->textures[i].sampler && ieq(m->textures[i].sampler, name) && has_image(&m->textures[i]))
      return &m->textures[i];
  return NULL;
}

// the world axes generated coordinates come from (EGxUVGenerate)
static bool generated_axes(const tmuf_visual_texture *t, float axes[2]) {
  if (t->texcoord != TMUF_TEXCOORD_GENERATED) return false;
  switch (t->generate) {
  case 3: axes[0] = 0.f, axes[1] = 1.f; return true;
  case 4: axes[0] = 0.f, axes[1] = 2.f; return true;
  case 5: axes[0] = 1.f, axes[1] = 2.f; return true;
  default: return false;
  }
}

static void transform_of(const tmuf_visual_texture *t, float out[8]) {
  static const float identity[6] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
  memset(out, 0, sizeof(float) * 8);
  if (t->has_transform) stored_transform(t, out);
  else memcpy(out, identity, sizeof identity);
}

void tm_blend_of(ft_game *game, tm_picture_table *table, const tmuf_visual_material *m, tm_blend *out) {
  memset(out, 0, sizeof *out);
  const tmuf_visual_texture *base = sampler_named(m, "Blend1"), *second = sampler_named(m, "Blend2"),
                            *mask = sampler_named(m, "BlendI");
  if (!base || !second || !mask || !generated_axes(second, out->gen) || !generated_axes(mask, out->gen + 2)) return;
  out->second = table_add(game, table, second->file ? second->file : second->pack_file, second->file,
                          second->pack_file);
  out->mask = table_add(game, table, mask->file ? mask->file : mask->pack_file, mask->file, mask->pack_file);
  if (out->second == UINT32_MAX || out->mask == UINT32_MAX) return;
  transform_of(second, out->blend2);
  transform_of(mask, out->blend_mask);
  out->has = true;
  out->stripe = UINT32_MAX;
  // a third layer over the mix by its alpha: Blend3, or the dirt's SoilFix
  const tmuf_visual_texture *third = sampler_named(m, "Blend3") ? sampler_named(m, "Blend3") : sampler_named(m, "SoilFix");
  out->third = third ? table_add(game, table, third->file ? third->file : third->pack_file, third->file,
                                 third->pack_file)
                     : UINT32_MAX;
  const tmuf_visual_texture *stripe = sampler_named(m, "Stripe");
  if (stripe && generated_axes(stripe, out->stripe_gen)) {
    out->stripe = table_add(game, table, stripe->file ? stripe->file : stripe->pack_file, stripe->file,
                            stripe->pack_file);
    out->stripe_gen[2] = 1.f;
    transform_of(stripe, out->stripe_transform);
  }
}

const tmuf_visual_texture *tm_occlusion_texture(const tmuf_visual_material *m) {
  const tmuf_visual_texture *t = sampler_named(m, "Occlusion");
  return t && t->texcoord != TMUF_TEXCOORD_GENERATED ? t : NULL;
}

const tmuf_visual_texture *tm_sampler(const tmuf_visual_material *m, const char *name) {
  return sampler_named(m, name);
}

uint32_t tm_map_index(ft_game *game, tm_picture_table *table, const tmuf_visual_texture *t) {
  return map_index(game, table, t);
}

uint32_t tm_vertex_normal(const tmuf_visual_mesh *m, uint32_t vertex) {
  if (!(m->flags & TMUF_VISUAL_NORMAL)) return 511u << 10;
  uint32_t n;
  memcpy(&n, m->vertices + (size_t)vertex * m->vertex_stride + 12, 4);
  return n;
}

uint32_t tm_vertex_color(const tmuf_visual_mesh *m, uint32_t vertex) {
  if (!(m->flags & TMUF_VISUAL_COLOR)) return 0xffffffffu;
  uint32_t c;
  memcpy(&c, m->vertices + (size_t)vertex * m->vertex_stride + 12 + ((m->flags & TMUF_VISUAL_NORMAL) ? 4 : 0), 4);
  return c;
}

uint32_t tm_picture_index(ft_game *game, tm_picture_table *table, const tmuf_visual_material *m) {
  const tm_mood_slot slot = tm_mood_slot_of(m);
  if (slot != TM_MOOD_NONE) {
    if (!table->mood_dir[0]) return UINT32_MAX;
    char file[1200];
    snprintf(file, sizeof file, "%s%s", table->mood_dir, slot == TM_MOOD_GRADIENT ? "SkyColor.tga" : "SkyPanoramic.dds");
    return table_add(game, table, file, file, NULL);
  }
  const tmuf_visual_texture *t = m ? picture_of(m) : NULL;
  if (!t && tm_is_glow(m)) t = glow_of(m);
  if (!t && m && is_water(m)) return water_index(game, table);
  if (!t) return UINT32_MAX;
  return map_index(game, table, t);
}

// Where a picture's alpha makes holes: the alpha test the shader sets, else
// half for a shader the game blends (foliage, fences, grids), which a
// textures-only view draws as cut-outs rather than sorting them.
float tm_alpha_cutoff(const tmuf_visual_material *m) {
  if (!m) return 0.f;
  // the sky is opaque; its pictures keep other things in their alpha
  if (tm_mood_slot_of(m) != TM_MOOD_NONE) return 0.f;
  if (m->has_render_state && ((m->render_state[0] >> 24) & 7u) != 6u) {
    const float ref = (float)((m->render_state[0] >> 14) & 0xffu) / 255.f;
    return ref > 0.f ? ref : 0.5f;
  }
  // an opaque surface cut by its picture's alpha (the start arch's joints)
  if (!m->alpha_blend && m->alpha_test) return (float)m->alpha_ref / 255.f;
  return m->has_shader_flags && (m->shader_flags[0] & 0x100u) ? 0.5f : 0.f;
}

// --- the game's shader families ---------------------------------------------------

// The block family: Techno2's normal-mapped block shader (block.frag). Its
// maps, in the program's texture order.
enum { MAP_DIFFUSE, MAP_SPECULAR, MAP_NORMAL, MAP_LIGHTING, MAP_AMBIENT, MAP_REFLECT, MAP_FRESNEL, MAP_CLOUDS,
       BLOCK_MAPS };
static const char *const block_samplers[BLOCK_MAPS] = {"Diffuse",     "Specular",    "Normal",  "Lighting",
                                                       "CubeAmbient", "ReflectSoft", "Fresnel", "Clouds"};

// The variant: with a normal map and baked light (0, or 1 outside the map's
// lightmap), or lit by the sun alone (2, no maps of its own); -1: not a block
// the material's pixel program: "... NoAmbient ..." (the Stadium's
// decoration: its baked light alone, no ambient cube)
static bool no_ambient(const tmuf_visual_material *m) {
  const char *ps = m && m->pass_count ? m->passes[0].pixel.file : NULL;
  return ps && strstr(ps, "NoAmbient") != NULL;
}

static int block_variant(const tmuf_visual_material *m, const tmuf_visual_mesh *mesh) {
  if (!m || !(mesh->flags & TMUF_VISUAL_NORMAL)) return -1;
  // TDiff_Spec_Nrm TLight CSpecSoft NoAmbient: Diffuse 2 Lighting Clouds 2
  // + the soft reflection and the specular (variant 1's)
  if (no_ambient(m)) {
    static const int maps[] = {MAP_DIFFUSE, MAP_SPECULAR, MAP_NORMAL, MAP_LIGHTING, MAP_REFLECT, MAP_FRESNEL, MAP_CLOUDS};
    for (size_t k = 0; k < sizeof maps / sizeof maps[0]; k++)
      if (!sampler_named(m, block_samplers[maps[k]])) return -1;
    return mesh->tangents && mesh->binormals ? 1 : -1;
  }
  static const int common[] = {MAP_DIFFUSE, MAP_SPECULAR, MAP_AMBIENT, MAP_REFLECT, MAP_FRESNEL, MAP_CLOUDS};
  for (size_t k = 0; k < sizeof common / sizeof common[0]; k++)
    if (!sampler_named(m, block_samplers[common[k]])) return -1;
  if (!sampler_named(m, "Normal")) return 2;
  if (!mesh->tangents || !mesh->binormals) return -1;
  return sampler_named(m, "Lighting") || sampler_named(m, "Occlusion") ? 0 : -1;
}

static bool is_block(const tmuf_visual_material *m, const tmuf_visual_mesh *mesh) {
  return block_variant(m, mesh) >= 0;
}

// The mood's own pictures in place of the collection's defaults: the sky's
// reflections and ambient light (Default...), and the sky itself
// (<Environment>SkyPanoramic, ...: SkyPanoramic.dds in the mood's folder).
static bool mood_file(const tm_picture_table *table, const char *path, char *out, size_t out_size) {
  if (!table->mood_dir[0]) return false;
  static const struct {
    const char *suffix, *mood;
  } maps[] = {{"DefaultCubeAmbientP.dds", "AmbCubeP.dds"},
              {"DefaultCubeReflectSoft.dds", "EnvCubicSoft.dds"},
              {"DefaultCubeReflectHardSpecA.dds", "EnvCubicSpecA.dds"},
              {"DefaultEnvCubic.dds", "EnvCubic.dds"}};
  const size_t n = strlen(path);
  const char *mood = NULL;
  for (size_t i = 0; i < sizeof maps / sizeof maps[0] && !mood; i++) {
    const size_t k = strlen(maps[i].suffix);
    if (n >= k && ieq(path + n - k, maps[i].suffix)) mood = maps[i].mood;
  }
  if (!mood) {
    const char *base = path;
    for (const char *c = path; *c; c++)
      if (*c == '\\' || *c == '/') base = c + 1;
    const char *sky = strstr(base, "Sky");
    if (!sky || sky == base) return false;
    mood = sky;
  }
  snprintf(out, out_size, "%s%s", table->mood_dir, mood);
  FILE *f = fopen(out, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static uint32_t map_index(ft_game *game, tm_picture_table *table, const tmuf_visual_texture *t) {
  if (!t) return UINT32_MAX;
  const char *path = t->file ? t->file : t->pack_file;
  char file[1200];
  if (mood_file(table, path, file, sizeof file)) return table_add(game, table, file, file, NULL);
  return table_add(game, table, path, t->file, t->pack_file);
}

static uint32_t pack_normal(const float n[3]) {
  uint32_t out = 0;
  for (int k = 0; k < 3; k++) {
    float c = n[k] * 511.f;
    c = c > 511.f ? 511.f : c < -511.f ? -511.f : c;
    out |= ((uint32_t)(int32_t)lrintf(c) & 1023u) << (10 * k);
  }
  return out;
}

static void unpack_normal(uint32_t p, float n[3]) {
  for (int k = 0; k < 3; k++) {
    int32_t c = (int32_t)(p << (22 - 10 * k)) >> 22;
    n[k] = (float)c / 511.f;
  }
}

// a packed direction of the mesh turned by an instance's rotation
static uint32_t turn(uint32_t packed, const float (*r)[3]) {
  float n[3], w[3];
  unpack_normal(packed, n);
  for (int row = 0; row < 3; row++)
    w[row] = r[row][0] * n[0] + r[row][1] * n[1] + r[row][2] * n[2];
  return pack_normal(w);
}

// The sea's waves: the game turns the height map of its Normal sampler into
// world-space normals at load (y up, central differences of the heights
// scaled by 4), signed (Q8W8V8U8); here 128 + 127 n, so that 0 stays 0 (a
// half's offset rounded tilted the far sea, whose mips average out, by 0.5
// degree: its reflection fell off the horizon, water.frag)
static uint32_t sea_normal_index(ft_game *game, tm_picture_table *table, const tmuf_visual_texture *t) {
  if (!t) return UINT32_MAX;
  const char *path = t->file ? t->file : t->pack_file;
  char key[1300];
  snprintf(key, sizeof key, "(sea normals) %s", path);
  for (uint32_t i = 0; i < table->count; i++)
    if (strcmp(table->keys[i], key) == 0) return table->textures[i] ? i : UINT32_MAX;
  const uint32_t index = table_add(game, table, key, NULL, NULL); // an empty slot to fill
  if (index != UINT32_MAX || table->count == 0 || strcmp(table->keys[table->count - 1], key) != 0) return index;
  const uint32_t slot = table->count - 1;
  uint8_t *data = NULL;
  size_t size = 0;
  void *engine_data = NULL;
  if (t->file) {
    if (game->engine->read_file(t->file, &engine_data, &size)) data = engine_data;
  } else {
    data = tmuf_packs_read(game->packs, t->pack_file, &size);
  }
  uint8_t *rgba = NULL;
  uint32_t w = 0, h = 0;
  const bool ok = data && tm_image_decode(data, size, &rgba, &w, &h);
  if (engine_data) game->engine->free_file_data(engine_data);
  else if (data) tmuf_free(data);
  if (!ok) return UINT32_MAX;
  float *height = malloc(sizeof(float) * w * h);
  uint8_t *normals = malloc((size_t)w * h * 4);
  if (height && normals) {
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++)
        height[(size_t)y * w + x] = rgba[((size_t)y * w + x) * 4] / 255.f;
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        const float dx = (height[(size_t)y * w + (x + 1) % w] - height[(size_t)y * w + (x + w - 1) % w]) * 0.5f;
        const float dy = (height[(size_t)((y + 1) % h) * w + x] - height[(size_t)((y + h - 1) % h) * w + x]) * 0.5f;
        float n[3] = {-4.f * dx, 1.f, 4.f * dy};
        const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        uint8_t *o = normals + ((size_t)y * w + x) * 4;
        for (int k = 0; k < 3; k++)
          o[k] = (uint8_t)(128 + lrintf(n[k] / l * 127.f));
        o[3] = 255;
      }
    tm_image image;
    const size_t bytes = (size_t)w * h * 4;
    uint8_t *copy = malloc(bytes);
    if (copy) {
      memcpy(copy, normals, bytes);
      // tm_image_load's RGBA path: a mip chain
      if (tm_image_load_rgba(copy, w, h, &image)) {
        table->textures[slot] = tg_texture_create(game->gpu, &image);
        tm_image_free(&image);
      }
    }
  }
  free(height);
  free(normals);
  free(rgba);
  return table->textures[slot] ? slot : UINT32_MAX;
}

// The sea's fog: <Environment>/Media/Texture/Image/WaterFog.tga, else
// Techno's SeaWaterFog.tga; the time of day selects its column.
static void water_fog_of(ft_game *game, tm_scene *scene, const tmuf_track *track) {
  char relative[256], file[1200];
  snprintf(relative, sizeof relative, "GameData/%s/Media/Texture/Image/WaterFog.tga", tmuf_track_environment(track));
  tm_data_resolve_path(game->engine, relative, file, sizeof file);
  FILE *f = fopen(file, "rb");
  if (f) {
    fclose(f);
    scene->water_fog = table_add(game, &scene->pictures, file, file, NULL);
  } else {
    scene->water_fog = table_add(game, &scene->pictures, "Techno\\Media\\Texture\\Image\\SeaWaterFog.tga", NULL,
                                 "Techno\\Media\\Texture\\Image\\SeaWaterFog.tga");
  }
  // the table's column: the remapped time of day (GbxDayTime.y)
  scene->day_time = scene->light.day_time[1];
}

// The lightmap the game ships for a map (it bakes the others at load):
// GameData/LightmapsCache/<Nations|United|Common>/<id>_<author>.<map>.<mood>.LightMap.zip,
// LightMap0.dds in it, for the 2048-texel atlas tmuf_track_lightmap places.
// the mood: its folder's last name (Stadium\Media\Moods\Sunset\), "" for none
static void mood_name(const tmuf_weather *w, char *out, size_t out_size) {
  out[0] = '\0';
  if (!w || !w->mood.folder) return;
  const char *end = w->mood.folder + strlen(w->mood.folder);
  while (end > w->mood.folder && (end[-1] == '\\' || end[-1] == '/')) end--;
  const char *start = end;
  while (start > w->mood.folder && start[-1] != '\\' && start[-1] != '/') start--;
  snprintf(out, out_size, "%.*s", (int)(end - start), start);
}

static tg_texture lightmap_of(ft_game *game, const tmuf_track *track) {
  const tmuf_weather *w = tmuf_track_weather(track);
  const tmuf_lightmap *lm = tmuf_track_lightmap(track);
  if (!w || !lm || !lm->corpus_count || !w->mood.folder) return 0;
  char mood[64];
  mood_name(w, mood, sizeof mood);
  char suffix[512];
  snprintf(suffix, sizeof suffix, ".%s.%s.LightMap.zip", tmuf_track_name(track), mood);
  const size_t suffix_len = strlen(suffix);
  static const char *const dirs[] = {"Nations", "United", "Common"};
  for (int d = 0; d < 3; d++) {
    char rel[64], dir[1024];
    snprintf(rel, sizeof rel, "GameData/LightmapsCache/%s", dirs[d]);
    tm_data_resolve_path(game->engine, rel, dir, sizeof dir);
    DIR *dh = opendir(dir);
    if (!dh) continue;
    struct dirent *e;
    char found[1400] = "";
    while ((e = readdir(dh)) != NULL) {
      const size_t n = strlen(e->d_name);
      if (n > suffix_len && strcasecmp(e->d_name + n - suffix_len, suffix) == 0)
        snprintf(found, sizeof found, "%s/%s", dir, e->d_name);
    }
    closedir(dh);
    if (!found[0]) continue;
    void *zip = NULL;
    size_t zip_size = 0;
    if (!game->engine->read_file(found, &zip, &zip_size)) return 0;
    size_t size = 0;
    uint8_t *dds = tmuf_zip_extract(zip, zip_size, "LightMap0.dds", &size);
    game->engine->free_file_data(zip);
    if (!dds) return 0;
    tm_image image;
    tg_texture texture = 0;
    if (tm_image_load(dds, size, tg_supports_bc(game->gpu), &image)) {
      texture = tg_texture_create(game->gpu, &image);
      tm_image_free(&image);
    }
    tmuf_free(dds);
    if (texture) tm_log(game, FT_LOG_INFO, "Lightmap: %s", found);
    return texture;
  }
  return 0;
}

// --- geometry ------------------------------------------------------------------

enum { FAMILY_PICTURE, FAMILY_BLOCK, FAMILY_FENCE, FAMILY_WATER, FAMILY_COUNT };

// grass tufts: Techno2's fence shader (fence.frag)
static bool is_fence(const tmuf_visual_material *m) { return m && sampler_named(m, "FenceA"); }

typedef struct placed {
  uint32_t instance;
  uint32_t material;
  uint32_t texture;
  uint8_t family;
  float alpha_cutoff;
  bool ranged;
  float lod_near, lod_far;
  int32_t cell_x, cell_z;
  uint32_t mip, mip_level;
  uint32_t anim; // a vertex animation's placement + 1 (its own batch), 0 for none
  bool skinned;  // texture is the block's skin (a block family's Diffuse in its place)
  bool underground; // under the sea and VIdHideWhenUnderground: in its refraction only
} placed;

static bool grow_sprites(struct scene_sprites **sprites, uint32_t *cap) {
  const uint32_t n = *cap ? *cap * 2 : 64;
  struct scene_sprites *g = realloc(*sprites, sizeof *g * n);
  if (!g) return false;
  *sprites = g, *cap = n;
  return true;
}

// the top of a placement's box in the world
static float box_top(const tmuf_visual_instance *in, const tmuf_visual_mesh *m) {
  const float(*r)[3] = in->location.r.m;
  return r[1][0] * m->bounds[0] + r[1][1] * m->bounds[1] + r[1][2] * m->bounds[2] + in->location.t.y +
         fabsf(r[1][0]) * m->bounds[3] + fabsf(r[1][1]) * m->bounds[4] + fabsf(r[1][2]) * m->bounds[5];
}

// whether a sphere meets the headlight's frustum (its receivers: the
// pieces whose box meets it, Projector_RenderReceivers)
static bool projector_reaches(const tm_projector *p, const float c[3], float r) {
  float q[3];
  for (int a = 0; a < 3; a++)
    q[a] = (c[0] - p->pos[0]) * p->axes[a][0] + (c[1] - p->pos[1]) * p->axes[a][1] + (c[2] - p->pos[2]) * p->axes[a][2];
  const float near = p->frustum[2], far = p->frustum[5] > 0.f ? p->frustum[5] : 100.f;
  if (q[2] + r < near || q[2] - r > far) return false;
  for (int a = 0; a < 2; a++) {
    const float lo = p->frustum[a], hi = p->frustum[a + 3];
    // the side planes x = lo z and x = hi z
    if ((lo * q[2] - q[a]) / sqrtf(1.f + lo * lo) > r) return false;
    if ((q[a] - hi * q[2]) / sqrtf(1.f + hi * hi) > r) return false;
  }
  return true;
}

static int compare_placed(const void *pa, const void *pb) {
  const placed *a = pa, *b = pb;
  if (a->family != b->family) return a->family < b->family ? -1 : 1;
  if (a->ranged != b->ranged) return a->ranged ? 1 : -1;
  if (a->texture != b->texture) return a->texture < b->texture ? -1 : 1;
  if (a->material != b->material) return a->material < b->material ? -1 : 1;
  if (a->alpha_cutoff != b->alpha_cutoff) return a->alpha_cutoff < b->alpha_cutoff ? -1 : 1;
  if (a->lod_near != b->lod_near) return a->lod_near < b->lod_near ? -1 : 1;
  if (a->lod_far != b->lod_far) return a->lod_far < b->lod_far ? -1 : 1;
  if (a->cell_x != b->cell_x) return a->cell_x < b->cell_x ? -1 : 1;
  if (a->cell_z != b->cell_z) return a->cell_z < b->cell_z ? -1 : 1;
  if (a->mip != b->mip) return a->mip < b->mip ? -1 : 1;
  if (a->mip_level != b->mip_level) return a->mip_level < b->mip_level ? -1 : 1;
  if (a->anim != b->anim) return a->anim < b->anim ? -1 : 1;
  if (a->underground != b->underground) return a->underground ? 1 : -1;
  return a->instance < b->instance ? -1 : a->instance > b->instance;
}

static bool same_batch(const placed *a, const placed *b) {
  return a->family == b->family && a->ranged == b->ranged && a->texture == b->texture &&
         a->material == b->material && a->alpha_cutoff == b->alpha_cutoff && a->lod_near == b->lod_near &&
         a->lod_far == b->lod_far && a->cell_x == b->cell_x && a->cell_z == b->cell_z && a->mip == b->mip &&
         a->mip_level == b->mip_level && a->anim == b->anim && a->underground == b->underground;
}

// the vertices of one family, growing
typedef struct family_buffer {
  uint8_t *vertices;
  size_t stride;
  uint32_t vertex_count;
  uint32_t *indices;
  uint32_t index_count;
} family_buffer;

tm_scene *tm_scene_create(ft_game *game, ft_level *level) {
  const tmuf_visuals *v = tmuf_track_visuals(level->track);
  // the map's IsNight: which shader each material has (tmuf_physics resolves them)
  const tmuf_weather *weather = tmuf_track_weather(level->track);
  const bool night = weather && weather->is_night;
  const tmuf_scenery_light *scenery = tmuf_track_scenery_light(level->track);
  if (!v || !game->gpu || !game->track_program || !game->block_program) return NULL;
  tg_upload_begin(game->gpu);

  tm_picture_table table;
  tm_picture_table_init(game, &table, level->track);
  placed *order = malloc(sizeof *order * (v->instance_count ? v->instance_count : 1));
  if (!order) return NULL;
  uint64_t vertex_total[FAMILY_COUNT] = {0}, index_total[FAMILY_COUNT] = {0};
  uint32_t n = 0;
  float cast_min[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, cast_max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  struct scene_sprites *sprites = NULL;
  uint32_t sprite_count = 0, sprite_cap = 0;
  // the sea's height (its placements' tops), for what hides under it
  float sea_y = -FLT_MAX;
  for (uint32_t i = 0; i < v->instance_count; i++) {
    const tmuf_visual_instance *in = &v->instances[i];
    if (in->material < v->material_count && is_water(&v->materials[in->material]))
      sea_y = fmaxf(sea_y, box_top(in, &v->meshes[in->mesh]));
  }
  for (uint32_t i = 0; i < v->instance_count; i++) {
    const tmuf_visual_instance *in = &v->instances[i];
    const tmuf_visual_mesh *m = &v->meshes[in->mesh];
    const tmuf_visual_material *mat = in->material != UINT32_MAX ? &v->materials[in->material] : NULL;
    // a shader sampling nothing is an effect (the lamps' additive glows at
    // night), not a surface
    if ((m->flags & TMUF_VISUAL_SPRITES) && m->vertex_count && mat && !mat->hidden) {
      const uint32_t picture = tm_picture_index(game, &table, mat);
      if (picture != UINT32_MAX && (sprite_count < sprite_cap || grow_sprites(&sprites, &sprite_cap))) {
        struct scene_sprites *sp = &sprites[sprite_count++];
        sp->mesh = m;
        sp->location = in->location;
        sp->picture = picture;
        sp->clouds = map_index(game, &table, sampler_named(mat, "Clouds"));
        sp->mip = in->mip < v->mip_count ? in->mip : UINT32_MAX;
        sp->mip_level = in->mip_level;
        sp->lod_near = in->lod_near, sp->lod_far = in->lod_far;
        const float *bb = m->bounds;
        for (int row = 0; row < 3; row++)
          sp->centre[row] = in->location.r.m[row][0] * bb[0] + in->location.r.m[row][1] * bb[1] +
                            in->location.r.m[row][2] * bb[2] + (&in->location.t.x)[row];
        sp->radius = sqrtf(bb[3] * bb[3] + bb[4] * bb[4] + bb[5] * bb[5]);
      }
      continue;
    }
    if (!m->index_count || (mat && !mat->texture_count)) continue;
    // a shader the game never draws at this time of day (VIdHideAlways: the
    // lamps' night glows by day)
    if (mat && mat->hidden) continue;
    // a mip level the game never draws
    if (in->mip != UINT32_MAX && in->mip < v->mip_count && v->mips[in->mip].rule == TMUF_VISUAL_MIP_HIDDEN) continue;
    // the bake's shadow box (lightmap_bake_spec.md 4.1a): the static
    // lightmapped surfaces at their highest level whose mesh has the
    // lightmap's coordinates
    if (in->is_static && mat && mat->lightmap_uv != UINT32_MAX && mat->lightmap_uv < m->uv_set_count &&
        (in->mip == UINT32_MAX || in->mip_level == 0)) {
      const float(*rr)[3] = in->location.r.m;
      const float tt[3] = {in->location.t.x, in->location.t.y, in->location.t.z};
      for (int row = 0; row < 3; row++) {
        const float cc = rr[row][0] * m->bounds[0] + rr[row][1] * m->bounds[1] + rr[row][2] * m->bounds[2] + tt[row];
        const float hh = fabsf(rr[row][0]) * m->bounds[3] + fabsf(rr[row][1]) * m->bounds[4] + fabsf(rr[row][2]) * m->bounds[5];
        if (cc - hh < cast_min[row]) cast_min[row] = cc - hh;
        if (cc + hh > cast_max[row]) cast_max[row] = cc + hh;
      }
    }
    placed *p = &order[n++];
    p->instance = i;
    p->material = in->material;
    p->family = mat && is_water(mat) ? FAMILY_WATER
                : is_fence(mat)      ? FAMILY_FENCE
                : is_block(mat, m)   ? FAMILY_BLOCK
                                     : FAMILY_PICTURE;
    p->texture = tm_picture_index(game, &table, mat);
    // a block's skin: its own picture in place of the material's (the
    // block family's Diffuse: the Stadium's inflatables)
    p->skinned = false;
    if (in->block < 0x80000000u && (p->family == FAMILY_PICTURE || p->family == FAMILY_BLOCK)) {
      const tmuf_visual_texture *own = p->family == FAMILY_BLOCK ? sampler_named(mat, "Diffuse") : picture_of(mat);
      const uint32_t skinned = skin_index(game, &table, tmuf_track_block_skin(level->track, in->block), own);
      if (skinned != UINT32_MAX) p->texture = skinned, p->skinned = true;
    }
    p->alpha_cutoff = tm_alpha_cutoff(mat);
    // a mip's level: one batch per level of each placed mip (the game draws
    // exactly one); else by the distances it is drawn at, in cells
    p->mip = in->mip < v->mip_count ? in->mip : UINT32_MAX;
    p->mip_level = p->mip != UINT32_MAX ? in->mip_level : 0;
    p->ranged = p->mip == UINT32_MAX && (in->lod_near > 0.f || in->lod_far < FLT_MAX);
    p->lod_near = in->lod_near > 0.f || in->lod_far < FLT_MAX ? in->lod_near : 0.f;
    p->lod_far = in->lod_near > 0.f || in->lod_far < FLT_MAX ? in->lod_far : FLT_MAX;
    p->cell_x = p->ranged ? (int32_t)floorf(in->location.t.x / CELL) : 0;
    p->cell_z = p->ranged ? (int32_t)floorf(in->location.t.z / CELL) : 0;
    p->anim = in->sequence && m->sub_visual_count && (p->family == FAMILY_PICTURE || p->family == FAMILY_BLOCK) ? i + 1 : 0;
    // VIdHideWhenUnderground (the shader's VId flag 0x40) below the sea's
    // surface: not in the view nor its reflection, only in the refraction
    // (Bay's sea floor, whose shores reach the water: the game's frame has
    // nothing there before the sea, its refraction has it)
    p->underground = p->family == FAMILY_PICTURE && mat && (mat->visible_id & 0x40u) && sea_y > -FLT_MAX &&
                     box_top(in, m) <= sea_y + 0.01f;
    vertex_total[p->family] += m->vertex_count;
    index_total[p->family] += m->index_count;
  }
  qsort(order, n, sizeof *order, compare_placed);

  family_buffer fb[FAMILY_COUNT] = {{.stride = sizeof(track_vertex)},
                                    {.stride = sizeof(tm_block_vertex)},
                                    {.stride = sizeof(tm_fence_vertex)},
                                    {.stride = sizeof(tm_water_vertex)}};
  bool ok = true;
  for (int f = 0; f < FAMILY_COUNT; f++) {
    fb[f].vertices = malloc(fb[f].stride * (vertex_total[f] ? vertex_total[f] : 1));
    fb[f].indices = malloc(sizeof(uint32_t) * (index_total[f] ? index_total[f] : 1));
    ok = ok && fb[f].vertices && fb[f].indices && vertex_total[f] <= UINT32_MAX;
  }
  batch *batches = malloc(sizeof *batches * (n ? n : 1));
  tm_scene *scene = calloc(1, sizeof *scene);
  if (scene) memcpy(scene->cast_min, cast_min, sizeof cast_min), memcpy(scene->cast_max, cast_max, sizeof cast_max);
  if (scene) scene->sprites = sprites, scene->sprite_count = sprite_count;
  else free(sprites);
  if (!ok || !batches || !scene) {
    for (int f = 0; f < FAMILY_COUNT; f++)
      free(fb[f].vertices), free(fb[f].indices);
    free(batches), free(scene), free(order);
    tg_upload_end(game->gpu);
    return NULL;
  }
  uint32_t bcount = 0;
  struct scene_piece *pieces = NULL, *recv = NULL;
  uint32_t piece_count = 0, piece_cap = 0, recv_count = 0, recv_cap = 0;
  for (uint32_t k = 0; k < n; k++) {
    const placed *p = &order[k];
    family_buffer *out = &fb[p->family];
    const tmuf_visual_material *mat = p->material != UINT32_MAX ? &v->materials[p->material] : NULL;
    if (k == 0 || !same_batch(p, &order[k - 1])) {
      batch *b = &batches[bcount++];
      memset(b, 0, sizeof *b);
      b->horizon = b->mask = UINT32_MAX;
      b->family = p->family;
      b->material = p->material;
      b->lightmapped = mat && mat->lightmap_uv != UINT32_MAX;
      {
        const tmuf_visual_mesh *pm = &v->meshes[v->instances[p->instance].mesh];
        b->in_lightmap = b->lightmapped && mat->lightmap_uv < pm->uv_set_count;
      }
      b->texture = p->texture;
      {
        // (the game clamps the tree atlases, mirrors CoastRoad: the bitmap's)
        const tmuf_visual_texture *pt = mat ? picture_of(mat) : NULL;
        b->address[0] = pt ? pt->address[0] : 0, b->address[1] = pt ? pt->address[1] : 0;
      }
      b->alpha_cutoff = p->alpha_cutoff;
      if (p->family == FAMILY_WATER) {
        b->maps[0] = sea_normal_index(game, &table, sampler_named(mat, "Normal"));
        b->maps[1] = map_index(game, &table, sampler_named(mat, "Fresnel"));
      } else if (p->family == FAMILY_FENCE) {
        b->maps[0] = map_index(game, &table, sampler_named(mat, "FenceA"));
        b->maps[1] = map_index(game, &table, sampler_named(mat, "FadeXZ"));
      } else if (p->family == FAMILY_BLOCK) {
        for (int m = 0; m < BLOCK_MAPS; m++)
          b->maps[m] = map_index(game, &table, sampler_named(mat, block_samplers[m]));
        if (p->skinned) b->maps[MAP_DIFFUSE] = p->texture;
        b->variant = (uint8_t)block_variant(mat, &v->meshes[v->instances[p->instance].mesh]);
        // a block outside the map's lightmap (its shader samples no
        // PreLightGen): its baked light alone (the game's other variant)
        if (b->variant == 0 && mat->lightmap_uv == UINT32_MAX) b->variant = 1;
        b->reflected = (mat->visible_id & 0x105u) == 0x001u;
        // a blended block (the stands' glass: BlendFromOpacityMap on a
        // picture with alpha): with the transparent ones, its alpha test
        if (mat->has_render_state && mat->alpha_blend) {
          b->blend_src = (uint8_t)(mat->blend_src + 1u);
          b->blend_dst = (uint8_t)(mat->blend_dst + 1u);
          b->blended = true;
          if (mat->alpha_test) b->alpha_cutoff = ((float)mat->alpha_ref + 0.5f) / 255.f;
        }
        // without its baked light, a block has its occlusion alone
        // by day the game's block shaders read the Occlusion map (TOcc), at
        // night the Lighting map (TLight); either when it has only one
        b->occlusion_only = b->maps[MAP_LIGHTING] == UINT32_MAX ||
                            (!night && sampler_named(mat, "Occlusion") && !no_ambient(mat));
        if (b->occlusion_only) b->maps[MAP_LIGHTING] = map_index(game, &table, sampler_named(mat, "Occlusion"));
      } else {
        tm_blend_of(game, &table, mat, &b->blend);
        // the Stadium's lawn: a blended ground with a Fresnel, lit by the
        // map's lightmap (with its occlusion when it has one), else by its
        // own Lighting map (Techno2's grass shaders)
        b->lawn = 0;
        b->lawn_occlusion = false;
        b->fresnel = b->clouds = UINT32_MAX;
        if (b->blend.has && sampler_named(mat, "Fresnel")) {
          if (mat->lightmap_uv != UINT32_MAX) {
            b->lawn = 1;
            b->lawn_occlusion = sampler_named(mat, "Occlusion") != NULL;
          } else if (sampler_named(mat, "Lighting")) {
            b->lawn = 2;
          }
          if (b->lawn) {
            b->fresnel = map_index(game, &table, sampler_named(mat, "Fresnel"));
            b->clouds = map_index(game, &table, sampler_named(mat, "Clouds"));
          }
        } else if (mat->lightmap_uv != UINT32_MAX && (sampler_named(mat, "Soil") || b->blend.has)) {
          // the Stadium's dirt (Techno2's soil shaders): its picture (or mix)
          // lit by the map's lightmap, its occlusion and the clouds' shadows
          b->lawn = 3;
          b->clouds = map_index(game, &table, sampler_named(mat, "Clouds"));
        }
        // an advert's mask goes over it as a ground's third layer does
        if (!b->blend.has && sampler_named(mat, "Advert") && sampler_named(mat, "Mask"))
          b->mask = map_index(game, &table, sampler_named(mat, "Mask"));
        b->glow = tm_is_glow(mat);
        b->gradient = tm_mood_slot_of(mat) == TM_MOOD_GRADIENT;
        b->sky = tm_mood_slot_of(mat) != TM_MOOD_NONE || sampler_named(mat, "Panorama") ||
                 sampler_named(mat, "HorizonClouds");
        // the vertex-lit environments' scenery (the collection's VertexLighting 1)
        b->prelit = scenery && scenery->vertex_lighting == 1 && !b->glow && !b->sky && !b->lawn && mat;
        b->double_sided = mat && mat->double_sided;
        b->reflected = mat && (mat->visible_id & 0x105u) == 0x001u;
        if (b->prelit && b->clouds == UINT32_MAX) b->clouds = map_index(game, &table, sampler_named(mat, "Clouds"));
        b->horizon = map_index(game, &table, sampler_named(mat, "HorizonClouds"));
        // the scenery's shader family, as its pixel program's file names it
        const char *ps = mat && mat->pass_count ? mat->passes[0].pixel.file : NULL;
        // (CSpecL: its SpecularCubeL; CSpecL_Pixel, the roads: the same lobe
        // from its generated 1D Specular, u^20 at u = R.-L, Bay's ps_22)
        b->spec_lobe = b->prelit && ps && strstr(ps, "CSpecL") &&
                       has_layer(mat, strstr(ps, "CSpecL_Pixel") ? "Specular" : "SpecularCubeL");
        // (only CSpecL_Pixel writes its gloss into the frame's alpha, for the
        // lamps' fake ground reflection; the SpecularCubeL ones write 0:
        // Coast's Sunset trace, its start road's ps_23)
        b->gloss = b->spec_lobe && strstr(ps, "CSpecL_Pixel");
        b->fc_out = 0;
        if (b->prelit && ps && strstr(ps, "Spec FCOut") && sampler_named(mat, "EnvCubic") && sampler_named(mat, "Fresnel"))
          b->fc_out = strstr(ps, "Trans") ? 2 : 1;
        b->env = UINT32_MAX;
        if (b->fc_out) {
          b->env = map_index(game, &table, sampler_named(mat, "EnvCubic"));
          b->fresnel = map_index(game, &table, sampler_named(mat, "Fresnel"));
        }
        b->kind = TM_KIND_X2;
        if (ps && strstr(ps, "TSelfI")) b->kind = TM_KIND_T_SELF_I;
        else if (ps && strstr(ps, "DiffG_SelfI")) b->kind = strstr(ps, "X2") ? TM_KIND_SELF_I_X2 : TM_KIND_SELF_I;
        // a second picture: the family's SelfIllum, a Glow added to a
        // picture (the start lights), the chasing signs' sequencer, the sky's
        // ceiling under an opaque panorama; on the third layer's coordinates
        // (a material has one or the other)
        b->extra_mode = TM_EXTRA_NONE;
        b->extra = UINT32_MAX;
        b->extra_texture = NULL;
        const bool third_layer = sampler_named(mat, "Blend3") || sampler_named(mat, "SoilFix") ||
                                 sampler_named(mat, "Mask");
        const tmuf_visual_texture *self_illum = sampler_named(mat, "SelfIllum"), *glow = sampler_named(mat, "Glow"),
                                  *anim = sampler_named(mat, "Anim"), *ceiling = sampler_named(mat, "Clouds"),
                                  *borders = sampler_named(mat, "Borders");
        if (self_illum && (b->kind == TM_KIND_SELF_I || b->kind == TM_KIND_SELF_I_X2)) {
          b->extra_mode = TM_EXTRA_SELF_ILLUM, b->extra_texture = self_illum;
        } else if (!third_layer && b->glow && anim) {
          b->extra_mode = TM_EXTRA_SEQUENCER, b->extra_texture = anim;
        } else if (!third_layer && !b->glow && glow && picture_of(mat)) {
          b->extra_mode = TM_EXTRA_GLOW, b->extra_texture = glow;
        } else if (!third_layer && b->sky && ceiling && sampler_named(mat, "Panorama") &&
                   ceiling->texcoord != TMUF_TEXCOORD_GENERATED) {
          b->extra_mode = TM_EXTRA_CEILING, b->extra_texture = ceiling;
        } else if (!third_layer && !b->lawn && borders && sampler_named(mat, "Grass")) {
          // the grass shaders: lerp(Grass, Borders, Borders.a) (env_light_spec §5)
          b->extra_mode = TM_EXTRA_BORDERS, b->extra_texture = borders;
        }
        if (b->extra_texture) b->extra = map_index(game, &table, b->extra_texture);
        if (b->extra == UINT32_MAX) b->extra_mode = TM_EXTRA_NONE, b->extra_texture = NULL;
        // the blending the game sets (CDx9ShaderKeeper::Undirty, the
        // library's alpha_blend: a SRC_ALPHA / INV_SRC_ALPHA apply whose
        // alpha bitmap has usage bit 21 is alpha tested instead, the Snow
        // and Coast trees and banners); the factors 0 Zero, 1 One, then
        // D3DBLEND's order
        b->blend_src = TG_BLEND_ONE, b->blend_dst = TG_BLEND_ZERO;
        if (mat && mat->has_render_state && mat->alpha_blend) {
          b->blend_src = (uint8_t)(mat->blend_src + 1u);
          b->blend_dst = (uint8_t)(mat->blend_dst + 1u);
        }
        if (b->glow) b->blend_src = b->blend_dst = TG_BLEND_ONE;
        // a lamp's glow picture as a surface's own (the floodlights' heads):
        // added by its alpha, as the game draws them (its pass's state is not
        // the material's)
        const tmuf_visual_texture *pic = tm_picture_texture(mat);
        const char *pic_file = pic ? (pic->file ? pic->file : pic->pack_file) : NULL;
        b->start_light = pic_file && strstr(pic_file, "StartSignGlow");
        if (!b->glow && pic_file && strstr(pic_file, "Glow.")) {
          b->blend_src = TG_BLEND_SRCALPHA;
          b->blend_dst = TG_BLEND_ONE;
        }
        b->blended = !(b->blend_src == TG_BLEND_ONE && b->blend_dst == TG_BLEND_ZERO);
        if (b->blended) {
          const bool tested = mat && mat->has_render_state && ((mat->render_state[0] >> 24) & 7u) != 6u;
          // the alpha test, when the material has one: above its reference
          b->alpha_cutoff = tested ? ((float)((mat->render_state[0] >> 14) & 0xffu) + 0.5f) / 255.f : 0.f;
        }
        const tmuf_visual_texture *occ = b->lawn == 2 ? sampler_named(mat, "Lighting") : tm_occlusion_texture(mat);
        b->occlusion = occ ? table_add(game, &table, occ->file ? occ->file : occ->pack_file, occ->file, occ->pack_file)
                           : UINT32_MAX;
      }
      b->first_index = out->index_count;
      b->ranged = p->ranged;
      b->lod_near = p->lod_near;
      b->lod_far = p->lod_far;
      b->mip = p->mip;
      b->mip_level = p->mip_level;
      b->underground = p->underground;
      if (p->anim) {
        b->anim = &v->instances[p->anim - 1];
        b->anim_mesh = &v->meshes[b->anim->mesh];
      }
      b->center[0] = ((float)p->cell_x + 0.5f) * CELL;
      b->center[2] = ((float)p->cell_z + 0.5f) * CELL;
      b->radius = CELL * 0.7072f;
    }
    batch *b = &batches[bcount - 1];
    const tmuf_visual_instance *in = &v->instances[p->instance];
    if ((in->block & 0xc0000000u) != 0xc0000000u) b->zone = true;
    const tmuf_visual_mesh *m = &v->meshes[in->mesh];
    const float(*r)[3] = in->location.r.m;
    const float t[3] = {in->location.t.x, in->location.t.y, in->location.t.z};
    if (p->family == FAMILY_WATER) {
      for (uint32_t i = 0; i < m->vertex_count; i++) {
        float pos[3];
        memcpy(pos, m->vertices + (size_t)i * m->vertex_stride, sizeof pos);
        tm_water_vertex *o = (tm_water_vertex *)out->vertices + out->vertex_count + i;
        for (int row = 0; row < 3; row++)
          o->pos[row] = r[row][0] * pos[0] + r[row][1] * pos[1] + r[row][2] * pos[2] + t[row];
        o->color = tm_vertex_color(m, i);
        if (!scene->water) scene->water_y = o->pos[1];
        scene->water = true;
      }
    } else if (p->family == FAMILY_FENCE) {
      const tmuf_visual_texture *fence = sampler_named(mat, "FenceA");
      for (uint32_t i = 0; i < m->vertex_count; i++) {
        float pos[3];
        memcpy(pos, m->vertices + (size_t)i * m->vertex_stride, sizeof pos);
        tm_fence_vertex *o = (tm_fence_vertex *)out->vertices + out->vertex_count + i;
        for (int row = 0; row < 3; row++)
          o->pos[row] = r[row][0] * pos[0] + r[row][1] * pos[1] + r[row][2] * pos[2] + t[row];
        tm_texcoord(fence, m, i, o->pos, o->uv);
        o->base_y = t[1];
        const float ax[3] = {r[0][0], r[1][0], r[2][0]}, az[3] = {r[0][2], r[1][2], r[2][2]};
        o->axis_x = pack_normal(ax);
        o->axis_z = pack_normal(az);
      }
    } else if (p->family == FAMILY_BLOCK) {
      // the coordinates of the map in the Lighting slot: the Occlusion map by
      // day, the Lighting map at night (as the batch picks it)
      const tmuf_visual_texture *diffuse = sampler_named(mat, "Diffuse");
      const tmuf_visual_texture *lit = sampler_named(mat, "Lighting"), *occ = sampler_named(mat, "Occlusion");
      const tmuf_visual_texture *occlusion = (night && lit) || !occ ? lit : occ;
      const tmuf_lightmap *lm = tmuf_track_lightmap(level->track);
      const tmuf_lightmap_corpus *corpus =
          lm && in->lightmap < lm->corpus_count && lm->corpora[in->lightmap].row != UINT32_MAX ? &lm->corpora[in->lightmap]
                                                                                              : NULL;
      for (uint32_t i = 0; i < m->vertex_count; i++) {
        float pos[3];
        memcpy(pos, m->vertices + (size_t)i * m->vertex_stride, sizeof pos);
        tm_block_vertex *o = (tm_block_vertex *)out->vertices + out->vertex_count + i;
        for (int row = 0; row < 3; row++)
          o->pos[row] = r[row][0] * pos[0] + r[row][1] * pos[1] + r[row][2] * pos[2] + t[row];
        tm_texcoord(diffuse, m, i, o->pos, o->uv);
        tm_texcoord(occlusion, m, i, o->pos, o->uv_lighting);
        // in the lightmap atlas: the corpus's cells (CorpusToLightGenP); a
        // mesh without the material's lightmap uvs reads the atlas's corner
        o->uv_prelight[0] = o->uv_prelight[1] = 0.f;
        if (corpus && mat->lightmap_uv < m->uv_set_count && m->uv_dims[mat->lightmap_uv] >= 2) {
          const float *uv = m->uv_sets[mat->lightmap_uv] + (size_t)i * m->uv_dims[mat->lightmap_uv];
          o->uv_prelight[0] = corpus->scale[0] * uv[0] + corpus->offset[0];
          o->uv_prelight[1] = corpus->scale[1] * uv[1] + corpus->offset[1];
        }
        o->normal = turn(tm_vertex_normal(m, i), r);
        o->tangent = m->tangents ? turn(m->tangents[i], r) : 0;
        o->binormal = m->binormals ? turn(m->binormals[i], r) : 0;
      }
    } else {
      const tmuf_visual_texture *picture = tm_picture_texture(mat),
                                *occlusion = b->lawn == 2 ? sampler_named(mat, "Lighting") : tm_occlusion_texture(mat);
      // the vertex-lit environments: the game's COLOR0 bake of this placement
      uint8_t *bake = NULL;
      if (b->prelit && (bake = malloc((size_t)m->vertex_count * 4)) != NULL &&
          tmuf_track_prelight_instance(level->track, p->instance, bake) != m->vertex_count) {
        free(bake);
        bake = NULL;
      }
      const tmuf_lightmap *lm = tmuf_track_lightmap(level->track);
      const tmuf_lightmap_corpus *corpus =
          mat && mat->lightmap_uv != UINT32_MAX && lm && in->lightmap < lm->corpus_count &&
                  lm->corpora[in->lightmap].row != UINT32_MAX
              ? &lm->corpora[in->lightmap]
              : NULL;
      for (uint32_t i = 0; i < m->vertex_count; i++) {
        float pos[3];
        memcpy(pos, m->vertices + (size_t)i * m->vertex_stride, sizeof pos);
        track_vertex *o = (track_vertex *)out->vertices + out->vertex_count + i;
        for (int row = 0; row < 3; row++)
          o->pos[row] = r[row][0] * pos[0] + r[row][1] * pos[1] + r[row][2] * pos[2] + t[row];
        if (b->start_light) {
          // CGameCtnChallenge::SetStartLight replaces the picture's own
          // transform (track.vert)
          tmuf_visual_texture raw = *picture;
          raw.has_transform = 0;
          tm_texcoord(&raw, m, i, o->pos, o->uv);
        } else {
          tm_texcoord(picture, m, i, o->pos, o->uv);
        }
        o->color = tm_vertex_color(m, i);
        if (occlusion || !sampler_named(mat, "HorizonClouds")) {
          tm_texcoord(occlusion, m, i, o->pos, o->uv_occlusion);
        } else {
          // the sky dome's horizon clouds run along its first uv set (its
          // second is empty)
          tmuf_visual_texture horizon = *sampler_named(mat, "HorizonClouds");
          horizon.texcoord = 0;
          tm_texcoord(&horizon, m, i, o->pos, o->uv_occlusion);
        }
        o->normal = turn(tm_vertex_normal(m, i), r);
        // (BGRA, as the game's D3DCOLOR; white without a bake)
        o->prelight = bake ? (uint32_t)bake[4 * i] | (uint32_t)bake[4 * i + 1] << 8 | (uint32_t)bake[4 * i + 2] << 16 |
                                 (uint32_t)bake[4 * i + 3] << 24
                           : 0xffffffffu;
        if (b->extra_mode >= TM_EXTRA_GLOW) {
          // the second picture's own coordinates, its transform per frame
          tmuf_visual_texture raw = *b->extra_texture;
          raw.has_transform = 0;
          tm_texcoord(&raw, m, i, o->pos, o->uv3);
        } else {
          tm_texcoord(sampler_named(mat, "Blend3")    ? sampler_named(mat, "Blend3")
                      : sampler_named(mat, "SoilFix") ? sampler_named(mat, "SoilFix")
                                                      : sampler_named(mat, "Mask"),
                      m, i, o->pos, o->uv3);
        }
        o->uv_prelight[0] = o->uv_prelight[1] = 0.f;
        if (corpus && mat->lightmap_uv < m->uv_set_count && m->uv_dims[mat->lightmap_uv] >= 2) {
          const float *uv = m->uv_sets[mat->lightmap_uv] + (size_t)i * m->uv_dims[mat->lightmap_uv];
          o->uv_prelight[0] = corpus->scale[0] * uv[0] + corpus->offset[0];
          o->uv_prelight[1] = corpus->scale[1] * uv[1] + corpus->offset[1];
        }
      }
      free(bake);
    }
    for (uint32_t i = 0; i < m->index_count; i++)
      out->indices[out->index_count + i] = out->vertex_count + m->indices[i];
    // a ranged batch's height and extent follow what it holds (its cell's
    // placements); any other's sphere holds its meshes' spheres
    const float mr = sqrtf(m->bounds[3] * m->bounds[3] + m->bounds[4] * m->bounds[4] + m->bounds[5] * m->bounds[5]);
    if (b->ranged) {
      if (b->index_count == 0) b->center[1] = t[1];
      if (b->radius < CELL * 0.7072f + mr) b->radius = CELL * 0.7072f + mr;
    } else {
      float c[3];
      for (int row = 0; row < 3; row++)
        c[row] = r[row][0] * m->bounds[0] + r[row][1] * m->bounds[1] + r[row][2] * m->bounds[2] + t[row];
      if (b->index_count == 0) {
        memcpy(b->center, c, sizeof c);
        b->radius = mr;
      } else {
        const float d[3] = {c[0] - b->center[0], c[1] - b->center[1], c[2] - b->center[2]};
        const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist + mr > b->radius) {
          if (dist + b->radius <= mr) { // the new one holds the batch
            memcpy(b->center, c, sizeof c);
            b->radius = mr;
          } else {
            const float nr = (dist + b->radius + mr) * 0.5f, k = (nr - b->radius) / dist;
            for (int i = 0; i < 3; i++)
              b->center[i] += d[i] * k;
            b->radius = nr;
          }
        }
      }
    }
    // a vertex animation keeps its first sub-visual's vertices (their
    // coordinates, colours and light; the frames only move them)
    if (b->anim && !b->anim_template) {
      const uint32_t first = m->sub_visuals[0], per = m->vertex_count / m->sub_visual_count;
      b->anim_template = malloc(out->stride * per);
      if (b->anim_template)
        memcpy(b->anim_template, out->vertices + (out->vertex_count + first) * out->stride, out->stride * per);
      else
        b->anim = NULL;
    }
    // the bake's pieces (the lightmapped placements, each on its own)
    if (b->lightmapped && (p->family == FAMILY_PICTURE || p->family == FAMILY_BLOCK)) {
      if (piece_count == piece_cap) {
        const uint32_t cap = piece_cap ? piece_cap * 2 : 1024;
        struct scene_piece *grown = realloc(pieces, sizeof *grown * cap);
        if (grown) pieces = grown, piece_cap = cap;
      }
      if (piece_count < piece_cap) {
        struct scene_piece *sp = &pieces[piece_count++];
        sp->batch = bcount - 1, sp->first = out->index_count, sp->count = m->index_count;
        for (int row = 0; row < 3; row++)
          sp->center[row] = r[row][0] * m->bounds[0] + r[row][1] * m->bounds[1] + r[row][2] * m->bounds[2] + t[row];
        sp->radius = mr;
      }
    }
    // the headlight's receiver pieces (the prelit pictures' placements)
    if (b->prelit && p->family == FAMILY_PICTURE) {
      if (recv_count == recv_cap) {
        const uint32_t cap = recv_cap ? recv_cap * 2 : 1024;
        struct scene_piece *grown = realloc(recv, sizeof *grown * cap);
        if (grown) recv = grown, recv_cap = cap;
      }
      if (recv_count < recv_cap) {
        if (!b->recv_count) b->recv_first = recv_count;
        struct scene_piece *sp = &recv[recv_count++];
        b->recv_count++;
        sp->batch = bcount - 1, sp->first = out->index_count, sp->count = m->index_count;
        for (int row = 0; row < 3; row++)
          sp->center[row] = r[row][0] * m->bounds[0] + r[row][1] * m->bounds[1] + r[row][2] * m->bounds[2] + t[row];
        sp->radius = mr;
      }
    }
    out->vertex_count += m->vertex_count;
    out->index_count += m->index_count;
    b->index_count += m->index_count;
  }
  free(order);
  scene->pieces = pieces;
  scene->piece_count = piece_count;
  scene->recv_pieces = recv;
  scene->recv_piece_count = recv_count;

  // each batch's sphere for culling, from its vertices' box (a vertex
  // animation's grown by its sub-visuals' reach: the flags wave by a few m)
  for (uint32_t i = 0; i < bcount; i++) {
    batch *b = &batches[i];
    const family_buffer *f = &fb[b->family];
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (uint32_t k = 0; k < b->index_count && b->first_index + k < f->index_count; k++) {
      const float *p = (const float *)(f->vertices + (size_t)f->indices[b->first_index + k] * f->stride);
      for (int c = 0; c < 3; c++)
        lo[c] = p[c] < lo[c] ? p[c] : lo[c], hi[c] = p[c] > hi[c] ? p[c] : hi[c];
    }
    if (lo[0] > hi[0]) {
      memcpy(b->cull_center, b->center, sizeof b->cull_center);
      b->cull_radius = FLT_MAX;
      continue;
    }
    float r2 = 0.f;
    for (int c = 0; c < 3; c++) {
      b->cull_center[c] = 0.5f * (lo[c] + hi[c]);
      r2 += 0.25f * (hi[c] - lo[c]) * (hi[c] - lo[c]);
    }
    b->cull_radius = sqrtf(r2) + (b->anim ? 8.f : 0.01f);
  }
  for (int f = 0; f < FAMILY_COUNT; f++) {
    if (fb[f].index_count)
      scene->meshes[f] = tg_mesh_create(game->gpu, fb[f].vertices, fb[f].stride * fb[f].vertex_count, fb[f].indices,
                                        fb[f].index_count);
    free(fb[f].vertices);
    free(fb[f].indices);
  }
  if (!tg_upload_end(game->gpu)) tm_log(game, FT_LOG_ERROR, "Uploading the track: %s", tg_error(game->gpu));
  scene->pictures = table;
  scene->batches = batches;
  scene->batch_count = bcount;
  // the zone's box: its blocks' and terrain's meshes (the decoration's
  // tags have both top bits)
  for (int k = 0; k < 3; k++)
    scene->zone_min[k] = FLT_MAX, scene->zone_max[k] = -FLT_MAX;
  for (uint32_t i = 0; i < v->instance_count; i++) {
    const tmuf_visual_instance *in = &v->instances[i];
    if ((in->block & 0xc0000000u) == 0xc0000000u) continue;
    const tmuf_visual_mesh *m = &v->meshes[in->mesh];
    const float(*r)[3] = in->location.r.m;
    const float t[3] = {in->location.t.x, in->location.t.y, in->location.t.z};
    for (int k = 0; k < 3; k++) {
      const float c = r[k][0] * m->bounds[0] + r[k][1] * m->bounds[1] + r[k][2] * m->bounds[2] + t[k];
      const float h = fabsf(r[k][0]) * m->bounds[3] + fabsf(r[k][1]) * m->bounds[4] + fabsf(r[k][2]) * m->bounds[5];
      if (c - h < scene->zone_min[k]) scene->zone_min[k] = c - h;
      if (c + h > scene->zone_max[k]) scene->zone_max[k] = c + h;
    }
  }
  if (scene->zone_min[0] > scene->zone_max[0])
    for (int k = 0; k < 3; k++)
      scene->zone_min[k] = 0.f, scene->zone_max[k] = 1.f;
  tm_pssm_forget(game);
  scene->mips = v->mips;
  scene->mip_count = v->mip_count;
  scene->mip_now = v->mip_count ? calloc(v->mip_count, sizeof *scene->mip_now) : NULL;
  scene->light = level->light;
  scene->scenery = scenery;
  scene->lightmap = lightmap_of(game, level->track);
  if (scene->lightmap) memcpy(scene->lightgen, scene->light.lightgen, sizeof scene->lightgen);
  // no cache: baked on the first frame, by day (the lightmap's AD mode)
  const tmuf_lightmap *lmap = tmuf_track_lightmap(level->track);
  scene->bake_pending = !scene->lightmap && game->bake && lmap && lmap->corpus_count &&
                        scene->cast_min[0] <= scene->cast_max[0];
  scene->clouds = tm_clouds_create(game, level->track, &scene->pictures);
  scene->water_fog = UINT32_MAX;
  if (scene->water) water_fog_of(game, scene, level->track);
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < scene->pictures.count; i++)
    loaded += scene->pictures.textures[i] != 0;
  tm_log(game, FT_LOG_INFO,
         "Track: %u + %u + %u + %u triangles (pictures, blocks, grass, sea at %.2f) in %u batches, %u sprite visuals, %u pictures; mood %s",
         fb[FAMILY_PICTURE].index_count / 3, fb[FAMILY_BLOCK].index_count / 3, fb[FAMILY_FENCE].index_count / 3,
         fb[FAMILY_WATER].index_count / 3, scene->water_y, bcount, scene->sprite_count, loaded,
         scene->pictures.mood_dir[0] ? scene->pictures.mood_dir : "(none)");
  return scene;
}

void tm_scene_destroy(ft_game *game, tm_scene *scene) {
  if (!scene) return;
  tm_bake_cancel(game); // (a bake still running draws this scene's meshes)
  for (int f = 0; f < FAMILY_COUNT; f++)
    if (scene->meshes[f]) tg_mesh_destroy(game->gpu, scene->meshes[f]);
  tm_picture_table_free(game, &scene->pictures);
  free(scene->sprites);
  free(scene->sprite_vertices);
  if (scene->baked) tg_target_destroy(game->gpu, scene->baked);
  else if (scene->lightmap) tg_texture_destroy(game->gpu, scene->lightmap);
  tm_clouds_destroy(game, scene->clouds);
  if (scene->reflection) tg_target_destroy(game->gpu, scene->reflection);
  if (scene->refraction) tg_target_destroy(game->gpu, scene->refraction);
  if (scene->export_depth) tg_target_destroy(game->gpu, scene->export_depth);
  for (uint32_t i = 0; i < scene->batch_count; i++)
    free(scene->batches[i].anim_template);
  free(scene->batches);
  free(scene->pieces);
  free(scene->recv_pieces);
  free(scene->mip_now);
  free(scene);
}

tg_texture tm_scene_clouds_occlusion(const tm_scene *scene) {
  return scene ? tm_clouds_occlusion_texture(scene->clouds) : 0;
}

// The static scenery around a point in the lightmap (the car's LightFromMap,
// tmuf_car.c): the opaque blocks and pictures whose cells reach it.
bool tm_scene_light_from_map(ft_game *game, const tm_scene *scene, const float centre[3], float radius,
                             const float *uniforms) {
  if (!scene || !scene->lightmap || !game->lfm_block_program || !game->lfm_track_program) return false;
  const tg_state state = {.depth_test = true, .depth_write = true, .cull = TG_CULL_NONE};
  const tg_sampler sampler = {3, 3, 2, 2};
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    if ((b->family != FAMILY_BLOCK && b->family != FAMILY_PICTURE) || b->blended || b->glow || b->sky) continue;
    const float dx = b->center[0] - centre[0], dy = b->center[1] - centre[1], dz = b->center[2] - centre[2];
    const float r = b->radius + radius;
    if (dx * dx + dy * dy + dz * dz > r * r) continue;
    tg_draw(game->gpu, b->family == FAMILY_BLOCK ? game->lfm_block_program : game->lfm_track_program, &state,
            scene->meshes[b->family], b->first_index, b->index_count, &scene->lightmap, &sampler, uniforms);
  }
  return true;
}

// The level each mip draws for this camera (CPlugTreeVisualMip::
// SetQualityFromMinZ; the static ones by their sphere past the near plane,
// CHmsZoneVPacker::CellAreAllMipLow): z scaled by the frustum's half height
// at unit depth. Without the near detail every mip at its lowest level.
static void pick_mip_levels(ft_game *game, tm_scene *scene, const ft_camera *cam) {
  const float view_z[4] = {cam->forward.x, cam->forward.y, cam->forward.z,
                           -(cam->forward.x * cam->eye.x + cam->forward.y * cam->eye.y + cam->forward.z * cam->eye.z)};
  for (uint32_t i = 0; i < scene->mip_count; i++) {
    const tmuf_visual_mip *m = &scene->mips[i];
    if (!game->settings.near_detail) {
      scene->mip_now[i] = m->level_count ? m->level_count - 1 : 0;
      continue;
    }
    const float f = cam->orthographic ? (m->rule == TMUF_VISUAL_MIP_PACKED ? 0.f : 1.f) : tanf(cam->fov_y * 0.5f);
    scene->mip_now[i] = tmuf_visual_mip_level(m, tmuf_visual_mip_z(m, view_z, cam->near_z, f));
  }
}

// In the main view's depth prepass: the opaque scenery (passes 0 and 1),
// but what is alpha tested, animated or the sky; drawn again with its
// colour on that depth, writing none
static bool prepassed(const batch *b) {
  return (b->family == FAMILY_PICTURE || b->family == FAMILY_BLOCK) && !b->blended && !b->glow && !b->sky && !b->anim &&
         b->alpha_cutoff <= 0.f && !b->underground;
}

static bool batch_visible(ft_game *game, const tm_scene *scene, const batch *b, const ft_camera *cam);

// The opaque scenery's depth alone (the prepass's batches), into the
// frame or a depth target
static void depth_draws(ft_game *game, const tm_scene *scene, const ft_camera *cam, const track_uniforms *u,
                        const tm_block_uniforms *bu) {
  if (!game->track_depth_program || !game->block_depth_program) return;
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    if (!prepassed(b) || !scene->meshes[b->family] || !batch_visible(game, scene, b, cam)) continue;
    const bool block = b->family == FAMILY_BLOCK;
    const tg_state depth = {.depth_test = true, .depth_write = true, .no_color_write = true,
                            .cull = block ? TG_CULL_CW : b->double_sided ? TG_CULL_NONE : TG_CULL_CW};
    tg_draw(game->gpu, block ? game->block_depth_program : game->track_depth_program, &depth, scene->meshes[b->family],
            b->first_index, b->index_count, NULL, NULL, block ? (const void *)bu : u);
  }
}

// The view batches are culled against from now on: the side planes of the
// frustum of vp (column major), NULL for none (the shadow maps' casters)
static void cull_set(tm_scene *scene, const float *vp) {
  scene->cull_on = vp != NULL;
  if (!vp) return;
  for (int i = 0; i < 4; i++) {
    const int axis = i >> 1;
    const float sign = (i & 1) ? -1.f : 1.f;
    float l = 0.f;
    for (int c = 0; c < 4; c++) {
      scene->cull[i][c] = vp[c * 4 + 3] + sign * vp[c * 4 + axis];
      if (c < 3) l += scene->cull[i][c] * scene->cull[i][c];
    }
    l = l > 0.f ? 1.f / sqrtf(l) : 0.f;
    for (int c = 0; c < 4; c++)
      scene->cull[i][c] *= l;
  }
}

static bool in_view(const tm_scene *scene, const float c[3], float r) {
  if (!scene->cull_on) return true;
  for (int i = 0; i < 4; i++)
    if (scene->cull[i][0] * c[0] + scene->cull[i][1] * c[1] + scene->cull[i][2] * c[2] + scene->cull[i][3] < -r)
      return false;
  return true;
}

static bool batch_visible(ft_game *game, const tm_scene *scene, const batch *b, const ft_camera *cam) {
  if (!in_view(scene, b->cull_center, b->cull_radius)) return false;
  if (b->mip != UINT32_MAX) {
    // this level of its mip, and of every mip holding it
    uint32_t mip = b->mip, level = b->mip_level;
    for (int depth = 0; mip < scene->mip_count && depth < 64; depth++) {
      if (scene->mip_now[mip] != level) return false;
      level = scene->mips[mip].parent_level;
      mip = scene->mips[mip].parent;
    }
    return true;
  }
  if (!b->ranged) return true;
  if (!game->settings.near_detail && b->lod_far < FLT_MAX) return false;
  const float dx = b->center[0] - cam->eye.x, dy = b->center[1] - cam->eye.y, dz = b->center[2] - cam->eye.z;
  const float d = sqrtf(dx * dx + dy * dy + dz * dz);
  // the game picks a level by the distance to the placement; a cell holds
  // placements up to `radius` from its centre
  return !(d - b->radius >= b->lod_far || d + b->radius < b->lod_near);
}

// whether a mip level shows this frame (and every mip holding it)
static bool mip_shown(const tm_scene *scene, uint32_t mip, uint32_t level) {
  for (int depth = 0; mip < scene->mip_count && depth < 64; depth++) {
    if (scene->mip_now[mip] != level) return false;
    level = scene->mips[mip].parent_level;
    mip = scene->mips[mip].parent;
  }
  return true;
}

// The sprite visuals (Rally's tree crowns, the trace's vs_26 / ps_21): each
// sprite a quad facing the camera (CLoadGeomDynaSprite::LoadSprite), alpha
// tested at 128, not culled, unfogged; lit by k = sat(N.-L + 0.5), N here
// from the crown's middle to the corner (the game's per-corner normals were
// not traced)
static void draw_sprites(ft_game *game, tm_scene *scene, const ft_camera *cam, const track_uniforms *base,
                         const tg_sampler *samplers) {
  if (!scene->sprite_count) return;
  track_uniforms u = *base;
  u.scenery[0] = 2.f;
  u.color_scale[0] = 1.f, u.color_scale[1] = 1.f, u.color_scale[2] = 0.f, u.color_scale[3] = 0.f;
  u.blend = 0.f;
  memset(u.stripe_gen, 0, sizeof u.stripe_gen);
  u.blend3[0] = 0.f;
  u.alpha_cutoff = 128.5f / 255.f;
  u.extra[0] = 0.f;
  u.proj[3] = 0.f;
  u.pssm_view[3] = 0.f;
  u.ao[0] = 0.f;
  u.spec_cube[0] = u.spec_cube[1] = u.spec_cube[3] = 0.f;
  u.lawn[0] = 0.f;
  u.water[1] = 0.f;
  u.fog_color[3] = 0.f;
  // GbxLightAmbient and GbxLightDirRgb0
  memcpy(u.blend_mask, scene->light.ambient, sizeof(float) * 3);
  memcpy(u.blend2 + 4, scene->light.sun_rgb, sizeof(float) * 3);
  // right = forward x up, up = right x forward (tmuf_clouds.c)
  float f[3] = {cam->forward.x, cam->forward.y, cam->forward.z}, cu[3] = {cam->up.x, cam->up.y, cam->up.z};
  float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (int a = 0; a < 3; a++) f[a] /= fl > 0.f ? fl : 1.f;
  float rt[3] = {f[1] * cu[2] - f[2] * cu[1], f[2] * cu[0] - f[0] * cu[2], f[0] * cu[1] - f[1] * cu[0]};
  const float rl = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
  for (int a = 0; a < 3; a++) rt[a] /= rl > 0.f ? rl : 1.f;
  const float upv[3] = {rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0]};
  const struct { float x, y, z; } right = {rt[0], rt[1], rt[2]}, up = {upv[0], upv[1], upv[2]},
                                  L = {scene->light.sun_dir[0], scene->light.sun_dir[1], scene->light.sun_dir[2]};
  const tg_state state = {.depth_test = true, .depth_write = true, .cull = TG_CULL_NONE};
  for (uint32_t i = 0; i < scene->sprite_count; i++) {
    const struct scene_sprites *sp = &scene->sprites[i];
    if (sp->mip != UINT32_MAX ? !mip_shown(scene, sp->mip, sp->mip_level) : false) continue;
    if (sp->mip == UINT32_MAX && (sp->lod_near > 0.f || sp->lod_far < FLT_MAX)) {
      const float dx = sp->centre[0] - cam->eye.x, dy = sp->centre[1] - cam->eye.y, dz = sp->centre[2] - cam->eye.z;
      const float d = sqrtf(dx * dx + dy * dy + dz * dz);
      if (d >= sp->lod_far || d < sp->lod_near) continue;
    }
    const tmuf_visual_mesh *m = sp->mesh;
    const float(*R)[3] = sp->location.r.m;
    // the camera's axes in the mesh's frame
    const float r[3] = {R[0][0] * right.x + R[1][0] * right.y + R[2][0] * right.z,
                        R[0][1] * right.x + R[1][1] * right.y + R[2][1] * right.z,
                        R[0][2] * right.x + R[1][2] * right.y + R[2][2] * right.z};
    const float uu[3] = {R[0][0] * up.x + R[1][0] * up.y + R[2][0] * up.z, R[0][1] * up.x + R[1][1] * up.y + R[2][1] * up.z,
                         R[0][2] * up.x + R[1][2] * up.y + R[2][2] * up.z};
    const uint32_t need = m->vertex_count * 6u;
    if (need > scene->sprite_vertex_cap) {
      tm_track_vertex *g = realloc(scene->sprite_vertices, sizeof *g * need);
      if (!g) return;
      scene->sprite_vertices = g, scene->sprite_vertex_cap = need;
    }
    tm_track_vertex *o = scene->sprite_vertices;
    memset(o, 0, sizeof *o * need);
    for (uint32_t k = 0; k < m->vertex_count; k++) {
      float corners[4][3], uv[4][2];
      tmuf_cloud_sprite_quad(m, k, r, uu, corners, uv);
      tm_track_vertex q[4];
      memset(q, 0, sizeof q);
      for (int c = 0; c < 4; c++) {
        float w[3];
        for (int row = 0; row < 3; row++)
          w[row] = R[row][0] * corners[c][0] + R[row][1] * corners[c][1] + R[row][2] * corners[c][2] +
                   (&sp->location.t.x)[row];
        memcpy(q[c].pos, w, sizeof w);
        q[c].uv[0] = uv[c][0], q[c].uv[1] = uv[c][1];
        float n[3] = {w[0] - sp->centre[0], w[1] - sp->centre[1], w[2] - sp->centre[2]};
        const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int a = 0; a < 3; a++) n[a] /= nl > 0.f ? nl : 1.f;
        float kk = -(n[0] * L.x + n[1] * L.y + n[2] * L.z) + 0.5f;
        kk = kk < 0.f ? 0.f : kk > 1.f ? 1.f : kk;
        const uint32_t g = (uint32_t)lrintf(kk * 255.f);
        q[c].color = 0xff000000u | g << 16 | g << 8 | g;
      }
      static const int tri[6] = {0, 1, 2, 2, 1, 3};
      for (int t = 0; t < 6; t++)
        o[k * 6 + t] = q[tri[t]];
    }
    tg_texture textures[TM_TRACK_TEXTURES] = {scene->pictures.textures[sp->picture]};
    if (sp->clouds != UINT32_MAX) textures[9] = scene->pictures.textures[sp->clouds];
    tg_draw_dynamic(game->gpu, game->track_program, &state, o, need, textures, samplers, &u);
  }
}

// The light of the time of day (the map's weather, tmuf_weather.c)
static void mood_uniforms(const tm_scene *scene, const ft_camera *cam, double seconds, tm_block_uniforms *u) {
  const tm_light *l = &scene->light;
  memcpy(u->view_proj, cam->view_proj, sizeof u->view_proj);
  u->eye[0] = cam->eye.x, u->eye[1] = cam->eye.y, u->eye[2] = cam->eye.z, u->eye[3] = 1.f;
  memcpy(u->light_dir, l->sun_dir, sizeof l->sun_dir);
  memcpy(u->light_rgb, l->sun_rgb, sizeof l->sun_rgb);
  u->light_rgb[3] = 1.f;
  tm_light_clouds(l, seconds, u->clouds_u, u->clouds_v);
  // linear fog over the view depth
  const float fwd[3] = {cam->forward.x, cam->forward.y, cam->forward.z};
  const float start = l->fog_start, end = l->fog_end, k = end > start ? 1.f / (end - start) : 0.f;
  for (int i = 0; i < 3; i++)
    u->fog[i] = -fwd[i] * k;
  u->fog[3] = (end + fwd[0] * cam->eye.x + fwd[1] * cam->eye.y + fwd[2] * cam->eye.z) * k;
  memcpy(u->fog_color, l->fog_rgb, sizeof l->fog_rgb);
  u->fog_color[3] = 1.f;
}

// The blocks (the Stadium's pools and what stands around them) into the
// water's refraction (water 1: under the water, in its fog) or reflection
// (-1: above it, mirrored by vp, its eye and fog along the mirrored camera)
static void blocks_into_water(ft_game *game, const tm_scene *scene, const ft_camera *cam, const tm_block_uniforms *bu,
                              const float vp[16], float water, const tg_state *state, tg_texture water_fog) {
  static const tg_sampler samplers[BLOCK_MAPS + 4] = {
      TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP,
      TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP,
      {3, 3, 1, 0},     {3, 3, 1, 0}};
  tm_block_uniforms u = *bu;
  memcpy(u.view_proj, vp, sizeof u.view_proj);
  // the sun's shadow from the view's maps in the refraction (A06's pool
  // trace: the floor's shader, ps_5, reads a shadow mask there too), none
  // in the reflection (mirrored, outside the cascades; the game's has none)
  if (water < 0.f) u.pssm_view[3] = 0.f;
  const float h = scene->water_y;
  if (water < 0.f) {
    u.eye[1] = 2.f * h - bu->eye[1];
    u.fog[3] = bu->fog[3] + 2.f * h * bu->fog[1];
    u.fog[1] = -bu->fog[1];
  }
  u.water[0] = h, u.water[1] = water, u.water[2] = scene->day_time, u.water[3] = 1.f / 30.f;
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    // (the blended and alpha tested ones not in the reflection, as the
    // pictures)
    if (b->family != FAMILY_BLOCK || b->anim || (water < 0.f && (!b->reflected || b->blended || b->alpha_cutoff > 0.f)) ||
        !batch_visible(game, scene, b, cam))
      continue;
    tg_texture textures[BLOCK_MAPS + 4] = {0};
    for (int m = 0; m < BLOCK_MAPS; m++)
      textures[m] = b->maps[m] != UINT32_MAX ? scene->pictures.textures[b->maps[m]] : 0;
    textures[BLOCK_MAPS] = scene->lightmap;
    textures[BLOCK_MAPS + 1] = water_fog;
    if (u.pssm_view[3] > 0.f) {
      textures[BLOCK_MAPS + 2] = tm_pssm_static_texture(game);
      textures[BLOCK_MAPS + 3] = tm_pssm_atlas_texture(game);
    }
    u.params[0] = b->alpha_cutoff;
    u.params[1] = scene->lightmap && b->in_lightmap ? 1.f : 0.f;
    u.params[2] = b->occlusion_only ? 1.f : 0.f;
    u.params[3] = (float)b->variant;
    tg_draw(game->gpu, game->block_program, state, scene->meshes[FAMILY_BLOCK], b->first_index, b->index_count,
            textures, samplers, &u);
  }
}

// The sea's targets' size (CVisionViewport::TextureAdd): 256^2 for a screen
// of at most 800 x 600, else 512^2; made again when it changes
static tg_target *water_target(ft_game *game, tg_target **t, const ft_camera *cam) {
  const uint32_t size = cam->viewport.x <= 800.f && cam->viewport.y <= 600.f ? 256u : 512u;
  if (*t && tg_target_width(*t) != size) tg_target_destroy(game->gpu, *t), *t = NULL;
  if (!*t) *t = tg_target_create_mips(game->gpu, size, size);
  return *t;
}

// The sea's refraction, as the game draws it: into a picture of its own,
// cleared transparent (the sea shows its deepest fog there), the opaque
// ground under the water in the water's fog.
// The sea's refraction (sea_reflection_spec.md §2.2): what is under the water,
// drawn as in the view into its own target (mipmapped: the sea samples
// it perturbed), the view's tangents widened 1.05, cleared to 0xfd36444f
static bool refraction(ft_game *game, tm_scene *scene, const ft_render_frame *frame, track_uniforms *u,
                       const tm_block_uniforms *bu, tg_texture water_fog, float vp[16], bool pssm) {
  const ft_camera *cam = &frame->state.camera;
  if (!water_target(game, &scene->refraction, cam)) return false;
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++)
      vp[col * 4 + row] = (row < 2 ? 1.f / 1.05f : 1.f) * cam->view_proj[col * 4 + row];
  float saved[16];
  memcpy(saved, u->view_proj, sizeof saved);
  memcpy(u->view_proj, vp, sizeof saved);
  cull_set(scene, vp);
  const float clear[4] = {0x36 / 255.f, 0x44 / 255.f, 0x4f / 255.f, 0xfd / 255.f};
  tg_target_begin(game->gpu, scene->refraction, clear);
  const tg_state opaque = {.depth_test = true, .depth_write = true, .cull = TG_CULL_NONE};
  const tg_sampler samplers[TM_TRACK_TEXTURES] = {TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,
                                   TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP,
                                   TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP,
                                   {3, 3, 1, 0},     {3, 3, 1, 0},     {3, 3, 1, 0},     TG_SAMPLER_WRAP};
  u->water[1] = 1.f;
  u->lawn[0] = 0.f;
  u->fog_color[3] = 0.f;
  // (none of the sky's horizon nor the headlight's projector under the water)
  const float saved_horizon = u->horizon0[3], saved_proj = u->proj[3];
  u->horizon0[3] = 0.f;
  u->proj[3] = 0.f;
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    if (b->family != FAMILY_PICTURE || b->blended || b->glow || b->sky || !batch_visible(game, scene, b, cam)) continue;
    tg_texture textures[TM_TRACK_TEXTURES] = {b->texture != UINT32_MAX ? scene->pictures.textures[b->texture] : 0, 0, 0, 0, 0, 0,
                               water_fog};
    memset(u->stripe_gen, 0, sizeof u->stripe_gen);
    if (b->blend.has && b->blend.stripe != UINT32_MAX) {
      textures[4] = scene->pictures.textures[b->blend.stripe];
      memcpy(u->stripe_gen, b->blend.stripe_gen, sizeof u->stripe_gen);
      memcpy(u->stripe, b->blend.stripe_transform, sizeof u->stripe);
    }
    u->blend3[0] = 0.f;
    if (b->blend.has && b->blend.third != UINT32_MAX) {
      textures[5] = scene->pictures.textures[b->blend.third];
      u->blend3[0] = 1.f;
    }
    u->color_scale[2] = b->occlusion != UINT32_MAX ? 1.f : 0.f;
    if (b->occlusion != UINT32_MAX) textures[3] = scene->pictures.textures[b->occlusion];
    u->alpha_cutoff = b->alpha_cutoff;
    u->blend = b->blend.has ? 1.f : 0.f;
    // the scenery lit as in the view (the game's refraction draws it so)
    u->scenery[0] = b->prelit ? 1.f : 0.f;
    u->scenery[3] = b->double_sided ? 1.f : 0.f;
    u->spec_cube[0] = u->spec_cube[1] = u->spec_cube[3] = 0.f;
    if (b->prelit && b->clouds != UINT32_MAX) textures[9] = scene->pictures.textures[b->clouds];
    // the sun's shadow from the static map alone, no AO (env_light_spec §7)
    u->pssm_view[3] = pssm && b->prelit ? 1.f : 0.f;
    u->ao[0] = 0.f;
    if (u->pssm_view[3] > 0.f) textures[12] = tm_pssm_static_texture(game);
    u->extra[0] = b->extra_mode == TM_EXTRA_SELF_ILLUM ? (float)b->extra_mode : 0.f;
    u->extra[1] = (float)b->kind;
    if (b->extra_mode == TM_EXTRA_SELF_ILLUM) textures[10] = scene->pictures.textures[b->extra];
    if (b->blend.has) {
      textures[1] = scene->pictures.textures[b->blend.second];
      textures[2] = scene->pictures.textures[b->blend.mask];
      memcpy(u->gen, b->blend.gen, sizeof u->gen);
      memcpy(u->blend2, b->blend.blend2, sizeof u->blend2);
      memcpy(u->blend_mask, b->blend.blend_mask, sizeof u->blend_mask);
    }
    tg_draw(game->gpu, game->track_program, &opaque, scene->meshes[FAMILY_PICTURE], b->first_index, b->index_count,
            textures, samplers, u);
  }
  blocks_into_water(game, scene, cam, bu, vp, 1.f, &opaque, water_fog);
  // and the car, what of it is under the water (its shaders are refracted)
  if (game->settings.draw_car && !game->camera_inside && frame->world && game->level && game->level->car) {
    ft_render_frame widened = *frame;
    memcpy(widened.state.camera.view_proj, vp, sizeof widened.state.camera.view_proj);
    tm_car_render_refraction(game, game->level->car, &widened, scene->water_y, scene->day_time, water_fog);
  }
  u->water[1] = 0.f;
  u->horizon0[3] = saved_horizon;
  u->proj[3] = saved_proj;
  memcpy(u->view_proj, saved, sizeof saved);
  cull_set(scene, cam->view_proj);
  tg_target_end(game->gpu);
  return true;
}

// The sun's shadow maps this frame (tmuf_pssm.c): the static one when the
// light changed, the cascades every frame, from the opaque scenery that
// reaches them (the alpha tested ones through their picture's holes)
static bool pssm_render(ft_game *game, tm_scene *scene, const ft_camera *cam, track_uniforms *u, tm_car_model *car) {
  const bool statics = tm_pssm_setup(game, scene->light.sun_dir, scene->zone_min, scene->zone_max, cam, u);
  // (what is out of view casts into it too: the cascades' own reach)
  cull_set(scene, NULL);
  for (int pass = statics ? 0 : 1; pass < 2; pass++) {
    if (!tm_pssm_begin(game, pass == 0)) {
      cull_set(scene, cam->view_proj);
      return false;
    }
    for (int map = pass == 0 ? -1 : 0; map < (pass == 0 ? 0 : TM_PSSM_CASCADES); map++) {
      static const char *const map_names[TM_PSSM_CASCADES + 1] = {"pssm_static", "pssm_c0", "pssm_c1", "pssm_c2",
                                                                  "pssm_c3", "pssm_c4"};
      tm_prof(game, map_names[map + 1]);
      for (uint32_t i = 0; i < scene->batch_count; i++) {
        const batch *b = &scene->batches[i];
        // (every static object of the zone: the blended foliage too, its
        // texels kept above 51 / 255 as the alpha-tested ones', env_light_spec §3.4)
        if ((b->family != FAMILY_PICTURE && b->family != FAMILY_BLOCK) || !scene->meshes[b->family] || !b->zone ||
            b->sky || b->glow || b->anim ||
            !batch_visible(game, scene, b, cam) || !tm_pssm_reaches(game, map, b->center, b->radius))
          continue;
        const bool block = b->family == FAMILY_BLOCK;
        const uint32_t pic = block ? b->maps[MAP_DIFFUSE] : b->texture;
        const tg_texture picture = pic != UINT32_MAX ? scene->pictures.textures[pic] : 0;
        tm_pssm_cast(game, map, scene->meshes[b->family], b->first_index, b->index_count, picture,
                     b->alpha_cutoff > 0.f || b->blended ? 51.f / 255.f : 0.f, b->double_sided, block);
      }
    }
    // and the car, into the cascades
    tm_prof(game, "pssm_car");
    if (pass == 1 && car)
      for (int map = 0; map < TM_PSSM_CASCADES; map++)
        tm_car_pssm_cast(game, car, map);
    tm_pssm_end(game);
  }
  cull_set(scene, cam->view_proj);
  return tm_pssm_static_texture(game) && tm_pssm_atlas_texture(game);
}

// The ambient occlusion this frame (tmuf_ssao.c): the depth of the
// receivers (the opaque vertex-lit scenery, not the double-sided foliage)
static bool ao_render(ft_game *game, tm_scene *scene, const ft_camera *cam, const tmuf_ambient_occlusion *params) {
  if (!tm_ssao_begin(game, cam)) return false;
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    if (b->family != FAMILY_PICTURE || !b->prelit || b->blended || b->double_sided || b->sky || b->glow || b->anim ||
        !batch_visible(game, scene, b, cam))
      continue;
    const tg_texture picture = b->texture != UINT32_MAX ? scene->pictures.textures[b->texture] : 0;
    tm_ssao_depth(game, cam, scene->meshes[FAMILY_PICTURE], b->first_index, b->index_count, picture,
                  b->alpha_cutoff > 0.f ? 0.5f : 0.f, false);
  }
  tm_ssao_end(game, cam, params);
  return tm_ssao_texture(game) != 0;
}

// A vertex animation this frame (CMotionTrackTree::UpdateTree,
// CFuncTreeSubVisualSequence::Compute: tmuf_visual_sequence): the two
// sub-visuals it is between at ms on the engine clock, tweened as the
// game's Tween shaders do (positions and normals, the normals
// renormalised), drawn from this frame's vertices
static void anim_draw(ft_game *game, const batch *b, uint32_t ms, tg_program *program, const tg_state *state,
                      const tg_texture *textures, const tg_sampler *samplers, const void *u) {
  const tmuf_visual_sequence *s = b->anim->sequence;
  const tmuf_visual_mesh *m = b->anim_mesh;
  const uint32_t period = (uint32_t)lrint(1000.0 * (double)s->period);
  if (!period || !b->anim_template || s->key_count < 2) return;
  const uint32_t offset = (uint32_t)lrint((double)period * (double)s->phase);
  const float t = (float)((ms + offset) % period) / (float)period;
  const float tk = t * s->key_times[s->key_count - 1];
  uint32_t a = s->key_count - 2;
  while (a > 0 && s->key_times[a] > tk)
    a--;
  const float span = s->key_times[a + 1] - s->key_times[a];
  float w = span > 0.f ? (tk - s->key_times[a]) / span * 255.f : 0.f;
  w = rintf(w < 0.f ? 0.f : w > 255.f ? 255.f : w) / 255.f;
  const uint32_t i0 = s->key_values[a], i1 = s->key_values[a + 1];
  if (i0 >= m->sub_visual_count || i1 >= m->sub_visual_count) return;
  const uint32_t v0 = m->sub_visuals[3 * i0], v1 = m->sub_visuals[3 * i1];
  const uint32_t first_index = m->sub_visuals[3 * i0 + 1], count = m->sub_visuals[3 * i0 + 2];
  const uint32_t per = m->vertex_count / m->sub_visual_count;
  if (!count || first_index + count > m->index_count || v0 + per > m->vertex_count || v1 + per > m->vertex_count) return;
  // the family's vertex: its position first, its normal where it keeps it
  const bool block = b->family == FAMILY_BLOCK;
  const size_t stride = block ? sizeof(tm_block_vertex) : sizeof(track_vertex);
  const size_t normal_at = block ? offsetof(tm_block_vertex, normal) : offsetof(track_vertex, normal);
  uint8_t *out = malloc(stride * count);
  if (!out) return;
  const float(*r)[3] = b->anim->location.r.m;
  const float tr[3] = {b->anim->location.t.x, b->anim->location.t.y, b->anim->location.t.z};
  for (uint32_t k = 0; k < count; k++) {
    const uint32_t i = m->indices[first_index + k];
    uint8_t *o = out + stride * k;
    if (i >= per) {
      memset(o, 0, stride);
      continue;
    }
    memcpy(o, b->anim_template + stride * i, stride);
    float p0[3], p1[3], n0[3], n1[3], p[3], n[3];
    memcpy(p0, m->vertices + (size_t)(v0 + i) * m->vertex_stride, sizeof p0);
    memcpy(p1, m->vertices + (size_t)(v1 + i) * m->vertex_stride, sizeof p1);
    unpack_normal(tm_vertex_normal(m, v0 + i), n0);
    unpack_normal(tm_vertex_normal(m, v1 + i), n1);
    for (int c = 0; c < 3; c++)
      p[c] = (1.f - w) * p0[c] + w * p1[c], n[c] = (1.f - w) * n0[c] + w * n1[c];
    const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float world[3];
    for (int row = 0; row < 3; row++) {
      world[row] = r[row][0] * p[0] + r[row][1] * p[1] + r[row][2] * p[2] + tr[row];
      n0[row] = nl > 0.f ? (r[row][0] * n[0] + r[row][1] * n[1] + r[row][2] * n[2]) / nl : 0.f;
    }
    memcpy(o, world, sizeof world);
    const uint32_t packed = pack_normal(n0);
    memcpy(o + normal_at, &packed, sizeof packed);
  }
  tg_draw_dynamic(game->gpu, program, state, out, count, textures, samplers, u);
  free(out);
}

// CGameCtnChallenge::SetStartLight: the start lights' phase is 0 (red) for
// "3" and "2", 1/2 (yellow) for "1", 1 (green) from "Go!" on; their
// picture's v is scaled by 1/3 and moved by 2/3 of the phase. (Only a race
// calls it, CTrackManiaRace::UpdateCountDownIndex: a replay shot to video
// has none and its lights keep the stopped motion's 0, red; the reference
// frames' TM_TEST_TICK draws them so)
static float start_phase(const ft_render_frame *frame) {
  if (getenv("TM_TEST_TICK")) return 0.f;
  const uint32_t ms = frame->world ? (uint32_t)frame->world->w.tick * TMUF_TICK_MS : 0u;
  const int index = ms < TMUF_RACE_START_MS ? (int)(ms * 3u / TMUF_RACE_START_MS) : 3;
  return index < 2 ? 0.f : index == 2 ? 0.5f : 1.f;
}

// A texture's coordinates transform at a time (a b c d, tx ty): its
// shader function's (CFuncShaderLayerUV::ComputeInternal at
// t = frac(seconds / period), CFuncShader::ComputeFromTime), else its own
void tm_texture_uv(const tmuf_visual_texture *t, double seconds, float out[8]) {
  float m[4] = {1.f, 0.f, 0.f, 1.f}, tr[2] = {0.f, 0.f};
  if (t->has_anim && t->anim_auto) {
    const double period = t->anim_period > 0.f ? (double)t->anim_period : 1.0;
    double ph = fmod(seconds / period, 1.0);
    if (ph < 0.0) ph += 1.0;
    const float s = (float)ph;
    switch (t->anim_type) {
    case 0: // translation
      tr[0] = t->anim_start[0] + s * t->anim_delta[0], tr[1] = t->anim_start[1] + s * t->anim_delta[1];
      break;
    case 1: { // a circle
      const float a = 6.28318531f * s;
      tr[0] = t->anim_start[0] + cosf(a) * t->anim_delta[0], tr[1] = t->anim_start[1] + sinf(a) * t->anim_delta[1];
      break;
    }
    case 2: { // a rotation about start
      const float a = (t->anim_delta[0] + s * (t->anim_delta[1] - t->anim_delta[0])) * 0.0174532925f;
      const float c = cosf(a), sn = sinf(a);
      m[0] = c, m[1] = sn, m[2] = -sn, m[3] = c;
      tr[0] = t->anim_start[0] - (c * t->anim_start[0] - sn * t->anim_start[1]);
      tr[1] = t->anim_start[1] - (sn * t->anim_start[0] + c * t->anim_start[1]);
      break;
    }
    case 3: { // sub-textures
      const uint32_t count = t->anim_cells[0] ? t->anim_cells[0] : 1u, cols = t->anim_cells[1] ? t->anim_cells[1] : 1u;
      const uint32_t rows = t->anim_cells[2] ? t->anim_cells[2] : (count + cols - 1u) / cols;
      const uint32_t cell = (uint32_t)floorf((float)count * s * 0.99999f), row = cell / cols;
      m[0] = 1.f / (float)cols, m[3] = 1.f / (float)rows;
      tr[0] = (float)(cell % cols) / (float)cols;
      tr[1] = t->anim_flip_v ? 1.f - (float)(row + 1u) / (float)rows : (float)row / (float)rows;
      break;
    }
    case 4: // translation and scale
      m[0] = t->anim_scale[0], m[3] = t->anim_scale[1];
      tr[0] = t->anim_start[0] + s * t->anim_delta[0], tr[1] = t->anim_start[1] + s * t->anim_delta[1];
      break;
    case 6: // a scale
      m[0] = t->anim_start[0] + s * t->anim_delta[0], m[3] = t->anim_start[1] + s * t->anim_delta[1];
      tr[0] = -m[0] * t->anim_scale[0], tr[1] = -m[3] * t->anim_scale[1];
      break;
    default:
      if (t->has_transform) {
        float x[6];
        stored_transform(t, x);
        memcpy(m, x, sizeof m), memcpy(tr, x + 4, sizeof tr);
      }
      break;
    }
  } else if (t->has_transform) {
    float x[6];
    stored_transform(t, x);
    memcpy(m, x, sizeof m);
    memcpy(tr, x + 4, sizeof tr);
  }
  memcpy(out, m, sizeof m);
  memcpy(out + 4, tr, sizeof tr);
  out[6] = out[7] = 0.f;
}

// A second picture's coordinates this frame (a b c d, tx ty: track.vert):
// its shader function's (tm_texture_uv on the clock the sea's time runs
// on), the start lights' row by the countdown, else its own transform
static void extra_transform(const tmuf_visual_texture *t, const ft_render_frame *frame, float out[8]) {
  const char *file = t->file ? t->file : t->pack_file ? t->pack_file : "";
  if (strstr(file, "TrafficLightGlow")) {
    // driven by the start (SetStartLight): three rows of lamps
    const float tl[8] = {1.f, 0.f, 0.f, 1.f / 3.f, 0.f, start_phase(frame) * (2.f / 3.f), 0.f, 0.f};
    memcpy(out, tl, sizeof tl);
    return;
  }
  tm_texture_uv(t, (double)frame->tick * 0.01, out);
}

// The sea's reflection (TexRender_Water_PlaneR): the scenery its shaders
// let in (Reflected), seen from the camera mirrored about the water (vp:
// its view_proj, tangents widened by 1.05), what is below the water left
// out, into a 512 square cleared to the fog's colour; lit as in the view,
// without shadows (sea_reflection_spec.md)
static bool reflection(ft_game *game, tm_scene *scene, const ft_camera *cam, const track_uniforms *u,
                       const tm_block_uniforms *bu, const float vp[16], tg_texture water_fog, uint32_t clouds_ms) {
  // with its mip chain: the sea samples it biased by the distance
  if (!water_target(game, &scene->reflection, cam)) return false;
  const float clear[4] = {u->fog_color[0], u->fog_color[1], u->fog_color[2], 1.f};
  tg_target_begin(game->gpu, scene->reflection, clear);
  // a mirror: what faces the camera turns the other way on screen
  const tg_state opaque = {.depth_test = true, .depth_write = true, .cull = getenv("TM_REFL_CW") ? TG_CULL_CW : TG_CULL_CCW};
  const tg_state sky_state = {.depth_test = true, .cull = TG_CULL_CCW};
  const tg_sampler samplers[TM_TRACK_TEXTURES] = {TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,
                                   TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP,
                                   TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP,
                                   {3, 3, 1, 0},     {3, 3, 1, 0},     {3, 3, 1, 0},     TG_SAMPLER_WRAP};
  track_uniforms r = *u;
  memcpy(r.view_proj, vp, sizeof r.view_proj);
  // the mirrored eye, and the fog along the mirrored camera
  const float h = scene->water_y;
  r.eye[1] = 2.f * h - u->eye[1];
  r.fog[3] = u->fog[3] + 2.f * h * u->fog[1];
  r.fog[1] = -u->fog[1];
  r.water[1] = -1.f; // what is below the water left out (track.frag)
  // the sky first (it writes no depth), its clouds (the game's reflection
  // draws them: A06's pool), then the scenery
  for (int sky = 1; sky >= 0; sky--) {
    if (sky == 0 && scene->clouds) {
      ft_camera m = *cam;
      memcpy(m.view_proj, vp, sizeof m.view_proj);
      m.eye.y = 2.f * scene->water_y - cam->eye.y;
      m.forward.y = -cam->forward.y;
      m.up.y = -cam->up.y;
      tm_clouds_render(game, scene->clouds, &scene->light, &m, clouds_ms);
    }
    // (neither the blended nor the alpha tested surfaces: A06's pool trace
    // has no draw with either in its reflection, StadiumWarpAuventAlpha's
    // girders and the glass left out)
    for (uint32_t i = 0; i < scene->batch_count; i++) {
      const batch *b = &scene->batches[i];
      if (b->family != FAMILY_PICTURE || !b->reflected || b->underground || b->sky != (sky == 1) || b->glow || b->anim ||
          (b->blended && !b->sky) || b->alpha_cutoff > 0.f || !batch_visible(game, scene, b, cam))
        continue;
      tg_texture textures[TM_TRACK_TEXTURES] = {b->texture != UINT32_MAX ? scene->pictures.textures[b->texture] : 0, 0, 0, 0, 0, 0,
                                 water_fog};
      r.blend3[0] = r.blend3[1] = r.blend3[2] = r.blend3[3] = 0.f;
      r.fog_color[3] = b->blended || b->sky ? 0.f : 1.f;
      memset(r.stripe_gen, 0, sizeof r.stripe_gen);
      if (b->blend.has && b->blend.stripe != UINT32_MAX) {
        textures[4] = scene->pictures.textures[b->blend.stripe];
        memcpy(r.stripe_gen, b->blend.stripe_gen, sizeof(float) * 3);
        memcpy(r.stripe, b->blend.stripe_transform, sizeof r.stripe);
      }
      if (b->blend.has && b->blend.third != UINT32_MAX) {
        textures[5] = scene->pictures.textures[b->blend.third];
        r.blend3[0] = 1.f;
      } else if (b->mask != UINT32_MAX) {
        textures[5] = scene->pictures.textures[b->mask];
        r.blend3[0] = 1.f;
      }
      r.color_scale[2] = b->occlusion != UINT32_MAX ? 1.f : 0.f;
      if (b->occlusion != UINT32_MAX) textures[3] = scene->pictures.textures[b->occlusion];
      r.horizon0[3] = 0.f;
      if (b->horizon != UINT32_MAX) {
        textures[3] = scene->pictures.textures[b->horizon];
        memcpy(r.horizon0, scene->light.clouds_min, sizeof scene->light.clouds_min);
        memcpy(r.horizon1, scene->light.clouds_max, sizeof scene->light.clouds_max);
        r.horizon0[3] = 1.f;
        r.horizon1[3] = 0.f;
      }
      r.alpha_cutoff = b->alpha_cutoff;
      r.blend = b->blend.has ? 1.f : 0.f;
      if (b->blend.has) {
        textures[1] = scene->pictures.textures[b->blend.second];
        textures[2] = scene->pictures.textures[b->blend.mask];
        memcpy(r.gen, b->blend.gen, sizeof r.gen);
        memcpy(r.blend2, b->blend.blend2, sizeof r.blend2);
        memcpy(r.blend_mask, b->blend.blend_mask, sizeof r.blend_mask);
      }
      r.scenery[0] = b->prelit ? 1.f : 0.f;
      r.scenery[3] = b->double_sided ? 1.f : 0.f;
      r.spec_cube[0] = r.spec_cube[1] = r.spec_cube[3] = 0.f;
      if (b->prelit && b->clouds != UINT32_MAX) textures[9] = scene->pictures.textures[b->clouds];
      r.extra[0] = (float)b->extra_mode;
      r.extra[1] = (float)b->kind;
      if (b->extra_mode != TM_EXTRA_NONE) {
        textures[10] = scene->pictures.textures[b->extra];
        memset(r.extra_t, 0, sizeof r.extra_t);
        r.extra_t[0] = r.extra_t[3] = 1.f;
        if (b->extra_texture->has_transform) stored_transform(b->extra_texture, r.extra_t);
      }
      tg_sampler rs[TM_TRACK_TEXTURES];
      memcpy(rs, samplers, sizeof rs);
      if (b->gradient) rs[0] = TG_SAMPLER_CLAMP;
      // the sky blended as in the view, onto the fog colour it was cleared to
      // (the game's GradientV: ONE / SRCALPHA, near white over it)
      tg_state sky_blended = sky_state;
      if (b->sky && b->blended) sky_blended.blend_src = b->blend_src, sky_blended.blend_dst = b->blend_dst;
      // (the sky not clipped at the water: the game's reflection shows it
      // all, A06's pool trace)
      r.water[1] = b->sky ? 0.f : -1.f;
      tg_draw(game->gpu, game->track_program, b->sky ? &sky_blended : &opaque, scene->meshes[FAMILY_PICTURE],
              b->first_index, b->index_count, textures, rs, &r);
    }
  }
  blocks_into_water(game, scene, cam, bu, vp, -1.f, &opaque, water_fog);
  tg_target_end(game->gpu);
  return true;
}

// the lightmap baked into this frame (tmuf_bake.c; at Sunset and Night
// over the next ones): the casters and receivers are the scene's own
// batches, and each lightmapped placement on its own for the spots
static void baked(tm_scene *scene, tg_target *out) {
  scene->baked = out;
  scene->lightmap = tg_target_texture(out);
  memcpy(scene->lightgen, scene->light.lightgen, sizeof scene->lightgen);
}

static void bake(ft_game *game, tm_scene *scene, const ft_level *level) {
  scene->bake_pending = false;
  cull_set(scene, NULL);
  const tmuf_visuals *v = tmuf_track_visuals(level->track);
  tm_bake_draw *all = calloc(scene->batch_count ? scene->batch_count : 1, sizeof *all);
  tm_bake_draw *draws = calloc(scene->batch_count ? scene->batch_count : 1, sizeof *draws);
  tm_bake_draw *pieces = calloc(scene->piece_count ? scene->piece_count : 1, sizeof *pieces);
  if (!all || !draws || !pieces) {
    free(all), free(draws), free(pieces);
    return;
  }
  uint32_t n = 0;
  for (uint32_t i = 0; i < scene->batch_count; i++) {
    const batch *b = &scene->batches[i];
    if ((b->family != FAMILY_PICTURE && b->family != FAMILY_BLOCK) || b->sky || !scene->meshes[b->family]) continue;
    tm_bake_draw *d = &all[i];
    d->mesh = scene->meshes[b->family];
    d->first = b->first_index, d->count = b->index_count;
    d->block = b->family == FAMILY_BLOCK;
    // the lightmapped surfaces (with the coordinates or without: the
    // stands, roof, masts) at their highest level, but the blended ones
    // (lightmap_bake_spec.md 4.1a; one advert material the game adds is
    // not). The sky's and the spots' shadows only the zone's: the
    // decoration's undersides (the stadium's outer ground at y -91) are in
    // none of the game's sky maps, its stands and masts in its sun maps
    // (B4: 4.7 % of the first sky map covered as the game's, 98.8 % of the
    // first sun map)
    d->caster = b->lightmapped && !b->glow && !(b->blended && b->alpha_cutoff <= 0.f) &&
                (b->mip == UINT32_MAX || b->mip_level == 0);
    d->receiver = b->in_lightmap;
    d->sky_caster = d->caster && b->zone;
    // SetAmbient's receivers: no CubeAmbient layer of their own (§9.3)
    d->ambient = !has_layer(b->material != UINT32_MAX && v ? &v->materials[b->material] : NULL, "CubeAmbient");
    memcpy(d->center, b->center, sizeof d->center);
    d->radius = b->radius;
    if (b->alpha_cutoff > 0.f) {
      const uint32_t pic = d->block ? b->maps[MAP_DIFFUSE] : b->texture;
      d->picture = pic != UINT32_MAX ? scene->pictures.textures[pic] : 0;
      d->cutoff = d->picture ? 0.2f : 0.f;
    }
    if (d->caster || d->receiver) draws[n++] = *d;
  }
  uint32_t pn = 0;
  for (uint32_t i = 0; i < scene->piece_count; i++) {
    const struct scene_piece *sp = &scene->pieces[i];
    if (sp->batch >= scene->batch_count || !all[sp->batch].mesh) continue;
    tm_bake_draw *d = &pieces[pn++];
    *d = all[sp->batch];
    d->first = sp->first, d->count = sp->count;
    memcpy(d->center, sp->center, sizeof d->center);
    d->radius = sp->radius;
  }
  const tmuf_weather *w = tmuf_track_weather(level->track);
  tm_bake_input in = {.track = level->track, .draws = draws, .draw_count = n, .pieces = pieces, .piece_count = pn};
  memcpy(in.box_min, scene->cast_min, sizeof in.box_min);
  memcpy(in.box_max, scene->cast_max, sizeof in.box_max);
  memcpy(in.sun_dir, scene->light.sun_dir, sizeof in.sun_dir);
  in.sunrise = w && w->mood.folder && strstr(w->mood.folder, "Sunrise");
  // at Sunset and Night the lightmap holds colours (§9): the moon's or the
  // sun's colour, its EmittAngularSize, the mood's ambient cube
  in.rgb = scene->light.lightgen[1] == 0.f;
  memcpy(in.dir_rgb, scene->light.sun_rgb, sizeof in.dir_rgb);
  in.dir_angle = w && w->mood.folder && strstr(w->mood.folder, "Sunset") ? 4.f : 40.f;
  if (in.rgb && scene->pictures.mood_dir[0]) {
    char cube[1100];
    snprintf(cube, sizeof cube, "%sAmbCubeP.dds", scene->pictures.mood_dir);
    const uint32_t k = table_add(game, &scene->pictures, cube, cube, NULL);
    in.ambient_cube = k != UINT32_MAX ? scene->pictures.textures[k] : 0;
  }
  tg_target *out = tm_bake_lightmap(game, &in);
  free(all), free(draws), free(pieces);
  if (out) baked(scene, out);
}

void tm_scene_render(ft_game *game, tm_scene *scene, const ft_render_frame *frame) {
  const ft_camera *cam = &frame->state.camera;
  if (scene->bake_pending && game->level) bake(game, scene, game->level);
  else if (!scene->baked && tm_bake_running(game)) {
    tg_target *out = tm_bake_continue(game);
    if (out) baked(scene, out);
  }
  track_uniforms u;
  memset(&u, 0, sizeof u);
  memcpy(u.view_proj, cam->view_proj, sizeof u.view_proj);
  u.lod_bias = frame->state.lod_bias;
  u.opacity = 1.f;
  u.color_scale[0] = 1.f;
  u.color_scale[1] = 1.f;
  memcpy(u.lightgen, scene->lightgen, sizeof u.lightgen);
  // the frame's alpha the ground's gloss, for the lamps' fake ground
  // reflection (tm_flares_ground_reflection)
  const bool ground_reflection = tm_flares_ground_reflects(game, game->level);
  u.spec_cube[2] = ground_reflection ? 1.f : 0.f;
  pick_mip_levels(game, scene, cam);
  cull_set(scene, cam->view_proj);
  tm_block_uniforms bu;
  memset(&bu, 0, sizeof bu);
  const double seconds = frame->world ? (double)frame->world->w.tick * TMUF_TICK_MS / 1000.0 : 0.0;
  mood_uniforms(scene, cam, seconds, &bu);
  memcpy(bu.lightgen, scene->lightgen, sizeof bu.lightgen);
  memcpy(u.light_dir, bu.light_dir, sizeof u.light_dir);
  memcpy(u.light_rgb, bu.light_rgb, sizeof u.light_rgb);
  // the vertex-lit scenery: GbxPrelight_ScaleTrans, the double-sided light
  if (scene->scenery) {
    u.scenery[1] = scene->scenery->prelight_scale;
    u.scenery[2] = scene->scenery->prelight_trans;
  }
  memcpy(u.light_dbl, scene->light.sun_rgb_double_sided, sizeof scene->light.sun_rgb_double_sided);
  // the car's headlight: DeferedScale is the intensity (one projector),
  // ProjectorRgb its colour over it (white)
  if (game->has_projector) {
    const tm_projector *p = &game->projector;
    const float far = p->frustum[5] > 0.f ? p->frustum[5] : 100.f;
    memcpy(u.proj_pos, p->pos, sizeof p->pos);
    u.proj_pos[3] = 1.f / (far * far);
    memcpy(u.proj_x, p->axes[0], sizeof p->axes[0]);
    memcpy(u.proj_y, p->axes[1], sizeof p->axes[1]);
    memcpy(u.proj_z, p->axes[2], sizeof p->axes[2]);
    u.proj_x[3] = 0.5f * (p->frustum[0] + p->frustum[3]);
    u.proj_y[3] = 0.5f * (p->frustum[1] + p->frustum[4]);
    const float hx = 0.5f * (p->frustum[3] - p->frustum[0]), hy = 0.5f * (p->frustum[4] - p->frustum[1]);
    u.proj[0] = hx > 0.f ? 1.f / hx : 0.f;
    u.proj[1] = hy > 0.f ? 1.f / hy : 0.f;
    u.proj[2] = p->intensity;
  }
  // the pictures' fog and clouds are the blocks'
  memcpy(u.fog, bu.fog, sizeof u.fog);
  memcpy(u.fog_color, bu.fog_color, sizeof u.fog_color);
  memcpy(u.clouds_u, bu.clouds_u, sizeof u.clouds_u);
  memcpy(u.clouds_v, bu.clouds_v, sizeof u.clouds_v);
  // the game culls what faces away (clockwise on screen) but for the grass
  // tufts, the sea and the double-sided materials
  const tg_state opaque = {.depth_test = true, .depth_write = true, .cull = TG_CULL_CW};
  const tg_state two_sided = {.depth_test = true, .depth_write = true, .cull = TG_CULL_NONE};
  const tg_sampler samplers[TM_TRACK_TEXTURES] = {TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,
                                   TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP,
                                   TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP,
                                   {3, 3, 1, 0},     {3, 3, 1, 0},     {3, 3, 1, 0},     TG_SAMPLER_WRAP};
  // as the game samples a block's maps
  const tg_sampler block_samplers_state[BLOCK_MAPS + 4] = {
      TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_WRAP,  TG_SAMPLER_CLAMP, TG_SAMPLER_WRAP,
      TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP, TG_SAMPLER_CLAMP,
      {3, 3, 1, 0},     {3, 3, 1, 0}};
  // The grounds first: grass tufts take the lawn's colour from the frame as
  // it is then (tg_frame_copy), where the ground drew (alpha 1; the frame is
  // cleared to 0). Then the rest, the tufts, and the glows added last.
  bool fences = false;
  for (uint32_t i = 0; i < scene->batch_count && !fences; i++)
    fences = scene->batches[i].family == FAMILY_FENCE && batch_visible(game, scene, &scene->batches[i], cam);
  tm_fence_uniforms fu;
  memset(&fu, 0, sizeof fu);
  memcpy(fu.view_proj, cam->view_proj, sizeof fu.view_proj);
  fu.eye[0] = cam->eye.x, fu.eye[1] = cam->eye.y, fu.eye[2] = cam->eye.z;
  const tg_sampler fence_samplers[3] = {{3, 3, 2, 0}, {1, 3, 3, 2}, {3, 3, 2, 0}};
  // the sea shows the frame as it is when it comes (tg_frame_copy again)
  u.water[0] = scene->water_y;
  u.water[1] = 0.f;
  u.water[2] = scene->day_time;
  u.water[3] = 1.f / 30.f; // WaterDepthMax
  u.eye[0] = cam->eye.x, u.eye[1] = cam->eye.y, u.eye[2] = cam->eye.z;
  const tg_texture water_fog = scene->water_fog != UINT32_MAX ? scene->pictures.textures[scene->water_fog] : 0;
  tm_water_uniforms wu;
  memset(&wu, 0, sizeof wu);
  memcpy(wu.view_proj, cam->view_proj, sizeof wu.view_proj);
  wu.eye[0] = cam->eye.x, wu.eye[1] = cam->eye.y, wu.eye[2] = cam->eye.z;
  const float water_params[4] = {(float)frame->tick * 0.01f, 0.05f, 0.01f, 0.1f};
  memcpy(wu.params, water_params, sizeof water_params);
  wu.params2[0] = scene->day_time;
  const uint32_t clouds_ms = frame->world ? (uint32_t)frame->world->w.tick * TMUF_TICK_MS : 0u;
  // the clouds' light occlusion (the game renders it before the scene and
  // the sea's reflection)
  tm_prof(game, "clouds_occlusion");
  if (scene->clouds) tm_clouds_occlusion(game, scene->clouds, &scene->light, cam, clouds_ms);
  tm_prof(game, "reflection");
  // the sea's reflection: the view mirrored about the water (y -> 2h - y),
  // its x and y tangents widened by 1.05; not with the eye at the water
  if (scene->water && cam->eye.y - scene->water_y >= 0.01f && game->settings.water_reflection) {
    float vp[16];
    const float *m = cam->view_proj, h = scene->water_y;
    // vp = diag(1/1.05, 1/1.05, 1, 1) view_proj S (column major)
    for (int row = 0; row < 4; row++) {
      const float k = row < 2 ? 1.f / 1.05f : 1.f;
      vp[0 * 4 + row] = k * m[0 * 4 + row];
      vp[1 * 4 + row] = -k * m[1 * 4 + row];
      vp[2 * 4 + row] = k * m[2 * 4 + row];
      vp[3 * 4 + row] = k * (m[3 * 4 + row] + 2.f * h * m[1 * 4 + row]);
    }
    cull_set(scene, vp);
    const bool reflected = reflection(game, scene, cam, &u, &bu, vp, water_fog, clouds_ms);
    cull_set(scene, cam->view_proj);
    if (reflected) {
      memcpy(wu.reflect_view_proj, vp, sizeof vp);
      wu.params2[1] = 1.f;
    }
  }
  // the reflection and refraction mirrored at their edges (D3DTADDRESS_MIRROR)
  const tg_sampler water_samplers[5] = {TG_SAMPLER_WRAP, TG_SAMPLER_CLAMP, {2, 2, 3, 2}, TG_SAMPLER_CLAMP,
                                        {2, 2, 3, 2}};
  // the sun's shadow maps, for the vertex-lit scenery and the Stadium by day
  // (its lightmap's two channels: the blocks', lawn's and dirt's light mask)
  const bool vertex_lit = scene->scenery && scene->scenery->vertex_lighting == 1;
  const bool stadium_sun = scene->lightmap && scene->lightgen[1] != 0.f;
  // (the car casts into the sun's maps by day only: at night, Speed's
  // Sunset included, the game's mask has no car (Desert's trace))
  const tmuf_weather *car_weather = tmuf_track_weather(game->level ? game->level->track : NULL);
  tm_car_model *car = game->level && frame->world && game->settings.draw_car && !game->camera_inside &&
                              !(car_weather && car_weather->is_night)
                          ? game->level->car
                          : NULL;
  // (none in the vertex-lit environments' Night (IA4's trace has no mask),
  // nor in Island's, Coast's and Bay's other nights (their Sunset: Island's
  // trace has none); Speed's Sunset, also a night for the cars' lights,
  // has one (Desert's): the collection's shadow_90, 0 for those three)
  const tmuf_weather *mood_weather = tmuf_track_weather(game->level ? game->level->track : NULL);
  const bool night_state = mood_weather && mood_weather->start.state == TMUF_DAY_NIGHT;
  const bool night_lights = mood_weather && mood_weather->is_night;
  const bool no_night_pssm = night_state || (night_lights && scene->scenery && scene->scenery->shadow_90 == 0);
  tm_prof(game, "pssm");
  const bool pssm = (vertex_lit || stadium_sun) && !(vertex_lit && no_night_pssm) && game->settings.shadows &&
                    pssm_render(game, scene, cam, &u, car);
  memcpy(bu.pssm_static, u.pssm_static, sizeof bu.pssm_static);
  memcpy(bu.pssm_cascade, u.pssm_cascade, sizeof bu.pssm_cascade);
  memcpy(bu.pssm_cell, u.pssm_cell, sizeof bu.pssm_cell);
  memcpy(bu.pssm_split, u.pssm_split, sizeof bu.pssm_split);
  memcpy(bu.pssm_view, u.pssm_view, sizeof bu.pssm_view);
  bu.pssm_view[3] = pssm && stadium_sun ? 1.f : 0.f;
  // and their ambient occlusion (the vertex-lit environments')
  const tmuf_weather *weather = tmuf_track_weather(game->level ? game->level->track : NULL);
  tm_prof(game, "ssao");
  const bool ao = pssm && vertex_lit && weather && ao_render(game, scene, cam, &weather->mood.ambient_occlusion);
  if (ao) {
    u.ao[1] = 1.f / (cam->viewport.x > 1.f ? cam->viewport.x : 1.f);
    u.ao[2] = 1.f / (cam->viewport.y > 1.f ? cam->viewport.y : 1.f);
    memcpy(u.ao_mid_gray, weather->mood.ambient_occlusion.mid_gray, sizeof(float) * 3);
  }
  // the sea's refraction, before the frame's own pass is begun (its target
  // switch would make it store and reload its samples)
  tm_prof(game, "refraction");
  const bool refracted = scene->water && refraction(game, scene, frame, &u, &bu, water_fog, wu.refract_view_proj, pssm);
  // the depth handed to the engine (its own 3D drawing's): the opaque
  // scenery's, single sampled (the frame's is never read: tmuf_gpu.c)
  tm_prof(game, "export_depth");
  if (tg_depth_exported(game->gpu)) {
    const uint32_t w = (uint32_t)cam->viewport.x, h = (uint32_t)cam->viewport.y;
    if (scene->export_depth && (tg_target_width(scene->export_depth) != w || tg_target_height(scene->export_depth) != h)) {
      tg_target_destroy(game->gpu, scene->export_depth);
      scene->export_depth = NULL;
    }
    if (!scene->export_depth && w && h) scene->export_depth = tg_target_create_depth(game->gpu, w, h);
    if (scene->export_depth) {
      static const float none[4] = {0.f, 0.f, 0.f, 0.f};
      tg_target_begin(game->gpu, scene->export_depth, none);
      depth_draws(game, scene, cam, &u, &bu);
      // and the car (what the engine draws hides behind it: the replay's
      // predicted path)
      if (game->settings.draw_car && !game->camera_inside && frame->world && game->level && game->level->car)
        tm_car_render_depth(game, game->level->car, frame);
      tg_target_end(game->gpu);
      tg_set_export_depth(game->gpu, tg_target_texture(scene->export_depth));
    }
  }
  // the depth prepass (as the game's: its main pass reuses that depth), so
  // each pixel's colour is shaded once
  tm_prof(game, "depth_prepass");
  depth_draws(game, scene, cam, &u, &bu);
  for (uint32_t pass = 0; pass < 6; pass++) {
    static const char *const pass_names[6] = {"pass0", "pass1", "pass2", "pass3", "pass4", "pass5"};
    tm_prof(game, pass_names[pass]);
    if (pass == 1 && fences) {
      for (uint32_t m = 0; m < game->marks_drawn_count; m++)
        tm_marks_flatten(game, game->marks_drawn[m].marks, &game->marks_drawn[m].frame);
      tg_frame_copy(game->gpu);
    }
    // the wheels' marks right after the opaque scenery
    if (pass == 3)
      for (uint32_t m = 0; m < game->marks_drawn_count; m++)
        tm_marks_render(game, game->marks_drawn[m].marks, &game->marks_drawn[m].frame);
    // the car's shadow on the opaque scenery it reaches (after the grass
    // tufts, before the sea and the transparent draws, as the game)
    if (pass == 3)
      for (uint32_t i = 0; i < scene->batch_count; i++) {
        const batch *b = &scene->batches[i];
        if ((b->family != FAMILY_BLOCK && b->family != FAMILY_PICTURE) || b->blended || b->glow || b->sky ||
            !batch_visible(game, scene, b, cam) || !tm_shadow_reaches(game, b->center, b->radius))
          continue;
        tm_shadow_receive(game, b->family == FAMILY_BLOCK, scene->meshes[b->family], b->first_index, b->index_count,
                          cam->view_proj, b->center, b->radius);
      }
    // the tree crowns' sprites with the opaque scenery
    if (pass == 2) draw_sprites(game, scene, cam, &u, samplers);
    // the lamps' fake ground reflection after the opaque scene and the sky
    if (pass == 5 && ground_reflection) tm_flares_ground_reflection(game, game->level, cam);
    // the clouds after the sky, before the other blended draws
    if (pass == 5 && scene->clouds) tm_clouds_render(game, scene->clouds, &scene->light, cam, clouds_ms);
    for (uint32_t i = 0; i < scene->batch_count; i++) {
      const batch *b = &scene->batches[i];
      const uint32_t want = b->blended || b->glow         ? (b->sky ? 4u : 5u)
                            : b->family == FAMILY_WATER ? 3
                            : b->family == FAMILY_FENCE ? 2
                            : b->blend.has              ? 0
                                                        : 1;
      if (want != pass || b->underground || !batch_visible(game, scene, b, cam)) continue;
      if (b->family == FAMILY_WATER) {
        const tg_texture textures[5] = {
            b->maps[0] != UINT32_MAX ? scene->pictures.textures[b->maps[0]] : 0,
            b->maps[1] != UINT32_MAX ? scene->pictures.textures[b->maps[1]] : 0,
            refracted ? tg_target_texture(scene->refraction) : 0, water_fog,
            scene->reflection && wu.params2[1] > 0.5f ? tg_target_texture(scene->reflection) : 0};
        tg_draw(game->gpu, game->water_program, &two_sided, scene->meshes[FAMILY_WATER], b->first_index,
                b->index_count, textures, water_samplers, &wu);
        continue;
      }
      if (b->family == FAMILY_FENCE) {
        const tg_texture textures[3] = {TG_TEXTURE_FRAME,
                                        b->maps[0] != UINT32_MAX ? scene->pictures.textures[b->maps[0]] : 0,
                                        b->maps[1] != UINT32_MAX ? scene->pictures.textures[b->maps[1]] : 0};
        const float params[4] = {0.2f, 2.f, 0.4f, b->lod_far < FLT_MAX ? b->lod_far : 1e6f};
        memcpy(fu.params, params, sizeof params);
        tg_draw(game->gpu, game->fence_program, &two_sided, scene->meshes[FAMILY_FENCE], b->first_index, b->index_count,
                textures, fence_samplers, &fu);
        continue;
      }
      if (b->family == FAMILY_BLOCK) {
        tg_texture textures[BLOCK_MAPS + 4] = {0};
        for (int m = 0; m < BLOCK_MAPS; m++)
          textures[m] = b->maps[m] != UINT32_MAX ? scene->pictures.textures[b->maps[m]] : 0;
        bu.params[0] = b->alpha_cutoff;
        bu.params[1] = scene->lightmap && b->in_lightmap ? 1.f : 0.f;
        textures[BLOCK_MAPS] = scene->lightmap;
        if (bu.pssm_view[3] > 0.5f) {
          textures[BLOCK_MAPS + 2] = tm_pssm_static_texture(game);
          textures[BLOCK_MAPS + 3] = tm_pssm_atlas_texture(game);
        }
        bu.params[2] = b->occlusion_only ? 1.f : 0.f;
        bu.params[3] = (float)b->variant;
        if (b->anim) {
          anim_draw(game, b, clouds_ms, game->block_program, &opaque, textures, block_samplers_state, &bu);
          continue;
        }
        const tg_state block_blended = {.blend_src = b->blend_src, .blend_dst = b->blend_dst, .depth_test = true,
                                        .cull = TG_CULL_CW};
        tg_state block_solid = opaque;
        block_solid.depth_write = !prepassed(b); // (its depth is the prepass's)
        tg_draw(game->gpu, game->block_program, b->blended ? &block_blended : &block_solid, scene->meshes[FAMILY_BLOCK],
                b->first_index, b->index_count, textures, block_samplers_state, &bu);
        continue;
      }
      // (a video's frame by the race's clock: the signs' arrows)
      tg_texture textures[TM_TRACK_TEXTURES] = {b->texture != UINT32_MAX ? tm_picture_at(&scene->pictures, b->texture, seconds) : 0,
                                 0, 0, 0, 0, 0, water_fog};
      u.blend3[0] = u.blend3[1] = u.blend3[2] = u.blend3[3] = 0.f;
      // the game fogs its opaque draws (but the self-illuminated signs and
      // the start lights with their glow)
      u.fog_color[3] = b->blended || b->sky || b->kind == TM_KIND_T_SELF_I || b->extra_mode == TM_EXTRA_GLOW ? 0.f : 1.f;
      u.lawn[0] = (b->lawn == 1 || b->lawn == 3) && !scene->lightmap ? 0.f : (float)b->lawn; // no lightmap: as before
      u.lawn[1] = b->lawn == 1 ? 8.f : 16.f;
      u.lawn[2] = b->lawn_occlusion ? 1.f : 0.f;
      if (b->lawn) {
        textures[7] = scene->lightmap;
        textures[8] = b->fresnel != UINT32_MAX ? scene->pictures.textures[b->fresnel] : 0;
        textures[9] = b->clouds != UINT32_MAX ? scene->pictures.textures[b->clouds] : 0;
      }
      u.scenery[0] = b->prelit ? 1.f : 0.f;
      u.scenery[3] = b->double_sided ? 1.f : 0.f;
      if (b->prelit) textures[9] = b->clouds != UINT32_MAX ? scene->pictures.textures[b->clouds] : 0;
      // the sun's specular lobe (SpecularCubeL), the Spec FCOut family
      u.spec_cube[0] = b->spec_lobe ? 1.f : 0.f;
      u.spec_cube[3] = b->gloss ? 1.f : 0.f;
      u.spec_cube[1] = (float)b->fc_out;
      if (b->fc_out) {
        textures[8] = b->fresnel != UINT32_MAX ? scene->pictures.textures[b->fresnel] : 0;
        textures[15] = b->env != UINT32_MAX ? scene->pictures.textures[b->env] : 0;
      }
      // the sun's shadow on the vertex-lit scenery, the Stadium's lawn and dirt
      const bool sun_shadowed = pssm && (b->prelit || (stadium_sun && (b->lawn == 1 || b->lawn == 3)));
      u.pssm_view[3] = sun_shadowed ? 1.f : 0.f;
      u.ao[0] = ao && b->prelit && !b->blended && !b->double_sided ? 1.f : 0.f;
      if (u.ao[0] > 0.f) textures[14] = tm_ssao_texture(game);
      if (sun_shadowed) {
        textures[12] = tm_pssm_static_texture(game);
        textures[13] = tm_pssm_atlas_texture(game);
      }
      if (b->start_light) {
        u.blend3[1] = 1.f;
        u.blend3[3] = start_phase(frame) * (2.f / 3.f);
      }
      u.extra[0] = (float)b->extra_mode;
      u.extra[1] = (float)b->kind;
      if (b->extra_mode != TM_EXTRA_NONE) {
        textures[10] = scene->pictures.textures[b->extra];
        extra_transform(b->extra_texture, frame, u.extra_t);
      }
      // the headlight on the opaque vertex-lit scenery (the receivers); at
      // night with the sun's shadows too (Speed's Sunset) the game's
      // projector buffer is the shadow mask's target, drawn over: its
      // receivers add sat(DeferedScale mask) (Desert's trace, draw 1436:
      // MapGbxShadow0 and MapProjector both the mask)
      u.proj[3] = 0.f;
      bool by_piece = false; // the mask's receivers picked per placement
      if (game->has_projector && b->prelit && !b->blended) {
        u.proj[3] = b->alpha_cutoff > 0.f ? 2.f : 1.f;
        textures[11] = game->projector.picture;
        if (pssm) {
          // (the projector's buffer is the mask: none for the others)
          u.proj[3] = 0.f;
          if (projector_reaches(&game->projector, b->center, b->radius)) {
            u.proj[3] = 3.f;
            by_piece = b->recv_count > 1;
          }
        }
      }
      if (b->blend.has && b->blend.third != UINT32_MAX) {
        textures[5] = scene->pictures.textures[b->blend.third];
        u.blend3[0] = 1.f;
      } else if (b->mask != UINT32_MAX) {
        textures[5] = scene->pictures.textures[b->mask];
        u.blend3[0] = 1.f;
      }
      memset(u.stripe_gen, 0, sizeof u.stripe_gen);
      u.stripe_gen[3] = b->blended ? 1.f : 0.f;
      if (b->blend.has && b->blend.stripe != UINT32_MAX) {
        textures[4] = scene->pictures.textures[b->blend.stripe];
        memcpy(u.stripe_gen, b->blend.stripe_gen, sizeof(float) * 3);
        memcpy(u.stripe, b->blend.stripe_transform, sizeof u.stripe);
      }
      u.color_scale[2] = b->occlusion != UINT32_MAX ? 1.f : 0.f;
      if (b->occlusion != UINT32_MAX) textures[3] = scene->pictures.textures[b->occlusion];
      // the colours as the game had them for A01's sunset (the weather is
      // not decoded yet)
      u.horizon0[3] = 0.f;
      if (b->horizon != UINT32_MAX) {
        textures[3] = scene->pictures.textures[b->horizon];
        memcpy(u.horizon0, scene->light.clouds_min, sizeof scene->light.clouds_min);
        memcpy(u.horizon1, scene->light.clouds_max, sizeof scene->light.clouds_max);
        u.horizon0[3] = 1.f;
        u.horizon1[3] = 0.f;
      }
      u.alpha_cutoff = b->alpha_cutoff;
      u.blend = b->blend.has ? 1.f : 0.f;
      if (b->blend.has) {
        textures[1] = scene->pictures.textures[b->blend.second];
        textures[2] = scene->pictures.textures[b->blend.mask];
        memcpy(u.gen, b->blend.gen, sizeof u.gen);
        memcpy(u.blend2, b->blend.blend2, sizeof u.blend2);
        memcpy(u.blend_mask, b->blend.blend_mask, sizeof u.blend_mask);
      }
      // (a double-sided material's not culled: Coast's tree cards, the
      // trace's cull none)
      const uint8_t cull = b->double_sided ? TG_CULL_NONE : TG_CULL_CW;
      const tg_state blended = {.blend_src = b->blend_src, .blend_dst = b->blend_dst, .depth_test = true,
                                .cull = cull};
      tg_state solid = opaque;
      solid.cull = cull;
      if (prepassed(b)) solid.depth_write = false; // (its depth is the prepass's)
      tg_sampler bs[TM_TRACK_TEXTURES];
      memcpy(bs, samplers, sizeof bs);
      if (b->address[0]) bs[0].address_u = b->address[0];
      if (b->address[1]) bs[0].address_v = b->address[1];
      if (b->gradient) bs[0] = TG_SAMPLER_CLAMP;
      if (b->anim) {
        anim_draw(game, b, clouds_ms, game->track_program, b->blended ? &blended : &solid, textures, bs, &u);
        continue;
      }
      if (by_piece) {
        // runs of placements the headlight's frustum reaches or not
        // (CHmsZone's per-piece boxes: Desert's far spire takes none)
        uint32_t k = 0;
        while (k < b->recv_count) {
          const struct scene_piece *sp = &scene->recv_pieces[b->recv_first + k];
          const bool in = projector_reaches(&game->projector, sp->center, sp->radius);
          uint32_t first = sp->first, count = 0;
          while (k < b->recv_count) {
            const struct scene_piece *q = &scene->recv_pieces[b->recv_first + k];
            if (projector_reaches(&game->projector, q->center, q->radius) != in) break;
            count += q->count;
            k++;
          }
          u.proj[3] = in ? 3.f : 0.f;
          tg_draw(game->gpu, game->track_program, b->blended ? &blended : &solid, scene->meshes[FAMILY_PICTURE],
                  first, count, textures, bs, &u);
        }
        continue;
      }
      tg_draw(game->gpu, game->track_program, b->blended ? &blended : &solid, scene->meshes[FAMILY_PICTURE],
              b->first_index, b->index_count, textures, bs, &u);
    }
  }
}

// TEMP: the reflection and refraction targets as PPM files
void tm_scene_dump_water(ft_game *game, tm_scene *scene);
void tm_scene_dump_water(ft_game *game, tm_scene *scene) {
  tg_target *ts[2] = {scene->reflection, scene->refraction};
  const char *names[2] = {"/tmp/claude-1000/ours_reflect.ppm", "/tmp/claude-1000/ours_refract.ppm"};
  for (int i = 0; i < 2; i++) {
    if (!ts[i]) continue;
    const uint32_t w = tg_target_width(ts[i]), h = tg_target_height(ts[i]);
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px || !tg_target_read(game->gpu, ts[i], px)) { free(px); continue; }
    FILE *f = fopen(names[i], "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t k = 0; k < (size_t)w * h; k++) fwrite(px + 4 * k, 3, 1, f);
    fclose(f);
    free(px);
  }
}
