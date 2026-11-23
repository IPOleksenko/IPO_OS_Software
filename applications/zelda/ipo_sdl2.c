#include "compat_sdl2/SDL.h"
#include "compat_sdl2/SDL_image.h"
#include "compat_sdl2/SDL_mixer.h"
#include "compat_sdl2/SDL_ttf.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/input/keyboard.h>
#include <driver/sound.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>

#include "common/ipo_hd_present.h"

extern FILE *zelda_open_asset(const char *path, const char *mode, int map_ogg_to_wav);
extern uint8_t *ns_image_wuffs_decode_to_bgra(const uint8_t *src, size_t len,
                                              int *out_w, int *out_h,
                                              size_t *out_stride, size_t *out_len);

void *ns_texture_new(int w, int h, int fmt, void *bytes, size_t stride, size_t len) {
    (void)w; (void)h; (void)fmt; (void)bytes; (void)stride; (void)len;
    return NULL;
}

#define TRANSP_IDX         255u
#define AUDIO_FREQ         22050u
#define AUDIO_RING_SAMPLES 16384u

struct SDL_Texture {
    int w, h;
    uint8_t *px; /* 0..247 WM palette index, or 255 = transparent */
    uint8_t mod_r, mod_g, mod_b;
    uint8_t alpha;
};

struct SDL_Window {
    int game_w, game_h;
    wm_window_t *wm_win;
    bool direct_gfx;
    ipo_hd_view_t hd;
    SDL_Renderer *rend;
};

struct SDL_Renderer {
    SDL_Window *win;
    int w, h;
    uint8_t *buf;
    uint8_t draw_r, draw_g, draw_b, draw_a;
    uint8_t draw_idx;
    SDL_BlendMode blend;
};

struct Mix_Music {
    int16_t *samples;
    uint32_t count;
};

struct Mix_Chunk {
    int16_t *samples;
    uint32_t count;
    int volume;
};

struct TTF_Font {
    int ptsize;
};

typedef struct {
    const Mix_Chunk *chunk;
    uint32_t pos;
    int loops;
    int volume;
    bool active;
} sfx_voice_t;

static const SDL_PixelFormat s_fmt_bgra32 = { 32u };
static uint8_t s_wm_palette[248][3];
static uint32_t s_wm_xrgb[256];
static bool s_pal_ready = false;

static SDL_Window *s_main_win = NULL;
static bool s_ctrl_down = false;
static char *s_last_err = NULL;

static void sdl_set_err(const char *msg) {
    if (!msg) msg = "";
    size_t n = strlen(msg) + 1u;
    char *p = (char *)realloc(s_last_err, n);
    if (p) {
        memcpy(p, msg, n);
        s_last_err = p;
    }
}

static SDL_Event *s_ev_q = NULL;
static int s_ev_cap = 0;
static int s_ev_head = 0;
static int s_ev_tail = 0;

/* Audio state */
static bool s_audio_ready = false;
static bool s_audio_inited = false;
static audio_stream_t *s_astream = NULL;
static int16_t s_audio_ring[AUDIO_RING_SAMPLES];
static uint32_t s_audio_write_samples = 0;
static uint32_t s_audio_start_ms = 0;
static uint64_t s_audio_total_written = 0;

static const Mix_Music *s_cur_music = NULL;
static uint32_t s_mus_pos = 0;
static int s_mus_loops = 0;
static bool s_mus_playing = false;
static int s_mus_volume = 128;
static sfx_voice_t *s_voices = NULL;
static int s_voices_cap = 0;

/* =========================================================================
 * Compact 8x8 ASCII Font (ASCII 0x20..0x5F + lowercase folding)
 * ========================================================================= */
static const uint8_t s_font_8x8[64][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* '\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}  /* '_' */
};

static void init_wm_palette_lut(void) {
    if (s_pal_ready) return;
    static const uint8_t cga[16][3] = {
        {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
        {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
        { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
        {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255}
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
    for (int i = 0; i < 248; i++) {
        s_wm_xrgb[i] = ((uint32_t)s_wm_palette[i][0] << 16) |
                       ((uint32_t)s_wm_palette[i][1] << 8) |
                       (uint32_t)s_wm_palette[i][2];
    }
    for (int i = 248; i < 256; i++) {
        s_wm_xrgb[i] = 0u;
    }
    s_pal_ready = true;
}

static inline uint8_t rgb_to_wm_fast(uint8_t r, uint8_t g, uint8_t b) {
    int ri = ((int)r * 5 + 127) / 255;
    int gi = ((int)g * 5 + 127) / 255;
    int bi = ((int)b * 5 + 127) / 255;
    return (uint8_t)(32 + (ri * 6 + gi) * 6 + bi);
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

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mix_audio_pump(void) {
    if (!s_astream) return;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    uint64_t played = 0ULL;
    if (ipo_audio_get_stream_clock(s_astream, &clk) == IPO_AUDIO_OK) {
        played = clk.played_frames;
    } else {
        uint32_t now = timer_millis();
        played = ((uint64_t)(now - s_audio_start_ms) * AUDIO_FREQ) / 1000ULL;
    }
    uint64_t target_written = played + (AUDIO_FREQ / 15u);
    if (target_written <= s_audio_total_written) {
        audio_tick();
        return;
    }
    uint64_t todo = target_written - s_audio_total_written;
    uint64_t ahead = (s_audio_total_written > played) ? (s_audio_total_written - played) : 0ULL;
    uint64_t max_safe = (AUDIO_RING_SAMPLES > ahead + 128u) ? (AUDIO_RING_SAMPLES - ahead - 128u) : 0ULL;
    if (todo > max_safe) todo = max_safe;
    if (todo > 2048u) todo = 2048u;
    if (todo == 0u) { audio_tick(); return; }

    for (uint64_t i = 0; i < todo; i++) {
        int32_t acc = 0;
        if (s_mus_playing && s_cur_music && s_cur_music->count > 0) {
            if (s_mus_pos >= s_cur_music->count) {
                if (s_mus_loops != 0) {
                    if (s_mus_loops > 0) s_mus_loops--;
                    s_mus_pos = 0;
                } else {
                    s_mus_playing = false;
                }
            }
            if (s_mus_playing) {
                acc += ((int32_t)s_cur_music->samples[s_mus_pos++] * s_mus_volume) >> 7;
            }
        }
        for (int v = 0; v < s_voices_cap; v++) {
            sfx_voice_t *vc = &s_voices[v];
            if (!vc->active || !vc->chunk || vc->chunk->count == 0) continue;
            if (vc->pos >= vc->chunk->count) {
                if (vc->loops != 0) {
                    if (vc->loops > 0) vc->loops--;
                    vc->pos = 0;
                } else {
                    vc->active = false;
                    continue;
                }
            }
            acc += ((int32_t)vc->chunk->samples[vc->pos++] * vc->volume) >> 7;
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        s_audio_ring[s_audio_write_samples % AUDIO_RING_SAMPLES] = (int16_t)acc;
        s_audio_write_samples++;
        s_audio_total_written++;
    }
    audio_stream_update_buffered(s_astream, s_audio_write_samples, false);
    audio_tick();
}

/* =========================================================================
 * SDL Core / Window / Renderer
 * ========================================================================= */
int SDL_Init(Uint32 flags) {
    (void)flags;
    init_wm_palette_lut();
    return 0;
}

void SDL_Quit(void) {
    Mix_CloseAudio();
    if (s_main_win) {
        SDL_DestroyWindow(s_main_win);
        s_main_win = NULL;
    }
    free(s_ev_q); s_ev_q = NULL; s_ev_cap = 0; s_ev_head = 0; s_ev_tail = 0;
    free(s_voices); s_voices = NULL; s_voices_cap = 0;
    free(s_last_err); s_last_err = NULL;
}

const char *SDL_GetError(void) {
    return s_last_err ? s_last_err : "";
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x; (void)y; (void)flags;
    init_wm_palette_lut();

    SDL_Window *win = (SDL_Window *)calloc(1, sizeof(SDL_Window));
    if (!win) {
        sdl_set_err("Out of memory");
        return NULL;
    }
    win->game_w = (w > 0) ? w : 768;
    win->game_h = (h > 0) ? h : 672;

    int sw = 320, sh = 200;
    wm_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    bool want_fullscreen = ((flags & SDL_WINDOW_FULLSCREEN) != 0u);
    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    int win_w = 216;
    int win_h = 152;
    if (win_w > sw - 12) win_w = sw - 12;
    if (win_h > sh - 36) win_h = sh - 36;
    int win_x = 18 + (int)((pid * 20u) % 72u);
    int win_y = 14 + (int)((pid * 10u) % 24u);
    if (win_x + win_w > sw - 4) win_x = (sw - win_w) / 2;
    if (win_y + win_h > sh - 24) win_y = 14;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = title ? title : "The Legend of Zelda";
    opt.decor_style = WM_DECOR_DEFAULT;
    opt.bg_color = 0;
    opt.fullscreen = want_fullscreen;
    opt.w = (uint16_t)win_w;
    opt.h = (uint16_t)win_h;
    opt.x = (int16_t)win_x;
    opt.y = (int16_t)win_y;

    win->wm_win = wm_create_window(&opt);
    if (win->wm_win) {
        wm_set_focus(win->wm_win);
        wm_set_fullscreen(win->wm_win, want_fullscreen);
        wm_set_cursor_visible(win->wm_win, true);
        wm_invalidate(win->wm_win);
        wm_compositor_tick();
        if (win->wm_win->w > 0 && win->wm_win->h > 0) {
            sw = (int)win->wm_win->w;
            sh = (int)win->wm_win->h;
        }
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        win->direct_gfx = true;
    }

    ipo_hd_setup(&win->hd, win->wm_win, win->game_w, win->game_h, false);
    if (win->wm_win) {
        /* Paint initial dark frame so window appears immediately during asset load */
        memset(win->wm_win->framebuf, 0, (size_t)win->wm_win->w * (size_t)win->wm_win->h);
        wm_invalidate(win->wm_win);
        wm_compositor_tick();
    }
    s_main_win = win;
    return win;
}

void SDL_DestroyWindow(SDL_Window *window) {
    if (!window) return;
    if (window->wm_win && wm_is_window_valid(window->wm_win)) {
        wm_set_cursor_visible(window->wm_win, true);
        wm_destroy_window(window->wm_win);
        window->wm_win = NULL;
    } else if (window->direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
        window->direct_gfx = false;
    }
    ipo_hd_free(&window->hd);
    if (s_main_win == window) s_main_win = NULL;
    free(window);
}

void SDL_SetWindowSize(SDL_Window *window, int w, int h) {
    if (!window || w <= 0 || h <= 0) return;
    window->game_w = w;
    window->game_h = h;
    ipo_hd_setup(&window->hd, window->wm_win, w, h, false);
    if (window->rend) {
        uint8_t *nbuf = (uint8_t *)calloc((size_t)w * (size_t)h, 1u);
        if (nbuf) {
            free(window->rend->buf);
            window->rend->buf = nbuf;
            window->rend->w = w;
            window->rend->h = h;
        }
    }
    serial_printf("[zelda-win] game=%dx%d scr=%dx%d pres=(%d,%d %dx%d) k=%d\n",
                  window->game_w, window->game_h, window->hd.scr_w, window->hd.scr_h,
                  window->hd.pres_x, window->hd.pres_y, window->hd.pres_w, window->hd.pres_h,
                  window->hd.scale_k);
}

void SDL_SetWindowTitle(SDL_Window *window, const char *title) {
    (void)window; (void)title;
}

void SDL_SetWindowPosition(SDL_Window *window, int x, int y) {
    (void)window; (void)x; (void)y;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags) {
    (void)index; (void)flags;
    if (!window) return NULL;
    SDL_Renderer *r = (SDL_Renderer *)calloc(1, sizeof(SDL_Renderer));
    if (!r) return NULL;
    r->win = window;
    r->w = window->game_w;
    r->h = window->game_h;
    r->buf = (uint8_t *)calloc((size_t)r->w * (size_t)r->h, 1u);
    if (!r->buf) { free(r); return NULL; }
    r->draw_idx = rgb_to_wm_fast(0, 0, 0);
    window->rend = r;
    return r;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {
    if (!renderer) return;
    if (renderer->win && renderer->win->rend == renderer) renderer->win->rend = NULL;
    free(renderer->buf);
    free(renderer);
}

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!renderer) return -1;
    renderer->draw_r = r;
    renderer->draw_g = g;
    renderer->draw_b = b;
    renderer->draw_a = a;
    renderer->draw_idx = rgb_to_wm_fast(r, g, b);
    return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode) {
    if (!renderer) return -1;
    renderer->blend = blendMode;
    return 0;
}

int SDL_RenderClear(SDL_Renderer *renderer) {
    if (!renderer || !renderer->buf) return -1;
    memset(renderer->buf, renderer->draw_idx, (size_t)renderer->w * (size_t)renderer->h);
    return 0;
}

static void draw_hline(SDL_Renderer *r, int x1, int x2, int y) {
    if (y < 0 || y >= r->h) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (x2 < 0 || x1 >= r->w) return;
    if (x1 < 0) x1 = 0;
    if (x2 >= r->w) x2 = r->w - 1;
    memset(r->buf + (size_t)y * (size_t)r->w + (size_t)x1, r->draw_idx, (size_t)(x2 - x1 + 1));
}

int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->buf) return -1;
    if (!rect) return SDL_RenderClear(renderer);
    int x0 = rect->x, y0 = rect->y;
    int x1 = rect->x + rect->w, y1 = rect->y + rect->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > renderer->w) x1 = renderer->w;
    if (y1 > renderer->h) y1 = renderer->h;
    if (x0 >= x1 || y0 >= y1) return 0;
    for (int y = y0; y < y1; y++) {
        memset(renderer->buf + (size_t)y * (size_t)renderer->w + (size_t)x0,
               renderer->draw_idx, (size_t)(x1 - x0));
    }
    return 0;
}

int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->buf || !rect || rect->w <= 0 || rect->h <= 0) return -1;
    int x0 = rect->x, y0 = rect->y;
    int x1 = rect->x + rect->w - 1, y1 = rect->y + rect->h - 1;
    draw_hline(renderer, x0, x1, y0);
    draw_hline(renderer, x0, x1, y1);
    for (int y = y0; y <= y1; y++) {
        if (y >= 0 && y < renderer->h) {
            if (x0 >= 0 && x0 < renderer->w) renderer->buf[(size_t)y * (size_t)renderer->w + (size_t)x0] = renderer->draw_idx;
            if (x1 >= 0 && x1 < renderer->w) renderer->buf[(size_t)y * (size_t)renderer->w + (size_t)x1] = renderer->draw_idx;
        }
    }
    return 0;
}

int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2) {
    if (!renderer || !renderer->buf) return -1;
    int dx = abs(x2 - x1), sx = (x1 < x2) ? 1 : -1;
    int dy = -abs(y2 - y1), sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if ((unsigned)x1 < (unsigned)renderer->w && (unsigned)y1 < (unsigned)renderer->h) {
            renderer->buf[(size_t)y1 * (size_t)renderer->w + (size_t)x1] = renderer->draw_idx;
        }
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *renderer) {
    if (!renderer || !renderer->buf || !renderer->win) return;
    SDL_Window *win = renderer->win;

    if (!ipo_hd_present_pal32(&win->hd, win->wm_win, win->direct_gfx,
                              renderer->buf, renderer->w, renderer->h, renderer->w,
                              s_wm_xrgb)) {
        ipo_hd_present(&win->hd, win->wm_win, win->direct_gfx,
                       renderer->buf, renderer->w, renderer->h, renderer->w);
    }
    if (win->wm_win) {
        wm_compositor_tick();
    }

    mix_audio_pump();

    static uint32_t s_fps_t0 = 0;
    static uint32_t s_fps_frames = 0;
    uint32_t now = timer_millis();
    if (s_fps_t0 == 0u) s_fps_t0 = now;
    s_fps_frames++;
    uint32_t elapsed = now - s_fps_t0;
    if (elapsed >= 1000u) {
        serial_printf("[zelda] %u FPS\n", (s_fps_frames * 1000u) / elapsed);
        s_fps_frames = 0;
        s_fps_t0 = now;
    }
}

/* =========================================================================
 * PNG / TTF / BMP Surface & Texture Blitting
 * ========================================================================= */
int IMG_Init(int flags) {
    return flags;
}

void IMG_Quit(void) {}

const char *IMG_GetError(void) {
    return s_last_err ? s_last_err : "";
}

SDL_Surface *IMG_Load(const char *file) {
    FILE *f = zelda_open_asset(file, "rb", 0);
    if (!f) {
        sdl_set_err("Cannot open image");
        return NULL;
    }
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
        sdl_set_err("PNG decode failed");
        return NULL;
    }

    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) { free(bgra); return NULL; }
    s->w = w;
    s->h = h;
    s->pitch = (int)stride;
    s->format = &s_fmt_bgra32;
    s->pixels = bgra;
    return s;
}

SDL_Surface *SDL_LoadBMP(const char *file) {
    (void)file;
    return NULL;
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    free(surface->pixels);
    free(surface);
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b) {
    (void)format;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key) {
    if (!surface) return -1;
    surface->has_colorkey = (flag != 0);
    surface->ck_r = (uint8_t)((key >> 16) & 0xFFu);
    surface->ck_g = (uint8_t)((key >> 8) & 0xFFu);
    surface->ck_b = (uint8_t)(key & 0xFFu);
    return 0;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface) {
    (void)renderer;
    if (!surface || !surface->pixels || surface->w <= 0 || surface->h <= 0) return NULL;
    init_wm_palette_lut();

    SDL_Texture *tex = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!tex) return NULL;
    tex->w = surface->w;
    tex->h = surface->h;
    tex->mod_r = 255;
    tex->mod_g = 255;
    tex->mod_b = 255;
    tex->alpha = 255;
    tex->px = (uint8_t *)malloc((size_t)tex->w * (size_t)tex->h);
    if (!tex->px) { free(tex); return NULL; }

    const uint8_t *src = (const uint8_t *)surface->pixels;
    for (int y = 0; y < tex->h; y++) {
        const uint8_t *srow = src + (size_t)y * (size_t)surface->pitch;
        uint8_t *drow = tex->px + (size_t)y * (size_t)tex->w;
        for (int x = 0; x < tex->w; x++) {
            uint8_t b = srow[x * 4 + 0];
            uint8_t g = srow[x * 4 + 1];
            uint8_t r = srow[x * 4 + 2];
            uint8_t a = srow[x * 4 + 3];
            if (a < 128u || (surface->has_colorkey &&
                             r == surface->ck_r && g == surface->ck_g && b == surface->ck_b)) {
                drow[x] = TRANSP_IDX;
            } else {
                drow[x] = rgb_to_wm_fast(r, g, b);
            }
        }
    }
    return tex;
}

void SDL_DestroyTexture(SDL_Texture *texture) {
    if (!texture) return;
    free(texture->px);
    free(texture);
}

int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h) {
    if (!texture) return -1;
    if (format) *format = 32u;
    if (access) *access = 0;
    if (w) *w = texture->w;
    if (h) *h = texture->h;
    return 0;
}

int SDL_SetTextureColorMod(SDL_Texture *texture, Uint8 r, Uint8 g, Uint8 b) {
    if (!texture) return -1;
    texture->mod_r = r;
    texture->mod_g = g;
    texture->mod_b = b;
    return 0;
}

int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha) {
    if (!texture) return -1;
    texture->alpha = alpha;
    return 0;
}

static int blit_texture_rotated(SDL_Renderer *renderer, SDL_Texture *texture,
                                const SDL_Rect *srcrect, const SDL_Rect *dstrect,
                                double angle, bool flip_x, bool flip_y) {
    if (!renderer || !renderer->buf || !texture || !texture->px) return -1;
    if (texture->alpha == 0) return 0;

    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : texture->w;
    int sh = srcrect ? srcrect->h : texture->h;

    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int dw = dstrect ? dstrect->w : sw;
    int dh = dstrect ? dstrect->h : sh;

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;

    if (angle == 0.0) {
        if (dx + dw <= 0 || dx >= renderer->w || dy + dh <= 0 || dy >= renderer->h) {
            return 0;
        }
    }

    bool has_mod = (texture->mod_r != 255 || texture->mod_g != 255 || texture->mod_b != 255);
    uint8_t mod_lut[256];
    if (has_mod) {
        for (int i = 0; i < 248; i++) {
            uint8_t mr = (uint8_t)(((int)s_wm_palette[i][0] * (int)texture->mod_r) / 255);
            uint8_t mg = (uint8_t)(((int)s_wm_palette[i][1] * (int)texture->mod_g) / 255);
            uint8_t mb = (uint8_t)(((int)s_wm_palette[i][2] * (int)texture->mod_b) / 255);
            mod_lut[i] = rgb_to_wm_fast(mr, mg, mb);
        }
    }

    double norm_ang = (angle == 0.0) ? 0.0 : fmod(angle, 360.0);
    if (norm_ang < 0.0) norm_ang += 360.0;

    /* Fast path when no rotation or 180-deg flip */
    if (norm_ang < 0.5 || norm_ang > 359.5 || (norm_ang > 179.5 && norm_ang < 180.5)) {
        if (norm_ang > 179.5 && norm_ang < 180.5) {
            flip_x = !flip_x;
            flip_y = !flip_y;
        }
        int x_start = (dx < 0) ? -dx : 0;
        int y_start = (dy < 0) ? -dy : 0;
        int x_end = (dx + dw > renderer->w) ? (renderer->w - dx) : dw;
        int y_end = (dy + dh > renderer->h) ? (renderer->h - dy) : dh;
        if (x_start >= x_end || y_start >= y_end) return 0;

        /* Zero-division fast path for integer 3x sprites (Cpp-Zelda-2019 SCALE=3) */
        if (dw == sw * 3 && dh == sh * 3 && !has_mod) {
            int rw = renderer->w;
            for (int ty = 0; ty < sh; ty++) {
                int py0 = dy + ty * 3;
                if (py0 + 3 <= 0 || py0 >= renderer->h) continue;
                int iy = sy + (flip_y ? (sh - 1 - ty) : ty);
                if ((unsigned)iy >= (unsigned)texture->h) continue;
                const uint8_t *srow = texture->px + (size_t)iy * (size_t)texture->w;
                for (int tx = 0; tx < sw; tx++) {
                    int ix = sx + (flip_x ? (sw - 1 - tx) : tx);
                    if ((unsigned)ix >= (unsigned)texture->w) continue;
                    uint8_t c = srow[ix];
                    if (c == TRANSP_IDX) continue;
                    int px0 = dx + tx * 3;
                    if (px0 >= 0 && px0 + 3 <= rw && py0 >= 0 && py0 + 3 <= renderer->h) {
                        uint8_t *d0 = renderer->buf + (size_t)py0 * (size_t)rw + (size_t)px0;
                        uint8_t *d1 = d0 + rw;
                        uint8_t *d2 = d1 + rw;
                        d0[0] = c; d0[1] = c; d0[2] = c;
                        d1[0] = c; d1[1] = c; d1[2] = c;
                        d2[0] = c; d2[1] = c; d2[2] = c;
                    } else {
                        for (int syy = 0; syy < 3; syy++) {
                            int py = py0 + syy;
                            if ((unsigned)py >= (unsigned)renderer->h) continue;
                            uint8_t *d = renderer->buf + (size_t)py * (size_t)rw;
                            for (int sxx = 0; sxx < 3; sxx++) {
                                int px = px0 + sxx;
                                if ((unsigned)px < (unsigned)rw) d[px] = c;
                            }
                        }
                    }
                }
            }
            return 0;
        }

        for (int ry = y_start; ry < y_end; ry++) {
            int ty = (ry * sh) / dh;
            int iy = sy + (flip_y ? (sh - 1 - ty) : ty);
            if ((unsigned)iy >= (unsigned)texture->h) continue;
            const uint8_t *srow = texture->px + (size_t)iy * (size_t)texture->w;
            uint8_t *drow = renderer->buf + (size_t)(dy + ry) * (size_t)renderer->w + (size_t)dx;
            for (int rx = x_start; rx < x_end; rx++) {
                int tx = (rx * sw) / dw;
                int ix = sx + (flip_x ? (sw - 1 - tx) : tx);
                if ((unsigned)ix >= (unsigned)texture->w) continue;
                uint8_t c = srow[ix];
                if (c != TRANSP_IDX) drow[rx] = has_mod ? mod_lut[c] : c;
            }
        }
        return 0;
    }

    /* General rotation around dst center */
    double cx = (double)dx + 0.5 * (double)dw;
    double cy = (double)dy + 0.5 * (double)dh;
    double rad = norm_ang * (3.14159265358979323846 / 180.0);
    double ca = cos(rad);
    double sa = sin(rad);

    double hx = 0.5 * (double)dw;
    double hy = 0.5 * (double)dh;
    double extent = fabs(hx * ca) + fabs(hy * sa);
    double extent_y = fabs(hx * sa) + fabs(hy * ca);

    int bx0 = (int)floor(cx - extent) - 1;
    int bx1 = (int)ceil(cx + extent) + 1;
    int by0 = (int)floor(cy - extent_y) - 1;
    int by1 = (int)ceil(cy + extent_y) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 >= renderer->w) bx1 = renderer->w - 1;
    if (by1 >= renderer->h) by1 = renderer->h - 1;

    for (int py = by0; py <= by1; py++) {
        double ddy = (double)py + 0.5 - cy;
        uint8_t *drow = renderer->buf + (size_t)py * (size_t)renderer->w;
        for (int px = bx0; px <= bx1; px++) {
            double ddx = (double)px + 0.5 - cx;
            double lx =  ddx * ca + ddy * sa + hx;
            double ly = -ddx * sa + ddy * ca + hy;
            int tx_i = (int)floor(lx);
            int ty_i = (int)floor(ly);
            if ((unsigned)tx_i >= (unsigned)dw || (unsigned)ty_i >= (unsigned)dh) continue;
            int ix = sx + (flip_x ? (sw - 1 - (tx_i * sw) / dw) : ((tx_i * sw) / dw));
            int iy = sy + (flip_y ? (sh - 1 - (ty_i * sh) / dh) : ((ty_i * sh) / dh));
            if ((unsigned)ix >= (unsigned)texture->w || (unsigned)iy >= (unsigned)texture->h) continue;
            uint8_t c = texture->px[(size_t)iy * (size_t)texture->w + (size_t)ix];
            if (c != TRANSP_IDX) drow[px] = has_mod ? mod_lut[c] : c;
        }
    }
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
    return blit_texture_rotated(renderer, texture, srcrect, dstrect, 0.0, false, false);
}

int SDL_RenderCopyEx(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect,
                     const SDL_Rect *dstrect, double angle, const SDL_Point *center, SDL_RendererFlip flip) {
    (void)center;
    return blit_texture_rotated(renderer, texture, srcrect, dstrect, angle,
                                (flip & SDL_FLIP_HORIZONTAL) != 0,
                                (flip & SDL_FLIP_VERTICAL) != 0);
}

/* =========================================================================
 * TTF Text Rendering
 * ========================================================================= */
int TTF_Init(void) {
    return 0;
}

void TTF_Quit(void) {}

const char *TTF_GetError(void) {
    return s_last_err ? s_last_err : "";
}

TTF_Font *TTF_OpenFont(const char *file, int ptsize) {
    (void)file;
    TTF_Font *f = (TTF_Font *)calloc(1, sizeof(TTF_Font));
    if (!f) return NULL;
    f->ptsize = (ptsize > 0) ? ptsize : 24;
    return f;
}

void TTF_CloseFont(TTF_Font *font) {
    free(font);
}

SDL_Surface *TTF_RenderText_Solid(TTF_Font *font, const char *text, SDL_Color fg) {
    if (!text || !text[0]) return NULL;
    size_t len = strlen(text);
    int scale = (font && font->ptsize >= 8) ? (font->ptsize / 9) : 2;
    if (scale < 2) scale = 2;

    int w = (int)len * 8 * scale;
    int h = 8 * scale;
    uint8_t *bgra = (uint8_t *)calloc((size_t)w * (size_t)h * 4u, 1u);
    if (!bgra) return NULL;

    for (size_t ci = 0; ci < len; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        int gi = (ch >= 32 && ch < 96) ? (ch - 32) : 0;
        const uint8_t *glyph = s_font_8x8[gi];
        int ox = (int)ci * 8 * scale;
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if ((bits & (1u << col)) == 0u) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = row * scale + sy;
                    uint8_t *dst = bgra + ((size_t)py * (size_t)w + (size_t)(ox + col * scale)) * 4u;
                    for (int sx = 0; sx < scale; sx++) {
                        dst[sx * 4 + 0] = fg.b;
                        dst[sx * 4 + 1] = fg.g;
                        dst[sx * 4 + 2] = fg.r;
                        dst[sx * 4 + 3] = 255u;
                    }
                }
            }
        }
    }

    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) { free(bgra); return NULL; }
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    s->format = &s_fmt_bgra32;
    s->pixels = bgra;
    return s;
}

/* =========================================================================
 * Events / Timing
 * ========================================================================= */
static int ps2_to_sdl_sym(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return SDLK_UP;
            case 0x50: return SDLK_DOWN;
            case 0x4B: return SDLK_LEFT;
            case 0x4D: return SDLK_RIGHT;
            case 0x1D: return SDLK_RCTRL;
            case 0x1C: return SDLK_RETURN;
            default:   return SDLK_UNKNOWN;
        }
    }
    switch (sc) {
        case 0x01: return SDLK_ESCAPE;
        case 0x02: return SDLK_1; case 0x03: return SDLK_2; case 0x04: return SDLK_3;
        case 0x05: return SDLK_4; case 0x06: return SDLK_5; case 0x07: return SDLK_6;
        case 0x08: return SDLK_7; case 0x09: return SDLK_8; case 0x0A: return SDLK_9;
        case 0x0B: return SDLK_0;
        case 0x0E: return SDLK_BACKSPACE;
        case 0x0F: return SDLK_TAB;
        case 0x10: return SDLK_q; case 0x11: return SDLK_w; case 0x12: return SDLK_e;
        case 0x13: return SDLK_r; case 0x14: return SDLK_t; case 0x15: return SDLK_y;
        case 0x16: return SDLK_u; case 0x17: return SDLK_i; case 0x18: return SDLK_o;
        case 0x19: return SDLK_p;
        case 0x1C: return SDLK_RETURN;
        case 0x1D: return SDLK_LCTRL;
        case 0x1E: return SDLK_a; case 0x1F: return SDLK_s; case 0x20: return SDLK_d;
        case 0x21: return SDLK_f; case 0x22: return SDLK_g; case 0x23: return SDLK_h;
        case 0x24: return SDLK_j; case 0x25: return SDLK_k; case 0x26: return SDLK_l;
        case 0x2A: return SDLK_LSHIFT;
        case 0x2C: return SDLK_z; case 0x2D: return SDLK_x; case 0x2E: return SDLK_c;
        case 0x2F: return SDLK_v; case 0x30: return SDLK_b; case 0x31: return SDLK_n;
        case 0x32: return SDLK_m;
        case 0x36: return SDLK_RSHIFT;
        case 0x39: return SDLK_SPACE;
        default:   return SDLK_UNKNOWN;
    }
}

static void push_event(const SDL_Event *ev) {
    if (!s_ev_q) {
        s_ev_cap = 64;
        s_ev_q = (SDL_Event *)calloc((size_t)s_ev_cap, sizeof(SDL_Event));
        if (!s_ev_q) { s_ev_cap = 0; return; }
        s_ev_head = 0;
        s_ev_tail = 0;
    }
    int next = (s_ev_tail + 1) % s_ev_cap;
    if (next == s_ev_head) {
        int new_cap = s_ev_cap * 2;
        if (new_cap <= s_ev_cap || (size_t)new_cap > SIZE_MAX / sizeof(SDL_Event)) return;
        SDL_Event *nq = (SDL_Event *)malloc((size_t)new_cap * sizeof(SDL_Event));
        if (!nq) return;
        int n = 0;
        for (int idx = s_ev_head; idx != s_ev_tail; idx = (idx + 1) % s_ev_cap) {
            nq[n++] = s_ev_q[idx];
        }
        free(s_ev_q);
        s_ev_q = nq;
        s_ev_head = 0;
        s_ev_tail = n;
        s_ev_cap = new_cap;
        next = s_ev_tail + 1;
    }
    s_ev_q[s_ev_tail] = *ev;
    s_ev_tail = next;
}

static void push_key_event(int sym, bool pressed) {
    if (sym == SDLK_LCTRL || sym == SDLK_RCTRL) s_ctrl_down = pressed;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    if (pressed && s_ctrl_down && (sym == SDLK_c || sym == SDLK_q)) {
        ev.type = SDL_QUIT;
        push_event(&ev);
        return;
    }
    ev.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.timestamp = timer_millis();
    ev.key.keysym.sym = sym;
    ev.key.keysym.scancode = sym;
    push_event(&ev);
}

static void pump_os_events(void) {
    if (!s_main_win) return;
    if (s_main_win->wm_win) {
        if (!wm_is_window_valid(s_main_win->wm_win) || !wm_session_active()) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_QUIT;
            push_event(&ev);
            return;
        }
        wm_event_t we;
        static bool s_wm_e0 = false;
        while (wm_poll_event(s_main_win->wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                SDL_Event ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = SDL_QUIT;
                push_event(&ev);
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                int sym = ps2_to_sdl_sym((uint8_t)(kc & 0x7Fu), e0);
                if (sym != SDLK_UNKNOWN) push_key_event(sym, !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { s_wm_e0 = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                int sym = ps2_to_sdl_sym(raw & 0x7Fu, s_wm_e0);
                s_wm_e0 = false;
                if (sym != SDLK_UNKNOWN) push_key_event(sym, !rel);
            }
        }
    } else if (s_main_win->direct_gfx) {
        if (system_is_interrupted()) {
            system_clear_interrupt();
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_QUIT;
            push_event(&ev);
            return;
        }
        static bool s_dir_e0 = false;
        keyboard_poll();
        uint8_t raw;
        while ((raw = keyboard_get_scancode()) != 0u) {
            if (raw == 0xE0u) { s_dir_e0 = true; continue; }
            bool rel = (raw & 0x80u) != 0u;
            int sym = ps2_to_sdl_sym(raw & 0x7Fu, s_dir_e0);
            s_dir_e0 = false;
            if (sym != SDLK_UNKNOWN) push_key_event(sym, !rel);
        }
    }
}

int SDL_PollEvent(SDL_Event *event) {
    pump_os_events();
    if (s_ev_q && s_ev_head != s_ev_tail) {
        if (event) *event = s_ev_q[s_ev_head];
        s_ev_head = (s_ev_head + 1) % s_ev_cap;
        return 1;
    }
    if (event) memset(event, 0, sizeof(*event));
    return 0;
}

Uint32 SDL_GetTicks(void) {
    return timer_millis();
}

void SDL_Delay(Uint32 ms) {
    uint32_t target = timer_millis() + ms;
    while ((int32_t)(timer_millis() - target) < 0) {
        mix_audio_pump();
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

/* =========================================================================
 * SDL_mixer WAV Loader & Mixer
 * ========================================================================= */
int Mix_Init(int flags) {
    return flags;
}

void Mix_Quit(void) {}

const char *Mix_GetError(void) {
    return s_last_err ? s_last_err : "";
}

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)frequency; (void)format; (void)channels; (void)chunksize;
    if (s_audio_inited) return s_audio_ready ? 0 : -1;
    s_audio_inited = true;
    if (s_voices && s_voices_cap > 0) memset(s_voices, 0, (size_t)s_voices_cap * sizeof(sfx_voice_t));
    if (!audio_init()) return 0; /* Keep silent fallback if no sound device */
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    s_audio_write_samples = 0;
    s_audio_start_ms = timer_millis();
    s_audio_total_written = 0;
    s_astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1u, AUDIO_FREQ,
                                         s_audio_ring, AUDIO_RING_SAMPLES, 0u);
    if (s_astream) {
        audio_stream_play(s_astream);
        s_audio_ready = true;
    }
    return 0;
}

void Mix_CloseAudio(void) {
    if (s_astream) {
        audio_stream_stop(s_astream);
        audio_stream_destroy(s_astream);
        s_astream = NULL;
    }
    s_audio_ready = false;
    s_audio_inited = false;
}

static bool load_wav_pcm(const char *file, int16_t **out_samples, uint32_t *out_count) {
    FILE *f = zelda_open_asset(file, "rb", 1);
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 16 * 1024 * 1024) { fclose(f); return false; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return false; }
    fclose(f);

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return false;
    }

    uint16_t chans = 1, bps = 16;
    const uint8_t *pcm_ptr = NULL;
    uint32_t pcm_sz = 0;
    size_t pos = 12;
    while (pos + 8 <= (size_t)sz) {
        uint32_t csz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= (size_t)sz) {
            chans = rd_u16(buf + pos + 10);
            bps   = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0 && pos + 8 + csz <= (size_t)sz) {
            pcm_ptr = buf + pos + 8;
            pcm_sz  = csz;
        }
        pos += 8 + ((csz + 1u) & ~1u);
    }

    if (!pcm_ptr || pcm_sz == 0 || bps != 16 || chans == 0) {
        free(buf);
        return false;
    }

    uint32_t frames = pcm_sz / (2u * chans);
    int16_t *mono = (int16_t *)malloc((size_t)frames * sizeof(int16_t));
    if (!mono) { free(buf); return false; }
    const int16_t *in16 = (const int16_t *)pcm_ptr;
    for (uint32_t i = 0; i < frames; i++) {
        if (chans == 1) {
            mono[i] = in16[i];
        } else {
            mono[i] = (int16_t)(((int32_t)in16[i * chans] + (int32_t)in16[i * chans + 1]) >> 1);
        }
    }
    free(buf);
    *out_samples = mono;
    *out_count = frames;
    return true;
}

Mix_Music *Mix_LoadMUS(const char *file) {
    Mix_Music *m = (Mix_Music *)calloc(1, sizeof(Mix_Music));
    if (!m) return NULL;
    if (!load_wav_pcm(file, &m->samples, &m->count)) {
        free(m);
        return NULL;
    }
    return m;
}

Mix_Chunk *Mix_LoadWAV(const char *file) {
    Mix_Chunk *c = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!c) return NULL;
    c->volume = 128;
    if (!load_wav_pcm(file, &c->samples, &c->count)) {
        free(c);
        return NULL;
    }
    return c;
}

void Mix_FreeMusic(Mix_Music *music) {
    if (!music) return;
    if (s_cur_music == music) { s_cur_music = NULL; s_mus_playing = false; }
    free(music->samples);
    free(music);
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    for (int i = 0; i < s_voices_cap; i++) {
        if (s_voices[i].chunk == chunk) s_voices[i].active = false;
    }
    free(chunk->samples);
    free(chunk);
}

int Mix_PlayMusic(Mix_Music *music, int loops) {
    if (!music) return -1;
    s_cur_music = music;
    s_mus_pos = 0;
    s_mus_loops = loops;
    s_mus_playing = true;
    return 0;
}

int Mix_HaltMusic(void) {
    s_mus_playing = false;
    s_mus_pos = 0;
    return 0;
}

int Mix_VolumeMusic(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    s_mus_volume = volume;
    return s_mus_volume;
}

int Mix_VolumeChunk(Mix_Chunk *chunk, int volume) {
    if (!chunk) return 0;
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    chunk->volume = volume;
    return chunk->volume;
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    if (!chunk) return -1;
    int slot = -1;
    if (channel >= 0) {
        if (channel >= s_voices_cap) {
            int new_cap = s_voices_cap ? s_voices_cap : 8;
            while (new_cap <= channel) {
                if (new_cap > INT32_MAX / 2) return -1;
                new_cap *= 2;
            }
            if ((size_t)new_cap > SIZE_MAX / sizeof(sfx_voice_t)) return -1;
            sfx_voice_t *nv = (sfx_voice_t *)realloc(s_voices, (size_t)new_cap * sizeof(sfx_voice_t));
            if (!nv) return -1;
            memset(nv + s_voices_cap, 0, (size_t)(new_cap - s_voices_cap) * sizeof(sfx_voice_t));
            s_voices = nv;
            s_voices_cap = new_cap;
        }
        slot = channel;
    } else {
        for (int i = 0; i < s_voices_cap; i++) {
            if (!s_voices[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            int new_cap = s_voices_cap ? (s_voices_cap * 2) : 8;
            if (new_cap <= s_voices_cap || (size_t)new_cap > SIZE_MAX / sizeof(sfx_voice_t)) return -1;
            sfx_voice_t *nv = (sfx_voice_t *)realloc(s_voices, (size_t)new_cap * sizeof(sfx_voice_t));
            if (!nv) return -1;
            memset(nv + s_voices_cap, 0, (size_t)(new_cap - s_voices_cap) * sizeof(sfx_voice_t));
            slot = s_voices_cap;
            s_voices = nv;
            s_voices_cap = new_cap;
        }
    }
    s_voices[slot].chunk = chunk;
    s_voices[slot].pos = 0;
    s_voices[slot].loops = loops;
    s_voices[slot].volume = chunk->volume;
    s_voices[slot].active = true;
    return slot;
}
