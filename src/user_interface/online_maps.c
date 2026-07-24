#include "online_maps.h"
#include "user_interface.h"
#include "widgets/imcol.h"
#include <stb_image.h>
#include <symbols.h>
#include <renderer/graphics_backend.h>
#include <system/fs.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

static void ensure_parent_dirs_exist(const char *filepath) {
    char tmp[1024];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char ch = *p;
            *p = '\0';
            fs_mkdir(tmp);
            *p = ch;
        }
    }
}

static void url_encode_path(const char *src, char *dst, size_t dst_size) {
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_size; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '/') {
            dst[d++] = '/';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = c;
        } else {
            snprintf(dst + d, dst_size - d, "%%%02X", c);
            d += 3;
        }
    }
    dst[d] = '\0';
}

static size_t curl_write_file_cb(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static bool http_download_to_file(const char *url, const char *dest_file_path) {
    ensure_parent_dirs_exist(dest_file_path);
    char temp_path[1024];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", dest_file_path);
    
    FILE *fp = fs_open(temp_path, "wb");
    if (!fp) return false;
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        remove(temp_path);
        return false;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "frametee/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);
    
    if (res == CURLE_OK && (http_code == 200 || http_code == 0)) {
        remove(dest_file_path);
        rename(temp_path, dest_file_path);
        return true;
    } else {
        remove(temp_path);
        return false;
    }
}

static void add_item_to_category(online_map_category_t *cat, const online_map_item_t *item) {
    if (cat->count >= cat->capacity) {
        cat->capacity = cat->capacity == 0 ? 128 : cat->capacity * 2;
        cat->items = realloc(cat->items, cat->capacity * sizeof(online_map_item_t));
    }
    cat->items[cat->count++] = *item;
}

static void clear_category(online_map_category_t *cat, gfx_handler_t *gfx) {
    if (!cat->items) return;
    for (int i = 0; i < cat->count; i++) {
        online_map_item_t *item = &cat->items[i];
        if (item->thumb_preview_texture) {
            destroy_imgui_texture_ref(&item->thumb_preview_texture);
        }
        if (gfx && item->thumb_texture_res) {
            renderer_destroy_texture(gfx, item->thumb_texture_res);
            item->thumb_texture_res = NULL;
        }
    }
    free(cat->items);
    cat->items = NULL;
    cat->count = 0;
    cat->capacity = 0;
}

static void parse_category_json(cJSON *arr, online_map_category_t *cat, const char *config_dir) {
    if (!arr || !cJSON_IsArray(arr)) return;
    
    int size = cJSON_GetArraySize(arr);
    for (int i = 0; i < size; i++) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        
        cJSON *j_name = cJSON_GetObjectItem(obj, "name");
        cJSON *j_repo = cJSON_GetObjectItem(obj, "repo");
        cJSON *j_map_path = cJSON_GetObjectItem(obj, "map_path");
        cJSON *j_thumb_path = cJSON_GetObjectItem(obj, "thumbnail_path");
        
        if (!j_name || !j_repo || !j_map_path || !j_thumb_path) continue;
        if (!j_name->valuestring || !j_repo->valuestring || !j_map_path->valuestring || !j_thumb_path->valuestring) continue;
        
        online_map_item_t item;
        memset(&item, 0, sizeof(item));
        
        strncpy(item.name, j_name->valuestring, sizeof(item.name) - 1);
        strncpy(item.repo, j_repo->valuestring, sizeof(item.repo) - 1);
        strncpy(item.map_path, j_map_path->valuestring, sizeof(item.map_path) - 1);
        strncpy(item.thumbnail_path, j_thumb_path->valuestring, sizeof(item.thumbnail_path) - 1);
        
        // Parse metadata fields
        cJSON *j_type = cJSON_GetObjectItem(obj, "type");
        if (j_type && j_type->valuestring) {
            strncpy(item.type, j_type->valuestring, sizeof(item.type) - 1);
        }
        
        cJSON *j_diff = cJSON_GetObjectItem(obj, "difficulty");
        if (j_diff) {
            if (cJSON_IsNumber(j_diff)) {
                item.difficulty = j_diff->valueint;
            } else if (cJSON_IsString(j_diff) && j_diff->valuestring) {
                strncpy(item.kog_difficulty, j_diff->valuestring, sizeof(item.kog_difficulty) - 1);
            }
        }
        
        cJSON *j_pts = cJSON_GetObjectItem(obj, "points");
        if (j_pts && cJSON_IsNumber(j_pts)) {
            item.points = j_pts->valueint;
        }
        
        cJSON *j_cat = cJSON_GetObjectItem(obj, "category");
        if (j_cat && j_cat->valuestring) {
            strncpy(item.category, j_cat->valuestring, sizeof(item.category) - 1);
        }
        
        cJSON *j_stars = cJSON_GetObjectItem(obj, "stars");
        if (j_stars && cJSON_IsNumber(j_stars)) {
            item.stars = j_stars->valueint;
        }
        
        cJSON *j_len = cJSON_GetObjectItem(obj, "length");
        if (j_len && j_len->valuestring) {
            strncpy(item.length, j_len->valuestring, sizeof(item.length) - 1);
        }
        
        snprintf(item.local_map_path, sizeof(item.local_map_path), "%s/cache/maps/%s/%s", config_dir, item.repo, item.map_path);
        snprintf(item.local_thumb_path, sizeof(item.local_thumb_path), "%s/cache/thumbs/%s", config_dir, item.thumbnail_path);
        
        FILE *f_check = fs_open(item.local_map_path, "rb");
        if (f_check) {
            item.map_downloaded = true;
            fclose(f_check);
        } else {
            item.map_downloaded = false;
        }
        
        add_item_to_category(cat, &item);
    }
}

static void parse_type_array(cJSON *arr, map_type_entry_t *out_types, int *out_count, int max_capacity) {
    *out_count = 0;
    if (!arr || !cJSON_IsArray(arr)) return;
    int size = cJSON_GetArraySize(arr);
    for (int i = 0; i < size && *out_count < max_capacity; i++) {
        cJSON *elem = cJSON_GetArrayItem(arr, i);
        if (elem && elem->valuestring) {
            strncpy(out_types[*out_count].name, elem->valuestring, sizeof(out_types[0].name) - 1);
            out_types[*out_count].name[sizeof(out_types[0].name) - 1] = '\0';
            (*out_count)++;
        }
    }
}

static const char *g_ddnet_type_order[] = {
    "Novice", "Moderate", "Brutal", "Insane", "Dummy",
    "DDmaX.Easy", "DDmaX.Next", "DDmaX.Pro", "DDmaX.Nut",
    "Oldschool", "Solo", "Race", "Fun", "Event"
};

static const char *g_kog_type_order[] = {
    "Solo", "Easy", "Main", "Hard", "Insane", "Extreme", "Mod", "Mods", "Others"
};

static const char *g_unique_type_order[] = {
    "Short", "Middle", "Long Easy", "Long Advanced", "Long Hard", "Fastcap"
};

static const char *g_kog_length_order[] = {
    "-", "XXS", "XS", "S", "M", "L", "XL", "XXL", "XXXL", "WTF"
};

static int get_type_priority(const char *name, const char **order_table, int order_size) {
    if (!name) return 999;
    for (int i = 0; i < order_size; i++) {
        if (strcasecmp(name, order_table[i]) == 0) return i;
        char clean1[64] = {0}, clean2[64] = {0};
        int d1 = 0, d2 = 0;
        for (int s = 0; name[s] && d1 < 63; s++) {
            if (isalnum((unsigned char)name[s])) clean1[d1++] = tolower((unsigned char)name[s]);
        }
        for (int s = 0; order_table[i][s] && d2 < 63; s++) {
            if (isalnum((unsigned char)order_table[i][s])) clean2[d2++] = tolower((unsigned char)order_table[i][s]);
        }
        if (d1 > 0 && d2 > 0 && strcmp(clean1, clean2) == 0) return i;
    }
    return 999;
}

static void sort_types_by_custom_order(map_type_entry_t *types, int count, const char **order_table, int order_size) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int rank_i = get_type_priority(types[i].name, order_table, order_size);
            int rank_j = get_type_priority(types[j].name, order_table, order_size);
            if (rank_j < rank_i) {
                map_type_entry_t tmp = types[i];
                types[i] = types[j];
                types[j] = tmp;
            }
        }
    }
}

static bool load_maps_from_json_string(online_map_manager_t *mgr, const char *json_str, const char *config_dir) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return false;
    
    pthread_mutex_lock(&mgr->mutex);
    
    // Parse dynamic "types" object if available
    cJSON *types = cJSON_GetObjectItem(root, "types");
    if (types) {
        parse_type_array(cJSON_GetObjectItem(types, "ddnet"), mgr->ddnet_types, &mgr->ddnet_type_count, 64);
        parse_type_array(cJSON_GetObjectItem(types, "unique"), mgr->unique_types, &mgr->unique_type_count, 64);
        parse_type_array(cJSON_GetObjectItem(types, "kog"), mgr->kog_types, &mgr->kog_type_count, 64);
        
        sort_types_by_custom_order(mgr->ddnet_types, mgr->ddnet_type_count, g_ddnet_type_order, 14);
        sort_types_by_custom_order(mgr->unique_types, mgr->unique_type_count, g_unique_type_order, 6);
        sort_types_by_custom_order(mgr->kog_types, mgr->kog_type_count, g_kog_type_order, 9);
    }
    
    cJSON *j_kog_lens = cJSON_GetObjectItem(root, "kog_lengths");
    if (!j_kog_lens && types) {
        j_kog_lens = cJSON_GetObjectItem(types, "kog_lengths");
    }
    if (j_kog_lens) {
        parse_type_array(j_kog_lens, mgr->kog_lengths, &mgr->kog_length_count, 32);
        sort_types_by_custom_order(mgr->kog_lengths, mgr->kog_length_count, g_kog_length_order, 10);
    }
    
    cJSON *maps = cJSON_GetObjectItem(root, "maps");
    if (maps) {
        mgr->ddnet.count = 0;
        mgr->kog.count = 0;
        mgr->unique.count = 0;
        
        parse_category_json(cJSON_GetObjectItem(maps, "ddnet"), &mgr->ddnet, config_dir);
        parse_category_json(cJSON_GetObjectItem(maps, "kog"), &mgr->kog, config_dir);
        parse_category_json(cJSON_GetObjectItem(maps, "unique"), &mgr->unique, config_dir);
        
        mgr->json_loaded = true;
        mgr->json_error = false;
    }
    
    pthread_mutex_unlock(&mgr->mutex);
    
    cJSON_Delete(root);
    return true;
}

// Background thread structure for fetching maps.json and category icons
typedef struct {
    online_map_manager_t *mgr;
    char config_dir[512];
    char json_file_path[512];
} json_fetch_args_t;

static void *fetch_json_thread(void *arg) {
    json_fetch_args_t *args = (json_fetch_args_t *)arg;
    
    // 1. Download tab icons if not present locally
    char icon_ddnet_path[1024], icon_unique_path[1024], icon_kog_path[1024];
    snprintf(icon_ddnet_path, sizeof(icon_ddnet_path), "%s/cache/icons/ddnet.png", args->config_dir);
    snprintf(icon_unique_path, sizeof(icon_unique_path), "%s/cache/icons/unique.png", args->config_dir);
    snprintf(icon_kog_path, sizeof(icon_kog_path), "%s/cache/icons/kog.png", args->config_dir);
    
    http_download_to_file("https://info.ddnet.org/icons/ddnet.png", icon_ddnet_path);
    http_download_to_file("https://info.ddnet.org/icons/unique.png", icon_unique_path);
    http_download_to_file("https://info.ddnet.org/icons/kog.png", icon_kog_path);
    
    // 2. Download maps.json
    const char *url = "https://raw.githubusercontent.com/Teero888/tw-thumbs/refs/heads/master/maps.json";
    bool success = http_download_to_file(url, args->json_file_path);
    
    if (success) {
        pthread_mutex_lock(&args->mgr->mutex);
        args->mgr->is_offline = false;
        args->mgr->needs_reload = true;
        pthread_mutex_unlock(&args->mgr->mutex);
    } else {
        pthread_mutex_lock(&args->mgr->mutex);
        if (!args->mgr->json_loaded) {
            args->mgr->json_error = true;
            snprintf(args->mgr->error_msg, sizeof(args->mgr->error_msg), "Could not connect to online server.");
        }
        args->mgr->is_offline = true;
        pthread_mutex_unlock(&args->mgr->mutex);
    }
    
    args->mgr->json_fetching = false;
    free(args);
    return NULL;
}

// Thumbnail worker pool
#define MAX_THUMB_LOAD_TASKS 6
typedef struct {
    online_map_item_t *target;
    char url[1024];
    char dest_path[512];
    bool in_use;
    bool done;
    bool success;
    unsigned char *decoded_pixels;
    int img_width;
    int img_height;
} thumb_load_task_t;

static thumb_load_task_t g_thumb_tasks[MAX_THUMB_LOAD_TASKS];
static pthread_t g_thumb_threads[MAX_THUMB_LOAD_TASKS];

static void *thumb_load_worker(void *arg) {
    thumb_load_task_t *task = (thumb_load_task_t *)arg;
    task->decoded_pixels = NULL;
    task->img_width = 0;
    task->img_height = 0;

    if (task->url[0] != '\0') {
        task->success = http_download_to_file(task->url, task->dest_path);
    } else {
        task->success = true; // loading local file
    }

    if (task->success) {
        FILE *f = fs_open(task->dest_path, "rb");
        if (f) {
            int channels;
            task->decoded_pixels = stbi_load_from_file(f, &task->img_width, &task->img_height, &channels, STBI_rgb_alpha);
            fclose(f);
            if (!task->decoded_pixels) {
                task->success = false;
            }
        } else {
            task->success = false;
        }
    }
    task->done = true;
    return NULL;
}

// Map file download task
typedef struct {
    online_map_item_t *target;
    char url[1024];
    char dest_path[512];
    bool in_use;
    bool done;
    bool success;
} map_download_task_t;

static map_download_task_t g_map_task;
static pthread_t g_map_thread;

static void *map_download_worker(void *arg) {
    map_download_task_t *task = (map_download_task_t *)arg;
    task->success = http_download_to_file(task->url, task->dest_path);
    task->done = true;
    return NULL;
}

void online_map_manager_init(online_map_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
    pthread_mutex_init(&mgr->mutex, NULL);
    
    char config_dir[512];
    if (!fs_get_config_dir(config_dir, sizeof(config_dir))) {
        return;
    }
    
    char json_file_path[1024];
    snprintf(json_file_path, sizeof(json_file_path), "%s/cache/maps.json", config_dir);
    
    // 1. If maps.json already exists on disk, parse it immediately (allows offline mode!)
    FILE *f = fs_open(json_file_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, f);
                buf[n] = '\0';
                load_maps_from_json_string(mgr, buf, config_dir);
                free(buf);
            }
        }
        fclose(f);
    }
    
    // Default filter state
    strncpy(mgr->filter_ddnet_type, "All", sizeof(mgr->filter_ddnet_type) - 1);
    strncpy(mgr->filter_unique_category, "All", sizeof(mgr->filter_unique_category) - 1);
    strncpy(mgr->filter_kog_difficulty, "All", sizeof(mgr->filter_kog_difficulty) - 1);
    strncpy(mgr->filter_kog_length, "All", sizeof(mgr->filter_kog_length) - 1);
    
    // 2. Launch background thread to update maps.json & category icons
    json_fetch_args_t *args = malloc(sizeof(json_fetch_args_t));
    args->mgr = mgr;
    snprintf(args->config_dir, sizeof(args->config_dir), "%.*s", (int)(sizeof(args->config_dir) - 1), config_dir);
    snprintf(args->json_file_path, sizeof(args->json_file_path), "%.*s", (int)(sizeof(args->json_file_path) - 1), json_file_path);
    
    mgr->json_fetching = true;
    pthread_t thread;
    if (pthread_create(&thread, NULL, fetch_json_thread, args) == 0) {
        pthread_detach(thread);
    } else {
        mgr->json_fetching = false;
        free(args);
    }
    
    mgr->initialized = true;
}

static void update_category_textures(online_map_category_t *cat, gfx_handler_t *gfx) {
    for (int i = 0; i < cat->count; i++) {
        online_map_item_t *item = &cat->items[i];
        
        if (!item->visible_this_frame && item->thumb_loaded) {
            if (item->thumb_preview_texture) {
                destroy_imgui_texture_ref(&item->thumb_preview_texture);
            }
            if (gfx && item->thumb_texture_res) {
                renderer_destroy_texture(gfx, item->thumb_texture_res);
                item->thumb_texture_res = NULL;
            }
            item->thumb_loaded = false;
        }
        
        item->visible_this_frame = false;
    }
}

static void load_icon_texture_if_needed(gfx_handler_t *gfx, const char *file_path, texture_t **out_res, struct ImTextureRef_c **out_tex) {
    if (*out_tex != NULL) return;
    FILE *f = fs_open(file_path, "rb");
    if (!f) return;
    fclose(f);
    
    *out_res = renderer_load_texture(gfx, file_path);
    if (*out_res) {
        *out_tex = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
            (*out_res)->sampler, (*out_res)->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }
}

void online_map_manager_update(online_map_manager_t *mgr, gfx_handler_t *gfx) {
    if (!mgr->initialized) return;
    
    pthread_mutex_lock(&mgr->mutex);
    bool should_reload = mgr->needs_reload;
    pthread_mutex_unlock(&mgr->mutex);
    
    char config_dir[512];
    bool has_config_dir = fs_get_config_dir(config_dir, sizeof(config_dir));
    
    if (should_reload && has_config_dir) {
        // Safe to clear and destroy previous textures/references on the main thread
        clear_category(&mgr->ddnet, gfx);
        clear_category(&mgr->kog, gfx);
        clear_category(&mgr->unique, gfx);
        
        char json_file_path[1024];
        snprintf(json_file_path, sizeof(json_file_path), "%s/cache/maps.json", config_dir);
        FILE *f = fs_open(json_file_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                char *buf = malloc(sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, sz, f);
                    buf[n] = '\0';
                    load_maps_from_json_string(mgr, buf, config_dir);
                    free(buf);
                }
            }
            fclose(f);
        }
        pthread_mutex_lock(&mgr->mutex);
        mgr->needs_reload = false;
        pthread_mutex_unlock(&mgr->mutex);
    }
    
    if (has_config_dir) {
        char p1[1024], p2[1024], p3[1024];
        snprintf(p1, sizeof(p1), "%s/cache/icons/ddnet.png", config_dir);
        snprintf(p2, sizeof(p2), "%s/cache/icons/unique.png", config_dir);
        snprintf(p3, sizeof(p3), "%s/cache/icons/kog.png", config_dir);
        
        load_icon_texture_if_needed(gfx, p1, &mgr->icon_ddnet_res, &mgr->icon_ddnet_tex);
        load_icon_texture_if_needed(gfx, p2, &mgr->icon_unique_res, &mgr->icon_unique_tex);
        load_icon_texture_if_needed(gfx, p3, &mgr->icon_kog_res, &mgr->icon_kog_tex);
    }
    
    // Process finished thumbnail download tasks
    for (int i = 0; i < MAX_THUMB_LOAD_TASKS; i++) {
        if (g_thumb_tasks[i].in_use && g_thumb_tasks[i].done) {
            pthread_join(g_thumb_threads[i], NULL);
            online_map_item_t *item = g_thumb_tasks[i].target;
            if (item) {
                item->thumb_fetching = false;
                if (!g_thumb_tasks[i].success || !g_thumb_tasks[i].decoded_pixels) {
                    item->thumb_failed = true;
                } else {
                    item->thumb_texture_res = renderer_create_texture_from_rgba(gfx, g_thumb_tasks[i].decoded_pixels, g_thumb_tasks[i].img_width, g_thumb_tasks[i].img_height);
                    if (item->thumb_texture_res) {
                        item->thumb_preview_texture = ImTextureRef_ImTextureRef_TextureID((ImTextureID)ImGui_ImplVulkan_AddTexture(
                            item->thumb_texture_res->sampler, item->thumb_texture_res->image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                        item->thumb_loaded = true;
                    } else {
                        item->thumb_failed = true;
                    }
                }
                
                if (g_thumb_tasks[i].decoded_pixels) {
                    stbi_image_free(g_thumb_tasks[i].decoded_pixels);
                    g_thumb_tasks[i].decoded_pixels = NULL;
                }
            }
            g_thumb_tasks[i].in_use = false;
        }
    }
    
    // Process finished map download task
    if (g_map_task.in_use && g_map_task.done) {
        pthread_join(g_map_thread, NULL);
        online_map_item_t *item = g_map_task.target;
        if (item) {
            item->map_downloading = false;
            if (!g_map_task.success) {
                item->map_download_failed = true;
            } else {
                item->map_downloaded = true;
            }
        }
        g_map_task.in_use = false;
    }
    
    update_category_textures(&mgr->ddnet, gfx);
    update_category_textures(&mgr->kog, gfx);
    update_category_textures(&mgr->unique, gfx);
}

void online_map_manager_cleanup(online_map_manager_t *mgr, gfx_handler_t *gfx) {
    for (int i = 0; i < MAX_THUMB_LOAD_TASKS; i++) {
        if (g_thumb_tasks[i].in_use) {
            pthread_join(g_thumb_threads[i], NULL);
            if (g_thumb_tasks[i].decoded_pixels) {
                stbi_image_free(g_thumb_tasks[i].decoded_pixels);
                g_thumb_tasks[i].decoded_pixels = NULL;
            }
            g_thumb_tasks[i].in_use = false;
        }
    }
    if (g_map_task.in_use) {
        pthread_join(g_map_thread, NULL);
        g_map_task.in_use = false;
    }
    
    if (mgr->icon_ddnet_tex) destroy_imgui_texture_ref(&mgr->icon_ddnet_tex);
    if (gfx && mgr->icon_ddnet_res) renderer_destroy_texture(gfx, mgr->icon_ddnet_res);
    
    if (mgr->icon_unique_tex) destroy_imgui_texture_ref(&mgr->icon_unique_tex);
    if (gfx && mgr->icon_unique_res) renderer_destroy_texture(gfx, mgr->icon_unique_res);
    
    if (mgr->icon_kog_tex) destroy_imgui_texture_ref(&mgr->icon_kog_tex);
    if (gfx && mgr->icon_kog_res) renderer_destroy_texture(gfx, mgr->icon_kog_res);
    
    clear_category(&mgr->ddnet, gfx);
    clear_category(&mgr->kog, gfx);
    clear_category(&mgr->unique, gfx);
    
    pthread_mutex_destroy(&mgr->mutex);
    mgr->initialized = false;
}

static online_map_category_t *get_active_category(online_map_manager_t *mgr) {
    if (mgr->active_tab == 1) return &mgr->kog;
    if (mgr->active_tab == 2) return &mgr->unique;
    return &mgr->ddnet;
}

static void trigger_thumbnail_load(online_map_item_t *item, bool download) {
    if (item->thumb_fetching || item->thumb_loaded || item->thumb_failed) return;
    
    for (int i = 0; i < MAX_THUMB_LOAD_TASKS; i++) {
        if (!g_thumb_tasks[i].in_use) {
            g_thumb_tasks[i].in_use = true;
            g_thumb_tasks[i].done = false;
            g_thumb_tasks[i].target = item;
            g_thumb_tasks[i].dest_path[0] = '\0';
            g_thumb_tasks[i].url[0] = '\0';
            g_thumb_tasks[i].decoded_pixels = NULL;
            g_thumb_tasks[i].img_width = 0;
            g_thumb_tasks[i].img_height = 0;
            
            snprintf(g_thumb_tasks[i].dest_path, sizeof(g_thumb_tasks[i].dest_path), "%.*s", (int)(sizeof(g_thumb_tasks[i].dest_path) - 1), item->local_thumb_path);
            
            if (download) {
                char encoded_thumb[512];
                url_encode_path(item->thumbnail_path, encoded_thumb, sizeof(encoded_thumb));
                snprintf(g_thumb_tasks[i].url, sizeof(g_thumb_tasks[i].url), "https://raw.githubusercontent.com/Teero888/tw-thumbs/refs/heads/master/%s", encoded_thumb);
            }
            
            item->thumb_fetching = true;
            pthread_create(&g_thumb_threads[i], NULL, thumb_load_worker, &g_thumb_tasks[i]);
            break;
        }
    }
}

static void trigger_map_download(online_map_item_t *item) {
    if (g_map_task.in_use || item->map_downloading) return;
    
    g_map_task.in_use = true;
    g_map_task.done = false;
    g_map_task.target = item;
    g_map_task.dest_path[0] = '\0';
    
    snprintf(g_map_task.dest_path, sizeof(g_map_task.dest_path), "%.*s", (int)(sizeof(g_map_task.dest_path) - 1), item->local_map_path);
    
    char encoded_map[512];
    url_encode_path(item->map_path, encoded_map, sizeof(encoded_map));
    
    const char *repo_base = "https://raw.githubusercontent.com/ddnet/ddnet-maps/master/";
    if (strcmp(item->repo, "unique") == 0) {
        repo_base = "https://raw.githubusercontent.com/unique-clan/unique-maps/master/";
    } else if (strcmp(item->repo, "kog") == 0) {
        repo_base = "https://raw.githubusercontent.com/Gamer12120/KoGmaps/master/";
    }
    
    snprintf(g_map_task.url, sizeof(g_map_task.url), "%s%s", repo_base, encoded_map);
    
    item->map_downloading = true;
    item->map_download_failed = false;
    pthread_create(&g_map_thread, NULL, map_download_worker, &g_map_task);
}

static void draw_spinning_icon(ImVec2 center, const char* icon_text) {
    ImVec2 text_size = igCalcTextSize(icon_text, NULL, false, -1.0f);
    ImVec2 top_left = {center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f};

    ImDrawList* draw_list = igGetWindowDrawList();
    int vtx_start = draw_list->VtxBuffer.Size;
    ImDrawList_AddText_Vec2(draw_list, top_left, 0xFFFFFFFF, icon_text, NULL);
    int vtx_end = draw_list->VtxBuffer.Size;
    
    float angle = (float)igGetTime() * 10.0f;
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    
    for (int j = vtx_start; j < vtx_end; ++j) {
        ImDrawVert* v = &draw_list->VtxBuffer.Data[j];
        float dx = v->pos.x - center.x;
        float dy = v->pos.y - center.y;
        v->pos.x = center.x + (dx * cos_a - dy * sin_a);
        v->pos.y = center.y + (dx * sin_a + dy * cos_a);
    }
}

static int g_current_sort_field = 0;
static bool g_current_sort_descending = false;

static int cmp_items_ptr(const void *a, const void *b) {
    const online_map_item_t *item_a = *(const online_map_item_t **)a;
    const online_map_item_t *item_b = *(const online_map_item_t **)b;
    
    int res = 0;
    switch (g_current_sort_field) {
        case 1: // Points
            if (item_a->points != item_b->points) {
                res = item_a->points - item_b->points;
            } else {
                res = strcasecmp(item_a->name, item_b->name);
            }
            break;
        case 2: { // Stars
            int stars_a = item_a->stars > 0 ? item_a->stars : item_a->difficulty;
            int stars_b = item_b->stars > 0 ? item_b->stars : item_b->difficulty;
            if (stars_a != stars_b) {
                res = stars_a - stars_b;
            } else {
                res = strcasecmp(item_a->name, item_b->name);
            }
            break;
        }
        case 0: // Name
        default:
            res = strcasecmp(item_a->name, item_b->name);
            break;
    }
    
    return g_current_sort_descending ? -res : res;
}

// Helper to draw custom tab button displaying ONLY the PNG icon (preserving 2:1 aspect ratio)
static bool render_custom_tab_icon_only(int tab_index, int active_tab, struct ImTextureRef_c *icon_tex, const char *fallback_text, float btn_w) {
    igPushID_Int(tab_index);
    bool is_active = (active_tab == tab_index);
    
    ImU32 bg_col = is_active ? IM_COL32(32, 52, 85, 245) : IM_COL32(20, 24, 34, 180);
    ImU32 border_col = is_active ? IM_COL32(90, 175, 255, 255) : IM_COL32(50, 60, 80, 150);
    
    igPushStyleColor_Vec4(ImGuiCol_Button, is_active ? (ImVec4){0.13f, 0.20f, 0.32f, 1.0f} : (ImVec4){0.08f, 0.10f, 0.14f, 0.7f});
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){0.18f, 0.28f, 0.44f, 1.0f});
    igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 6.0f);
    igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, is_active ? 1.5f : 1.0f);
    
    ImVec2 cursor_pos = igGetCursorScreenPos();
    
    bool clicked = igButton("##tab_btn", (ImVec2){btn_w, 40.0f});
    
    ImDrawList *draw_list = igGetWindowDrawList();
    ImVec2 p_min = cursor_pos;
    ImVec2 p_max = {cursor_pos.x + btn_w, cursor_pos.y + 40.0f};
    
    ImDrawList_AddRectFilled(draw_list, p_min, p_max, bg_col, 6.0f, ImDrawFlags_None);
    ImDrawList_AddRect(draw_list, p_min, p_max, border_col, 6.0f, ImDrawFlags_None, is_active ? 1.5f : 1.0f);
    
    // Preserve 2:1 PNG aspect ratio (128x64 -> 60x30)
    float icon_h = 30.0f;
    float icon_w = icon_h * 2.0f; // 60.0f
    
    if (icon_w > btn_w - 12.0f) {
        icon_w = btn_w - 12.0f;
        icon_h = icon_w * 0.5f;
    }
    
    float start_x = p_min.x + (btn_w - icon_w) * 0.5f;
    float start_y = p_min.y + (40.0f - icon_h) * 0.5f;
    
    if (icon_tex) {
        ImDrawList_AddImage(draw_list, *icon_tex, (ImVec2){start_x, start_y}, (ImVec2){start_x + icon_w, start_y + icon_h}, (ImVec2){0, 0}, (ImVec2){1, 1}, 0xFFFFFFFF);
    } else {
        ImVec2 txt_sz = igCalcTextSize(fallback_text, NULL, false, -1.0f);
        ImVec2 txt_pos = {p_min.x + (btn_w - txt_sz.x) * 0.5f, p_min.y + (40.0f - txt_sz.y) * 0.5f};
        ImU32 txt_col = is_active ? IM_COL32(240, 245, 255, 255) : IM_COL32(160, 175, 200, 255);
        ImDrawList_AddText_Vec2(draw_list, txt_pos, txt_col, fallback_text, NULL);
    }
    
    igPopStyleVar(2);
    igPopStyleColor(2);
    igPopID();
    
    return clicked;
}

bool render_online_map_browser(ui_handler_t *ui, online_map_manager_t *mgr, float avail_width, float avail_height) {
    (void)avail_height;
    if (!mgr->initialized) {
        online_map_manager_init(mgr);
    }
    
    online_map_manager_update(mgr, ui->gfx_handler);
    
    bool map_loaded = false;
    
    // Top Bar: Tab Icons Selector (ONLY PNG Icons preserving 2:1 aspect ratio)
    igBeginGroup();
    
    float tab_w = (avail_width - 24.0f) / 3.0f;
    if (tab_w < 120.0f) tab_w = 120.0f;
    
    if (render_custom_tab_icon_only(0, mgr->active_tab, mgr->icon_ddnet_tex, "DDNet", tab_w)) {
        mgr->active_tab = 0;
    }
    igSameLine(0, 12.0f);
    if (render_custom_tab_icon_only(1, mgr->active_tab, mgr->icon_kog_tex, "KoG", tab_w)) {
        mgr->active_tab = 1;
    }
    igSameLine(0, 12.0f);
    if (render_custom_tab_icon_only(2, mgr->active_tab, mgr->icon_unique_tex, "Unique", tab_w)) {
        mgr->active_tab = 2;
    }
    
    igSpacing();
    
    // Controls Row 1: Search Bar & Downloaded Only Checkbox
    float search_w = avail_width - 165.0f;
    if (search_w < 150.0f) search_w = avail_width;
    
    igSetNextItemWidth(search_w);
    igInputTextWithHint("##map_search", ICON_FA_MAGNIFYING_GLASS " Search maps by name...", mgr->search_filter, sizeof(mgr->search_filter), 0, NULL, NULL);
    
    igSameLine(0, 12.0f);
    igCheckbox("Downloaded Only", &mgr->downloaded_only);
    
    igSpacing();
    
    // Direction Toggle Button (Left of Sort Combo, no tooltip)
    const char *dir_icon = mgr->sort_descending ? ICON_FA_ARROW_DOWN_SHORT_WIDE : ICON_FA_ARROW_UP_SHORT_WIDE;
    if (igButton(dir_icon, (ImVec2){32.0f, 0})) {
        mgr->sort_descending = !mgr->sort_descending;
    }
    igSameLine(0, 4.0f);
    
    // Sort Field Combo
    const char *sort_fields[] = { "Name", "Points", "Stars" };
    igPushItemWidth(110.0f);
    igCombo_Str_arr("Sort", &mgr->sort_field, sort_fields, 3, 3);
    igPopItemWidth();
    
    igSameLine(0, 12.0f);
    igPushItemWidth(140.0f);
    
    // Category specific filters (using dynamically parsed types from maps.json!)
    if (mgr->active_tab == 0) { // DDNet
        const char *ddnet_type_ptrs[65];
        ddnet_type_ptrs[0] = "All Types";
        int type_count = mgr->ddnet_type_count;
        if (type_count > 64) type_count = 64;
        
        int type_idx = 0;
        for (int i = 0; i < type_count; i++) {
            ddnet_type_ptrs[i + 1] = mgr->ddnet_types[i].name;
            if (strcmp(mgr->filter_ddnet_type, mgr->ddnet_types[i].name) == 0) {
                type_idx = i + 1;
            }
        }
        
        if (igCombo_Str_arr("Type", &type_idx, ddnet_type_ptrs, type_count + 1, 12)) {
            if (type_idx == 0) {
                strncpy(mgr->filter_ddnet_type, "All", sizeof(mgr->filter_ddnet_type) - 1);
            } else {
                strncpy(mgr->filter_ddnet_type, ddnet_type_ptrs[type_idx], sizeof(mgr->filter_ddnet_type) - 1);
            }
        }
        igSameLine(0, 12.0f);
        
        const char *diff_names[] = {"All Diff", "1 Star", "2 Stars", "3 Stars", "4 Stars", "5 Stars"};
        igCombo_Str_arr("Diff", &mgr->filter_ddnet_difficulty, diff_names, 6, 6);
    } else if (mgr->active_tab == 1) { // KoG
        const char *kog_diff_ptrs[65];
        kog_diff_ptrs[0] = "All Diffs";
        int kcount = mgr->kog_type_count;
        if (kcount > 64) kcount = 64;
        
        int kdiff_idx = 0;
        for (int i = 0; i < kcount; i++) {
            kog_diff_ptrs[i + 1] = mgr->kog_types[i].name;
            if (strcmp(mgr->filter_kog_difficulty, mgr->kog_types[i].name) == 0) {
                kdiff_idx = i + 1;
            }
        }
        
        if (igCombo_Str_arr("Diff", &kdiff_idx, kog_diff_ptrs, kcount + 1, 10)) {
            if (kdiff_idx == 0) {
                strncpy(mgr->filter_kog_difficulty, "All", sizeof(mgr->filter_kog_difficulty) - 1);
            } else {
                strncpy(mgr->filter_kog_difficulty, kog_diff_ptrs[kdiff_idx], sizeof(mgr->filter_kog_difficulty) - 1);
            }
        }
        igSameLine(0, 12.0f);
        
        const char *stars_names[] = {"All Stars", "1 Star", "2 Stars", "3 Stars", "4 Stars", "5 Stars"};
        igCombo_Str_arr("Stars", &mgr->filter_kog_stars, stars_names, 6, 6);
        igSameLine(0, 12.0f);
        
        const char *kog_len_ptrs[33];
        kog_len_ptrs[0] = "All Lengths";
        int len_count = mgr->kog_length_count;
        if (len_count > 32) len_count = 32;
        
        int len_idx = 0;
        if (len_count > 0) {
            for (int i = 0; i < len_count; i++) {
                kog_len_ptrs[i + 1] = mgr->kog_lengths[i].name;
                if (strcmp(mgr->filter_kog_length, mgr->kog_lengths[i].name) == 0) {
                    len_idx = i + 1;
                }
            }
            if (igCombo_Str_arr("Length", &len_idx, kog_len_ptrs, len_count + 1, 10)) {
                if (len_idx == 0) {
                    strncpy(mgr->filter_kog_length, "All", sizeof(mgr->filter_kog_length) - 1);
                } else {
                    strncpy(mgr->filter_kog_length, kog_len_ptrs[len_idx], sizeof(mgr->filter_kog_length) - 1);
                }
            }
        } else {
            const char *fallback_len_names[] = {"All Lengths", "-", "XXS", "XS", "S", "M", "L", "XL", "XXL", "XXXL", "WTF"};
            for (int i = 0; i < 11; i++) {
                if (strcmp(mgr->filter_kog_length, fallback_len_names[i]) == 0) { len_idx = i; break; }
            }
            if (igCombo_Str_arr("Length", &len_idx, fallback_len_names, 11, 11)) {
                if (len_idx == 0) {
                    strncpy(mgr->filter_kog_length, "All", sizeof(mgr->filter_kog_length) - 1);
                } else {
                    strncpy(mgr->filter_kog_length, fallback_len_names[len_idx], sizeof(mgr->filter_kog_length) - 1);
                }
            }
        }
    } else if (mgr->active_tab == 2) { // Unique
        const char *unique_cat_ptrs[65];
        unique_cat_ptrs[0] = "All Categories";
        int ucount = mgr->unique_type_count;
        if (ucount > 64) ucount = 64;
        
        int ucat_idx = 0;
        for (int i = 0; i < ucount; i++) {
            unique_cat_ptrs[i + 1] = mgr->unique_types[i].name;
            if (strcmp(mgr->filter_unique_category, mgr->unique_types[i].name) == 0) {
                ucat_idx = i + 1;
            }
        }
        
        if (igCombo_Str_arr("Category", &ucat_idx, unique_cat_ptrs, ucount + 1, 10)) {
            if (ucat_idx == 0) {
                strncpy(mgr->filter_unique_category, "All", sizeof(mgr->filter_unique_category) - 1);
            } else {
                strncpy(mgr->filter_unique_category, unique_cat_ptrs[ucat_idx], sizeof(mgr->filter_unique_category) - 1);
            }
        }
    }
    
    if (mgr->is_offline) {
        igSameLine(0, 12.0f);
        igTextColored((ImVec4){1.0f, 0.7f, 0.3f, 1.0f}, "%s Offline Mode", ICON_FA_PLUG);
    }
    
    igPopItemWidth();
    igEndGroup();
    igSpacing();
    
    online_map_category_t *cat = get_active_category(mgr);
    
    if (!mgr->json_loaded && mgr->json_fetching) {
        igTextDisabled("Loading online map list...");
        return false;
    }
    
    if (mgr->json_error && cat->count == 0) {
        igTextColored((ImVec4){1.0f, 0.4f, 0.4f, 1.0f}, "%s", mgr->error_msg);
        return false;
    }
    
    // Filter & Sort items
    int filtered_count = 0;
    static online_map_item_t **filtered_items = NULL;
    static int filtered_capacity = 0;
    
    if (cat->count > filtered_capacity) {
        filtered_capacity = cat->count + 256;
        filtered_items = realloc(filtered_items, filtered_capacity * sizeof(online_map_item_t*));
    }
    
    for (int i = 0; i < cat->count; i++) {
        online_map_item_t *item = &cat->items[i];
        
        // Search filter
        if (!contains_case_insensitive(item->name, mgr->search_filter)) continue;
        
        // Downloaded Only filter or Offline Mode
        if (mgr->downloaded_only || mgr->is_offline) {
            if (!item->map_downloaded) continue;
        }
        
        // Category specific filters
        if (mgr->active_tab == 0) { // DDNet
            if (strcmp(mgr->filter_ddnet_type, "All") != 0 && strcmp(mgr->filter_ddnet_type, "All Types") != 0) {
                if (strcasecmp(item->type, mgr->filter_ddnet_type) != 0) continue;
            }
            if (mgr->filter_ddnet_difficulty > 0) {
                if (item->difficulty != mgr->filter_ddnet_difficulty) continue;
            }
        } else if (mgr->active_tab == 1) { // KoG
            if (strcmp(mgr->filter_kog_difficulty, "All") != 0 && strcmp(mgr->filter_kog_difficulty, "All Diffs") != 0) {
                if (strcasecmp(item->kog_difficulty, mgr->filter_kog_difficulty) != 0) continue;
            }
            if (mgr->filter_kog_stars > 0) {
                if (item->stars != mgr->filter_kog_stars) continue;
            }
            if (strcmp(mgr->filter_kog_length, "All") != 0 && strcmp(mgr->filter_kog_length, "All Lengths") != 0) {
                if (strcasecmp(item->length, mgr->filter_kog_length) != 0) continue;
            }
        } else if (mgr->active_tab == 2) { // Unique
            if (strcmp(mgr->filter_unique_category, "All") != 0 && strcmp(mgr->filter_unique_category, "All Categories") != 0) {
                if (strcasecmp(item->category, mgr->filter_unique_category) != 0) continue;
            }
        }
        
        filtered_items[filtered_count++] = item;
    }
    
    // Apply sorting
    if (filtered_count > 1) {
        g_current_sort_field = mgr->sort_field;
        g_current_sort_descending = mgr->sort_descending;
        qsort(filtered_items, filtered_count, sizeof(online_map_item_t*), cmp_items_ptr);
    }
    
    // Sub-header displaying total map count below controls
    if (filtered_count == cat->count) {
        igTextDisabled(ICON_FA_MAP " %d maps available", cat->count);
    } else {
        igTextDisabled(ICON_FA_FILTER " Showing %d of %d maps", filtered_count, cat->count);
    }
    
    ImVec2 content_avail = igGetContentRegionAvail();
    
    // Dedicated scrollable region for ONLY the map grid / images
    if (igBeginChild_Str("MapImagesScrollRegion", (ImVec2){0, content_avail.y}, false, 0)) {
        ImVec2 grid_avail = igGetContentRegionAvail();
        
        float card_width = 195.0f;
        float card_margin = 5.0f; // Equal horizontal and vertical margin
        int columns = (int)(grid_avail.x / (card_width + card_margin * 2.0f));
        if (columns < 1) columns = 1;
        
        igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, (ImVec2){card_margin, card_margin});
        if (igBeginTable("OnlineMapGrid", columns, ImGuiTableFlags_SizingStretchSame, (ImVec2){0, 0}, 0)) {
            int rows = (filtered_count + columns - 1) / columns;
            
            ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
            ImGuiListClipper_Begin(clipper, rows, -1.0f);
            
            while (ImGuiListClipper_Step(clipper)) {
                for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; row++) {
                    igTableNextRow(0, 0);
                    for (int col = 0; col < columns; col++) {
                        int idx = row * columns + col;
                        if (idx >= filtered_count) break;
                        
                        igTableNextColumn();
                        online_map_item_t *item = filtered_items[idx];
                        item->visible_this_frame = true;
                        
                        // Check local thumbnail file existence and load texture if needed
                        if (!item->thumb_loaded && !item->thumb_fetching && !item->thumb_failed) {
                            FILE *tf = fs_open(item->local_thumb_path, "rb");
                            if (tf) {
                                fclose(tf);
                                trigger_thumbnail_load(item, false); // load local file async
                            } else {
                                trigger_thumbnail_load(item, true);  // download and load async
                            }
                        }
                        
                        igPushID_Int(idx);
                        
                        ImVec2 cursor_pos = igGetCursorScreenPos();
                        
                        float actual_card_w = igGetColumnWidth(-1) - 4.0f;
                        if (actual_card_w < 110.0f) actual_card_w = card_width;
                        float actual_thumb_h = actual_card_w * (9.0f / 16.0f);
                        float total_item_h = actual_thumb_h + 46.0f;
                        
                        ImVec2 card_min = cursor_pos;
                        ImVec2 card_max = {cursor_pos.x + actual_card_w, cursor_pos.y + total_item_h};
                        ImDrawList *draw_list = igGetWindowDrawList();
                        
                        // Invisible button for click handling
                        bool clicked = igInvisibleButton("##map_card", (ImVec2){actual_card_w, total_item_h}, 0);
                        bool hovered = igIsItemHovered(0);
                        
                        // Draw Card background
                        ImU32 bg_color = hovered ? IM_COL32(35, 46, 68, 245) : IM_COL32(22, 26, 36, 230);
                        ImU32 border_color = hovered ? IM_COL32(90, 175, 255, 255) : IM_COL32(48, 56, 75, 140);
                        float border_thick = hovered ? 1.8f : 1.0f;
                        
                        ImDrawList_AddRectFilled(draw_list, card_min, card_max, bg_color, 8.0f, ImDrawFlags_None);
                        
                        // Draw Thumbnail image or placeholder
                        ImVec2 thumb_min = card_min;
                        ImVec2 thumb_max = {card_min.x + actual_card_w, card_min.y + actual_thumb_h};
                        
                        if (item->thumb_loaded && item->thumb_preview_texture) {
                            ImDrawList_AddImageRounded(draw_list, *item->thumb_preview_texture, thumb_min, thumb_max, (ImVec2){0, 0}, (ImVec2){1, 1}, 0xFFFFFFFF, 8.0f, ImDrawFlags_RoundCornersTop);
                        } else {
                            // Placeholder background
                            ImDrawList_AddRectFilled(draw_list, thumb_min, thumb_max, IM_COL32(18, 22, 30, 240), 8.0f, ImDrawFlags_RoundCornersTop);
                            
                            ImVec2 center = {thumb_min.x + actual_card_w * 0.5f, thumb_min.y + actual_thumb_h * 0.5f};
                            if (item->thumb_fetching) {
                                draw_spinning_icon(center, ICON_FA_ROTATE);
                            } else {
                                const char *status_txt = "No Image";
                                ImVec2 txt_sz = igCalcTextSize(status_txt, NULL, false, -1.0f);
                                ImVec2 txt_pos = {center.x - txt_sz.x * 0.5f, center.y - txt_sz.y * 0.5f};
                                ImDrawList_AddText_Vec2(draw_list, txt_pos, IM_COL32(140, 150, 170, 255), status_txt, NULL);
                            }
                        }
                        
                        // Draw Map Name
                        ImVec2 name_pos = {card_min.x + 8.0f, card_min.y + actual_thumb_h + 6.0f};
                        ImDrawList_AddText_Vec2(draw_list, name_pos, IM_COL32(235, 240, 250, 255), item->name, NULL);
                        
                        // Draw Metadata Row (using FontAwesome ICON_FA_STAR instead of broken UTF8 unicode)
                        char meta_str[128] = {0};
                        if (strcmp(item->repo, "ddnet") == 0) {
                            char stars_buf[32] = {0};
                            if (item->difficulty > 0) {
                                int s_count = item->difficulty > 5 ? 5 : item->difficulty;
                                for (int s = 0; s < s_count; s++) strcat(stars_buf, ICON_FA_STAR);
                            }
                            if (item->type[0] && item->points > 0) {
                                snprintf(meta_str, sizeof(meta_str), "%s %s • %d pts", item->type, stars_buf, item->points);
                            } else if (item->type[0]) {
                                snprintf(meta_str, sizeof(meta_str), "%s %s", item->type, stars_buf);
                            } else if (item->points > 0) {
                                snprintf(meta_str, sizeof(meta_str), "%d pts", item->points);
                            }
                        } else if (strcmp(item->repo, "unique") == 0) {
                            if (item->category[0]) {
                                snprintf(meta_str, sizeof(meta_str), "%s", item->category);
                            }
                        } else if (strcmp(item->repo, "kog") == 0) {
                            char stars_buf[32] = {0};
                            if (item->stars > 0) {
                                int s_count = item->stars > 5 ? 5 : item->stars;
                                for (int s = 0; s < s_count; s++) strcat(stars_buf, ICON_FA_STAR);
                            }
                            if (item->length[0]) {
                                snprintf(meta_str, sizeof(meta_str), "%s %s • %d pts • %s", item->kog_difficulty[0] ? item->kog_difficulty : "KoG", stars_buf, item->points, item->length);
                            } else {
                                snprintf(meta_str, sizeof(meta_str), "%s %s • %d pts", item->kog_difficulty[0] ? item->kog_difficulty : "KoG", stars_buf, item->points);
                            }
                        }
                        
                        if (meta_str[0]) {
                            ImVec2 meta_pos = {card_min.x + 8.0f, card_min.y + actual_thumb_h + 24.0f};
                            ImDrawList_AddText_Vec2(draw_list, meta_pos, IM_COL32(140, 160, 195, 255), meta_str, NULL);
                        }
                        
                        // Overlay badge if downloading or completed check
                        if (item->map_downloading) {
                            ImDrawList_AddRectFilled(draw_list, thumb_min, thumb_max, IM_COL32(10, 14, 22, 210), 8.0f, ImDrawFlags_RoundCornersTop);
                            ImVec2 center = {thumb_min.x + actual_card_w * 0.5f, thumb_min.y + actual_thumb_h * 0.5f};
                            draw_spinning_icon(center, ICON_FA_ROTATE);
                        }

                        // Draw Card Border Outline ON TOP of thumbnail image & contents
                        ImDrawList_AddRect(draw_list, card_min, card_max, border_color, 8.0f, ImDrawFlags_None, border_thick);
                        
                        // Handle Click on Map Card
                        if (clicked && !item->map_downloading) {
                            FILE *mf = fs_open(item->local_map_path, "rb");
                            if (mf) {
                                fclose(mf);
                                on_map_load_path(ui->gfx_handler, item->local_map_path);
                                map_loaded = true;
                            } else {
                                trigger_map_download(item);
                            }
                        }
                        
                        // If map download just finished in background, load it automatically!
                        if (g_map_task.done && item == g_map_task.target) {
                            FILE *check_mf = fs_open(item->local_map_path, "rb");
                            if (check_mf && !item->map_downloading && item->map_download_failed == false) {
                                fseek(check_mf, 0, SEEK_END);
                                long fsize = ftell(check_mf);
                                fclose(check_mf);
                                if (fsize > 0) {
                                    on_map_load_path(ui->gfx_handler, item->local_map_path);
                                    map_loaded = true;
                                    g_map_task.target = NULL;
                                }
                            }
                        }
                        
                        igPopID();
                    }
                }
            }
            
            ImGuiListClipper_End(clipper);
            ImGuiListClipper_destroy(clipper);
            igEndTable();
        }
        igPopStyleVar(1);
    }
    igEndChild();
    
    return map_loaded;
}
