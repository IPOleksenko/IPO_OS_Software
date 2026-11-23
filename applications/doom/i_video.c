/**
 * i_video.c - Platform video and input driver for DOOM on IPO_OS
 *
 * Renders into a low-level WM window framebuffer with userland palette
 * quantization and userland scaling, reading input from the WM event queue.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_event.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

#include <wm.h>
#include <syscall.h>
#include <kernel/input_ext.h>
#include <ipo/gfx.h>

extern boolean system_is_interrupted(void);
extern void system_clear_interrupt(void);

static wm_window_t *s_win = NULL;
static boolean s_graphics_inited = false;
static boolean s_e0_prefix = false;
static uint8_t s_cached_pal[256][3];
static uint8_t s_pal_lut[256];
static uint32_t s_pal32[256];
static uint32_t s_doom_xrgb32[320 * 200];
static boolean s_use_truecolor32 = false;
static uint8_t s_wm_palette[248][3];
static boolean s_wm_pal_ready = false;
static boolean s_pal_valid = false;
static int *s_x_lut = NULL;
static int s_x_lut_w = 0;

static void init_wm_palette_table(void) {
    int i, idx, r, g, b;
    if (s_wm_pal_ready) return;

    static const uint8_t cga[16][3] = {
        {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
        {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
        {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
        {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}
    };
    for (i = 0; i < 16; i++) {
        s_wm_palette[i][0] = cga[i][0];
        s_wm_palette[i][1] = cga[i][1];
        s_wm_palette[i][2] = cga[i][2];
    }
    for (i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        s_wm_palette[16 + i][0] = v;
        s_wm_palette[16 + i][1] = v;
        s_wm_palette[16 + i][2] = v;
    }
    idx = 32;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                s_wm_palette[idx][0] = (uint8_t)(r * 51);
                s_wm_palette[idx][1] = (uint8_t)(g * 51);
                s_wm_palette[idx][2] = (uint8_t)(b * 51);
                idx++;
            }
        }
    }
    for (i = 0; i < 256; i++) {
        s_pal_lut[i] = (i < 248) ? (uint8_t)i : 0u;
    }
    s_wm_pal_ready = true;
}

static uint8_t find_nearest_wm_color(uint8_t r, uint8_t g, uint8_t b) {
    int i;
    int best_idx = 0;
    uint32_t best_dist = 0xFFFFFFFFu;
    for (i = 0; i < 248; i++) {
        int dr = (int)r - (int)s_wm_palette[i][0];
        int dg = (int)g - (int)s_wm_palette[i][1];
        int db = (int)b - (int)s_wm_palette[i][2];
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist == 0u) return (uint8_t)i;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    return (uint8_t)best_idx;
}

static void poll_wm_events(void);
static bool ensure_x_lut(int dst_w);

void I_InitGraphics(void) {
    if (s_graphics_inited) return;

    init_wm_palette_table();

    int sw = 0, sh = 0;
    wm_get_screen_bounds(&sw, &sh);
    int max_w = (sw > 16) ? (sw - 8) : SCREENWIDTH;
    int max_h = (sh > 36) ? (sh - 32) : SCREENHEIGHT;
    int win_w = (SCREENWIDTH <= max_w) ? SCREENWIDTH : max_w;
    int win_h = (SCREENHEIGHT <= max_h) ? SCREENHEIGHT : max_h;
    if (win_w < 160) win_w = 160;
    if (win_h < 100) win_h = 100;

    wm_window_options_t opts = WM_WINDOW_OPTIONS_DEFAULT;
    opts.title = "DOOM";
    opts.x = (int16_t)((sw > win_w) ? (sw - win_w) / 2 : 0);
    opts.y = (int16_t)((sh > win_h + 26) ? (sh - win_h - 14) / 2 : 14);
    opts.w = (uint16_t)win_w;
    opts.h = (uint16_t)win_h;
    opts.bg_color = 0;

    s_win = wm_create_window(&opts);
    if (!s_win) {
        I_Error("I_InitGraphics: failed to create WM window");
    }
    wm_set_fullscreen(s_win, true);
    wm_set_cursor_visible(s_win, false);

    if (!screens[0]) {
        screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    }
    if (screens[0]) {
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    }

    if (s_win->w != SCREENWIDTH) {
        ensure_x_lut((int)s_win->w);
    }

    s_pal_valid = false;
    s_e0_prefix = false;
    {
        ipo_gfx_caps_t caps;
        s_use_truecolor32 = (ipo_gfx_get_caps(&caps) == 0 && caps.active_hw_bpp >= 15u);
    }
    s_graphics_inited = true;
}

void I_ShutdownGraphics(void) {
    if (!s_graphics_inited) return;

    if (s_win && wm_is_window_valid(s_win)) {
        wm_destroy_window(s_win);
    }
    s_win = NULL;

    free(s_x_lut);
    s_x_lut = NULL;
    s_x_lut_w = 0;

    s_graphics_inited = false;
}

void I_SetPalette(byte *palette) {
    int i;
    if (!palette) return;
    init_wm_palette_table();

    for (i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        if (!s_pal_valid ||
            s_cached_pal[i][0] != r ||
            s_cached_pal[i][1] != g ||
            s_cached_pal[i][2] != b) {
            s_cached_pal[i][0] = r;
            s_cached_pal[i][1] = g;
            s_cached_pal[i][2] = b;
            s_pal32[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            if (!s_use_truecolor32) {
                s_pal_lut[i] = find_nearest_wm_color(r, g, b);
            }
        }
    }
    s_pal_valid = true;
}

void I_UpdateNoBlit(void) {
}

static bool ensure_x_lut(int dst_w) {
    int x;
    int *nlut;
    if (s_x_lut && s_x_lut_w == dst_w) return true;
    nlut = (int *)realloc(s_x_lut, (size_t)dst_w * sizeof(int));
    if (!nlut) return false;
    for (x = 0; x < dst_w; x++) {
        nlut[x] = (x * SCREENWIDTH) / dst_w;
    }
    s_x_lut = nlut;
    s_x_lut_w = dst_w;
    return true;
}

void I_FinishUpdate(void) {
    int dw, dh;
    const uint8_t *src_fb;
    uint8_t *dst_fb;

    if (!s_graphics_inited) {
        I_InitGraphics();
    }

    poll_wm_events();
    if (!s_win || !wm_is_window_valid(s_win) || !s_win->framebuf || !screens[0]) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        return;
    }

    dw = (int)s_win->w;
    dh = (int)s_win->h;
    if (dw <= 0 || dh <= 0) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        return;
    }

    src_fb = screens[0];

    if (s_use_truecolor32) {
        size_t i;
        size_t total = (size_t)SCREENWIDTH * (size_t)SCREENHEIGHT;
        ipo_gfx_surface_desc_t surf;
        for (i = 0; i < total; i += 4u) {
            s_doom_xrgb32[i + 0u] = s_pal32[src_fb[i + 0u]];
            s_doom_xrgb32[i + 1u] = s_pal32[src_fb[i + 1u]];
            s_doom_xrgb32[i + 2u] = s_pal32[src_fb[i + 2u]];
            s_doom_xrgb32[i + 3u] = s_pal32[src_fb[i + 3u]];
        }
        memset(&surf, 0, sizeof(surf));
        surf.struct_size = (uint32_t)sizeof(surf);
        surf.width = (uint32_t)SCREENWIDTH;
        surf.height = (uint32_t)SCREENHEIGHT;
        surf.stride_bytes = (uint32_t)(SCREENWIDTH * 4);
        surf.format_id = IPO_COLOR_FMT_XRGB8888;
        surf.tonemap_op = IPO_TONEMAP_NONE;
        surf.exposure = 1.0;
        surf.pixels = s_doom_xrgb32;
        ipo_gfx_present_window(s_win, &surf);
    } else {
        dst_fb = s_win->framebuf;

        if (dw == SCREENWIDTH && dh == SCREENHEIGHT) {
            size_t i;
            size_t total = (size_t)SCREENWIDTH * (size_t)SCREENHEIGHT;
            for (i = 0; i < total; i += 4) {
                dst_fb[i + 0] = s_pal_lut[src_fb[i + 0]];
                dst_fb[i + 1] = s_pal_lut[src_fb[i + 1]];
                dst_fb[i + 2] = s_pal_lut[src_fb[i + 2]];
                dst_fb[i + 3] = s_pal_lut[src_fb[i + 3]];
            }
        } else if (ensure_x_lut(dw)) {
            int y, x;
            int prev_sy = -1;
            uint8_t *prev_dst = NULL;
            for (y = 0; y < dh; y++) {
                int sy = (y * SCREENHEIGHT) / dh;
                uint8_t *dst_row = dst_fb + (size_t)y * (size_t)dw;
                const uint8_t *src_row;
                if (sy == prev_sy && prev_dst) {
                    memcpy(dst_row, prev_dst, (size_t)dw);
                    continue;
                }
                src_row = src_fb + (size_t)sy * SCREENWIDTH;
                for (x = 0; x < dw; x++) {
                    dst_row[x] = s_pal_lut[src_row[s_x_lut[x]]];
                }
                prev_sy = sy;
                prev_dst = dst_row;
            }
        }

        wm_invalidate(s_win);
    }

    {
        static uint32_t s_fps_last_ms = 0;
        static uint32_t s_fps_frames = 0;
        static uint32_t s_last_yield_ms = 0;
        extern int serial_printf(const char *fmt, ...);
        uint32_t now = (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
        if (s_fps_last_ms == 0u) {
            s_fps_last_ms = now;
            s_last_yield_ms = now;
        }
        s_fps_frames++;
        uint32_t elapsed = now - s_fps_last_ms;
        if (elapsed >= 1000u) {
            uint32_t fps = (s_fps_frames * 1000u) / elapsed;
            serial_printf("[doom] %u FPS\n", fps);
            s_fps_frames = 0;
            s_fps_last_ms = now;
        }
        if (now - s_last_yield_ms >= 8u) {
            s_last_yield_ms = now;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        }
    }
}

void I_WaitVBL(int count) {
    while (count-- > 0) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

void I_ReadScreen(byte *scr) {
    if (screens[0] && scr) {
        memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
    }
}

void I_BeginRead(void) {
}

void I_EndRead(void) {
}

void I_StartFrame(void) {
}

static int translate_scancode(uint8_t sc, boolean e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1D: return KEY_RCTRL;
            case 0x38: return KEY_RALT;
            default:   return 0;
        }
    }

    switch (sc) {
        case 0x39: return ' ';          /* Spacebar -> key_use */
        case 0x1C: return KEY_ENTER;
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1D: return KEY_RCTRL;     /* Left Ctrl -> key_fire */
        case 0x2A: return KEY_RSHIFT;    /* Left Shift */
        case 0x36: return KEY_RSHIFT;    /* Right Shift */
        case 0x38: return KEY_RALT;      /* Left Alt */

        /* Function keys */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;

        /* Number row */
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;

        /* Letters row 1 */
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1A: return '[';
        case 0x1B: return ']';

        /* Letters row 2 */
        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return ';';
        case 0x28: return '\'';

        /* Letters row 3 */
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';

        default: return 0;
    }
}

static uint32_t s_ps2_kbd_hook = 0u;
static boolean  s_ps2_atexit_registered = false;
static boolean  s_ps2_e0 = false;
static uint8_t  s_ps2_e1_skip = 0u;
static boolean  s_ps2_lctrl = false;
static boolean  s_ps2_rctrl = false;
static boolean  s_ps2_lshift = false;
static boolean  s_ps2_rshift = false;
static boolean  s_ps2_lalt = false;
static boolean  s_ps2_ralt = false;
static uint8_t  s_doom_keys_down[256];

static void doom_post_key(int doom_key, boolean is_release) {
    event_t ev;
    if (doom_key <= 0 || doom_key >= 256) return;

    /* Keep modifier key pressed in Doom until both Left and Right keys are released */
    if (is_release) {
        if (doom_key == KEY_RCTRL && (s_ps2_lctrl || s_ps2_rctrl)) return;
        if (doom_key == KEY_RALT  && (s_ps2_lalt  || s_ps2_ralt))  return;
        if (doom_key == KEY_RSHIFT && (s_ps2_lshift || s_ps2_rshift)) return;
    }

    s_doom_keys_down[doom_key] = is_release ? 0u : 1u;
    { extern int serial_printf(const char *fmt, ...);
      serial_printf("[doomdbg] key=%d %s hook=%u\n", doom_key, is_release ? "up" : "down", s_ps2_kbd_hook); }

    ev.type = is_release ? ev_keyup : ev_keydown;
    ev.data1 = doom_key;
    ev.data2 = 0;
    ev.data3 = 0;
    D_PostEvent(&ev);
}

static void doom_release_all_keys(void) {
    int k;
    s_ps2_e0 = false;
    s_ps2_e1_skip = 0u;
    s_e0_prefix = false;
    s_ps2_lctrl = s_ps2_rctrl = false;
    s_ps2_lshift = s_ps2_rshift = false;
    s_ps2_lalt = s_ps2_ralt = false;

    for (k = 1; k < 256; k++) {
        if (s_doom_keys_down[k]) {
            event_t ev;
            s_doom_keys_down[k] = 0u;
            ev.type = ev_keyup;
            ev.data1 = k;
            ev.data2 = 0;
            ev.data3 = 0;
            D_PostEvent(&ev);
        }
    }
}

static void doom_ps2_hook_unregister(void) {
    if (s_ps2_kbd_hook != 0u) {
        ipo_ps2_hook_unregister(s_ps2_kbd_hook);
        s_ps2_kbd_hook = 0u;
    }
    s_ps2_e0 = false;
    s_ps2_e1_skip = 0u;
}

static void doom_ps2_atexit_cleanup(void) {
    doom_ps2_hook_unregister();
}

static void doom_ps2_hook_sync(boolean focused) {
    uint32_t h;
    if (!focused) {
        if (s_ps2_kbd_hook != 0u) {
            doom_ps2_hook_unregister();
            doom_release_all_keys();
        }
        return;
    }

    if (s_ps2_kbd_hook != 0u) return;

    /* Wait until any held modifier from shell/workspace-switch clears so kernel
     * keyboard state observes its break scancode before exclusive capture begins. */
    if ((ipo_input_get_modifiers() & (IPO_MOD_CTRL | IPO_MOD_ALT | IPO_MOD_SHIFT)) != 0u) {
        return;
    }

    h = ipo_ps2_hook_register(IPO_PS2_CHAN_KBD, IPO_PS2_HOOK_EXCLUSIVE);
    { extern int serial_printf(const char *fmt, ...); serial_printf("[doomdbg] hook register -> %u\n", h); }
    if (h != 0u) {
        s_ps2_kbd_hook = h;
        s_ps2_e0 = false;
        s_ps2_e1_skip = 0u;
        s_e0_prefix = false;
        if (!s_ps2_atexit_registered) {
            atexit(doom_ps2_atexit_cleanup);
            s_ps2_atexit_registered = true;
        }
    }
}

static boolean doom_handle_wm_hotkey(uint8_t make, boolean is_e0, boolean is_release) {
    boolean down = !is_release;
    boolean ctrl_held;
    boolean shift_held;
    boolean alt_held;

    if (make == 0x1Du) {
        if (is_e0) s_ps2_rctrl = down;
        else       s_ps2_lctrl = down;
    } else if (make == 0x2Au) {
        if (!is_e0) s_ps2_lshift = down;
    } else if (make == 0x36u) {
        if (!is_e0) s_ps2_rshift = down;
    } else if (make == 0x38u) {
        if (is_e0) s_ps2_ralt = down;
        else       s_ps2_lalt = down;
    }

    if (is_release) return false;

    ctrl_held  = s_ps2_lctrl  || s_ps2_rctrl;
    shift_held = s_ps2_lshift || s_ps2_rshift;
    alt_held   = s_ps2_lalt   || s_ps2_ralt;

    if (!is_e0 && make == 0x57u && s_win) { /* F11: toggle WM fullscreen */
        boolean next_fs = !s_win->fullscreen;
        wm_set_fullscreen(s_win, next_fs);
        wm_set_cursor_visible(s_win, !next_fs);
        return true;
    }

    if (ctrl_held && !alt_held && is_e0 && (make == 0x49u || make == 0x51u)) {
        if (ipo_syscall(IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE, 0u, NULL) != 0u) {
            uint32_t arg = (make == 0x49u) ? 1u : 0u;
            doom_release_all_keys();
            doom_ps2_hook_unregister();
            ipo_syscall(IPO_SYSCALL_PROCESS_SWITCH_FG, 1u, &arg);
            return true;
        }
    }

    if (ctrl_held && !alt_held && !is_e0 && make == 0x0Fu && wm_get_window_count() > 1) {
        doom_release_all_keys();
        doom_ps2_hook_unregister();
        if (shift_held) wm_focus_prev();
        else            wm_focus_next();
        return true;
    }

    return false;
}

static void doom_process_scancode_byte(uint8_t sc, boolean *e0_flag,
                                       uint8_t *e1_skip, boolean check_hotkeys) {
    boolean is_e0;
    boolean is_release;
    uint8_t make_code;
    int doom_key;

    if (*e1_skip > 0u) {
        boolean is_pause_make = (*e1_skip == 1u && sc == 0x45u);
        (*e1_skip)--;
        *e0_flag = false;
        if (is_pause_make) {
            doom_post_key(KEY_PAUSE, false);
            doom_post_key(KEY_PAUSE, true);
        }
        return;
    }

    if (sc == 0xE1u) {
        *e1_skip = 2u;
        *e0_flag = false;
        return;
    }

    if (sc == 0xE0u) {
        *e0_flag = true;
        return;
    }

    if (sc == 0x00u || sc == 0xFAu || sc == 0xFEu || sc == 0xFFu) {
        *e0_flag = false;
        return;
    }

    is_e0 = *e0_flag;
    *e0_flag = false;

    is_release = (sc & 0x80u) != 0;
    make_code = sc & 0x7Fu;

    if (check_hotkeys && doom_handle_wm_hotkey(make_code, is_e0, is_release)) {
        return;
    }

    doom_key = translate_scancode(make_code, is_e0);
    if (doom_key != 0) {
        doom_post_key(doom_key, is_release);
    }
}

static void poll_ps2_kbd_events(void) {
    static ipo_ps2_byte_event_t *s_ps2_evs = NULL;
    static uint32_t s_ps2_evs_cap = 0u;
    if (s_ps2_kbd_hook == 0u) return;

    if (!s_ps2_evs) {
        s_ps2_evs = (ipo_ps2_byte_event_t *)malloc(64u * sizeof(ipo_ps2_byte_event_t));
        if (!s_ps2_evs) return;
        s_ps2_evs_cap = 64u;
    }

    for (;;) {
        uint32_t i;
        uint32_t count = ipo_ps2_hook_poll(s_ps2_kbd_hook, s_ps2_evs, s_ps2_evs_cap);
        if (count == 0u) break;
        for (i = 0u; i < count; i++) {
            if ((s_ps2_evs[i].chan_mask & IPO_PS2_CHAN_KBD) == 0u) continue;
            doom_process_scancode_byte(s_ps2_evs[i].byte, &s_ps2_e0, &s_ps2_e1_skip, true);
            if (s_ps2_kbd_hook == 0u) {
                return;
            }
        }
        if (count == s_ps2_evs_cap && s_ps2_evs_cap < 4096u) {
            uint32_t new_cap = s_ps2_evs_cap * 2u;
            ipo_ps2_byte_event_t *nb = (ipo_ps2_byte_event_t *)realloc(
                s_ps2_evs, (size_t)new_cap * sizeof(ipo_ps2_byte_event_t));
            if (nb) {
                s_ps2_evs = nb;
                s_ps2_evs_cap = new_cap;
            }
        }
    }
}

static void poll_wm_events(void) {
    boolean focused;
    wm_event_t wev;

    /* Handle Ctrl+C interrupt cleanly */
    if (system_is_interrupted()) {
        doom_ps2_hook_unregister();
        doom_release_all_keys();
        system_clear_interrupt();
        I_Quit();
        return;
    }

    if (!s_win) return;

    if (!wm_is_window_valid(s_win) || !wm_session_active()) {
        doom_ps2_hook_unregister();
        doom_release_all_keys();
        I_Quit();
        return;
    }

    focused = (wm_get_focused() == s_win);
    doom_ps2_hook_sync(focused);

    while (wm_poll_event(s_win, &wev)) {
        uint8_t sc;
        uint8_t unused_e1;
        if (wev.type == WM_EVENT_CLOSE) {
            doom_ps2_hook_unregister();
            doom_release_all_keys();
            I_Quit();
            return;
        }
        if (wev.type == WM_EVENT_FOCUS_OUT) {
            doom_ps2_hook_sync(false);
            continue;
        }
        if (wev.type == WM_EVENT_FOCUS_IN) {
            focused = (wm_get_focused() == s_win);
            doom_ps2_hook_sync(focused);
            continue;
        }
        if (s_ps2_kbd_hook != 0u) {
            continue;
        }
        if (wev.type != WM_EVENT_KEY_DOWN && wev.type != WM_EVENT_KEY_UP) {
            continue;
        }

        sc = (uint8_t)(wev.data & 0xFFu);
        unused_e1 = 0u;
        doom_process_scancode_byte(sc, &s_e0_prefix, &unused_e1, false);
    }

    poll_ps2_kbd_events();
}

void I_StartTic(void) {
    poll_wm_events();
}
