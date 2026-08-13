#ifndef DD_MAPS_H
#define DD_MAPS_H

#include "dd_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include "dd_imgui.h"
#include "dd_threads.h"

typedef struct {
    char name[128];
    char repo[16];            // "ddnet", "kog", "unique"
    char map_path[256];       // e.g. "types/brutal/maps/-JustDoIt-.map"
    char thumbnail_path[256]; // e.g. "ddnet/-JustDoIt-.png"
    
    // Metadata fields
    char type[64];            // DDNet map type e.g. "Brutal", "Novice"
    int difficulty;           // DDNet difficulty (1-5)
    int points;               // DDNet / KoG points (e.g. 24)
    char category[64];        // Unique map category e.g. "Short", "Medium", "Long"
    char kog_difficulty[64];  // KoG difficulty e.g. "Easy", "Main", "Hard"
    int stars;                // KoG stars (1-5)
    char length[16];          // KoG length e.g. "S", "M", "L"
    
    char local_map_path[512];
    char local_thumb_path[512];
    
    ft_texture *thumb_texture_res;
    ImTextureRef *thumb_preview_texture;
    
    bool thumb_loaded;
    bool thumb_fetching;
    bool thumb_failed;
    bool visible_this_frame;
    
    bool map_downloading;
    bool map_download_failed;
    bool map_downloaded;
} online_map_item_t;

typedef struct {
    char name[64];
} map_type_entry_t;

typedef struct {
    online_map_item_t *items;
    int count;
    int capacity;
} online_map_category_t;

typedef enum {
    SORT_NAME_ASC = 0,
    SORT_NAME_DESC,
    SORT_POINTS_DESC,
    SORT_POINTS_ASC,
    SORT_STARS_DESC,
    SORT_STARS_ASC,
} map_sort_mode_t;

typedef struct online_map_manager_t {
    online_map_category_t ddnet;
    online_map_category_t kog;
    online_map_category_t unique;
    
    // Dynamic map types parsed from maps.json "types" object
    map_type_entry_t ddnet_types[64];
    int ddnet_type_count;
    
    map_type_entry_t unique_types[64];
    int unique_type_count;
    
    map_type_entry_t kog_types[64];
    int kog_type_count;
    
    map_type_entry_t kog_lengths[32];
    int kog_length_count;
    
    bool initialized;
    bool json_fetching;
    bool json_loaded;
    bool json_error;
    bool is_offline;
    bool needs_reload;
    char error_msg[256];
    
    int active_tab; // 0: ddnet, 1: kog, 2: unique
    char search_filter[128];
    bool downloaded_only;
    
    // Sort & Filter state
    int sort_field;       // 0: Name, 1: Points, 2: Stars
    bool sort_descending; // false: Forward/Ascending, true: Backward/Descending
    
    // DDNet Filters
    char filter_ddnet_type[64];      // "All", "Brutal", "DDmaX.Easy", etc.
    int filter_ddnet_difficulty;     // 0 = All, 1-5
    
    // Unique Filters
    char filter_unique_category[64]; // "All", "Short", etc.
    
    // KoG Filters
    char filter_kog_difficulty[64];  // "All", "Easy", "Main", "Hard", etc.
    int filter_kog_stars;            // 0 = All, 1-5
    char filter_kog_length[16];      // "All", "S", "M", "L"
    
    // Category PNG Icon Textures
    ft_texture *icon_ddnet_res;
    ImTextureRef *icon_ddnet_tex;
    
    ft_texture *icon_unique_res;
    ImTextureRef *icon_unique_tex;
    
    ft_texture *icon_kog_res;
    ImTextureRef *icon_kog_tex;
    
    // Background download worker management
    pthread_mutex_t mutex;
} online_map_manager_t;

void online_map_manager_init(online_map_manager_t *mgr, ft_game *game);
void online_map_manager_update(online_map_manager_t *mgr, ft_game *game);
void online_map_manager_cleanup(online_map_manager_t *mgr, ft_game *game);

// Renders the online map grid & search controls. Returns true if a map was selected and loaded!
bool render_online_map_browser(ft_game *game, online_map_manager_t *mgr, float avail_width, float avail_height);

#endif // DD_MAPS_H
