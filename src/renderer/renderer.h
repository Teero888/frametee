#ifndef RENDERER_H
#define RENDERER_H

#include <animation/anim_system.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>
#include <vulkan/vulkan_core.h>

#define MAX_SHADERS 16
#define MAX_TEXTURES 256
#define MAX_MESHES 64
#define MAX_TEXTURES_PER_DRAW 8
#define MAX_UBOS_PER_DRAW 2
#define MAX_PRIMITIVE_VERTICES 100000
#define MAX_PRIMITIVE_INDICES 200000
#define MAX_RENDER_COMMANDS 32768

#if defined(_MSC_VER) && !defined(__clang__)
#include <malloc.h>
#define VLA(T, name, n) T *name = (T *)_malloca(sizeof(T) * (n))
#define VLA_FREE(name) _freea(name)
#else
#define VLA(T, name, n) T name[(n)]
#define VLA_FREE(name) (void)0
#endif

#define Z_LAYER_PICKUPS 1.f
#define Z_LAYER_PARTICLES_BACK 2.f
#define Z_LAYER_HOOK 3.f
#define Z_LAYER_PROJECTILES 4.f
#define Z_LAYER_WEAPONS 5.f
#define Z_LAYER_SKINS 6.f
#define Z_LAYER_MAP 7.f
#define Z_LAYER_PARTICLES_FRONT 8.0f
#define Z_LAYER_CURSOR 100.0f

struct buffer_t {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped_memory;
};

struct texture_t {
  uint32_t id;
  bool active;
  VkImage image;
  VkDeviceMemory memory;
  VkImageView image_view;
  VkSampler sampler;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t layer_count;
  char path[256];
  uint8_t gs_org; // ddnet grayscale shit for coloring skins
};

struct mesh_t {
  uint32_t id;
  bool active;
  buffer_t vertex_buffer;
  buffer_t index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
};

struct shader_t {
  uint32_t id;
  bool active;
  VkShaderModule vert_shader_module;
  VkShaderModule frag_shader_module;
  char vert_path[256];
  char frag_path[256];
};

struct vertex_t {
  vec2 pos;
  vec3 color;
  vec2 tex_coord;
};

struct primitive_vertex_t {
  vec2 pos;
  vec4 color;
};

struct primitive_ubo_t {
  vec2 camPos; // normalized [0..1] camera center
  float zoom;
  float aspect;
  float maxMapSize;
  float _pad[3];
  mat4 proj;
  vec2 mapSize; // width, height
  float lod_bias;
};

struct map_buffer_object_t {
  vec3 transform; // x, y, zoom
  float aspect;
  float lod_bias;
};

struct pipeline_cache_entry_t {
  bool initialized;
  VkPipeline pipeline;
  VkPipelineLayout pipeline_layout;
  VkDescriptorSetLayout descriptor_set_layout;
  VkRenderPass render_pass;
  uint32_t ubo_count;
  uint32_t texture_count;
};

struct camera_t {
  vec2 pos;
  vec2 drag_start_pos;
  float zoom;
  float zoom_wanted;
  bool is_dragging;
};

struct skin_instance_t {
  vec2 pos;
  float scale;
  int skin_index;
  int eye_state;

  // Animation data per-part (x,y offset + angle)
  vec4 body; // x, y, angle, unused
  vec4 back_foot;
  vec4 front_foot;
  vec4 attach;
  vec2 dir; // aim
  vec3 col_body;
  vec3 col_feet;
  int col_custom;
  int col_gs;
};

struct skin_renderer_t {
  shader_t *skin_shader;
  buffer_t instance_buffer;
  skin_instance_t *instance_ptr;
  uint32_t instance_count;
};

#define MAX_SKINS 128
struct skin_atlas_manager_t {
  texture_t *atlas_array; // giant 2D array texture for all skins
  bool layer_used[MAX_SKINS];
};

struct sprite_definition_t {
  uint32_t x, y, w, h;
};

struct atlas_instance_t {
  vec2 pos;         // World-space center position
  vec2 size;        // World-space size (width, height)
  float rotation;   // Rotation in radians
  int sprite_index; // Sprite index in the texture array
  vec2 uv_scale;    // Scaling factor (sprite size / layer size)
  vec2 uv_offset;   // Offset (padding / layer size)
  vec2 tiling;      // Texture repetition factor
  vec4 color;       // Instance color
};

#define MAX_ATLAS_SPRITES 512
struct atlas_renderer_t {
  shader_t *shader;
  texture_t *atlas_texture;
  VkSampler sampler;
  sprite_definition_t *sprite_definitions;
  uint32_t sprite_count;
  buffer_t instance_buffer;
  atlas_instance_t *instance_ptr;
  uint32_t instance_count;
  uint32_t max_instances;
  uint32_t layer_width;  // Max width of a sprite, used for UV scaling
  uint32_t layer_height; // Max height of a sprite, used for UV scaling
};

typedef enum {
  RENDER_CMD_MAP,
  RENDER_CMD_SKIN,
  RENDER_CMD_ATLAS,
  RENDER_CMD_RECT_FILLED,
  RENDER_CMD_CIRCLE_FILLED,
  RENDER_CMD_LINE
} render_cmd_type_t;

struct render_command_t{
  render_cmd_type_t type;
  float z; // Depth: lower is further back
  union {
    struct {
      vec2 pos;
      float scale;
      int skin_index;
      int eye_state;
      vec2 dir;
      anim_state_t anim_state;
      vec3 col_body;
      vec3 col_feet;
      bool custom_color;
    } skin;
    struct {
      atlas_renderer_t *ar;
      vec2 pos;
      vec2 size;
      float rotation;
      uint32_t sprite_index;
      bool tile_uv;
      vec4 color;
      bool screen_space;
    } atlas;
    struct {
      vec2 p1;
      vec2 p2;
      vec4 color;
      float thickness;
      uint32_t segments;
    } prim;
  } data;
};

struct render_queue_t {
  render_command_t *commands;
  uint32_t count;
};

struct renderer_state_t {
  shader_t shaders[MAX_SHADERS];
  uint32_t shader_count;

  texture_t textures[MAX_TEXTURES];
  mesh_t meshes[MAX_MESHES];
  uint32_t mesh_count;

  pipeline_cache_entry_t pipeline_cache[MAX_SHADERS];

  VkDescriptorPool frame_descriptor_pools[3];
  VkCommandPool transfer_command_pool;

  shader_t *primitive_shader;
  buffer_t dynamic_vertex_buffer;
  buffer_t dynamic_index_buffer;
  primitive_vertex_t *vertex_buffer_ptr;
  uint32_t *index_buffer_ptr;
  uint32_t primitive_vertex_count;
  uint32_t primitive_index_count;
  VkCommandBuffer current_command_buffer;

  buffer_t dynamic_ubo_buffer;
  void *ubo_buffer_ptr;
  uint32_t ubo_buffer_offset;
  VkDeviceSize min_ubo_alignment;

  camera_t camera;
  float lod_bias;
  texture_t *default_texture;
  gfx_handler_t *gfx;
  skin_atlas_manager_t skin_manager;
  skin_renderer_t skin_renderer;
  atlas_renderer_t gameskin_renderer;
  atlas_renderer_t cursor_renderer;
  atlas_renderer_t particle_renderer;
  atlas_renderer_t extras_renderer;

  render_queue_t queue; // The new Render Queue
};

void check_vk_result(VkResult err);
int renderer_init(gfx_handler_t *handler);
void renderer_cleanup(gfx_handler_t *handler);

shader_t *renderer_load_shader(gfx_handler_t *handler, const char *vert_path, const char *frag_path);
texture_t *renderer_load_texture(gfx_handler_t *handler, const char *image_path);
texture_t *renderer_load_texture_from_array(gfx_handler_t *handler, const uint8_t *pixel_array, uint32_t width, uint32_t height);
texture_t *renderer_load_compact_texture_from_array(gfx_handler_t *handler, const uint8_t **pixel_array, uint32_t width, uint32_t height);
texture_t *renderer_create_texture_from_rgba(gfx_handler_t *handler, const unsigned char *pixels, int width, int height);

void renderer_destroy_texture(gfx_handler_t *handler, texture_t *tex);
mesh_t *renderer_create_mesh(gfx_handler_t *handler, vertex_t *vertices, uint32_t vertex_count, uint32_t *indices, uint32_t index_count);

void renderer_begin_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);
void renderer_draw_mesh(gfx_handler_t *handler, VkCommandBuffer command_buffer, mesh_t *mesh, shader_t *shader, texture_t **textures, uint32_t texture_count, void **ubos, VkDeviceSize *ubo_sizes, uint32_t ubo_count);
void renderer_end_frame(gfx_handler_t *handler, VkCommandBuffer command_buffer);

void renderer_draw_map(gfx_handler_t *h);

// primitive drawing api
void renderer_draw_rect_filled(gfx_handler_t *handler, vec2 pos, vec2 size, vec4 color);
void renderer_draw_circle_filled(gfx_handler_t *handler, vec2 center, float radius, vec4 color, uint32_t segments);
void renderer_draw_line(gfx_handler_t *handler, vec2 p1, vec2 p2, vec4 color, float thickness);

texture_t *renderer_create_texture_array_from_atlas(gfx_handler_t *handler, texture_t *atlas, uint32_t tile_width, uint32_t tile_height, uint32_t num_tiles_x, uint32_t num_tiles_y);
void screen_to_world(gfx_handler_t *handler, float screen_x, float screen_y, float *world_x, float *world_y);
void world_to_screen(gfx_handler_t *h, float wx, float wy, float *sx, float *sy);

// skin rendering
void renderer_begin_skins(gfx_handler_t *h);
void renderer_push_skin_instance(gfx_handler_t *h, vec2 pos, float scale, int skin_index, int eye_state, vec2 dir, const anim_state_t *anim_state, vec3 col_body, vec3 col_feet, bool use_custom_color);
void renderer_flush_skins(gfx_handler_t *h, VkCommandBuffer cmd, texture_t *skin_array);
int renderer_load_skin_from_file(gfx_handler_t *h, const char *path, texture_t **out_preview_texture);
int renderer_load_skin_from_memory(gfx_handler_t *h, const unsigned char *buffer, size_t size, texture_t **out_preview_texture);
void renderer_unload_skin(gfx_handler_t *h, int layer);

void create_image(gfx_handler_t *handler, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t array_layers, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *image, VkDeviceMemory *image_memory);
VkImageView create_image_view(gfx_handler_t *handler, VkImage image, VkFormat format, VkImageViewType view_type, uint32_t mip_levels, uint32_t layer_count);
VkSampler create_texture_sampler(gfx_handler_t *handler, uint32_t mip_levels, VkFilter filter);

void renderer_submit_map(gfx_handler_t *h, float z);
void renderer_submit_skin(gfx_handler_t *h, float z, vec2 pos, float scale, int skin_index, int eye_state, vec2 dir, const anim_state_t *anim_state, vec3 col_body, vec3 col_feet, bool custom);
void renderer_submit_atlas(gfx_handler_t *h, atlas_renderer_t *ar, float z, vec2 pos, vec2 size, float rotation, uint32_t sprite_index, bool tile_uv, vec4 color, bool screen_space);
void renderer_submit_rect_filled(gfx_handler_t *h, float z, vec2 pos, vec2 size, vec4 color);
void renderer_submit_circle_filled(gfx_handler_t *h, float z, vec2 center, float radius, vec4 color, uint32_t segments);
void renderer_submit_line(gfx_handler_t *h, float z, vec2 p1, vec2 p2, vec4 color, float thickness);
void renderer_flush_queue(gfx_handler_t *h, VkCommandBuffer cmd);

typedef enum { CURSOR_HAMMER,
               CURSOR_GUN,
               CURSOR_SHOTGUN,
               CURSOR_GRENADE,
               CURSOR_LASER,
               CURSOR_NINJA,
               CURSOR_SPRITE_COUNT } cursor_type_t;

// game.png 32x16 grid
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
} gameskin_sprite_t;

// particles.png 8x8 grid
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
} particle_sprite_t;

// extras.png 16x16 grid
typedef enum { EXTRA_SNOWFLAKE,
               EXTRA_SPARKLE,
               EXTRA_PULLEY,
               EXTRA_HECTAGON,
               EXTRA_SPRITE_COUNT } extra_sprite_t;

#define PARTICLE_SPRITE_OFFSET 1000
#define EXTRA_SPRITE_OFFSET 2000

#endif // RENDERER_H
