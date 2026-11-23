#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

FILE *__real_fopen(const char *path, const char *mode);

static char *tetris_resolve_asset_alloc(const char *path) {
    if (!path) return NULL;
    size_t plen = strlen(path);
    if (path[0] == '/') {
        char *out = (char *)malloc(plen + 1u);
        if (out) memcpy(out, path, plen + 1u);
        return out;
    }
    const char *pfx = (strncmp(path, "src/", 4) == 0) ? "/data/tetris/" : "/data/tetris/src/";
    size_t pfx_len = strlen(pfx);
    char *out = (char *)malloc(pfx_len + plen + 1u);
    if (!out) return NULL;
    memcpy(out, pfx, pfx_len);
    memcpy(out + pfx_len, path, plen + 1u);
    return out;
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    if (!path) return NULL;
    if (path[0] == '/') {
        return __real_fopen(path, mode);
    }
    char *mapped = tetris_resolve_asset_alloc(path);
    FILE *f = mapped ? __real_fopen(mapped, mode) : NULL;
    free(mapped);
    if (f) return f;
    return __real_fopen(path, mode);
}

typedef struct {
    bool used;
    bool exists;
    char *key;
    char *data;
    size_t len;
} FsTxtCacheEntry;

static FsTxtCacheEntry *s_txt_cache = NULL;
static int s_txt_cache_cap = 0;

bool tetris_fs_read_cached(const char *path, const char **out_buf, size_t *out_len) {
    if (!path || !out_buf || !out_len) return false;
    *out_buf = NULL;
    *out_len = 0;

    char *resolved = tetris_resolve_asset_alloc(path);
    if (!resolved) return false;

    for (int i = 0; i < s_txt_cache_cap; i++) {
        if (s_txt_cache[i].used && s_txt_cache[i].key && strcmp(s_txt_cache[i].key, resolved) == 0) {
            free(resolved);
            if (!s_txt_cache[i].exists) return false;
            *out_buf = s_txt_cache[i].data;
            *out_len = s_txt_cache[i].len;
            return true;
        }
    }

    int slot = -1;
    for (int i = 0; i < s_txt_cache_cap; i++) {
        if (!s_txt_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_txt_cache_cap ? (s_txt_cache_cap * 2) : 16;
        if (new_cap <= s_txt_cache_cap || (size_t)new_cap > SIZE_MAX / sizeof(FsTxtCacheEntry)) {
            free(resolved);
            return false;
        }
        FsTxtCacheEntry *nc = (FsTxtCacheEntry *)realloc(s_txt_cache, (size_t)new_cap * sizeof(FsTxtCacheEntry));
        if (!nc) { free(resolved); return false; }
        memset(nc + s_txt_cache_cap, 0, (size_t)(new_cap - s_txt_cache_cap) * sizeof(FsTxtCacheEntry));
        slot = s_txt_cache_cap;
        s_txt_cache = nc;
        s_txt_cache_cap = new_cap;
    }

    FILE *f = __wrap_fopen(path, "rb");
    s_txt_cache[slot].used = true;
    s_txt_cache[slot].key = resolved;
    if (!f) {
        s_txt_cache[slot].exists = false;
        s_txt_cache[slot].data = NULL;
        s_txt_cache[slot].len = 0;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        free(s_txt_cache[slot].key);
        s_txt_cache[slot].key = NULL;
        s_txt_cache[slot].used = false;
        return false;
    }
    size_t nr = sz > 0 ? fread(buf, 1u, (size_t)sz, f) : 0;
    fclose(f);
    buf[nr] = '\0';

    s_txt_cache[slot].exists = true;
    s_txt_cache[slot].data = buf;
    s_txt_cache[slot].len = nr;
    *out_buf = buf;
    *out_len = nr;
    return true;
}

void tetris_fs_invalidate(const char *path) {
    if (!path) return;
    char *resolved = tetris_resolve_asset_alloc(path);
    if (!resolved) return;
    for (int i = 0; i < s_txt_cache_cap; i++) {
        if (s_txt_cache[i].used && s_txt_cache[i].key && strcmp(s_txt_cache[i].key, resolved) == 0) {
            free(s_txt_cache[i].data);
            free(s_txt_cache[i].key);
            s_txt_cache[i].data = NULL;
            s_txt_cache[i].key = NULL;
            s_txt_cache[i].used = false;
        }
    }
    free(resolved);
}
