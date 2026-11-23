#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern FILE *__real_fopen(const char *path, const char *mode);
extern int __real_open(const char *path, int flags, ...);
extern int __real_stat(const char *path, struct stat *st);
extern int __real_mkdir(const char *path, mode_t mode);
extern char *__real_getenv(const char *name);

static char *resolve_wolf_path_alloc(const char *in) {
    if (!in) return NULL;

    const char *needle = "/.chocolate_wolfenstein_3d/";
    const char *hit = strstr(in, needle);
    if (hit) {
        in = hit + strlen(needle);
    } else if (strcmp(in, "/data/wolf3d/.chocolate_wolfenstein_3d") == 0) {
        size_t sz = strlen("/data/wolf3d") + 1u;
        char *out = (char *)malloc(sz);
        if (out) memcpy(out, "/data/wolf3d", sz);
        return out;
    }

    if (in[0] == '/') {
        size_t sz = strlen(in) + 1u;
        char *out = (char *)malloc(sz);
        if (out) memcpy(out, in, sz);
        return out;
    }

    const char *rel = in;
    if (rel[0] == '.' && rel[1] == '/') rel += 2;

    size_t rlen = strlen(rel);
    const char *prefix = "/data/wolf3d/";
    size_t plen = strlen(prefix);
    if (rlen > ((size_t)-1) - plen - 1u) return NULL;
    char *out = (char *)malloc(plen + rlen + 1u);
    if (!out) return NULL;
    memcpy(out, prefix, plen);
    for (size_t i = 0; i < rlen; i++) {
        out[plen + i] = (char)tolower((unsigned char)rel[i]);
    }
    out[plen + rlen] = '\0';
    return out;
}

char *__wrap_getenv(const char *name) {
    if (name && strcmp(name, "HOME") == 0) {
        return "/data/wolf3d";
    }
    return __real_getenv(name);
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    if (!path || !mode) return __real_fopen(path, mode);
    char *resolved = resolve_wolf_path_alloc(path);
    FILE *f = resolved ? __real_fopen(resolved, mode) : NULL;
    free(resolved);
    if (f) return f;
    return __real_fopen(path, mode);
}

int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    if (!path) return __real_open(path, flags, mode);
    char *resolved = resolve_wolf_path_alloc(path);
    int fd = resolved ? __real_open(resolved, flags, mode) : -1;
    free(resolved);
    if (fd >= 0) return fd;
    return __real_open(path, flags, mode);
}

int __wrap_stat(const char *path, struct stat *st) {
    if (!path || !st) return __real_stat(path, st);
    char *resolved = resolve_wolf_path_alloc(path);
    int rc = resolved ? __real_stat(resolved, st) : -1;
    free(resolved);
    if (rc == 0) return 0;
    return __real_stat(path, st);
}

int __wrap_mkdir(const char *path, mode_t mode) {
    if (!path) return -1;
    char *resolved = resolve_wolf_path_alloc(path);
    if (!resolved) return -1;
    int rc = __real_mkdir(resolved, mode);
    free(resolved);
    return rc;
}
