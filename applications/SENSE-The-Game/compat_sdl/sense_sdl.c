/*
 * SDL2 / SDL_image / SDL_mixer compatibility shim for SENSE: The Game on IPO_OS.
 */

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_ttf.h>

#include <driver/audio_core.h>
#include <driver/input/keyboard.h>
#include <ioport.h>
#include <kernel/input_ext.h>
#include <kernel/process.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>
#include <vga_gfx.h>
#include <wm.h>

extern uint8_t *ns_image_wuffs_decode_to_bgra(const uint8_t *data, size_t len,
                                              int *out_w, int *out_h,
                                              size_t *out_stride, size_t *out_buf_len);

void *ns_texture_new(int width, int height, int format,
                     void *bytes, size_t stride, size_t len) {
    (void)width;
    (void)height;
    (void)format;
    (void)bytes;
    (void)stride;
    (void)len;
    return NULL;
}

/* =========================================================================
 * Adaptive Perceptual 256-Color VGA DAC Palette, RGB555->Index LUT & Blend
 * ========================================================================= */
#define PAL_COUNT 256
#define PAL_STD_RESERVED 16
#define PAL_GRAY_RESERVED 8
#define PAL_MC_START (PAL_STD_RESERVED + PAL_GRAY_RESERVED)
#define PAL_MC_SLOTS (PAL_COUNT - PAL_MC_START)
#define BLEND_LEVELS 7

static const uint8_t s_std16_dac[PAL_STD_RESERVED][3] = {
    { 0,  0,  0}, { 0,  0, 42}, { 0, 42,  0}, { 0, 42, 42},
    {42,  0,  0}, {42,  0, 42}, {42, 21,  0}, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
    {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}
};

static uint8_t s_pal_dac[PAL_COUNT][3];
static uint8_t s_pal_r[PAL_COUNT];
static uint8_t s_pal_g[PAL_COUNT];
static uint8_t s_pal_b[PAL_COUNT];
static uint8_t s_rgb15_to_pal[32768];
static uint8_t s_blend[BLEND_LEVELS][256][256];
static uint32_t s_hist_rgb15[32768];
static bool    s_luts_ready = false;
static bool    s_hist_dirty = false;
static bool    s_adaptive_active = false;
static bool    s_palette_applied = false;
static int     s_active_pal_count = 248;

static const uint8_t s_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

static uint8_t dac6_to_8(uint8_t v6) {
    v6 &= 0x3Fu;
    return (uint8_t)((v6 << 2) | (v6 >> 4));
}

static uint8_t u8_to_dac6(int v) {
    if (v <= 0) return 0;
    if (v >= 255) return 63;
    return (uint8_t)((v * 63 + 127) / 255);
}

static void set_pal_dac(int idx, uint8_t r6, uint8_t g6, uint8_t b6) {
    r6 &= 0x3Fu;
    g6 &= 0x3Fu;
    b6 &= 0x3Fu;
    s_pal_dac[idx][0] = r6;
    s_pal_dac[idx][1] = g6;
    s_pal_dac[idx][2] = b6;
    s_pal_r[idx] = dac6_to_8(r6);
    s_pal_g[idx] = dac6_to_8(g6);
    s_pal_b[idx] = dac6_to_8(b6);
}

/* Redmean perceptual distance preserves hue accuracy on browns, skin, and teals. */
static uint8_t find_nearest_pal(int r, int g, int b) {
    int best_i = 0;
    int best_d = 0x7FFFFFFF;
    int lim = s_active_pal_count;
    for (int i = 0; i < lim; i++) {
        int pr = (int)s_pal_r[i];
        int pg = (int)s_pal_g[i];
        int pb = (int)s_pal_b[i];
        int rmean = (r + pr) >> 1;
        int dr = r - pr;
        int dg = g - pg;
        int db = b - pb;
        int d = (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rmean) * db * db) >> 8);
        if (d < best_d) {
            best_d = d;
            best_i = i;
            if (d == 0) break;
        }
    }
    return (uint8_t)best_i;
}

static inline uint8_t quant_rgb(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return s_rgb15_to_pal[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

static inline uint8_t quant_rgb_dither(int r, int g, int b, int x, int y) {
    int shift = s_adaptive_active ? 2 : 0;
    int d = ((int)s_bayer4[y & 3][x & 3] - 7) >> shift;
    return quant_rgb(r + d, g + d, b + d);
}

static void init_default_palette_entries(void) {
    for (int i = 0; i < PAL_STD_RESERVED; i++) {
        set_pal_dac(i, s_std16_dac[i][0], s_std16_dac[i][1], s_std16_dac[i][2]);
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 63 / 15);
        set_pal_dac(16 + i, v, v, v);
    }
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                set_pal_dac(idx++,
                            (uint8_t)(r * 63 / 5),
                            (uint8_t)(g * 63 / 5),
                            (uint8_t)(b * 63 / 5));
            }
        }
    }
    for (int i = 248; i < PAL_COUNT; i++) {
        set_pal_dac(i, 0, 0, 0);
    }
    s_active_pal_count = 248;
}

static void rebuild_rgb15_and_blend_luts(void) {
    for (int r5 = 0; r5 < 32; r5++) {
        int r8 = (r5 << 3) | (r5 >> 2);
        for (int g5 = 0; g5 < 32; g5++) {
            int g8 = (g5 << 3) | (g5 >> 2);
            for (int b5 = 0; b5 < 32; b5++) {
                int b8 = (b5 << 3) | (b5 >> 2);
                s_rgb15_to_pal[(r5 << 10) | (g5 << 5) | b5] =
                    find_nearest_pal(r8, g8, b8);
            }
        }
    }

    int lim = s_active_pal_count;
    for (int lvl = 0; lvl < BLEND_LEVELS; lvl++) {
        int a = lvl + 1; /* 1..7 out of 8 */
        int ia = 8 - a;
        for (int src = 0; src < lim; src++) {
            int sr = s_pal_r[src], sg = s_pal_g[src], sb = s_pal_b[src];
            for (int dst = 0; dst < lim; dst++) {
                int mr = (sr * a + (int)s_pal_r[dst] * ia) >> 3;
                int mg = (sg * a + (int)s_pal_g[dst] * ia) >> 3;
                int mb = (sb * a + (int)s_pal_b[dst] * ia) >> 3;
                s_blend[lvl][src][dst] = quant_rgb(mr, mg, mb);
            }
            for (int dst = lim; dst < 256; dst++) {
                s_blend[lvl][src][dst] = (uint8_t)src;
            }
        }
        for (int src = lim; src < 256; src++) {
            for (int dst = 0; dst < 256; dst++) {
                s_blend[lvl][src][dst] = (uint8_t)dst;
            }
        }
    }
}

static void apply_sense_dac_palette(bool restore_default) {
    if (restore_default) {
        if (!s_palette_applied) return;
        init_default_palette_entries();
        s_palette_applied = false;
        s_adaptive_active = false;
    }
    for (int i = 0; i < PAL_COUNT; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            s_pal_dac[i][0],
                            s_pal_dac[i][1],
                            s_pal_dac[i][2]);
    }
    /* Keep 0..15 WM/Win95 colors intact; program 16..255 hardware DAC slots directly */
    if (process_is_foreground()) {
        outb(0x3C8, (uint8_t)PAL_STD_RESERVED);
        for (int i = PAL_STD_RESERVED; i < PAL_COUNT; i++) {
            outb(0x3C9, s_pal_dac[i][0] & 0x3Fu);
            outb(0x3C9, s_pal_dac[i][1] & 0x3Fu);
            outb(0x3C9, s_pal_dac[i][2] & 0x3Fu);
        }
    }
}

static void init_palette_and_luts(void) {
    if (s_luts_ready) return;
    s_luts_ready = true;
    init_default_palette_entries();
    rebuild_rgb15_and_blend_luts();
}

/* =========================================================================
 * Error & Logging
 * ========================================================================= */
static char s_err_buf[256] = "";

void sense_sdl_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err_buf, sizeof(s_err_buf), fmt, ap);
    va_end(ap);
}

const char *SDL_GetError(void) {
    return s_err_buf;
}

void SDL_Log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_printf("[SENSE: The Game] %s\n", buf);
}

void SDL_LogWarn(int category, const char *fmt, ...) {
    (void)category;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_printf("[SENSE: The Game WARN] %s\n", buf);
}

void SDL_LogError(int category, const char *fmt, ...) {
    (void)category;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_printf("[SENSE: The Game ERR] %s\n", buf);
}

void SDL_LogCritical(int category, const char *fmt, ...) {
    (void)category;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_printf("[SENSE: The Game CRIT] %s\n", buf);
}

void SDL_LogDebug(int category, const char *fmt, ...) {
    (void)category;
    (void)fmt;
}

SDL_bool SDL_SetHint(const char *name, const char *value) {
    (void)name;
    (void)value;
    return SDL_TRUE;
}

/* =========================================================================
 * Compiled Scaled RLE-Span Texture Engine
 * ========================================================================= */
enum {
    RLE_END = 0,
    RLE_SKIP = 1,
    RLE_OPAQUE = 2,
    RLE_ALPHA = 3
};

typedef struct {
    int sx, sy, sw, sh;
    int dw, dh;
    uint8_t *blob;
    uint32_t *row_off;
    uint32_t stamp;
} RLECacheEntry;

#define TEX_RLE_CAP 160

struct SDL_Texture {
    int w, h;
    uint32_t *bgra; /* straight-alpha BGRA8888 (A<<24 | R<<16 | G<<8 | B) */
    SDL_BlendMode blend_mode;
    Uint8 alpha_mod;

    bool is_solid;
    bool is_image;
    uint8_t solid_pal;
    uint8_t solid_a;

    uint32_t refcount;
    struct SDL_Texture *backing_tex;
    struct SDL_Texture *prev;
    struct SDL_Texture *next;
    int last_rle;
    RLECacheEntry cache[TEX_RLE_CAP];
    uint32_t stamp_seq;
};

static SDL_Texture *s_tex_head = NULL;

static inline void unpack_bgra(uint32_t px, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    *b = (uint8_t)(px & 0xFFu);
    *g = (uint8_t)((px >> 8) & 0xFFu);
    *r = (uint8_t)((px >> 16) & 0xFFu);
    *a = (uint8_t)((px >> 24) & 0xFFu);
}

static uint32_t isqrt_u32(uint32_t x) {
    uint32_t res = 0;
    uint32_t bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res ? res : 1u;
}

typedef struct {
    uint8_t r5, g5, b5;
    uint32_t w;
} MCBin;

typedef struct {
    int start;
    int count;
    uint8_t rmin, rmax;
    uint8_t gmin, gmax;
    uint8_t bmin, bmax;
    uint64_t total_w;
    uint64_t score;
} MCBox;

static int mc_cmp_r(const void *a, const void *b) {
    return (int)((const MCBin *)a)->r5 - (int)((const MCBin *)b)->r5;
}
static int mc_cmp_g(const void *a, const void *b) {
    return (int)((const MCBin *)a)->g5 - (int)((const MCBin *)b)->g5;
}
static int mc_cmp_b(const void *a, const void *b) {
    return (int)((const MCBin *)a)->b5 - (int)((const MCBin *)b)->b5;
}

static void mc_box_fit(MCBox *box, const MCBin *bins) {
    uint8_t rmin = 31, rmax = 0, gmin = 31, gmax = 0, bmin = 31, bmax = 0;
    uint64_t tw = 0;
    for (int i = 0; i < box->count; i++) {
        const MCBin *bn = &bins[box->start + i];
        if (bn->r5 < rmin) rmin = bn->r5;
        if (bn->r5 > rmax) rmax = bn->r5;
        if (bn->g5 < gmin) gmin = bn->g5;
        if (bn->g5 > gmax) gmax = bn->g5;
        if (bn->b5 < bmin) bmin = bn->b5;
        if (bn->b5 > bmax) bmax = bn->b5;
        tw += bn->w;
    }
    box->rmin = rmin; box->rmax = rmax;
    box->gmin = gmin; box->gmax = gmax;
    box->bmin = bmin; box->bmax = bmax;
    box->total_w = tw;
    uint32_t sr = (uint32_t)(rmax - rmin) * 3u;
    uint32_t sg = (uint32_t)(gmax - gmin) * 4u;
    uint32_t sb = (uint32_t)(bmax - bmin) * 2u;
    uint32_t max_span = sr;
    if (sg > max_span) max_span = sg;
    if (sb > max_span) max_span = sb;
    box->score = (box->count > 1) ? (tw * (uint64_t)max_span) : 0ULL;
}

static void accumulate_texture_hist(const uint32_t *bgra, int w, int h) {
    size_t npx = (size_t)w * (size_t)h;
    size_t step = (npx > 16384u) ? (npx / 16384u) : 1u;
    uint32_t w_base = (npx <= 4096u) ? 4u : 1u;
    uint32_t new_bins = 0;

    for (size_t i = 0; i < npx; i += step) {
        uint8_t r, g, b, a;
        unpack_bgra(bgra[i], &r, &g, &b, &a);
        if (a < 16) continue;
        uint32_t key = ((uint32_t)(r >> 3) << 10) | ((uint32_t)(g >> 3) << 5) | (uint32_t)(b >> 3);
        if (s_hist_rgb15[key] == 0) new_bins++;
        s_hist_rgb15[key] += w_base;

        /* Include 50% darkened shade so black overlays keep true hues rather than turning gray */
        uint32_t key_dk = ((uint32_t)(r >> 4) << 10) | ((uint32_t)(g >> 4) << 5) | (uint32_t)(b >> 4);
        if (s_hist_rgb15[key_dk] == 0) new_bins++;
        s_hist_rgb15[key_dk] += w_base;
    }

    if (!s_adaptive_active || new_bins >= 8u) {
        s_hist_dirty = true;
    }
}

static void invalidate_all_tex_rles(void) {
    for (SDL_Texture *t = s_tex_head; t; t = t->next) {
        for (int i = 0; i < TEX_RLE_CAP; i++) {
            free(t->cache[i].blob);
            free(t->cache[i].row_off);
            t->cache[i].blob = NULL;
            t->cache[i].row_off = NULL;
        }
        t->last_rle = -1;
        t->stamp_seq = 0;
        if (t->is_solid && t->bgra) {
            uint8_t r, g, b, a;
            unpack_bgra(t->bgra[0], &r, &g, &b, &a);
            t->solid_pal = quant_rgb(r, g, b);
        }
    }
}

static void rebuild_adaptive_palette(void) {
    s_hist_dirty = false;

    /* Ensure baseline UI text/meter accent bins remain represented */
    static const uint32_t k_ui_anchors[] = {
        (31u << 10) | (31u << 5) | 31u, /* White */
        (31u << 10) | (26u << 5) | 0u,  /* Gold timer / arrow */
        (0u << 10)  | (25u << 5) | 31u, /* Cyan text */
        (24u << 10) | (4u << 5)  | 4u,  /* Red scale */
        (0u << 10)  | (24u << 5) | 0u   /* Green scale */
    };
    for (size_t i = 0; i < sizeof(k_ui_anchors) / sizeof(k_ui_anchors[0]); i++) {
        if (s_hist_rgb15[k_ui_anchors[i]] < 4u) s_hist_rgb15[k_ui_anchors[i]] = 4u;
    }

    int bin_count = 0;
    for (int i = 0; i < 32768; i++) {
        if (s_hist_rgb15[i] > 0) bin_count++;
    }
    if (bin_count == 0) return;

    MCBin *bins = (MCBin *)malloc((size_t)bin_count * sizeof(MCBin));
    MCBox *boxes = (MCBox *)malloc(PAL_MC_SLOTS * sizeof(MCBox));
    if (!bins || !boxes) {
        free(bins);
        free(boxes);
        return;
    }

    int bi = 0;
    for (int i = 0; i < 32768; i++) {
        uint32_t c = s_hist_rgb15[i];
        if (c == 0) continue;
        bins[bi].r5 = (uint8_t)((i >> 10) & 31);
        bins[bi].g5 = (uint8_t)((i >> 5) & 31);
        bins[bi].b5 = (uint8_t)(i & 31);
        bins[bi].w = isqrt_u32(c);
        bi++;
    }

    int nboxes = 1;
    boxes[0].start = 0;
    boxes[0].count = bin_count;
    mc_box_fit(&boxes[0], bins);

    while (nboxes < PAL_MC_SLOTS) {
        int pick = -1;
        uint64_t best_score = 0;
        for (int i = 0; i < nboxes; i++) {
            if (boxes[i].score > best_score) {
                best_score = boxes[i].score;
                pick = i;
            }
        }
        if (pick < 0) break;

        MCBox *b = &boxes[pick];
        uint32_t sr = (uint32_t)(b->rmax - b->rmin) * 3u;
        uint32_t sg = (uint32_t)(b->gmax - b->gmin) * 4u;
        uint32_t sb = (uint32_t)(b->bmax - b->bmin) * 2u;
        if (sg >= sr && sg >= sb) {
            qsort(&bins[b->start], (size_t)b->count, sizeof(MCBin), mc_cmp_g);
        } else if (sr >= sb) {
            qsort(&bins[b->start], (size_t)b->count, sizeof(MCBin), mc_cmp_r);
        } else {
            qsort(&bins[b->start], (size_t)b->count, sizeof(MCBin), mc_cmp_b);
        }

        uint64_t half = b->total_w / 2u;
        uint64_t acc = 0;
        int split = 1;
        while (split < b->count - 1) {
            acc += bins[b->start + split - 1].w;
            if (acc >= half) break;
            split++;
        }

        MCBox right;
        right.start = b->start + split;
        right.count = b->count - split;
        b->count = split;
        mc_box_fit(b, bins);
        mc_box_fit(&right, bins);
        boxes[nboxes++] = right;
    }

    for (int i = 0; i < PAL_STD_RESERVED; i++) {
        set_pal_dac(i, s_std16_dac[i][0], s_std16_dac[i][1], s_std16_dac[i][2]);
    }
    static const uint8_t k_gray_anchors[PAL_GRAY_RESERVED] = { 4, 8, 15, 23, 31, 40, 49, 57 };
    for (int i = 0; i < PAL_GRAY_RESERVED; i++) {
        uint8_t v = k_gray_anchors[i];
        set_pal_dac(PAL_STD_RESERVED + i, v, v, v);
    }

    for (int i = 0; i < nboxes; i++) {
        const MCBox *bx = &boxes[i];
        uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;
        for (int j = 0; j < bx->count; j++) {
            const MCBin *bn = &bins[bx->start + j];
            int r8 = (bn->r5 << 3) | (bn->r5 >> 2);
            int g8 = (bn->g5 << 3) | (bn->g5 >> 2);
            int b8 = (bn->b5 << 3) | (bn->b5 >> 2);
            sum_r += (uint64_t)r8 * bn->w;
            sum_g += (uint64_t)g8 * bn->w;
            sum_b += (uint64_t)b8 * bn->w;
            sum_w += bn->w;
        }
        int ar = sum_w ? (int)((sum_r + sum_w / 2u) / sum_w) : 0;
        int ag = sum_w ? (int)((sum_g + sum_w / 2u) / sum_w) : 0;
        int ab = sum_w ? (int)((sum_b + sum_w / 2u) / sum_w) : 0;
        set_pal_dac(PAL_MC_START + i, u8_to_dac6(ar), u8_to_dac6(ag), u8_to_dac6(ab));
    }
    for (int i = PAL_MC_START + nboxes; i < PAL_COUNT; i++) {
        set_pal_dac(i, 0, 0, 0);
    }

    free(bins);
    free(boxes);

    s_active_pal_count = PAL_COUNT;
    s_adaptive_active = true;
    s_palette_applied = true;
    rebuild_rgb15_and_blend_luts();
    invalidate_all_tex_rles();
    apply_sense_dac_palette(false);
}

static SDL_Texture *create_texture_from_bgra_ex(SDL_Renderer *renderer, int w, int h,
                                                void *bgra_pixels, SDL_bool is_premultiplied,
                                                bool is_image) {
    (void)renderer;
    init_palette_and_luts();
    if (!bgra_pixels || w <= 0 || h <= 0) return NULL;

    uint32_t *src = (uint32_t *)bgra_pixels;
    size_t npx = (size_t)w * (size_t)h;

    if (is_premultiplied) {
        for (size_t i = 0; i < npx; i++) {
            uint32_t px = src[i];
            uint8_t a = (uint8_t)(px >> 24);
            if (a > 0 && a < 255) {
                uint32_t b = ((px & 0xFFu) * 255u) / a; if (b > 255u) b = 255u;
                uint32_t g = (((px >> 8) & 0xFFu) * 255u) / a; if (g > 255u) g = 255u;
                uint32_t r = (((px >> 16) & 0xFFu) * 255u) / a; if (r > 255u) r = 255u;
                src[i] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    if (is_image) {
        accumulate_texture_hist(src, w, h);
    }

    SDL_Texture *tex = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!tex) { free(src); return NULL; }
    tex->w = w;
    tex->h = h;
    tex->bgra = src;
    tex->blend_mode = SDL_BLENDMODE_BLEND;
    tex->alpha_mod = 255;
    tex->is_image = is_image;
    tex->refcount = 1;
    tex->last_rle = -1;
    tex->next = s_tex_head;
    if (s_tex_head) s_tex_head->prev = tex;
    s_tex_head = tex;

    uint32_t first = src[0];
    bool uniform = true;
    size_t check_lim = (npx < 4096) ? npx : 4096;
    for (size_t i = 1; i < check_lim; i++) {
        if (src[i] != first) { uniform = false; break; }
    }
    if (uniform && npx <= 4096) {
        uint8_t r, g, b, a;
        unpack_bgra(first, &r, &g, &b, &a);
        tex->is_solid = true;
        tex->solid_pal = quant_rgb(r, g, b);
        tex->solid_a = a;
    }

    return tex;
}

SDL_Texture *sense_sdl_create_texture_from_bgra(SDL_Renderer *renderer, int w, int h,
                                                void *bgra_pixels, SDL_bool is_premultiplied) {
    return create_texture_from_bgra_ex(renderer, w, h, bgra_pixels, is_premultiplied, false);
}

#define TEX_SURF_CACHE_CAP 64

typedef struct {
    SDL_Renderer *ren;
    uint64_t      key;
    SDL_Texture  *tex;
    uint32_t      stamp;
} TexSurfCacheEntry;

static TexSurfCacheEntry s_tex_surf_cache[TEX_SURF_CACHE_CAP];
static uint32_t s_tex_surf_stamp = 0;

static void tex_surf_cache_purge_renderer(SDL_Renderer *ren) {
    for (int i = 0; i < TEX_SURF_CACHE_CAP; i++) {
        if (s_tex_surf_cache[i].tex && s_tex_surf_cache[i].ren == ren) {
            SDL_Texture *t = s_tex_surf_cache[i].tex;
            s_tex_surf_cache[i].tex = NULL;
            s_tex_surf_cache[i].ren = NULL;
            s_tex_surf_cache[i].key = 0;
            SDL_DestroyTexture(t);
        }
    }
}

static SDL_Texture *wrap_surface_texture_alpha(SDL_Texture *tex, const SDL_Surface *surface) {
    Uint8 surf_alpha = (surface->has_alpha_mod) ? surface->alpha_mod : 255;
    if (!tex || surf_alpha == 255) return tex;
    SDL_Texture *alias = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!alias) {
        tex->alpha_mod = surf_alpha;
        return tex;
    }
    alias->w = tex->w;
    alias->h = tex->h;
    alias->blend_mode = tex->blend_mode;
    alias->alpha_mod = surf_alpha;
    alias->refcount = 1;
    alias->backing_tex = tex;
    return alias;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface) {
    if (!renderer || !surface || !surface->pixels || surface->w <= 0 || surface->h <= 0) {
        return NULL;
    }

    int lru_idx = -1;
    if (surface->cache_key != 0) {
        uint32_t lru_stamp = 0xFFFFFFFFu;
        for (int i = 0; i < TEX_SURF_CACHE_CAP; i++) {
            TexSurfCacheEntry *e = &s_tex_surf_cache[i];
            if (e->tex && e->ren == renderer && e->key == surface->cache_key) {
                e->stamp = ++s_tex_surf_stamp;
                e->tex->refcount++;
                return wrap_surface_texture_alpha(e->tex, surface);
            }
            if (!e->tex) {
                lru_idx = i;
                break;
            }
            if (e->stamp < lru_stamp) {
                lru_stamp = e->stamp;
                lru_idx = i;
            }
        }
    }

    size_t sz = (size_t)surface->w * (size_t)surface->h * 4u;
    uint32_t *copy = (uint32_t *)malloc(sz);
    if (!copy) return NULL;
    memcpy(copy, surface->pixels, sz);
    SDL_Texture *tex = sense_sdl_create_texture_from_bgra(renderer, surface->w, surface->h,
                                                          copy, surface->is_premultiplied);
    if (tex && surface->cache_key != 0 && lru_idx >= 0) {
        TexSurfCacheEntry *slot = &s_tex_surf_cache[lru_idx];
        if (slot->tex) {
            SDL_Texture *old = slot->tex;
            slot->tex = NULL;
            SDL_DestroyTexture(old);
        }
        slot->ren = renderer;
        slot->key = surface->cache_key;
        slot->tex = tex;
        slot->stamp = ++s_tex_surf_stamp;
        tex->refcount++;
    }
    return wrap_surface_texture_alpha(tex, surface);
}

void SDL_DestroyTexture(SDL_Texture *texture) {
    if (!texture) return;
    if (texture->refcount > 0 && --texture->refcount > 0) return;
    if (texture->backing_tex) {
        SDL_Texture *b = texture->backing_tex;
        texture->backing_tex = NULL;
        SDL_DestroyTexture(b);
        free(texture);
        return;
    }
    if (texture->prev) texture->prev->next = texture->next;
    else if (s_tex_head == texture) s_tex_head = texture->next;
    if (texture->next) texture->next->prev = texture->prev;
    for (int i = 0; i < TEX_RLE_CAP; i++) {
        free(texture->cache[i].blob);
        free(texture->cache[i].row_off);
    }
    free(texture->bgra);
    free(texture);
}

int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h) {
    if (!texture) return -1;
    if (format) *format = 0;
    if (access) *access = 0;
    if (w) *w = texture->w;
    if (h) *h = texture->h;
    return 0;
}

int SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode) {
    if (!texture) return -1;
    texture->blend_mode = blendMode;
    return 0;
}

int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha) {
    if (!texture) return -1;
    texture->alpha_mod = alpha;
    return 0;
}

/* =========================================================================
 * RLE Span Compiler & Cache Lookup
 * ========================================================================= */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} RLEBuilder;

static bool rb_ensure(RLEBuilder *rb, size_t extra) {
    if (rb->len + extra <= rb->cap) return true;
    size_t ncap = rb->cap ? rb->cap * 2 : 4096;
    while (ncap < rb->len + extra) ncap *= 2;
    uint8_t *nb = (uint8_t *)realloc(rb->buf, ncap);
    if (!nb) return false;
    rb->buf = nb;
    rb->cap = ncap;
    return true;
}

static void rb_push_u8(RLEBuilder *rb, uint8_t v) {
    if (rb_ensure(rb, 1)) rb->buf[rb->len++] = v;
}

static void rb_push_u16(RLEBuilder *rb, uint16_t v) {
    if (rb_ensure(rb, 2)) {
        rb->buf[rb->len++] = (uint8_t)(v & 0xFFu);
        rb->buf[rb->len++] = (uint8_t)(v >> 8);
    }
}

static const RLECacheEntry *tex_get_rle(SDL_Texture *tex,
                                        int sx, int sy, int sw, int sh,
                                        int dw, int dh) {
    int last = tex->last_rle;
    if (last >= 0 && last < TEX_RLE_CAP) {
        RLECacheEntry *e = &tex->cache[last];
        if (e->blob && e->sx == sx && e->sy == sy && e->sw == sw && e->sh == sh &&
            e->dw == dw && e->dh == dh) {
            e->stamp = ++tex->stamp_seq;
            return e;
        }
    }

    uint32_t oldest_idx = 0;
    uint32_t oldest_stamp = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < TEX_RLE_CAP; i++) {
        RLECacheEntry *e = &tex->cache[i];
        if (e->blob && e->sx == sx && e->sy == sy && e->sw == sw && e->sh == sh &&
            e->dw == dw && e->dh == dh) {
            e->stamp = ++tex->stamp_seq;
            tex->last_rle = (int)i;
            return e;
        }
        if (e->stamp < oldest_stamp) {
            oldest_stamp = e->stamp;
            oldest_idx = i;
        }
    }

    RLECacheEntry *e = &tex->cache[oldest_idx];
    tex->last_rle = (int)oldest_idx;
    free(e->blob);
    free(e->row_off);
    memset(e, 0, sizeof(*e));
    e->sx = sx; e->sy = sy; e->sw = sw; e->sh = sh;
    e->dw = dw; e->dh = dh;
    e->stamp = ++tex->stamp_seq;

    uint32_t *row_off = (uint32_t *)malloc((size_t)dh * sizeof(uint32_t));
    int *xlut = (int *)malloc((size_t)dw * sizeof(int));
    uint8_t *row_kind = (uint8_t *)malloc((size_t)dw);
    uint8_t *row_lvl  = (uint8_t *)malloc((size_t)dw);
    uint8_t *row_pal  = (uint8_t *)malloc((size_t)dw);
    if (!row_off || !xlut || !row_kind || !row_lvl || !row_pal) {
        free(row_off); free(xlut); free(row_kind); free(row_lvl); free(row_pal);
        return NULL;
    }

    for (int dx = 0; dx < dw; dx++) {
        xlut[dx] = sx + (dx * sw) / dw;
    }

    RLEBuilder rb = { NULL, 0, 0 };
    rb_ensure(&rb, (size_t)dw * (size_t)dh / 2u + 1024u);

    for (int dy = 0; dy < dh; dy++) {
        row_off[dy] = (uint32_t)rb.len;
        int isy = sy + (dy * sh) / dh;
        const uint32_t *srow = tex->bgra + (size_t)isy * (size_t)tex->w;

        for (int dx = 0; dx < dw; dx++) {
            uint32_t px = srow[xlut[dx]];
            uint8_t r, g, b, a;
            unpack_bgra(px, &r, &g, &b, &a);
            if (tex->blend_mode == SDL_BLENDMODE_NONE) a = 255;

            uint8_t q = (uint8_t)(((unsigned)a + 16u) >> 5);
            if (q == 0) {
                row_kind[dx] = RLE_SKIP;
            } else if (q >= 8) {
                row_kind[dx] = RLE_OPAQUE;
                row_pal[dx]  = quant_rgb_dither(r, g, b, dx, dy);
            } else {
                row_kind[dx] = RLE_ALPHA;
                row_lvl[dx]  = (uint8_t)(q - 1u);
                row_pal[dx]  = quant_rgb_dither(r, g, b, dx, dy);
            }
        }

        int dx = 0;
        while (dx < dw) {
            uint8_t k = row_kind[dx];
            int run = 1;
            while (dx + run < dw && row_kind[dx + run] == k && run < 65535) {
                run++;
            }
            if (k == RLE_SKIP) {
                rb_push_u8(&rb, RLE_SKIP);
                rb_push_u16(&rb, (uint16_t)run);
            } else if (k == RLE_OPAQUE) {
                rb_push_u8(&rb, RLE_OPAQUE);
                rb_push_u16(&rb, (uint16_t)run);
                if (rb_ensure(&rb, (size_t)run)) {
                    memcpy(rb.buf + rb.len, row_pal + dx, (size_t)run);
                    rb.len += (size_t)run;
                }
            } else {
                rb_push_u8(&rb, RLE_ALPHA);
                rb_push_u16(&rb, (uint16_t)run);
                if (rb_ensure(&rb, (size_t)run * 2u)) {
                    uint8_t *dstp = rb.buf + rb.len;
                    for (int j = 0; j < run; j++) {
                        dstp[j * 2 + 0] = row_lvl[dx + j];
                        dstp[j * 2 + 1] = row_pal[dx + j];
                    }
                    rb.len += (size_t)run * 2u;
                }
            }
            dx += run;
        }
        rb_push_u8(&rb, RLE_END);
    }

    free(xlut);
    free(row_kind);
    free(row_lvl);
    free(row_pal);

    e->blob = rb.buf;
    e->row_off = row_off;
    return e;
}

/* =========================================================================
 * Window & Renderer State
 * ========================================================================= */
struct SDL_Window {
    wm_window_t *wm_win;
    bool direct_gfx;
    bool hw_direct_fb;
    bool fast_2x_scale;
    bool fullscreen;
    bool closed;
    int screen_w, screen_h;
    int win_w, win_h;
    int canvas_w, canvas_h;
};

struct SDL_Renderer {
    SDL_Window *window;
    int fb_w, fb_h;
    uint8_t *fb8;
    uint8_t *present_buf8;
    int present_w, present_h;
    int logical_w, logical_h;
    Uint8 draw_r, draw_g, draw_b, draw_a;
    SDL_BlendMode draw_blend;

    uint32_t *wm_xlut;
    int wm_xlut_w;
    int wm_xlut_src_w;

    uint32_t fps_frames;
    uint32_t fps_last_ms;
    bool frame_timer_init;
    uint32_t frame_target_ms;
};

static SDL_Window   *s_active_win = NULL;
static SDL_Renderer *s_active_ren = NULL;
static uint32_t      s_ev_kb = 0, s_ev_gp = 0, s_ev_tp = 0;

static void sdl_push_event(const SDL_Event *ev);

static void sync_window_dimensions(SDL_Window *win, bool emit_resize) {
    if (!win) return;
    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320 || sw > 4096) sw = 320;
    if (sh < 200 || sh > 4096) sh = 200;
    win->screen_w = sw;
    win->screen_h = sh;

    int new_w = sw, new_h = sh;
    win->fast_2x_scale = false;
    if (win->wm_win && wm_is_window_valid(win->wm_win)) {
        if (win->fullscreen) {
            win->hw_direct_fb = (sw > (int)win->wm_win->w || sh > (int)win->wm_win->h);
            if (win->hw_direct_fb) {
                if (sw >= 960 && (sw & 1) == 0 && (sh & 1) == 0) {
                    new_w = sw / 2;
                    new_h = sh / 2;
                    win->fast_2x_scale = true;
                } else {
                    new_w = sw;
                    new_h = sh;
                }
            } else {
                new_w = (int)win->wm_win->w;
                new_h = (int)win->wm_win->h;
            }
        } else {
            win->hw_direct_fb = false;
            int ww = (int)win->wm_win->w;
            int wh = (int)win->wm_win->h;
            if (ww < 16) ww = 16;
            if (wh < 16) wh = 16;
            if (sw >= 640 && ww < 640) {
                new_w = 640;
                new_h = (640 * wh) / ww;
                if (new_h < 200) new_h = 200;
            } else {
                new_w = ww;
                new_h = wh;
            }
        }
    }

    bool changed = (new_w != win->canvas_w || new_h != win->canvas_h);
    win->canvas_w = new_w;
    win->canvas_h = new_h;

    if (s_active_ren && s_active_ren->window == win &&
        (s_active_ren->fb_w != new_w || s_active_ren->fb_h != new_h)) {
        uint8_t *nfb = (uint8_t *)calloc((size_t)new_w * (size_t)new_h, 1);
        if (nfb) {
            free(s_active_ren->fb8);
            s_active_ren->fb8 = nfb;
            s_active_ren->fb_w = new_w;
            s_active_ren->fb_h = new_h;
        }
    }

    if (emit_resize && changed) {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_WINDOWEVENT;
        ev.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        ev.window.windowID = 1;
        ev.window.data1 = new_w;
        ev.window.data2 = new_h;
        sdl_push_event(&ev);
    }
}

int SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect) {
    (void)displayIndex;
    if (!rect) return -1;
    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320 || sw > 4096) sw = 320;
    if (sh < 200 || sh > 4096) sh = 200;
    rect->x = 0;
    rect->y = 0;
    rect->w = sw;
    rect->h = sh;
    return 0;
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x; (void)y; (void)w; (void)h;
    init_palette_and_luts();

    SDL_Window *win = (SDL_Window *)calloc(1, sizeof(SDL_Window));
    if (!win) return NULL;

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320 || sw > 4096) sw = 320;
    if (sh < 200 || sh > 4096) sh = 200;

    win->fullscreen = ((flags & SDL_WINDOW_FULLSCREEN) != 0);

    {
        wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
        opt.title = title ? title : "SENSE: The Game";
        opt.decor_style = WM_DECOR_DEFAULT;
        opt.bg_color = 0;
        opt.w = 272;
        opt.h = 154;
        opt.x = 24;
        opt.y = 14;
        win->wm_win = wm_create_window(&opt);
    }

    if (win->wm_win) {
        if (win->fullscreen) {
            wm_set_fullscreen(win->wm_win, true);
            wm_set_cursor_visible(win->wm_win, false);
        } else {
            wm_set_fullscreen(win->wm_win, false);
            wm_set_cursor_visible(win->wm_win, true);
        }
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        win->direct_gfx = true;
    }

    sync_window_dimensions(win, false);
    s_active_win = win;
    return win;
}

void SDL_DestroyWindow(SDL_Window *window) {
    if (!window) return;
    apply_sense_dac_palette(true);
    if (s_active_win == window) s_active_win = NULL;
    if (window->wm_win) {
        if (wm_is_window_valid(window->wm_win)) {
            wm_set_cursor_visible(window->wm_win, true);
            wm_destroy_window(window->wm_win);
        }
        window->wm_win = NULL;
    } else if (window->direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    free(window);
}

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) {
    if (!window) return -1;
    bool want_fs = ((flags & SDL_WINDOW_FULLSCREEN) != 0);
    window->fullscreen = want_fs;
    if (window->wm_win && wm_is_window_valid(window->wm_win)) {
        wm_set_fullscreen(window->wm_win, want_fs);
        wm_set_cursor_visible(window->wm_win, !want_fs);
        if (!want_fs) {
            wm_invalidate_all();
        }
        wm_compositor_tick();
    }
    if (s_palette_applied) {
        apply_sense_dac_palette(false);
    }
    sync_window_dimensions(window, true);
    serial_printf("[SENSE: The Game] mode -> %s (%dx%d hw_direct=%d wm=%p)\n",
                  want_fs ? "fullscreen" : "windowed",
                  s_active_ren ? s_active_ren->fb_w : 0,
                  s_active_ren ? s_active_ren->fb_h : 0,
                  window->hw_direct_fb ? 1 : 0,
                  (void *)window->wm_win);
    return 0;
}

void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    (void)window;
    (void)icon;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h) {
    if (!window) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = window->canvas_w;
    if (h) *h = window->canvas_h;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags) {
    (void)index; (void)flags;
    if (!window) return NULL;
    SDL_Renderer *ren = (SDL_Renderer *)calloc(1, sizeof(SDL_Renderer));
    if (!ren) return NULL;
    ren->window = window;
    ren->fb_w = window->canvas_w > 0 ? window->canvas_w : 320;
    ren->fb_h = window->canvas_h > 0 ? window->canvas_h : 200;
    ren->fb8 = (uint8_t *)calloc((size_t)ren->fb_w * (size_t)ren->fb_h, 1);
    if (!ren->fb8) { free(ren); return NULL; }
    ren->fps_last_ms = timer_millis();
    s_active_ren = ren;
    return ren;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {
    if (!renderer) return;
    tex_surf_cache_purge_renderer(renderer);
    if (s_active_ren == renderer) s_active_ren = NULL;
    free(renderer->wm_xlut);
    free(renderer->present_buf8);
    free(renderer->fb8);
    free(renderer);
}

int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) {
    if (!renderer) return -1;
    renderer->logical_w = w;
    renderer->logical_h = h;
    return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!renderer) return -1;
    renderer->draw_r = r;
    renderer->draw_g = g;
    renderer->draw_b = b;
    renderer->draw_a = a;
    return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode) {
    if (!renderer) return -1;
    renderer->draw_blend = blendMode;
    return 0;
}

static void map_dst_rect(const SDL_Renderer *ren, const SDL_Rect *in_rect,
                         int *ox, int *oy, int *ow, int *oh) {
    int rw = in_rect ? in_rect->w : (ren->logical_w > 0 ? ren->logical_w : ren->fb_w);
    int rh = in_rect ? in_rect->h : (ren->logical_h > 0 ? ren->logical_h : ren->fb_h);
    int rx = in_rect ? in_rect->x : 0;
    int ry = in_rect ? in_rect->y : 0;

    if (ren->logical_w > 0 && ren->logical_h > 0 &&
        (ren->logical_w != ren->fb_w || ren->logical_h != ren->fb_h)) {
        float sx = (float)ren->fb_w / (float)ren->logical_w;
        float sy = (float)ren->fb_h / (float)ren->logical_h;
        float s = (sx < sy) ? sx : sy;
        int vw = (int)floorf((float)ren->logical_w * s + 0.5f);
        int vh = (int)floorf((float)ren->logical_h * s + 0.5f);
        int vx = (ren->fb_w - vw) / 2;
        int vy = (ren->fb_h - vh) / 2;
        *ox = vx + (int)floorf((float)rx * s + 0.5f);
        *oy = vy + (int)floorf((float)ry * s + 0.5f);
        *ow = (int)floorf((float)rw * s + 0.5f);
        *oh = (int)floorf((float)rh * s + 0.5f);
        if (*ow < 1 && rw > 0) *ow = 1;
        if (*oh < 1 && rh > 0) *oh = 1;
    } else {
        *ox = rx;
        *oy = ry;
        *ow = rw;
        *oh = rh;
    }
}

static void fill_solid_pal_rect(SDL_Renderer *ren, int x, int y, int w, int h,
                                uint8_t pal, uint8_t alpha) {
    if (!ren || !ren->fb8 || w <= 0 || h <= 0) return;
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w; if (x1 > ren->fb_w) x1 = ren->fb_w;
    int y1 = y + h; if (y1 > ren->fb_h) y1 = ren->fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    int cw = x1 - x0;
    uint8_t q = (uint8_t)(((unsigned)alpha + 16u) >> 5);
    if (q == 0) return;

    if (q >= 8) {
        for (int dy = y0; dy < y1; dy++) {
            memset(ren->fb8 + (size_t)dy * (size_t)ren->fb_w + x0, pal, (size_t)cw);
        }
        return;
    }

    const uint8_t *bl = s_blend[q - 1][pal];
    for (int dy = y0; dy < y1; dy++) {
        uint8_t *dst = ren->fb8 + (size_t)dy * (size_t)ren->fb_w + x0;
        int cx = 0;
        while (cx < cw && (((uintptr_t)(dst + cx)) & 3u) != 0u) {
            dst[cx] = bl[dst[cx]];
            cx++;
        }
        uint32_t *d4 = (uint32_t *)(dst + cx);
        int cw4 = (cw - cx) >> 2;
        for (int i = 0; i < cw4; i++) {
            uint32_t v = d4[i];
            d4[i] = (uint32_t)bl[v & 0xFFu] |
                    ((uint32_t)bl[(v >> 8) & 0xFFu] << 8) |
                    ((uint32_t)bl[(v >> 16) & 0xFFu] << 16) |
                    ((uint32_t)bl[v >> 24] << 24);
        }
        cx += cw4 << 2;
        while (cx < cw) {
            dst[cx] = bl[dst[cx]];
            cx++;
        }
    }
}

int SDL_RenderClear(SDL_Renderer *renderer) {
    if (!renderer || !renderer->fb8) return -1;
    uint8_t pal = quant_rgb(renderer->draw_r, renderer->draw_g, renderer->draw_b);
    memset(renderer->fb8, pal, (size_t)renderer->fb_w * (size_t)renderer->fb_h);
    return 0;
}

int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->fb8) return -1;
    int ox, oy, ow, oh;
    map_dst_rect(renderer, rect, &ox, &oy, &ow, &oh);
    uint8_t pal = quant_rgb(renderer->draw_r, renderer->draw_g, renderer->draw_b);
    uint8_t alpha = (renderer->draw_blend == SDL_BLENDMODE_NONE) ? 255 : renderer->draw_a;
    fill_solid_pal_rect(renderer, ox, oy, ow, oh, pal, alpha);
    return 0;
}

static inline void fast_copy_u8(uint8_t *dst, const uint8_t *src, uint16_t cnt) {
    if (cnt >= 32u) {
        size_t dwords = cnt >> 2;
        size_t tail = cnt & 3u;
        __asm__ volatile(
            "cld\n\t"
            "rep movsl\n\t"
            : "+D"(dst), "+S"(src), "+c"(dwords)
            :
            : "memory");
        while (tail--) *dst++ = *src++;
        return;
    }
    while (cnt >= 4u) {
        *(uint32_t *)dst = *(const uint32_t *)src;
        dst += 4;
        src += 4;
        cnt -= 4u;
    }
    while (cnt--) {
        *dst++ = *src++;
    }
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
    if (!renderer || !renderer->fb8 || !texture) return -1;
    SDL_Texture *src_tex = texture->backing_tex ? texture->backing_tex : texture;
    if (!src_tex->bgra) return -1;

    if (src_tex->is_image && s_hist_dirty) {
        rebuild_adaptive_palette();
    }

    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : src_tex->w;
    int sh = srcrect ? srcrect->h : src_tex->h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > src_tex->w) sw = src_tex->w - sx;
    if (sy + sh > src_tex->h) sh = src_tex->h - sy;
    if (sw <= 0 || sh <= 0) return 0;

    int ox, oy, ow, oh;
    map_dst_rect(renderer, dstrect, &ox, &oy, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 0;
    if (ox >= renderer->fb_w || oy >= renderer->fb_h || ox + ow <= 0 || oy + oh <= 0) return 0;

    if (src_tex->is_solid) {
        uint8_t base_a = (texture->blend_mode == SDL_BLENDMODE_NONE) ? 255 : src_tex->solid_a;
        uint8_t eff_a = (uint8_t)(((unsigned)base_a * texture->alpha_mod) / 255u);
        fill_solid_pal_rect(renderer, ox, oy, ow, oh, src_tex->solid_pal, eff_a);
        return 0;
    }

    const RLECacheEntry *rle = tex_get_rle(src_tex, sx, sy, sw, sh, ow, oh);
    if (!rle || !rle->blob) return -1;

    int sy0 = (oy < 0) ? -oy : 0;
    int sy1 = oh;
    if (oy + sy1 > renderer->fb_h) sy1 = renderer->fb_h - oy;

    Uint8 amod = (texture->blend_mode == SDL_BLENDMODE_NONE) ? 255 : texture->alpha_mod;
    if (amod == 0) return 0;

    bool unclipped_x = (ox >= 0 && ox + ow <= renderer->fb_w);
    uint8_t mod_q = (uint8_t)(((unsigned)amod + 16u) >> 5); /* 0..8 */
    if (mod_q == 0) return 0;

    for (int sdy = sy0; sdy < sy1; sdy++) {
        uint8_t *dst = renderer->fb8 + (size_t)(oy + sdy) * (size_t)renderer->fb_w;
        const uint8_t *p = rle->blob + rle->row_off[sdy];
        int x = ox;

        if (unclipped_x && mod_q >= 8) {
            uint8_t *dp = dst + x;
            for (;;) {
                uint8_t op = *p++;
                if (op == RLE_END) break;
                uint16_t cnt = *(const uint16_t *)p;
                p += 2;
                if (op == RLE_SKIP) {
                    dp += cnt;
                } else if (op == RLE_OPAQUE) {
                    fast_copy_u8(dp, p, cnt);
                    p += cnt;
                    dp += cnt;
                } else {
                    uint16_t j = cnt;
                    while (j >= 4u) {
                        dp[0] = s_blend[p[0]][p[1]][dp[0]];
                        dp[1] = s_blend[p[2]][p[3]][dp[1]];
                        dp[2] = s_blend[p[4]][p[5]][dp[2]];
                        dp[3] = s_blend[p[6]][p[7]][dp[3]];
                        dp += 4;
                        p += 8;
                        j -= 4u;
                    }
                    while (j--) {
                        *dp = s_blend[p[0]][p[1]][*dp];
                        dp++;
                        p += 2;
                    }
                }
            }
        } else {
            for (;;) {
                uint8_t op = *p++;
                if (op == RLE_END) break;
                uint16_t cnt = (uint16_t)(p[0] | (p[1] << 8));
                p += 2;
                if (op == RLE_SKIP) {
                    x += cnt;
                    continue;
                }
                int x_end = x + (int)cnt;
                int c0 = (x < 0) ? 0 : x;
                int c1 = (x_end > renderer->fb_w) ? renderer->fb_w : x_end;
                if (op == RLE_OPAQUE) {
                    if (c0 < c1) {
                        if (mod_q >= 8) {
                            memcpy(dst + c0, p + (c0 - x), (size_t)(c1 - c0));
                        } else {
                            const uint8_t (*btbl)[256] = s_blend[mod_q - 1];
                            for (int cx = c0; cx < c1; cx++) {
                                uint8_t src_pal = p[cx - x];
                                dst[cx] = btbl[src_pal][dst[cx]];
                            }
                        }
                    }
                    p += cnt;
                } else {
                    if (c0 < c1) {
                        for (int cx = c0; cx < c1; cx++) {
                            const uint8_t *ep = p + (size_t)(cx - x) * 2u;
                            uint8_t lvl = ep[0];
                            if (mod_q < 8) {
                                int ql = (((int)lvl + 1) * (int)mod_q + 4) >> 3;
                                if (ql <= 0) continue;
                                lvl = (uint8_t)(ql - 1);
                            }
                            dst[cx] = s_blend[lvl][ep[1]][dst[cx]];
                        }
                    }
                    p += (size_t)cnt * 2u;
                }
                x = x_end;
            }
        }
    }

    return 0;
}

static void blit_to_wm_window(SDL_Renderer *ren, wm_window_t *wm) {
    if (!wm || !wm->framebuf || !ren->fb8) return;
    int ww = (int)wm->w;
    int wh = (int)wm->h;
    int gw = ren->fb_w;
    int gh = ren->fb_h;
    if (ww <= 0 || wh <= 0 || gw <= 0 || gh <= 0) return;

    if (ww == gw && wh == gh) {
        memcpy(wm->framebuf, ren->fb8, (size_t)ww * (size_t)wh);
        return;
    }

    if (!ren->wm_xlut || ren->wm_xlut_w != ww || ren->wm_xlut_src_w != gw) {
        uint32_t *nl = (uint32_t *)realloc(ren->wm_xlut, (size_t)ww * sizeof(uint32_t));
        if (nl) {
            ren->wm_xlut = nl;
            ren->wm_xlut_w = ww;
            ren->wm_xlut_src_w = gw;
            for (int x = 0; x < ww; x++) {
                ren->wm_xlut[x] = (uint32_t)((x * gw) / ww);
            }
        }
    }

    int prev_sy = -1;
    uint8_t *prev_dst = NULL;
    for (int y = 0; y < wh; y++) {
        int sy = (y * gh) / wh;
        uint8_t *dst = wm->framebuf + (size_t)y * (size_t)ww;
        if (sy == prev_sy && prev_dst) {
            memcpy(dst, prev_dst, (size_t)ww);
            continue;
        }
        const uint8_t *src = ren->fb8 + (size_t)sy * (size_t)gw;
        if (ren->wm_xlut && ren->wm_xlut_w == ww) {
            for (int x = 0; x < ww; x++) dst[x] = src[ren->wm_xlut[x]];
        } else {
            for (int x = 0; x < ww; x++) dst[x] = src[(x * gw) / ww];
        }
        prev_sy = sy;
        prev_dst = dst;
    }
}

static void flip_fast_2x(SDL_Renderer *ren, int sw, int sh) {
    int dw = ren->fb_w * 2;
    int dh = ren->fb_h * 2;
    if (dw != sw || dh != sh) {
        vga_gfx_flip_scaled_buffer(ren->fb8, ren->fb_w, ren->fb_h);
        return;
    }
    if (!ren->present_buf8 || ren->present_w != dw || ren->present_h != dh) {
        uint8_t *nb = (uint8_t *)realloc(ren->present_buf8, (size_t)dw * (size_t)dh);
        if (!nb) {
            vga_gfx_flip_scaled_buffer(ren->fb8, ren->fb_w, ren->fb_h);
            return;
        }
        ren->present_buf8 = nb;
        ren->present_w = dw;
        ren->present_h = dh;
    }

    static bool s_dup_init = false;
    static uint16_t s_dup16[256];
    if (!s_dup_init) {
        for (int i = 0; i < 256; i++) s_dup16[i] = (uint16_t)(i | (i << 8));
        s_dup_init = true;
    }

    int fw = ren->fb_w;
    int fh = ren->fb_h;
    const uint8_t *src = ren->fb8;
    uint8_t *dst_base = ren->present_buf8;
    for (int y = 0; y < fh; y++) {
        const uint8_t *srow = src + (size_t)y * (size_t)fw;
        uint8_t *d0 = dst_base + (size_t)(y * 2) * (size_t)dw;
        uint8_t *d1 = d0 + dw;
        uint32_t *d0_32 = (uint32_t *)d0;
        int pairs = fw >> 1;
        for (int x = 0; x < pairs; x++) {
            uint16_t a = s_dup16[srow[x * 2]];
            uint16_t b = s_dup16[srow[x * 2 + 1]];
            d0_32[x] = (uint32_t)a | ((uint32_t)b << 16);
        }
        if (fw & 1) {
            uint16_t a = s_dup16[srow[fw - 1]];
            *(uint16_t *)(d0 + (fw - 1) * 2) = a;
        }
        memcpy(d1, d0, (size_t)dw);
    }
    vga_gfx_flip_scaled_buffer(dst_base, dw, dh);
}

void SDL_RenderPresent(SDL_Renderer *renderer) {
    if (!renderer || !renderer->window || !renderer->fb8) return;
    SDL_Window *win = renderer->window;
    if (win->wm_win && wm_is_window_valid(win->wm_win)) {
        if (win->hw_direct_fb && wm_get_focused() == win->wm_win) {
            if (win->fast_2x_scale) {
                flip_fast_2x(renderer, win->screen_w, win->screen_h);
            } else {
                vga_gfx_flip_scaled_buffer(renderer->fb8, renderer->fb_w, renderer->fb_h);
            }
        } else {
            blit_to_wm_window(renderer, win->wm_win);
            wm_invalidate(win->wm_win);
            wm_compositor_tick();
        }
    } else if (win->direct_gfx) {
        vga_gfx_flip_scaled_buffer(renderer->fb8, renderer->fb_w, renderer->fb_h);
    }

    renderer->fps_frames++;
    uint32_t now = timer_millis();
    uint32_t dt = now - renderer->fps_last_ms;
    if (dt >= 1000u) {
        uint32_t fps = (renderer->fps_frames * 1000u) / dt;
        serial_printf("[SENSE: The Game] fps=%u (%dx%d %s) t=%u kb=%u gp=%u tp=%u\n",
                      (unsigned)fps, renderer->fb_w, renderer->fb_h,
                      win->fullscreen ? "fullscreen" : "windowed",
                      (unsigned)now, (unsigned)s_ev_kb, (unsigned)s_ev_gp, (unsigned)s_ev_tp);
        s_ev_kb = s_ev_gp = s_ev_tp = 0;
        renderer->fps_frames = 0;
        renderer->fps_last_ms = now;
    }
}

/* =========================================================================
 * Timing
 * ========================================================================= */
Uint32 SDL_GetTicks(void) {
    return (Uint32)timer_millis();
}

void SDL_Delay(Uint32 ms) {
    if (ms == 0) return;
    if (ms == 200u) {
        serial_printf("[SENSE: The Game] loss_screen t=%u\n", (unsigned)timer_millis());
    }

    /* Adaptive target pacing on the main 60Hz loop call SDL_Delay(16) */
    if (ms == 16 && s_active_ren) {
        uint32_t now = timer_millis();
        if (!s_active_ren->frame_timer_init) {
            s_active_ren->frame_timer_init = true;
            s_active_ren->frame_target_ms = now + 13u;
        } else {
            s_active_ren->frame_target_ms += 13u;
            int32_t ahead = (int32_t)(s_active_ren->frame_target_ms - now);
            if (ahead > 14) s_active_ren->frame_target_ms = now + 13u;
            else if (ahead < -25) s_active_ren->frame_target_ms = now;
        }
        while ((int32_t)(timer_millis() - s_active_ren->frame_target_ms) < 0) {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        }
        return;
    }

    uint32_t end = timer_millis() + ms;
    while ((int32_t)(timer_millis() - end) < 0) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }
}

static uint64_t s_perf_last_ms = 0;
static uint64_t s_perf_last_tsc = 0;
static uint64_t s_perf_last_us = 0;

Uint64 SDL_GetPerformanceCounter(void) {
    uint64_t ms = (uint64_t)timer_millis();
    uint64_t tsc = read_tsc();
    if (ms != s_perf_last_ms) {
        s_perf_last_ms = ms;
        s_perf_last_tsc = tsc;
    }
    uint64_t sub_us = (tsc - s_perf_last_tsc) / 1000ULL;
    if (sub_us > 999ULL) sub_us = 999ULL;
    uint64_t us = ms * 1000ULL + sub_us;
    if (us < s_perf_last_us) us = s_perf_last_us;
    s_perf_last_us = us;
    return us;
}

Uint64 SDL_GetPerformanceFrequency(void) {
    return 1000000ULL;
}

/* =========================================================================
 * RWops & Image / BMP Loading
 * ========================================================================= */
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size) {
    if (!mem || size <= 0) return NULL;
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!rw) return NULL;
    rw->data = (const uint8_t *)mem;
    rw->size = (size_t)size;
    rw->own_mem = SDL_FALSE;
    return rw;
}

void SDL_FreeRW(SDL_RWops *area) {
    if (!area) return;
    if (area->own_mem && area->data) free((void *)(uintptr_t)area->data);
    free(area);
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    if (!surface->borrowed_pixels) free(surface->pixels);
    free(surface);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t  les32(const uint8_t *p) { return (int32_t)le32(p); }

SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc) {
    if (!src || !src->data || src->size < 54) {
        if (src && freesrc) SDL_FreeRW(src);
        return NULL;
    }
    const uint8_t *d = src->data;
    if (d[0] != 'B' || d[1] != 'M') {
        if (freesrc) SDL_FreeRW(src);
        return NULL;
    }

    uint32_t pix_off = le32(d + 10);
    int32_t  bw = les32(d + 18);
    int32_t  bh = les32(d + 22);
    uint16_t bpp = le16(d + 28);
    bool top_down = false;
    if (bh < 0) { top_down = true; bh = -bh; }

    if (bw <= 0 || bh <= 0 || pix_off >= src->size || (bpp != 24 && bpp != 32)) {
        if (freesrc) SDL_FreeRW(src);
        return NULL;
    }

    int step = 1;
    while ((bw / step) > 64 || (bh / step) > 64) step++;
    int ow = bw / step; if (ow < 1) ow = 1;
    int oh = bh / step; if (oh < 1) oh = 1;

    uint32_t *pix = (uint32_t *)calloc((size_t)ow * (size_t)oh, sizeof(uint32_t));
    if (!pix) {
        if (freesrc) SDL_FreeRW(src);
        return NULL;
    }

    size_t row_stride = (((size_t)bw * bpp + 31u) / 32u) * 4u;
    for (int y = 0; y < oh; y++) {
        int sy = y * step;
        int file_y = top_down ? sy : (bh - 1 - sy);
        size_t roff = pix_off + (size_t)file_y * row_stride;
        if (roff + row_stride > src->size) continue;
        const uint8_t *row = d + roff;
        for (int x = 0; x < ow; x++) {
            int sx = x * step;
            size_t po = (size_t)sx * (bpp / 8u);
            uint8_t b = row[po + 0];
            uint8_t g = row[po + 1];
            uint8_t r = row[po + 2];
            uint8_t a = (bpp == 32) ? row[po + 3] : 255;
            pix[(size_t)y * (size_t)ow + (size_t)x] =
                ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    if (freesrc) SDL_FreeRW(src);
    SDL_Surface *surf = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!surf) { free(pix); return NULL; }
    surf->w = ow;
    surf->h = oh;
    surf->pitch = ow * 4;
    surf->pixels = pix;
    surf->is_premultiplied = SDL_FALSE;
    return surf;
}

int IMG_Init(int flags) {
    return flags;
}

void IMG_Quit(void) {
}

SDL_Texture *IMG_LoadTexture_RW(SDL_Renderer *renderer, SDL_RWops *src, int freesrc) {
    if (!renderer || !src || !src->data || src->size == 0) {
        if (src && freesrc) SDL_FreeRW(src);
        return NULL;
    }
    int w = 0, h = 0;
    size_t stride = 0, out_len = 0;
    uint8_t *bgra = ns_image_wuffs_decode_to_bgra(src->data, src->size,
                                                  &w, &h, &stride, &out_len);
    if (freesrc) SDL_FreeRW(src);
    if (!bgra) {
        sense_sdl_set_error("Wuffs decode failed");
        return NULL;
    }
    if ((int)stride != w * 4 && w > 0 && h > 0) {
        uint8_t *packed = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
        if (packed) {
            for (int row = 0; row < h; row++) {
                memcpy(packed + (size_t)row * (size_t)w * 4u,
                       bgra + (size_t)row * stride, (size_t)w * 4u);
            }
            free(bgra);
            bgra = packed;
        }
    }
    return create_texture_from_bgra_ex(renderer, w, h, bgra, SDL_TRUE, true);
}

SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file) {
    if (!renderer || !file) return NULL;
    FILE *fp = fopen(file, "rb");
    if (!fp && file[0] != '/') {
        const char *rel = (file[0] == '.' && file[1] == '/') ? (file + 2) : file;
        static const char prefix[] = "/data/SENSE: The Game/";
        size_t plen = sizeof(prefix) - 1u;
        size_t flen = strlen(rel);
        char *alt = (char *)malloc(plen + flen + 1u);
        if (alt) {
            memcpy(alt, prefix, plen);
            memcpy(alt + plen, rel, flen + 1u);
            fp = fopen(alt, "rb");
            free(alt);
        }
    }
    if (!fp) {
        sense_sdl_set_error("Cannot open %s", file);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    SDL_RWops rw = { buf, (size_t)sz, 0, SDL_FALSE };
    SDL_Texture *tex = IMG_LoadTexture_RW(renderer, &rw, 0);
    free(buf);
    return tex;
}

/* =========================================================================
 * Audio (SDL_mixer -> IPO_OS audio_stream 22050 Hz S16 mono)
 * ========================================================================= */
#define MIX_TARGET_RATE 22050u
#define MIX_MAX_SFX_CH  8

struct Mix_Music {
    int16_t *pcm;
    size_t frames;
    uint32_t duration_ms;
    audio_stream_t *stream;
    bool playing;
    bool paused;
    bool loop;
    uint32_t play_start_ms;
};

struct Mix_Chunk {
    int16_t *pcm;
    size_t frames;
    uint32_t duration_ms;
};

typedef struct {
    audio_stream_t *stream;
    bool playing;
    uint32_t start_ms;
    uint32_t duration_ms;
} MixChannelState;

static Mix_Music *s_cur_music = NULL;
static MixChannelState s_sfx_ch[MIX_MAX_SFX_CH];

static int16_t *decode_wav_to_s16_mono_22k(const uint8_t *data, size_t size,
                                           size_t *out_frames, uint32_t *out_ms) {
    if (!data || size < 44) return NULL;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return NULL;

    uint16_t audio_fmt = 0, ch = 0, bps = 0;
    uint32_t rate = 0;
    const uint8_t *pcm_src = NULL;
    uint32_t pcm_bytes = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *cid = data + pos;
        uint32_t clen = le32(data + pos + 4);
        pos += 8;
        if (pos + clen > size) clen = (uint32_t)(size - pos);

        if (memcmp(cid, "fmt ", 4) == 0 && clen >= 16) {
            audio_fmt = le16(data + pos + 0);
            ch        = le16(data + pos + 2);
            rate      = le32(data + pos + 4);
            bps       = le16(data + pos + 14);
        } else if (memcmp(cid, "data", 4) == 0) {
            pcm_src = data + pos;
            pcm_bytes = clen;
        }
        pos += clen + (clen & 1u);
    }

    if (!pcm_src || pcm_bytes == 0 || ch == 0 || rate == 0 ||
        !((audio_fmt == 1 && (bps == 8 || bps == 16)) || (audio_fmt == 3 && bps == 32))) {
        return NULL;
    }

    size_t frame_bytes = (size_t)ch * (bps / 8u);
    size_t in_frames = pcm_bytes / frame_bytes;
    if (in_frames == 0) return NULL;

    size_t out_n = (size_t)(((uint64_t)in_frames * MIX_TARGET_RATE) / rate);
    if (out_n == 0) out_n = 1;

    int16_t *out = (int16_t *)malloc(out_n * sizeof(int16_t));
    if (!out) return NULL;

    for (size_t i = 0; i < out_n; i++) {
        double src_f = ((double)i * (double)rate) / (double)MIX_TARGET_RATE;
        size_t s0 = (size_t)src_f;
        if (s0 >= in_frames) s0 = in_frames - 1;
        size_t s1 = (s0 + 1 < in_frames) ? (s0 + 1) : s0;
        double frac = src_f - (double)s0;

        float v0 = 0.0f, v1 = 0.0f;
        for (int c = 0; c < ch; c++) {
            const uint8_t *p0 = pcm_src + s0 * frame_bytes + (size_t)c * (bps / 8u);
            const uint8_t *p1 = pcm_src + s1 * frame_bytes + (size_t)c * (bps / 8u);
            if (audio_fmt == 1 && bps == 16) {
                v0 += (float)(int16_t)le16(p0);
                v1 += (float)(int16_t)le16(p1);
            } else if (audio_fmt == 1 && bps == 8) {
                v0 += ((float)p0[0] - 128.0f) * 256.0f;
                v1 += ((float)p1[0] - 128.0f) * 256.0f;
            } else {
                float f0, f1;
                memcpy(&f0, p0, 4);
                memcpy(&f1, p1, 4);
                v0 += f0 * 32767.0f;
                v1 += f1 * 32767.0f;
            }
        }
        v0 /= (float)ch;
        v1 /= (float)ch;
        float v = v0 + (v1 - v0) * (float)frac;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }

    *out_frames = out_n;
    *out_ms = (uint32_t)(((uint64_t)out_n * 1000ULL) / MIX_TARGET_RATE);
    return out;
}

static void pump_audio_state(void) {
    uint32_t now = timer_millis();
    if (s_cur_music && s_cur_music->stream && s_cur_music->playing && !s_cur_music->paused) {
        double p = audio_stream_get_progress(s_cur_music->stream);
        if (p >= 0.99 || (s_cur_music->duration_ms > 0 &&
                          now - s_cur_music->play_start_ms >= s_cur_music->duration_ms + 40u)) {
            if (s_cur_music->loop) {
                audio_stream_seek(s_cur_music->stream, 0.0);
                audio_stream_play(s_cur_music->stream);
                s_cur_music->play_start_ms = now;
            } else {
                audio_stream_stop(s_cur_music->stream);
                s_cur_music->playing = false;
            }
        }
    }
    for (int i = 0; i < MIX_MAX_SFX_CH; i++) {
        if (s_sfx_ch[i].playing && s_sfx_ch[i].stream) {
            double p = audio_stream_get_progress(s_sfx_ch[i].stream);
            if (p >= 0.995 || (now - s_sfx_ch[i].start_ms >= s_sfx_ch[i].duration_ms + 120u)) {
                audio_stream_stop(s_sfx_ch[i].stream);
                audio_stream_destroy(s_sfx_ch[i].stream);
                s_sfx_ch[i].stream = NULL;
                s_sfx_ch[i].playing = false;
            }
        }
    }
}

int Mix_Init(int flags) { return flags; }
void Mix_Quit(void) {}

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)frequency; (void)format; (void)channels; (void)chunksize;
    return 0;
}

void Mix_CloseAudio(void) {
    Mix_HaltMusic();
    for (int i = 0; i < MIX_MAX_SFX_CH; i++) {
        if (s_sfx_ch[i].stream) {
            audio_stream_stop(s_sfx_ch[i].stream);
            audio_stream_destroy(s_sfx_ch[i].stream);
            s_sfx_ch[i].stream = NULL;
        }
        s_sfx_ch[i].playing = false;
    }
}

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc) {
    if (!src || !src->data) {
        if (src && freesrc) SDL_FreeRW(src);
        return NULL;
    }
    size_t frames = 0;
    uint32_t ms = 0;
    int16_t *pcm = decode_wav_to_s16_mono_22k(src->data, src->size, &frames, &ms);
    if (freesrc) SDL_FreeRW(src);
    if (!pcm) return NULL;
    Mix_Music *m = (Mix_Music *)calloc(1, sizeof(Mix_Music));
    if (!m) { free(pcm); return NULL; }
    m->pcm = pcm;
    m->frames = frames;
    m->duration_ms = ms;
    return m;
}

void Mix_FreeMusic(Mix_Music *music) {
    if (!music) return;
    if (s_cur_music == music) s_cur_music = NULL;
    if (music->stream) {
        audio_stream_stop(music->stream);
        audio_stream_destroy(music->stream);
    }
    free(music->pcm);
    free(music);
}

int Mix_PlayMusic(Mix_Music *music, int loops) {
    if (!music || !music->pcm) return -1;
    Mix_HaltMusic();
    if (!music->stream) {
        music->stream = audio_stream_create(AUDIO_FORMAT_S16, 1, MIX_TARGET_RATE,
                                            music->pcm, music->frames * sizeof(int16_t));
    }
    if (music->stream) {
        audio_stream_seek(music->stream, 0.0);
        audio_stream_set_volume(music->stream, 0.85f);
        audio_stream_play(music->stream);
    }
    music->playing = true;
    music->paused = false;
    music->loop = (loops != 0);
    music->play_start_ms = timer_millis();
    s_cur_music = music;
    return 0;
}

int Mix_HaltMusic(void) {
    if (s_cur_music) {
        if (s_cur_music->stream) audio_stream_stop(s_cur_music->stream);
        s_cur_music->playing = false;
        s_cur_music->paused = false;
    }
    return 0;
}

int Mix_PausedMusic(void) {
    return (s_cur_music && s_cur_music->paused) ? 1 : 0;
}

void Mix_PauseMusic(void) {
    if (s_cur_music && s_cur_music->playing && !s_cur_music->paused) {
        if (s_cur_music->stream) audio_stream_pause(s_cur_music->stream);
        s_cur_music->paused = true;
    }
}

void Mix_ResumeMusic(void) {
    if (s_cur_music && s_cur_music->playing && s_cur_music->paused) {
        if (s_cur_music->stream) audio_stream_play(s_cur_music->stream);
        s_cur_music->paused = false;
        s_cur_music->play_start_ms = timer_millis();
    }
}

int Mix_PlayingMusic(void) {
    pump_audio_state();
    return (s_cur_music && s_cur_music->playing) ? 1 : 0;
}

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
    if (!src || !src->data) {
        if (src && freesrc) SDL_FreeRW(src);
        return NULL;
    }
    size_t frames = 0;
    uint32_t ms = 0;
    int16_t *pcm = decode_wav_to_s16_mono_22k(src->data, src->size, &frames, &ms);
    if (freesrc) SDL_FreeRW(src);
    if (!pcm) return NULL;
    Mix_Chunk *c = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!c) { free(pcm); return NULL; }
    c->pcm = pcm;
    c->frames = frames;
    c->duration_ms = ms;
    return c;
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    free(chunk->pcm);
    free(chunk);
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    (void)loops;
    if (!chunk || !chunk->pcm) return -1;
    int ch = channel;
    if (ch < 0 || ch >= MIX_MAX_SFX_CH) {
        ch = 0;
        for (int i = 0; i < MIX_MAX_SFX_CH; i++) {
            if (!s_sfx_ch[i].playing) { ch = i; break; }
        }
    }
    if (s_sfx_ch[ch].stream) {
        audio_stream_stop(s_sfx_ch[ch].stream);
        audio_stream_destroy(s_sfx_ch[ch].stream);
        s_sfx_ch[ch].stream = NULL;
    }
    audio_stream_t *st = audio_stream_create(AUDIO_FORMAT_S16, 1, MIX_TARGET_RATE,
                                             chunk->pcm, chunk->frames * sizeof(int16_t));
    if (st) {
        audio_stream_set_volume(st, 0.95f);
        audio_stream_play(st);
    }
    s_sfx_ch[ch].stream = st;
    s_sfx_ch[ch].playing = true;
    s_sfx_ch[ch].start_ms = timer_millis();
    s_sfx_ch[ch].duration_ms = chunk->duration_ms;
    serial_printf("[SENSE: The Game] play_sfx t=%u ch=%d duration=%u ms\n",
                  (unsigned)timer_millis(), ch, (unsigned)chunk->duration_ms);
    return ch;
}

int Mix_Playing(int channel) {
    pump_audio_state();
    if (channel < 0) {
        for (int i = 0; i < MIX_MAX_SFX_CH; i++) {
            if (s_sfx_ch[i].playing) return 1;
        }
        return 0;
    }
    if (channel >= MIX_MAX_SFX_CH) return 0;
    return s_sfx_ch[channel].playing ? 1 : 0;
}

/* =========================================================================
 * Event Queue + Keyboard / Gamepad (.kmd) / Touchpad (.kmd)
 * ========================================================================= */
#define EVQ_CAP 256
static SDL_Event s_evq[EVQ_CAP];
static int s_evq_head = 0;
static int s_evq_tail = 0;

static bool s_k_e0 = false;
static bool s_key_down[512];

static uint32_t s_gamepad_dev_id = 0;
static uint32_t s_touchpad_dev_id = 0;
static bool s_gamepad_announced = false;
static bool s_pad_btn_down[SDL_CONTROLLER_BUTTON_MAX];
static int  s_dummy_controller_obj = 1;
static int  s_dummy_joystick_obj = 2;

typedef struct {
    bool active;
    bool next_active;
    bool dirty;
    int32_t tracking_id;
    float x, y;
} TouchSlotState;

static TouchSlotState s_tslots[8];
static int s_cur_tslot = 0;

static void sdl_push_event(const SDL_Event *ev) {
    if (ev->type == SDL_KEYDOWN) s_ev_kb++;
    else if (ev->type == SDL_CONTROLLERBUTTONDOWN) s_ev_gp++;
    else if (ev->type == SDL_FINGERDOWN) s_ev_tp++;
    int nxt = (s_evq_head + 1) % EVQ_CAP;
    if (nxt == s_evq_tail) return;
    s_evq[s_evq_head] = *ev;
    s_evq_head = nxt;
}

static bool sdl_pop_event(SDL_Event *ev) {
    if (s_evq_head == s_evq_tail) return false;
    *ev = s_evq[s_evq_tail];
    s_evq_tail = (s_evq_tail + 1) % EVQ_CAP;
    return true;
}

static SDL_Keycode scancode_to_sdlk(uint8_t make_sc, bool e0) {
    if (!e0) {
        switch (make_sc) {
            case 0x01: return SDLK_ESCAPE;
            case 0x1E: return SDLK_a;
            case 0x20: return SDLK_d;
            case 0x21: return SDLK_f;
            case 0x39: return SDLK_SPACE;
            default:   return SDLK_UNKNOWN;
        }
    } else {
        switch (make_sc) {
            case 0x4B: return SDLK_LEFT;
            case 0x4D: return SDLK_RIGHT;
            default:   return SDLK_UNKNOWN;
        }
    }
}

static void handle_ps2_scancode(uint8_t sc) {
    if (sc == 0xE0) {
        s_k_e0 = true;
        return;
    }
    bool is_break = (sc & 0x80u) != 0;
    uint8_t make_sc = sc & 0x7Fu;
    bool e0 = s_k_e0;
    s_k_e0 = false;

    int kidx = (e0 ? 256 : 0) | (int)make_sc;
    SDL_Keycode sym = scancode_to_sdlk(make_sc, e0);

    if (is_break) {
        s_key_down[kidx] = false;
        if (sym != SDLK_UNKNOWN) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYUP;
            ev.key.timestamp = SDL_GetTicks();
            ev.key.windowID = 1;
            ev.key.state = 0;
            ev.key.repeat = 0;
            ev.key.keysym.scancode = kidx;
            ev.key.keysym.sym = sym;
            sdl_push_event(&ev);
        }
    } else {
        bool was_down = s_key_down[kidx];
        s_key_down[kidx] = true;
        if (sym != SDLK_UNKNOWN) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYDOWN;
            ev.key.timestamp = SDL_GetTicks();
            ev.key.windowID = 1;
            ev.key.state = 1;
            ev.key.repeat = was_down ? 1 : 0;
            ev.key.keysym.scancode = kidx;
            ev.key.keysym.sym = sym;
            sdl_push_event(&ev);
        }
    }
}

static void discover_input_devices(bool announce) {
    uint32_t prev_gp = s_gamepad_dev_id;
    s_gamepad_dev_id = 0;
    s_touchpad_dev_id = 0;

    uint32_t cnt = ipo_input_dev_list(NULL, 0u);
    ipo_input_dev_info_t *devs = cnt ? (ipo_input_dev_info_t *)calloc(cnt, sizeof(ipo_input_dev_info_t)) : NULL;
    if (devs && cnt > 0u) {
        (void)ipo_input_dev_list(devs, cnt);
        for (uint32_t i = 0u; i < cnt; i++) {
            devs[i].name_cap = devs[i].name_len + 1u;
            devs[i].name = (char *)calloc(devs[i].name_cap, 1u);
        }
        uint32_t got = ipo_input_dev_list(devs, cnt);
        uint32_t n = (got < cnt) ? got : cnt;
        for (uint32_t i = 0u; i < n; i++) {
            if (devs[i].name && strcmp(devs[i].name, "ipo-gamepad") == 0) {
                s_gamepad_dev_id = devs[i].id;
            } else if (devs[i].name && strcmp(devs[i].name, "ipo-touchpad") == 0) {
                s_touchpad_dev_id = devs[i].id;
            }
        }
        for (uint32_t i = 0u; i < cnt; i++) {
            if (devs[i].name) free(devs[i].name);
        }
        free(devs);
    }

    if (s_gamepad_dev_id && !s_gamepad_announced && announce) {
        s_gamepad_announced = true;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_CONTROLLERDEVICEADDED;
        ev.cdevice.timestamp = SDL_GetTicks();
        ev.cdevice.which = 0;
        sdl_push_event(&ev);
    } else if (!s_gamepad_dev_id && prev_gp && s_gamepad_announced) {
        s_gamepad_announced = false;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_CONTROLLERDEVICEREMOVED;
        ev.cdevice.timestamp = SDL_GetTicks();
        ev.cdevice.which = 0;
        sdl_push_event(&ev);
    }
}

static SDL_GameControllerButton map_gamepad_code(uint32_t code) {
    switch (code) {
        case 0x130u: return SDL_CONTROLLER_BUTTON_A;
        case 0x131u: return SDL_CONTROLLER_BUTTON_B;
        case 0x133u: return SDL_CONTROLLER_BUTTON_Y;
        case 0x134u: return SDL_CONTROLLER_BUTTON_X;
        case 0x13Bu: return SDL_CONTROLLER_BUTTON_START;
        case 0x222u: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        case 0x223u: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        default:     return SDL_CONTROLLER_BUTTON_INVALID;
    }
}

static void emit_gamepad_btn(SDL_GameControllerButton btn, bool down) {
    if (btn <= SDL_CONTROLLER_BUTTON_INVALID || btn >= SDL_CONTROLLER_BUTTON_MAX) return;
    if (down == s_pad_btn_down[btn]) return;
    s_pad_btn_down[btn] = down;

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.timestamp = SDL_GetTicks();
    ev.cbutton.which = 0;
    ev.cbutton.button = (Uint8)btn;
    ev.cbutton.state = down ? 1 : 0;
    sdl_push_event(&ev);
}

static void flush_touch_slot(TouchSlotState *s, int slot_idx) {
    if (!s->dirty && s->active == s->next_active) return;
    s->dirty = false;
    if (!s->active && s->next_active) {
        s->active = true;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_FINGERDOWN;
        ev.tfinger.timestamp = SDL_GetTicks();
        ev.tfinger.touchId = 1;
        ev.tfinger.fingerId = (SDL_FingerID)(slot_idx + 1);
        ev.tfinger.x = s->x;
        ev.tfinger.y = s->y;
        ev.tfinger.pressure = 1.0f;
        ev.tfinger.windowID = 1;
        sdl_push_event(&ev);
    } else if (s->active && !s->next_active) {
        s->active = false;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_FINGERUP;
        ev.tfinger.timestamp = SDL_GetTicks();
        ev.tfinger.touchId = 1;
        ev.tfinger.fingerId = (SDL_FingerID)(slot_idx + 1);
        ev.tfinger.x = s->x;
        ev.tfinger.y = s->y;
        ev.tfinger.pressure = 0.0f;
        ev.tfinger.windowID = 1;
        sdl_push_event(&ev);
    }
}

static void flush_all_touch_slots(void) {
    for (int i = 0; i < 8; i++) {
        flush_touch_slot(&s_tslots[i], i);
    }
}

static void pump_all_inputs(void) {
    static uint32_t s_last_probe_ms = 0;
    uint32_t now = timer_millis();
    if (now - s_last_probe_ms >= 500u) {
        s_last_probe_ms = now;
        discover_input_devices(true);
    }

    if (s_active_win) {
        if (s_active_win->wm_win) {
            if (!wm_is_window_valid(s_active_win->wm_win) || !wm_session_active()) {
                if (!s_active_win->closed) {
                    s_active_win->closed = true;
                    SDL_Event q;
                    memset(&q, 0, sizeof(q));
                    q.type = SDL_QUIT;
                    sdl_push_event(&q);
                }
            } else {
                wm_event_t wev;
                while (wm_poll_event(s_active_win->wm_win, &wev)) {
                    if (wev.type == WM_EVENT_CLOSE) {
                        s_active_win->closed = true;
                        SDL_Event q;
                        memset(&q, 0, sizeof(q));
                        q.type = SDL_QUIT;
                        sdl_push_event(&q);
                    } else if (wev.type == WM_EVENT_KEY_DOWN || wev.type == WM_EVENT_KEY_UP) {
                        handle_ps2_scancode((uint8_t)(wev.data & 0xFFu));
                    }
                }
            }
        } else if (s_active_win->direct_gfx) {
            if (system_is_interrupted() && !s_active_win->closed) {
                s_active_win->closed = true;
                SDL_Event q;
                memset(&q, 0, sizeof(q));
                q.type = SDL_QUIT;
                sdl_push_event(&q);
            } else {
                keyboard_poll();
                uint8_t raw;
                while ((raw = keyboard_get_scancode()) != 0) {
                    handle_ps2_scancode(raw);
                }
            }
        }
    }

    if (s_gamepad_dev_id || s_touchpad_dev_id) {
        static bool s_tp_logged = false;
        ipo_input_event_t iev;
        /* Poll all queued events (dev_filter_id=0) so one device's poll never discards another's events. */
        while (ipo_input_dev_poll_event(0u, &iev) > 0) {
            if (s_gamepad_dev_id && iev.dev_id == s_gamepad_dev_id) {
                if (iev.type == IPO_EV_KEY) {
                    emit_gamepad_btn(map_gamepad_code(iev.code), iev.value != 0);
                } else if (iev.type == IPO_EV_ABS && iev.code == 0x10u) {
                    emit_gamepad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, iev.value < 0);
                    emit_gamepad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, iev.value > 0);
                }
            } else if (s_touchpad_dev_id && iev.dev_id == s_touchpad_dev_id) {
                if (iev.type == IPO_EV_SYN) {
                    if (!s_tp_logged) {
                        for (int k = 0; k < 8; k++) {
                            if (s_tslots[k].dirty && s_tslots[k].next_active) {
                                s_tp_logged = true;
                                printf("[SENSE: The Game] touchpad active slot=%d x_milli=%d\n",
                                       k, (int)(s_tslots[k].x * 1000.0f));
                                break;
                            }
                        }
                    }
                    flush_all_touch_slots();
                } else if (iev.type == IPO_EV_ABS) {
                    if (iev.code == 0x2Fu) {
                        s_cur_tslot = (iev.value >= 0 && iev.value < 8) ? iev.value : 0;
                    } else {
                        TouchSlotState *s = &s_tslots[s_cur_tslot];
                        if (iev.code == 0x39u) {
                            if (s->dirty) flush_touch_slot(s, s_cur_tslot);
                            s->tracking_id = iev.value;
                            s->next_active = (iev.value >= 0);
                            s->dirty = true;
                        } else if (iev.code == 0x35u || iev.code == 0x00u) {
                            float fx = (float)iev.value / 10000.0f;
                            if (fx < 0.0f) fx = 0.0f; else if (fx > 1.0f) fx = 1.0f;
                            s->x = fx;
                        } else if (iev.code == 0x36u || iev.code == 0x01u) {
                            float fy = (float)iev.value / 10000.0f;
                            if (fy < 0.0f) fy = 0.0f; else if (fy > 1.0f) fy = 1.0f;
                            s->y = fy;
                        }
                    }
                } else if (iev.type == IPO_EV_KEY && iev.code == 0x14Au) {
                    s_tslots[0].next_active = (iev.value != 0);
                    s_tslots[0].dirty = true;
                    flush_touch_slot(&s_tslots[0], 0);
                }
            }
        }
    }
}

int SDL_PollEvent(SDL_Event *event) {
    pump_audio_state();
    if (s_evq_head == s_evq_tail) {
        pump_all_inputs();
    }
    if (!event) return (s_evq_head != s_evq_tail) ? 1 : 0;
    return sdl_pop_event(event) ? 1 : 0;
}

/* =========================================================================
 * GameController / Joystick + SDL_Init / SDL_Quit
 * ========================================================================= */
int SDL_NumJoysticks(void) {
    discover_input_devices(false);
    return s_gamepad_dev_id ? 1 : 0;
}

const char *SDL_JoystickNameForIndex(int device_index) {
    return (device_index == 0 && s_gamepad_dev_id) ? "IPO Gamepad" : "";
}

SDL_bool SDL_IsGameController(int joystick_index) {
    return (joystick_index == 0 && s_gamepad_dev_id) ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController *SDL_GameControllerOpen(int joystick_index) {
    if (joystick_index != 0 || !s_gamepad_dev_id) return NULL;
    return (SDL_GameController *)&s_dummy_controller_obj;
}

void SDL_GameControllerClose(SDL_GameController *gamecontroller) {
    (void)gamecontroller;
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller) {
    return gamecontroller ? (SDL_Joystick *)&s_dummy_joystick_obj : NULL;
}

const char *SDL_GameControllerName(SDL_GameController *gamecontroller) {
    return gamecontroller ? "IPO Gamepad" : "Unknown";
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick) {
    return joystick ? 0 : -1;
}

int SDL_Init(Uint32 flags) {
    (void)flags;
    init_palette_and_luts();
    memset(s_evq, 0, sizeof(s_evq));
    s_evq_head = s_evq_tail = 0;
    s_k_e0 = false;
    memset(s_key_down, 0, sizeof(s_key_down));
    memset(s_pad_btn_down, 0, sizeof(s_pad_btn_down));
    memset(s_tslots, 0, sizeof(s_tslots));
    s_gamepad_announced = false;
    discover_input_devices(true);
    return 0;
}

void SDL_Quit(void) {
    Mix_CloseAudio();
}
