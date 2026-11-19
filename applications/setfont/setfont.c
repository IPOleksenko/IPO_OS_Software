#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static char *query_current_font_name(uint32_t *out_count) {
    size_t cap = 64u;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    while (cap <= 65536u) {
        buf[0] = '\0';
        uint32_t cnt = 0u;
        ipo_font_get_info(buf, (uint32_t)cap, &cnt);
        if (out_count) *out_count = cnt;
        if (strlen(buf) + 1u < cap) {
            return buf;
        }
        size_t next_cap = cap * 2u;
        char *nb = (char *)realloc(buf, next_cap);
        if (!nb) break;
        buf = nb;
        cap = next_cap;
    }
    return buf;
}

static char *alloc_font_path(const char *fmt, const char *target) {
    int n = snprintf(NULL, 0, fmt, target);
    if (n < 0) return NULL;
    char *p = (char *)malloc((size_t)n + 1u);
    if (!p) return NULL;
    snprintf(p, (size_t)n + 1u, fmt, target);
    return p;
}

static void print_usage(void) {
    printf("Usage: setfont [options] [font_file]\n");
    printf("Manage and switch system text console fonts.\n\n");
    printf("Options / Commands:\n");
    printf("  setfont              Display active font and list available fonts\n");
    printf("  setfont <name/path>  Switch system font (e.g. terminus, bold, default)\n");
    printf("  setfont default      Switch back to default system font\n");
    printf("  setfont -r           Reset system font\n");
    printf("  setfont -h, --help   Show this help message\n\n");
    printf("Known fonts in /fonts:\n");
    printf("  default   - Standard console font (Fixed16 + Hanzi)\n");
    printf("  terminus  - Terminus clean console font\n");
    printf("  bold      - Terminus Bold VGA font\n");
}

int main(int argc, char **argv) {
    uint32_t glyph_count = 0;
    char *current_name = query_current_font_name(&glyph_count);

    if (argc < 2) {
        printf("Current system font: %s (%u glyphs)\n\n",
               (current_name && current_name[0]) ? current_name : "default", glyph_count);
        free(current_name);
        printf("Usage:\n");
        printf("  setfont terminus\n");
        printf("  setfont bold\n");
        printf("  setfont default\n");
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        free(current_name);
        print_usage();
        return 0;
    }

    const char *target = argv[1];
    char *resolved_path = alloc_font_path("/fonts/%s.fnt", target);
    if (!resolved_path) {
        free(current_name);
        return 1;
    }

    printf("Loading system font from: %s...\n", resolved_path);

    int res = ipo_font_load(resolved_path);
    if (res != 0 && target[0] != '/') {
        /* If .fnt failed and didn't have leading /fonts/, try exact path */
        char *fallback_path = alloc_font_path("/fonts/%s", target);
        if (fallback_path) {
            free(resolved_path);
            resolved_path = fallback_path;
            res = ipo_font_load(resolved_path);
        }
    }

    if (res != 0) {
        printf("Error: Failed to load font '%s' (code %d)\n", target, res);
        free(resolved_path);
        free(current_name);
        return 1;
    }

    /* Query updated info */
    glyph_count = 0;
    free(current_name);
    current_name = query_current_font_name(&glyph_count);

    printf("Success! System font updated.\n");
    printf("Active Font: %s (%u glyphs)\n",
           (current_name && current_name[0]) ? current_name : resolved_path, glyph_count);
    printf("Preview: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789\n");
    free(resolved_path);
    free(current_name);
    return 0;
}

