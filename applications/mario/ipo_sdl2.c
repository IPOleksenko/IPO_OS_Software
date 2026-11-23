#include "compat_sdl2/SDL.h"
#include "compat_sdl2/SDL_mixer.h"

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
    char *path;
    bool loaded;
    bool failed;
};

struct Mix_Chunk {
    int16_t *samples;
    uint32_t count;
    int volume; /* 0..128 */
    char *path;
    bool loaded;
    bool failed;
};

typedef struct {
    const Mix_Chunk *chunk;
    uint32_t pos;
    int loops;
    int volume;
    bool active;
} sfx_voice_t;

static const SDL_PixelFormat s_fmt_bgr24 = { 24u };
static uint8_t s_wm_palette[248][3];
static uint32_t s_wm_pal32[256];
static bool s_pal_ready = false;

static SDL_Window *s_main_win = NULL;
static bool s_ctrl_down = false;

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
static bool s_mus_paused = false;
static int s_mus_volume = 128;
static sfx_voice_t *s_voices = NULL;
static int s_voices_cap = 0;

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
        s_wm_pal32[i] = 0xFF000000u |
                        ((uint32_t)s_wm_palette[i][0] << 16) |
                        ((uint32_t)s_wm_palette[i][1] << 8)  |
                        (uint32_t)s_wm_palette[i][2];
    }
    for (int i = 248; i < 256; i++) s_wm_pal32[i] = 0xFF000000u;
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

static FILE *open_mario_asset(const char *path) {
    if (!path || !path[0]) return NULL;
    if (path[0] != '/') {
        size_t len = strlen(path);
        if (len <= ((size_t)-1) - 14u) {
            char *alt = (char *)malloc(len + 14u);
            if (!alt) return NULL;
            snprintf(alt, len + 14u, "/data/mario/%s", path);
            FILE *f = fopen(alt, "rb");
            free(alt);
            if (f) return f;
        }
    }
    return fopen(path, "rb");
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
        if (s_mus_playing && !s_mus_paused && s_cur_music && s_cur_music->count > 0) {
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
    if (s_astream) {
        audio_stream_stop(s_astream);
        audio_stream_destroy(s_astream);
        s_astream = NULL;
    }
    if (s_main_win) {
        SDL_DestroyWindow(s_main_win);
        s_main_win = NULL;
    }
    free(s_ev_q); s_ev_q = NULL; s_ev_cap = 0; s_ev_head = 0; s_ev_tail = 0;
    free(s_voices); s_voices = NULL; s_voices_cap = 0;
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x; (void)y; (void)flags;
    init_wm_palette_lut();

    SDL_Window *win = (SDL_Window *)calloc(1, sizeof(SDL_Window));
    if (!win) return NULL;
    win->game_w = (w > 0) ? w : 800;
    win->game_h = (h > 0) ? h : 448;

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = title ? title : "uMario";
    opt.bg_color = 0;
    opt.fullscreen = false;
    opt.w = (uint16_t)sw;
    opt.h = (uint16_t)sh;
    opt.x = 0;
    opt.y = 0;

    win->wm_win = wm_create_window(&opt);
    if (win->wm_win) {
        wm_set_fullscreen(win->wm_win, true);
        wm_set_cursor_visible(win->wm_win, false);
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

    ipo_hd_setup(&win->hd, win->wm_win, win->game_w, win->game_h, true);
    serial_printf("[mario-win] game=%dx%d scr=%dx%d pres=(%d,%d %dx%d) k=%d\n",
                  win->game_w, win->game_h, win->hd.scr_w, win->hd.scr_h,
                  win->hd.pres_x, win->hd.pres_y, win->hd.pres_w, win->hd.pres_h, win->hd.scale_k);
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

void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    (void)window; (void)icon;
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
    return r;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {
    if (!renderer) return;
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

int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->buf) return -1;
    if (!rect) {
        memset(renderer->buf, renderer->draw_idx, (size_t)renderer->w * (size_t)renderer->h);
        return 0;
    }
    int x0 = rect->x, y0 = rect->y, x1 = rect->x + rect->w, y1 = rect->y + rect->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > renderer->w) x1 = renderer->w;
    if (y1 > renderer->h) y1 = renderer->h;
    if (x0 >= x1 || y0 >= y1) return 0;
    size_t span = (size_t)(x1 - x0);
    for (int y = y0; y < y1; y++) {
        memset(renderer->buf + (size_t)y * (size_t)renderer->w + (size_t)x0, renderer->draw_idx, span);
    }
    return 0;
}

int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !rect || rect->w <= 0 || rect->h <= 0) return -1;
    int x0 = rect->x, y0 = rect->y, x1 = rect->x + rect->w - 1, y1 = rect->y + rect->h - 1;
    SDL_RenderDrawLine(renderer, x0, y0, x1, y0);
    SDL_RenderDrawLine(renderer, x0, y1, x1, y1);
    SDL_RenderDrawLine(renderer, x0, y0, x0, y1);
    SDL_RenderDrawLine(renderer, x1, y0, x1, y1);
    return 0;
}

int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2) {
    if (!renderer || !renderer->buf) return -1;
    int dx = abs(x2 - x1), sx = (x1 < x2) ? 1 : -1;
    int dy = -abs(y2 - y1), sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    uint8_t col = renderer->draw_idx;
    for (;;) {
        if ((unsigned)x1 < (unsigned)renderer->w && (unsigned)y1 < (unsigned)renderer->h) {
            renderer->buf[(size_t)y1 * (size_t)renderer->w + (size_t)x1] = col;
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

    init_wm_palette_lut();
    if (!ipo_hd_present_pal32(&win->hd, win->wm_win, win->direct_gfx,
                              renderer->buf, renderer->w, renderer->h, renderer->w, s_wm_pal32)) {
        ipo_hd_present(&win->hd, win->wm_win, win->direct_gfx,
                       renderer->buf, renderer->w, renderer->h, renderer->w);
    }

    mix_audio_pump();

    static uint32_t s_fps_t0 = 0;
    static uint32_t s_fps_frames = 0;
    uint32_t now = timer_millis();
    if (s_fps_t0 == 0u) s_fps_t0 = now;
    s_fps_frames++;
    uint32_t elapsed = now - s_fps_t0;
    if (elapsed >= 1000u) {
        serial_printf("[mario] %u FPS\n", (s_fps_frames * 1000u) / elapsed);
        s_fps_frames = 0;
        s_fps_t0 = now;
    }
}

/* =========================================================================
 * BMP Surface & Texture Blitting
 * ========================================================================= */
SDL_Surface *SDL_LoadBMP(const char *file) {
    FILE *f = open_mario_asset(file);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 54 || sz > 32 * 1024 * 1024) { fclose(f); return NULL; }

    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) { fclose(f); return NULL; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return NULL; }
    fclose(f);

    if (raw[0] != 'B' || raw[1] != 'M') { free(raw); return NULL; }
    uint32_t off = rd_u32(raw + 10);
    int32_t w = (int32_t)rd_u32(raw + 18);
    int32_t h_signed = (int32_t)rd_u32(raw + 22);
    uint16_t bpp = rd_u16(raw + 28);
    uint32_t comp = rd_u32(raw + 30);
    bool top_down = (h_signed < 0);
    int h = top_down ? -h_signed : h_signed;

    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        free(raw);
        return NULL;
    }

    int bytes_pp = bpp / 8;
    size_t src_stride = (((size_t)w * (size_t)bpp + 31u) / 32u) * 4u;
    if ((size_t)off + src_stride * (size_t)h > (size_t)sz) {
        free(raw);
        return NULL;
    }

    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) { free(raw); return NULL; }
    s->w = w;
    s->h = h;
    s->pitch = w * 3;
    s->format = &s_fmt_bgr24;
    s->pixels = malloc((size_t)w * (size_t)h * 3u);
    if (!s->pixels) { free(s); free(raw); return NULL; }

    uint8_t *dst = (uint8_t *)s->pixels;
    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h - 1 - y);
        const uint8_t *srow = raw + off + (size_t)src_y * src_stride;
        uint8_t *drow = dst + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < w; x++) {
            drow[x * 3 + 0] = srow[x * bytes_pp + 0]; /* B */
            drow[x * 3 + 1] = srow[x * bytes_pp + 1]; /* G */
            drow[x * 3 + 2] = srow[x * bytes_pp + 2]; /* R */
        }
    }
    free(raw);
    return s;
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    free(surface->pixels);
    free(surface);
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b) {
    (void)format;
    return ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key) {
    if (!surface) return -1;
    surface->has_colorkey = (flag != 0);
    surface->ck_r = (Uint8)((key >> 16) & 0xFFu);
    surface->ck_g = (Uint8)((key >> 8) & 0xFFu);
    surface->ck_b = (Uint8)(key & 0xFFu);
    return 0;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface) {
    (void)renderer;
    if (!surface || !surface->pixels) return NULL;
    init_wm_palette_lut();
    SDL_Texture *tex = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!tex) return NULL;
    tex->w = surface->w;
    tex->h = surface->h;
    tex->mod_r = 255;
    tex->mod_g = 255;
    tex->mod_b = 255;
    tex->alpha = 255;
    size_t n = (size_t)tex->w * (size_t)tex->h;
    tex->px = (uint8_t *)malloc(n);
    if (!tex->px) { free(tex); return NULL; }

    const uint8_t *src = (const uint8_t *)surface->pixels;
    bool ck = surface->has_colorkey;
    uint8_t ckr = surface->ck_r, ckg = surface->ck_g, ckb = surface->ck_b;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t r = src[i * 3 + 2];
        if (ck && r == ckr && g == ckg && b == ckb) {
            tex->px[i] = (uint8_t)TRANSP_IDX;
        } else {
            tex->px[i] = rgb_to_wm_fast(r, g, b);
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
    if (format) *format = 0;
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

static int blit_texture(SDL_Renderer *renderer, SDL_Texture *texture,
                        const SDL_Rect *srcrect, const SDL_Rect *dstrect,
                        bool flip_x, bool flip_y) {
    if (!renderer || !renderer->buf || !texture || !texture->px) return -1;
    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : texture->w;
    int sh = srcrect ? srcrect->h : texture->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int dw = dstrect ? dstrect->w : sw;
    int dh = dstrect ? dstrect->h : sh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;

    bool has_mod = (texture->mod_r != 255 || texture->mod_g != 255 || texture->mod_b != 255);
    uint8_t mod_lut[248];
    if (has_mod) {
        for (int i = 0; i < 248; i++) {
            uint8_t r = (uint8_t)(((int)s_wm_palette[i][0] * texture->mod_r) / 255);
            uint8_t g = (uint8_t)(((int)s_wm_palette[i][1] * texture->mod_g) / 255);
            uint8_t b = (uint8_t)(((int)s_wm_palette[i][2] * texture->mod_b) / 255);
            mod_lut[i] = rgb_to_wm_fast(r, g, b);
        }
    }

    int x_start = (dx < 0) ? -dx : 0;
    int x_end   = (dx + dw > renderer->w) ? (renderer->w - dx) : dw;
    int y_start = (dy < 0) ? -dy : 0;
    int y_end   = (dy + dh > renderer->h) ? (renderer->h - dy) : dh;
    if (x_start >= x_end || y_start >= y_end) return 0;

    if (sw == dw && sh == dh && !flip_x && !flip_y && !has_mod) {
        for (int ry = y_start; ry < y_end; ry++) {
            int iy = sy + ry;
            if ((unsigned)iy >= (unsigned)texture->h) continue;
            const uint8_t *srow = texture->px + (size_t)iy * (size_t)texture->w + (size_t)sx;
            uint8_t *drow = renderer->buf + (size_t)(dy + ry) * (size_t)renderer->w + (size_t)dx;
            for (int rx = x_start; rx < x_end; rx++) {
                int ix = sx + rx;
                if ((unsigned)ix >= (unsigned)texture->w) continue;
                uint8_t c = srow[rx];
                if (c != TRANSP_IDX) drow[rx] = c;
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
            if (c != TRANSP_IDX) {
                drow[rx] = has_mod ? mod_lut[c] : c;
            }
        }
    }
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
    return blit_texture(renderer, texture, srcrect, dstrect, false, false);
}

int SDL_RenderCopyEx(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect,
                     const SDL_Rect *dstrect, double angle, const SDL_Point *center, SDL_RendererFlip flip) {
    (void)center;
    bool fx = (flip & SDL_FLIP_HORIZONTAL) != 0;
    bool fy = (flip & SDL_FLIP_VERTICAL) != 0;
    /* Match SDL rotation semantics used in uMario IMG.cpp (180 deg + VFLIP = HFLIP, etc.) */
    if (angle > 90.0 && angle < 270.0) {
        fx = !fx;
        fy = !fy;
    }
    return blit_texture(renderer, texture, srcrect, dstrect, fx, fy);
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
            case 0x1C: return SDLK_KP_ENTER;
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
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)frequency; (void)format; (void)channels; (void)chunksize;
    if (s_audio_inited) return s_audio_ready ? 0 : -1;
    s_audio_inited = true;
    if (s_voices && s_voices_cap > 0) memset(s_voices, 0, (size_t)s_voices_cap * sizeof(sfx_voice_t));
    if (!audio_init()) return -1;
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    s_audio_write_samples = 0;
    s_audio_start_ms = timer_millis();
    s_audio_total_written = 0;
    s_astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1u, AUDIO_FREQ,
                                         s_audio_ring, AUDIO_RING_SAMPLES, 0u);
    if (s_astream) {
        audio_stream_play(s_astream);
        s_audio_ready = true;
        return 0;
    }
    return -1;
}

static bool load_wav_pcm(const char *file, int16_t **out_samples, uint32_t *out_count) {
    FILE *f = open_mario_asset(file);
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
    uint32_t rate = AUDIO_FREQ;
    const uint8_t *pcm_ptr = NULL;
    uint32_t pcm_sz = 0;
    size_t pos = 12;
    while (pos + 8 <= (size_t)sz) {
        uint32_t csz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= (size_t)sz) {
            chans = rd_u16(buf + pos + 10);
            rate  = rd_u32(buf + pos + 12);
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
    (void)rate;
    free(buf);
    *out_samples = mono;
    *out_count = frames;
    return true;
}

static char *mario_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static bool ensure_music_loaded(Mix_Music *music) {
    if (!music) return false;
    if (music->loaded) return !music->failed;
    music->loaded = true;
    if (!music->path || !load_wav_pcm(music->path, &music->samples, &music->count)) {
        music->failed = true;
        return false;
    }
    free(music->path);
    music->path = NULL;
    return true;
}

static bool ensure_chunk_loaded(Mix_Chunk *chunk) {
    if (!chunk) return false;
    if (chunk->loaded) return !chunk->failed;
    chunk->loaded = true;
    if (!chunk->path || !load_wav_pcm(chunk->path, &chunk->samples, &chunk->count)) {
        chunk->failed = true;
        return false;
    }
    free(chunk->path);
    chunk->path = NULL;
    return true;
}

Mix_Music *Mix_LoadMUS(const char *file) {
    if (!file || !file[0]) return NULL;
    Mix_Music *m = (Mix_Music *)calloc(1, sizeof(Mix_Music));
    if (!m) return NULL;
    m->path = mario_strdup(file);
    if (!m->path) {
        free(m);
        return NULL;
    }
    return m;
}

Mix_Chunk *Mix_LoadWAV(const char *file) {
    if (!file || !file[0]) return NULL;
    Mix_Chunk *c = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!c) return NULL;
    c->volume = 128;
    c->path = mario_strdup(file);
    if (!c->path) {
        free(c);
        return NULL;
    }
    return c;
}

void Mix_FreeMusic(Mix_Music *music) {
    if (!music) return;
    if (s_cur_music == music) { s_cur_music = NULL; s_mus_playing = false; }
    free(music->path);
    free(music->samples);
    free(music);
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    for (int i = 0; i < s_voices_cap; i++) {
        if (s_voices[i].chunk == chunk) s_voices[i].active = false;
    }
    free(chunk->path);
    free(chunk->samples);
    free(chunk);
}

int Mix_PlayMusic(Mix_Music *music, int loops) {
    if (!ensure_music_loaded(music)) return -1;
    s_cur_music = music;
    s_mus_pos = 0;
    s_mus_loops = loops;
    s_mus_playing = true;
    s_mus_paused = false;
    return 0;
}

int Mix_HaltMusic(void) {
    s_mus_playing = false;
    s_mus_pos = 0;
    return 0;
}

void Mix_PauseMusic(void) {
    s_mus_paused = true;
}

void Mix_ResumeMusic(void) {
    s_mus_paused = false;
}

int Mix_PausedMusic(void) {
    return s_mus_paused ? 1 : 0;
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
    if (!ensure_chunk_loaded(chunk)) return -1;
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
