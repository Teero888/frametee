#ifndef DD_ASSETS_H
#define DD_ASSETS_H

// DDNet's assets: the sheets the game is drawn with (entities, game, emoticons,
// particles, HUD, extras), taken from DDNet's own asset directories and the
// module's, as DDNet's asset settings do. On top of the sheet a type uses, its
// parts (the sprites DDNet cuts out of it) can each come from another pack;
// the sheet that mix makes can be saved as a pack of its own.
//
// dd_assets.c keeps the catalog, the selection and the sheets; it decodes and
// composes on a worker thread and swaps the game's textures before a frame is
// drawn. dd_assets_window.c is the window.

#include "dd_internal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  DD_ASSET_NAME_SIZE = 64,
  DD_ASSET_PATH_SIZE = 1024,
  DD_ASSET_PAGE_SIZE = 1024,
  DD_ASSET_MAX_PAGES = 64,
  DD_ASSET_MAX_PARTS = 256,
};

// A sprite of a sheet, the unit a mix is made of: a rectangle of grid cells.
typedef struct dd_asset_part {
  const char *id;   // DDNet's sprite name, which the config keeps
  const char *name; // as the window shows it
  uint8_t group;    // index into the type's groups
  uint8_t x, y, w, h;
} dd_asset_part_t;

typedef struct dd_asset_type_info {
  const char *id;        // the directory under assets/, as in DDNet
  const char *label;     // the tab
  const char *pack_file; // the sheet in a pack that is a directory: <pack_file>.png
  const char *builtin;   // the module's own sheet, the pack called "default"
  int grid_x, grid_y;
  int default_w, default_h; // the size the module's sprite tables are in
  int thumb_w, thumb_h;     // a pack's preview in the window
  const char *const *groups;
  int group_count;
  const dd_asset_part_t *parts;
  int part_count;
  // Whether a part a pack leaves fully transparent is pointed out. DDNet 20
  // draws the default's there instead (for packs made before a sprite
  // existed), which makes an invisible pack show the default; here a pack is
  // drawn as it is, and the window offers the default for its empty parts.
  // Not for entities, where an empty tile is a tile.
  bool mark_empty;
} dd_asset_type_info_t;

extern const dd_asset_type_info_t dd_asset_types[DD_ASSET_TYPE_COUNT];

typedef enum dd_asset_origin {
  DD_ORIGIN_BUILTIN = 0,
  DD_ORIGIN_FRAMETEE,
  DD_ORIGIN_DDNET,
  DD_ORIGIN_DDNET_DATA,
} dd_asset_origin_t;

typedef enum dd_pack_state {
  DD_PACK_LOADING = 0,
  DD_PACK_READY,
  DD_PACK_FAILED,
} dd_pack_state_t;

typedef struct dd_asset_pack {
  char name[DD_ASSET_NAME_SIZE];
  char path[DD_ASSET_PATH_SIZE];
  uint8_t origin;
  uint8_t state;
  bool resized; // its size does not divide into the grid, so it is resized as DDNet does
  int width, height;
} dd_asset_pack_t;

// Previews of a type's packs, packed into textures the window draws from.
typedef struct dd_asset_page {
  ft_texture *texture;
  uint64_t texture_id;
  uint8_t *pixels; // kept until every preview on it is in
  int pending;
  bool dirty;
} dd_asset_page_t;

typedef struct dd_asset_override {
  uint16_t part;
  char pack[DD_ASSET_NAME_SIZE];
} dd_asset_override_t;

typedef struct dd_asset_kind {
  dd_asset_pack_t *packs;
  int pack_count, pack_capacity;
  bool scanned;
  uint32_t generation; // of the catalog; a preview made for an older one is dropped
  dd_asset_page_t pages[DD_ASSET_MAX_PAGES];

  // The selection: the pack the sheet is, and the parts taken from others.
  char base[DD_ASSET_NAME_SIZE];
  dd_asset_override_t *overrides;
  int override_count, override_capacity;
  char *mix_setting; // the overrides as the config keeps them, made for setting_get

  // The sheet: which selection it shows, and what the window draws of it.
  uint32_t revision, requested, applied;
  int sheet_w, sheet_h;
  uint64_t empty[DD_ASSET_MAX_PARTS / 64]; // parts left fully transparent by the pack they come from
  int empty_count;                         // of those, the ones not from the default
  uint8_t *preview;                        // the sheet, at most DD_ASSET_PAGE_SIZE on its long side
  int preview_w, preview_h;
  bool preview_dirty;
  ft_texture *preview_texture;
  uint64_t preview_id;
  char error[256];
} dd_asset_kind_t;

typedef struct dd_assets_t {
  dd_asset_kind_t kinds[DD_ASSET_TYPE_COUNT];
  // The worker that decodes and mixes (dd_assets.c).
  struct dd_asset_worker *worker;
  // Until the first frame is drawn, a selection is shown at once rather than
  // when the worker has it: the config only arrives after the sheets are made.
  bool synchronous;

  // The window (dd_assets_window.c).
  int tab;
  bool restore_tab;
  char search[96];
  int hovered_part;      // on the sheet this frame
  int list_hovered_part; // in the list of parts last frame, which the sheet then shows
  int context_pack;
  bool picker_open;
  int picker_part;   // the part the picker chooses for, or -1 for a whole group
  int picker_group;  // the group it chooses for
  bool picker_whole; // apply to the whole group of picker_part
  char picker_search[96];
  bool export_open;
  char export_name[DD_ASSET_NAME_SIZE];
  int export_location;
  int export_type;
  bool export_busy;
  char export_error[256];
  char saved_path[DD_ASSET_PATH_SIZE];
  int saved_type;
} dd_assets_t;

dd_assets_t *dd_assets(ft_game *game);

// The catalog of a type: the built-in pack first, then every pack found, by
// name; a name found twice is the first one's.
void dd_assets_scan(ft_game *game, int type);
int dd_assets_find_pack(const dd_asset_kind_t *kind, const char *name);
// Uploads the previews made since the last call. Window only (inside the UI).
void dd_assets_upload_pages(ft_game *game, int type);
// Where a pack's preview, or a part of it, is: a texture and its uvs. False
// while it is not made yet.
bool dd_assets_pack_uv(ft_game *game, int type, int pack, const dd_asset_part_t *part, uint64_t *texture_id,
                       float uv[4]);
// The sheet as it is shown (the window's preview of it).
bool dd_assets_sheet_texture(ft_game *game, int type, uint64_t *texture_id);
int dd_assets_previews_pending(const dd_asset_kind_t *kind);

// Changing the selection; each saves the config.
void dd_assets_select(ft_game *game, int type, const char *pack);
// `pack` NULL (or the base) takes the part from the base again.
void dd_assets_set_part(ft_game *game, int type, int part, const char *pack);
void dd_assets_set_group(ft_game *game, int type, int group, const char *pack);
void dd_assets_clear_mix(ft_game *game, int type);
// Takes every empty part from the default, as DDNet 20 shows them. False when none is empty.
bool dd_assets_fill_empty(ft_game *game, int type);
int dd_assets_empty_count(const dd_asset_kind_t *kind, int type);
bool dd_assets_part_empty(const dd_asset_kind_t *kind, int part);
// The pack a part comes from (its override, else the base), and whether that is an override.
const char *dd_assets_part_pack(const dd_asset_kind_t *kind, int part, bool *overridden);
int dd_assets_pack_part_count(const dd_asset_kind_t *kind, const char *pack);

// The directories packs are kept in: the module's own, and DDNet's (its user
// directory). False when there is none.
bool dd_assets_directory(ft_game *game, int type, int origin, char *out, size_t out_size);
// Creates a directory and the ones it is in.
void dd_assets_make_directory(const char *path);
// Saves the sheet the selection makes as `<directory>/<name>.png`, on the worker.
bool dd_assets_save(ft_game *game, int type, const char *name, const char *directory);
// A pack name the directories can hold: no separators, no "default".
bool dd_assets_valid_name(const char *name);

#endif // DD_ASSETS_H
