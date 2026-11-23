#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *__real_fopen(const char *path, const char *mode);

/* Rewrite upstream "../Assets/..." or "Assets/..." paths and .ogg -> .wav */
static char *zelda_resolve_asset_path_alloc(const char *in, int map_ogg_to_wav) {
    if (!in) return NULL;

    const char *prefix = "";
    const char *rel = in;
    if (strncmp(rel, "../Assets/", 10) == 0) {
        rel += 10;
        prefix = "/data/zelda/Assets/";
    } else if (strncmp(rel, "Assets/", 7) == 0) {
        rel += 7;
        prefix = "/data/zelda/Assets/";
    } else if (strncmp(rel, "/data/zelda/Assets/", 19) == 0) {
        rel += 19;
        prefix = "/data/zelda/Assets/";
    } else if (strncmp(rel, "data/zelda/Assets/", 18) == 0) {
        rel += 18;
        prefix = "/data/zelda/Assets/";
    }

    size_t plen = strlen(prefix);
    size_t rlen = strlen(rel);
    if (plen > ((size_t)-1) - rlen - 1u) return NULL;
    char *out = (char *)malloc(plen + rlen + 1u);
    if (!out) return NULL;
    if (plen > 0) memcpy(out, prefix, plen);
    memcpy(out + plen, rel, rlen + 1u);

    if (map_ogg_to_wav) {
        size_t len = plen + rlen;
        if (len >= 4 && strcmp(out + len - 4, ".ogg") == 0) {
            memcpy(out + len - 4, ".wav", 4);
        }
    }
    return out;
}

FILE *zelda_open_asset(const char *path, const char *mode, int map_ogg_to_wav) {
    char *buf = zelda_resolve_asset_path_alloc(path, map_ogg_to_wav);
    FILE *f = buf ? __real_fopen(buf, mode) : NULL;
    if (f) {
        free(buf);
        return f;
    }

    /* Host / relative workspace fallback if /data/zelda/Assets/ is absent */
    if (buf && strncmp(buf, "/data/zelda/", 12) == 0) {
        f = __real_fopen(buf + 1, mode);
        if (f) {
            free(buf);
            return f;
        }
    }
    free(buf);
    return __real_fopen(path, mode);
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    if (!path || !mode) return __real_fopen(path, mode);
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
        return __real_fopen(path, mode);
    }
    return zelda_open_asset(path, mode, 0);
}
