// DDNet's assets: the catalog of packs, the selection, and the sheets it makes.
//
// A pack is found the way DDNet's asset settings find one: in an assets/<type>
// directory, as <name>.png or as a directory <name>/ holding <type>.png
// (ddnet.png for entities), with "default" reserved for the module's own sheet.
// The directories looked in are the module's own (its cache directory's
// assets/), then DDNet's user directory, then the data directories of DDNet
// installations; a name found twice is the first one's.
//
// The sheet a type is drawn with is its selected pack, with any of its parts
// (the sprites DDNet cuts out of it) taken from other packs, each drawn as its
// pack has it, transparent parts included. Decoding and
// mixing happen on a worker thread; the result is swapped in by
// dd_assets_update, before anything of a frame is drawn.

#include "dd_assets.h"
#include "dd_threads.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb_image_resize2.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#include <direct.h>
#define DD_ASSET_SEP '\\'
#define dd_strcasecmp _stricmp
#define dd_mkdir(path) _mkdir(path)
#else
#include <strings.h>
#include <sys/stat.h>
#define DD_ASSET_SEP '/'
#define dd_strcasecmp strcasecmp
#define dd_mkdir(path) mkdir(path, 0755)
#endif

// --- the sheets' parts ---------------------------------------------------------
//
// DDNet's sprites (datasrc/content.py), on each sheet's grid. Sprites that are
// the same cells (a weapon and its pickup) are one part.

enum { G_HAMMER,
       G_GUN,
       G_SHOTGUN,
       G_GRENADE,
       G_LASER,
       G_NINJA,
       G_HOOK,
       G_HEALTH,
       G_PICKUPS,
       G_PARTICLES,
       G_FLAGS };
static const char *const game_groups[] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja",
                                          "Hook", "Health and armor", "Pickups", "Particles", "Flags"};
static const dd_asset_part_t game_parts[] = {
    {"weapon_hammer_body", "Hammer", G_HAMMER, 2, 1, 4, 3},
    {"weapon_hammer_cursor", "Hammer cursor", G_HAMMER, 0, 0, 2, 2},
    {"weapon_gun_body", "Gun", G_GUN, 2, 4, 4, 2},
    {"weapon_gun_cursor", "Gun cursor", G_GUN, 0, 4, 2, 2},
    {"weapon_gun_proj", "Gun bullet", G_GUN, 6, 4, 2, 2},
    {"weapon_gun_muzzle1", "Gun muzzle 1", G_GUN, 8, 4, 4, 2},
    {"weapon_gun_muzzle2", "Gun muzzle 2", G_GUN, 12, 4, 4, 2},
    {"weapon_gun_muzzle3", "Gun muzzle 3", G_GUN, 16, 4, 4, 2},
    {"weapon_shotgun_body", "Shotgun", G_SHOTGUN, 2, 6, 8, 2},
    {"weapon_shotgun_cursor", "Shotgun cursor", G_SHOTGUN, 0, 6, 2, 2},
    {"weapon_shotgun_proj", "Shotgun pellet", G_SHOTGUN, 10, 6, 2, 2},
    {"weapon_shotgun_muzzle1", "Shotgun muzzle 1", G_SHOTGUN, 12, 6, 4, 2},
    {"weapon_shotgun_muzzle2", "Shotgun muzzle 2", G_SHOTGUN, 16, 6, 4, 2},
    {"weapon_shotgun_muzzle3", "Shotgun muzzle 3", G_SHOTGUN, 20, 6, 4, 2},
    {"weapon_grenade_body", "Grenade launcher", G_GRENADE, 2, 8, 7, 2},
    {"weapon_grenade_cursor", "Grenade cursor", G_GRENADE, 0, 8, 2, 2},
    {"weapon_grenade_proj", "Grenade", G_GRENADE, 10, 8, 2, 2},
    {"weapon_laser_body", "Laser rifle", G_LASER, 2, 12, 7, 3},
    {"weapon_laser_cursor", "Laser cursor", G_LASER, 0, 12, 2, 2},
    {"weapon_laser_proj", "Laser projectile", G_LASER, 10, 12, 2, 2},
    {"weapon_ninja_body", "Katana", G_NINJA, 2, 10, 8, 2},
    {"weapon_ninja_cursor", "Ninja cursor", G_NINJA, 0, 10, 2, 2},
    {"weapon_ninja_muzzle1", "Ninja slash 1", G_NINJA, 25, 0, 7, 4},
    {"weapon_ninja_muzzle2", "Ninja slash 2", G_NINJA, 25, 4, 7, 4},
    {"weapon_ninja_muzzle3", "Ninja slash 3", G_NINJA, 25, 8, 7, 4},
    {"hook_chain", "Hook chain", G_HOOK, 2, 0, 1, 1},
    {"hook_head", "Hook head", G_HOOK, 3, 0, 2, 1},
    {"health_full", "Health", G_HEALTH, 21, 0, 2, 2},
    {"health_empty", "Health, empty", G_HEALTH, 23, 0, 2, 2},
    {"armor_full", "Armor", G_HEALTH, 21, 2, 2, 2},
    {"armor_empty", "Armor, empty", G_HEALTH, 23, 2, 2, 2},
    {"pickup_health", "Heart", G_PICKUPS, 10, 2, 2, 2},
    {"pickup_armor", "Shield", G_PICKUPS, 12, 2, 2, 2},
    {"pickup_armor_shotgun", "Shotgun shield", G_PICKUPS, 15, 2, 2, 2},
    {"pickup_armor_grenade", "Grenade shield", G_PICKUPS, 17, 2, 2, 2},
    {"pickup_armor_laser", "Laser shield", G_PICKUPS, 19, 2, 2, 2},
    {"pickup_armor_ninja", "Ninja shield", G_PICKUPS, 10, 10, 2, 2},
    {"part1", "Particle 1", G_PARTICLES, 6, 0, 1, 1},
    {"part2", "Particle 2", G_PARTICLES, 6, 1, 1, 1},
    {"part3", "Particle 3", G_PARTICLES, 7, 0, 1, 1},
    {"part4", "Particle 4", G_PARTICLES, 7, 1, 1, 1},
    {"part5", "Particle 5", G_PARTICLES, 8, 0, 1, 1},
    {"part6", "Particle 6", G_PARTICLES, 8, 1, 1, 1},
    {"part7", "Particle 7", G_PARTICLES, 9, 0, 2, 2},
    {"part8", "Particle 8", G_PARTICLES, 11, 0, 2, 2},
    {"part9", "Particle 9", G_PARTICLES, 13, 0, 2, 2},
    {"star1", "Star 1", G_PARTICLES, 15, 0, 2, 2},
    {"star2", "Star 2", G_PARTICLES, 17, 0, 2, 2},
    {"star3", "Star 3", G_PARTICLES, 19, 0, 2, 2},
    {"flag_blue", "Blue flag", G_FLAGS, 12, 8, 4, 8},
    {"flag_red", "Red flag", G_FLAGS, 16, 8, 4, 8},
};

static const char *const particle_groups[] = {"Particles", "Effects"};
static const dd_asset_part_t particle_parts[] = {
    {"part_slice", "Slice", 0, 0, 0, 1, 1},
    {"part_ball", "Ball", 0, 1, 0, 1, 1},
    {"part_splat01", "Splat 1", 0, 2, 0, 1, 1},
    {"part_splat02", "Splat 2", 0, 3, 0, 1, 1},
    {"part_splat03", "Splat 3", 0, 4, 0, 1, 1},
    {"part_smoke", "Smoke", 0, 0, 1, 1, 1},
    {"part_shell", "Shell", 1, 0, 2, 2, 2},
    {"part_airjump", "Air jump", 1, 2, 2, 2, 2},
    {"part_hit01", "Hit", 1, 4, 1, 2, 2},
    {"part_expl01", "Explosion", 1, 0, 4, 4, 4},
};

static const char *const emoticon_groups[] = {"Emoticons"};
static const dd_asset_part_t emoticon_parts[] = {
    {"oop", "Oop!", 0, 0, 0, 1, 1},
    {"exclamation", "Exclamation", 0, 1, 0, 1, 1},
    {"hearts", "Hearts", 0, 2, 0, 1, 1},
    {"drop", "Drop", 0, 3, 0, 1, 1},
    {"dotdot", "...", 0, 0, 1, 1, 1},
    {"music", "Music", 0, 1, 1, 1, 1},
    {"sorry", "Sorry", 0, 2, 1, 1, 1},
    {"ghost", "Ghost", 0, 3, 1, 1, 1},
    {"sushi", "Sushi", 0, 0, 2, 1, 1},
    {"splattee", "Splattee", 0, 1, 2, 1, 1},
    {"deviltee", "Deviltee", 0, 2, 2, 1, 1},
    {"zomg", "Zomg", 0, 3, 2, 1, 1},
    {"zzz", "Zzz", 0, 0, 3, 1, 1},
    {"wtf", "Wtf", 0, 1, 3, 1, 1},
    {"eyes", "Eyes", 0, 2, 3, 1, 1},
    {"question", "Question", 0, 3, 3, 1, 1},
};

enum { H_JUMPS,
       H_STATUS,
       H_FREEZE,
       H_NINJA,
       H_HITS,
       H_TELEPORT,
       H_MODES };
static const char *const hud_groups[] = {"Jumps", "Status", "Freeze bar", "Ninja bar", "Disabled hits", "Teleport guns", "Modes"};
static const dd_asset_part_t hud_parts[] = {
    {"hud_airjump", "Air jump", H_JUMPS, 0, 0, 2, 2},
    {"hud_airjump_empty", "Air jump, used", H_JUMPS, 2, 0, 2, 2},
    {"hud_solo", "Solo", H_STATUS, 4, 0, 2, 2},
    {"hud_collision_disabled", "Collision off", H_STATUS, 6, 0, 2, 2},
    {"hud_endless_jump", "Endless jump", H_STATUS, 8, 0, 2, 2},
    {"hud_endless_hook", "Endless hook", H_STATUS, 10, 0, 2, 2},
    {"hud_jetpack", "Jetpack", H_STATUS, 12, 0, 2, 2},
    {"hud_deep_frozen", "Deep frozen", H_STATUS, 10, 4, 2, 2},
    {"hud_live_frozen", "Live frozen", H_STATUS, 12, 4, 2, 2},
    {"hud_freeze_bar_full_left", "Freeze bar: full, left end", H_FREEZE, 0, 2, 1, 1},
    {"hud_freeze_bar_full", "Freeze bar: full", H_FREEZE, 1, 2, 1, 1},
    {"hud_freeze_bar_empty", "Freeze bar: empty", H_FREEZE, 2, 2, 1, 1},
    {"hud_freeze_bar_empty_right", "Freeze bar: empty, right end", H_FREEZE, 3, 2, 1, 1},
    {"hud_ninja_bar_full_left", "Ninja bar: full, left end", H_NINJA, 0, 3, 1, 1},
    {"hud_ninja_bar_full", "Ninja bar: full", H_NINJA, 1, 3, 1, 1},
    {"hud_ninja_bar_empty", "Ninja bar: empty", H_NINJA, 2, 3, 1, 1},
    {"hud_ninja_bar_empty_right", "Ninja bar: empty, right end", H_NINJA, 3, 3, 1, 1},
    {"hud_hook_hit_disabled", "Hook hits off", H_HITS, 4, 2, 2, 2},
    {"hud_hammer_hit_disabled", "Hammer hits off", H_HITS, 6, 2, 2, 2},
    {"hud_shotgun_hit_disabled", "Shotgun hits off", H_HITS, 8, 2, 2, 2},
    {"hud_grenade_hit_disabled", "Grenade hits off", H_HITS, 10, 2, 2, 2},
    {"hud_laser_hit_disabled", "Laser hits off", H_HITS, 12, 2, 2, 2},
    {"hud_gun_hit_disabled", "Gun hits off", H_HITS, 14, 2, 2, 2},
    {"hud_teleport_grenade", "Teleport grenade", H_TELEPORT, 4, 4, 2, 2},
    {"hud_teleport_gun", "Teleport gun", H_TELEPORT, 6, 4, 2, 2},
    {"hud_teleport_laser", "Teleport laser", H_TELEPORT, 8, 4, 2, 2},
    {"hud_practice_mode", "Practice", H_MODES, 4, 6, 2, 2},
    {"hud_dummy_hammer", "Dummy hammer", H_MODES, 6, 6, 2, 2},
    {"hud_dummy_copy", "Dummy copy", H_MODES, 8, 6, 2, 2},
    {"hud_lock_mode", "Locked team", H_MODES, 10, 6, 2, 2},
    {"hud_team0_mode", "Team 0", H_MODES, 12, 6, 2, 2},
};

static const char *const extra_groups[] = {"Extras"};
static const dd_asset_part_t extra_parts[] = {
    {"part_snowflake", "Snowflake", 0, 0, 0, 2, 2},
    {"part_sparkle", "Sparkle", 0, 2, 0, 2, 2},
    {"part_pulley", "Pulley", 0, 4, 0, 1, 1},
    {"part_hectagon", "Hectagon", 0, 6, 0, 2, 2},
};

// Every tile of the entities sheet is a part; made once (entity_parts_init).
static const char *const entity_groups[] = {"Tiles"};
static dd_asset_part_t entity_parts[256];
static char entity_part_ids[256][12];
static char entity_part_names[256][12];

static void entity_parts_init(void) {
  if (entity_parts[255].id) return;
  for (int i = 0; i < 256; ++i) {
    snprintf(entity_part_ids[i], sizeof(entity_part_ids[i]), "tile_%d", i);
    snprintf(entity_part_names[i], sizeof(entity_part_names[i]), "Tile %d", i);
    entity_parts[i] = (dd_asset_part_t){entity_part_ids[i], entity_part_names[i], 0, (uint8_t)(i % 16), (uint8_t)(i / 16), 1, 1};
  }
}

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

const dd_asset_type_info_t dd_asset_types[DD_ASSET_TYPE_COUNT] = {
    [DD_ASSET_ENTITIES] = {"entities", "Entities", "ddnet", "textures/ddnet.png", 16, 16, 1024, 1024, 512, 512, entity_groups,
                           COUNT_OF(entity_groups), entity_parts, 256, false},
    [DD_ASSET_GAME] = {"game", "Game", "game", "textures/game.png", 32, 16, 1024, 512, 512, 256, game_groups,
                       COUNT_OF(game_groups), game_parts, COUNT_OF(game_parts), true},
    [DD_ASSET_EMOTICONS] = {"emoticons", "Emoticons", "emoticons", "textures/emoticons.png", 4, 4, 512, 512, 512, 512,
                            emoticon_groups, COUNT_OF(emoticon_groups), emoticon_parts, COUNT_OF(emoticon_parts), true},
    [DD_ASSET_PARTICLES] = {"particles", "Particles", "particles", "textures/particles.png", 8, 8, 512, 512, 512, 512,
                            particle_groups, COUNT_OF(particle_groups), particle_parts, COUNT_OF(particle_parts), true},
    [DD_ASSET_HUD] = {"hud", "HUD", "hud", "textures/hud.png", 16, 16, 512, 512, 512, 512, hud_groups, COUNT_OF(hud_groups),
                      hud_parts, COUNT_OF(hud_parts), true},
    [DD_ASSET_EXTRAS] = {"extras", "Extras", "extras", "textures/extras.png", 16, 16, 512, 512, 512, 512, extra_groups,
                         COUNT_OF(extra_groups), extra_parts, COUNT_OF(extra_parts), true},
};

// A sheet larger than this on either side is not loaded.
#define DD_ASSET_MAX_SIDE 8192
// What is drawn has at most twice the cells of the module's own sheets. The
// engine's atlases give every sprite a layer the size of the largest one, so a
// 4096-wide game sheet would take over 200 MB; twice the default is sharp in a
// 4K video and a quarter of that. A saved sheet keeps every pixel.
#define DD_ASSET_MAX_RENDER_SCALE 2

static int pages_per_type(const dd_asset_type_info_t *info) {
  return (DD_ASSET_PAGE_SIZE / info->thumb_w) * (DD_ASSET_PAGE_SIZE / info->thumb_h);
}

// --- images ------------------------------------------------------------------

typedef struct dd_image {
  uint8_t *pixels;
  int w, h;
  bool resized;
} dd_image_t;

static void image_free(dd_image_t *image) {
  // (decoded and resized pixels alike are malloc's: stb_image's default allocator)
  free(image->pixels);
  memset(image, 0, sizeof(*image));
}

static int highest_bit(int value) {
  int bit = 1;
  while (bit <= value / 2)
    bit *= 2;
  return bit;
}

// A sheet as DDNet takes it: decoded, and resized when its sides do not divide
// into the grid (CGraphics_Threaded::CheckImageDivisibility).
static bool image_load(ft_game *game, const dd_asset_type_info_t *info, const char *path, dd_image_t *out, char *error,
                       size_t error_size) {
  memset(out, 0, sizeof(*out));
  void *file = NULL;
  size_t size = 0;
  if (!path || !*path || !game->engine->read_file(path, &file, &size)) {
    snprintf(error, error_size, "Could not read %s", path && *path ? path : "(no file)");
    return false;
  }
  int w = 0, h = 0, channels = 0;
  uint8_t *pixels = dd_decode_png(file, size, &w, &h, &channels);
  game->engine->free_file_data(file);
  if (!pixels) {
    snprintf(error, error_size, "%s is not a PNG image", path);
    return false;
  }
  if (w <= 0 || h <= 0 || w > DD_ASSET_MAX_SIDE || h > DD_ASSET_MAX_SIDE) {
    dd_free_png(pixels);
    snprintf(error, error_size, "%s is %dx%d, larger than %d on a side", path, w, h, DD_ASSET_MAX_SIDE);
    return false;
  }
  if (w % info->grid_x != 0 || h % info->grid_y != 0) {
    int nw, nh;
    if (w % info->grid_x != 0) {
      nw = highest_bit(w) > info->grid_x ? highest_bit(w) : info->grid_x;
      nh = nw / info->grid_x * info->grid_y;
    } else {
      nh = highest_bit(h) > info->grid_y ? highest_bit(h) : info->grid_y;
      nw = nh / info->grid_y * info->grid_x;
    }
    uint8_t *resized = malloc((size_t)nw * nh * 4);
    if (!resized || !stbir_resize_uint8_srgb(pixels, w, h, w * 4, resized, nw, nh, nw * 4, STBIR_RGBA)) {
      free(resized);
      dd_free_png(pixels);
      snprintf(error, error_size, "Could not resize %s", path);
      return false;
    }
    dd_free_png(pixels);
    pixels = resized;
    w = nw;
    h = nh;
    out->resized = true;
  }
  out->pixels = pixels;
  out->w = w;
  out->h = h;
  return true;
}

// The pixels of a part, in a sheet of the type.
static void part_rect(const dd_asset_type_info_t *info, const dd_asset_part_t *part, int w, int h, int rect[4]) {
  const int cell_w = w / info->grid_x, cell_h = h / info->grid_y;
  rect[0] = part->x * cell_w;
  rect[1] = part->y * cell_h;
  rect[2] = part->w * cell_w;
  rect[3] = part->h * cell_h;
}

static bool part_empty(const dd_asset_type_info_t *info, const dd_asset_part_t *part, const dd_image_t *image) {
  int r[4];
  part_rect(info, part, image->w, image->h, r);
  for (int y = 0; y < r[3]; ++y) {
    const uint8_t *row = image->pixels + ((size_t)(r[1] + y) * image->w + r[0]) * 4;
    for (int x = 0; x < r[2]; ++x)
      if (row[x * 4 + 3] != 0) return false;
  }
  return true;
}

// Copies a part from one sheet into the same part of another, resizing it
// when the two differ in size. A part is resized from its own pixels only, so
// no sprite picks up the edge of its neighbour.
static void copy_part(const dd_asset_type_info_t *info, const dd_asset_part_t *part, const uint8_t *src, int sw, int sh,
                      uint8_t *dst, int dw, int dh) {
  int s[4], d[4];
  part_rect(info, part, sw, sh, s);
  part_rect(info, part, dw, dh, d);
  if (s[2] <= 0 || s[3] <= 0 || d[2] <= 0 || d[3] <= 0) return;
  const uint8_t *from = src + ((size_t)s[1] * sw + s[0]) * 4;
  uint8_t *to = dst + ((size_t)d[1] * dw + d[0]) * 4;
  if (s[2] == d[2] && s[3] == d[3]) {
    for (int y = 0; y < s[3]; ++y)
      memcpy(to + (size_t)y * dw * 4, from + (size_t)y * sw * 4, (size_t)s[2] * 4);
    return;
  }
  stbir_resize_uint8_srgb(from, s[2], s[3], sw * 4, to, d[2], d[3], dw * 4, STBIR_RGBA);
}

// A sheet resized to `dw` x `dh`: whatever lies between the parts as a whole,
// then every part from its own pixels.
static uint8_t *resample_sheet(const dd_asset_type_info_t *info, const uint8_t *src, int sw, int sh, int dw, int dh) {
  uint8_t *out = malloc((size_t)dw * dh * 4);
  if (!out) return NULL;
  if (sw == dw && sh == dh) {
    memcpy(out, src, (size_t)dw * dh * 4);
    return out;
  }
  if (!stbir_resize_uint8_srgb(src, sw, sh, sw * 4, out, dw, dh, dw * 4, STBIR_RGBA)) {
    free(out);
    return NULL;
  }
  for (int i = 0; i < info->part_count; ++i)
    copy_part(info, &info->parts[i], src, sw, sh, out, dw, dh);
  return out;
}

// --- the worker's cache of decoded sheets -----------------------------------
//
// Mixing parts one after another decodes the same few packs again and again,
// which for a 4096-wide one takes longer than anyone wants to wait for a click.

enum { DD_CACHE_ENTRIES = 4 };
#define DD_CACHE_BYTES ((size_t)256 << 20)

// An entry stays in its slot for as long as it lives: a mix holds pointers to
// the sheets it got, and moving one would hand it another pack's pixels.
typedef struct dd_cached_image {
  bool live;
  char path[DD_ASSET_PATH_SIZE];
  int type;
  dd_image_t image;
  uint32_t used;
  bool pinned; // in use by the mix being made
} dd_cached_image_t;

typedef struct dd_image_cache {
  dd_cached_image_t entries[DD_CACHE_ENTRIES];
  uint32_t clock;
  uint32_t generation;
} dd_image_cache_t;

static void cache_evict(dd_cached_image_t *entry) {
  image_free(&entry->image);
  entry->live = false;
  entry->pinned = false;
}

static void cache_clear(dd_image_cache_t *cache) {
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i)
    if (cache->entries[i].live) cache_evict(&cache->entries[i]);
}

static bool cache_empty(const dd_image_cache_t *cache) {
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i)
    if (cache->entries[i].live) return false;
  return true;
}

static size_t cache_bytes(const dd_image_cache_t *cache) {
  size_t bytes = 0;
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i)
    if (cache->entries[i].live) bytes += (size_t)cache->entries[i].image.w * cache->entries[i].image.h * 4;
  return bytes;
}

// The least recently used entry nothing is using, or -1.
static int cache_victim(const dd_image_cache_t *cache) {
  int victim = -1;
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i) {
    const dd_cached_image_t *entry = &cache->entries[i];
    if (entry->live && !entry->pinned && (victim < 0 || entry->used < cache->entries[victim].used)) victim = i;
  }
  return victim;
}

static int cache_free_slot(const dd_image_cache_t *cache) {
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i)
    if (!cache->entries[i].live) return i;
  return -1;
}

// A sheet for a mix: from the cache when there is one (kept there, pinned
// until cache_unpin), or decoded into `own`, which the caller frees.
static const dd_image_t *sheet_get(ft_game *game, dd_image_cache_t *cache, int type, const char *path, dd_image_t *own,
                                   char *error, size_t error_size) {
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  if (cache) {
    for (int i = 0; i < DD_CACHE_ENTRIES; ++i) {
      dd_cached_image_t *entry = &cache->entries[i];
      if (entry->live && entry->type == type && strcmp(entry->path, path) == 0) {
        entry->used = ++cache->clock;
        entry->pinned = true;
        return &entry->image;
      }
    }
  }
  if (!image_load(game, info, path, own, error, error_size)) return NULL;
  if (!cache) return own;
  const size_t bytes = (size_t)own->w * own->h * 4;
  if (bytes > DD_CACHE_BYTES / 2) return own;
  // Room for it, from what nothing is using; or it stays the mix's own.
  int slot = cache_free_slot(cache);
  while (slot < 0 || cache_bytes(cache) + bytes > DD_CACHE_BYTES) {
    const int victim = cache_victim(cache);
    if (victim < 0) return own;
    cache_evict(&cache->entries[victim]);
    if (slot < 0) slot = victim;
  }
  dd_cached_image_t *entry = &cache->entries[slot];
  entry->live = true;
  snprintf(entry->path, sizeof(entry->path), "%s", path);
  entry->type = type;
  entry->image = *own;
  entry->used = ++cache->clock;
  entry->pinned = true;
  memset(own, 0, sizeof(*own));
  return &entry->image;
}

static void cache_unpin(dd_image_cache_t *cache) {
  if (!cache) return;
  for (int i = 0; i < DD_CACHE_ENTRIES; ++i)
    cache->entries[i].pinned = false;
}

// --- mixing -------------------------------------------------------------------

// What a sheet is made of: the pack, the module's own sheet, and the other
// packs parts are taken from; and per part, which (0, the pack, for most).
typedef struct dd_compose_request {
  int type;
  int path_count;
  char (*paths)[DD_ASSET_PATH_SIZE]; // [0] the pack, [1] the module's own, then the others
  char (*names)[DD_ASSET_NAME_SIZE];
  uint16_t sources[DD_ASSET_MAX_PARTS];
} dd_compose_request_t;

typedef struct dd_sheet {
  uint8_t *pixels;
  int w, h;
  uint64_t empty[DD_ASSET_MAX_PARTS / 64];
} dd_sheet_t;

static void request_free(dd_compose_request_t *request) {
  free(request->paths);
  free(request->names);
  request->paths = NULL;
  request->names = NULL;
  request->path_count = 0;
}

static bool compose(ft_game *game, const dd_compose_request_t *request, dd_image_cache_t *cache, dd_sheet_t *out,
                    char *message, size_t message_size) {
  const dd_asset_type_info_t *info = &dd_asset_types[request->type];
  memset(out, 0, sizeof(*out));
  message[0] = '\0';
  const dd_image_t **images = calloc((size_t)request->path_count, sizeof(*images));
  dd_image_t *own = calloc((size_t)request->path_count, sizeof(*own));
  bool ok = false;
  if (!images || !own) goto done;

  char error[DD_ASSET_PATH_SIZE + 64];
  const bool base_is_default = strcmp(request->paths[0], request->paths[1]) == 0;
  if (!base_is_default) images[0] = sheet_get(game, cache, request->type, request->paths[0], &own[0], error, sizeof(error));
  if (!images[0]) {
    if (!base_is_default) snprintf(message, message_size, "%s could not be loaded; the default is shown.", request->names[0]);
    images[1] = sheet_get(game, cache, request->type, request->paths[1], &own[1], error, sizeof(error));
    images[0] = images[1];
  }
  if (!images[0]) {
    snprintf(message, message_size, "The module's own %s sheet is missing.", info->id);
    goto done;
  }
  for (int i = 0; i < info->part_count; ++i) {
    const int source = request->sources[i];
    if (source == 0 || source >= request->path_count || images[source]) continue;
    images[source] = sheet_get(game, cache, request->type, request->paths[source], &own[source], error, sizeof(error));
    if (!images[source] && !message[0])
      snprintf(message, message_size, "%s could not be loaded; its parts come from the base.", request->names[source]);
  }

  // Where each part comes from, and which of them are empty there.
  const dd_image_t *from[DD_ASSET_MAX_PARTS];
  for (int i = 0; i < info->part_count; ++i) {
    const int source = request->sources[i];
    from[i] = source > 0 && source < request->path_count && images[source] ? images[source] : images[0];
    if (info->mark_empty && part_empty(info, &info->parts[i], from[i])) out->empty[i / 64] |= UINT64_C(1) << (i % 64);
  }

  // As sharp as the sharpest sheet in it.
  int cell_w = images[0]->w / info->grid_x, cell_h = images[0]->h / info->grid_y;
  for (int i = 0; i < info->part_count; ++i) {
    if (from[i]->w / info->grid_x > cell_w) cell_w = from[i]->w / info->grid_x;
    if (from[i]->h / info->grid_y > cell_h) cell_h = from[i]->h / info->grid_y;
  }
  out->w = cell_w * info->grid_x;
  out->h = cell_h * info->grid_y;
  out->pixels = resample_sheet(info, images[0]->pixels, images[0]->w, images[0]->h, out->w, out->h);
  if (!out->pixels) {
    snprintf(message, message_size, "Out of memory for a %dx%d sheet.", out->w, out->h);
    goto done;
  }
  for (int i = 0; i < info->part_count; ++i) {
    // (the pack itself is in place already)
    if (from[i] == images[0]) continue;
    copy_part(info, &info->parts[i], from[i]->pixels, from[i]->w, from[i]->h, out->pixels, out->w, out->h);
  }
  ok = true;

done:
  cache_unpin(cache);
  if (own)
    for (int i = 0; i < request->path_count; ++i)
      image_free(&own[i]);
  free(own);
  free((void *)images);
  return ok;
}

// The same sheet at a size that fits `max_w` x `max_h` cells of the grid.
static uint8_t *sheet_capped(const dd_asset_type_info_t *info, const dd_sheet_t *sheet, int max_cell_w, int max_cell_h,
                             int *out_w, int *out_h) {
  int cell_w = sheet->w / info->grid_x, cell_h = sheet->h / info->grid_y;
  if (cell_w <= max_cell_w && cell_h <= max_cell_h) {
    *out_w = sheet->w;
    *out_h = sheet->h;
    return NULL;
  }
  // (the cells keep their shape)
  const double scale = (double)max_cell_w / cell_w < (double)max_cell_h / cell_h ? (double)max_cell_w / cell_w
                                                                                 : (double)max_cell_h / cell_h;
  cell_w = (int)(cell_w * scale);
  cell_h = (int)(cell_h * scale);
  if (cell_w < 1) cell_w = 1;
  if (cell_h < 1) cell_h = 1;
  *out_w = cell_w * info->grid_x;
  *out_h = cell_h * info->grid_y;
  return resample_sheet(info, sheet->pixels, sheet->w, sheet->h, *out_w, *out_h);
}

// --- jobs ---------------------------------------------------------------------

typedef enum { JOB_PREVIEW,
               JOB_COMPOSE,
               JOB_SAVE } dd_job_kind_t;

typedef struct dd_asset_job {
  struct dd_asset_job *next;
  int kind, type;
  uint32_t stamp; // the selection's revision, or the catalog's generation
  int pack;
  char path[DD_ASSET_PATH_SIZE]; // a preview's sheet, or where a save goes
  dd_compose_request_t request;
} dd_asset_job_t;

typedef struct dd_asset_result {
  struct dd_asset_result *next;
  int kind, type;
  uint32_t stamp;
  int pack;
  bool ok, resized;
  int width, height; // of the pack, or of the sheet
  uint8_t *pixels;   // the preview, or the sheet as it is drawn
  int pixels_w, pixels_h;
  uint8_t *preview; // the sheet as the window shows it
  int preview_w, preview_h;
  uint64_t empty[DD_ASSET_MAX_PARTS / 64];
  char message[DD_ASSET_PATH_SIZE + 128];
} dd_asset_result_t;

struct dd_asset_worker {
  ft_game *game;
  pthread_mutex_t lock;
  pthread_t thread;
  bool started; // a thread was made, and is joined before the next
  bool running; // and has work, or has not seen that it has none
  bool cancel;
  dd_asset_job_t *head, *tail;
  dd_asset_result_t *results; // newest first
  uint32_t cache_generation;
  // The worker's own while it runs; the main thread's while it does not.
  dd_image_cache_t cache;
};

static void job_free(dd_asset_job_t *job) {
  if (!job) return;
  request_free(&job->request);
  free(job);
}

static void result_free(dd_asset_result_t *result) {
  if (!result) return;
  free(result->pixels);
  free(result->preview);
  free(result);
}

// The sheet the window shows: at most a page on its long side.
static uint8_t *preview_of(const dd_sheet_t *sheet, int *out_w, int *out_h) {
  int w = sheet->w, h = sheet->h;
  if (w > DD_ASSET_PAGE_SIZE || h > DD_ASSET_PAGE_SIZE) {
    const double scale = (double)DD_ASSET_PAGE_SIZE / (w > h ? w : h);
    w = (int)(w * scale + 0.5);
    h = (int)(h * scale + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
  }
  uint8_t *pixels = malloc((size_t)w * h * 4);
  if (!pixels) return NULL;
  if (w == sheet->w && h == sheet->h) memcpy(pixels, sheet->pixels, (size_t)w * h * 4);
  else if (!stbir_resize_uint8_srgb(sheet->pixels, sheet->w, sheet->h, sheet->w * 4, pixels, w, h, w * 4, STBIR_RGBA)) {
    free(pixels);
    return NULL;
  }
  *out_w = w;
  *out_h = h;
  return pixels;
}

// A composed sheet as the game draws it and the window shows it.
static void finish_compose(const dd_asset_type_info_t *info, dd_sheet_t *sheet, dd_asset_result_t *result) {
  result->width = sheet->w;
  result->height = sheet->h;
  memcpy(result->empty, sheet->empty, sizeof(result->empty));
  const int max_cell_w = DD_ASSET_MAX_RENDER_SCALE * info->default_w / info->grid_x;
  const int max_cell_h = DD_ASSET_MAX_RENDER_SCALE * info->default_h / info->grid_y;
  result->pixels = sheet_capped(info, sheet, max_cell_w, max_cell_h, &result->pixels_w, &result->pixels_h);
  result->preview = preview_of(sheet, &result->preview_w, &result->preview_h);
  if (!result->pixels) {
    // (no smaller than it is: the sheet itself is drawn)
    result->pixels = sheet->pixels;
    result->pixels_w = sheet->w;
    result->pixels_h = sheet->h;
    sheet->pixels = NULL;
  }
  result->ok = result->pixels != NULL;
}

static dd_asset_result_t *job_run(ft_game *game, const dd_asset_job_t *job, dd_image_cache_t *cache) {
  dd_asset_result_t *result = calloc(1, sizeof(*result));
  if (!result) return NULL;
  result->kind = job->kind;
  result->type = job->type;
  result->stamp = job->stamp;
  result->pack = job->pack;
  const dd_asset_type_info_t *info = &dd_asset_types[job->type];

  switch (job->kind) {
  case JOB_PREVIEW: {
    dd_image_t image;
    if (!image_load(game, info, job->path, &image, result->message, sizeof(result->message))) break;
    result->width = image.w;
    result->height = image.h;
    result->resized = image.resized;
    result->pixels = resample_sheet(info, image.pixels, image.w, image.h, info->thumb_w, info->thumb_h);
    result->pixels_w = info->thumb_w;
    result->pixels_h = info->thumb_h;
    result->ok = result->pixels != NULL;
    image_free(&image);
    break;
  }
  case JOB_COMPOSE: {
    dd_sheet_t sheet;
    if (compose(game, &job->request, cache, &sheet, result->message, sizeof(result->message)))
      finish_compose(info, &sheet, result);
    free(sheet.pixels);
    break;
  }
  case JOB_SAVE: {
    dd_sheet_t sheet;
    if (compose(game, &job->request, cache, &sheet, result->message, sizeof(result->message))) {
      result->width = sheet.w;
      result->height = sheet.h;
      result->ok = stbi_write_png(job->path, sheet.w, sheet.h, 4, sheet.pixels, sheet.w * 4) != 0;
      if (!result->ok) snprintf(result->message, sizeof(result->message), "Could not write %s", job->path);
      else snprintf(result->message, sizeof(result->message), "%s", job->path);
    }
    free(sheet.pixels);
    break;
  }
  default:
    break;
  }
  return result;
}

static void *worker_main(void *arg) {
  struct dd_asset_worker *worker = arg;
  for (;;) {
    pthread_mutex_lock(&worker->lock);
    dd_asset_job_t *job = worker->cancel ? NULL : worker->head;
    if (job) {
      worker->head = job->next;
      if (!worker->head) worker->tail = NULL;
    } else {
      worker->running = false;
    }
    const uint32_t cache_generation = worker->cache_generation;
    pthread_mutex_unlock(&worker->lock);
    if (!job) return NULL;

    if (worker->cache.generation != cache_generation) {
      cache_clear(&worker->cache);
      worker->cache.generation = cache_generation;
    }
    dd_asset_result_t *result = job_run(worker->game, job, &worker->cache);
    job_free(job);
    pthread_mutex_lock(&worker->lock);
    if (result && !worker->cancel) {
      result->next = worker->results;
      worker->results = result;
      result = NULL;
    }
    pthread_mutex_unlock(&worker->lock);
    result_free(result);
  }
}

static struct dd_asset_worker *worker_of(ft_game *game) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets) return NULL;
  if (!assets->worker) {
    struct dd_asset_worker *worker = calloc(1, sizeof(*worker));
    if (!worker) return NULL;
    worker->game = game;
    pthread_mutex_init(&worker->lock, NULL);
    assets->worker = worker;
  }
  return assets->worker;
}

// Queues a job, at the front for what someone waits on (a mix, a save) and at
// the back for previews; starts the worker when it is not running.
static void job_queue(ft_game *game, dd_asset_job_t *job, bool front) {
  struct dd_asset_worker *worker = worker_of(game);
  if (!worker) {
    job_free(job);
    return;
  }
  pthread_mutex_lock(&worker->lock);
  if (front) {
    job->next = worker->head;
    worker->head = job;
    if (!worker->tail) worker->tail = job;
  } else {
    job->next = NULL;
    if (worker->tail) worker->tail->next = job;
    else worker->head = job;
    worker->tail = job;
  }
  const bool start = !worker->running;
  if (start) worker->running = true;
  pthread_mutex_unlock(&worker->lock);
  if (!start) return;
  if (worker->started) pthread_join(worker->thread, NULL);
  worker->started = pthread_create(&worker->thread, NULL, worker_main, worker) == 0;
  if (!worker->started) {
    dd_log(game, FT_LOG_ERROR, "Could not start the asset worker.");
    pthread_mutex_lock(&worker->lock);
    worker->running = false;
    pthread_mutex_unlock(&worker->lock);
  }
}

// Drops the queued jobs of one kind for a type: a newer one replaces them.
static void jobs_drop(struct dd_asset_worker *worker, int kind, int type) {
  if (!worker) return;
  dd_asset_job_t *dropped = NULL;
  pthread_mutex_lock(&worker->lock);
  dd_asset_job_t **link = &worker->head;
  worker->tail = NULL;
  while (*link) {
    dd_asset_job_t *job = *link;
    if (job->kind == kind && job->type == type) {
      *link = job->next;
      job->next = dropped;
      dropped = job;
    } else {
      worker->tail = job;
      link = &job->next;
    }
  }
  pthread_mutex_unlock(&worker->lock);
  while (dropped) {
    dd_asset_job_t *next = dropped->next;
    job_free(dropped);
    dropped = next;
  }
}

// Stops the worker and frees what it holds.
static void worker_stop(struct dd_asset_worker *worker) {
  if (!worker) return;
  pthread_mutex_lock(&worker->lock);
  worker->cancel = true;
  dd_asset_job_t *jobs = worker->head;
  worker->head = worker->tail = NULL;
  pthread_mutex_unlock(&worker->lock);
  while (jobs) {
    dd_asset_job_t *next = jobs->next;
    job_free(jobs);
    jobs = next;
  }
  if (worker->started) pthread_join(worker->thread, NULL);
  worker->started = false;
  dd_asset_result_t *result = worker->results;
  while (result) {
    dd_asset_result_t *next = result->next;
    result_free(result);
    result = next;
  }
  worker->results = NULL;
  cache_clear(&worker->cache);
  worker->cancel = false;
}

// --- directories --------------------------------------------------------------

typedef struct dd_entry_probe {
  const char *name;
  bool directory;
  bool found;
} dd_entry_probe_t;

static bool probe_visitor(void *user, const ft_directory_entry *entry) {
  dd_entry_probe_t *probe = user;
  if (entry->is_directory == probe->directory && strcmp(entry->name, probe->name) == 0) {
    probe->found = true;
    return false;
  }
  return true;
}

// Whether `directory` holds `name` (listed rather than opened, so names in
// any script are found on every system).
static bool has_entry(ft_game *game, const char *directory, const char *name, bool is_directory) {
  if (!game->engine->visit_directory) return false;
  dd_entry_probe_t probe = {name, is_directory, false};
  game->engine->visit_directory(directory, probe_visitor, &probe);
  return probe.found;
}

static bool stop_visiting(void *user, const ft_directory_entry *entry) {
  (void)user;
  (void)entry;
  return false;
}

// (every directory lists at least itself)
static bool directory_exists(ft_game *game, const char *path) {
  return game->engine->visit_directory && game->engine->visit_directory(path, stop_visiting, NULL) > 0;
}

static void join_path(char *out, size_t out_size, const char *a, const char *b) {
  const size_t length = strlen(a);
  if (length > 0 && (a[length - 1] == '/' || a[length - 1] == '\\')) snprintf(out, out_size, "%s%s", a, b);
  else snprintf(out, out_size, "%s%c%s", a, DD_ASSET_SEP, b);
}

// DDNet's own directories: where it keeps the user's files (its downloaded
// and custom assets), and the data directories of its installations, in the
// order DDNet's storage looks in them.
enum { DD_MAX_ROOTS = 16 };
typedef struct dd_roots {
  char paths[DD_MAX_ROOTS][DD_ASSET_PATH_SIZE];
  uint8_t origins[DD_MAX_ROOTS];
  int count;
} dd_roots_t;

static void roots_add(dd_roots_t *roots, int origin, const char *format, const char *a, const char *b) {
  if (roots->count >= DD_MAX_ROOTS || !a) return;
  snprintf(roots->paths[roots->count], DD_ASSET_PATH_SIZE, format, a, b ? b : "");
  roots->origins[roots->count++] = (uint8_t)origin;
}

static void ddnet_roots(dd_roots_t *roots) {
#ifdef _WIN32
  const char *appdata = getenv("APPDATA");
  roots_add(roots, DD_ORIGIN_DDNET, "%s\\DDNet%s", appdata, NULL);
  roots_add(roots, DD_ORIGIN_DDNET, "%s\\Teeworlds%s", appdata, NULL);
  const char *program_files = getenv("ProgramFiles(x86)");
  roots_add(roots, DD_ORIGIN_DDNET_DATA, "%s\\Steam\\steamapps\\common\\DDraceNetwork\\ddnet\\data%s", program_files, NULL);
  roots_add(roots, DD_ORIGIN_DDNET_DATA, "%s\\DDNet\\data%s", getenv("ProgramFiles"), NULL);
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  roots_add(roots, DD_ORIGIN_DDNET, "%s/Library/Application Support/DDNet%s", home, NULL);
  roots_add(roots, DD_ORIGIN_DDNET, "%s/Library/Application Support/Teeworlds%s", home, NULL);
  roots_add(roots, DD_ORIGIN_DDNET_DATA, "/Applications/DDNet.app/Contents/Resources/data%s%s", "", NULL);
  roots_add(roots, DD_ORIGIN_DDNET_DATA,
            "%s/Library/Application Support/Steam/steamapps/common/DDraceNetwork/ddnet/DDNet.app/Contents/Resources/data%s",
            home, NULL);
#else
  const char *home = getenv("HOME");
  const char *data_home = getenv("XDG_DATA_HOME");
  if (data_home && *data_home) roots_add(roots, DD_ORIGIN_DDNET, "%s/ddnet%s", data_home, NULL);
  else roots_add(roots, DD_ORIGIN_DDNET, "%s/.local/share/ddnet%s", home, NULL);
  roots_add(roots, DD_ORIGIN_DDNET, "%s/.teeworlds%s", home, NULL);
  roots_add(roots, DD_ORIGIN_DDNET, "%s/.var/app/tw.ddnet.ddnet/data/ddnet%s", home, NULL);
  static const char *const data_dirs[] = {"/usr/share/ddnet/data", "/usr/share/games/ddnet/data",
                                          "/usr/local/share/ddnet/data", "/usr/local/share/games/ddnet/data",
                                          "/usr/pkg/share/ddnet/data", "/usr/pkg/share/games/ddnet/data",
                                          "/opt/ddnet/data"};
  for (int i = 0; i < COUNT_OF(data_dirs); ++i)
    roots_add(roots, DD_ORIGIN_DDNET_DATA, "%s%s", data_dirs[i], NULL);
  roots_add(roots, DD_ORIGIN_DDNET_DATA, "%s/.local/share/Steam/steamapps/common/DDraceNetwork/ddnet/data%s", home, NULL);
  roots_add(roots, DD_ORIGIN_DDNET_DATA, "%s/.steam/steam/steamapps/common/DDraceNetwork/ddnet/data%s", home, NULL);
#endif
}

// The directories a type's packs are looked for in, in order, that exist.
static void type_directories(ft_game *game, int type, dd_roots_t *out) {
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  out->count = 0;
  char relative[64];
  snprintf(relative, sizeof(relative), "assets/%s", info->id);
  char path[DD_ASSET_PATH_SIZE];
  if (game->engine->resolve_cache_path && game->engine->resolve_cache_path(relative, path, sizeof(path)) > 0 &&
      directory_exists(game, path))
    roots_add(out, DD_ORIGIN_FRAMETEE, "%s%s", path, NULL);
  dd_roots_t roots = {.count = 0};
  ddnet_roots(&roots);
  for (int i = 0; i < roots.count; ++i) {
    char assets[DD_ASSET_PATH_SIZE], directory[DD_ASSET_PATH_SIZE];
    join_path(assets, sizeof(assets), roots.paths[i], "assets");
    join_path(directory, sizeof(directory), assets, info->id);
    bool seen = false;
    for (int j = 0; j < out->count; ++j)
      seen |= strcmp(out->paths[j], directory) == 0;
    if (!seen && directory_exists(game, directory)) roots_add(out, roots.origins[i], "%s%s", directory, NULL);
  }
}

bool dd_assets_directory(ft_game *game, int type, int origin, char *out, size_t out_size) {
  if (type < 0 || type >= DD_ASSET_TYPE_COUNT || !out || out_size == 0) return false;
  out[0] = '\0';
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  if (origin == DD_ORIGIN_FRAMETEE) {
    char relative[64];
    snprintf(relative, sizeof(relative), "assets/%s", info->id);
    return game->engine->resolve_cache_path && game->engine->resolve_cache_path(relative, out, out_size) > 0;
  }
  if (origin != DD_ORIGIN_DDNET) return false;
  // The user directory DDNet uses: the first that exists.
  dd_roots_t roots = {.count = 0};
  ddnet_roots(&roots);
  for (int i = 0; i < roots.count; ++i) {
    if (roots.origins[i] != DD_ORIGIN_DDNET || !directory_exists(game, roots.paths[i])) continue;
    char assets[DD_ASSET_PATH_SIZE];
    join_path(assets, sizeof(assets), roots.paths[i], "assets");
    join_path(out, out_size, assets, info->id);
    return true;
  }
  return false;
}

void dd_assets_make_directory(const char *path) {
  char partial[DD_ASSET_PATH_SIZE];
  snprintf(partial, sizeof(partial), "%s", path);
  for (char *c = partial + 1; *c; ++c) {
    if (*c != '/' && *c != '\\') continue;
    const char separator = *c;
    *c = '\0';
#ifdef _WIN32
    // (not a drive on its own)
    if (!(c - partial == 2 && partial[1] == ':'))
#endif
      dd_mkdir(partial);
    *c = separator;
  }
  dd_mkdir(partial);
}

// --- the catalog --------------------------------------------------------------

dd_assets_t *dd_assets(ft_game *game) {
  if (!game) return NULL;
  if (!game->assets) {
    entity_parts_init();
    game->assets = calloc(1, sizeof(*game->assets));
    if (!game->assets) return NULL;
    for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type)
      snprintf(game->assets->kinds[type].base, DD_ASSET_NAME_SIZE, "default");
    game->assets->hovered_part = -1;
    game->assets->list_hovered_part = -1;
    game->assets->picker_part = -1;
    game->assets->context_pack = -1;
    game->assets->tab = DD_ASSET_GAME;
  }
  return game->assets;
}

static bool is_default(const char *name) { return !name || !*name || strcmp(name, "default") == 0; }

int dd_assets_find_pack(const dd_asset_kind_t *kind, const char *name) {
  if (!kind || !name) return -1;
  if (is_default(name)) return kind->pack_count > 0 ? 0 : -1;
  for (int i = 0; i < kind->pack_count; ++i)
    if (strcmp(kind->packs[i].name, name) == 0) return i;
  return -1;
}

static void pages_release(ft_game *game, dd_asset_kind_t *kind) {
  for (int i = 0; i < DD_ASSET_MAX_PAGES; ++i) {
    dd_asset_page_t *page = &kind->pages[i];
    if (page->texture_id && game->engine->imgui_texture_release) game->engine->imgui_texture_release(page->texture_id);
    if (page->texture) game->engine->texture_destroy(page->texture);
    free(page->pixels);
    memset(page, 0, sizeof(*page));
  }
}

static bool pack_add(dd_asset_kind_t *kind, const char *name, const char *path, int origin, int max) {
  if (kind->pack_count >= max) return false;
  for (int i = 0; i < kind->pack_count; ++i)
    if (strcmp(kind->packs[i].name, name) == 0) return false;
  if (kind->pack_count == kind->pack_capacity) {
    const int capacity = kind->pack_capacity ? kind->pack_capacity * 2 : 32;
    dd_asset_pack_t *grown = realloc(kind->packs, (size_t)capacity * sizeof(*grown));
    if (!grown) return false;
    kind->packs = grown;
    kind->pack_capacity = capacity;
  }
  dd_asset_pack_t *pack = &kind->packs[kind->pack_count++];
  memset(pack, 0, sizeof(*pack));
  snprintf(pack->name, sizeof(pack->name), "%s", name);
  snprintf(pack->path, sizeof(pack->path), "%s", path);
  pack->origin = (uint8_t)origin;
  pack->state = DD_PACK_LOADING;
  return true;
}

typedef struct dd_scan_context {
  ft_game *game;
  dd_asset_kind_t *kind;
  const dd_asset_type_info_t *info;
  const char *directory;
  int origin;
  int max;
} dd_scan_context_t;

static bool has_png_extension(const char *name) {
  const size_t length = strlen(name);
  return length > 4 && dd_strcasecmp(name + length - 4, ".png") == 0;
}

// AssetScan / EntitiesScan in DDNet's menus_settings_assets.cpp.
static bool scan_visitor(void *user, const ft_directory_entry *entry) {
  dd_scan_context_t *scan = user;
  if (entry->name[0] == '.') return true;
  char name[DD_ASSET_NAME_SIZE], path[DD_ASSET_PATH_SIZE];
  if (entry->is_directory) {
    if (strlen(entry->name) >= sizeof(name)) return true;
    snprintf(name, sizeof(name), "%s", entry->name);
    char directory[DD_ASSET_PATH_SIZE], file[96];
    join_path(directory, sizeof(directory), scan->directory, entry->name);
    snprintf(file, sizeof(file), "%s.png", scan->info->pack_file);
    if (!has_entry(scan->game, directory, file, false)) return true;
    join_path(path, sizeof(path), directory, file);
  } else {
    if (!has_png_extension(entry->name)) return true;
    const size_t stem = strlen(entry->name) - 4;
    if (stem >= sizeof(name)) return true;
    snprintf(name, sizeof(name), "%.*s", (int)stem, entry->name);
    join_path(path, sizeof(path), scan->directory, entry->name);
  }
  if (strcmp(name, "default") == 0) return true;
  pack_add(scan->kind, name, path, scan->origin, scan->max);
  return true;
}

static int pack_compare(const void *a, const void *b) {
  const dd_asset_pack_t *x = a, *y = b;
  if ((x->origin == DD_ORIGIN_BUILTIN) != (y->origin == DD_ORIGIN_BUILTIN)) return x->origin == DD_ORIGIN_BUILTIN ? -1 : 1;
  const int order = dd_strcasecmp(x->name, y->name);
  return order ? order : strcmp(x->name, y->name);
}

static void builtin_path(ft_game *game, int type, char *out, size_t out_size) {
  game->engine->resolve_data_path(dd_asset_types[type].builtin, out, out_size);
}

void dd_assets_scan(ft_game *game, int type) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  dd_asset_kind_t *kind = &assets->kinds[type];
  jobs_drop(assets->worker, JOB_PREVIEW, type);
  pages_release(game, kind);
  kind->pack_count = 0;
  ++kind->generation;

  const int per_page = pages_per_type(info);
  const int max = per_page * DD_ASSET_MAX_PAGES;
  char path[DD_ASSET_PATH_SIZE];
  builtin_path(game, type, path, sizeof(path));
  pack_add(kind, "default", path, DD_ORIGIN_BUILTIN, max);
  dd_roots_t directories;
  type_directories(game, type, &directories);
  for (int i = 0; i < directories.count; ++i) {
    dd_scan_context_t scan = {game, kind, info, directories.paths[i], directories.origins[i], max};
    game->engine->visit_directory(directories.paths[i], scan_visitor, &scan);
  }
  qsort(kind->packs, (size_t)kind->pack_count, sizeof(*kind->packs), pack_compare);

  for (int i = 0; i < kind->pack_count; ++i) {
    dd_asset_page_t *page = &kind->pages[i / per_page];
    if (!page->pixels) page->pixels = calloc((size_t)DD_ASSET_PAGE_SIZE * DD_ASSET_PAGE_SIZE, 4);
    ++page->pending;
    dd_asset_job_t *job = calloc(1, sizeof(*job));
    if (!job) continue;
    job->kind = JOB_PREVIEW;
    job->type = type;
    job->stamp = kind->generation;
    job->pack = i;
    snprintf(job->path, sizeof(job->path), "%s", kind->packs[i].path);
    job_queue(game, job, false);
  }
  kind->scanned = true;
}

int dd_assets_previews_pending(const dd_asset_kind_t *kind) {
  int pending = 0;
  for (int i = 0; i < kind->pack_count; ++i)
    pending += kind->packs[i].state == DD_PACK_LOADING;
  return pending;
}

void dd_assets_upload_pages(ft_game *game, int type) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  dd_asset_kind_t *kind = &assets->kinds[type];
  for (int i = 0; i < DD_ASSET_MAX_PAGES; ++i) {
    dd_asset_page_t *page = &kind->pages[i];
    if (!page->dirty || !page->pixels) continue;
    if (!page->texture) {
      const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                    .pixels = page->pixels,
                                    .width = DD_ASSET_PAGE_SIZE,
                                    .height = DD_ASSET_PAGE_SIZE,
                                    .layers = 1,
                                    .format = FT_TEXTURE_RGBA8,
                                    .mipmaps = false,
                                    .linear_filter = true};
      page->texture = game->engine->texture_create(&desc);
      if (page->texture && game->engine->imgui_texture_id) page->texture_id = game->engine->imgui_texture_id(page->texture);
    } else {
      game->engine->texture_update_layer(page->texture, 0, page->pixels, DD_ASSET_PAGE_SIZE, DD_ASSET_PAGE_SIZE);
    }
    page->dirty = false;
    if (page->pending == 0 && page->texture) {
      free(page->pixels);
      page->pixels = NULL;
    }
  }
}

bool dd_assets_pack_uv(ft_game *game, int type, int pack, const dd_asset_part_t *part, uint64_t *texture_id, float uv[4]) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return false;
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  const dd_asset_kind_t *kind = &assets->kinds[type];
  if (pack < 0 || pack >= kind->pack_count || kind->packs[pack].state != DD_PACK_READY) return false;
  const int per_page = pages_per_type(info);
  const dd_asset_page_t *page = &kind->pages[pack / per_page];
  if (!page->texture_id) return false;
  const int columns = DD_ASSET_PAGE_SIZE / info->thumb_w;
  const int slot = pack % per_page;
  float x = (float)((slot % columns) * info->thumb_w), y = (float)((slot / columns) * info->thumb_h);
  float w = (float)info->thumb_w, h = (float)info->thumb_h;
  if (part) {
    const float cell_w = w / info->grid_x, cell_h = h / info->grid_y;
    x += part->x * cell_w;
    y += part->y * cell_h;
    w = part->w * cell_w;
    h = part->h * cell_h;
  }
  *texture_id = page->texture_id;
  uv[0] = x / DD_ASSET_PAGE_SIZE;
  uv[1] = y / DD_ASSET_PAGE_SIZE;
  uv[2] = (x + w) / DD_ASSET_PAGE_SIZE;
  uv[3] = (y + h) / DD_ASSET_PAGE_SIZE;
  return true;
}

bool dd_assets_sheet_texture(ft_game *game, int type, uint64_t *texture_id) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return false;
  dd_asset_kind_t *kind = &assets->kinds[type];
  if (kind->preview_dirty && kind->preview) {
    if (kind->preview_id && game->engine->imgui_texture_release) game->engine->imgui_texture_release(kind->preview_id);
    if (kind->preview_texture) game->engine->texture_destroy(kind->preview_texture);
    kind->preview_id = 0;
    const ft_texture_desc desc = {.struct_size = sizeof(desc),
                                  .pixels = kind->preview,
                                  .width = (uint32_t)kind->preview_w,
                                  .height = (uint32_t)kind->preview_h,
                                  .layers = 1,
                                  .format = FT_TEXTURE_RGBA8,
                                  .mipmaps = true,
                                  .linear_filter = true};
    kind->preview_texture = game->engine->texture_create(&desc);
    if (kind->preview_texture && game->engine->imgui_texture_id)
      kind->preview_id = game->engine->imgui_texture_id(kind->preview_texture);
    kind->preview_dirty = false;
  }
  *texture_id = kind->preview_id;
  return kind->preview_id != 0;
}

// --- the selection --------------------------------------------------------------

// (what the window says about the sheet stays until the new one replaces it,
// so nothing below it moves while the sheet is made)
static void selection_changed(ft_game *game, dd_asset_kind_t *kind) {
  ++kind->revision;
  if (game->engine->settings_changed) game->engine->settings_changed();
}

const char *dd_assets_part_pack(const dd_asset_kind_t *kind, int part, bool *overridden) {
  for (int i = 0; i < kind->override_count; ++i)
    if (kind->overrides[i].part == part) {
      if (overridden) *overridden = true;
      return kind->overrides[i].pack;
    }
  if (overridden) *overridden = false;
  return kind->base;
}

int dd_assets_pack_part_count(const dd_asset_kind_t *kind, const char *pack) {
  int count = 0;
  for (int i = 0; i < kind->override_count; ++i)
    count += strcmp(kind->overrides[i].pack, pack) == 0;
  return count;
}

static void override_remove(dd_asset_kind_t *kind, int part) {
  for (int i = 0; i < kind->override_count; ++i)
    if (kind->overrides[i].part == part) {
      kind->overrides[i] = kind->overrides[--kind->override_count];
      return;
    }
}

static bool override_set(dd_asset_kind_t *kind, int part, const char *pack) {
  for (int i = 0; i < kind->override_count; ++i)
    if (kind->overrides[i].part == part) {
      snprintf(kind->overrides[i].pack, DD_ASSET_NAME_SIZE, "%s", pack);
      return true;
    }
  if (kind->override_count == kind->override_capacity) {
    const int capacity = kind->override_capacity ? kind->override_capacity * 2 : 16;
    dd_asset_override_t *grown = realloc(kind->overrides, (size_t)capacity * sizeof(*grown));
    if (!grown) return false;
    kind->overrides = grown;
    kind->override_capacity = capacity;
  }
  dd_asset_override_t *entry = &kind->overrides[kind->override_count++];
  entry->part = (uint16_t)part;
  snprintf(entry->pack, DD_ASSET_NAME_SIZE, "%s", pack);
  return true;
}

void dd_assets_select(ft_game *game, int type, const char *pack) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const char *name = is_default(pack) ? "default" : pack;
  if (strcmp(kind->base, name) == 0) return;
  snprintf(kind->base, DD_ASSET_NAME_SIZE, "%s", name);
  // (a part taken from the pack that is now the base is the base's again)
  for (int i = kind->override_count - 1; i >= 0; --i)
    if (strcmp(kind->overrides[i].pack, name) == 0) kind->overrides[i] = kind->overrides[--kind->override_count];
  selection_changed(game, kind);
}

void dd_assets_set_part(ft_game *game, int type, int part, const char *pack) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT || part < 0 || part >= dd_asset_types[type].part_count) return;
  dd_asset_kind_t *kind = &assets->kinds[type];
  bool overridden;
  const char *current = dd_assets_part_pack(kind, part, &overridden);
  const bool to_base = !pack || strcmp(is_default(pack) ? "default" : pack, kind->base) == 0;
  if (to_base) {
    if (!overridden) return;
    override_remove(kind, part);
  } else {
    const char *name = is_default(pack) ? "default" : pack;
    if (overridden && strcmp(current, name) == 0) return;
    if (!override_set(kind, part, name)) return;
  }
  selection_changed(game, kind);
}

void dd_assets_set_group(ft_game *game, int type, int group, const char *pack) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  dd_asset_kind_t *kind = &assets->kinds[type];
  const char *name = is_default(pack) ? "default" : pack;
  const bool to_base = !pack || strcmp(name, kind->base) == 0;
  bool changed = false;
  for (int part = 0; part < info->part_count; ++part) {
    if (info->parts[part].group != group) continue;
    bool overridden;
    const char *current = dd_assets_part_pack(kind, part, &overridden);
    if (to_base) {
      if (!overridden) continue;
      override_remove(kind, part);
    } else {
      if (overridden && strcmp(current, name) == 0) continue;
      if (!override_set(kind, part, name)) continue;
    }
    changed = true;
  }
  if (changed) selection_changed(game, kind);
}

bool dd_assets_part_empty(const dd_asset_kind_t *kind, int part) {
  return part >= 0 && part < DD_ASSET_MAX_PARTS && ((kind->empty[part / 64] >> (part % 64)) & 1u);
}

// (as the sheet shown says: call it when the sheet matches the selection)
int dd_assets_empty_count(const dd_asset_kind_t *kind, int type) {
  int count = 0;
  for (int part = 0; part < dd_asset_types[type].part_count; ++part)
    if (dd_assets_part_empty(kind, part) && strcmp(dd_assets_part_pack(kind, part, NULL), "default") != 0) ++count;
  return count;
}

bool dd_assets_fill_empty(ft_game *game, int type) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return false;
  dd_asset_kind_t *kind = &assets->kinds[type];
  if (kind->applied != kind->revision) return false;
  bool changed = false;
  for (int part = 0; part < dd_asset_types[type].part_count; ++part) {
    if (!dd_assets_part_empty(kind, part) || strcmp(dd_assets_part_pack(kind, part, NULL), "default") == 0) continue;
    if (strcmp(kind->base, "default") == 0) override_remove(kind, part);
    else if (!override_set(kind, part, "default")) continue;
    changed = true;
  }
  if (changed) selection_changed(game, kind);
  return changed;
}

void dd_assets_clear_mix(ft_game *game, int type) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  dd_asset_kind_t *kind = &assets->kinds[type];
  if (kind->override_count == 0) return;
  kind->override_count = 0;
  selection_changed(game, kind);
}

// A mix as the config keeps it: part=pack;part=pack, with %, ; and = escaped.
static void append(char **out, size_t *length, size_t *capacity, const char *text, bool escape) {
  for (const char *c = text; *c; ++c) {
    if (*length + 4 >= *capacity) {
      const size_t grown_capacity = *capacity ? *capacity * 2 : 256;
      char *grown = realloc(*out, grown_capacity);
      if (!grown) return;
      *out = grown;
      *capacity = grown_capacity;
    }
    if (escape && (*c == '%' || *c == ';' || *c == '='))
      *length += (size_t)snprintf(*out + *length, 4, "%%%02X", (unsigned char)*c);
    else (*out)[(*length)++] = *c;
    (*out)[*length] = '\0';
  }
}

static void unescape(char *text) {
  char *to = text;
  for (const char *from = text; *from; ++from) {
    if (from[0] == '%' && isxdigit((unsigned char)from[1]) && isxdigit((unsigned char)from[2])) {
      char hex[3] = {from[1], from[2], '\0'};
      *to++ = (char)strtol(hex, NULL, 16);
      from += 2;
    } else {
      *to++ = *from;
    }
  }
  *to = '\0';
}

const char *dd_assets_setting(ft_game *game, int type, bool mix) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return "";
  dd_asset_kind_t *kind = &assets->kinds[type];
  if (!mix) return kind->base;
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  size_t length = 0, capacity = 0;
  char *text = NULL;
  for (int i = 0; i < kind->override_count; ++i) {
    if (kind->overrides[i].part >= info->part_count) continue;
    if (length > 0) append(&text, &length, &capacity, ";", false);
    append(&text, &length, &capacity, info->parts[kind->overrides[i].part].id, true);
    append(&text, &length, &capacity, "=", false);
    append(&text, &length, &capacity, kind->overrides[i].pack, true);
  }
  free(kind->mix_setting);
  kind->mix_setting = text;
  return text ? text : "";
}

void dd_assets_set_setting(ft_game *game, int type, bool mix, const char *value) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT) return;
  dd_asset_kind_t *kind = &assets->kinds[type];
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  if (!mix) {
    const char *name = is_default(value) ? "default" : value;
    if (strcmp(kind->base, name) == 0) return;
    snprintf(kind->base, DD_ASSET_NAME_SIZE, "%s", name);
    ++kind->revision;
    return;
  }
  kind->override_count = 0;
  const size_t length = value ? strlen(value) : 0;
  char *text = length ? malloc(length + 1) : NULL;
  if (text) memcpy(text, value, length + 1);
  for (char *entry = text, *next; entry && *entry; entry = next) {
    next = strchr(entry, ';');
    if (next) *next++ = '\0';
    char *equals = strchr(entry, '=');
    if (!equals) continue;
    *equals = '\0';
    char *pack = equals + 1;
    unescape(entry);
    unescape(pack);
    if (!*pack || strlen(pack) >= DD_ASSET_NAME_SIZE) continue;
    for (int part = 0; part < info->part_count; ++part)
      if (strcmp(info->parts[part].id, entry) == 0) {
        if (strcmp(pack, kind->base) != 0) override_set(kind, part, pack);
        break;
      }
  }
  free(text);
  ++kind->revision;
}

bool dd_assets_valid_name(const char *name) {
  if (!name || !*name || name[0] == '.' || strlen(name) >= DD_ASSET_NAME_SIZE || strcmp(name, "default") == 0) return false;
  for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
    if (*c < 32 || strchr("/\\:*?\"<>|", *c)) return false;
  const char last = name[strlen(name) - 1];
  return last != ' ' && last != '.';
}

// --- making the sheets ------------------------------------------------------------

// The file a pack name stands for: the catalog's, or, before the catalog is
// read, the first directory's that has it.
static bool resolve_pack(ft_game *game, int type, const char *name, char *out, size_t out_size) {
  if (is_default(name)) {
    builtin_path(game, type, out, out_size);
    return true;
  }
  const dd_asset_kind_t *kind = &dd_assets(game)->kinds[type];
  if (kind->scanned) {
    const int index = dd_assets_find_pack(kind, name);
    if (index < 0) return false;
    snprintf(out, out_size, "%s", kind->packs[index].path);
    return true;
  }
  const dd_asset_type_info_t *info = &dd_asset_types[type];
  dd_roots_t directories;
  type_directories(game, type, &directories);
  char file[DD_ASSET_NAME_SIZE + 8], pack_file[96];
  snprintf(file, sizeof(file), "%s.png", name);
  snprintf(pack_file, sizeof(pack_file), "%s.png", info->pack_file);
  for (int i = 0; i < directories.count; ++i) {
    const char *directory = directories.paths[i];
    char sub[DD_ASSET_PATH_SIZE];
    join_path(sub, sizeof(sub), directory, name);
    // (DDNet looks for an entities directory first, and for a file first otherwise)
    const bool as_file = has_entry(game, directory, file, false);
    const bool as_directory = has_entry(game, directory, name, true) && has_entry(game, sub, pack_file, false);
    if (as_directory && (type == DD_ASSET_ENTITIES || !as_file)) {
      join_path(out, out_size, sub, pack_file);
      return true;
    }
    if (as_file) {
      join_path(out, out_size, directory, file);
      return true;
    }
  }
  return false;
}

static bool request_build(ft_game *game, int type, dd_compose_request_t *request) {
  const dd_asset_kind_t *kind = &dd_assets(game)->kinds[type];
  memset(request, 0, sizeof(*request));
  request->type = type;
  const int capacity = 2 + kind->override_count;
  request->paths = calloc((size_t)capacity, sizeof(*request->paths));
  request->names = calloc((size_t)capacity, sizeof(*request->names));
  if (!request->paths || !request->names) {
    request_free(request);
    return false;
  }
  builtin_path(game, type, request->paths[1], DD_ASSET_PATH_SIZE);
  snprintf(request->names[1], DD_ASSET_NAME_SIZE, "default");
  if (!resolve_pack(game, type, kind->base, request->paths[0], DD_ASSET_PATH_SIZE))
    snprintf(request->paths[0], DD_ASSET_PATH_SIZE, "%s", request->paths[1]);
  snprintf(request->names[0], DD_ASSET_NAME_SIZE, "%s", kind->base);
  request->path_count = 2;
  for (int i = 0; i < kind->override_count; ++i) {
    const dd_asset_override_t *entry = &kind->overrides[i];
    if (entry->part >= dd_asset_types[type].part_count) continue;
    // A pack many parts come from is looked up once.
    int source = is_default(entry->pack) ? 1 : -1;
    for (int j = 2; source < 0 && j < request->path_count; ++j)
      if (strcmp(request->names[j], entry->pack) == 0) source = j;
    if (source < 0) {
      char path[DD_ASSET_PATH_SIZE];
      if (!resolve_pack(game, type, entry->pack, path, sizeof(path))) continue;
      source = request->path_count++;
      snprintf(request->paths[source], DD_ASSET_PATH_SIZE, "%s", path);
      snprintf(request->names[source], DD_ASSET_NAME_SIZE, "%s", entry->pack);
    }
    request->sources[entry->part] = (uint16_t)source;
  }
  return true;
}

// A finished sheet into the game, and into the window's preview of it.
static void sheet_apply(ft_game *game, int type, dd_asset_result_t *result) {
  dd_asset_kind_t *kind = &dd_assets(game)->kinds[type];
  kind->applied = result->stamp;
  if (result->message[0]) snprintf(kind->error, sizeof(kind->error), "%s", result->message);
  else kind->error[0] = '\0';
  if (!result->ok) return;
  if (!dd_gfx_install_sheet(game, type, result->pixels, result->pixels_w, result->pixels_h))
    snprintf(kind->error, sizeof(kind->error), "The %s sheet could not be uploaded.", dd_asset_types[type].label);
  kind->sheet_w = result->width;
  kind->sheet_h = result->height;
  memcpy(kind->empty, result->empty, sizeof(kind->empty));
  kind->empty_count = dd_assets_empty_count(kind, type);
  if (result->preview) {
    free(kind->preview);
    kind->preview = result->preview;
    kind->preview_w = result->preview_w;
    kind->preview_h = result->preview_h;
    kind->preview_dirty = true;
    result->preview = NULL;
  }
}

// Makes a type's sheet from the selection and installs it, at once.
static void load_now(ft_game *game, int type) {
  dd_asset_kind_t *kind = &dd_assets(game)->kinds[type];
  dd_compose_request_t request;
  dd_asset_result_t result;
  memset(&result, 0, sizeof(result));
  result.stamp = kind->revision;
  if (request_build(game, type, &request)) {
    dd_sheet_t sheet;
    if (compose(game, &request, NULL, &sheet, result.message, sizeof(result.message)))
      finish_compose(&dd_asset_types[type], &sheet, &result);
    free(sheet.pixels);
    request_free(&request);
  }
  if (result.message[0]) dd_log(game, FT_LOG_WARN, "%s", result.message);
  sheet_apply(game, type, &result);
  kind->requested = kind->revision;
  free(result.pixels);
  free(result.preview);
}

void dd_assets_load_type(ft_game *game, int type) {
  if (!game || game->headless || type < 0 || type >= DD_ASSET_TYPE_COUNT || !dd_assets(game)) return;
  load_now(game, type);
}

void dd_assets_load(ft_game *game) {
  if (!game || game->headless || !dd_assets(game)) return;
  for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type)
    load_now(game, type);
  game->assets->synchronous = true;
}

static void request_compose(ft_game *game, int type) {
  dd_asset_kind_t *kind = &game->assets->kinds[type];
  jobs_drop(game->assets->worker, JOB_COMPOSE, type);
  dd_asset_job_t *job = calloc(1, sizeof(*job));
  if (!job) return;
  job->kind = JOB_COMPOSE;
  job->type = type;
  job->stamp = kind->revision;
  if (!request_build(game, type, &job->request)) {
    job_free(job);
    return;
  }
  kind->requested = kind->revision;
  job_queue(game, job, true);
}

bool dd_assets_save(ft_game *game, int type, const char *name, const char *directory) {
  dd_assets_t *assets = dd_assets(game);
  if (!assets || type < 0 || type >= DD_ASSET_TYPE_COUNT || !dd_assets_valid_name(name) || !directory || !*directory)
    return false;
  dd_assets_make_directory(directory);
  dd_asset_job_t *job = calloc(1, sizeof(*job));
  if (!job) return false;
  job->kind = JOB_SAVE;
  job->type = type;
  char file[DD_ASSET_NAME_SIZE + 8];
  snprintf(file, sizeof(file), "%s.png", name);
  join_path(job->path, sizeof(job->path), directory, file);
  if (!request_build(game, type, &job->request)) {
    job_free(job);
    return false;
  }
  assets->export_busy = true;
  assets->export_type = type;
  assets->export_error[0] = '\0';
  job_queue(game, job, true);
  return true;
}

static void saved(ft_game *game, dd_asset_result_t *result) {
  dd_assets_t *assets = game->assets;
  assets->export_busy = false;
  if (!result->ok) {
    snprintf(assets->export_error, sizeof(assets->export_error), "%s",
             result->message[0] ? result->message : "The sheet could not be saved.");
    return;
  }
  // The file may be one a sheet was decoded from before.
  struct dd_asset_worker *worker = assets->worker;
  if (worker) {
    pthread_mutex_lock(&worker->lock);
    ++worker->cache_generation;
    pthread_mutex_unlock(&worker->lock);
  }
  snprintf(assets->saved_path, sizeof(assets->saved_path), "%s", result->message);
  assets->saved_type = result->type;
  assets->export_open = false;
  // It is a pack now: the selection becomes it, and nothing is mixed into it.
  const int type = result->type;
  dd_asset_kind_t *kind = &assets->kinds[type];
  dd_assets_scan(game, type);
  const char *slash = strrchr(result->message, '/');
#ifdef _WIN32
  const char *backslash = strrchr(result->message, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  char name[DD_ASSET_NAME_SIZE];
  snprintf(name, sizeof(name), "%s", slash ? slash + 1 : result->message);
  char *extension = strrchr(name, '.');
  if (extension) *extension = '\0';
  kind->override_count = 0;
  snprintf(kind->base, DD_ASSET_NAME_SIZE, "%s", name);
  selection_changed(game, kind);
}

void dd_assets_update(ft_game *game) {
  if (!game || game->headless || !game->gfx.ready || !game->assets) return;
  dd_assets_t *assets = game->assets;
  struct dd_asset_worker *worker = assets->worker;

  // What the worker made, oldest first.
  dd_asset_result_t *results = NULL;
  if (worker) {
    pthread_mutex_lock(&worker->lock);
    results = worker->results;
    worker->results = NULL;
    pthread_mutex_unlock(&worker->lock);
  }
  dd_asset_result_t *ordered = NULL;
  while (results) {
    dd_asset_result_t *next = results->next;
    results->next = ordered;
    ordered = results;
    results = next;
  }
  while (ordered) {
    dd_asset_result_t *result = ordered;
    ordered = ordered->next;
    dd_asset_kind_t *kind = &assets->kinds[result->type];
    const dd_asset_type_info_t *info = &dd_asset_types[result->type];
    switch (result->kind) {
    case JOB_PREVIEW: {
      if (result->stamp != kind->generation || result->pack < 0 || result->pack >= kind->pack_count) break;
      dd_asset_pack_t *pack = &kind->packs[result->pack];
      pack->state = result->ok ? DD_PACK_READY : DD_PACK_FAILED;
      pack->width = result->width;
      pack->height = result->height;
      pack->resized = result->resized;
      const int per_page = pages_per_type(info);
      dd_asset_page_t *page = &kind->pages[result->pack / per_page];
      if (page->pending > 0) --page->pending;
      if (result->ok && page->pixels) {
        const int columns = DD_ASSET_PAGE_SIZE / info->thumb_w;
        const int slot = result->pack % per_page;
        const int x = (slot % columns) * info->thumb_w, y = (slot / columns) * info->thumb_h;
        for (int row = 0; row < info->thumb_h; ++row)
          memcpy(page->pixels + ((size_t)(y + row) * DD_ASSET_PAGE_SIZE + x) * 4,
                 result->pixels + (size_t)row * info->thumb_w * 4, (size_t)info->thumb_w * 4);
      }
      page->dirty = true;
      break;
    }
    case JOB_COMPOSE:
      // (only the newest selection is shown; an older one's sheet is dropped)
      if (result->stamp == kind->revision) sheet_apply(game, result->type, result);
      break;
    case JOB_SAVE:
      saved(game, result);
      break;
    default:
      break;
    }
    result_free(result);
  }

  // A changed selection: made by the worker, or at once before the first frame.
  for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type) {
    dd_asset_kind_t *kind = &assets->kinds[type];
    if (kind->revision == kind->requested) continue;
    if (assets->synchronous) load_now(game, type);
    else request_compose(game, type);
  }
  assets->synchronous = false;

  // Decoded sheets are only kept while the window may want them again (the
  // cache is the main thread's while the worker is not running, and only the
  // main thread starts it).
  if (!game->show_assets && worker) {
    pthread_mutex_lock(&worker->lock);
    const bool idle = !worker->running;
    pthread_mutex_unlock(&worker->lock);
    if (idle && !cache_empty(&worker->cache)) cache_clear(&worker->cache);
  }
}

void dd_assets_release_gpu(ft_game *game) {
  if (!game || !game->assets) return;
  for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type) {
    dd_asset_kind_t *kind = &game->assets->kinds[type];
    pages_release(game, kind);
    // (previews come back with the next scan)
    kind->scanned = false;
    if (kind->preview_id && game->engine->imgui_texture_release) game->engine->imgui_texture_release(kind->preview_id);
    if (kind->preview_texture) game->engine->texture_destroy(kind->preview_texture);
    kind->preview_id = 0;
    kind->preview_texture = NULL;
    kind->preview_dirty = kind->preview != NULL;
  }
}

void dd_assets_cleanup(ft_game *game) {
  if (!game || !game->assets) return;
  dd_assets_t *assets = game->assets;
  if (assets->worker) {
    worker_stop(assets->worker);
    pthread_mutex_destroy(&assets->worker->lock);
    free(assets->worker);
  }
  for (int type = 0; type < DD_ASSET_TYPE_COUNT; ++type) {
    dd_asset_kind_t *kind = &assets->kinds[type];
    for (int i = 0; i < DD_ASSET_MAX_PAGES; ++i)
      free(kind->pages[i].pixels);
    free(kind->packs);
    free(kind->overrides);
    free(kind->mix_setting);
    free(kind->preview);
  }
  free(assets);
  game->assets = NULL;
}
