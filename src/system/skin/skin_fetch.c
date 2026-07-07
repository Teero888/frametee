#include "skin_fetch.h"
#include <system/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>



// Helper to copy a file
static bool copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fs_open(src_path, "rb");
    if (!src) return false;
    
    FILE *dst = fs_open(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }
    
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, n, dst);
    }
    
    fclose(src);
    fclose(dst);
    return true;
}

// Curl write callback
static size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

bool fetch_skin(const char *skin_name, char *out_path, size_t out_path_size) {
    char config_dir[1024];
    if (!fs_get_config_dir(config_dir, sizeof(config_dir))) {
        return false;
    }
    
    // Create config dir if it doesn't exist
    fs_mkdir(config_dir);
    
    char skins_dir[1024];
    snprintf(skins_dir, sizeof(skins_dir), "%.*s%cskins", (int)(sizeof(skins_dir) - 8), config_dir, PATH_SEP);
    fs_mkdir(skins_dir);
    
    // 1. Check if it already exists
    snprintf(out_path, out_path_size, "%s%c%s.png", skins_dir, PATH_SEP, skin_name);
    FILE *f = fs_open(out_path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    
    // 2. Check fallback locations
    char fallback_path[2048];
    bool found_local = false;
    
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        const char *paths[] = {
            "Teeworlds\\skins",
            "Teeworlds\\downloadedskins",
            "DDNet\\skins",
            "DDNet\\downloadedskins"
        };
        for (int i = 0; i < 4; i++) {
            snprintf(fallback_path, sizeof(fallback_path), "%s\\%s\\%s.png", appdata, paths[i], skin_name);
            f = fs_open(fallback_path, "rb");
            if (f) {
                fclose(f);
                found_local = true;
                break;
            }
        }
    }
#else
    const char *home = getenv("HOME");
    if (home) {
        const char *paths[] = {
            ".teeworlds/skins",
            ".teeworlds/downloadedskins",
            ".local/share/ddnet/skins",
            ".local/share/ddnet/downloadedskins"
        };
        for (int i = 0; i < 4; i++) {
            snprintf(fallback_path, sizeof(fallback_path), "%s/%s/%s.png", home, paths[i], skin_name);
            f = fs_open(fallback_path, "rb");
            if (f) {
                fclose(f);
                found_local = true;
                break;
            }
        }
    }
#endif

    // 3. If found locally, copy it
    if (found_local) {
        if (copy_file(fallback_path, out_path)) {
            return true;
        }
    }
    
    // 4. Download it using libcurl
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    
    char *encoded_name = curl_easy_escape(curl, skin_name, 0);
    if (!encoded_name) {
        curl_easy_cleanup(curl);
        return false;
    }
    
    const char *base_urls[] = {
        "https://skins.ddstats.tw",
        "https://skins.ddnet.org/skin/community",
        "https://skins.ddnet.org/skin"
    };
    
    bool downloaded = false;
    for (int i = 0; i < 3; i++) {
        char url[2048];
        snprintf(url, sizeof(url), "%s/%s.png", base_urls[i], encoded_name);
        
        FILE *fp = fs_open(out_path, "wb");
        if (!fp) continue;
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        CURLcode res = curl_easy_perform(curl);
        
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        fclose(fp);
        
        if (res == CURLE_OK && http_code == 200) {
            downloaded = true;
            break;
        } else {
            // Remove the empty/failed file
            fs_remove(out_path);
        }
    }
    
    curl_free(encoded_name);
    curl_easy_cleanup(curl);
    
    return downloaded;
}
