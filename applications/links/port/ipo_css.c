#include "ipo_css.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Links' raw attr accessor declared in upstream/html.c */
extern unsigned char *get_attr_val(unsigned char *e, unsigned char *name);

typedef enum {
    COMB_NONE = 0,
    COMB_DESC,
    COMB_CHILD
} css_comb_t;

typedef struct {
    char *tag;
    char *id;
    char **classes;
    size_t n_classes;
    char *attr_name;
    char *attr_val;
    uint8_t attr_op;   /* 0=none, 1=exists, 2=exact, 3=word(~=), 4=contains(*=) */
    css_comb_t comb_to_prev; /* relationship with previous (left) ancestor segment */
} css_seg_t;

typedef struct {
    char *prop;
    char *val;
    uint8_t important;
} css_decl_t;

typedef struct {
    css_seg_t *segs;
    size_t n_segs;
    uint32_t specificity;
    uint32_t order;
    css_decl_t *decls;
    uint16_t n_decls;
} css_rule_t;

static char *css_dup_n(const char *s, size_t len) {
    if (!s) return NULL;
    char *out = (char *)malloc(len + 1u);
    if (!out) return NULL;
    if (len > 0u) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char *css_dup_lower_n(const char *s, size_t len) {
    char *out = css_dup_n(s, len);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)tolower((unsigned char)out[i]);
    }
    return out;
}

static void css_free_segs(css_seg_t *segs, size_t n_segs) {
    if (!segs) return;
    for (size_t i = 0; i < n_segs; i++) {
        free(segs[i].tag);
        free(segs[i].id);
        for (size_t c = 0; c < segs[i].n_classes; c++) {
            free(segs[i].classes[c]);
        }
        free(segs[i].classes);
        free(segs[i].attr_name);
        free(segs[i].attr_val);
    }
    free(segs);
}

typedef struct {
    char *name;
    char *val;
} css_var_t;

struct ipo_css_sheet {
    int viewport_w;
    css_rule_t *rules;
    size_t n_rules;
    size_t cap_rules;
    css_var_t *vars;
    size_t n_vars;
    size_t cap_vars;
    uint32_t next_order;
};

typedef struct {
    const char *name;
    uint32_t rgb;
} css_named_color_t;

/* Sorted alphabetically for bsearch */
static const css_named_color_t k_css_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080},
    {"oldlace", 0xFDF5E6}, {"olive", 0x808000}, {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9},
    {"peru", 0xCD853F}, {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460},
    {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C},
    {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347},
    {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32}
};

static const char *css_strcasestr(const char *hay, const char *needle) {
    size_t nlen;
    if (!hay || !needle || !*needle) return hay;
    nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

static char *css_trim_dup(const char *s, size_t len) {
    while (len > 0 && isspace((unsigned char)*s)) { s++; len--; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char *css_trim_lower_dup(const char *s, size_t len) {
    char *out = css_trim_dup(s, len);
    if (!out) return NULL;
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    return out;
}

static void css_set_var(ipo_css_sheet_t *sheet, const char *name, const char *val) {
    if (!sheet || !name || !val) return;
    for (size_t i = 0; i < sheet->n_vars; i++) {
        if (strcmp(sheet->vars[i].name, name) == 0) {
            char *nv = strdup(val);
            if (nv) { free(sheet->vars[i].val); sheet->vars[i].val = nv; }
            return;
        }
    }
    if (sheet->n_vars >= sheet->cap_vars) {
        size_t nc = sheet->cap_vars ? (sheet->cap_vars * 2u) : 32u;
        css_var_t *nvars = (css_var_t *)realloc(sheet->vars, nc * sizeof(css_var_t));
        if (!nvars) return;
        sheet->vars = nvars;
        sheet->cap_vars = nc;
    }
    char *nn = strdup(name);
    char *nv = strdup(val);
    if (!nn || !nv) { free(nn); free(nv); return; }
    sheet->vars[sheet->n_vars].name = nn;
    sheet->vars[sheet->n_vars].val = nv;
    sheet->n_vars++;
}

static const char *css_find_var(const ipo_css_sheet_t *sheet, const char *name) {
    if (!sheet || !name) return NULL;
    for (size_t i = 0; i < sheet->n_vars; i++) {
        if (strcmp(sheet->vars[i].name, name) == 0) return sheet->vars[i].val;
    }
    return NULL;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} css_sb_t;

static bool css_sb_append_n(css_sb_t *sb, const char *s, size_t n) {
    if (!sb) return false;
    if (!s || n == 0u) {
        if (!sb->data) {
            sb->cap = 32u;
            sb->data = (char *)malloc(sb->cap);
            if (!sb->data) { sb->cap = 0; return false; }
            sb->data[0] = '\0';
        }
        return true;
    }
    if (n > SIZE_MAX - sb->len - 1u) return false;
    size_t need = sb->len + n + 1u;
    if (need > sb->cap) {
        size_t nc = sb->cap ? sb->cap : 64u;
        while (nc < need) {
            if (nc > SIZE_MAX / 2u) { nc = need; break; }
            nc *= 2u;
        }
        char *nb = (char *)realloc(sb->data, nc);
        if (!nb) return false;
        sb->data = nb;
        sb->cap = nc;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

static char *css_resolve_vars_alloc(const ipo_css_sheet_t *sheet, const char *in, int depth) {
    if (!in) return strdup("");
    if (depth > 32 || !strstr(in, "var(")) {
        return strdup(in);
    }
    css_sb_t sb = { NULL, 0, 0 };
    css_sb_append_n(&sb, "", 0u);
    const char *p = in;
    while (*p) {
        if (strncasecmp(p, "var(", 4) == 0) {
            const char *q = p + 4;
            int parens = 1;
            const char *comma = NULL;
            while (*q && parens > 0) {
                if (*q == '(') parens++;
                else if (*q == ')') parens--;
                else if (*q == ',' && parens == 1 && !comma) comma = q;
                if (parens > 0) q++;
            }
            if (parens == 0) {
                const char *vname_end = comma ? comma : q;
                const char *ns = p + 4;
                while (ns < vname_end && isspace((unsigned char)*ns)) ns++;
                size_t nl = (size_t)(vname_end - ns);
                while (nl > 0 && isspace((unsigned char)ns[nl - 1])) nl--;
                char *vname = css_dup_n(ns, nl);
                const char *resolved = vname ? css_find_var(sheet, vname) : NULL;
                char *fb = NULL;
                if (!resolved && comma && q > comma + 1) {
                    const char *fs = comma + 1;
                    while (fs < q && isspace((unsigned char)*fs)) fs++;
                    size_t fl = (size_t)(q - fs);
                    while (fl > 0 && isspace((unsigned char)fs[fl - 1])) fl--;
                    fb = css_dup_n(fs, fl);
                    resolved = fb;
                }
                if (resolved) {
                    char *rec = css_resolve_vars_alloc(sheet, resolved, depth + 1);
                    if (rec) {
                        css_sb_append_n(&sb, rec, strlen(rec));
                        free(rec);
                    }
                }
                free(fb);
                free(vname);
                p = q + 1;
                continue;
            }
        }
        if (!css_sb_append_n(&sb, p, 1u)) break;
        p++;
    }
    return sb.data ? sb.data : strdup("");
}

static uint32_t blend_rgb(uint32_t fg, uint32_t bg, int alpha_255) {
    if (alpha_255 >= 255) return fg & 0xFFFFFFu;
    if (alpha_255 <= 0) return bg & 0xFFFFFFu;
    int fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    int br = (bg >> 16) & 0xFF, bg_ = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int r = (fr * alpha_255 + br * (255 - alpha_255) + 127) / 255;
    int g = (fg_ * alpha_255 + bg_ * (255 - alpha_255) + 127) / 255;
    int b = (fb * alpha_255 + bb * (255 - alpha_255) + 127) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static double parse_num_or_pct(const char *s, const char **endp, double base) {
    char *e = NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    double v = strtod(s, &e);
    if (e && *e == '%') { v = (v * base) / 100.0; e++; }
    if (endp) *endp = e ? e : s;
    return v;
}

static int clamp_byte(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t hsl_to_rgb(double h, double s_pct, double l_pct) {
    double c, x, m, r1 = 0, g1 = 0, b1 = 0;
    while (h < 0) h += 360.0;
    while (h >= 360.0) h -= 360.0;
    if (s_pct < 0) s_pct = 0; if (s_pct > 100) s_pct = 100;
    if (l_pct < 0) l_pct = 0; if (l_pct > 100) l_pct = 100;
    double s = s_pct / 100.0, l = l_pct / 100.0;
    c = (1.0 - (2.0 * l - 1.0 < 0 ? -(2.0 * l - 1.0) : (2.0 * l - 1.0))) * s;
    double hp = h / 60.0;
    int sect = (int)hp;
    double rem = hp - (double)(sect / 2 * 2) - 1.0;
    if (rem < 0) rem = -rem;
    x = c * (1.0 - rem);
    switch (sect % 6) {
        case 0: r1 = c; g1 = x; b1 = 0; break;
        case 1: r1 = x; g1 = c; b1 = 0; break;
        case 2: r1 = 0; g1 = c; b1 = x; break;
        case 3: r1 = 0; g1 = x; b1 = c; break;
        case 4: r1 = x; g1 = 0; b1 = c; break;
        default: r1 = c; g1 = 0; b1 = x; break;
    }
    m = l - c * 0.5;
    int r = clamp_byte((int)((r1 + m) * 255.0 + 0.5));
    int g = clamp_byte((int)((g1 + m) * 255.0 + 0.5));
    int b = clamp_byte((int)((b1 + m) * 255.0 + 0.5));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int ipo_css_parse_color(const ipo_css_sheet_t *sheet, const char *val, uint32_t parent_bg, uint32_t *out_rgb, int *out_transparent) {
    if (out_transparent) *out_transparent = 0;
    if (!val) return 0;
    char *buf = css_resolve_vars_alloc(sheet, val, 0);
    if (!buf) return 0;
    char *s = buf;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t slen = strlen(s);
    while (slen > 0 && isspace((unsigned char)s[slen - 1])) s[--slen] = 0;
    if (!slen) { free(buf); return 0; }

    if (strcasecmp(s, "transparent") == 0) {
        if (out_transparent) *out_transparent = 1;
        *out_rgb = parent_bg & 0xFFFFFFu;
        free(buf);
        return 1;
    }
    if (strcasecmp(s, "none") == 0 || strcasecmp(s, "initial") == 0 ||
        strcasecmp(s, "unset") == 0 || strcasecmp(s, "inherit") == 0 ||
        strcasecmp(s, "currentColor") == 0) {
        free(buf);
        return 0;
    }

    if (strncasecmp(s, "light-dark(", 11) == 0) {
        const char *p = s + 11;
        while (*p && isspace((unsigned char)*p)) p++;
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                if (depth == 0) break;
                depth--;
            } else if (*p == ',' && depth == 0) {
                break;
            }
            p++;
        }
        size_t fl = (size_t)(p - start);
        if (fl > 0) {
            char *first = css_dup_n(start, fl);
            int ok = first ? ipo_css_parse_color(sheet, first, parent_bg, out_rgb, out_transparent) : 0;
            free(first);
            free(buf);
            return ok;
        }
        free(buf);
        return 0;
    }

    if (s[0] == '#') {
        const char *h = s + 1;
        size_t hl = strlen(h);
        if (hl == 3 || hl == 4) {
            int r = hex_val(h[0]), g = hex_val(h[1]), b = hex_val(h[2]);
            int a = (hl == 4) ? hex_val(h[3]) : 15;
            if (r < 0 || g < 0 || b < 0 || a < 0) { free(buf); return 0; }
            uint32_t col = ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            int alpha = a * 17;
            if (alpha < 20) { if (out_transparent) *out_transparent = 1; *out_rgb = parent_bg; free(buf); return 1; }
            *out_rgb = blend_rgb(col, parent_bg, alpha);
            free(buf);
            return 1;
        }
        if (hl == 6 || hl == 8) {
            int digits[8];
            for (size_t i = 0; i < hl; i++) {
                digits[i] = hex_val(h[i]);
                if (digits[i] < 0) { free(buf); return 0; }
            }
            uint32_t col = ((uint32_t)((digits[0] << 4) | digits[1]) << 16) |
                           ((uint32_t)((digits[2] << 4) | digits[3]) << 8) |
                           (uint32_t)((digits[4] << 4) | digits[5]);
            int alpha = (hl == 8) ? ((digits[6] << 4) | digits[7]) : 255;
            if (alpha < 20) { if (out_transparent) *out_transparent = 1; *out_rgb = parent_bg; free(buf); return 1; }
            *out_rgb = blend_rgb(col, parent_bg, alpha);
            free(buf);
            return 1;
        }
        free(buf);
        return 0;
    }

    if (strncasecmp(s, "rgb(", 4) == 0 || strncasecmp(s, "rgba(", 5) == 0) {
        const char *p = strchr(s, '(') + 1;
        double r = parse_num_or_pct(p, &p, 255.0);
        if (*p == ',') p++;
        double g = parse_num_or_pct(p, &p, 255.0);
        if (*p == ',') p++;
        double b = parse_num_or_pct(p, &p, 255.0);
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '/')) p++;
        int alpha = 255;
        if (*p && *p != ')') {
            double av = parse_num_or_pct(p, &p, 1.0);
            if (av < 0.0) av = 0.0; if (av > 1.0) av = 1.0;
            alpha = (int)(av * 255.0 + 0.5);
        }
        if (alpha < 20) { if (out_transparent) *out_transparent = 1; *out_rgb = parent_bg; free(buf); return 1; }
        uint32_t raw = ((uint32_t)clamp_byte((int)(r + 0.5)) << 16) |
                       ((uint32_t)clamp_byte((int)(g + 0.5)) << 8) |
                       (uint32_t)clamp_byte((int)(b + 0.5));
        *out_rgb = blend_rgb(raw, parent_bg, alpha);
        free(buf);
        return 1;
    }

    if (strncasecmp(s, "hsl(", 4) == 0 || strncasecmp(s, "hsla(", 5) == 0) {
        const char *p = strchr(s, '(') + 1;
        double h = parse_num_or_pct(p, &p, 360.0);
        if (strncasecmp(p, "deg", 3) == 0) p += 3;
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        double sat = parse_num_or_pct(p, &p, 100.0);
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        double lig = parse_num_or_pct(p, &p, 100.0);
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '/')) p++;
        int alpha = 255;
        if (*p && *p != ')') {
            double av = parse_num_or_pct(p, &p, 1.0);
            if (av < 0.0) av = 0.0; if (av > 1.0) av = 1.0;
            alpha = (int)(av * 255.0 + 0.5);
        }
        if (alpha < 20) { if (out_transparent) *out_transparent = 1; *out_rgb = parent_bg; free(buf); return 1; }
        uint32_t raw = hsl_to_rgb(h, sat, lig);
        *out_rgb = blend_rgb(raw, parent_bg, alpha);
        free(buf);
        return 1;
    }

    /* Binary search named color */
    size_t lo = 0, hi = sizeof(k_css_colors) / sizeof(k_css_colors[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcasecmp(s, k_css_colors[mid].name);
        if (cmp == 0) {
            *out_rgb = k_css_colors[mid].rgb;
            free(buf);
            return 1;
        }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    free(buf);
    return 0;
}

/* Parse length in px; returns 1 on success, 0 on auto/unsupported. Negative *out_px represents negative percentage if allow_pct. */
static int css_parse_length(const ipo_css_sheet_t *sheet, const char *val, int font_px, int allow_pct, int *out_px, int *is_auto) {
    if (is_auto) *is_auto = 0;
    if (!val) return 0;
    char *buf = css_resolve_vars_alloc(sheet, val, 0);
    if (!buf) return 0;
    const char *s = buf;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) { free(buf); return 0; }
    if (strncasecmp(s, "auto", 4) == 0) {
        if (is_auto) *is_auto = 1;
        free(buf);
        return 0;
    }
    if (strncasecmp(s, "none", 4) == 0 || strncasecmp(s, "fit-content", 11) == 0 ||
        strncasecmp(s, "max-content", 11) == 0 || strncasecmp(s, "min-content", 11) == 0) {
        free(buf);
        return 0;
    }
    char *e = NULL;
    double v = strtod(s, &e);
    if (e == s) { free(buf); return 0; }
    while (*e && isspace((unsigned char)*e)) e++;
    double px = v;
    if (*e == '%') {
        if (!allow_pct) { free(buf); return 0; }
        int p = (int)(v + (v >= 0 ? 0.5 : -0.5));
        if (p < 0) p = 0; if (p > 100) p = 100;
        *out_px = -p;
        free(buf);
        return 1;
    } else if (strncasecmp(e, "rem", 3) == 0) {
        px = v * 16.0;
    } else if (strncasecmp(e, "em", 2) == 0) {
        px = v * (double)(font_px > 0 ? font_px : 16);
    } else if (strncasecmp(e, "pt", 2) == 0) {
        px = v * (4.0 / 3.0);
    } else if (strncasecmp(e, "vw", 2) == 0) {
        int vw = (sheet && sheet->viewport_w > 0) ? sheet->viewport_w : 320;
        px = (v * (double)vw) / 100.0;
    } else if (strncasecmp(e, "vh", 2) == 0) {
        px = (v * 140.0) / 100.0;
    } else if (strncasecmp(e, "px", 2) == 0 || *e == 0) {
        px = v;
    } else if (strncasecmp(e, "thin", 4) == 0) {
        px = 1.0;
    } else if (strncasecmp(e, "medium", 6) == 0) {
        px = 2.0;
    } else if (strncasecmp(e, "thick", 5) == 0) {
        px = 3.0;
    } else {
        free(buf);
        return 0;
    }
    free(buf);
    if (px < 0) px = 0;
    if (px > 4000) px = 4000;
    *out_px = (int)(px + 0.5);
    return 1;
}

static void css_free_tokens(char **tokens, int n) {
    if (!tokens) return;
    for (int i = 0; i < n; i++) free(tokens[i]);
    free(tokens);
}

/* Split whitespace-delimited CSS shorthand tokens dynamically, keeping (...) grouped */
static char **css_split_tokens_alloc(const char *s, int *out_count) {
    *out_count = 0;
    if (!s) return NULL;
    char **tokens = NULL;
    int n = 0, cap = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        const char *st = s;
        int par = 0;
        while (*s && (par > 0 || !isspace((unsigned char)*s))) {
            if (*s == '(') par++;
            else if (*s == ')' && par > 0) par--;
            s++;
        }
        size_t l = (size_t)(s - st);
        char *tok = css_dup_n(st, l);
        if (!tok) { css_free_tokens(tokens, n); *out_count = 0; return NULL; }
        if (n >= cap) {
            int nc = cap ? cap * 2 : 8;
            char **nt = (char **)realloc(tokens, (size_t)nc * sizeof(char *));
            if (!nt) { free(tok); css_free_tokens(tokens, n); *out_count = 0; return NULL; }
            tokens = nt;
            cap = nc;
        }
        tokens[n++] = tok;
    }
    *out_count = n;
    return tokens;
}

static void parse_box_4_lengths(const ipo_css_sheet_t *sheet, const char *val, int font_px,
                                int16_t out[4], uint8_t *auto_l, uint8_t *auto_r) {
    char *resolved = css_resolve_vars_alloc(sheet, val, 0);
    if (!resolved) return;
    int n = 0;
    char **tok = css_split_tokens_alloc(resolved, &n);
    free(resolved);
    if (!tok || n <= 0) { css_free_tokens(tok, n); return; }
    int px[4] = {0, 0, 0, 0};
    int aut[4] = {0, 0, 0, 0};
    int valid[4] = {0, 0, 0, 0};
    int cnt = (n > 4) ? 4 : n;
    for (int i = 0; i < cnt; i++) {
        valid[i] = css_parse_length(sheet, tok[i], font_px, 0, &px[i], &aut[i]);
    }
    css_free_tokens(tok, n);
    int map[4];
    if (cnt == 1) { map[0] = map[1] = map[2] = map[3] = 0; }
    else if (cnt == 2) { map[0] = map[2] = 0; map[1] = map[3] = 1; }
    else if (cnt == 3) { map[0] = 0; map[1] = map[3] = 1; map[2] = 2; }
    else { map[0] = 0; map[1] = 1; map[2] = 2; map[3] = 3; }
    for (int k = 0; k < 4; k++) {
        int m = map[k];
        if (valid[m]) out[k] = (int16_t)px[m];
    }
    if (auto_l) *auto_l = (uint8_t)aut[map[3]];
    if (auto_r) *auto_r = (uint8_t)aut[map[1]];
}

static void apply_decl(const ipo_css_sheet_t *sheet, const char *prop, const char *val,
                       int parent_font_px, uint32_t parent_bg_rgb, ipo_css_props_t *st) {
    char *vbuf = css_resolve_vars_alloc(sheet, val, 0);
    if (!vbuf) return;
    char *v = vbuf;
    while (*v && isspace((unsigned char)*v)) v++;
    size_t vl = strlen(v);
    while (vl > 0 && isspace((unsigned char)v[vl - 1])) v[--vl] = 0;
    if (!vl) { free(vbuf); return; }

    if (strcmp(prop, "color") == 0) {
        uint32_t col = 0;
        int transp = 0;
        if (ipo_css_parse_color(sheet, v, parent_bg_rgb, &col, &transp) && !transp) {
            st->has_color = 1;
            st->color = col;
        }
    } else if (strcmp(prop, "background-color") == 0) {
        uint32_t col = 0;
        int transp = 0;
        if (ipo_css_parse_color(sheet, v, parent_bg_rgb, &col, &transp)) {
            if (transp) { st->has_bg = 0; }
            else { st->has_bg = 1; st->bg = col; }
        }
    } else if (strcmp(prop, "background") == 0) {
        if (strcasecmp(v, "none") == 0 || strcasecmp(v, "transparent") == 0) {
            st->has_bg = 0;
            free(vbuf);
            return;
        }
        /* Extract first color token or linear-gradient stop color */
        const char *lg = css_strcasestr(v, "linear-gradient(");
        if (lg) {
            const char *p = lg + 16;
            while (*p) {
                const char *seg_st = p;
                int par = 0;
                while (*p && (par > 0 || (*p != ',' && *p != ')'))) {
                    if (*p == '(') par++;
                    else if (*p == ')' && par > 0) par--;
                    p++;
                }
                char *seg = css_dup_n(seg_st, (size_t)(p - seg_st));
                if (seg) {
                    uint32_t col = 0;
                    int transp = 0;
                    /* Trim optional stop percentage like "#1a73e8 0%" */
                    char *sp = seg;
                    while (*sp && isspace((unsigned char)*sp)) sp++;
                    if (strncasecmp(sp, "rgb", 3) != 0 && strncasecmp(sp, "hsl", 3) != 0) {
                        char *w = strpbrk(sp, " \t");
                        if (w) *w = 0;
                    }
                    if (ipo_css_parse_color(sheet, sp, parent_bg_rgb, &col, &transp) && !transp) {
                        st->has_bg = 1;
                        st->bg = col;
                        free(seg);
                        free(vbuf);
                        return;
                    }
                    free(seg);
                }
                if (!*p || *p == ')') break;
                p++;
            }
            free(vbuf);
            return;
        }
        int nt = 0;
        char **tok = css_split_tokens_alloc(v, &nt);
        for (int i = 0; i < nt; i++) {
            uint32_t col = 0;
            int transp = 0;
            if (ipo_css_parse_color(sheet, tok[i], parent_bg_rgb, &col, &transp)) {
                if (transp) { st->has_bg = 0; }
                else { st->has_bg = 1; st->bg = col; }
                break;
            }
        }
        css_free_tokens(tok, nt);
    } else if (strcmp(prop, "display") == 0) {
        if (strcasecmp(v, "none") == 0) st->display = IPO_CSS_DISP_NONE;
        else if (strcasecmp(v, "block") == 0 || strcasecmp(v, "table") == 0 || strcasecmp(v, "grid") == 0) st->display = IPO_CSS_DISP_BLOCK;
        else if (strcasecmp(v, "inline") == 0) st->display = IPO_CSS_DISP_INLINE;
        else if (strcasecmp(v, "inline-block") == 0 || strcasecmp(v, "inline-flex") == 0) st->display = IPO_CSS_DISP_INLINE_BLOCK;
        else if (strcasecmp(v, "flex") == 0) st->display = IPO_CSS_DISP_FLEX_ROW;
        else if (strcasecmp(v, "list-item") == 0) st->display = IPO_CSS_DISP_LIST_ITEM;
    } else if (strcmp(prop, "flex-direction") == 0) {
        if (strncasecmp(v, "column", 6) == 0 && st->display == IPO_CSS_DISP_FLEX_ROW) {
            st->display = IPO_CSS_DISP_FLEX_COL;
        } else if (strncasecmp(v, "row", 3) == 0 && st->display == IPO_CSS_DISP_FLEX_COL) {
            st->display = IPO_CSS_DISP_FLEX_ROW;
        }
    } else if (strcmp(prop, "visibility") == 0) {
        if (strcasecmp(v, "hidden") == 0 || strcasecmp(v, "collapse") == 0) st->visibility_hidden = 1;
        else if (strcasecmp(v, "visible") == 0) st->visibility_hidden = 0;
    } else if (strcmp(prop, "opacity") == 0) {
        double op = strtod(v, NULL);
        if (op <= 0.01) st->visibility_hidden = 1;
    } else if (strcmp(prop, "text-align") == 0) {
        if (strcasecmp(v, "left") == 0 || strcasecmp(v, "start") == 0) st->text_align = IPO_CSS_ALIGN_LEFT;
        else if (strcasecmp(v, "center") == 0) st->text_align = IPO_CSS_ALIGN_CENTER;
        else if (strcasecmp(v, "right") == 0 || strcasecmp(v, "end") == 0) st->text_align = IPO_CSS_ALIGN_RIGHT;
        else if (strcasecmp(v, "justify") == 0) st->text_align = IPO_CSS_ALIGN_JUSTIFY;
    } else if (strcmp(prop, "white-space") == 0) {
        if (strcasecmp(v, "nowrap") == 0) st->white_space = IPO_CSS_WS_NOWRAP;
        else if (strcasecmp(v, "pre") == 0 || strcasecmp(v, "pre-wrap") == 0 || strcasecmp(v, "pre-line") == 0) st->white_space = IPO_CSS_WS_PRE;
        else if (strcasecmp(v, "normal") == 0) st->white_space = IPO_CSS_WS_NORMAL;
    } else if (strcmp(prop, "font-weight") == 0) {
        if (strcasecmp(v, "bold") == 0 || strcasecmp(v, "bolder") == 0) st->font_weight = 1;
        else if (strcasecmp(v, "normal") == 0 || strcasecmp(v, "lighter") == 0) st->font_weight = 0;
        else {
            long w = strtol(v, NULL, 10);
            if (w >= 600) st->font_weight = 1;
            else if (w > 0) st->font_weight = 0;
        }
    } else if (strcmp(prop, "font-style") == 0) {
        if (strcasecmp(v, "italic") == 0 || strcasecmp(v, "oblique") == 0) st->font_style = 1;
        else if (strcasecmp(v, "normal") == 0) st->font_style = 0;
    } else if (strcmp(prop, "text-decoration") == 0 || strcmp(prop, "text-decoration-line") == 0) {
        if (css_strcasestr(v, "underline")) st->text_deco = 1;
        else if (strcasecmp(v, "none") == 0) st->text_deco = 0;
    } else if (strcmp(prop, "text-transform") == 0) {
        if (strcasecmp(v, "uppercase") == 0) st->text_transform = 1;
        else if (strcasecmp(v, "lowercase") == 0) st->text_transform = 2;
        else if (strcasecmp(v, "none") == 0) st->text_transform = 0;
    } else if (strcmp(prop, "font-family") == 0) {
        if (css_strcasestr(v, "mono") || css_strcasestr(v, "courier") || css_strcasestr(v, "consolas"))
            st->font_mono = 1;
        else
            st->font_mono = 0;
    } else if (strcmp(prop, "font-size") == 0) {
        int px = 0;
        if (strcasecmp(v, "xx-small") == 0) px = 9;
        else if (strcasecmp(v, "x-small") == 0) px = 11;
        else if (strcasecmp(v, "small") == 0) px = 13;
        else if (strcasecmp(v, "medium") == 0) px = 16;
        else if (strcasecmp(v, "large") == 0) px = 19;
        else if (strcasecmp(v, "x-large") == 0) px = 24;
        else if (strcasecmp(v, "xx-large") == 0) px = 32;
        else if (strcasecmp(v, "smaller") == 0) px = (parent_font_px > 10) ? (parent_font_px - 3) : 8;
        else if (strcasecmp(v, "larger") == 0) px = parent_font_px + 3;
        else if (css_parse_length(sheet, v, parent_font_px, 1, &px, NULL)) {
            if (px < 0) px = (parent_font_px * (-px)) / 100;
        }
        if (px > 0) { st->has_font_size = 1; st->font_px = (int16_t)px; }
    } else if (strcmp(prop, "list-style") == 0 || strcmp(prop, "list-style-type") == 0) {
        if (css_strcasestr(v, "none")) st->list_style_none = 1;
        else st->list_style_none = 0;
    } else if (strcmp(prop, "padding") == 0) {
        st->has_padding = 1;
        parse_box_4_lengths(sheet, v, parent_font_px, st->padding, NULL, NULL);
    } else if (strncmp(prop, "padding-", 8) == 0) {
        int px = 0;
        if (css_parse_length(sheet, v, parent_font_px, 0, &px, NULL)) {
            st->has_padding = 1;
            if (strcmp(prop + 8, "top") == 0) st->padding[0] = (int16_t)px;
            else if (strcmp(prop + 8, "right") == 0) st->padding[1] = (int16_t)px;
            else if (strcmp(prop + 8, "bottom") == 0) st->padding[2] = (int16_t)px;
            else if (strcmp(prop + 8, "left") == 0) st->padding[3] = (int16_t)px;
        }
    } else if (strcmp(prop, "margin") == 0) {
        st->has_margin = 1;
        parse_box_4_lengths(sheet, v, parent_font_px, st->margin, &st->margin_auto_l, &st->margin_auto_r);
    } else if (strncmp(prop, "margin-", 7) == 0) {
        int px = 0, is_auto = 0;
        int ok = css_parse_length(sheet, v, parent_font_px, 0, &px, &is_auto);
        if (ok || is_auto) st->has_margin = 1;
        if (strcmp(prop + 7, "top") == 0 && ok) st->margin[0] = (int16_t)px;
        else if (strcmp(prop + 7, "right") == 0) {
            if (is_auto) st->margin_auto_r = 1;
            else if (ok) { st->margin_auto_r = 0; st->margin[1] = (int16_t)px; }
        } else if (strcmp(prop + 7, "bottom") == 0 && ok) st->margin[2] = (int16_t)px;
        else if (strcmp(prop + 7, "left") == 0) {
            if (is_auto) st->margin_auto_l = 1;
            else if (ok) { st->margin_auto_l = 0; st->margin[3] = (int16_t)px; }
        }
    } else if (strcmp(prop, "width") == 0) {
        int px = 0, is_auto = 0;
        if (css_parse_length(sheet, v, parent_font_px, 1, &px, &is_auto)) {
            st->has_width = 1;
            st->width_val = (int16_t)px;
        }
    } else if (strcmp(prop, "max-width") == 0) {
        int px = 0, is_auto = 0;
        if (css_parse_length(sheet, v, parent_font_px, 1, &px, &is_auto)) {
            st->has_max_width = 1;
            st->max_width_val = (int16_t)px;
        }
    } else if (strcmp(prop, "border") == 0 ||
               strcmp(prop, "border-top") == 0 || strcmp(prop, "border-right") == 0 ||
               strcmp(prop, "border-bottom") == 0 || strcmp(prop, "border-left") == 0) {
        int side = -1;
        if (strcmp(prop, "border-top") == 0) side = 0;
        else if (strcmp(prop, "border-right") == 0) side = 1;
        else if (strcmp(prop, "border-bottom") == 0) side = 2;
        else if (strcmp(prop, "border-left") == 0) side = 3;

        if (strcasecmp(v, "none") == 0 || strcasecmp(v, "0") == 0) {
            if (side < 0) memset(st->border_w, 0, sizeof(st->border_w));
            else st->border_w[side] = 0;
            free(vbuf);
            return;
        }
        int nt = 0;
        char **tok = css_split_tokens_alloc(v, &nt);
        int bw = 1;
        int got_w = 0;
        uint32_t bcol = st->has_border ? st->border_color : 0xCCCCCCu;
        int got_style_none = 0;
        for (int i = 0; i < nt; i++) {
            if (strcasecmp(tok[i], "none") == 0 || strcasecmp(tok[i], "hidden") == 0) {
                got_style_none = 1;
                bw = 0;
                got_w = 1;
            } else {
                int len_px = 0, transp = 0;
                uint32_t c = 0;
                if (css_parse_length(sheet, tok[i], parent_font_px, 0, &len_px, NULL)) {
                    bw = (len_px > 6) ? 6 : len_px;
                    got_w = 1;
                } else if (ipo_css_parse_color(sheet, tok[i], parent_bg_rgb, &c, &transp)) {
                    if (!transp) bcol = c;
                    else got_style_none = 1;
                }
            }
        }
        css_free_tokens(tok, nt);
        if (got_style_none) bw = 0;
        else if (!got_w) bw = 1;
        if (bw > 0) { st->has_border = 1; st->border_color = bcol; }
        if (side < 0) {
            for (int k = 0; k < 4; k++) st->border_w[k] = (uint8_t)bw;
        } else {
            st->border_w[side] = (uint8_t)bw;
        }
    } else if (strcmp(prop, "border-color") == 0) {
        uint32_t c = 0; int transp = 0;
        if (ipo_css_parse_color(sheet, v, parent_bg_rgb, &c, &transp) && !transp) {
            st->has_border = 1;
            st->border_color = c;
        }
    } else if (strcmp(prop, "border-width") == 0) {
        int16_t b[4] = {0, 0, 0, 0};
        parse_box_4_lengths(sheet, v, parent_font_px, b, NULL, NULL);
        for (int k = 0; k < 4; k++) {
            int w = (b[k] > 6) ? 6 : (b[k] < 0 ? 0 : b[k]);
            st->border_w[k] = (uint8_t)w;
            if (w > 0) st->has_border = 1;
        }
    }
    free(vbuf);
}

static size_t parse_decls_into(const char *blk, size_t len, css_decl_t **out_decls, ipo_css_sheet_t *var_sink) {
    if (out_decls) *out_decls = NULL;
    css_decl_t *out = NULL;
    size_t cap = 0;
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (isspace((unsigned char)blk[i]) || blk[i] == ';')) i++;
        if (i >= len) break;
        const char *pstart = blk + i;
        while (i < len && blk[i] != ':' && blk[i] != ';') i++;
        if (i >= len || blk[i] != ':') {
            while (i < len && blk[i] != ';') i++;
            continue;
        }
        size_t plen = (size_t)((blk + i) - pstart);
        i++; /* skip ':' */
        const char *vstart = blk + i;
        int par = 0;
        char quote = 0;
        while (i < len) {
            char ch = blk[i];
            if (quote) {
                if (ch == '\\' && i + 1 < len) { i += 2; continue; }
                if (ch == quote) quote = 0;
            } else {
                if (ch == '\'' || ch == '"') quote = ch;
                else if (ch == '(') par++;
                else if (ch == ')' && par > 0) par--;
                else if (ch == ';' && par == 0) break;
            }
            i++;
        }
        size_t vlen = (size_t)((blk + i) - vstart);
        if (i < len && blk[i] == ';') i++;

        char *prop = css_trim_dup(pstart, plen);
        char *val = css_trim_dup(vstart, vlen);
        if (!prop || !val) { free(prop); free(val); continue; }

        if (strncmp(prop, "--", 2) == 0) {
            if (var_sink) css_set_var(var_sink, prop, val);
            free(prop); free(val);
            continue;
        }

        for (char *pp = prop; *pp; pp++) *pp = (char)tolower((unsigned char)*pp);
        uint8_t important = 0;
        size_t vl = strlen(val);
        while (vl > 0 && isspace((unsigned char)val[vl - 1])) val[--vl] = 0;
        if (vl >= 10 && strcasecmp(val + vl - 10, "!important") == 0) {
            important = 1;
            vl -= 10;
            while (vl > 0 && isspace((unsigned char)val[vl - 1])) val[--vl] = 0;
            val[vl] = 0;
        } else if (vl >= 9 && strcasecmp(val + vl - 9, "important") == 0) {
            size_t k = vl - 9;
            while (k > 0 && isspace((unsigned char)val[k - 1])) k--;
            if (k > 0 && val[k - 1] == '!') {
                important = 1;
                k--;
                while (k > 0 && isspace((unsigned char)val[k - 1])) k--;
                val[k] = 0;
            }
        }

        if (*prop && *val) {
            if (n >= cap) {
                size_t next_cap = cap ? (cap * 2u) : 8u;
                css_decl_t *nd = (css_decl_t *)realloc(out, next_cap * sizeof(css_decl_t));
                if (!nd) {
                    free(prop);
                    free(val);
                    break;
                }
                out = nd;
                cap = next_cap;
            }
            out[n].prop = prop;
            out[n].val = val;
            out[n].important = important;
            n++;
        } else {
            free(prop);
            free(val);
        }
    }
    if (n == 0) {
        free(out);
        out = NULL;
    }
    if (out_decls) *out_decls = out;
    return n;
}

static int parse_selector_branch(const char *sel, size_t len, css_seg_t **out_segs, size_t *out_n, uint32_t *out_spec) {
    *out_segs = NULL;
    *out_n = 0;
    char *buf = css_trim_dup(sel, len);
    if (!buf) return 0;
    const char *p = buf;
    css_seg_t *segs = NULL;
    size_t n = 0;
    size_t cap = 0;
    uint32_t spec = 0;
    css_comb_t pending_comb = COMB_NONE;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '>') {
            if (n == 0) { css_free_segs(segs, n); free(buf); return 0; }
            pending_comb = COMB_CHILD;
            p++;
            continue;
        }
        if (*p == '+' || *p == '~') { css_free_segs(segs, n); free(buf); return 0; }
        if (n >= cap) {
            size_t new_cap = cap ? (cap * 2u) : 8u;
            if (new_cap <= cap || new_cap > SIZE_MAX / sizeof(css_seg_t)) { css_free_segs(segs, n); free(buf); return 0; }
            css_seg_t *ns = (css_seg_t *)realloc(segs, new_cap * sizeof(css_seg_t));
            if (!ns) { css_free_segs(segs, n); free(buf); return 0; }
            segs = ns;
            cap = new_cap;
        }

        css_seg_t *sg = &segs[n];
        memset(sg, 0, sizeof(*sg));
        sg->comb_to_prev = (n == 0) ? COMB_NONE : (pending_comb == COMB_CHILD ? COMB_CHILD : COMB_DESC);
        pending_comb = COMB_DESC;
        n++;

        int parsed_any = 0;
        if (isalnum((unsigned char)*p) || *p == '*' || *p == '_' || *p == '-') {
            const char *st = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '*' || *p == '_' || *p == '-')) p++;
            size_t l = (size_t)(p - st);
            if (!(l == 1 && st[0] == '*')) {
                sg->tag = css_dup_lower_n(st, l);
                if (!sg->tag) { css_free_segs(segs, n); free(buf); return 0; }
                spec += 1;
            }
            parsed_any = 1;
        }

        while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '+' && *p != '~') {
            parsed_any = 1;
            if (*p == '#') {
                p++;
                const char *st = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
                size_t l = (size_t)(p - st);
                if (l == 0) { css_free_segs(segs, n); free(buf); return 0; }
                free(sg->id);
                sg->id = css_dup_n(st, l);
                if (!sg->id) { css_free_segs(segs, n); free(buf); return 0; }
                spec += 65536u;
            } else if (*p == '.') {
                p++;
                const char *st = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
                size_t l = (size_t)(p - st);
                if (l == 0) { css_free_segs(segs, n); free(buf); return 0; }
                char *cdup = css_dup_n(st, l);
                if (!cdup) { css_free_segs(segs, n); free(buf); return 0; }
                char **ncls = (char **)realloc(sg->classes, (sg->n_classes + 1u) * sizeof(char *));
                if (!ncls) { free(cdup); css_free_segs(segs, n); free(buf); return 0; }
                sg->classes = ncls;
                sg->classes[sg->n_classes++] = cdup;
                spec += 256u;
            } else if (*p == '[') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                const char *nst = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':')) p++;
                size_t nl = (size_t)(p - nst);
                free(sg->attr_name);
                sg->attr_name = css_dup_lower_n(nst, nl);
                while (*p && isspace((unsigned char)*p)) p++;
                uint8_t op = 1;
                if (*p == '=') { op = 2; p++; }
                else if (*p == '~' && p[1] == '=') { op = 3; p += 2; }
                else if (*p == '*' && p[1] == '=') { op = 4; p += 2; }
                else if (*p == '|' && p[1] == '=') { op = 2; p += 2; }
                free(sg->attr_val);
                sg->attr_val = NULL;
                if (op > 1) {
                    while (*p && isspace((unsigned char)*p)) p++;
                    char q = 0;
                    if (*p == '"' || *p == '\'') q = *p++;
                    const char *vst = p;
                    while (*p && ((q && *p != q) || (!q && *p != ']' && !isspace((unsigned char)*p)))) p++;
                    size_t vl = (size_t)(p - vst);
                    sg->attr_val = css_dup_n(vst, vl);
                    if (q && *p == q) p++;
                }
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                sg->attr_op = (sg->attr_name && sg->attr_name[0] && (op == 1 || sg->attr_val)) ? op : 0;
                spec += 256u;
            } else if (*p == ':') {
                p++;
                int dbl = (*p == ':');
                if (dbl) p++;
                const char *pst = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '-')) p++;
                size_t pl = (size_t)(p - pst);
                if (*p == '(') {
                    int par = 1;
                    p++;
                    while (*p && par > 0) {
                        if (*p == '(') par++;
                        else if (*p == ')') par--;
                        p++;
                    }
                    css_free_segs(segs, n);
                    free(buf);
                    return 0;
                }
                if (dbl) { css_free_segs(segs, n); free(buf); return 0; }
                if (pl == 4 && strncasecmp(pst, "root", 4) == 0) {
                    free(sg->tag);
                    sg->tag = strdup("html");
                    spec += 256u;
                } else if ((pl == 4 && strncasecmp(pst, "link", 4) == 0) ||
                           (pl == 7 && strncasecmp(pst, "visited", 7) == 0)) {
                    spec += 256u;
                } else {
                    /* :hover, :focus, :active, :first-child, etc. */
                    css_free_segs(segs, n);
                    free(buf);
                    return 0;
                }
            } else {
                css_free_segs(segs, n);
                free(buf);
                return 0;
            }
        }
        if (!parsed_any) { css_free_segs(segs, n); free(buf); return 0; }
    }
    free(buf);
    if (n == 0) { css_free_segs(segs, n); return 0; }
    *out_segs = segs;
    *out_n = n;
    *out_spec = spec;
    return 1;
}

static int css_media_applies(const char *cond, int viewport_w) {
    if (!cond) return 1;
    if (css_strcasestr(cond, "print") && !css_strcasestr(cond, "screen") && !css_strcasestr(cond, "all")) {
        return 0;
    }
    if (css_strcasestr(cond, "prefers-color-scheme: dark") || css_strcasestr(cond, "prefers-color-scheme:dark")) {
        return 0;
    }
    const char *mw = css_strcasestr(cond, "min-width");
    if (mw) {
        const char *c = strchr(mw, ':');
        if (c) {
            double v = strtod(c + 1, NULL);
            if ((int)v > (viewport_w > 0 ? viewport_w : 320)) return 0;
        }
    }
    const char *mxw = css_strcasestr(cond, "max-width");
    if (mxw) {
        const char *c = strchr(mxw, ':');
        if (c) {
            double v = strtod(c + 1, NULL);
            if ((int)v < (viewport_w > 0 ? viewport_w : 320)) return 0;
        }
    }
    return 1;
}

ipo_css_sheet_t *ipo_css_create(int viewport_w) {
    ipo_css_sheet_t *s = (ipo_css_sheet_t *)calloc(1, sizeof(ipo_css_sheet_t));
    if (!s) return NULL;
    s->viewport_w = viewport_w > 0 ? viewport_w : 320;
    return s;
}

void ipo_css_set_viewport(ipo_css_sheet_t *sheet, int viewport_w) {
    if (sheet && viewport_w > 0) sheet->viewport_w = viewport_w;
}

void ipo_css_free(ipo_css_sheet_t *sheet) {
    if (!sheet) return;
    for (size_t i = 0; i < sheet->n_rules; i++) {
        css_free_segs(sheet->rules[i].segs, sheet->rules[i].n_segs);
        for (uint16_t j = 0; j < sheet->rules[i].n_decls; j++) {
            free(sheet->rules[i].decls[j].prop);
            free(sheet->rules[i].decls[j].val);
        }
        free(sheet->rules[i].decls);
    }
    free(sheet->rules);
    for (size_t i = 0; i < sheet->n_vars; i++) {
        free(sheet->vars[i].name);
        free(sheet->vars[i].val);
    }
    free(sheet->vars);
    free(sheet);
}

static size_t css_skip_ws_comments(const char *s, size_t len, size_t i) {
    while (i < len) {
        if (isspace((unsigned char)s[i])) { i++; continue; }
        if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        if (i + 3 < len && s[i] == '<' && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            i += 4;
            continue;
        }
        if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
            i += 3;
            continue;
        }
        break;
    }
    return i;
}

/* Strip CSS comments into clean temporary text for declaration / selector blocks */
static char *css_strip_comments_dup(const char *s, size_t len, size_t *out_len) {
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    char q = 0;
    for (size_t i = 0; i < len; ) {
        if (!q && i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            out[o++] = ' ';
            continue;
        }
        if (!q && (s[i] == '"' || s[i] == '\'')) q = s[i];
        else if (q && s[i] == '\\' && i + 1 < len) {
            out[o++] = s[i++]; out[o++] = s[i++]; continue;
        } else if (q && s[i] == q) q = 0;
        out[o++] = s[i++];
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

static void css_parse_block_list(ipo_css_sheet_t *sheet, const char *s, size_t len, int depth) {
    if (!sheet || !s || depth > 4) return;
    size_t i = 0;
    while (i < len) {
        i = css_skip_ws_comments(s, len, i);
        if (i >= len) break;
        const char *hdr_st = s + i;
        char q = 0;
        while (i < len) {
            char ch = s[i];
            if (q) {
                if (ch == '\\' && i + 1 < len) { i += 2; continue; }
                if (ch == q) q = 0;
            } else {
                if (ch == '\'' || ch == '"') q = ch;
                else if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
                    i = (i + 1 < len) ? i + 2 : len;
                    continue;
                } else if (ch == '{' || ch == ';') break;
            }
            i++;
        }
        if (i >= len) break;
        if (s[i] == ';') { i++; continue; } /* @charset / @import / etc. */

        size_t hdr_len = (size_t)((s + i) - hdr_st);
        i++; /* skip '{' */
        const char *body_st = s + i;
        int br = 1;
        q = 0;
        while (i < len && br > 0) {
            char ch = s[i];
            if (q) {
                if (ch == '\\' && i + 1 < len) { i += 2; continue; }
                if (ch == q) q = 0;
            } else {
                if (ch == '\'' || ch == '"') q = ch;
                else if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
                    i = (i + 1 < len) ? i + 2 : len;
                    continue;
                } else if (ch == '{') br++;
                else if (ch == '}') br--;
            }
            if (br > 0) i++;
        }
        size_t body_len = (size_t)((s + i) - body_st);
        if (i < len && s[i] == '}') i++;

        size_t clean_hdr_len = 0;
        char *clean_hdr = css_strip_comments_dup(hdr_st, hdr_len, &clean_hdr_len);
        if (!clean_hdr) continue;
        char *hp = clean_hdr;
        while (*hp && isspace((unsigned char)*hp)) hp++;

        if (hp[0] == '@') {
            if (strncasecmp(hp, "@media", 6) == 0 && css_media_applies(hp + 6, sheet->viewport_w)) {
                css_parse_block_list(sheet, body_st, body_len, depth + 1);
            } else if (strncasecmp(hp, "@supports", 9) == 0 || strncasecmp(hp, "@layer", 6) == 0) {
                if (strchr(body_st, '{')) css_parse_block_list(sheet, body_st, body_len, depth + 1);
            }
            free(clean_hdr);
            continue;
        }

        size_t clean_body_len = 0;
        char *clean_body = css_strip_comments_dup(body_st, body_len, &clean_body_len);
        if (!clean_body) { free(clean_hdr); continue; }

        css_decl_t *tmp_decls = NULL;
        size_t n_decls = parse_decls_into(clean_body, clean_body_len, &tmp_decls, sheet);
        free(clean_body);

        if (n_decls == 0 || !tmp_decls) {
            free(tmp_decls);
            free(clean_hdr);
            continue;
        }

        /* Split comma-separated selector list safely (skipping commas inside [...]) */
        const char *sp = hp;
        int added_rules = 0;
        while (*sp) {
            const char *b_st = sp;
            int sq = 0, par = 0;
            while (*sp && (sq > 0 || par > 0 || *sp != ',')) {
                if (*sp == '[') sq++;
                else if (*sp == ']' && sq > 0) sq--;
                else if (*sp == '(') par++;
                else if (*sp == ')' && par > 0) par--;
                sp++;
            }
            size_t b_len = (size_t)(sp - b_st);
            if (*sp == ',') sp++;

            css_seg_t *segs = NULL;
            size_t n_segs = 0;
            uint32_t spec = 0;
            if (!parse_selector_branch(b_st, b_len, &segs, &n_segs, &spec)) continue;

            if (sheet->n_rules >= sheet->cap_rules) {
                size_t nc = sheet->cap_rules ? sheet->cap_rules * 2 : 64;
                css_rule_t *nr = (css_rule_t *)realloc(sheet->rules, nc * sizeof(css_rule_t));
                if (!nr) { css_free_segs(segs, n_segs); break; }
                sheet->rules = nr;
                sheet->cap_rules = nc;
            }
            css_rule_t *r = &sheet->rules[sheet->n_rules];
            memset(r, 0, sizeof(*r));
            r->segs = segs;
            r->n_segs = n_segs;
            r->specificity = spec;
            r->order = sheet->next_order++;
            r->n_decls = (uint16_t)n_decls;
            if (added_rules == 0) {
                r->decls = tmp_decls;
            } else {
                r->decls = (css_decl_t *)malloc(n_decls * sizeof(css_decl_t));
                if (!r->decls) { css_free_segs(segs, n_segs); break; }
                for (size_t d = 0; d < n_decls; d++) {
                    r->decls[d].prop = strdup(tmp_decls[d].prop);
                    r->decls[d].val = strdup(tmp_decls[d].val);
                    r->decls[d].important = tmp_decls[d].important;
                }
            }
            added_rules = 1;
            sheet->n_rules++;
        }
        if (!added_rules) {
            for (size_t d = 0; d < n_decls; d++) {
                free(tmp_decls[d].prop);
                free(tmp_decls[d].val);
            }
            free(tmp_decls);
        }
        free(clean_hdr);
    }
}

void ipo_css_add_stylesheet(ipo_css_sheet_t *sheet, const char *css, size_t len) {
    if (!sheet || !css || !len) return;
    css_parse_block_list(sheet, css, len, 0);
}

static int class_list_contains(const char *list, const char *cls) {
    if (!list || !*list || !cls || !*cls) return 0;
    size_t clen = strlen(cls);
    const char *p = list;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *st = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - st) == clen && strncmp(st, cls, clen) == 0) return 1;
    }
    return 0;
}

static int seg_matches_node(const css_seg_t *sg, const ipo_css_node_t *node) {
    if (!node) return 0;
    if (sg->tag && sg->tag[0]) {
        if (!node->tag || strcmp(sg->tag, node->tag) != 0) return 0;
    }
    if (sg->id && sg->id[0]) {
        if (!node->id || strcmp(sg->id, node->id) != 0) return 0;
    }
    for (size_t c = 0; c < sg->n_classes; c++) {
        if (!class_list_contains(node->classes, sg->classes[c])) return 0;
    }
    if (sg->attr_op != 0 && sg->attr_name) {
        if (!node->raw_attr) return 0;
        unsigned char *av = get_attr_val((unsigned char *)node->raw_attr, (unsigned char *)sg->attr_name);
        if (!av) return 0;
        const char *aval = sg->attr_val ? sg->attr_val : "";
        int ok = 0;
        if (sg->attr_op == 1) ok = 1;
        else if (sg->attr_op == 2) ok = (strcasecmp((char *)av, aval) == 0);
        else if (sg->attr_op == 3) ok = class_list_contains((char *)av, aval);
        else if (sg->attr_op == 4) ok = (css_strcasestr((char *)av, aval) != NULL);
        /* free using Links' standard allocator? Note Links uses malloc/mem_free; call mem_free */
        extern void mem_free(void *);
        mem_free(av);
        if (!ok) return 0;
    }
    return 1;
}

static int rule_matches_stack(const css_rule_t *r, const ipo_css_node_t *ancestors, int n_ancestors) {
    if (n_ancestors <= 0 || r->n_segs == 0) return 0;
    int si = (int)r->n_segs - 1;
    if (!seg_matches_node(&r->segs[si], &ancestors[0])) return 0;
    int ai = 0;
    while (si > 0) {
        css_comb_t comb = r->segs[si].comb_to_prev;
        si--;
        if (comb == COMB_CHILD) {
            ai++;
            if (ai >= n_ancestors) return 0;
            if (!seg_matches_node(&r->segs[si], &ancestors[ai])) return 0;
        } else {
            int matched = 0;
            for (ai = ai + 1; ai < n_ancestors; ai++) {
                if (seg_matches_node(&r->segs[si], &ancestors[ai])) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) return 0;
        }
    }
    return 1;
}

void ipo_css_compute(const ipo_css_sheet_t *sheet,
                     const ipo_css_node_t *ancestors,
                     int n_ancestors,
                     const char *inline_style,
                     int parent_font_px,
                     uint32_t parent_bg_rgb,
                     ipo_css_props_t *out) {
    memset(out, 0, sizeof(*out));
    out->font_weight = -1;
    out->font_style = -1;
    out->text_deco = -1;
    out->font_mono = -1;
    out->text_align = IPO_CSS_ALIGN_UNSET;
    out->white_space = IPO_CSS_WS_UNSET;
    out->visibility_hidden = -1;
    out->list_style_none = -1;

    if (parent_font_px <= 0) parent_font_px = 16;

    const css_rule_t **matches = NULL;
    size_t cap_m = 0;
    size_t nm = 0;
    if (sheet && ancestors && n_ancestors > 0) {
        for (size_t i = 0; i < sheet->n_rules; i++) {
            if (rule_matches_stack(&sheet->rules[i], ancestors, n_ancestors)) {
                if (nm >= cap_m) {
                    size_t next_cap = cap_m ? (cap_m * 2u) : 32u;
                    const css_rule_t **nm_arr = (const css_rule_t **)realloc(matches, next_cap * sizeof(const css_rule_t *));
                    if (!nm_arr) break;
                    matches = nm_arr;
                    cap_m = next_cap;
                }
                matches[nm++] = &sheet->rules[i];
            }
        }
    }
    /* Sort matching rules by (specificity, order) ascending */
    for (size_t i = 1; i < nm; i++) {
        const css_rule_t *key = matches[i];
        size_t j = i;
        while (j > 0 && (matches[j - 1]->specificity > key->specificity ||
                         (matches[j - 1]->specificity == key->specificity && matches[j - 1]->order > key->order))) {
            matches[j] = matches[j - 1];
            j--;
        }
        matches[j] = key;
    }

    css_decl_t *inl = NULL;
    size_t n_inl = 0;
    if (inline_style && *inline_style) {
        n_inl = parse_decls_into(inline_style, strlen(inline_style), &inl, NULL);
    }

    /* Resolve background/font-size non-important first, then full cascade so color contrast/em use updated parent/bg */
    for (int imp = 0; imp <= 1; imp++) {
        for (size_t i = 0; i < nm; i++) {
            const css_rule_t *r = matches[i];
            for (uint16_t d = 0; d < r->n_decls; d++) {
                if ((int)r->decls[d].important == imp) {
                    uint32_t bg = out->has_bg ? out->bg : parent_bg_rgb;
                    apply_decl(sheet, r->decls[d].prop, r->decls[d].val, parent_font_px, bg, out);
                }
            }
        }
        for (size_t d = 0; d < n_inl; d++) {
            if ((int)inl[d].important == imp) {
                uint32_t bg = out->has_bg ? out->bg : parent_bg_rgb;
                apply_decl(sheet, inl[d].prop, inl[d].val, parent_font_px, bg, out);
            }
        }
    }

    for (size_t d = 0; d < n_inl; d++) {
        free(inl[d].prop);
        free(inl[d].val);
    }
    free(inl);
    free(matches);
}

int ipo_css_resolve_table_bg(const ipo_css_sheet_t *sheet,
                               const char *tag,
                               const char *id,
                               const char *classes,
                               const char *inline_style,
                               uint32_t parent_bg_rgb,
                               uint32_t *out_rgb) {
    ipo_css_node_t n;
    n.tag = tag;
    n.id = id;
    n.classes = classes;
    n.raw_attr = NULL;
    ipo_css_props_t p;
    ipo_css_compute(sheet, &n, 1, inline_style, 16, parent_bg_rgb, &p);
    if (p.has_bg) {
        *out_rgb = p.bg;
        return 1;
    }
    return 0;
}

int ipo_css_px_to_links_size(int px) {
    if (px <= 11) return 1;
    if (px <= 13) return 2;
    if (px <= 17) return 3;
    if (px <= 20) return 4;
    if (px <= 25) return 5;
    if (px <= 32) return 6;
    return 7;
}
