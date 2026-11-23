#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <wm.h>
#include <vga.h>
#include <vga_gfx.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/sound.h>
#include <system/state.h>
#include <system/timer.h>
#include <syscall.h>
#include "common/ipo_hd_present.h"

extern uint8_t *ns_image_wuffs_decode_to_bgra(const uint8_t *data, size_t len,
                                               int *out_w, int *out_h,
                                               size_t *out_stride, size_t *out_buf_len);

extern int serial_printf(const char *fmt, ...);
extern void tetris_resolve_asset_path(const char *path, char *out, size_t out_sz);

/* Renamed symbols from sense_ttf.c */
extern int sense_TTF_Init(void);
extern void sense_TTF_Quit(void);
extern TTF_Font *sense_TTF_OpenFont(const char *file, int ptsize);
extern void sense_TTF_CloseFont(TTF_Font *font);

/* =========================================================================
 * Palette & 8bpp WM Color LUT
 * ========================================================================= */
static uint8_t s_wm_palette[248][3];
static uint32_t s_wm_pal32[256];
static uint8_t s_rgb555_to_wm[32768];
static uint8_t s_a_to_lvl[256];
static bool    s_pal_ready = false;
static const char *s_err_buf = "";

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
    for (int a = 0; a < 256; a++) {
        s_a_to_lvl[a] = (uint8_t)((a * 7 + 127) / 255);
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

/* =========================================================================
 * Structs: Window, Texture, Renderer
 * ========================================================================= */
struct SDL_Texture {
    int w;
    int h;
    uint8_t *px;
    uint8_t *alpha; /* NULL => opaque (255 everywhere) */
    int min_x, min_y, max_x, max_y; /* Tight non-zero alpha bbox */
    bool is_target;
    bool is_cached;
    SDL_BlendMode blend;
    Uint8 alpha_mod;
};

struct SDL_Window {
    int log_w;
    int log_h;
    int scr_w;
    int scr_h;
    wm_window_t *wm_win;
    bool direct_gfx;
    int pres_x;
    int pres_w;
    int pres_h;
    ipo_hd_view_t hd;
};

struct SDL_Renderer {
    SDL_Window *win;
    SDL_Texture *screen_tex;
    SDL_Texture *cur_target;
    int scale_div;
    uint8_t draw_r, draw_g, draw_b, draw_a;
    uint8_t draw_wm;
    SDL_BlendMode draw_blend;
};

static SDL_Window   *s_active_win = NULL;
static SDL_Renderer *s_active_ren = NULL;
static int           s_mouse_x = 0;
static int           s_mouse_y = 0;
static Uint32        s_mouse_buttons = 0;
static bool          s_ctrl_down = false;

/* =========================================================================
 * Event Queue & PS/2 -> SDL2 Keycode Mapping
 * ========================================================================= */
static SDL_Event *s_evq = NULL;
static int s_evq_cap = 0;
static int s_ev_head = 0;
static int s_ev_tail = 0;

static void push_sdl_event(const SDL_Event *ev) {
    if (!s_evq) {
        s_evq_cap = 256;
        s_evq = (SDL_Event *)calloc((size_t)s_evq_cap, sizeof(SDL_Event));
        if (!s_evq) { s_evq_cap = 0; return; }
        s_ev_head = 0;
        s_ev_tail = 0;
    }
    int next = (s_ev_tail + 1) % s_evq_cap;
    if (next == s_ev_head) {
        int new_cap = s_evq_cap * 2;
        if (new_cap <= s_evq_cap || (size_t)new_cap > SIZE_MAX / sizeof(SDL_Event)) return;
        SDL_Event *nq = (SDL_Event *)malloc((size_t)new_cap * sizeof(SDL_Event));
        if (!nq) return;
        int n = 0;
        for (int idx = s_ev_head; idx != s_ev_tail; idx = (idx + 1) % s_evq_cap) {
            nq[n++] = s_evq[idx];
        }
        free(s_evq);
        s_evq = nq;
        s_ev_head = 0;
        s_ev_tail = n;
        s_evq_cap = new_cap;
        next = s_ev_tail + 1;
    }
    s_evq[s_ev_tail] = *ev;
    s_ev_tail = next;
}

int SDL_PushEvent(SDL_Event *event) {
    if (!event) return -1;
    push_sdl_event(event);
    return 1;
}

static SDL_Keycode ps2_to_sdl_key(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return SDLK_UP;
            case 0x50: return SDLK_DOWN;
            case 0x4B: return SDLK_LEFT;
            case 0x4D: return SDLK_RIGHT;
            case 0x1D: return SDLK_RCTRL;
            case 0x38: return SDLK_RALT;
            case 0x53: return SDLK_DELETE;
            case 0x1C: return SDLK_KP_ENTER;
            case 0x35: return SDLK_KP_DIVIDE;
            default:   return SDLK_UNKNOWN;
        }
    }
    switch (sc) {
        case 0x01: return SDLK_ESCAPE;
        case 0x02: return SDLK_1; case 0x03: return SDLK_2; case 0x04: return SDLK_3;
        case 0x05: return SDLK_4; case 0x06: return SDLK_5; case 0x07: return SDLK_6;
        case 0x08: return SDLK_7; case 0x09: return SDLK_8; case 0x0A: return SDLK_9;
        case 0x0B: return SDLK_0; case 0x0C: return SDLK_MINUS; case 0x0D: return SDLK_EQUALS;
        case 0x0E: return SDLK_BACKSPACE;
        case 0x0F: return SDLK_TAB;
        case 0x10: return SDLK_q; case 0x11: return SDLK_w; case 0x12: return SDLK_e;
        case 0x13: return SDLK_r; case 0x14: return SDLK_t; case 0x15: return SDLK_y;
        case 0x16: return SDLK_u; case 0x17: return SDLK_i; case 0x18: return SDLK_o;
        case 0x19: return SDLK_p;
        case 0x1A: return SDLK_LEFTBRACKET; case 0x1B: return SDLK_RIGHTBRACKET;
        case 0x1C: return SDLK_RETURN;
        case 0x1D: return SDLK_LCTRL;
        case 0x1E: return SDLK_a; case 0x1F: return SDLK_s; case 0x20: return SDLK_d;
        case 0x21: return SDLK_f; case 0x22: return SDLK_g; case 0x23: return SDLK_h;
        case 0x24: return SDLK_j; case 0x25: return SDLK_k; case 0x26: return SDLK_l;
        case 0x27: return SDLK_SEMICOLON; case 0x28: return SDLK_QUOTE;
        case 0x29: return SDLK_BACKQUOTE;
        case 0x2A: return SDLK_LSHIFT;
        case 0x2B: return SDLK_BACKSLASH;
        case 0x2C: return SDLK_z; case 0x2D: return SDLK_x; case 0x2E: return SDLK_c;
        case 0x2F: return SDLK_v; case 0x30: return SDLK_b; case 0x31: return SDLK_n;
        case 0x32: return SDLK_m;
        case 0x33: return SDLK_COMMA; case 0x34: return SDLK_PERIOD; case 0x35: return SDLK_SLASH;
        case 0x36: return SDLK_RSHIFT;
        case 0x37: return SDLK_KP_MULTIPLY;
        case 0x38: return SDLK_LALT;
        case 0x39: return SDLK_SPACE;
        case 0x3A: return SDLK_CAPSLOCK;
        case 0x47: return SDLK_KP_7; case 0x48: return SDLK_KP_8; case 0x49: return SDLK_KP_9;
        case 0x4A: return SDLK_KP_MINUS;
        case 0x4B: return SDLK_KP_4; case 0x4C: return SDLK_KP_5; case 0x4D: return SDLK_KP_6;
        case 0x4E: return SDLK_KP_PLUS;
        case 0x4F: return SDLK_KP_1; case 0x50: return SDLK_KP_2; case 0x51: return SDLK_KP_3;
        case 0x52: return SDLK_KP_0; case 0x53: return SDLK_KP_PERIOD;
        default:   return SDLK_UNKNOWN;
    }
}

static void emit_key(SDL_Keycode sym, bool down) {
    if (sym == SDLK_UNKNOWN) return;
    if (sym == SDLK_LCTRL || sym == SDLK_RCTRL) s_ctrl_down = down;
    if (down && s_ctrl_down && (sym == SDLK_c || sym == SDLK_q)) {
        SDL_Event q;
        memset(&q, 0, sizeof(q));
        q.type = SDL_QUIT;
        push_sdl_event(&q);
        return;
    }
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = timer_millis();
    ev.key.state = down ? 1 : 0;
    ev.key.keysym.sym = sym;
    push_sdl_event(&ev);
}

static void map_mouse_to_logical(int sx, int sy, int *lx, int *ly) {
    SDL_Window *w = s_active_win;
    if (!w || w->pres_w <= 0 || w->pres_h <= 0) {
        *lx = sx;
        *ly = sy;
        return;
    }
    int rx = sx - w->pres_x;
    if (rx < 0) rx = 0;
    if (rx >= w->pres_w) rx = w->pres_w - 1;
    if (sy < 0) sy = 0;
    if (sy >= w->pres_h) sy = w->pres_h - 1;
    *lx = (rx * w->log_w) / w->pres_w;
    *ly = (sy * w->log_h) / w->pres_h;
}

static void pump_os_events(void) {
    SDL_Window *w = s_active_win;
    if (!w) return;

    if (system_is_interrupted()) {
        system_clear_interrupt();
        SDL_Event q;
        memset(&q, 0, sizeof(q));
        q.type = SDL_QUIT;
        push_sdl_event(&q);
        return;
    }

    if (w->wm_win) {
        if (!wm_is_window_valid(w->wm_win) || !wm_session_active()) {
            SDL_Event q;
            memset(&q, 0, sizeof(q));
            q.type = SDL_QUIT;
            push_sdl_event(&q);
            return;
        }
        wm_event_t we;
        while (wm_poll_event(w->wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                SDL_Event q;
                memset(&q, 0, sizeof(q));
                q.type = SDL_QUIT;
                push_sdl_event(&q);
                return;
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                emit_key(ps2_to_sdl_key((uint8_t)(kc & 0x7Fu), e0), !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                static bool s_e0 = false;
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { s_e0 = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                emit_key(ps2_to_sdl_key(raw & 0x7Fu, s_e0), !rel);
                s_e0 = false;
            }
        }
    } else if (w->direct_gfx) {
        static bool s_e0 = false;
        uint8_t raw;
        keyboard_poll();
        while ((raw = keyboard_get_scancode()) != 0u) {
            if (raw == 0xE0u) { s_e0 = true; continue; }
            bool rel = (raw & 0x80u) != 0u;
            emit_key(ps2_to_sdl_key(raw & 0x7Fu, s_e0), !rel);
            s_e0 = false;
        }
    }

    mouse_state_t ms;
    memset(&ms, 0, sizeof(ms));
    mouse_get_state(&ms);
    int wx = (w->wm_win) ? w->wm_win->x : 0;
    int wy = (w->wm_win) ? w->wm_win->y : 0;
    int sx = (int)ms.x - wx;
    int sy = (int)ms.y - wy;
    int lx, ly;
    map_mouse_to_logical(sx, sy, &lx, &ly);
    if (lx != s_mouse_x || ly != s_mouse_y) {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_MOUSEMOTION;
        ev.motion.timestamp = timer_millis();
        ev.motion.xrel = lx - s_mouse_x;
        ev.motion.yrel = ly - s_mouse_y;
        s_mouse_x = lx;
        s_mouse_y = ly;
        ev.motion.x = lx;
        ev.motion.y = ly;
        ev.motion.state = s_mouse_buttons;
        push_sdl_event(&ev);
    }
    bool prev_l = (s_mouse_buttons & SDL_BUTTON_LMASK) != 0u;
    if (ms.left_button != prev_l) {
        if (ms.left_button) s_mouse_buttons |= SDL_BUTTON_LMASK;
        else                s_mouse_buttons &= ~SDL_BUTTON_LMASK;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ms.left_button ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.timestamp = timer_millis();
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = ms.left_button ? 1 : 0;
        ev.button.clicks = 1;
        ev.button.x = lx;
        ev.button.y = ly;
        push_sdl_event(&ev);
    }
}

int SDL_PollEvent(SDL_Event *event) {
    pump_os_events();
    if (!s_evq || s_ev_head == s_ev_tail) return 0;
    if (event) *event = s_evq[s_ev_head];
    s_ev_head = (s_ev_head + 1) % s_evq_cap;
    return 1;
}

Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = s_mouse_x;
    if (y) *y = s_mouse_y;
    return s_mouse_buttons;
}

/* =========================================================================
 * Audio (SDL_mixer -> SB16 22050 Hz S16 Ring Buffer)
 * ========================================================================= */
struct Mix_Chunk {
    int16_t *pcm;
    uint32_t samples;
};

struct Mix_Music {
    int16_t *pcm;
    uint32_t samples;
};

#define AUDIO_RING_SAMPLES 8192

typedef struct {
    Mix_Chunk *chunk;
    uint32_t pos;
    int loops;
    bool active;
    bool paused;
    int volume; /* 0..128 */
} SfxChan;

static bool           s_audio_open = false;
static audio_stream_t *s_astream = NULL;
static int16_t        s_audio_ring[AUDIO_RING_SAMPLES];
static uint32_t       s_write_cursor = 0;
static uint32_t       s_last_mix_ms = 0;
static SfxChan       *s_chans = NULL;
static int            s_chans_cap = 0;
static Mix_Music     *s_cur_music = NULL;
static uint32_t       s_music_pos = 0;
static int            s_music_loops = 0;
static bool           s_music_playing = false;
static bool           s_music_paused = false;
static int            s_music_vol = 64;

static int16_t *decode_wav_to_22050_mono(const uint8_t *buf, size_t len, uint32_t *out_samples) {
    *out_samples = 0;
    if (!buf || len < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return NULL;
    }
    uint16_t channels = 1, bits = 16;
    uint32_t rate = 22050;
    const uint8_t *pcm_ptr = NULL;
    uint32_t pcm_bytes = 0;

    size_t pos = 12;
    while (pos + 8 <= len) {
        uint32_t csz = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                       ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
        const uint8_t *cdata = buf + pos + 8;
        if (pos + 8 + csz > len) csz = (uint32_t)(len - (pos + 8));
        if (memcmp(buf + pos, "fmt ", 4) == 0 && csz >= 16) {
            channels = (uint16_t)(cdata[2] | (cdata[3] << 8));
            rate = (uint32_t)cdata[4] | ((uint32_t)cdata[5] << 8) |
                   ((uint32_t)cdata[6] << 16) | ((uint32_t)cdata[7] << 24);
            bits = (uint16_t)(cdata[14] | (cdata[15] << 8));
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            pcm_ptr = cdata;
            pcm_bytes = csz;
        }
        pos += 8 + ((csz + 1u) & ~1u);
    }
    if (!pcm_ptr || pcm_bytes == 0 || rate == 0 || channels == 0) return NULL;
    uint32_t bps = (uint32_t)(bits / 8) * channels;
    if (bps == 0) return NULL;
    uint32_t src_frames = pcm_bytes / bps;
    uint32_t dst_frames = (uint32_t)(((uint64_t)src_frames * 22050ULL) / rate);
    if (dst_frames == 0) return NULL;
    int16_t *out = (int16_t *)malloc((size_t)dst_frames * sizeof(int16_t));
    if (!out) return NULL;

    for (uint32_t i = 0; i < dst_frames; i++) {
        uint32_t si = (uint32_t)(((uint64_t)i * rate) / 22050ULL);
        if (si >= src_frames) si = src_frames - 1;
        const uint8_t *f = pcm_ptr + (size_t)si * bps;
        int32_t s = 0;
        if (bits == 16) {
            for (int c = 0; c < channels; c++) {
                int16_t v = (int16_t)((uint16_t)f[c * 2] | ((uint16_t)f[c * 2 + 1] << 8));
                s += v;
            }
            s /= channels;
        } else if (bits == 8) {
            for (int c = 0; c < channels; c++) {
                s += ((int32_t)f[c] - 128) * 256;
            }
            s /= channels;
        }
        out[i] = (int16_t)s;
    }
    *out_samples = dst_frames;
    return out;
}

static void pump_audio_mixer(void) {
    if (!s_audio_open || !s_astream) return;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    uint32_t n = 0u;
    if (ipo_audio_get_stream_clock(s_astream, &clk) == IPO_AUDIO_OK) {
        uint64_t target = clk.played_frames + 1470u; /* ~66 ms lead */
        if (target <= s_write_cursor) { audio_tick(); return; }
        uint64_t ahead = (s_write_cursor > clk.played_frames) ? (s_write_cursor - clk.played_frames) : 0ULL;
        uint64_t max_safe = (AUDIO_RING_SAMPLES > ahead + 128u) ? (AUDIO_RING_SAMPLES - ahead - 128u) : 0ULL;
        uint64_t todo = target - s_write_cursor;
        if (todo > max_safe) todo = max_safe;
        if (todo > 2048u) todo = 2048u;
        n = (uint32_t)todo;
        if (n == 0u) { audio_tick(); return; }
    } else {
        uint32_t now = timer_millis();
        uint32_t dt = now - s_last_mix_ms;
        if (dt == 0) { audio_tick(); return; }
        if (dt > 50u) dt = 50u;
        s_last_mix_ms = now;
        n = (dt * 22050u) / 1000u;
        if (n == 0) n = 1;
        if (n > 2048u) n = 2048u;
    }

    for (uint32_t i = 0; i < n; i++) {
        int32_t mix = 0;
        if (s_music_playing && !s_music_paused && s_cur_music && s_cur_music->pcm && s_cur_music->samples > 0) {
            mix += ((int32_t)s_cur_music->pcm[s_music_pos] * s_music_vol) >> 7;
            s_music_pos++;
            if (s_music_pos >= s_cur_music->samples) {
                s_music_pos = 0;
                if (s_music_loops == 0) {
                    s_music_playing = false;
                } else if (s_music_loops > 0) {
                    s_music_loops--;
                }
            }
        }
        for (int c = 0; c < s_chans_cap; c++) {
            SfxChan *ch = &s_chans[c];
            if (!ch->active || ch->paused || !ch->chunk || !ch->chunk->pcm) continue;
            mix += ((int32_t)ch->chunk->pcm[ch->pos] * ch->volume) >> 7;
            ch->pos++;
            if (ch->pos >= ch->chunk->samples) {
                if (ch->loops == 0) {
                    ch->active = false;
                } else {
                    ch->pos = 0;
                    if (ch->loops > 0) ch->loops--;
                }
            }
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        s_audio_ring[s_write_cursor % AUDIO_RING_SAMPLES] = (int16_t)mix;
        s_write_cursor++;
    }
    audio_stream_update_buffered(s_astream, s_write_cursor, false);
    audio_tick();
}

int Mix_Init(int flags) { return flags ? flags : MIX_INIT_MP3; }
void Mix_Quit(void) {}
const char *Mix_GetError(void) { return s_err_buf; }

static bool tetris_ensure_chans(int need) {
    if (need <= s_chans_cap) return true;
    int new_cap = s_chans_cap ? s_chans_cap : 16;
    while (new_cap < need) {
        if (new_cap > INT32_MAX / 2) return false;
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(SfxChan)) return false;
    SfxChan *nc = (SfxChan *)realloc(s_chans, (size_t)new_cap * sizeof(SfxChan));
    if (!nc) return false;
    memset(nc + s_chans_cap, 0, (size_t)(new_cap - s_chans_cap) * sizeof(SfxChan));
    for (int i = s_chans_cap; i < new_cap; i++) nc[i].volume = 96;
    s_chans = nc;
    s_chans_cap = new_cap;
    return true;
}

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)frequency; (void)format; (void)channels; (void)chunksize;
    if (s_audio_open) return 0;
    if (!audio_init()) return -1;
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    if (!tetris_ensure_chans(16)) return -1;
    memset(s_chans, 0, (size_t)s_chans_cap * sizeof(SfxChan));
    for (int i = 0; i < s_chans_cap; i++) s_chans[i].volume = 96;
    s_astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1, 22050u, s_audio_ring, AUDIO_RING_SAMPLES, 0u);
    if (!s_astream) return -1;
    audio_stream_play(s_astream);
    s_last_mix_ms = timer_millis();
    s_audio_open = true;
    return 0;
}

void Mix_CloseAudio(void) {
    if (s_astream) {
        audio_stream_stop(s_astream);
        audio_stream_destroy(s_astream);
        s_astream = NULL;
    }
    free(s_chans);
    s_chans = NULL;
    s_chans_cap = 0;
    s_audio_open = false;
}

int Mix_AllocateChannels(int numchans) {
    if (numchans > s_chans_cap) tetris_ensure_chans(numchans);
    return s_chans_cap;
}

static uint8_t *read_entire_file(const char *path, size_t *out_sz) {
    *out_sz = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    *out_sz = nr;
    return buf;
}

static uint8_t *read_audio_wav_data(const char *file, size_t *out_sz) {
    *out_sz = 0;
    if (!file) return NULL;
    uint8_t *raw = read_entire_file(file, out_sz);
    if (!raw) {
        const char *slash = strrchr(file, '/');
        const char *base = slash ? (slash + 1) : file;
        int num = atoi(base);
        if (num >= 1) {
            int mapped_num = ((num - 1) % 5) + 1;
            const char *prefix = "/data/tetris/src/Sounds/Music/0";
            size_t plen = strlen(prefix);
            char *alt = (char *)malloc(plen + 6u);
            if (alt) {
                snprintf(alt, plen + 6u, "%s%d.wav", prefix, mapped_num);
                raw = read_entire_file(alt, out_sz);
                free(alt);
            }
        }
    }
    return raw;
}

Mix_Chunk *Mix_LoadWAV(const char *file) {
    if (!file) return NULL;
    size_t sz = 0;
    uint8_t *raw = read_audio_wav_data(file, &sz);
    if (!raw) return NULL;
    uint32_t samples = 0;
    int16_t *pcm = decode_wav_to_22050_mono(raw, sz, &samples);
    free(raw);
    if (!pcm) return NULL;
    Mix_Chunk *c = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!c) { free(pcm); return NULL; }
    c->pcm = pcm;
    c->samples = samples;
    return c;
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    for (int i = 0; i < s_chans_cap; i++) {
        if (s_chans[i].chunk == chunk) s_chans[i].active = false;
    }
    free(chunk->pcm);
    free(chunk);
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    if (!chunk) return -1;
    if (channel >= 0) {
        if (!tetris_ensure_chans(channel + 1)) return -1;
    } else {
        channel = -1;
        for (int i = 0; i < s_chans_cap; i++) {
            if (!s_chans[i].active) { channel = i; break; }
        }
        if (channel < 0) {
            channel = s_chans_cap;
            if (!tetris_ensure_chans(channel + 1)) return -1;
        }
    }
    s_chans[channel].chunk = chunk;
    s_chans[channel].pos = 0;
    s_chans[channel].loops = loops;
    s_chans[channel].active = true;
    s_chans[channel].paused = false;
    return channel;
}

void Mix_Pause(int channel) {
    if (channel >= 0 && channel < s_chans_cap) s_chans[channel].paused = true;
}
void Mix_Resume(int channel) {
    if (channel >= 0 && channel < s_chans_cap) s_chans[channel].paused = false;
}
int Mix_HaltChannel(int channel) {
    if (channel >= 0 && channel < s_chans_cap) s_chans[channel].active = false;
    return 0;
}
int Mix_Playing(int channel) {
    return (channel >= 0 && channel < s_chans_cap && s_chans[channel].active) ? 1 : 0;
}
int Mix_Paused(int channel) {
    return (channel >= 0 && channel < s_chans_cap && s_chans[channel].paused) ? 1 : 0;
}
int Mix_Volume(int channel, int volume) {
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    if (channel >= 0 && channel < s_chans_cap) {
        int prev = s_chans[channel].volume;
        s_chans[channel].volume = volume;
        return prev;
    }
    return 128;
}

Mix_Music *Mix_LoadMUS(const char *file) {
    if (!file) return NULL;
    size_t sz = 0;
    uint8_t *raw = read_audio_wav_data(file, &sz);
    if (!raw) return NULL;
    uint32_t samples = 0;
    int16_t *pcm = decode_wav_to_22050_mono(raw, sz, &samples);
    free(raw);
    if (!pcm) return NULL;
    Mix_Music *m = (Mix_Music *)calloc(1, sizeof(Mix_Music));
    if (!m) { free(pcm); return NULL; }
    m->pcm = pcm;
    m->samples = samples;
    return m;
}

void Mix_FreeMusic(Mix_Music *music) {
    if (!music) return;
    if (s_cur_music == music) {
        s_cur_music = NULL;
        s_music_playing = false;
    }
    free(music->pcm);
    free(music);
}

int Mix_PlayMusic(Mix_Music *music, int loops) {
    if (!music) return -1;
    s_cur_music = music;
    s_music_pos = 0;
    s_music_loops = loops;
    s_music_playing = true;
    s_music_paused = false;
    return 0;
}

void Mix_PauseMusic(void) { s_music_paused = true; }
void Mix_ResumeMusic(void) { s_music_paused = false; }
int Mix_HaltMusic(void) { s_music_playing = false; s_music_pos = 0; return 0; }
int Mix_PlayingMusic(void) { return s_music_playing ? 1 : 0; }
int Mix_PausedMusic(void) { return s_music_paused ? 1 : 0; }
int Mix_VolumeMusic(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    int prev = s_music_vol;
    s_music_vol = volume;
    return prev;
}

/* =========================================================================
 * PNG Image Cache (IMG_LoadTexture) & TTF Font Cache
 * ========================================================================= */
static char *tetris_sdl_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

typedef struct {
    bool used;
    char *path;
    int w, h;
    int min_x, min_y, max_x, max_y;
    uint8_t *px;
    uint8_t *alpha;
} PngCacheEntry;

typedef struct {
    bool used;
    char *path;
    int ptsize;
    TTF_Font *font;
} FontCacheEntry;

typedef struct {
    bool used;
    TTF_Font *font;
    SDL_Color fg;
    char *text;
    int w, h, pitch;
    int min_x, min_y, max_x, max_y;
    SDL_bool has_alpha_mod;
    Uint8 alpha_mod;
    void *rgba_pixels;
    uint8_t *px;
    uint8_t *alpha;
    uint32_t last_use;
} TextCacheEntry;

static PngCacheEntry  *s_png_cache = NULL;
static int             s_png_cache_cap = 0;
static FontCacheEntry *s_font_cache = NULL;
static int             s_font_cache_cap = 0;
static TextCacheEntry *s_text_cache = NULL;
static int             s_text_cache_cap = 0;
static uint32_t        s_text_cache_clock = 0;

int IMG_Init(int flags) { init_wm_palette_lut(); return flags ? flags : IMG_INIT_PNG; }
void IMG_Quit(void) {
    for (int i = 0; i < s_png_cache_cap; i++) {
        if (s_png_cache[i].used) {
            free(s_png_cache[i].path);
            free(s_png_cache[i].px);
            free(s_png_cache[i].alpha);
        }
    }
    free(s_png_cache);
    s_png_cache = NULL;
    s_png_cache_cap = 0;
}
const char *IMG_GetError(void) { return s_err_buf; }

SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file) {
    (void)renderer;
    if (!file) return NULL;
    init_wm_palette_lut();

    for (int i = 0; i < s_png_cache_cap; i++) {
        if (s_png_cache[i].used && s_png_cache[i].path && strcmp(s_png_cache[i].path, file) == 0) {
            SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
            if (!t) return NULL;
            t->w = s_png_cache[i].w;
            t->h = s_png_cache[i].h;
            t->min_x = s_png_cache[i].min_x;
            t->min_y = s_png_cache[i].min_y;
            t->max_x = s_png_cache[i].max_x;
            t->max_y = s_png_cache[i].max_y;
            t->px = s_png_cache[i].px;
            t->alpha = s_png_cache[i].alpha;
            t->is_cached = true;
            t->blend = t->alpha ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
            t->alpha_mod = 255;
            return t;
        }
    }

    size_t sz = 0;
    uint8_t *raw = read_entire_file(file, &sz);
    if (!raw) return NULL;

    int w = 0, h = 0;
    size_t stride = 0, out_len = 0;
    uint8_t *bgra = ns_image_wuffs_decode_to_bgra(raw, sz, &w, &h, &stride, &out_len);
    free(raw);
    if (!bgra || w <= 0 || h <= 0) { free(bgra); return NULL; }

    size_t n = (size_t)w * (size_t)h;
    uint8_t *px = (uint8_t *)malloc(n);
    uint8_t *al = (uint8_t *)malloc(n);
    if (!px || !al) { free(px); free(al); free(bgra); return NULL; }

    bool has_alpha = false;
    int min_x = w, min_y = h, max_x = 0, max_y = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = bgra + (size_t)y * stride;
        uint8_t *dpx = px + (size_t)y * (size_t)w;
        uint8_t *dal = al + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint8_t b = srow[x * 4 + 0];
            uint8_t g = srow[x * 4 + 1];
            uint8_t r = srow[x * 4 + 2];
            uint8_t a = srow[x * 4 + 3];
            dpx[x] = rgb_to_wm(r, g, b);
            dal[x] = a;
            if (a != 255u) has_alpha = true;
            if (a != 0u) {
                if (x < min_x) min_x = x;
                if (x + 1 > max_x) max_x = x + 1;
                if (y < min_y) min_y = y;
                if (y + 1 > max_y) max_y = y + 1;
            }
        }
    }
    free(bgra);
    if (!has_alpha) { free(al); al = NULL; min_x = 0; min_y = 0; max_x = w; max_y = h; }

    bool cached = false;
    int slot = -1;
    for (int i = 0; i < s_png_cache_cap; i++) {
        if (!s_png_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_png_cache_cap ? (s_png_cache_cap * 2) : 32;
        if (new_cap > s_png_cache_cap && (size_t)new_cap <= SIZE_MAX / sizeof(PngCacheEntry)) {
            PngCacheEntry *nc = (PngCacheEntry *)realloc(s_png_cache, (size_t)new_cap * sizeof(PngCacheEntry));
            if (nc) {
                memset(nc + s_png_cache_cap, 0, (size_t)(new_cap - s_png_cache_cap) * sizeof(PngCacheEntry));
                slot = s_png_cache_cap;
                s_png_cache = nc;
                s_png_cache_cap = new_cap;
            }
        }
    }
    if (slot >= 0) {
        char *pdup = tetris_sdl_strdup(file);
        if (pdup) {
            s_png_cache[slot].used = true;
            s_png_cache[slot].path = pdup;
            s_png_cache[slot].w = w;
            s_png_cache[slot].h = h;
            s_png_cache[slot].min_x = min_x;
            s_png_cache[slot].min_y = min_y;
            s_png_cache[slot].max_x = max_x;
            s_png_cache[slot].max_y = max_y;
            s_png_cache[slot].px = px;
            s_png_cache[slot].alpha = al;
            cached = true;
        }
    }

    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!t) {
        if (!cached) { free(px); free(al); }
        return NULL;
    }
    t->w = w;
    t->h = h;
    t->min_x = min_x;
    t->min_y = min_y;
    t->max_x = max_x;
    t->max_y = max_y;
    t->px = px;
    t->alpha = al;
    t->is_cached = cached;
    t->blend = al ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
    t->alpha_mod = 255;
    return t;
}

int TTF_Init(void) { return sense_TTF_Init(); }
void TTF_Quit(void) {
    for (int i = 0; i < s_text_cache_cap; i++) {
        if (s_text_cache[i].used) {
            free(s_text_cache[i].text);
            free(s_text_cache[i].rgba_pixels);
            free(s_text_cache[i].px);
            free(s_text_cache[i].alpha);
        }
    }
    free(s_text_cache);
    s_text_cache = NULL;
    s_text_cache_cap = 0;
    for (int i = 0; i < s_font_cache_cap; i++) {
        if (s_font_cache[i].used) {
            sense_TTF_CloseFont(s_font_cache[i].font);
            free(s_font_cache[i].path);
        }
    }
    free(s_font_cache);
    s_font_cache = NULL;
    s_font_cache_cap = 0;
    sense_TTF_Quit();
}
const char *TTF_GetError(void) { return s_err_buf; }

TTF_Font *TTF_OpenFont(const char *file, int ptsize) {
    if (!file) return NULL;
    if (ptsize < 6) ptsize = 6;
    for (int i = 0; i < s_font_cache_cap; i++) {
        if (s_font_cache[i].used && s_font_cache[i].ptsize == ptsize &&
            s_font_cache[i].path && strcmp(s_font_cache[i].path, file) == 0) {
            return s_font_cache[i].font;
        }
    }
    TTF_Font *f = sense_TTF_OpenFont(file, ptsize);
    if (!f) return NULL;
    int slot = -1;
    for (int i = 0; i < s_font_cache_cap; i++) {
        if (!s_font_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_font_cache_cap ? (s_font_cache_cap * 2) : 16;
        if (new_cap > s_font_cache_cap && (size_t)new_cap <= SIZE_MAX / sizeof(FontCacheEntry)) {
            FontCacheEntry *nc = (FontCacheEntry *)realloc(s_font_cache, (size_t)new_cap * sizeof(FontCacheEntry));
            if (nc) {
                memset(nc + s_font_cache_cap, 0, (size_t)(new_cap - s_font_cache_cap) * sizeof(FontCacheEntry));
                slot = s_font_cache_cap;
                s_font_cache = nc;
                s_font_cache_cap = new_cap;
            }
        }
    }
    if (slot >= 0) {
        char *pdup = tetris_sdl_strdup(file);
        if (pdup) {
            s_font_cache[slot].used = true;
            s_font_cache[slot].path = pdup;
            s_font_cache[slot].ptsize = ptsize;
            s_font_cache[slot].font = f;
        }
    }
    return f;
}

void TTF_CloseFont(TTF_Font *font) {
    for (int i = 0; i < s_font_cache_cap; i++) {
        if (s_font_cache[i].used && s_font_cache[i].font == font) return;
    }
    sense_TTF_CloseFont(font);
}

SDL_Surface *TTF_RenderText_Solid(TTF_Font *font, const char *text, SDL_Color fg) {
    if (!font || !text) return NULL;
    init_wm_palette_lut();
    s_text_cache_clock++;

    for (int i = 0; i < s_text_cache_cap; i++) {
        TextCacheEntry *e = &s_text_cache[i];
        if (e->used && e->font == font && e->text &&
            e->fg.r == fg.r && e->fg.g == fg.g && e->fg.b == fg.b && e->fg.a == fg.a &&
            strcmp(e->text, text) == 0) {
            e->last_use = s_text_cache_clock;
            SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
            if (!s) return NULL;
            s->w = e->w;
            s->h = e->h;
            s->pitch = e->pitch;
            s->pixels = e->rgba_pixels;
            s->borrowed_pixels = SDL_TRUE;
            s->has_alpha_mod = e->has_alpha_mod;
            s->alpha_mod = e->alpha_mod;
            s->cache_key = (uint64_t)(i + 1);
            return s;
        }
    }

    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, fg);
    if (!s || !s->pixels || s->w <= 0 || s->h <= 0) {
        return s;
    }

    int slot = -1;
    for (int i = 0; i < s_text_cache_cap; i++) {
        if (!s_text_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_text_cache_cap ? (s_text_cache_cap * 2) : 64;
        if (new_cap > s_text_cache_cap && (size_t)new_cap <= SIZE_MAX / sizeof(TextCacheEntry)) {
            TextCacheEntry *nc = (TextCacheEntry *)realloc(s_text_cache, (size_t)new_cap * sizeof(TextCacheEntry));
            if (nc) {
                memset(nc + s_text_cache_cap, 0, (size_t)(new_cap - s_text_cache_cap) * sizeof(TextCacheEntry));
                slot = s_text_cache_cap;
                s_text_cache = nc;
                s_text_cache_cap = new_cap;
            }
        }
    }
    if (slot >= 0) {
        char *tdup = tetris_sdl_strdup(text);
        int w = s->w, h = s->h;
        size_t n = (size_t)w * (size_t)h;
        uint8_t *px = (uint8_t *)malloc(n);
        uint8_t *al = (uint8_t *)malloc(n);
        if (tdup && px && al) {
            TextCacheEntry *e = &s_text_cache[slot];
            const uint32_t *src = (const uint32_t *)s->pixels;
            int pitch_px = s->pitch > 0 ? (s->pitch / 4) : w;
            int min_x = w, min_y = h, max_x = 0, max_y = 0;
            for (int y = 0; y < h; y++) {
                const uint32_t *srow = src + (size_t)y * (size_t)pitch_px;
                uint8_t *dpx = px + (size_t)y * (size_t)w;
                uint8_t *dal = al + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) {
                    uint32_t c = srow[x];
                    uint8_t a = (uint8_t)(c >> 24);
                    uint8_t r = (uint8_t)((c >> 16) & 0xFFu);
                    uint8_t g = (uint8_t)((c >> 8) & 0xFFu);
                    uint8_t b = (uint8_t)(c & 0xFFu);
                    dpx[x] = rgb_to_wm(r, g, b);
                    dal[x] = a;
                    if (a != 0u) {
                        if (x < min_x) min_x = x;
                        if (x + 1 > max_x) max_x = x + 1;
                        if (y < min_y) min_y = y;
                        if (y + 1 > max_y) max_y = y + 1;
                    }
                }
            }
            e->used = true;
            e->font = font;
            e->fg = fg;
            e->text = tdup;
            e->w = w;
            e->h = h;
            e->pitch = s->pitch;
            e->min_x = min_x; e->min_y = min_y;
            e->max_x = max_x; e->max_y = max_y;
            e->has_alpha_mod = s->has_alpha_mod;
            e->alpha_mod = s->alpha_mod;
            e->rgba_pixels = s->pixels;
            e->px = px;
            e->alpha = al;
            e->last_use = s_text_cache_clock;
            s->borrowed_pixels = SDL_TRUE;
            s->cache_key = (uint64_t)(slot + 1);
        } else {
            free(tdup);
            free(px);
            free(al);
        }
    }
    return s;
}

/* =========================================================================
 * Software Renderer & Window Present (960x1000 -> WM Fullscreen)
 * ========================================================================= */
int SDL_Init(Uint32 flags) {
    (void)flags;
    init_wm_palette_lut();
    return 0;
}

static void tetris_render_caches_free(void);

void SDL_Quit(void) {
    IMG_Quit();
    TTF_Quit();
    Mix_CloseAudio();
    tetris_render_caches_free();
    free(s_evq); s_evq = NULL; s_evq_cap = 0; s_ev_head = 0; s_ev_tail = 0;
}

SDL_bool SDL_SetHint(const char *name, const char *value) {
    (void)name; (void)value;
    return SDL_TRUE;
}

const char *SDL_GetError(void) { return s_err_buf; }

Uint32 SDL_GetTicks(void) { return timer_millis(); }

void SDL_Delay(Uint32 ms) {
    uint32_t end = timer_millis() + ms;
    while (timer_millis() < end) {
        pump_audio_mixer();
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x; (void)y; (void)flags;
    init_wm_palette_lut();
    if (s_active_win) return s_active_win;

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    SDL_Window *win = (SDL_Window *)calloc(1, sizeof(SDL_Window));
    if (!win) return NULL;
    win->log_w = (w > 0) ? w : 960;
    win->log_h = (h > 0) ? h : 1000;
    win->scr_w = sw;
    win->scr_h = sh;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = title ? title : "Tetris";
    opt.bg_color = 0;
    opt.fullscreen = true;
    opt.w = (uint16_t)sw;
    opt.h = (uint16_t)sh;
    win->wm_win = wm_create_window(&opt);
    if (win->wm_win) {
        wm_set_fullscreen(win->wm_win, true);
        wm_set_cursor_visible(win->wm_win, true);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        win->direct_gfx = true;
    }

    ipo_hd_setup(&win->hd, win->wm_win, win->log_w, win->log_h, false);
    win->scr_w = win->hd.scr_w;
    win->scr_h = win->hd.scr_h;
    win->pres_w = win->hd.pres_w;
    win->pres_h = win->hd.pres_h;
    win->pres_x = win->hd.pres_x;

    s_active_win = win;
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
    }
    ipo_hd_free(&window->hd);
    if (s_active_win == window) s_active_win = NULL;
    free(window);
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h) {
    (void)renderer; (void)format;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    t->px = (uint8_t *)calloc((size_t)w * (size_t)h, 1u);
    t->is_target = (access == SDL_TEXTUREACCESS_TARGET);
    if (t->is_target) {
        t->alpha = (uint8_t *)calloc((size_t)w * (size_t)h, 1u);
        t->blend = SDL_BLENDMODE_BLEND;
    }
    if (!t->px || (t->is_target && !t->alpha)) {
        free(t->px);
        free(t->alpha);
        free(t);
        return NULL;
    }
    t->alpha_mod = 255;
    return t;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface) {
    (void)renderer;
    if (!surface || !surface->pixels || surface->w <= 0 || surface->h <= 0) return NULL;
    if (surface->cache_key >= 1 && surface->cache_key <= (uint64_t)s_text_cache_cap) {
        TextCacheEntry *e = &s_text_cache[surface->cache_key - 1];
        if (e->used && e->rgba_pixels == surface->pixels) {
            SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
            if (!t) return NULL;
            t->w = e->w;
            t->h = e->h;
            t->min_x = e->min_x; t->min_y = e->min_y;
            t->max_x = e->max_x; t->max_y = e->max_y;
            t->px = e->px;
            t->alpha = e->alpha;
            t->is_cached = true;
            t->blend = SDL_BLENDMODE_BLEND;
            t->alpha_mod = surface->has_alpha_mod ? surface->alpha_mod : 255;
            return t;
        }
    }
    init_wm_palette_lut();
    int w = surface->w;
    int h = surface->h;
    size_t n = (size_t)w * (size_t)h;
    uint8_t *px = (uint8_t *)malloc(n);
    uint8_t *al = (uint8_t *)malloc(n);
    if (!px || !al) { free(px); free(al); return NULL; }

    const uint32_t *src = (const uint32_t *)surface->pixels;
    int pitch_px = surface->pitch / 4;
    if (pitch_px <= 0) pitch_px = w;

    for (int y = 0; y < h; y++) {
        const uint32_t *srow = src + (size_t)y * (size_t)pitch_px;
        uint8_t *dpx = px + (size_t)y * (size_t)w;
        uint8_t *dal = al + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t c = srow[x];
            uint8_t a = (uint8_t)(c >> 24);
            uint8_t r = (uint8_t)((c >> 16) & 0xFFu);
            uint8_t g = (uint8_t)((c >> 8) & 0xFFu);
            uint8_t b = (uint8_t)(c & 0xFFu);
            dpx[x] = rgb_to_wm(r, g, b);
            dal[x] = a;
        }
    }

    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    if (!t) { free(px); free(al); return NULL; }
    t->w = w;
    t->h = h;
    t->px = px;
    t->alpha = al;
    t->blend = SDL_BLENDMODE_BLEND;
    t->alpha_mod = surface->has_alpha_mod ? surface->alpha_mod : 255;
    return t;
}

void SDL_DestroyTexture(SDL_Texture *texture) {
    if (!texture) return;
    if (!texture->is_cached) {
        free(texture->px);
        free(texture->alpha);
    }
    free(texture);
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    if (!surface->borrowed_pixels) free(surface->pixels);
    free(surface);
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags) {
    (void)index; (void)flags;
    if (!window) return NULL;
    if (s_active_ren) return s_active_ren;
    SDL_Renderer *r = (SDL_Renderer *)calloc(1, sizeof(SDL_Renderer));
    if (!r) return NULL;
    r->win = window;
    int sdiv = (window->pres_h > 0 && window->pres_h <= 750) ? 2 : 1;
    r->scale_div = sdiv;
    int tw = window->log_w / sdiv;
    int th = window->log_h / sdiv;
    r->screen_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, tw, th);
    if (!r->screen_tex) { free(r); return NULL; }
    if (r->screen_tex->alpha) {
        memset(r->screen_tex->alpha, 255, (size_t)tw * (size_t)th);
    }
    r->cur_target = r->screen_tex;
    r->draw_a = 255;
    s_active_ren = r;
    return r;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {
    if (!renderer) return;
    SDL_DestroyTexture(renderer->screen_tex);
    if (s_active_ren == renderer) s_active_ren = NULL;
    free(renderer);
}

typedef struct {
    bool active;
    bool flushing;
    uint8_t clear_wm;
    int count;
    int cap;
    SDL_Texture *texs;
} PendingBgSeq;

typedef struct {
    bool used;
    uint8_t clear_wm;
    int w, h;
    int count;
    const uint8_t **orig_px;
    uint8_t *px;
    uint32_t last_use;
} BgSnapEntry;

static PendingBgSeq s_pending_bg;
static BgSnapEntry *s_bg_snaps = NULL;
static int          s_bg_snaps_cap = 0;
static uint32_t     s_bg_snap_clock = 0;

static void flush_pending_bg(SDL_Renderer *renderer) {
    if (!s_pending_bg.active || s_pending_bg.flushing || !renderer || !renderer->screen_tex) return;
    SDL_Texture *dst = renderer->screen_tex;
    s_pending_bg.active = false;

    if (s_pending_bg.count > 0) {
        s_bg_snap_clock++;
        for (int i = 0; i < s_bg_snaps_cap; i++) {
            BgSnapEntry *e = &s_bg_snaps[i];
            if (e->used && e->clear_wm == s_pending_bg.clear_wm &&
                e->w == dst->w && e->h == dst->h && e->count == s_pending_bg.count) {
                bool match = true;
                for (int k = 0; k < e->count; k++) {
                    if (e->orig_px[k] != s_pending_bg.texs[k].px) { match = false; break; }
                }
                if (match) {
                    e->last_use = s_bg_snap_clock;
                    memcpy(dst->px, e->px, (size_t)dst->w * (size_t)dst->h);
                    return;
                }
            }
        }
    }

    size_t n = (size_t)dst->w * (size_t)dst->h;
    memset(dst->px, s_pending_bg.clear_wm, n);
    if (dst->alpha) memset(dst->alpha, 255, n);

    if (s_pending_bg.count > 0) {
        SDL_Texture *saved_target = renderer->cur_target;
        renderer->cur_target = dst;
        s_pending_bg.flushing = true;
        for (int k = 0; k < s_pending_bg.count; k++) {
            SDL_RenderCopy(renderer, &s_pending_bg.texs[k], NULL, NULL);
        }
        s_pending_bg.flushing = false;
        renderer->cur_target = saved_target;

        int slot = -1;
        for (int i = 0; i < s_bg_snaps_cap; i++) {
            if (!s_bg_snaps[i].used) { slot = i; break; }
        }
        if (slot < 0) {
            int new_cap = s_bg_snaps_cap ? (s_bg_snaps_cap * 2) : 8;
            if (new_cap > s_bg_snaps_cap && (size_t)new_cap <= SIZE_MAX / sizeof(BgSnapEntry)) {
                BgSnapEntry *nb = (BgSnapEntry *)realloc(s_bg_snaps, (size_t)new_cap * sizeof(BgSnapEntry));
                if (nb) {
                    memset(nb + s_bg_snaps_cap, 0, (size_t)(new_cap - s_bg_snaps_cap) * sizeof(BgSnapEntry));
                    slot = s_bg_snaps_cap;
                    s_bg_snaps = nb;
                    s_bg_snaps_cap = new_cap;
                }
            }
        }
        if (slot >= 0) {
            uint8_t *snap = (uint8_t *)malloc(n);
            const uint8_t **op = (const uint8_t **)malloc((size_t)s_pending_bg.count * sizeof(const uint8_t *));
            if (snap && op) {
                BgSnapEntry *e = &s_bg_snaps[slot];
                memcpy(snap, dst->px, n);
                e->used = true;
                e->clear_wm = s_pending_bg.clear_wm;
                e->w = dst->w;
                e->h = dst->h;
                e->count = s_pending_bg.count;
                for (int k = 0; k < e->count; k++) op[k] = s_pending_bg.texs[k].px;
                e->orig_px = op;
                e->px = snap;
                e->last_use = s_bg_snap_clock;
            } else {
                free(snap);
                free((void *)op);
            }
        }
    }
}

int SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture) {
    if (!renderer) return -1;
    if (renderer->cur_target == renderer->screen_tex && texture && texture != renderer->screen_tex) {
        flush_pending_bg(renderer);
    }
    renderer->cur_target = texture ? texture : renderer->screen_tex;
    return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!renderer) return -1;
    renderer->draw_r = r;
    renderer->draw_g = g;
    renderer->draw_b = b;
    renderer->draw_a = a;
    renderer->draw_wm = rgb_to_wm(r, g, b);
    return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode) {
    if (!renderer) return -1;
    renderer->draw_blend = blendMode;
    return 0;
}

int SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode) {
    if (!texture) return -1;
    texture->blend = blendMode;
    return 0;
}

int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha) {
    if (!texture) return -1;
    texture->alpha_mod = alpha;
    return 0;
}

static uint8_t s_blend_lut[7][256][256];
static bool    s_blend_lut_ready[7];

static void build_blend_lut(int bucket) {
    int a = ((bucket + 1) * 255 + 3) / 7;
    int inv = 255 - a;
    for (int s = 0; s < 248; s++) {
        int sr = (int)s_wm_palette[s][0] * a;
        int sg = (int)s_wm_palette[s][1] * a;
        int sb = (int)s_wm_palette[s][2] * a;
        for (int d = 0; d < 248; d++) {
            uint8_t r = (uint8_t)((sr + (int)s_wm_palette[d][0] * inv) >> 8);
            uint8_t g = (uint8_t)((sg + (int)s_wm_palette[d][1] * inv) >> 8);
            uint8_t b = (uint8_t)((sb + (int)s_wm_palette[d][2] * inv) >> 8);
            s_blend_lut[bucket][s][d] = rgb_to_wm(r, g, b);
        }
    }
    s_blend_lut_ready[bucket] = true;
}

static inline uint8_t blend_wm(uint8_t src_idx, uint8_t dst_idx, uint8_t a) {
    if (a == 255) return src_idx;
    if (a == 0) return dst_idx;
    int lvl = (int)s_a_to_lvl[a];
    if (lvl <= 0) return dst_idx;
    if (lvl >= 7) return src_idx;
    if (!s_blend_lut_ready[lvl - 1]) build_blend_lut(lvl - 1);
    return s_blend_lut[lvl - 1][src_idx][dst_idx];
}

int SDL_RenderClear(SDL_Renderer *renderer) {
    if (!renderer || !renderer->cur_target) return -1;
    SDL_Texture *dst = renderer->cur_target;
    if (dst == renderer->screen_tex) {
        s_pending_bg.active = true;
        s_pending_bg.clear_wm = renderer->draw_wm;
        s_pending_bg.count = 0;
        return 0;
    }
    size_t n = (size_t)dst->w * (size_t)dst->h;
    memset(dst->px, renderer->draw_wm, n);
    if (dst->alpha) {
        memset(dst->alpha, renderer->draw_a, n);
    }
    return 0;
}

static inline void render_put_pixel_raw(SDL_Renderer *renderer, int x, int y) {
    SDL_Texture *dst = renderer->cur_target;
    if ((unsigned)x >= (unsigned)dst->w || (unsigned)y >= (unsigned)dst->h) return;
    size_t off = (size_t)y * (size_t)dst->w + (size_t)x;
    if (renderer->draw_blend == SDL_BLENDMODE_BLEND && renderer->draw_a < 255) {
        dst->px[off] = blend_wm(renderer->draw_wm, dst->px[off], renderer->draw_a);
    } else {
        dst->px[off] = renderer->draw_wm;
        if (dst->alpha) dst->alpha[off] = renderer->draw_a;
    }
}

int SDL_RenderDrawPoint(SDL_Renderer *renderer, int x, int y) {
    if (!renderer || !renderer->cur_target) return -1;
    if (renderer->cur_target == renderer->screen_tex) {
        flush_pending_bg(renderer);
        if (renderer->scale_div > 1) {
            x /= renderer->scale_div;
            y /= renderer->scale_div;
        }
    }
    render_put_pixel_raw(renderer, x, y);
    return 0;
}

int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2) {
    if (!renderer || !renderer->cur_target) return -1;
    if (renderer->cur_target == renderer->screen_tex) {
        flush_pending_bg(renderer);
        if (renderer->scale_div > 1) {
            int s = renderer->scale_div;
            x1 /= s; y1 /= s; x2 /= s; y2 /= s;
        }
    }
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        render_put_pixel_raw(renderer, x1, y1);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
    return 0;
}

int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->cur_target) return -1;
    SDL_Texture *dst = renderer->cur_target;
    if (dst == renderer->screen_tex) flush_pending_bg(renderer);
    SDL_Rect r;
    if (!rect) {
        r = (SDL_Rect){0, 0, dst->w, dst->h};
    } else {
        r = *rect;
        if (dst == renderer->screen_tex && renderer->scale_div > 1) {
            int s = renderer->scale_div;
            int nx0 = r.x / s, ny0 = r.y / s;
            int nx1 = (r.x + r.w + s - 1) / s;
            int ny1 = (r.y + r.h + s - 1) / s;
            r.x = nx0; r.y = ny0;
            r.w = (r.w > 0 && nx1 <= nx0) ? 1 : (nx1 - nx0);
            r.h = (r.h > 0 && ny1 <= ny0) ? 1 : (ny1 - ny0);
        }
    }
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = (r.x + r.w > dst->w) ? dst->w : (r.x + r.w);
    int y1 = (r.y + r.h > dst->h) ? dst->h : (r.y + r.h);
    if (x0 >= x1 || y0 >= y1) return 0;
    int rw = x1 - x0;
    bool blend = (renderer->draw_blend == SDL_BLENDMODE_BLEND && renderer->draw_a < 255);
    uint8_t wm = renderer->draw_wm;
    uint8_t a = renderer->draw_a;

    for (int y = y0; y < y1; y++) {
        size_t row = (size_t)y * (size_t)dst->w + (size_t)x0;
        if (!blend) {
            memset(dst->px + row, wm, (size_t)rw);
            if (dst->alpha && dst != renderer->screen_tex) {
                memset(dst->alpha + row, a, (size_t)rw);
            }
        } else {
            for (int x = 0; x < rw; x++) {
                dst->px[row + x] = blend_wm(wm, dst->px[row + x], a);
            }
        }
    }
    return 0;
}

int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect) {
    if (!renderer || !renderer->cur_target) return -1;
    SDL_Texture *dst = renderer->cur_target;
    SDL_Rect r;
    if (!rect) {
        int s = (dst == renderer->screen_tex && renderer->scale_div > 1) ? renderer->scale_div : 1;
        r = (SDL_Rect){0, 0, dst->w * s, dst->h * s};
    } else {
        r = *rect;
    }
    if (r.w <= 0 || r.h <= 0) return 0;
    SDL_RenderDrawLine(renderer, r.x, r.y, r.x + r.w - 1, r.y);
    SDL_RenderDrawLine(renderer, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1);
    SDL_RenderDrawLine(renderer, r.x, r.y, r.x, r.y + r.h - 1);
    SDL_RenderDrawLine(renderer, r.x + r.w - 1, r.y, r.x + r.w - 1, r.y + r.h - 1);
    return 0;
}

typedef struct {
    bool used;
    const uint8_t *orig_px;
    int dst_w, dst_h;
    int min_x, min_y, max_x, max_y;
    uint8_t *px;
    uint8_t *alpha;
    uint32_t last_use;
} ScaleCacheEntry;

static ScaleCacheEntry *s_scale_cache = NULL;
static int s_scale_cache_cap = 0;
static uint32_t s_scale_cache_clock = 0;

static void tetris_render_caches_free(void) {
    free(s_pending_bg.texs);
    s_pending_bg.texs = NULL;
    s_pending_bg.count = 0;
    s_pending_bg.cap = 0;
    for (int i = 0; i < s_bg_snaps_cap; i++) {
        if (s_bg_snaps[i].used) {
            free((void *)s_bg_snaps[i].orig_px);
            free(s_bg_snaps[i].px);
        }
    }
    free(s_bg_snaps);
    s_bg_snaps = NULL;
    s_bg_snaps_cap = 0;
    for (int i = 0; i < s_scale_cache_cap; i++) {
        if (s_scale_cache[i].used) {
            free(s_scale_cache[i].px);
            free(s_scale_cache[i].alpha);
        }
    }
    free(s_scale_cache);
    s_scale_cache = NULL;
    s_scale_cache_cap = 0;
}

static void lookup_or_build_scaled(const SDL_Texture *texture, int dw, int dh,
                                   const uint8_t **out_px, const uint8_t **out_al,
                                   int *min_x, int *min_y, int *max_x, int *max_y) {
    s_scale_cache_clock++;
    for (int i = 0; i < s_scale_cache_cap; i++) {
        ScaleCacheEntry *e = &s_scale_cache[i];
        if (e->used && e->orig_px == texture->px && e->dst_w == dw && e->dst_h == dh) {
            e->last_use = s_scale_cache_clock;
            *out_px = e->px;
            *out_al = e->alpha;
            *min_x = e->min_x; *min_y = e->min_y;
            *max_x = e->max_x; *max_y = e->max_y;
            return;
        }
    }

    int slot = -1;
    for (int i = 0; i < s_scale_cache_cap; i++) {
        if (!s_scale_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_scale_cache_cap ? (s_scale_cache_cap * 2) : 32;
        if (new_cap > s_scale_cache_cap && (size_t)new_cap <= SIZE_MAX / sizeof(ScaleCacheEntry)) {
            ScaleCacheEntry *ns = (ScaleCacheEntry *)realloc(s_scale_cache, (size_t)new_cap * sizeof(ScaleCacheEntry));
            if (ns) {
                memset(ns + s_scale_cache_cap, 0, (size_t)(new_cap - s_scale_cache_cap) * sizeof(ScaleCacheEntry));
                slot = s_scale_cache_cap;
                s_scale_cache = ns;
                s_scale_cache_cap = new_cap;
            }
        }
    }
    if (slot < 0) return;

    size_t n = (size_t)dw * (size_t)dh;
    uint8_t *px = (uint8_t *)malloc(n);
    uint8_t *al = texture->alpha ? (uint8_t *)malloc(n) : NULL;
    if (!px || (texture->alpha && !al)) { free(px); free(al); return; }

    int *sx_lut = (int *)malloc((size_t)dw * sizeof(int));
    if (!sx_lut) { free(px); free(al); return; }
    for (int x = 0; x < dw; x++) sx_lut[x] = (x * texture->w) / dw;

    int bx0 = dw, by0 = dh, bx1 = 0, by1 = 0;
    for (int y = 0; y < dh; y++) {
        int sy = (y * texture->h) / dh;
        const uint8_t *spx = texture->px + (size_t)sy * (size_t)texture->w;
        const uint8_t *sal = texture->alpha ? (texture->alpha + (size_t)sy * (size_t)texture->w) : NULL;
        uint8_t *dpx = px + (size_t)y * (size_t)dw;
        uint8_t *dal = al ? (al + (size_t)y * (size_t)dw) : NULL;
        for (int x = 0; x < dw; x++) {
            int sx = sx_lut[x];
            dpx[x] = spx[sx];
            if (dal) {
                uint8_t a = sal[sx];
                dal[x] = a;
                if (a != 0) {
                    if (x < bx0) bx0 = x;
                    if (x + 1 > bx1) bx1 = x + 1;
                    if (y < by0) by0 = y;
                    if (y + 1 > by1) by1 = y + 1;
                }
            }
        }
    }
    free(sx_lut);
    if (!al) { bx0 = 0; by0 = 0; bx1 = dw; by1 = dh; }

    ScaleCacheEntry *e = &s_scale_cache[slot];
    if (e->used) { free(e->px); free(e->alpha); }
    e->used = true;
    e->orig_px = texture->px;
    e->dst_w = dw; e->dst_h = dh;
    e->min_x = bx0; e->min_y = by0; e->max_x = bx1; e->max_y = by1;
    e->px = px; e->alpha = al;
    e->last_use = s_scale_cache_clock;

    *out_px = px; *out_al = al;
    *min_x = bx0; *min_y = by0; *max_x = bx1; *max_y = by1;
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
    if (!renderer || !renderer->cur_target || !texture || !texture->px) return -1;
    SDL_Texture *dst = renderer->cur_target;

    SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0, 0, texture->w, texture->h};
    SDL_Rect dr;
    if (!dstrect) {
        dr = (SDL_Rect){0, 0, dst->w, dst->h};
    } else {
        dr = *dstrect;
        if (dst == renderer->screen_tex && renderer->scale_div > 1) {
            int s = renderer->scale_div;
            int nx0 = dr.x / s, ny0 = dr.y / s;
            int nx1 = (dr.x + dr.w + s - 1) / s;
            int ny1 = (dr.y + dr.h + s - 1) / s;
            dr.x = nx0; dr.y = ny0;
            dr.w = (dr.w > 0 && nx1 <= nx0) ? 1 : (nx1 - nx0);
            dr.h = (dr.h > 0 && ny1 <= ny0) ? 1 : (ny1 - ny0);
        }
    }
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0) return 0;

    if (dst == renderer->screen_tex && s_pending_bg.active && !s_pending_bg.flushing) {
        if (texture->is_cached && texture->alpha_mod == 255 &&
            sr.x == 0 && sr.y == 0 && sr.w == texture->w && sr.h == texture->h &&
            dr.x == 0 && dr.y == 0 && dr.w == dst->w && dr.h == dst->h) {
            if (s_pending_bg.count >= s_pending_bg.cap) {
                int new_cap = s_pending_bg.cap ? (s_pending_bg.cap * 2) : 8;
                if (new_cap > s_pending_bg.cap && (size_t)new_cap <= SIZE_MAX / sizeof(SDL_Texture)) {
                    SDL_Texture *nt = (SDL_Texture *)realloc(s_pending_bg.texs, (size_t)new_cap * sizeof(SDL_Texture));
                    if (nt) {
                        s_pending_bg.texs = nt;
                        s_pending_bg.cap = new_cap;
                    }
                }
            }
            if (s_pending_bg.count < s_pending_bg.cap) {
                s_pending_bg.texs[s_pending_bg.count++] = *texture;
                return 0;
            }
        }
        flush_pending_bg(renderer);
    }

    const uint8_t *src_px = texture->px;
    const uint8_t *src_al = texture->alpha;
    int src_w = texture->w;
    int src_h = texture->h;
    int bmin_x = texture->min_x, bmin_y = texture->min_y;
    int bmax_x = texture->max_x, bmax_y = texture->max_y;

    /* Use pre-scaled 1:1 buffer when a cached texture is drawn at a repeated scaled size */
    if (texture->is_cached && sr.x == 0 && sr.y == 0 &&
        sr.w == src_w && sr.h == src_h &&
        (src_w != dr.w || src_h != dr.h) &&
        (size_t)dr.w * (size_t)dr.h <= 250000u) {
        const uint8_t *c_px = NULL, *c_al = NULL;
        int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        lookup_or_build_scaled(texture, dr.w, dr.h, &c_px, &c_al, &cx0, &cy0, &cx1, &cy1);
        if (c_px) {
            src_px = c_px;
            src_al = c_al;
            src_w = dr.w;
            src_h = dr.h;
            sr.w = dr.w;
            sr.h = dr.h;
            bmin_x = cx0; bmin_y = cy0;
            bmax_x = cx1; bmax_y = cy1;
        }
    }

    int dx0 = dr.x < 0 ? 0 : dr.x;
    int dy0 = dr.y < 0 ? 0 : dr.y;
    int dx1 = (dr.x + dr.w > dst->w) ? dst->w : (dr.x + dr.w);
    int dy1 = (dr.y + dr.h > dst->h) ? dst->h : (dr.y + dr.h);

    /* Clip full-texture copies tightly to non-zero alpha bounding box */
    if (sr.x == 0 && sr.y == 0 && sr.w == src_w && sr.h == src_h &&
        src_al && bmax_x > bmin_x && bmax_y > bmin_y) {
        int bx0 = dr.x + (bmin_x * dr.w) / src_w;
        int bx1 = dr.x + (bmax_x * dr.w + src_w - 1) / src_w;
        int by0 = dr.y + (bmin_y * dr.h) / src_h;
        int by1 = dr.y + (bmax_y * dr.h + src_h - 1) / src_h;
        if (bx0 > dx0) dx0 = bx0;
        if (bx1 < dx1) dx1 = bx1;
        if (by0 > dy0) dy0 = by0;
        if (by1 < dy1) dy1 = by1;
    } else if (sr.x == 0 && sr.y == 0 && sr.w == src_w && sr.h == src_h &&
               src_al && bmax_x <= bmin_x) {
        return 0;
    }

    if (dx0 >= dx1 || dy0 >= dy1) {
        return 0;
    }

    int cw = dx1 - dx0;
    uint8_t amod = texture->alpha_mod;
    bool opaque_copy = (!src_al && amod == 255);
    bool keep_alpha = (dst != renderer->screen_tex && dst->alpha != NULL);

    /* Fast 1:1 unscaled path */
    if (sr.w == dr.w && sr.h == dr.h &&
        sr.x >= 0 && sr.y >= 0 && sr.x + dr.w <= src_w && sr.y + dr.h <= src_h) {
        int sx0 = sr.x + (dx0 - dr.x);
        if (opaque_copy && !keep_alpha) {
            for (int dy = dy0; dy < dy1; dy++) {
                int sy = sr.y + (dy - dr.y);
                memcpy(dst->px + (size_t)dy * (size_t)dst->w + (size_t)dx0,
                       src_px + (size_t)sy * (size_t)src_w + (size_t)sx0,
                       (size_t)cw);
            }
            return 0;
        }
        if (!keep_alpha && src_al && amod == 255) {
            for (int dy = dy0; dy < dy1; dy++) {
                int sy = sr.y + (dy - dr.y);
                const uint8_t *spx = src_px + (size_t)sy * (size_t)src_w + (size_t)sx0;
                const uint8_t *sal = src_al + (size_t)sy * (size_t)src_w + (size_t)sx0;
                uint8_t *dpx = dst->px + (size_t)dy * (size_t)dst->w + (size_t)dx0;
                int i = 0;
                while (i + 4 <= cw) {
                    uint32_t a4 = *(const uint32_t *)(sal + i);
                    if (a4 == 0u) {
                        i += 4;
                        continue;
                    }
                    if (a4 == 0xFFFFFFFFu) {
                        *(uint32_t *)(dpx + i) = *(const uint32_t *)(spx + i);
                        i += 4;
                        continue;
                    }
                    for (int k = 0; k < 4; k++) {
                        uint8_t a = sal[i + k];
                        if (a == 255) dpx[i + k] = spx[i + k];
                        else if (a != 0) dpx[i + k] = blend_wm(spx[i + k], dpx[i + k], a);
                    }
                    i += 4;
                }
                for (; i < cw; i++) {
                    uint8_t a = sal[i];
                    if (a == 255) dpx[i] = spx[i];
                    else if (a != 0) dpx[i] = blend_wm(spx[i], dpx[i], a);
                }
            }
            return 0;
        }
    }

    int sx_lut_stack[1024];
    int *sx_lut = (cw <= 1024) ? sx_lut_stack : (int *)malloc((size_t)cw * sizeof(int));
    if (!sx_lut) return -1;
    for (int i = 0; i < cw; i++) {
        int sx = sr.x + ((dx0 + i - dr.x) * sr.w) / dr.w;
        if (sx < 0) sx = 0;
        if (sx >= src_w) sx = src_w - 1;
        sx_lut[i] = sx;
    }

    for (int dy = dy0; dy < dy1; dy++) {
        int sy = sr.y + ((dy - dr.y) * sr.h) / dr.h;
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;

        const uint8_t *spx = src_px + (size_t)sy * (size_t)src_w;
        const uint8_t *sal = src_al ? (src_al + (size_t)sy * (size_t)src_w) : NULL;
        uint8_t *dpx = dst->px + (size_t)dy * (size_t)dst->w + (size_t)dx0;
        uint8_t *dal = keep_alpha ? (dst->alpha + (size_t)dy * (size_t)dst->w + (size_t)dx0) : NULL;

        if (opaque_copy) {
            for (int i = 0; i < cw; i++) {
                dpx[i] = spx[sx_lut[i]];
                if (dal) dal[i] = 255;
            }
        } else if (!sal) {
            for (int i = 0; i < cw; i++) {
                dpx[i] = blend_wm(spx[sx_lut[i]], dpx[i], amod);
                if (dal && amod > dal[i]) dal[i] = amod;
            }
        } else {
            for (int i = 0; i < cw; i++) {
                int sx = sx_lut[i];
                uint8_t a = sal[sx];
                if (a == 0) continue;
                if (amod < 255) a = (uint8_t)(((uint32_t)a * amod) >> 8);
                if (a == 0) continue;
                if (a == 255) {
                    dpx[i] = spx[sx];
                    if (dal) dal[i] = 255;
                } else {
                    dpx[i] = blend_wm(spx[sx], dpx[i], a);
                    if (dal && a > dal[i]) dal[i] = a;
                }
            }
        }
    }

    if (sx_lut != sx_lut_stack) free(sx_lut);
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *renderer) {
    if (!renderer || !renderer->win || !renderer->screen_tex) return;
    flush_pending_bg(renderer);
    SDL_Window *w = renderer->win;
    SDL_Texture *fb = renderer->screen_tex;

    static uint32_t s_next_ms = 0;
    static uint32_t s_fps_t0 = 0;
    static uint32_t s_fps_frames = 0;
    uint32_t now = timer_millis();
    if (s_next_ms == 0) {
        s_next_ms = now;
        s_fps_t0 = now;
    }

    init_wm_palette_lut();
    if (!ipo_hd_present_pal32(&w->hd, w->wm_win, w->direct_gfx, fb->px, fb->w, fb->h, fb->w, s_wm_pal32)) {
        ipo_hd_present(&w->hd, w->wm_win, w->direct_gfx, fb->px, fb->w, fb->h, fb->w);
    }

    pump_audio_mixer();

    s_fps_frames++;
    now = timer_millis();
    uint32_t elapsed = now - s_fps_t0;
    if (elapsed >= 1000u) {
        serial_printf("[tetris] %u FPS\n", (s_fps_frames * 1000u) / elapsed);
        s_fps_frames = 0;
        s_fps_t0 = now;
    }

    /* Cap at ~60 FPS cleanly */
    s_next_ms += 16u;
    if ((int32_t)(now - s_next_ms) > 64) s_next_ms = now + 16u;
    while (timer_millis() < s_next_ms) {
        pump_audio_mixer();
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    (void)mode;
    size_t sz = 0;
    uint8_t *buf = read_entire_file(file, &sz);
    if (!buf) return NULL;
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!rw) { free(buf); return NULL; }
    rw->data = buf;
    rw->size = sz;
    rw->own_mem = SDL_TRUE;
    return rw;
}

SDL_RWops *SDL_RWFromConstMem(const void *mem, int size) {
    if (!mem || size <= 0) return NULL;
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!rw) return NULL;
    rw->data = mem;
    rw->size = (size_t)size;
    rw->own_mem = SDL_FALSE;
    return rw;
}

size_t SDL_RWread(SDL_RWops *context, void *ptr, size_t size, size_t maxnum) {
    if (!context || !ptr || size == 0 || maxnum == 0) return 0;
    size_t avail = (context->pos < context->size) ? (context->size - context->pos) : 0;
    size_t max_objs = avail / size;
    size_t count = (maxnum < max_objs) ? maxnum : max_objs;
    if (count > 0) {
        memcpy(ptr, (const uint8_t *)context->data + context->pos, count * size);
        context->pos += count * size;
    }
    return count;
}

int SDL_RWclose(SDL_RWops *context) {
    if (!context) return 0;
    if (context->own_mem) free((void *)context->data);
    free(context);
    return 0;
}

void SDL_FreeRW(SDL_RWops *area) {
    SDL_RWclose(area);
}

void *ns_texture_new(int width, int height, int format, void *bytes, size_t stride) {
    (void)width; (void)height; (void)format; (void)bytes; (void)stride;
    return NULL;
}
