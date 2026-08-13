// The engine's half of the game module ABI: everything a game is allowed to ask
// the editor to do. Each function here translates one ABI call into engine
// internals, and nothing above this file leaks into a module.
//
// The ABI deliberately passes no context pointer, because a module should never
// be able to reach an engine struct. There is exactly one engine per process, so
// this file keeps the handler in a static and resolves it on every call.

#include "engine_api.h"

#include <frametee/game_abi.h>
#include <logger/logger.h>
#include <math.h>
#include <nfd.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/fs.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/user_interface.h>

extern bool g_is_headless;

static gfx_handler_t *g_engine = NULL;
static const char *LOG_SOURCE = "GameAPI";

// Handles handed across the ABI are the engine's own resource types. They stay
// opaque to the module, so the casts are contained entirely in this file.
#define AS_TEXTURE(p) ((texture_t *)(p))
#define AS_ATLAS(p) ((atlas_renderer_t *)(p))
#define AS_PIPELINE(p) ((custom_pipeline_t *)(p))
#define AS_MESH(p) ((mesh_t *)(p))

static bool have_graphics(void) { return g_engine && !g_is_headless; }

// --- diagnostics -------------------------------------------------------------

static void api_log(ft_log_level level, const char *category, const char *message) {
  const char *cat = category ? category : "Game";
  switch (level) {
  case FT_LOG_ERROR: log_error(cat, "%s", message); break;
  case FT_LOG_WARN: log_warn(cat, "%s", message); break;
  case FT_LOG_TRACE:
  case FT_LOG_INFO:
  default: log_info(cat, "%s", message); break;
  }
}

// --- filesystem --------------------------------------------------------------

static size_t api_resolve_data_path(const char *relative, char *out, size_t out_size) {
  // Every game keeps its assets under data/games/<id>/, so a module can ship
  // textures and shaders without knowing where the editor was installed.
  const char *id = game_host_active_id(&g_engine->game_host);
  char buffer[GAME_HOST_MAX_PATH];
  const int needed = snprintf(buffer, sizeof(buffer), "data/games/%s/%s", id, relative ? relative : "");
  if (out && out_size > 0) snprintf(out, out_size, "%s", buffer);
  return needed > 0 ? (size_t)needed : 0;
}

static bool api_read_file(const char *path, void **out_data, size_t *out_size) {
  if (!path || !out_data || !out_size) return false;
  *out_data = NULL;
  *out_size = 0;

  FILE *f = fs_open(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  const long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return false;
  }
  rewind(f);

  void *data = malloc((size_t)size + 1);
  if (!data) {
    fclose(f);
    return false;
  }
  const size_t read = fread(data, 1, (size_t)size, f);
  fclose(f);
  ((char *)data)[read] = '\0';

  *out_data = data;
  *out_size = read;
  return true;
}

static void api_free_file_data(void *data) { free(data); }

// Downloads and generated files belong beside the user's config, not in the
// install directory, which may not even be writable.
static size_t api_resolve_cache_path(const char *relative, char *out, size_t out_size) {
  const char *id = game_host_active_id(&g_engine->game_host);
  char base[GAME_HOST_MAX_PATH];
  if (!fs_get_config_dir(base, sizeof(base))) snprintf(base, sizeof(base), ".");

  char directory[GAME_HOST_MAX_PATH];
  snprintf(directory, sizeof(directory), "%s%cgames%c%s", base, PATH_SEP, PATH_SEP, id);

  // Create the chain lazily; a game should not have to think about it.
  char partial[GAME_HOST_MAX_PATH];
  snprintf(partial, sizeof(partial), "%s%cgames", base, PATH_SEP);
  fs_mkdir(partial);
  fs_mkdir(directory);

  char full[GAME_HOST_MAX_PATH];
  const int needed = snprintf(full, sizeof(full), "%s%c%s", directory, PATH_SEP, relative ? relative : "");
  if (out && out_size > 0) snprintf(out, out_size, "%s", full);
  return needed > 0 ? (size_t)needed : 0;
}

// --- resources ---------------------------------------------------------------

static VkFormat abi_texture_format(ft_texture_format format) {
  switch (format) {
  case FT_TEXTURE_RG8: return VK_FORMAT_R8G8_UNORM;
  case FT_TEXTURE_RGBA8:
  default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

static ft_texture *api_texture_create(const ft_texture_desc *desc) {
  if (!have_graphics() || !desc) return NULL;
  const uint32_t layers = desc->layers ? desc->layers : 1;
  texture_t *tex = renderer_create_texture_layered(g_engine, (const unsigned char *)desc->pixels, desc->width, desc->height, layers,
                                                   abi_texture_format(desc->format), desc->mipmaps, desc->linear_filter);
  return (ft_texture *)tex;
}

static bool api_texture_update_layer(ft_texture *texture, uint32_t layer, const void *pixels, uint32_t width, uint32_t height) {
  if (!have_graphics() || !texture || !pixels) return false;
  return renderer_update_texture_layer(g_engine, AS_TEXTURE(texture), layer, pixels, width, height);
}

static void api_texture_destroy(ft_texture *texture) {
  if (!have_graphics() || !texture) return;
  renderer_destroy_texture(g_engine, AS_TEXTURE(texture));
}

static ft_texture *api_render_instances_preview(const ft_instance_preview_desc *desc) {
  if (!have_graphics() || !desc || desc->struct_size < sizeof(*desc) || !desc->pipeline || !desc->instances ||
      desc->instance_count == 0 || desc->update_count > MAX_TEXTURES_PER_DRAW || (desc->update_count > 0 && !desc->updates))
    return NULL;
  renderer_texture_layer_update_t updates[MAX_TEXTURES_PER_DRAW];
  for (uint32_t i = 0; i < desc->update_count; ++i) {
    updates[i] = (renderer_texture_layer_update_t){.texture = AS_TEXTURE(desc->updates[i].texture),
                                                   .layer = desc->updates[i].layer,
                                                   .pixels = desc->updates[i].pixels,
                                                   .width = desc->updates[i].width,
                                                   .height = desc->updates[i].height};
  }
  vec4 clear = {desc->clear_color.r, desc->clear_color.g, desc->clear_color.b, desc->clear_color.a};
  return (ft_texture *)renderer_render_instances_preview(g_engine, AS_PIPELINE(desc->pipeline), (texture_t *const *)desc->textures,
                                                         desc->texture_count, desc->instances, desc->instance_count, desc->width,
                                                         desc->height, clear, updates, desc->update_count, AS_TEXTURE(desc->destination),
                                                         desc->destination_x, desc->destination_y);
}

static ft_atlas *api_atlas_create(const ft_atlas_desc *desc) {
  if (!have_graphics() || !desc || !desc->texture) return NULL;
  // ft_sprite_rect and sprite_definition_t are the same four uint32s, but the
  // copy keeps the ABI free to diverge from the renderer's own type later.
  sprite_definition_t *sprites = malloc(sizeof(sprite_definition_t) * (desc->sprite_count ? desc->sprite_count : 1));
  if (!sprites) return NULL;
  for (uint32_t i = 0; i < desc->sprite_count; ++i) {
    sprites[i] = (sprite_definition_t){desc->sprites[i].x, desc->sprites[i].y, desc->sprites[i].w, desc->sprites[i].h};
  }
  atlas_renderer_t *atlas =
      renderer_create_atlas(g_engine, AS_TEXTURE(desc->texture), sprites, desc->sprite_count, desc->max_instances_per_frame);
  free(sprites);
  return (ft_atlas *)atlas;
}

static void api_atlas_destroy(ft_atlas *atlas) {
  if (!have_graphics() || !atlas) return;
  renderer_destroy_atlas(g_engine, AS_ATLAS(atlas));
}

static ft_pipeline *api_pipeline_create(const ft_pipeline_desc *desc) {
  if (!have_graphics() || !desc) return NULL;
  if (desc->instance_attr_count > MAX_CUSTOM_VERTEX_ATTRS) return NULL;

  uint32_t locations[MAX_CUSTOM_VERTEX_ATTRS];
  uint32_t offsets[MAX_CUSTOM_VERTEX_ATTRS];
  int formats[MAX_CUSTOM_VERTEX_ATTRS];
  for (uint32_t i = 0; i < desc->instance_attr_count; ++i) {
    locations[i] = desc->instance_attrs[i].location;
    offsets[i] = desc->instance_attrs[i].offset;
    formats[i] = (int)desc->instance_attrs[i].format;
  }

  custom_pipeline_t *pipe = renderer_create_custom_pipeline(
      g_engine, desc->vertex_spirv, desc->vertex_spirv_size, desc->fragment_spirv, desc->fragment_spirv_size, locations, offsets, formats,
      desc->instance_attr_count, desc->instance_stride, desc->max_instances_per_frame, desc->texture_count, desc->alpha_blend);
  return (ft_pipeline *)pipe;
}

static void api_pipeline_destroy(ft_pipeline *pipeline) {
  if (!have_graphics() || !pipeline) return;
  renderer_destroy_custom_pipeline(g_engine, AS_PIPELINE(pipeline));
}

static ft_mesh *api_mesh_create(const void *vertices, uint32_t vertex_count, uint32_t vertex_stride, const uint32_t *indices,
                                uint32_t index_count) {
  // ft_vertex and the renderer's vertex_t are the same layout; the check keeps
  // a module that got its struct wrong from silently drawing garbage.
  if (!have_graphics() || !vertices) return NULL;
  if (vertex_stride != sizeof(ft_vertex) || sizeof(ft_vertex) != sizeof(vertex_t)) return NULL;
  return (ft_mesh *)renderer_create_mesh(g_engine, (vertex_t *)vertices, vertex_count, (uint32_t *)indices, index_count);
}

static void api_mesh_destroy(ft_mesh *mesh) {
  // Meshes live in the renderer's fixed table and are released with it.
  (void)mesh;
}

// --- drawing -----------------------------------------------------------------

static void copy_color(const ft_color *in, vec4 out) {
  out[0] = in->r;
  out[1] = in->g;
  out[2] = in->b;
  out[3] = in->a;
}

static void api_draw_sprites(ft_atlas *atlas, float z, const ft_sprite_draw *draws, uint32_t count) {
  if (!have_graphics() || !atlas || !draws || count == 0) return;
  atlas_renderer_t *ar = AS_ATLAS(atlas);

  // The renderer batches by atlas, so the whole array goes in as one command.
  atlas_instance_t *instances = malloc(sizeof(atlas_instance_t) * count);
  if (!instances) return;
  for (uint32_t i = 0; i < count; ++i) {
    const ft_sprite_draw *d = &draws[i];
    atlas_instance_t *inst = &instances[i];
    renderer_calculate_atlas_uvs(ar, d->sprite_index, inst);
    inst->pos[0] = d->pos.x;
    inst->pos[1] = d->pos.y;
    inst->size[0] = d->size.x;
    inst->size[1] = d->size.y;
    inst->rotation = d->rotation;
    inst->sprite_index = (int)d->sprite_index;
    inst->tiling[0] = d->tiling.x != 0.f ? d->tiling.x : 1.f;
    inst->tiling[1] = d->tiling.y != 0.f ? d->tiling.y : 1.f;
    copy_color(&d->color, inst->color);
  }
  renderer_submit_atlas_batch(g_engine, ar, z, instances, count, false);
  free(instances);
}

static void api_draw_line(float z, ft_vec2 a, ft_vec2 b, ft_color color, float thickness) {
  if (!have_graphics()) return;
  vec4 col;
  copy_color(&color, col);
  renderer_submit_line(g_engine, z, (vec2){a.x, a.y}, (vec2){b.x, b.y}, col, thickness);
}

static void api_draw_rect(float z, ft_vec2 pos, ft_vec2 size, ft_color color) {
  if (!have_graphics()) return;
  vec4 col;
  copy_color(&color, col);
  renderer_submit_rect_filled(g_engine, z, (vec2){pos.x, pos.y}, (vec2){size.x, size.y}, col);
}

static void api_draw_circle(float z, ft_vec2 center, float radius, ft_color color, uint32_t segments) {
  if (!have_graphics()) return;
  vec4 col;
  copy_color(&color, col);
  renderer_submit_circle_filled(g_engine, z, (vec2){center.x, center.y}, radius, col, segments ? segments : 16);
}

static void api_draw_triangle(float z, ft_vec2 a, ft_vec2 b, ft_vec2 c, ft_color color) {
  if (!have_graphics()) return;
  vec4 col;
  copy_color(&color, col);
  renderer_submit_triangle_filled(g_engine, z, (vec2){a.x, a.y}, (vec2){b.x, b.y}, (vec2){c.x, c.y}, col);
}

static void api_draw_text(float z, ft_vec2 pos, float size, ft_color color, const char *text) {
  // World-space text goes through ImGui's viewport draw list: the renderer has
  // no glyph pipeline of its own yet, and this keeps module labels legible at
  // any zoom without one.
  if (!have_graphics() || !text || !*text) return;
  (void)z;
  float sx, sy;
  world_to_screen(g_engine, pos.x, pos.y, &sx, &sy);
  sx += g_engine->user_interface.viewport_window_pos.x;
  sy += g_engine->user_interface.viewport_window_pos.y;

  ImDrawList *list = igGetForegroundDrawList_ViewportPtr(igGetMainViewport());
  if (!list) return;
  const ImU32 packed = igGetColorU32_Vec4((ImVec4){color.r, color.g, color.b, color.a});
  ImFont *font = g_engine->user_interface.font;
  const float font_size = size > 0.f ? size : igGetFontSize();
  ImDrawList_AddText_FontPtr(list, font, font_size, (ImVec2){sx, sy}, packed, text, NULL, 0.f, NULL);
}

static void api_draw_instances(ft_pipeline *pipeline, float z, ft_texture *const *textures, uint32_t texture_count, const void *instances,
                               uint32_t count) {
  if (!have_graphics() || !pipeline) return;
  renderer_submit_instances(g_engine, AS_PIPELINE(pipeline), z, (texture_t *const *)textures, texture_count, instances, count);
}

static void api_draw_mesh(ft_pipeline *pipeline, float z, ft_mesh *mesh, ft_texture *const *textures, uint32_t texture_count,
                          const void *uniforms, size_t uniform_size) {
  // Queued like every other draw, so a game's level sorts against its own
  // entities by z instead of always landing underneath them.
  if (!have_graphics() || !mesh || !pipeline) return;
  renderer_submit_mesh(g_engine, AS_PIPELINE(pipeline), z, AS_MESH(mesh), (texture_t *const *)textures, texture_count, uniforms,
                       uniform_size);
}

// --- camera ------------------------------------------------------------------

static void api_camera_get(ft_camera *out) {
  if (!out || !g_engine) return;
  out->struct_size = sizeof(*out);
  out->position = (ft_vec2){g_engine->renderer.camera.pos[0] * g_engine->world_width, g_engine->renderer.camera.pos[1] * g_engine->world_height};
  out->zoom = g_engine->renderer.camera.zoom;
  out->mode = g_engine->renderer.camera.mode;
  out->aspect = g_engine->viewport[1] > 0.f ? g_engine->viewport[0] / g_engine->viewport[1] : 1.f;
  out->viewport = (ft_vec2){g_engine->viewport[0], g_engine->viewport[1]};

  // The visible rectangle, so a game culls against what the engine actually
  // shows instead of guessing at screen coordinates.
  float left, top, right, bottom;
  screen_to_world(g_engine, 0.f, 0.f, &left, &top);
  screen_to_world(g_engine, g_engine->viewport[0], g_engine->viewport[1], &right, &bottom);
  const float min_x = fminf(left, right);
  const float max_x = fmaxf(left, right);
  const float min_y = fminf(top, bottom);
  const float max_y = fmaxf(top, bottom);
  out->visible = (ft_rect){min_x, min_y, max_x - min_x, max_y - min_y};
}

static void api_camera_set(const ft_camera *camera) {
  if (!camera || !g_engine) return;
  if (g_engine->world_width > 0.f) g_engine->renderer.camera.pos[0] = camera->position.x / g_engine->world_width;
  if (g_engine->world_height > 0.f) g_engine->renderer.camera.pos[1] = camera->position.y / g_engine->world_height;
  if (camera->zoom > 0.f) {
    g_engine->renderer.camera.zoom = camera->zoom;
    g_engine->renderer.camera.zoom_wanted = camera->zoom;
  }
}

static ft_vec2 api_screen_to_world(ft_vec2 screen) {
  ft_vec2 out = {0.f, 0.f};
  if (!g_engine) return out;
  screen_to_world(g_engine, screen.x, screen.y, &out.x, &out.y);
  return out;
}

static ft_vec2 api_world_to_screen(ft_vec2 world) {
  ft_vec2 out = {0.f, 0.f};
  if (!g_engine) return out;
  world_to_screen(g_engine, world.x, world.y, &out.x, &out.y);
  return out;
}

// --- UI ----------------------------------------------------------------------

static void *api_imgui_context(void) {
  if (g_is_headless) return NULL;
  return igGetCurrentContext();
}

static void api_imgui_allocators(void **alloc_fn, void **free_fn, void **user_data) {
  if (g_is_headless) {
    if (alloc_fn) *alloc_fn = NULL;
    if (free_fn) *free_fn = NULL;
    if (user_data) *user_data = NULL;
    return;
  }
  igGetAllocatorFunctions((ImGuiMemAllocFunc *)alloc_fn, (ImGuiMemFreeFunc *)free_fn, user_data);
}

// --- engine feedback ---------------------------------------------------------

// A game's start screen picks a level and asks the editor to open it, which is
// the same path the user's own file dialog takes.
static bool api_request_level(const char *path) {
  if (!g_engine || !path || !*path) return false;
  on_level_load_path(g_engine, path);
  if (g_engine->level) {
    g_engine->user_interface.show_splash = false;
    return true;
  }
  return false;
}

static uint64_t api_imgui_texture_id(ft_texture *texture) {
  if (!have_graphics() || !texture) return 0;
  texture_t *tex = AS_TEXTURE(texture);
  if (!tex->image_view || !tex->sampler) return 0;
  return (uint64_t)ImGui_ImplVulkan_AddTexture(tex->sampler, tex->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void api_imgui_texture_release(uint64_t texture_id) {
  if (!have_graphics() || !texture_id) return;
  gfx_retire_imgui_texture(g_engine, texture_id);
}

static void api_mark_dirty(void) {
  if (g_engine) ui_mark_unsaved(&g_engine->user_interface);
}

static void api_invalidate_simulation(int32_t tick) {
  if (!g_engine) return;
  model_recalc_physics(&g_engine->user_interface.timeline, tick < 0 ? 0 : tick);
}

void engine_api_camera_get(ft_camera *out) { api_camera_get(out); }

void engine_api_fill_state(ft_engine_state *out) {
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  if (!g_engine) return;
  const timeline_state_t *ts = &g_engine->user_interface.timeline;
  out->current_tick = ts->current_tick;
  out->playing = ts->is_playing;
  out->recording = ts->recording;
  out->headless = g_is_headless;
  out->selected_player = ts->selected_player_track_index;
  api_camera_get(&out->camera);
}

static void api_get_state(ft_engine_state *out) {
  if (out) engine_api_fill_state(out);
}

static bool api_get_player_input(int32_t player, int32_t tick, void *out_record) {
  if (!g_engine || !out_record) return false;
  timeline_state_t *ts = &g_engine->user_interface.timeline;
  if (player < 0 || player >= ts->player_track_count) return false;

  // While recording, the tick under the playhead is still being authored: the
  // committed snippets do not have it yet, but the live input does. Handing back
  // a stale value here makes cursor-directed controls lag the mouse.
  if (ts->recording && tick >= ts->current_tick) {
    memcpy(out_record, ts->player_tracks[player].current_input.bytes, game_input_size(&g_engine->game_host));
    return true;
  }
  // The record is the game's own layout; the engine just hands back the bytes
  // it stored, trimmed to the size the game declared.
  const input_record_t record = model_get_input_at_tick(ts, player, tick);
  memcpy(out_record, record.bytes, game_input_size(&g_engine->game_host));
  return true;
}

static bool api_save_file_dialog(const char *filter_name, const char *filter_ext, const char *default_name, char *out_path, size_t out_size) {
  if (!out_path || out_size == 0) return false;
  out_path[0] = '\0';
  if (g_is_headless) {
    log_warn(LOG_SOURCE, "save_file_dialog is unavailable in headless mode");
    return false;
  }

  nfdu8filteritem_t filter = {filter_name, filter_ext};
  nfdu8char_t *path = NULL;
  const bool has_filter = filter_name && filter_ext;
  if (NFD_SaveDialogU8(&path, has_filter ? &filter : NULL, has_filter ? 1 : 0, NULL, default_name) != NFD_OKAY || !path) return false;

  snprintf(out_path, out_size, "%s", path);
  NFD_FreePathU8(path);
  return true;
}

static bool api_open_file_dialog(const char *filter_name, const char *filter_ext, char *out_path, size_t out_size) {
  if (!out_path || out_size == 0) return false;
  out_path[0] = '\0';
  if (g_is_headless) return false;

  nfdu8filteritem_t filter = {filter_name, filter_ext};
  nfdu8char_t *path = NULL;
  const bool has_filter = filter_name && filter_ext;
  if (NFD_OpenDialogU8(&path, has_filter ? &filter : NULL, has_filter ? 1 : 0, NULL) != NFD_OKAY || !path) return false;

  snprintf(out_path, out_size, "%s", path);
  NFD_FreePathU8(path);
  return true;
}

static uint32_t api_visit_directory(const char *path, ft_directory_visitor visitor, void *user) {
  if (!path || !visitor) return 0;
  fs_dir_t *directory = fs_opendir(path);
  if (!directory) return 0;
  uint32_t count = 0;
  fs_dirent_t *item;
  while ((item = fs_readdir(directory)) != NULL) {
    const ft_directory_entry entry = {.name = item->name, .is_directory = item->is_directory};
    ++count;
    if (!visitor(user, &entry)) break;
  }
  fs_closedir(directory);
  return count;
}

static bool api_get_player_setup(int32_t player, ft_player_setup *out) {
  if (!g_engine || !out) return false;
  timeline_state_t *timeline = &g_engine->user_interface.timeline;
  if (player < 0 || player >= timeline->player_track_count) return false;
  const player_track_t *track = &timeline->player_tracks[player];
  const player_info_t *info = &track->player_info;
  *out = (ft_player_setup){.struct_size = sizeof(*out),
                           .name = info->name,
                           .tag = info->tag,
                           .appearance_id = info->appearance_id,
                           .primary_color = {info->primary_color[0], info->primary_color[1], info->primary_color[2], info->primary_color[3]},
                           .secondary_color = {info->secondary_color[0], info->secondary_color[1], info->secondary_color[2],
                                               info->secondary_color[3]},
                           .use_custom_color = info->use_custom_color,
                           .linked_player = track->is_linked ? track->linked_source_player : -1};
  return true;
}

static bool api_set_player_appearance(int32_t player, const char *appearance_id) {
  if (!g_engine || !appearance_id) return false;
  timeline_state_t *timeline = &g_engine->user_interface.timeline;
  if (player < 0 || player >= timeline->player_track_count) return false;
  player_info_t *info = &timeline->player_tracks[player].player_info;
  if (strcmp(info->appearance_id, appearance_id) == 0) return true;
  snprintf(info->appearance_id, sizeof(info->appearance_id), "%s", appearance_id);
  api_mark_dirty();
  return true;
}

static uint32_t api_timeline_world_count(void) {
  return g_engine ? (uint32_t)g_engine->user_interface.timeline.group_count : 0;
}

static bool api_timeline_world_info(uint32_t world_index, ft_timeline_world_info *out) {
  if (!g_engine || !out) return false;
  timeline_state_t *timeline = &g_engine->user_interface.timeline;
  if (world_index >= (uint32_t)timeline->group_count) return false;
  *out = (ft_timeline_world_info){.struct_size = sizeof(*out),
                                  .world_index = (int32_t)world_index,
                                  .start_offset = timeline->groups[world_index]->start_offset,
                                  .player_count = (uint32_t)model_group_track_count(timeline, (int)world_index)};
  return true;
}

static bool api_timeline_world_pair(uint32_t world_index, int32_t global_tick, const ft_world **out_previous,
                                    const ft_world **out_current) {
  if (out_previous) *out_previous = NULL;
  if (out_current) *out_current = NULL;
  if (!g_engine) return false;
  timeline_state_t *timeline = &g_engine->user_interface.timeline;
  if (world_index >= (uint32_t)timeline->group_count) return false;
  model_group_world_pair(timeline, (int)world_index, global_tick, out_previous, out_current);
  return out_current && *out_current;
}

static int32_t api_timeline_player_track(uint32_t world_index, uint32_t local_player) {
  if (!g_engine || world_index >= (uint32_t)g_engine->user_interface.timeline.group_count) return -1;
  return model_group_track_index(&g_engine->user_interface.timeline, (int)world_index, (int)local_player);
}

// --- assembly ----------------------------------------------------------------

const ft_engine_api *engine_api_init(gfx_handler_t *handler) {
  static ft_engine_api api;
  g_engine = handler;

  api = (ft_engine_api){
      .struct_size = sizeof(ft_engine_api),
      .log = api_log,
      .resolve_data_path = api_resolve_data_path,
      .read_file = api_read_file,
      .free_file_data = api_free_file_data,
      .resolve_cache_path = api_resolve_cache_path,
      .texture_create = api_texture_create,
      .texture_destroy = api_texture_destroy,
      .texture_update_layer = api_texture_update_layer,
      .atlas_create = api_atlas_create,
      .atlas_destroy = api_atlas_destroy,
      .pipeline_create = api_pipeline_create,
      .pipeline_destroy = api_pipeline_destroy,
      .mesh_create = api_mesh_create,
      .mesh_destroy = api_mesh_destroy,
      .draw_sprites = api_draw_sprites,
      .draw_line = api_draw_line,
      .draw_rect = api_draw_rect,
      .draw_circle = api_draw_circle,
      .draw_triangle = api_draw_triangle,
      .draw_text = api_draw_text,
      .draw_instances = api_draw_instances,
      .draw_mesh = api_draw_mesh,
      .camera_get = api_camera_get,
      .camera_set = api_camera_set,
      .screen_to_world = api_screen_to_world,
      .world_to_screen = api_world_to_screen,
      .imgui_context = api_imgui_context,
      .imgui_allocators = api_imgui_allocators,
      .request_level = api_request_level,
      .imgui_texture_id = api_imgui_texture_id,
      .imgui_texture_release = api_imgui_texture_release,
      .mark_dirty = api_mark_dirty,
      .invalidate_simulation = api_invalidate_simulation,
      .get_state = api_get_state,
      .get_player_input = api_get_player_input,
      .save_file_dialog = api_save_file_dialog,
      .open_file_dialog = api_open_file_dialog,
      .visit_directory = api_visit_directory,
      .get_player_setup = api_get_player_setup,
      .set_player_appearance = api_set_player_appearance,
      .timeline_world_count = api_timeline_world_count,
      .timeline_world_info = api_timeline_world_info,
      .timeline_world_pair = api_timeline_world_pair,
      .timeline_player_track = api_timeline_player_track,
      .render_instances_preview = api_render_instances_preview,
  };
  return &api;
}

void engine_api_set_world_extent(gfx_handler_t *handler, float width, float height) {
  handler->world_width = width > 0.f ? width : 1.f;
  handler->world_height = height > 0.f ? height : 1.f;
}
