#include <allegro5/allegro.h>
#include <allegro5/allegro_image.h>
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_ttf.h>
#include <allegro5/allegro_primitives.h>
#include <allegro5/allegro_audio.h>
#include <allegro5/allegro_acodec.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wm.h>
#include <vga.h>
#include <vga_gfx.h>
#include <driver/input/keyboard.h>
#include <system/state.h>
#include <system/timer.h>
#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/sound.h>
#include <syscall.h>

#include "common/ipo_hd_present.h"

extern int serial_printf(const char *fmt, ...);
extern uint8_t *ns_image_wuffs_decode_to_bgra(const uint8_t *data, size_t len,
                                              int *out_w, int *out_h,
                                              size_t *out_stride, size_t *out_buf_len);

void *ns_texture_new(int w, int h, int fmt, void *bytes, size_t stride, size_t len) {
    (void)w; (void)h; (void)fmt; (void)bytes; (void)stride; (void)len;
    return NULL;
}

/* =========================================================================
 * O(1) WM Palette LUT
 * ========================================================================= */
static uint8_t s_wm_palette[248][3];
static uint32_t s_wm_pal32[256];
static uint8_t s_rgb555_to_wm[32768];
static bool    s_pal_ready = false;

static void init_wm_palette_lut(void) {
    if (s_pal_ready) return;
    static const uint8_t cga[16][3] = {
        {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
        {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
        {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
        {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}
    };
    for (int i = 0; i < 16; i++) {
        s_wm_palette[i][0] = cga[i][0];
        s_wm_palette[i][1] = cga[i][1];
        s_wm_palette[i][2] = cga[i][2];
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        s_wm_palette[16 + i][0] = v;
        s_wm_palette[16 + i][1] = v;
        s_wm_palette[16 + i][2] = v;
    }
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                s_wm_palette[idx][0] = (uint8_t)(r * 51);
                s_wm_palette[idx][1] = (uint8_t)(g * 51);
                s_wm_palette[idx][2] = (uint8_t)(b * 51);
                idx++;
            }
        }
    }
    for (int r5 = 0; r5 < 32; r5++) {
        int r = (r5 << 3) | (r5 >> 2);
        for (int g5 = 0; g5 < 32; g5++) {
            int g = (g5 << 3) | (g5 >> 2);
            for (int b5 = 0; b5 < 32; b5++) {
                int b = (b5 << 3) | (b5 >> 2);
                int best = 0;
                uint32_t best_d = 0xFFFFFFFFu;
                for (int i = 0; i < 248; i++) {
                    int dr = r - (int)s_wm_palette[i][0];
                    int dg = g - (int)s_wm_palette[i][1];
                    int db = b - (int)s_wm_palette[i][2];
                    uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
                    if (d < best_d) {
                        best_d = d;
                        best = i;
                        if (d == 0) break;
                    }
                }
                s_rgb555_to_wm[(r5 << 10) | (g5 << 5) | b5] = (uint8_t)best;
            }
        }
    }
    for (int i = 0; i < 248; i++) {
        s_wm_pal32[i] = 0xFF000000u |
                        ((uint32_t)s_wm_palette[i][0] << 16) |
                        ((uint32_t)s_wm_palette[i][1] << 8)  |
                        (uint32_t)s_wm_palette[i][2];
    }
    for (int i = 248; i < 256; i++) s_wm_pal32[i] = 0xFF000000u;
    s_pal_ready = true;
}

static inline uint8_t rgb_to_wm(uint8_t r, uint8_t g, uint8_t b) {
    return s_rgb555_to_wm[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

static void apply_wm_dac_palette(void) {
    init_wm_palette_lut();
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
}

static inline uint8_t color_to_wm(ALLEGRO_COLOR c) {
    int r = (int)(c.r * 255.0f + 0.5f); if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)(c.g * 255.0f + 0.5f); if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)(c.b * 255.0f + 0.5f); if (b < 0) b = 0; if (b > 255) b = 255;
    return rgb_to_wm((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* =========================================================================
 * Asset path helper (supports CWD or /data/donkey_kong/<path>)
 * ========================================================================= */
static FILE *open_dk_asset(const char *filename, const char *mode) {
    if (!filename) return NULL;
    FILE *f = fopen(filename, mode);
    if (f) return f;
    size_t len = strlen(filename);
    if (len > ((size_t)-1) - 20u) return NULL;
    char *alt = (char *)malloc(len + 20u);
    if (!alt) return NULL;
    snprintf(alt, len + 20u, "/data/donkey_kong/%s", filename);
    f = fopen(alt, mode);
    free(alt);
    return f;
}

/* =========================================================================
 * Structures
 * ========================================================================= */
struct ALLEGRO_BITMAP {
    int w, h;
    uint8_t *px;    /* WM 8bpp index */
    uint8_t *alpha; /* NULL if opaque (display/world), non-NULL for sprites */
};

struct ALLEGRO_DISPLAY {
    int w, h;
    ALLEGRO_BITMAP *backbuffer;
    wm_window_t *wm_win;
    bool direct_gfx;
    ipo_hd_view_t hd;
};

struct ALLEGRO_EVENT_SOURCE {
    int kind; /* 1=kbd, 2=timer, 3=display */
    void *owner;
};

struct ALLEGRO_TIMER {
    ALLEGRO_EVENT_SOURCE src;
    uint32_t period_ms;
    bool started;
    uint32_t next_tick_ms;
    int64_t count;
};

static char *dk_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

struct ALLEGRO_EVENT_QUEUE {
    ALLEGRO_EVENT *items;
    int head, tail, cap;
    bool has_kbd;
    ALLEGRO_TIMER *timer;
    ALLEGRO_DISPLAY *display;
};

struct ALLEGRO_FONT {
    int size;
};

struct ALLEGRO_SAMPLE {
    int16_t *samples;
    uint32_t num_samples; /* total frames */
    uint32_t sample_rate;
    uint16_t channels;
};

typedef struct {
    char *section;
    char *key;
    char *val;
} allegro_cfg_entry_t;

struct ALLEGRO_CONFIG {
    allegro_cfg_entry_t *entries;
    int count;
    int cap;
};

static ALLEGRO_EVENT_SOURCE s_kbd_src = { 1, NULL };
static ALLEGRO_DISPLAY     *s_cur_display = NULL;
static ALLEGRO_BITMAP      *s_cur_target = NULL;
static int                  s_new_bitmap_flags = 0;

/* =========================================================================
 * Init & Keyboard Scancodes
 * ========================================================================= */
bool al_init(void) {
    init_wm_palette_lut();
    return true;
}

bool al_install_keyboard(void) { return true; }
void al_uninstall_keyboard(void) {}
bool al_init_image_addon(void) { return true; }
void al_shutdown_image_addon(void) {}
bool al_init_font_addon(void) { return true; }
void al_shutdown_font_addon(void) {}
bool al_init_ttf_addon(void) { return true; }
void al_shutdown_ttf_addon(void) {}
bool al_init_primitives_addon(void) { return true; }
void al_shutdown_primitives_addon(void) {}

static int ps2_to_al_key(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return ALLEGRO_KEY_UP;
            case 0x50: return ALLEGRO_KEY_DOWN;
            case 0x4B: return ALLEGRO_KEY_LEFT;
            case 0x4D: return ALLEGRO_KEY_RIGHT;
            default:   return 0;
        }
    }
    switch (sc) {
        case 0x01: return ALLEGRO_KEY_ESCAPE;
        case 0x0F: return ALLEGRO_KEY_TAB;
        case 0x1C: return ALLEGRO_KEY_ENTER;
        case 0x39: return ALLEGRO_KEY_SPACE;
        case 0x11: return ALLEGRO_KEY_W;
        case 0x1E: return ALLEGRO_KEY_A;
        case 0x1F: return ALLEGRO_KEY_S;
        case 0x20: return ALLEGRO_KEY_D;
        case 0x18: return ALLEGRO_KEY_O;
        case 0x19: return ALLEGRO_KEY_P;
        case 0x31: return ALLEGRO_KEY_N;
        default:   return 0;
    }
}

/* =========================================================================
 * Bitmaps & Drawing
 * ========================================================================= */
int al_get_new_bitmap_flags(void) { return s_new_bitmap_flags; }
void al_set_new_bitmap_flags(int flags) { s_new_bitmap_flags = flags; }

ALLEGRO_BITMAP *al_create_bitmap(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    ALLEGRO_BITMAP *b = (ALLEGRO_BITMAP *)calloc(1, sizeof(ALLEGRO_BITMAP));
    if (!b) return NULL;
    b->w = w;
    b->h = h;
    b->px = (uint8_t *)calloc((size_t)w * (size_t)h, 1u);
    b->alpha = NULL;
    if (!b->px) {
        free(b);
        return NULL;
    }
    return b;
}

void al_destroy_bitmap(ALLEGRO_BITMAP *bitmap) {
    if (!bitmap) return;
    free(bitmap->px);
    free(bitmap->alpha);
    free(bitmap);
}

ALLEGRO_BITMAP *al_load_bitmap(const char *filename) {
    FILE *f = open_dk_asset(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) { fclose(f); return NULL; }
    size_t nr = fread(raw, 1u, (size_t)sz, f);
    fclose(f);

    int w = 0, h = 0;
    size_t stride = 0, out_len = 0;
    uint8_t *bgra = ns_image_wuffs_decode_to_bgra(raw, nr, &w, &h, &stride, &out_len);
    free(raw);
    if (!bgra || w <= 0 || h <= 0) {
        free(bgra);
        return NULL;
    }

    ALLEGRO_BITMAP *bmp = (ALLEGRO_BITMAP *)calloc(1, sizeof(ALLEGRO_BITMAP));
    if (!bmp) { free(bgra); return NULL; }
    bmp->w = w;
    bmp->h = h;
    bmp->px = (uint8_t *)malloc((size_t)w * (size_t)h);
    bmp->alpha = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!bmp->px || !bmp->alpha) {
        free(bmp->px); free(bmp->alpha); free(bmp); free(bgra);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = bgra + (size_t)y * stride;
        uint8_t *dpx = bmp->px + (size_t)y * (size_t)w;
        uint8_t *dal = bmp->alpha + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint8_t b = srow[x * 4 + 0];
            uint8_t g = srow[x * 4 + 1];
            uint8_t r = srow[x * 4 + 2];
            uint8_t a = srow[x * 4 + 3];
            dpx[x] = rgb_to_wm(r, g, b);
            dal[x] = a;
        }
    }
    free(bgra);
    return bmp;
}

void al_set_target_bitmap(ALLEGRO_BITMAP *bitmap) {
    s_cur_target = bitmap;
}

void al_set_target_backbuffer(ALLEGRO_DISPLAY *display) {
    if (display) s_cur_target = display->backbuffer;
}

ALLEGRO_COLOR al_map_rgb(unsigned char r, unsigned char g, unsigned char b) {
    return (ALLEGRO_COLOR){ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
}

void al_clear_to_color(ALLEGRO_COLOR color) {
    if (!s_cur_target || !s_cur_target->px) return;
    uint8_t c = color_to_wm(color);
    memset(s_cur_target->px, c, (size_t)s_cur_target->w * (size_t)s_cur_target->h);
}

static inline void put_px(ALLEGRO_BITMAP *dst, int x, int y, uint8_t c) {
    if (dst && (unsigned)x < (unsigned)dst->w && (unsigned)y < (unsigned)dst->h) {
        dst->px[(size_t)y * (size_t)dst->w + (size_t)x] = c;
    }
}

void al_draw_bitmap_region(ALLEGRO_BITMAP *bitmap, float sx, float sy, float sw, float sh,
                           float dx, float dy, int flags) {
    if (!bitmap || !s_cur_target || !bitmap->px || !s_cur_target->px) return;
    int ix = (int)floorf(sx + 0.5f);
    int iy = (int)floorf(sy + 0.5f);
    int iw = (int)floorf(sw + 0.5f);
    int ih = (int)floorf(sh + 0.5f);
    int ox = (int)floorf(dx + 0.5f);
    int oy = (int)floorf(dy + 0.5f);
    bool fx = (flags & ALLEGRO_FLIP_HORIZONTAL) != 0;
    bool fy = (flags & ALLEGRO_FLIP_VERTICAL) != 0;

    for (int row = 0; row < ih; row++) {
        int dst_y = oy + row;
        if ((unsigned)dst_y >= (unsigned)s_cur_target->h) continue;
        int src_y = iy + (fy ? (ih - 1 - row) : row);
        if ((unsigned)src_y >= (unsigned)bitmap->h) continue;

        const uint8_t *spx = bitmap->px + (size_t)src_y * (size_t)bitmap->w;
        const uint8_t *sal = bitmap->alpha ? (bitmap->alpha + (size_t)src_y * (size_t)bitmap->w) : NULL;
        uint8_t *dpx = s_cur_target->px + (size_t)dst_y * (size_t)s_cur_target->w;

        for (int col = 0; col < iw; col++) {
            int dst_x = ox + col;
            if ((unsigned)dst_x >= (unsigned)s_cur_target->w) continue;
            int src_x = ix + (fx ? (iw - 1 - col) : col);
            if ((unsigned)src_x >= (unsigned)bitmap->w) continue;
            if (sal && sal[src_x] < 128u) continue;
            dpx[dst_x] = spx[src_x];
        }
    }
}

void al_draw_scaled_bitmap(ALLEGRO_BITMAP *bitmap,
                           float sx, float sy, float sw, float sh,
                           float dx, float dy, float dw, float dh, int flags) {
    (void)flags;
    if (!bitmap || !s_cur_target || !bitmap->px || !s_cur_target->px) return;
    int isx = (int)sx, isy = (int)sy, isw = (int)sw, ish = (int)sh;
    int idx = (int)dx, idy = (int)dy, idw = (int)dw, idh = (int)dh;
    if (isw <= 0 || ish <= 0 || idw <= 0 || idh <= 0) return;

    for (int y = 0; y < idh; y++) {
        int dst_y = idy + y;
        if ((unsigned)dst_y >= (unsigned)s_cur_target->h) continue;
        int src_y = isy + (y * ish) / idh;
        if ((unsigned)src_y >= (unsigned)bitmap->h) continue;
        const uint8_t *srow = bitmap->px + (size_t)src_y * (size_t)bitmap->w;
        uint8_t *drow = s_cur_target->px + (size_t)dst_y * (size_t)s_cur_target->w;
        if (idw == isw && idx >= 0 && idx + idw <= s_cur_target->w && isx == 0) {
            memcpy(drow + idx, srow, (size_t)idw);
        } else {
            for (int x = 0; x < idw; x++) {
                int dst_x = idx + x;
                if ((unsigned)dst_x >= (unsigned)s_cur_target->w) continue;
                int src_x = isx + (x * isw) / idw;
                if ((unsigned)src_x < (unsigned)bitmap->w) {
                    drow[dst_x] = srow[src_x];
                }
            }
        }
    }
}

/* =========================================================================
 * Primitives & 8x8 Crisp Arcade Font
 * ========================================================================= */
static const uint8_t s_font6x8[95 * 8] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*   */
    0x20,0x20,0x20,0x20,0x00,0x20,0x00,0x00, /* ! */
    0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x00, /* " */
    0x50,0xF8,0x50,0x50,0xF8,0x50,0x00,0x00, /* # */
    0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00, /* $ */
    0xC0,0xC8,0x10,0x20,0x40,0x98,0x18,0x00, /* % */
    0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68,0x00, /* & */
    0x20,0x40,0x00,0x00,0x00,0x00,0x00,0x00, /* ' */
    0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00, /* ( */
    0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00, /* ) */
    0x00,0x20,0xA8,0x70,0xA8,0x20,0x00,0x00, /* * */
    0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00, /* + */
    0x00,0x00,0x00,0x00,0x00,0x20,0x40,0x00, /* , */
    0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00, /* - */
    0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00, /* . */
    0x08,0x10,0x10,0x20,0x40,0x40,0x80,0x00, /* / */
    0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00, /* 0 */
    0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00, /* 1 */
    0x7C,0xC6,0x06,0x1C,0x38,0x60,0xFE,0x00, /* 2 */
    0xFC,0x06,0x06,0x3C,0x06,0xC6,0x7C,0x00, /* 3 */
    0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00, /* 4 */
    0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00, /* 5 */
    0x7C,0xC0,0xC0,0xFC,0xC6,0xC6,0x7C,0x00, /* 6 */
    0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00, /* 7 */
    0x7C,0xC6,0x7C,0x7C,0xC6,0xC6,0x7C,0x00, /* 8 */
    0x7C,0xC6,0xC6,0x7E,0x06,0x06,0x7C,0x00, /* 9 */
    0x00,0x60,0x60,0x00,0x60,0x60,0x00,0x00, /* : */
    0x00,0x30,0x00,0x30,0x30,0x60,0x00,0x00, /* ; */
    0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00, /* < */
    0x00,0xFC,0xFC,0x00,0xFC,0xFC,0x00,0x00, /* = */
    0xC0,0x60,0x30,0x18,0x30,0x60,0xC0,0x00, /* > */
    0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00, /* ? */
    0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00, /* @ */
    0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00, /* A */
    0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00, /* B */
    0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00, /* C */
    0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00, /* D */
    0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00, /* E */
    0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00, /* F */
    0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7C,0x00, /* G */
    0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00, /* H */
    0x7C,0x18,0x18,0x18,0x18,0x18,0x7C,0x00, /* I */
    0x3E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00, /* J */
    0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00, /* K */
    0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00, /* L */
    0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00, /* M */
    0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00, /* N */
    0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00, /* O */
    0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00, /* P */
    0x7C,0xC6,0xC6,0xC6,0xD6,0xCC,0x76,0x00, /* Q */
    0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00, /* R */
    0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00, /* S */
    0xFE,0x18,0x18,0x18,0x18,0x18,0x18,0x00, /* T */
    0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00, /* U */
    0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00, /* V */
    0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00, /* W */
    0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00, /* X */
    0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x00, /* Y */
    0xFE,0x0C,0x18,0x30,0x60,0xC0,0xFE,0x00, /* Z */
    0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00, /* [ */
    0x80,0x40,0x40,0x20,0x10,0x10,0x08,0x00, /* \ */
    0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00, /* ] */
    0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00, /* ^ */
    0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00, /* _ */
    0x40,0x20,0x00,0x00,0x00,0x00,0x00,0x00, /* ` */
    0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00, /* a */
    0x80,0x80,0xF0,0x88,0x88,0x88,0xF0,0x00, /* b */
    0x00,0x00,0x70,0x88,0x80,0x88,0x70,0x00, /* c */
    0x08,0x08,0x78,0x88,0x88,0x88,0x78,0x00, /* d */
    0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00, /* e */
    0x30,0x48,0x40,0xF0,0x40,0x40,0x40,0x00, /* f */
    0x00,0x00,0x78,0x88,0x78,0x08,0x70,0x00, /* g */
    0x80,0x80,0xF0,0x88,0x88,0x88,0x88,0x00, /* h */
    0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00, /* i */
    0x10,0x00,0x30,0x10,0x10,0x90,0x60,0x00, /* j */
    0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00, /* k */
    0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00, /* l */
    0x00,0x00,0xD8,0xA8,0xA8,0xA8,0xA8,0x00, /* m */
    0x00,0x00,0xF0,0x88,0x88,0x88,0x88,0x00, /* n */
    0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00, /* o */
    0x00,0x00,0xF0,0x88,0x88,0xF0,0x80,0x00, /* p */
    0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x00, /* q */
    0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00, /* r */
    0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00, /* s */
    0x40,0x40,0xF0,0x40,0x40,0x48,0x30,0x00, /* t */
    0x00,0x00,0x88,0x88,0x88,0x88,0x78,0x00, /* u */
    0x00,0x00,0x88,0x88,0x50,0x50,0x20,0x00, /* v */
    0x00,0x00,0x88,0x88,0xA8,0xA8,0x50,0x00, /* w */
    0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00, /* x */
    0x00,0x00,0x88,0x88,0x78,0x08,0x70,0x00, /* y */
    0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00, /* z */
    0x18,0x20,0x20,0x60,0x20,0x20,0x18,0x00, /* { */
    0x20,0x20,0x20,0x00,0x20,0x20,0x20,0x00, /* | */
    0xC0,0x20,0x20,0x30,0x20,0x20,0xC0,0x00, /* } */
    0x48,0x90,0x00,0x00,0x00,0x00,0x00,0x00, /* ~ */
};

ALLEGRO_FONT *al_create_builtin_font(void) {
    ALLEGRO_FONT *f = (ALLEGRO_FONT *)calloc(1, sizeof(ALLEGRO_FONT));
    if (f) f->size = 8;
    return f;
}

ALLEGRO_FONT *al_load_ttf_font(const char *filename, int size, int flags) {
    (void)flags;
    FILE *fp = open_dk_asset(filename, "rb");
    if (!fp) return NULL;
    fclose(fp);
    ALLEGRO_FONT *f = (ALLEGRO_FONT *)calloc(1, sizeof(ALLEGRO_FONT));
    if (f) f->size = (size > 0) ? size : 8;
    return f;
}

void al_destroy_font(ALLEGRO_FONT *f) { free(f); }

void al_draw_text(const ALLEGRO_FONT *font, ALLEGRO_COLOR color, float x, float y, int flags, const char *text) {
    (void)font; (void)flags;
    if (!text || !s_cur_target) return;
    uint8_t col = color_to_wm(color);
    int cx = (int)floorf(x + 0.5f);
    int cy = (int)floorf(y + 0.5f);
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= 0x20 && ch <= 0x7E) {
            const uint8_t *g = s_font6x8 + (ch - 0x20) * 8;
            for (int r = 0; r < 8; r++) {
                uint8_t bits = g[r];
                for (int c = 0; c < 7; c++) {
                    if (bits & (0x80u >> c)) put_px(s_cur_target, cx + c, cy + r, col);
                }
            }
        }
        cx += 8;
    }
}

void al_draw_multiline_text(const ALLEGRO_FONT *font, ALLEGRO_COLOR color,
                            float x, float y, float max_width, float line_height,
                            int flags, const char *text) {
    (void)max_width;
    if (!text) return;
    char *line = NULL;
    size_t line_cap = 0;
    float cur_y = y;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == ((size_t)-1)) break;
        if (len + 1u > line_cap) {
            char *nb = (char *)realloc(line, len + 1u);
            if (!nb) break;
            line = nb;
            line_cap = len + 1u;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        al_draw_text(font, color, x, cur_y, flags, line);
        cur_y += (line_height > 0.0f) ? line_height : 10.0f;
        if (!nl) break;
        p = nl + 1;
    }
    free(line);
}

void al_draw_filled_rectangle(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color) {
    if (!s_cur_target) return;
    uint8_t c = color_to_wm(color);
    int ix1 = (int)x1, iy1 = (int)y1, ix2 = (int)x2, iy2 = (int)y2;
    if (ix1 > ix2) { int t = ix1; ix1 = ix2; ix2 = t; }
    if (iy1 > iy2) { int t = iy1; iy1 = iy2; iy2 = t; }
    for (int y = iy1; y <= iy2; y++) {
        for (int x = ix1; x <= ix2; x++) put_px(s_cur_target, x, y, c);
    }
}

void al_draw_filled_rounded_rectangle(float x1, float y1, float x2, float y2, float rx, float ry, ALLEGRO_COLOR color) {
    (void)rx; (void)ry;
    al_draw_filled_rectangle(x1, y1, x2, y2, color);
}

void al_draw_line(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color, float thickness) {
    (void)thickness;
    if (!s_cur_target) return;
    uint8_t c = color_to_wm(color);
    int ix1 = (int)x1, iy1 = (int)y1, ix2 = (int)x2, iy2 = (int)y2;
    int dx = abs(ix2 - ix1), sx = ix1 < ix2 ? 1 : -1;
    int dy = -abs(iy2 - iy1), sy = iy1 < iy2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(s_cur_target, ix1, iy1, c);
        if (ix1 == ix2 && iy1 == iy2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ix1 += sx; }
        if (e2 <= dx) { err += dx; iy1 += sy; }
    }
}

void al_draw_rectangle(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color, float thickness) {
    (void)thickness;
    al_draw_line(x1, y1, x2, y1, color, 1.0f);
    al_draw_line(x2, y1, x2, y2, color, 1.0f);
    al_draw_line(x2, y2, x1, y2, color, 1.0f);
    al_draw_line(x1, y2, x1, y1, color, 1.0f);
}

void al_draw_circle(float cx, float cy, float r, ALLEGRO_COLOR color, float thickness) {
    (void)thickness;
    if (!s_cur_target || r <= 0.0f) return;
    uint8_t c = color_to_wm(color);
    int ir = (int)(r + 0.5f), icx = (int)cx, icy = (int)cy;
    int x = ir, y = 0, err = 1 - x;
    while (x >= y) {
        put_px(s_cur_target, icx + x, icy + y, c);
        put_px(s_cur_target, icx + y, icy + x, c);
        put_px(s_cur_target, icx - y, icy + x, c);
        put_px(s_cur_target, icx - x, icy + y, c);
        put_px(s_cur_target, icx - x, icy - y, c);
        put_px(s_cur_target, icx - y, icy - x, c);
        put_px(s_cur_target, icx + y, icy - x, c);
        put_px(s_cur_target, icx + x, icy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

/* =========================================================================
 * Config
 * ========================================================================= */
ALLEGRO_CONFIG *al_create_config(void) {
    return (ALLEGRO_CONFIG *)calloc(1, sizeof(ALLEGRO_CONFIG));
}

void al_destroy_config(ALLEGRO_CONFIG *config) {
    if (!config) return;
    for (int i = 0; i < config->count; i++) {
        free(config->entries[i].section);
        free(config->entries[i].key);
        free(config->entries[i].val);
    }
    free(config->entries);
    free(config);
}

void al_set_config_value(ALLEGRO_CONFIG *config, const char *section, const char *key, const char *value) {
    if (!config || !key || !value) return;
    const char *sec = section ? section : "";
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->entries[i].section, sec) == 0 && strcmp(config->entries[i].key, key) == 0) {
            char *nv = dk_strdup(value);
            if (!nv) return;
            free(config->entries[i].val);
            config->entries[i].val = nv;
            return;
        }
    }
    if (config->count >= config->cap) {
        int new_cap = config->cap ? (config->cap * 2) : 32;
        if (new_cap <= config->cap || (size_t)new_cap > SIZE_MAX / sizeof(allegro_cfg_entry_t)) return;
        allegro_cfg_entry_t *ne = (allegro_cfg_entry_t *)realloc(config->entries, (size_t)new_cap * sizeof(allegro_cfg_entry_t));
        if (!ne) return;
        config->entries = ne;
        config->cap = new_cap;
    }
    char *s_dup = dk_strdup(sec);
    char *k_dup = dk_strdup(key);
    char *v_dup = dk_strdup(value);
    if (!s_dup || !k_dup || !v_dup) {
        free(s_dup); free(k_dup); free(v_dup);
        return;
    }
    int i = config->count++;
    config->entries[i].section = s_dup;
    config->entries[i].key = k_dup;
    config->entries[i].val = v_dup;
}

const char *al_get_config_value(const ALLEGRO_CONFIG *config, const char *section, const char *key) {
    if (!config || !key) return NULL;
    const char *sec = section ? section : "";
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->entries[i].section, sec) == 0 && strcmp(config->entries[i].key, key) == 0) {
            return config->entries[i].val;
        }
    }
    return NULL;
}

static char *dk_read_line(FILE *f) {
    size_t cap = 128u, len = 0u;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len + 2u > cap) {
            if (cap > SIZE_MAX / 2u) { free(buf); return NULL; }
            cap *= 2u;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)ch;
        if (ch == '\n') break;
    }
    if (len == 0u) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

ALLEGRO_CONFIG *al_load_config_file(const char *filename) {
    FILE *f = open_dk_asset(filename, "r");
    if (!f) return NULL;
    ALLEGRO_CONFIG *cfg = al_create_config();
    if (!cfg) { fclose(f); return NULL; }
    char *cur_sec = dk_strdup("");
    char *line;
    while ((line = dk_read_line(f)) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\0' && *p != '\n' && *p != '\r' && *p != '#' && *p != ';') {
            size_t l = strlen(p);
            while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = '\0';
            if (p[0] == '[' && l >= 2 && p[l - 1] == ']') {
                p[l - 1] = '\0';
                char *ns = dk_strdup(p + 1);
                if (ns) { free(cur_sec); cur_sec = ns; }
            } else {
                char *eq = strchr(p, '=');
                if (eq) {
                    *eq = '\0';
                    char *k = p;
                    char *v = eq + 1;
                    size_t kl = strlen(k);
                    while (kl > 0 && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = '\0';
                    while (*v == ' ' || *v == '\t') v++;
                    al_set_config_value(cfg, cur_sec ? cur_sec : "", k, v);
                }
            }
        }
        free(line);
    }
    free(cur_sec);
    fclose(f);
    return cfg;
}

bool al_save_config_file(const char *filename, const ALLEGRO_CONFIG *config) {
    if (!filename || !config) return false;
    FILE *f = fopen(filename, "w");
    if (!f) return false;
    for (int i = 0; i < config->count; i++) {
        fprintf(f, "%s = %s\n", config->entries[i].key, config->entries[i].val);
    }
    fclose(f);
    return true;
}

/* =========================================================================
 * Audio (RIFF WAV parser + polyphonic voice pool via IPO_OS audio_core)
 * ========================================================================= */
static bool s_audio_ok = false;

typedef struct {
    bool            active;
    bool            loop;
    int             id;
    ALLEGRO_SAMPLE *spl;
    float           gain;
    float           speed;
    audio_stream_t *stream;
    int16_t        *pan_buf;
} dk_voice_t;

static dk_voice_t *s_dk_voices = NULL;
static int         s_dk_voices_cap = 0;
static int         s_dk_next_id = 1;

static uint16_t dk_rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t dk_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool dk_voice_is_playing(audio_stream_t *st) {
    if (!st) return false;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    return (ipo_audio_get_stream_clock(st, &clk) == IPO_AUDIO_OK) && (clk.paused == 0u);
}

static void dk_reap_voices(void) {
    for (int i = 0; i < s_dk_voices_cap; i++) {
        dk_voice_t *v = &s_dk_voices[i];
        if (!v->active) continue;
        if (!dk_voice_is_playing(v->stream)) {
            if (v->stream) {
                audio_stream_destroy(v->stream);
                v->stream = NULL;
            }
            if (v->loop && v->spl && v->spl->samples && v->spl->num_samples > 0u) {
                const int16_t *pcm = v->pan_buf ? v->pan_buf : v->spl->samples;
                int ch = v->pan_buf ? 2 : (int)(v->spl->channels ? v->spl->channels : 1u);
                uint32_t rate = (uint32_t)((float)(v->spl->sample_rate ? v->spl->sample_rate : 22050u) * v->speed);
                if (rate < 100u) rate = 100u;
                v->stream = audio_stream_create(AUDIO_FORMAT_S16, ch, rate, pcm,
                                                (size_t)v->spl->num_samples * (size_t)ch * sizeof(int16_t));
                if (v->stream) {
                    audio_stream_set_volume(v->stream, v->gain);
                    audio_stream_play(v->stream);
                    continue;
                }
            }
            free(v->pan_buf);
            memset(v, 0, sizeof(*v));
        }
    }
}

static void dk_stop_all_voices(void) {
    for (int i = 0; i < s_dk_voices_cap; i++) {
        if (s_dk_voices[i].active) {
            if (s_dk_voices[i].stream) {
                audio_stream_stop(s_dk_voices[i].stream);
                audio_stream_destroy(s_dk_voices[i].stream);
            }
            free(s_dk_voices[i].pan_buf);
            memset(&s_dk_voices[i], 0, sizeof(s_dk_voices[i]));
        }
    }
    free(s_dk_voices);
    s_dk_voices = NULL;
    s_dk_voices_cap = 0;
}

bool al_install_audio(void) {
    s_audio_ok = audio_init();
    return true;
}

bool al_init_acodec_addon(void) { return true; }
bool al_reserve_samples(int reserve_samples) { (void)reserve_samples; return true; }

ALLEGRO_SAMPLE *al_load_sample(const char *filename) {
    FILE *f = open_dk_asset(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 12) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    if (nr < 12u || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return NULL;
    }

    uint16_t audio_fmt = 1u, ch = 1u, bits = 16u;
    uint32_t rate = 22050u;
    bool have_fmt = false;
    const uint8_t *data_ptr = NULL;
    size_t data_bytes = 0u;

    size_t off = 12u;
    while (off + 8u <= nr) {
        const uint8_t *hdr = buf + off;
        uint32_t csz = dk_rd32(hdr + 4u);
        size_t payload_off = off + 8u;
        size_t avail = (payload_off <= nr) ? (nr - payload_off) : 0u;
        size_t chunk_len = ((size_t)csz <= avail) ? (size_t)csz : avail;
        if (memcmp(hdr, "fmt ", 4) == 0 && chunk_len >= 16u) {
            audio_fmt = dk_rd16(buf + payload_off + 0u);
            ch        = dk_rd16(buf + payload_off + 2u);
            rate      = dk_rd32(buf + payload_off + 4u);
            bits      = dk_rd16(buf + payload_off + 14u);
            have_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0 && chunk_len > 0u) {
            data_ptr = buf + payload_off;
            data_bytes = chunk_len;
        }
        size_t next_off = payload_off + (((size_t)csz + 1u) & ~1u);
        if (next_off <= off) break;
        off = next_off;
    }

    if (!have_fmt || !data_ptr || audio_fmt != 1u || ch == 0u ||
        (bits != 8u && bits != 16u && bits != 24u && bits != 32u)) {
        free(buf);
        return NULL;
    }

    size_t bps = (size_t)bits / 8u;
    size_t frames = data_bytes / (bps * (size_t)ch);
    if (frames == 0u) {
        free(buf);
        return NULL;
    }

    ALLEGRO_SAMPLE *s = (ALLEGRO_SAMPLE *)calloc(1, sizeof(ALLEGRO_SAMPLE));
    if (!s) { free(buf); return NULL; }
    s->samples = (int16_t *)malloc(frames * (size_t)ch * sizeof(int16_t));
    if (!s->samples) { free(s); free(buf); return NULL; }

    size_t total_smp = frames * (size_t)ch;
    for (size_t i = 0u; i < total_smp; i++) {
        const uint8_t *sp = data_ptr + i * bps;
        int32_t v = 0;
        if (bits == 8u)       v = ((int32_t)sp[0] - 128) << 8;
        else if (bits == 16u) v = (int16_t)dk_rd16(sp);
        else if (bits == 24u) {
            int32_t x = (int32_t)((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16));
            if (x & 0x800000) x |= (int32_t)0xFF000000u;
            v = x >> 8;
        } else {
            v = (int32_t)dk_rd32(sp) >> 16;
        }
        s->samples[i] = (int16_t)v;
    }
    s->sample_rate = rate ? rate : 22050u;
    s->channels    = ch;
    s->num_samples = (uint32_t)frames;
    free(buf);
    return s;
}

void al_destroy_sample(ALLEGRO_SAMPLE *spl) {
    if (!spl) return;
    for (int i = 0; i < s_dk_voices_cap; i++) {
        if (s_dk_voices[i].active && s_dk_voices[i].spl == spl) {
            if (s_dk_voices[i].stream) {
                audio_stream_stop(s_dk_voices[i].stream);
                audio_stream_destroy(s_dk_voices[i].stream);
            }
            free(s_dk_voices[i].pan_buf);
            memset(&s_dk_voices[i], 0, sizeof(s_dk_voices[i]));
        }
    }
    free(spl->samples);
    free(spl);
}

bool al_play_sample(ALLEGRO_SAMPLE *spl, float gain, float pan, float speed,
                    ALLEGRO_PLAYMODE loop, ALLEGRO_SAMPLE_ID *ret_id) {
    if (!s_audio_ok || !spl || !spl->samples || spl->num_samples == 0u) return false;
    dk_reap_voices();

    int slot = -1;
    for (int i = 0; i < s_dk_voices_cap; i++) {
        if (!s_dk_voices[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        int ncap = s_dk_voices_cap ? (s_dk_voices_cap * 2) : 8;
        dk_voice_t *nv = (dk_voice_t *)realloc(s_dk_voices, (size_t)ncap * sizeof(dk_voice_t));
        if (!nv) return false;
        memset(nv + s_dk_voices_cap, 0, (size_t)(ncap - s_dk_voices_cap) * sizeof(dk_voice_t));
        s_dk_voices = nv;
        slot = s_dk_voices_cap;
        s_dk_voices_cap = ncap;
    }

    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    if (speed <= 0.05f) speed = 1.0f;

    uint16_t src_ch = spl->channels ? spl->channels : 1u;
    const int16_t *pcm = spl->samples;
    int play_ch = (int)src_ch;
    int16_t *pan_buf = NULL;

    if (pan < -0.02f || pan > 0.02f) {
        if (pan < -1.0f) pan = -1.0f;
        if (pan > 1.0f) pan = 1.0f;
        float gl = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
        float gr = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
        pan_buf = (int16_t *)malloc((size_t)spl->num_samples * 2u * sizeof(int16_t));
        if (pan_buf) {
            for (uint32_t i = 0u; i < spl->num_samples; i++) {
                int16_t sl = spl->samples[i * src_ch];
                int16_t sr = (src_ch >= 2u) ? spl->samples[i * src_ch + 1u] : sl;
                pan_buf[i * 2u]      = (int16_t)((float)sl * gl);
                pan_buf[i * 2u + 1u] = (int16_t)((float)sr * gr);
            }
            pcm = pan_buf;
            play_ch = 2;
        }
    }

    uint32_t eff_rate = (uint32_t)((float)(spl->sample_rate ? spl->sample_rate : 22050u) * speed);
    if (eff_rate < 100u) eff_rate = 100u;

    audio_stream_t *st = audio_stream_create(AUDIO_FORMAT_S16, play_ch, eff_rate, pcm,
                                             (size_t)spl->num_samples * (size_t)play_ch * sizeof(int16_t));
    if (!st) {
        free(pan_buf);
        return false;
    }

    audio_stream_set_volume(st, gain);
    audio_stream_play(st);

    int vid = s_dk_next_id++;
    if (s_dk_next_id <= 0) s_dk_next_id = 1;
    dk_voice_t *v = &s_dk_voices[slot];
    v->active  = true;
    v->loop    = (loop == ALLEGRO_PLAYMODE_LOOP);
    v->id      = vid;
    v->spl     = spl;
    v->gain    = gain;
    v->speed   = speed;
    v->stream  = st;
    v->pan_buf = pan_buf;

    if (ret_id) {
        ret_id->_id = vid;
        ret_id->_index = slot;
    }
    return true;
}

void al_stop_sample(ALLEGRO_SAMPLE_ID *spl_id) {
    if (!spl_id) return;
    int idx = spl_id->_index;
    if (idx >= 0 && idx < s_dk_voices_cap &&
        s_dk_voices[idx].active && s_dk_voices[idx].id == spl_id->_id) {
        if (s_dk_voices[idx].stream) {
            audio_stream_stop(s_dk_voices[idx].stream);
            audio_stream_destroy(s_dk_voices[idx].stream);
        }
        free(s_dk_voices[idx].pan_buf);
        memset(&s_dk_voices[idx], 0, sizeof(s_dk_voices[idx]));
    }
}

/* =========================================================================
 * Display, Timer & Event Queue (60 FPS Target)
 * ========================================================================= */
void al_set_new_display_flags(int flags) { (void)flags; }

ALLEGRO_DISPLAY *al_create_display(int w, int h) {
    (void)w; (void)h;
    ALLEGRO_DISPLAY *d = (ALLEGRO_DISPLAY *)calloc(1, sizeof(ALLEGRO_DISPLAY));
    if (!d) return NULL;
    /* Keep 4:3 virtual display >= 224x256 so world_bitmap draws 1:1 at 224x256 */
    d->w = 342;
    d->h = 256;
    d->backbuffer = al_create_bitmap(d->w, d->h);
    if (!d->backbuffer) { free(d); return NULL; }

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    {
        wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
        opt.title = "Donkey Kong (1981)";
        opt.bg_color = 0;
        opt.fullscreen = true;
        opt.w = (uint16_t)sw;
        opt.h = (uint16_t)sh;
        opt.x = 0;
        opt.y = 0;
        d->wm_win = wm_create_window(&opt);
    }
    if (d->wm_win) {
        wm_set_fullscreen(d->wm_win, true);
        wm_set_cursor_visible(d->wm_win, false);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        d->direct_gfx = true;
    }

    ipo_hd_setup(&d->hd, d->wm_win, d->w, d->h, true);
    s_cur_display = d;
    s_cur_target = d->backbuffer;
    return d;
}

void al_destroy_display(ALLEGRO_DISPLAY *display) {
    if (!display) return;
    dk_stop_all_voices();
    if (display->wm_win && wm_is_window_valid(display->wm_win)) {
        wm_set_cursor_visible(display->wm_win, true);
        wm_destroy_window(display->wm_win);
    } else if (display->direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    ipo_hd_free(&display->hd);
    al_destroy_bitmap(display->backbuffer);
    if (s_cur_display == display) s_cur_display = NULL;
    free(display);
}

int al_get_display_width(ALLEGRO_DISPLAY *display) { return display ? display->w : 342; }
int al_get_display_height(ALLEGRO_DISPLAY *display) { return display ? display->h : 256; }
bool al_acknowledge_resize(ALLEGRO_DISPLAY *display) { (void)display; return true; }
ALLEGRO_EVENT_SOURCE *al_get_display_event_source(ALLEGRO_DISPLAY *display) {
    static ALLEGRO_EVENT_SOURCE s_disp_src = { 3, NULL };
    s_disp_src.owner = display;
    return &s_disp_src;
}
ALLEGRO_EVENT_SOURCE *al_get_keyboard_event_source(void) { return &s_kbd_src; }

void al_flip_display(void) {
    if (!s_cur_display || !s_cur_display->backbuffer) return;
    ALLEGRO_BITMAP *bb = s_cur_display->backbuffer;

    init_wm_palette_lut();
    if (!ipo_hd_present_pal32(&s_cur_display->hd, s_cur_display->wm_win, s_cur_display->direct_gfx,
                              bb->px, bb->w, bb->h, bb->w, s_wm_pal32)) {
        ipo_hd_present(&s_cur_display->hd, s_cur_display->wm_win, s_cur_display->direct_gfx,
                       bb->px, bb->w, bb->h, bb->w);
    }
    if (s_audio_ok) {
        dk_reap_voices();
        audio_tick();
    }

    static uint32_t s_fps_t0 = 0;
    static uint32_t s_fps_frames = 0;
    uint32_t now = timer_millis();
    if (s_fps_t0 == 0u) s_fps_t0 = now;
    s_fps_frames++;
    uint32_t elapsed = now - s_fps_t0;
    if (elapsed >= 1000u) {
        serial_printf("[donkey_kong] %u FPS\n", (s_fps_frames * 1000u) / elapsed);
        s_fps_frames = 0;
        s_fps_t0 = now;
    }
}

ALLEGRO_TIMER *al_create_timer(double speed_secs) {
    ALLEGRO_TIMER *t = (ALLEGRO_TIMER *)calloc(1, sizeof(ALLEGRO_TIMER));
    if (!t) return NULL;
    t->src.kind = 2;
    t->src.owner = t;
    uint32_t ms = (uint32_t)(speed_secs * 1000.0 + 0.5);
    t->period_ms = (ms > 0u) ? ms : 16u;
    return t;
}

void al_destroy_timer(ALLEGRO_TIMER *timer) { free(timer); }
void al_start_timer(ALLEGRO_TIMER *timer) {
    if (!timer) return;
    timer->started = true;
    timer->next_tick_ms = timer_millis() + timer->period_ms;
}
ALLEGRO_EVENT_SOURCE *al_get_timer_event_source(ALLEGRO_TIMER *timer) { return timer ? &timer->src : NULL; }

ALLEGRO_EVENT_QUEUE *al_create_event_queue(void) {
    ALLEGRO_EVENT_QUEUE *q = (ALLEGRO_EVENT_QUEUE *)calloc(1, sizeof(ALLEGRO_EVENT_QUEUE));
    if (!q) return NULL;
    q->cap = 128;
    q->items = (ALLEGRO_EVENT *)calloc((size_t)q->cap, sizeof(ALLEGRO_EVENT));
    if (!q->items) { free(q); return NULL; }
    return q;
}
void al_destroy_event_queue(ALLEGRO_EVENT_QUEUE *queue) {
    if (!queue) return;
    free(queue->items);
    free(queue);
}

void al_register_event_source(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT_SOURCE *source) {
    if (!queue || !source) return;
    if (source->kind == 1) queue->has_kbd = true;
    else if (source->kind == 2) queue->timer = (ALLEGRO_TIMER *)source->owner;
    else if (source->kind == 3) queue->display = (ALLEGRO_DISPLAY *)source->owner;
}

static void evq_push(ALLEGRO_EVENT_QUEUE *q, const ALLEGRO_EVENT *ev) {
    if (!q || q->cap <= 0 || !q->items) return;
    int next = (q->tail + 1) % q->cap;
    if (next == q->head) {
        int new_cap = q->cap * 2;
        if (new_cap <= q->cap || (size_t)new_cap > SIZE_MAX / sizeof(ALLEGRO_EVENT)) return;
        ALLEGRO_EVENT *ni = (ALLEGRO_EVENT *)malloc((size_t)new_cap * sizeof(ALLEGRO_EVENT));
        if (!ni) return;
        int n = 0;
        for (int idx = q->head; idx != q->tail; idx = (idx + 1) % q->cap) {
            ni[n++] = q->items[idx];
        }
        free(q->items);
        q->items = ni;
        q->head = 0;
        q->tail = n;
        q->cap = new_cap;
        next = q->tail + 1;
    }
    q->items[q->tail] = *ev;
    q->tail = next;
}

static void pump_sources(ALLEGRO_EVENT_QUEUE *q) {
    dk_reap_voices();
    if (q->display && q->display->direct_gfx && system_is_interrupted()) {
        system_clear_interrupt();
        ALLEGRO_EVENT ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
        evq_push(q, &ev);
    }

    if (q->has_kbd) {
        if (q->display && q->display->wm_win) {
            if (!wm_is_window_valid(q->display->wm_win) || !wm_session_active()) {
                ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
                ev.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
                evq_push(q, &ev);
            } else {
                wm_event_t we;
                while (wm_poll_event(q->display->wm_win, &we)) {
                    if (we.type == WM_EVENT_CLOSE) {
                        ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
                        ev.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
                        evq_push(q, &ev);
                    } else if (we.type == WM_EVENT_KEY_EXT) {
                        bool rel = (we.data & 0x80000000u) != 0u;
                        uint32_t kc = we.data & 0x7FFFFFFFu;
                        bool e0 = ((kc & 0xFF00u) == 0xE000u);
                        int ak = ps2_to_al_key((uint8_t)(kc & 0x7Fu), e0);
                        if (ak != 0) {
                            ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
                            ev.type = rel ? ALLEGRO_EVENT_KEY_UP : ALLEGRO_EVENT_KEY_DOWN;
                            ev.keyboard.keycode = ak;
                            evq_push(q, &ev);
                        }
                    } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                        static bool s_wm_e0 = false;
                        uint8_t raw = (uint8_t)(we.data & 0xFFu);
                        if (raw == 0xE0u) { s_wm_e0 = true; continue; }
                        bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                        int ak = ps2_to_al_key(raw & 0x7Fu, s_wm_e0);
                        s_wm_e0 = false;
                        if (ak != 0) {
                            ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
                            ev.type = rel ? ALLEGRO_EVENT_KEY_UP : ALLEGRO_EVENT_KEY_DOWN;
                            ev.keyboard.keycode = ak;
                            evq_push(q, &ev);
                        }
                    }
                }
            }
        } else {
            static bool s_dir_e0 = false;
            uint8_t raw;
            keyboard_poll();
            while ((raw = keyboard_get_scancode()) != 0u) {
                if (raw == 0xE0u) { s_dir_e0 = true; continue; }
                bool rel = (raw & 0x80u) != 0u;
                int ak = ps2_to_al_key(raw & 0x7Fu, s_dir_e0);
                s_dir_e0 = false;
                if (ak != 0) {
                    ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
                    ev.type = rel ? ALLEGRO_EVENT_KEY_UP : ALLEGRO_EVENT_KEY_DOWN;
                    ev.keyboard.keycode = ak;
                    evq_push(q, &ev);
                }
            }
        }
    }

    if (q->timer && q->timer->started) {
        uint32_t now = timer_millis();
        int pumped = 0;
        while ((int32_t)(now - q->timer->next_tick_ms) >= 0 && pumped < 5) {
            q->timer->count++;
            q->timer->next_tick_ms += q->timer->period_ms;
            ALLEGRO_EVENT ev; memset(&ev, 0, sizeof(ev));
            ev.type = ALLEGRO_EVENT_TIMER;
            ev.timer.source = q->timer;
            ev.timer.count = q->timer->count;
            evq_push(q, &ev);
            pumped++;
        }
        if ((int32_t)(now - q->timer->next_tick_ms) > 64) {
            q->timer->next_tick_ms = now + q->timer->period_ms;
        }
    }
}

bool al_event_queue_is_empty(ALLEGRO_EVENT_QUEUE *queue) {
    if (!queue) return true;
    pump_sources(queue);
    return queue->head == queue->tail;
}

void al_wait_for_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event) {
    if (!queue || !ret_event) return;
    for (;;) {
        pump_sources(queue);
        if (queue->head != queue->tail) {
            *ret_event = queue->items[queue->head];
            queue->head = (queue->head + 1) % queue->cap;
            return;
        }
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}
