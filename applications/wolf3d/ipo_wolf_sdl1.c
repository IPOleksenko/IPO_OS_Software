#include "SDL/SDL.h"
#include "SDL/SDL_mixer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/sound.h>
#include <system/timer.h>
#include <unistd.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>

#include "common/ipo_hd_present.h"

#define AUDIO_RING_SAMPLES 16384u

typedef struct {
    Mix_Chunk *chunk;
    uint32_t pos;
    int loops;
    int tag;
    uint8_t pan_l;
    uint8_t pan_r;
    bool active;
} wolf_voice_t;

static wm_window_t *s_wm_win = NULL;
static bool s_direct_gfx = false;
static ipo_hd_view_t s_hd;
static SDL_Surface *s_screen = NULL;
static const uint8_t *s_last_pres_px = NULL;
static int s_last_pres_w = 0;
static int s_last_pres_h = 0;
static int s_last_pres_pitch = 0;

static SDL_PixelFormat s_vfmt;
static SDL_VideoInfo s_vinfo;
static SDL_Color s_hw_pal[256];
static bool s_hw_pal_valid = false;
static uint8_t s_wm_palette[248][3];
static bool s_wm_pal_ready = false;
static uint8_t s_wm_lut[256];
static bool s_wm_lut_valid = false;
static SDL_Color s_last_lut_pal[256];

static uint8_t s_keystate[SDLK_LAST];
static SDLMod s_modstate = KMOD_NONE;
static bool s_ctrl_down = false;
static bool s_alt_down = false;
static int64_t s_prev_cum_x = 0;
static int64_t s_prev_cum_y = 0;
static bool s_cum_init = false;
static int s_mouse_dx = 0;
static int s_mouse_dy = 0;
static uint8_t s_mouse_btns = 0;
static bool s_ignore_motion = false;

static SDL_Event *s_ev_q = NULL;
static int s_ev_cap = 0;
static int s_ev_head = 0;
static int s_ev_tail = 0;
static const char *s_last_err = "";

/* Audio */
static bool s_audio_inited = false;
static bool s_audio_ready = false;
static uint32_t s_audio_freq = 44100u;
static audio_stream_t *s_astream = NULL;
static int16_t s_audio_ring[AUDIO_RING_SAMPLES];
static uint32_t s_audio_write_samples = 0;
static uint32_t s_audio_start_ms = 0;
static uint64_t s_audio_total_written = 0;

static void (*s_music_hook)(void *, Uint8 *, int) = NULL;
static void *s_music_udata = NULL;
static void (*s_chan_finished)(int) = NULL;
static wolf_voice_t *s_voices = NULL;
static int s_voices_cap = 0;
static int s_reserved_chans = 0;
static int16_t *s_music_tmp = NULL;
static size_t s_music_tmp_samples = 0;

static bool wolf_ensure_voices(int need) {
    if (need <= s_voices_cap) return true;
    int new_cap = s_voices_cap ? s_voices_cap : 16;
    while (new_cap < need) {
        if (new_cap > INT32_MAX / 2) return false;
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(wolf_voice_t)) return false;
    wolf_voice_t *nv = (wolf_voice_t *)realloc(s_voices, (size_t)new_cap * sizeof(wolf_voice_t));
    if (!nv) return false;
    memset(nv + s_voices_cap, 0, (size_t)(new_cap - s_voices_cap) * sizeof(wolf_voice_t));
    for (int i = s_voices_cap; i < new_cap; i++) {
        nv[i].pan_l = 255;
        nv[i].pan_r = 255;
    }
    s_voices = nv;
    s_voices_cap = new_cap;
    return true;
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void init_wm_palette_table(void) {
    if (s_wm_pal_ready) return;
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
    s_wm_pal_ready = true;
}

static void restore_wm_dac_palette(void) {
    init_wm_palette_table();
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
    s_hw_pal_valid = false;
}

static uint8_t s_rgb555_lut[32768];
static uint8_t s_rgb555_valid[32768];

static uint8_t nearest_wm_color(uint8_t r, uint8_t g, uint8_t b) {
    init_wm_palette_table();
    uint32_t key = ((uint32_t)(r >> 3) << 10) | ((uint32_t)(g >> 3) << 5) | (uint32_t)(b >> 3);
    if (s_rgb555_valid[key]) {
        return s_rgb555_lut[key];
    }
    int best = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    for (int i = 0; i < 248; i++) {
        int dr = (int)r - (int)s_wm_palette[i][0];
        int dg = (int)g - (int)s_wm_palette[i][1];
        int db = (int)b - (int)s_wm_palette[i][2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) { best_d = d; best = i; }
    }
    s_rgb555_lut[key] = (uint8_t)best;
    s_rgb555_valid[key] = 1u;
    return (uint8_t)best;
}

static void apply_hw_palette(const SDL_Color *pal) {
    if (!pal) return;
    for (int i = 0; i < 256; i++) {
        if (!s_hw_pal_valid ||
            s_hw_pal[i].r != pal[i].r ||
            s_hw_pal[i].g != pal[i].g ||
            s_hw_pal[i].b != pal[i].b) {
            s_hw_pal[i] = pal[i];
            vga_gfx_set_palette((uint8_t)i,
                                (uint8_t)(pal[i].r >> 2),
                                (uint8_t)(pal[i].g >> 2),
                                (uint8_t)(pal[i].b >> 2));
        }
    }
    s_hw_pal_valid = true;
}

static void mix_audio_pump(void) {
    if (!s_astream || !s_audio_ready) return;

    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    uint64_t played = 0ULL;
    if (ipo_audio_get_stream_clock(s_astream, &clk) == IPO_AUDIO_OK) {
        played = clk.played_frames;
    } else {
        uint32_t now = timer_millis();
        played = ((uint64_t)(now - s_audio_start_ms) * s_audio_freq) / 1000ULL;
    }

    uint64_t target = played + (s_audio_freq / 15u);
    if (target <= s_audio_total_written) {
        audio_tick();
        return;
    }

    uint64_t todo = target - s_audio_total_written;
    uint64_t ahead = (s_audio_total_written > played) ? (s_audio_total_written - played) : 0ULL;
    uint64_t max_safe = (AUDIO_RING_SAMPLES > ahead + 128u) ? (AUDIO_RING_SAMPLES - ahead - 128u) : 0ULL;
    if (todo > max_safe) todo = max_safe;
    if (todo > s_audio_freq / 15u) todo = s_audio_freq / 15u;
    if (todo == 0) {
        audio_tick();
        return;
    }

    size_t need_samples = (size_t)todo * 2u;
    if (need_samples > s_music_tmp_samples) {
        size_t new_cap = s_music_tmp_samples ? s_music_tmp_samples : 1024u;
        while (new_cap < need_samples) {
            if (new_cap > SIZE_MAX / (2u * sizeof(int16_t))) {
                new_cap = need_samples;
                break;
            }
            new_cap *= 2u;
        }
        int16_t *nb = (int16_t *)realloc(s_music_tmp, new_cap * sizeof(int16_t));
        if (!nb) {
            audio_tick();
            return;
        }
        s_music_tmp = nb;
        s_music_tmp_samples = new_cap;
    }

    int32_t n = (int32_t)todo;
    if (s_music_hook) {
        s_music_hook(s_music_udata, (Uint8 *)s_music_tmp, n * 4);
    } else {
        memset(s_music_tmp, 0, (size_t)n * 4u);
    }

    for (int32_t i = 0; i < n; i++) {
        int32_t acc = ((int32_t)s_music_tmp[i * 2] + (int32_t)s_music_tmp[i * 2 + 1]) >> 1;
        for (int v = 0; v < s_voices_cap; v++) {
            wolf_voice_t *vc = &s_voices[v];
            if (!vc->active || !vc->chunk || vc->chunk->count == 0) continue;
            if (vc->pos >= vc->chunk->count) {
                if (vc->loops != 0) {
                    if (vc->loops > 0) vc->loops--;
                    vc->pos = 0;
                } else {
                    vc->active = false;
                    if (s_chan_finished) s_chan_finished(v);
                    continue;
                }
            }
            int32_t gain = ((int32_t)vc->pan_l + (int32_t)vc->pan_r) >> 1;
            acc += ((int32_t)vc->chunk->samples[vc->pos++] * gain) >> 8;
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

static SDLKey scancode_to_sdlk(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x1C: return SDLK_KP_ENTER;
            case 0x1D: return SDLK_RCTRL;
            case 0x38: return SDLK_RALT;
            case 0x47: return SDLK_HOME;
            case 0x48: return SDLK_UP;
            case 0x49: return SDLK_PAGEUP;
            case 0x4B: return SDLK_LEFT;
            case 0x4D: return SDLK_RIGHT;
            case 0x4F: return SDLK_END;
            case 0x50: return SDLK_DOWN;
            case 0x51: return SDLK_PAGEDOWN;
            case 0x52: return SDLK_INSERT;
            case 0x53: return SDLK_DELETE;
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
        case 0x38: return SDLK_LALT;
        case 0x39: return SDLK_SPACE;
        case 0x3A: return SDLK_CAPSLOCK;
        case 0x3B: return SDLK_F1;  case 0x3C: return SDLK_F2;
        case 0x3D: return SDLK_F3;  case 0x3E: return SDLK_F4;
        case 0x3F: return SDLK_F5;  case 0x40: return SDLK_F6;
        case 0x41: return SDLK_F7;  case 0x42: return SDLK_F8;
        case 0x43: return SDLK_F9;  case 0x44: return SDLK_F10;
        case 0x45: return SDLK_NUMLOCK;
        case 0x46: return SDLK_SCROLLOCK;
        case 0x48: return SDLK_KP8;
        case 0x4B: return SDLK_KP4;
        case 0x4C: return SDLK_KP5;
        case 0x4D: return SDLK_KP6;
        case 0x50: return SDLK_KP2;
        case 0x57: return SDLK_F11;
        case 0x58: return SDLK_F12;
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

static void handle_key_scancode(SDLKey sym, bool pressed) {
    if (sym == SDLK_UNKNOWN || sym >= SDLK_LAST) return;

    if (sym == SDLK_LCTRL || sym == SDLK_RCTRL) {
        s_ctrl_down = pressed;
        if (pressed) s_modstate = (SDLMod)(s_modstate | KMOD_LCTRL);
        else s_modstate = (SDLMod)(s_modstate & ~KMOD_LCTRL);
    } else if (sym == SDLK_LALT || sym == SDLK_RALT) {
        s_alt_down = pressed;
        if (pressed) s_modstate = (SDLMod)(s_modstate | KMOD_LALT);
        else s_modstate = (SDLMod)(s_modstate & ~KMOD_LALT);
    } else if (sym == SDLK_LSHIFT || sym == SDLK_RSHIFT) {
        if (pressed) s_modstate = (SDLMod)(s_modstate | KMOD_LSHIFT);
        else s_modstate = (SDLMod)(s_modstate & ~KMOD_LSHIFT);
    }

    s_keystate[sym] = pressed ? 1u : 0u;

    if (pressed && s_ctrl_down && (sym == SDLK_c || sym == SDLK_q)) {
        SDL_Event qev;
        memset(&qev, 0, sizeof(qev));
        qev.type = SDL_QUIT;
        push_event(&qev);
        return;
    }
    if (pressed && s_alt_down && sym == SDLK_F4) {
        SDL_Event qev;
        memset(&qev, 0, sizeof(qev));
        qev.type = SDL_QUIT;
        push_event(&qev);
        return;
    }

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.state = pressed ? 1u : 0u;
    ev.key.keysym.scancode = (Uint8)(sym & 0xFF);
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = s_modstate;
    push_event(&ev);
}

static void pump_os_events(void) {
    if (s_wm_win) {
        if (!wm_is_window_valid(s_wm_win) || !wm_session_active()) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_QUIT;
            push_event(&ev);
            return;
        }
        wm_event_t we;
        static bool s_wm_e0 = false;
        while (wm_poll_event(s_wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                SDL_Event ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = SDL_QUIT;
                push_event(&ev);
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool is_e0 = ((kc & 0xFF00u) == 0xE000u);
                uint8_t sc = (uint8_t)(kc & 0x7Fu);
                SDLKey sym = scancode_to_sdlk(sc, is_e0);
                if (sym != SDLK_UNKNOWN) handle_key_scancode(sym, !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { s_wm_e0 = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                SDLKey sym = scancode_to_sdlk(raw & 0x7Fu, s_wm_e0);
                s_wm_e0 = false;
                if (sym != SDLK_UNKNOWN) handle_key_scancode(sym, !rel);
            }
        }
    } else if (s_direct_gfx) {
        static bool s_dir_e0 = false;
        keyboard_poll();
        uint8_t raw;
        while ((raw = keyboard_get_scancode()) != 0) {
            if (raw == 0xE0) { s_dir_e0 = true; continue; }
            bool e0 = s_dir_e0;
            s_dir_e0 = false;
            bool rel = (raw & 0x80u) != 0u;
            uint8_t sc = raw & 0x7Fu;
            if (sc == 0) continue;
            SDLKey sym = scancode_to_sdlk(sc, e0);
            if (sym != SDLK_UNKNOWN) handle_key_scancode(sym, !rel);
        }
    }

    mouse_state_t ms;
    mouse_get_state(&ms);
    int64_t cx = 0, cy = 0;
    mouse_get_cumulative_motion(&cx, &cy);
    if (!s_cum_init) {
        s_prev_cum_x = cx;
        s_prev_cum_y = cy;
        s_cum_init = true;
    } else {
        s_mouse_dx += (int)(cx - s_prev_cum_x);
        s_mouse_dy += (int)(cy - s_prev_cum_y);
        s_prev_cum_x = cx;
        s_prev_cum_y = cy;
    }
    uint8_t b = 0;
    if (ms.buttons & MOUSE_BTN_LEFT)   b |= SDL_BUTTON(SDL_BUTTON_LEFT);
    if (ms.buttons & MOUSE_BTN_RIGHT)  b |= SDL_BUTTON(SDL_BUTTON_RIGHT);
    if (ms.buttons & MOUSE_BTN_MIDDLE) b |= SDL_BUTTON(SDL_BUTTON_MIDDLE);
    s_mouse_btns = b;
}

static SDL_Surface *alloc_surface_8bpp(int w, int h, Uint32 flags) {
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) return NULL;
    SDL_PixelFormat *fmt = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    SDL_Palette *pal = (SDL_Palette *)calloc(1, sizeof(SDL_Palette));
    SDL_Color *cols = (SDL_Color *)calloc(256, sizeof(SDL_Color));
    void *px = calloc((size_t)w * (size_t)h, 1u);
    if (!fmt || !pal || !cols || !px) {
        free(cols); free(pal); free(fmt); free(px); free(s);
        return NULL;
    }
    pal->ncolors = 256;
    pal->colors = cols;
    fmt->palette = pal;
    fmt->BitsPerPixel = 8;
    fmt->BytesPerPixel = 1;
    s->flags = flags | SDL_HWPALETTE;
    s->format = fmt;
    s->w = w;
    s->h = h;
    s->pitch = (Uint16)w;
    s->pixels = px;
    s->clip_rect.w = (Uint16)w;
    s->clip_rect.h = (Uint16)h;
    s->refcount = 1;
    return s;
}

int SDL_Init(Uint32 flags) {
    (void)flags;
    static bool s_atexit_reg = false;
    if (!s_atexit_reg) {
        s_atexit_reg = true;
        atexit(SDL_Quit);
    }
    init_wm_palette_table();
    memset(s_keystate, 0, sizeof(s_keystate));
    s_ev_head = s_ev_tail = 0;
    s_vfmt.BitsPerPixel = 8;
    s_vfmt.BytesPerPixel = 1;
    s_vinfo.vfmt = &s_vfmt;
    return 0;
}

void SDL_Quit(void) {
    Mix_CloseAudio();
    if (s_screen) {
        SDL_FreeSurface(s_screen);
        s_screen = NULL;
    }
    if (s_wm_win && wm_is_window_valid(s_wm_win)) {
        restore_wm_dac_palette();
        wm_set_cursor_visible(s_wm_win, true);
        wm_destroy_window(s_wm_win);
        s_wm_win = NULL;
    } else if (s_direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
        s_direct_gfx = false;
    }
    ipo_hd_free(&s_hd);
    free(s_ev_q); s_ev_q = NULL; s_ev_cap = 0; s_ev_head = 0; s_ev_tail = 0;
    free(s_voices); s_voices = NULL; s_voices_cap = 0;
}

const char *SDL_GetError(void) {
    return s_last_err;
}

const SDL_VideoInfo *SDL_GetVideoInfo(void) {
    return &s_vinfo;
}

void SDL_WM_SetCaption(const char *title, const char *icon) {
    (void)title; (void)icon;
}

SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode) {
    return mode;
}

int SDL_ShowCursor(int toggle) {
    (void)toggle;
    return 0;
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) {
    (void)width; (void)height; (void)bpp; (void)flags;
    if (s_screen) return s_screen;

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Wolfenstein 3D";
    opt.bg_color = 0;
    opt.fullscreen = false;
    opt.w = 320;
    opt.h = 200;
    opt.x = 0;
    opt.y = 0;

    s_wm_win = wm_create_window(&opt);
    if (s_wm_win) {
        wm_set_fullscreen(s_wm_win, true);
        wm_set_cursor_visible(s_wm_win, false);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        s_direct_gfx = true;
    }

    ipo_hd_setup(&s_hd, s_wm_win, 320, 200, true);
    s_screen = alloc_surface_8bpp(320, 200, SDL_SWSURFACE | SDL_DOUBLEBUF);
    serial_printf("[wolf3d] video ready wm=%p direct=%d scr=%dx%d pres=(%d,%d %dx%d) k=%d\n",
                  (void *)s_wm_win, s_direct_gfx ? 1 : 0,
                  s_hd.scr_w, s_hd.scr_h,
                  s_hd.pres_x, s_hd.pres_y, s_hd.pres_w, s_hd.pres_h, s_hd.scale_k);
    return s_screen;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
    (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return alloc_surface_8bpp(width > 0 ? width : 320, height > 0 ? height : 200, flags);
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags) {
    (void)fmt;
    if (!src) return NULL;
    SDL_Surface *dst = alloc_surface_8bpp(src->w, src->h, flags);
    if (!dst) return NULL;
    if (src->pixels && dst->pixels) {
        memcpy(dst->pixels, src->pixels, (size_t)src->w * (size_t)src->h);
    }
    if (src->format && src->format->palette && dst->format && dst->format->palette) {
        memcpy(dst->format->palette->colors, src->format->palette->colors, 256u * sizeof(SDL_Color));
    }
    return dst;
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    if (surface->format) {
        if (surface->format->palette) {
            free(surface->format->palette->colors);
            free(surface->format->palette);
        }
        free(surface->format);
    }
    free(surface->pixels);
    free(surface);
}

int SDL_LockSurface(SDL_Surface *surface) {
    (void)surface;
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface) {
    (void)surface;
}

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors) {
    return SDL_SetPalette(surface, SDL_LOGPAL | SDL_PHYSPAL, colors, firstcolor, ncolors);
}

int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors, int firstcolor, int ncolors) {
    if (!colors || firstcolor < 0 || firstcolor + ncolors > 256) return 0;
    if (surface && surface->format && surface->format->palette && surface->format->palette->colors) {
        memcpy(&surface->format->palette->colors[firstcolor], colors, (size_t)ncolors * sizeof(SDL_Color));
    }
    if ((flags & SDL_PHYSPAL) != 0 && s_direct_gfx) {
        for (int i = 0; i < ncolors; i++) {
            int idx = firstcolor + i;
            SDL_Color c = colors[i];
            if (!s_hw_pal_valid ||
                s_hw_pal[idx].r != c.r ||
                s_hw_pal[idx].g != c.g ||
                s_hw_pal[idx].b != c.b) {
                s_hw_pal[idx] = c;
                vga_gfx_set_palette((uint8_t)idx,
                                    (uint8_t)(c.r >> 2),
                                    (uint8_t)(c.g >> 2),
                                    (uint8_t)(c.b >> 2));
            }
        }
        s_hw_pal_valid = true;
    } else if ((flags & SDL_PHYSPAL) != 0 && s_last_pres_px && firstcolor == 0 && ncolors == 256) {
        ipo_wolf_present(s_last_pres_px, s_last_pres_w, s_last_pres_h, s_last_pres_pitch, colors);
    }
    return 1;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b) {
    if (!fmt || !fmt->palette || !fmt->palette->colors) return 0;
    int best = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    for (int i = 0; i < fmt->palette->ncolors; i++) {
        int dr = (int)r - (int)fmt->palette->colors[i].r;
        int dg = (int)g - (int)fmt->palette->colors[i].g;
        int db = (int)b - (int)fmt->palette->colors[i].b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) { best_d = d; best = i; }
    }
    return (Uint32)best;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color) {
    if (!dst || !dst->pixels) return -1;
    int x0 = dstrect ? dstrect->x : 0;
    int y0 = dstrect ? dstrect->y : 0;
    int w = dstrect ? dstrect->w : dst->w;
    int h = dstrect ? dstrect->h : dst->h;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > dst->w) w = dst->w - x0;
    if (y0 + h > dst->h) h = dst->h - y0;
    if (w <= 0 || h <= 0) return 0;
    uint8_t *base = (uint8_t *)dst->pixels;
    for (int y = 0; y < h; y++) {
        memset(base + (size_t)(y0 + y) * (size_t)dst->pitch + x0, (int)(color & 0xFFu), (size_t)w);
    }
    return 0;
}

int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    if (!src || !dst || !src->pixels || !dst->pixels) return -1;
    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : src->w;
    int sh = srcrect ? srcrect->h : src->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;
    if (sw <= 0 || sh <= 0) return 0;
    const uint8_t *sp = (const uint8_t *)src->pixels;
    uint8_t *dp = (uint8_t *)dst->pixels;
    for (int y = 0; y < sh; y++) {
        memcpy(dp + (size_t)(dy + y) * (size_t)dst->pitch + dx,
               sp + (size_t)(sy + y) * (size_t)src->pitch + sx,
               (size_t)sw);
    }
    return 0;
}

int SDL_SaveBMP(SDL_Surface *surface, const char *file) {
    (void)surface; (void)file;
    return 0;
}

void SDL_GL_SwapBuffers(void) {
}

Uint32 SDL_GetTicks(void) {
    mix_audio_pump();
    return timer_millis();
}

void SDL_Delay(Uint32 ms) {
    uint32_t end = timer_millis() + ms;
    while ((int32_t)(timer_millis() - end) < 0) {
        mix_audio_pump();
        usleep(1000u);
    }
}

int SDL_PollEvent(SDL_Event *event) {
    mix_audio_pump();
    pump_os_events();
    if (!s_ev_q || s_ev_head == s_ev_tail) return 0;
    if (!event) return 1;
    *event = s_ev_q[s_ev_head];
    s_ev_head = (s_ev_head + 1) % s_ev_cap;
    return 1;
}

int SDL_WaitEvent(SDL_Event *event) {
    while (!SDL_PollEvent(event)) {
        usleep(5000u);
    }
    return 1;
}

Uint8 SDL_EventState(Uint8 type, int state) {
    if (type == SDL_MOUSEMOTION && state == SDL_IGNORE) s_ignore_motion = true;
    return SDL_ENABLE;
}

Uint8 *SDL_GetKeyState(int *numkeys) {
    if (numkeys) *numkeys = SDLK_LAST;
    return s_keystate;
}

SDLMod SDL_GetModState(void) {
    return s_modstate;
}

Uint8 SDL_GetMouseState(int *x, int *y) {
    pump_os_events();
    if (x) *x = s_mouse_dx;
    if (y) *y = s_mouse_dy;
    s_mouse_dx = 0;
    s_mouse_dy = 0;
    return s_mouse_btns;
}

void SDL_WarpMouse(Uint16 x, Uint16 y) {
    (void)x; (void)y;
    s_mouse_dx = 0;
    s_mouse_dy = 0;
}

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int device_index) { (void)device_index; return NULL; }
void SDL_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }
void SDL_JoystickUpdate(void) {}
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis) { (void)joystick; (void)axis; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button) { (void)joystick; (void)button; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat) { (void)joystick; (void)hat; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *joystick) { (void)joystick; return 0; }

SDL_RWops *SDL_RWFromMem(void *mem, int size) {
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!rw) return NULL;
    rw->mem = mem;
    rw->size = size;
    return rw;
}

/* Present 320x200 8bpp buffer cleanly via ipo_hd_present */
void ipo_wolf_present(const uint8_t *pixels, int w, int h, int pitch, const SDL_Color *pal) {
    if (!pixels || w <= 0 || h <= 0) return;
    s_last_pres_px = pixels;
    s_last_pres_w = w;
    s_last_pres_h = h;
    s_last_pres_pitch = pitch;
    static bool s_pres_logged = false;
    if (!s_pres_logged) {
        s_pres_logged = true;
        serial_printf("[wolf3d] first present %dx%d hw=%d focus=%d\n",
                      w, h, s_hd.hw_direct_fb ? 1 : 0,
                      (s_wm_win && wm_get_focused() == s_wm_win) ? 1 : 0);
    }
    mix_audio_pump();

    bool presented32 = false;
    if (pal) {
        static uint32_t s_wolf_pal32[256];
        for (int i = 0; i < 256; i++) {
            s_wolf_pal32[i] = 0xFF000000u | ((uint32_t)pal[i].r << 16) |
                              ((uint32_t)pal[i].g << 8) | (uint32_t)pal[i].b;
        }
        presented32 = ipo_hd_present_pal32(&s_hd, s_direct_gfx ? NULL : s_wm_win, s_direct_gfx,
                                           pixels, w, h, pitch, s_wolf_pal32);
    }
    if (!presented32 && s_direct_gfx) {
        apply_hw_palette(pal);
        ipo_hd_present(&s_hd, NULL, true, pixels, w, h, pitch);
    } else if (!presented32 && s_wm_win && wm_is_window_valid(s_wm_win) && pal) {
        static uint8_t mapped[320 * 200];
        for (int i = 0; i < 256; i++) {
            if (!s_wm_lut_valid ||
                s_last_lut_pal[i].r != pal[i].r ||
                s_last_lut_pal[i].g != pal[i].g ||
                s_last_lut_pal[i].b != pal[i].b) {
                s_last_lut_pal[i] = pal[i];
                s_wm_lut[i] = nearest_wm_color(pal[i].r, pal[i].g, pal[i].b);
            }
        }
        s_wm_lut_valid = true;
        int cw = (w > 320) ? 320 : w;
        int ch = (h > 200) ? 200 : h;
        for (int y = 0; y < ch; y++) {
            const uint8_t *sr = pixels + (size_t)y * (size_t)pitch;
            uint8_t *dr = mapped + (size_t)y * (size_t)cw;
            for (int x = 0; x < cw; x++) dr[x] = s_wm_lut[sr[x]];
        }
        ipo_hd_present(&s_hd, s_wm_win, false, mapped, cw, ch, cw);
    }
    {
        static uint32_t s_fps_t0 = 0, s_fps_n = 0;
        uint32_t now = (uint32_t)timer_millis();
        if (s_fps_t0 == 0u) s_fps_t0 = now;
        s_fps_n++;
        if (now - s_fps_t0 >= 1000u) {
            serial_printf("[wolf3d] %u FPS\n", (unsigned)((s_fps_n * 1000u) / (now - s_fps_t0)));
            s_fps_n = 0;
            s_fps_t0 = now;
        }
    }
}

/* =========================================================================
 * SDL_mixer implementation for Chocolate-Wolfenstein-3D
 * ========================================================================= */
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)format; (void)channels; (void)chunksize;
    if (s_audio_inited) return s_audio_ready ? 0 : -1;
    s_audio_inited = true;
    if (!wolf_ensure_voices(16)) return -1;
    for (int i = 0; i < s_voices_cap; i++) {
        memset(&s_voices[i], 0, sizeof(s_voices[i]));
        s_voices[i].pan_l = 255;
        s_voices[i].pan_r = 255;
    }
    s_audio_freq = (frequency > 8000) ? (uint32_t)frequency : 22050u;
    if (!audio_init()) return 0;
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    s_audio_write_samples = 0;
    s_audio_start_ms = timer_millis();
    s_audio_total_written = 0;
    s_astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1u, s_audio_freq,
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
    free(s_music_tmp);
    s_music_tmp = NULL;
    s_music_tmp_samples = 0;
    s_audio_ready = false;
    s_audio_inited = false;
}

const char *Mix_GetError(void) { return s_last_err; }

int Mix_ReserveChannels(int num) {
    if (num < 0) num = 0;
    if (!wolf_ensure_voices(num > 0 ? num : 1)) return s_reserved_chans;
    s_reserved_chans = num;
    return s_reserved_chans;
}

int Mix_GroupChannels(int from, int to, int tag) {
    if (from < 0) from = 0;
    if (to < from) return 0;
    if (!wolf_ensure_voices(to + 1)) return 0;
    for (int i = from; i <= to; i++) s_voices[i].tag = tag;
    return (to >= from) ? (to - from + 1) : 0;
}

int Mix_GroupAvailable(int tag) {
    for (int i = 0; i < s_voices_cap; i++) {
        if (s_voices[i].tag == tag && !s_voices[i].active) return i;
    }
    return -1;
}

int Mix_GroupOldest(int tag) {
    int best = -1;
    uint32_t max_pos = 0;
    for (int i = 0; i < s_voices_cap; i++) {
        if (s_voices[i].tag == tag && s_voices[i].pos >= max_pos) {
            max_pos = s_voices[i].pos;
            best = i;
        }
    }
    return (best >= 0) ? best : s_reserved_chans;
}

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
    if (!src || !src->mem || src->size < 44) {
        if (freesrc && src) free(src);
        return NULL;
    }
    const uint8_t *buf = (const uint8_t *)src->mem;
    size_t sz = (size_t)src->size;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        if (freesrc) free(src);
        return NULL;
    }

    uint16_t chans = 1, bps = 16;
    const uint8_t *pcm_ptr = NULL;
    uint32_t pcm_sz = 0;
    size_t pos = 12;
    while (pos + 8 <= sz) {
        uint32_t csz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= sz) {
            chans = rd_u16(buf + pos + 10);
            bps   = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0 && pos + 8 + csz <= sz) {
            pcm_ptr = buf + pos + 8;
            pcm_sz  = csz;
        }
        pos += 8 + ((csz + 1u) & ~1u);
    }

    if (!pcm_ptr || pcm_sz == 0 || bps != 16 || chans == 0) {
        if (freesrc) free(src);
        return NULL;
    }

    uint32_t frames = pcm_sz / (2u * chans);
    int16_t *mono = (int16_t *)malloc((size_t)frames * sizeof(int16_t));
    if (!mono) {
        if (freesrc) free(src);
        return NULL;
    }
    const int16_t *in16 = (const int16_t *)pcm_ptr;
    for (uint32_t i = 0; i < frames; i++) {
        if (chans == 1) mono[i] = in16[i];
        else mono[i] = (int16_t)(((int32_t)in16[i * chans] + (int32_t)in16[i * chans + 1]) >> 1);
    }

    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!chunk) {
        free(mono);
        if (freesrc) free(src);
        return NULL;
    }
    chunk->samples = mono;
    chunk->count = frames;
    chunk->volume = 128;
    chunk->abuf = (Uint8 *)mono;
    chunk->alen = frames * 2u;

    if (freesrc) free(src);
    return chunk;
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    for (int i = 0; i < s_voices_cap; i++) {
        if (s_voices[i].chunk == chunk) s_voices[i].active = false;
    }
    free(chunk->samples);
    free(chunk);
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    if (!chunk) return -1;
    int slot = -1;
    if (channel >= 0) {
        if (!wolf_ensure_voices(channel + 1)) return -1;
        slot = channel;
    } else {
        for (int i = s_reserved_chans; i < s_voices_cap; i++) {
            if (!s_voices[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            int want = (s_voices_cap > s_reserved_chans) ? s_voices_cap : s_reserved_chans;
            if (!wolf_ensure_voices(want + 1)) return -1;
            slot = want;
        }
    }
    s_voices[slot].chunk = chunk;
    s_voices[slot].pos = 0;
    s_voices[slot].loops = loops;
    s_voices[slot].active = true;
    return slot;
}

int Mix_HaltChannel(int channel) {
    if (channel < 0) {
        for (int i = 0; i < s_voices_cap; i++) {
            if (s_voices[i].active) {
                s_voices[i].active = false;
                if (s_chan_finished) s_chan_finished(i);
            }
        }
    } else if (channel < s_voices_cap && s_voices[channel].active) {
        s_voices[channel].active = false;
        if (s_chan_finished) s_chan_finished(channel);
    }
    return 0;
}

int Mix_SetPanning(int channel, Uint8 left, Uint8 right) {
    if (channel >= 0) {
        if (!wolf_ensure_voices(channel + 1)) return 0;
        s_voices[channel].pan_l = left;
        s_voices[channel].pan_r = right;
    }
    return 1;
}

void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg) {
    s_music_hook = mix_func;
    s_music_udata = arg;
}

void Mix_ChannelFinished(void (*channel_finished)(int channel)) {
    s_chan_finished = channel_finished;
}
